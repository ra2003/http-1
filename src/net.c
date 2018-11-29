/*
    net.c -- Network I/O.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void manageNet(HttpNet *net, int flags);
static void netTimeout(HttpNet *net, MprEvent *mprEvent);
static void secureNet(HttpNet *net, MprSsl *ssl, cchar *peerName);

#if ME_HTTP_HTTP2
static HttpHeaderTable *createHeaderTable();
static void manageHeaderTable(HttpHeaderTable *table, int flags);
#endif

/*********************************** Code *************************************/

PUBLIC HttpNet *httpCreateNet(MprDispatcher *dispatcher, HttpEndpoint *endpoint, int protocol, int flags)
{
    Http        *http;
    HttpNet     *net;
    HttpHost    *host;
    HttpRoute   *route;

    http = HTTP;

    if ((net = mprAllocObj(HttpNet, manageNet)) == 0) {
        return 0;
    }
    net->http = http;
    net->endpoint = endpoint;
    net->lastActivity = http->now;
    net->ioCallback = httpIOEvent;

    if (endpoint) {
        net->async = endpoint->async;
        net->notifier = endpoint->notifier;
        net->endpoint = endpoint;
        host = mprGetFirstItem(endpoint->hosts);
        if (host && (route = host->defaultRoute) != 0) {
            net->trace = route->trace;
            net->limits = route->limits;
        } else {
            net->limits = http->serverLimits;
            net->trace = http->trace;
        }
    } else {
        net->limits = http->clientLimits;
        net->trace = http->trace;
        net->nextStream = 1;
    }
    net->port = -1;
    net->async = (flags & HTTP_NET_ASYNC) ? 1 : 0;


    net->socketq = httpCreateQueue(net, NULL, http->netConnector, HTTP_QUEUE_TX, NULL);
    net->socketq->name = sclone("socket-tx");

#if ME_HTTP_HTTP2
    /*
        The socket queue will typically send and accept packets of HTTP2_DEFAULT_FRAME_SIZE plus the frame size overhead.
        Set the max to fit four packets. Note that HTTP/2 flow control happens on the http filters and not on the socketq.
        Other queues created in netConnector after the protocol is known.
     */
    httpSetQueueLimits(net->socketq, HTTP2_DEFAULT_FRAME_SIZE + HTTP2_FRAME_OVERHEAD, -1, HTTP2_DEFAULT_FRAME_SIZE * 4);
    net->rxHeaders = createHeaderTable(HTTP2_TABLE_SIZE);
    net->txHeaders = createHeaderTable(HTTP2_TABLE_SIZE);
#endif

    /*
        Create queue of queues that require servicing
     */
    net->serviceq = httpCreateQueueHead(net, NULL, "serviceq", 0);
    httpInitSchedulerQueue(net->serviceq);

    if (dispatcher) {
        net->dispatcher = dispatcher;
    } else if (endpoint) {
        net->dispatcher = endpoint->dispatcher;
    } else {
        net->dispatcher = mprGetDispatcher();
    }
    net->connections = mprCreateList(0, 0);

    if (protocol >= 0) {
        httpSetNetProtocol(net, protocol);
    }

    lock(http);
    net->seqno = (int) ++http->totalConnections;
    unlock(http);
    httpAddNet(net);
    return net;
}


/*
    Destroy a network. This removes the network from the list of networks.
 */
PUBLIC void httpDestroyNet(HttpNet *net)
{
    HttpConn    *conn;
    int         next;

    if (!net->destroyed && !net->borrowed) {
        if (httpIsServer(net)) {
            for (ITERATE_ITEMS(net->connections, conn, next)) {
                mprRemoveItem(net->connections, conn);
                httpDestroyConn(conn);
                next--;
            }
            httpMonitorNetEvent(net, HTTP_COUNTER_ACTIVE_CONNECTIONS, -1);
        }
        httpRemoveNet(net);
        if (net->sock) {
            mprCloseSocket(net->sock, 0);
            /* Don't zero just incase another thread (in error) uses net->sock */
        }
        if (net->dispatcher && net->dispatcher->flags & MPR_DISPATCHER_AUTO) {
            mprDestroyDispatcher(net->dispatcher);
            /* Don't zero just incase another thread (in error) uses net->dispatcher */
        }
        net->destroyed = 1;
    }
}


static void manageNet(HttpNet *net, int flags)
{
    assert(net);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(net->address);
        mprMark(net->connections);
        mprMark(net->context);
        mprMark(net->data);
        mprMark(net->dispatcher);
        mprMark(net->ejs);
        mprMark(net->endpoint);
        mprMark(net->errorMsg);
        mprMark(net->holdq);
        mprMark(net->http);
        mprMark(net->inputq);
        mprMark(net->ip);
        mprMark(net->limits);
        mprMark(net->newDispatcher);
        mprMark(net->oldDispatcher);
        mprMark(net->outputq);
        mprMark(net->pool);
        mprMark(net->serviceq);
        mprMark(net->sock);
        mprMark(net->socketq);
        mprMark(net->trace);
        mprMark(net->timeoutEvent);
        mprMark(net->workerEvent);

#if ME_HTTP_HTTP2
        mprMark(net->frame);
        mprMark(net->rxHeaders);
        mprMark(net->txHeaders);
#endif
    }
}


PUBLIC void httpBindSocket(HttpNet *net, MprSocket *sock)
{
    assert(net);
    if (sock) {
        sock->data = net;
        net->sock = sock;
        net->port = sock->port;
        net->ip = sclone(sock->ip);
    }
}


/*
    Client connect the network to a new peer.
    Existing connections are closed
 */
PUBLIC int httpConnectNet(HttpNet *net, cchar *ip, int port, MprSsl *ssl)
{
    MprSocket   *sp;

    assert(net);

    if (net->sock) {
        mprCloseSocket(net->sock, 0);
        net->sock = 0;
    }
    if ((sp = mprCreateSocket()) == 0) {
        httpNetError(net, "Cannot create socket");
        return MPR_ERR_CANT_ALLOCATE;
    }
    net->error = 0;
    if (mprConnectSocket(sp, ip, port, MPR_SOCKET_NODELAY) < 0) {
        httpNetError(net, "Cannot open socket on %s:%d", ip, port);
        return MPR_ERR_CANT_CONNECT;
    }
    net->sock = sp;
    net->ip = sclone(ip);
    net->port = port;

    if (ssl) {
        secureNet(net, ssl, ip);
    }
    httpTrace(net->trace, "net.peer", "context", "peer:'%s:%d'", net->ip, net->port);
    return 0;
}


static void secureNet(HttpNet *net, MprSsl *ssl, cchar *peerName)
{
#if ME_COM_SSL
    MprSocket   *sock;

    sock = net->sock;
    if (mprUpgradeSocket(sock, ssl, peerName) < 0) {
        httpNetError(net, "Cannot perform SSL upgrade. %s", sock->errorMsg);

    } else if (sock->peerCert) {
        httpTrace(net->trace, "net.ssl", "context",
            "msg:'Connection secured with peer certificate', " \
            "secure:true,cipher:'%s',peerName:'%s',subject:'%s',issuer:'%s'",
            sock->cipher, sock->peerName, sock->peerCert, sock->peerCertIssuer);
    }
#endif
}


PUBLIC void httpSetNetProtocol(HttpNet *net, int protocol)
{
    Http        *http;
    HttpStage   *stage;

    http = net->http;
    protocol = net->protocol = protocol > 0 ? protocol : HTTP_1_1;

    /*
        Create queues connected to the appropriate protocol filter. Supply conn for HTTP/1.
        For HTTP/2:
            The Q packetSize defines frame size and the Q max defines the window size.
            The outputq->max must be set to HTTP2_DEFAULT_WINDOW by spec. The packetSize must be set to 16K minimum.
     */
#if ME_HTTP_HTTP2
    stage = protocol == 1 ? http->http1Filter : http->http2Filter;
#else
    stage = http->http1Filter;
#endif
    net->inputq = httpCreateQueue(net, NULL, stage, HTTP_QUEUE_RX, NULL);
    net->outputq = httpCreateQueue(net, NULL, stage, HTTP_QUEUE_TX, NULL);
    httpPairQueues(net->inputq, net->outputq);
    httpAppendQueue(net->socketq, net->outputq);

#if ME_HTTP_HTTP2
    httpSetQueueLimits(net->inputq, net->limits->frameSize, -1, net->limits->windowSize);
    httpSetQueueLimits(net->outputq, HTTP2_DEFAULT_FRAME_SIZE, -1, HTTP2_DEFAULT_WINDOW);
#endif
}


PUBLIC void httpNetClosed(HttpNet *net)
{
    HttpConn    *conn;
    int         next;

    for (ITERATE_ITEMS(net->connections, conn, next)) {
        if (conn->state < HTTP_STATE_PARSED) {
            if (!conn->errorMsg) {
                conn->errorMsg = sfmt("Peer closed connection before receiving a response");
            }
            if (!net->errorMsg) {
                net->errorMsg = conn->errorMsg;
            }
            conn->error = 1;
        }
        httpSetEof(conn);
        httpSetState(conn, HTTP_STATE_COMPLETE);
        mprCreateEvent(net->dispatcher, "disconnect", 0, httpProcess, conn->inputq, 0);
    }
}


#if ME_HTTP_HTTP2
static HttpHeaderTable *createHeaderTable(int maxsize)
{
    HttpHeaderTable     *table;

    table = mprAllocObj(HttpHeaderTable, manageHeaderTable);
    table->list = mprCreateList(256, 0);
    table->size = 0;
    table->max = maxsize;
    return table;
}


static void manageHeaderTable(HttpHeaderTable *table, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(table->list);
    }
}
#endif


PUBLIC void httpAddConn(HttpNet *net, HttpConn *conn)
{
    mprAddItem(net->connections, conn);
    conn->net = net;
}


PUBLIC void httpRemoveConn(HttpNet *net, HttpConn *conn)
{
    mprRemoveItem(net->connections, conn);
}


PUBLIC void httpNetTimeout(HttpNet *net)
{
    if (!net->timeoutEvent && !net->destroyed) {
        /*
            Will run on the HttpNet dispatcher unless shutting down and it is destroyed already
         */
        net->timeoutEvent = mprCreateEvent(net->dispatcher, "netTimeout", 0, netTimeout, net, 0);
    }
}


PUBLIC bool httpGetAsync(HttpNet *net)
{
    return net->async;
}


PUBLIC void httpSetAsync(HttpNet *net, bool async)
{
    net->async = async;
}


//  TODO - naming. Some have Net, some not.

PUBLIC void httpSetIOCallback(HttpNet *net, HttpIOCallback fn)
{
    net->ioCallback = fn;
}


PUBLIC void httpSetNetContext(HttpNet *net, void *context)
{
    net->context = context;
}


static void netTimeout(HttpNet *net, MprEvent *mprEvent)
{
    if (net->destroyed) {
        return;
    }
    /* This will trigger an I/O event which will then destroy the network */
    mprDisconnectSocket(net->sock);
}



//  TODO - review all these
/*
    Used by ejs
 */
PUBLIC void httpUseWorker(HttpNet *net, MprDispatcher *dispatcher, MprEvent *event)
{
    lock(net->http);
    net->oldDispatcher = net->dispatcher;
    net->dispatcher = dispatcher;
    net->worker = 1;
    assert(!net->workerEvent);
    net->workerEvent = event;
    unlock(net->http);
}


//  TODO comment?
PUBLIC void httpUsePrimary(HttpNet *net)
{
    lock(net->http);
    assert(net->worker);
    assert(net->oldDispatcher && net->dispatcher != net->oldDispatcher);
    net->dispatcher = net->oldDispatcher;
    net->oldDispatcher = 0;
    net->worker = 0;
    unlock(net->http);
}


PUBLIC void httpBorrowNet(HttpNet *net)
{
    assert(!net->borrowed);
    if (!net->borrowed) {
        mprAddRoot(net);
        net->borrowed = 1;
    }
}


PUBLIC void httpReturnNet(HttpNet *net)
{
    assert(net->borrowed);
    if (net->borrowed) {
        net->borrowed = 0;
        mprRemoveRoot(net);
        httpEnableNetEvents(net);
    }
}


/*
    Steal the socket object from a network. This disconnects the socket from management by the Http service.
    It is the callers responsibility to call mprCloseSocket when required.
    Harder than it looks. We clone the socket, steal the socket handle and set the socket handle to invalid.
    This preserves the HttpNet.sock object for the network and returns a new MprSocket for the caller.
 */
PUBLIC MprSocket *httpStealSocket(HttpNet *net)
{
    MprSocket   *sock;

    assert(net->sock);
    assert(!net->destroyed);

    if (!net->destroyed && !net->borrowed) {
        lock(net->http);
        sock = mprCloneSocket(net->sock);
        (void) mprStealSocketHandle(net->sock);
        mprRemoveSocketHandler(net->sock);
        httpRemoveNet(net);

        /* This will cause httpIOEvent to regard this as a client connection and not destroy this connection */
        net->endpoint = 0;
        net->async = 0;
        unlock(net->http);
        return sock;
    }
    return 0;
}


/*
    Steal the O/S socket handle. This disconnects the socket handle from management by the network.
    It is the callers responsibility to call close() on the Socket when required.
    Note: this does not change the state of the network.
 */
PUBLIC Socket httpStealSocketHandle(HttpNet *net)
{
    return mprStealSocketHandle(net->sock);
}


PUBLIC cchar *httpGetProtocol(HttpNet *net)
{
    if (net->protocol == 0) {
        return "HTTP/1.0";
    } else if (net->protocol >= 2) {
        return "HTTP/2";
    } else {
        return "HTTP/1.1";
    }
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
