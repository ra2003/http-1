/*
    conn.c -- Connection module to handle individual HTTP connections.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void manageConn(HttpConn *conn, int flags);
static HttpPacket *getPacket(HttpConn *conn, ssize *bytesToRead);
static bool prepForNext(HttpConn *conn);
static void readEvent(HttpConn *conn);
static void writeEvent(HttpConn *conn);

/*********************************** Code *************************************/
/*
    Create a new connection object
 */
PUBLIC HttpConn *httpCreateConn(Http *http, HttpEndpoint *endpoint, MprDispatcher *dispatcher)
{
    HttpConn    *conn;
    HttpHost    *host;
    HttpRoute   *route;

    if ((conn = mprAllocObj(HttpConn, manageConn)) == 0) {
        return 0;
    }
    conn->http = http;
    conn->protocol = http->protocol;
    conn->port = -1;
    conn->retries = HTTP_RETRIES;
    conn->endpoint = endpoint;
    conn->lastActivity = http->now;
    conn->ioCallback = httpEvent;

    if (endpoint) {
        conn->notifier = endpoint->notifier;
        host = mprGetFirstItem(endpoint->hosts);
        if (host && (route = host->defaultRoute) != 0) {
            conn->limits = route->limits;
            conn->trace[0] = route->trace[0];
            conn->trace[1] = route->trace[1];
        } else {
            conn->limits = http->serverLimits;
            httpInitTrace(conn->trace);
        }
    } else {
        conn->limits = http->clientLimits;
        httpInitTrace(conn->trace);
    }
    conn->keepAliveCount = conn->limits->keepAliveMax;
    conn->serviceq = httpCreateQueueHead(conn, "serviceq");

    if (dispatcher) {
        conn->dispatcher = dispatcher;
    } else if (endpoint) {
        conn->dispatcher = endpoint->dispatcher;
    } else {
        conn->dispatcher = mprGetDispatcher();
    }
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);
    httpSetState(conn, HTTP_STATE_BEGIN);
    httpAddConn(http, conn);
    return conn;
}


/*
    Destroy a connection. This removes the connection from the list of connections. Should GC after that.
 */
PUBLIC void httpDestroyConn(HttpConn *conn)
{
    if (conn->http) {
        assert(conn->http);
        HTTP_NOTIFY(conn, HTTP_EVENT_DESTROY, 0);
        httpRemoveConn(conn->http, conn);
        if (conn->endpoint) {
            httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, -1);
            if (conn->activeRequest) {
                httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
                conn->activeRequest = 0;
            }
        }
        conn->input = 0;
        if (conn->tx) {
            httpDestroyPipeline(conn);
            conn->tx->conn = 0;
            conn->tx = 0;
        }
        if (conn->rx) {
            conn->rx->conn = 0;
            conn->rx = 0;
        }
        httpCloseConn(conn);
        if (conn->endpoint) {
            mprDestroyDispatcher(conn->dispatcher);
            conn->dispatcher = 0;
        }
        conn->http = 0;
    }
}


static void manageConn(HttpConn *conn, int flags)
{
    assert(conn);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(conn->address);
        mprMark(conn->rx);
        mprMark(conn->tx);
        mprMark(conn->endpoint);
        mprMark(conn->host);
        mprMark(conn->limits);
        mprMark(conn->http);
        mprMark(conn->dispatcher);
        mprMark(conn->newDispatcher);
        mprMark(conn->oldDispatcher);
        mprMark(conn->sock);
        mprMark(conn->serviceq);
        mprMark(conn->currentq);
        mprMark(conn->input);
        mprMark(conn->readq);
        mprMark(conn->writeq);
        mprMark(conn->connectorq);
        mprMark(conn->timeoutEvent);
        mprMark(conn->workerEvent);
        mprMark(conn->context);
        mprMark(conn->ejs);
        mprMark(conn->pool);
        mprMark(conn->mark);
        mprMark(conn->data);
#if (DEPRECATE || 1) && !DOXYGEN
        mprMark(conn->grid);
        mprMark(conn->record);
#endif
        mprMark(conn->boundary);
        mprMark(conn->errorMsg);
        mprMark(conn->ip);
        mprMark(conn->protocol);
        mprMark(conn->protocols);
        httpManageTrace(&conn->trace[0], flags);
        httpManageTrace(&conn->trace[1], flags);
        mprMark(conn->headersCallbackArg);

        mprMark(conn->authType);
        mprMark(conn->authData);
        mprMark(conn->username);
        mprMark(conn->password);

    } else if (flags & MPR_MANAGE_FREE) {
        httpDestroyConn(conn);
    }
}


/*
    Close the connection but don't destroy the conn object.
 */
PUBLIC void httpCloseConn(HttpConn *conn)
{
    assert(conn);

    if (conn->sock) {
        mprLog(4, "Closing connection");
        mprCloseSocket(conn->sock, 0);
        conn->sock = 0;
    }
}


PUBLIC void httpConnTimeout(HttpConn *conn)
{
    HttpLimits  *limits;
    MprTicks    now;

    if (!conn->http) {
        return;
    }
    now = conn->http->now;
    limits = conn->limits;
    assert(limits);
    mprLog(5, "Inactive connection timed out");

    if (conn->timeoutCallback) {
        (conn->timeoutCallback)(conn);
    }
    if (!conn->connError) {
        if (HTTP_STATE_CONNECTED < conn->state && conn->state < HTTP_STATE_PARSED && 
                (conn->started + limits->requestParseTimeout) < now) {
            httpError(conn, HTTP_CODE_REQUEST_TIMEOUT, "Exceeded parse headers timeout of %Ld sec", 
                limits->requestParseTimeout  / 1000);
            if (conn->rx) {
                mprTrace(2, "  State %d, uri %s", conn->state, conn->rx->uri);
            }
        } else {
            if ((conn->lastActivity + limits->inactivityTimeout) < now) {
                if (conn->state > HTTP_STATE_BEGIN) {
                    httpError(conn, HTTP_CODE_REQUEST_TIMEOUT,
                        "Exceeded inactivity timeout of %Ld sec", limits->inactivityTimeout / 1000);
                    if (conn->rx) {
                        mprTrace(2, "  State %d, uri %s", conn->state, conn->rx->uri);
                    }
                }

            } else if ((conn->started + limits->requestTimeout) < now) {
                httpError(conn, HTTP_CODE_REQUEST_TIMEOUT, "Exceeded timeout %d sec", limits->requestTimeout / 1000);
                if (conn->rx) {
                    mprTrace(2, "  State %d, uri %s", conn->state, conn->rx->uri);
                }
            }
        }
    }
    if (conn->endpoint) {
        httpDestroyConn(conn);
    } else {
        httpDisconnect(conn);
    }
}


static void commonPrep(HttpConn *conn)
{
    if (conn->timeoutEvent) {
        mprRemoveEvent(conn->timeoutEvent);
        conn->timeoutEvent = 0;
    }
    conn->lastActivity = conn->http->now;
    conn->error = 0;
    conn->errorMsg = 0;
    conn->state = 0;
    conn->authRequested = 0;
    httpSetState(conn, HTTP_STATE_BEGIN);
    httpInitSchedulerQueue(conn->serviceq);
}


/*
    Prepare for another request
    Return true if there is another request ready for serving
 */
static bool prepForNext(HttpConn *conn)
{
    assert(conn->endpoint);
    assert(conn->state == HTTP_STATE_COMPLETE);

    if (conn->keepAliveCount < 0) {
        return 0;
    }
    if (conn->tx) {
        assert(conn->tx->finalized && conn->tx->finalizedConnector && conn->tx->finalizedOutput);
        conn->tx->conn = 0;
    }
    if (conn->rx) {
        conn->rx->conn = 0;
    }
    conn->authType = 0;
    conn->username = 0;
    conn->password = 0;
    conn->user = 0;
    conn->authData = 0;
    conn->encoded = 0;
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);
    commonPrep(conn);
    assert(conn->state == HTTP_STATE_BEGIN);
    return conn->input && (httpGetPacketLength(conn->input) > 0) && !conn->connError;
}


PUBLIC void httpConsumeLastRequest(HttpConn *conn)
{
    MprTicks    mark;
    char        junk[4096];

    if (!conn->sock) {
        return;
    }
    if (conn->state >= HTTP_STATE_FIRST) {
        mark = conn->http->now;
        while (!httpIsEof(conn) && mprGetRemainingTicks(mark, conn->limits->requestTimeout) > 0) {
            if (httpRead(conn, junk, sizeof(junk)) <= 0) {
                break;
            }
        }
    }
    if (HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_COMPLETE) {
        conn->keepAliveCount = -1;
    }
}
 

PUBLIC void httpPrepClientConn(HttpConn *conn, bool keepHeaders)
{
    MprHash     *headers;

    assert(conn);
    conn->connError = 0;
    if (conn->keepAliveCount >= 0 && conn->sock) {
        /* Eat remaining input incase last request did not consume all data */
        httpConsumeLastRequest(conn);
    } else {
        conn->input = 0;
    }
    conn->input = 0;
    if (conn->tx) {
        conn->tx->conn = 0;
    }
    if (conn->rx) {
        conn->rx->conn = 0;
    }
    headers = (keepHeaders && conn->tx) ? conn->tx->headers: NULL;
    conn->tx = httpCreateTx(conn, headers);
    conn->rx = httpCreateRx(conn);
    commonPrep(conn);
}


/*
    Accept a new client connection on a new socket. 
    This will come in on a worker thread with a new dispatcher dedicated to this connection. 
 */
PUBLIC HttpConn *httpAcceptConn(HttpEndpoint *endpoint, MprEvent *event)
{
    Http        *http;
    HttpConn    *conn;
    HttpAddress *address;
    MprSocket   *sock;
    int         level;

    assert(event);
    assert(event->dispatcher);
    assert(endpoint);

    sock = event->sock;
    http = endpoint->http;

    if (mprShouldDenyNewRequests()) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    if ((conn = httpCreateConn(http, endpoint, event->dispatcher)) == 0) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    conn->notifier = endpoint->notifier;
    conn->async = endpoint->async;
    conn->endpoint = endpoint;
    conn->sock = sock;
    conn->port = sock->port;
    conn->ip = sclone(sock->ip);

    if (httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, 1) > conn->limits->connectionsMax) {
        mprError("Too many concurrent connections");
        httpDestroyConn(conn);
        return 0;
    }
    if (mprGetHashLength(http->addresses) > conn->limits->clientMax) {
        mprError("Too many concurrent clients");
        httpDestroyConn(conn);
        return 0;
    }
    address = conn->address;
    if (address && address->banUntil > http->now) {
        if (address->banStatus) {
            httpError(conn, HTTP_CLOSE | address->banStatus, address->banMsg ? address->banMsg : "Client banned");
        } else if (address->banMsg) {
            httpError(conn, HTTP_CLOSE | HTTP_CODE_NOT_ACCEPTABLE, address->banMsg);
        } else {
            httpDisconnect(conn);
            return 0;
        }
    }
    if (endpoint->ssl) {
        if (mprUpgradeSocket(sock, endpoint->ssl, 0) < 0) {
            mprError("Cannot upgrade socket for SSL: %s", sock->errorMsg);
            mprCloseSocket(sock, 0);
            httpMonitorEvent(conn, HTTP_COUNTER_SSL_ERRORS, 1); 
            return 0;
        }
        conn->secure = 1;
    }
    assert(conn->state == HTTP_STATE_BEGIN);
    httpSetState(conn, HTTP_STATE_CONNECTED);

    if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_CONN, NULL)) >= 0) {
        mprLog(level, "### Incoming connection from %s:%d to %s:%d %s", 
            conn->ip, conn->port, sock->acceptIp, sock->acceptPort, conn->secure ? "(secure)" : "");
        if (endpoint->ssl) {
            mprLog(level, "Upgrade to TLS");
        }
    }
    event->mask = MPR_READABLE;
    event->timestamp = conn->http->now;
    (conn->ioCallback)(conn, event);
    return conn;
}


/*
    IO event handler. This is invoked by the wait subsystem in response to I/O events. It is also invoked via 
    relay when an accept event is received by the server. Initially the conn->dispatcher will be set to the
    server->dispatcher and the first I/O event will be handled on the server thread (or main thread). A request handler
    may create a new conn->dispatcher and transfer execution to a worker thread if required.
 */
PUBLIC void httpEvent(HttpConn *conn, MprEvent *event)
{
    assert(conn->sock);
    mprTrace(6, "httpEvent for fd %d, mask %d", conn->sock->fd, event->mask);
    conn->lastActivity = conn->http->now;

    if (event->mask & MPR_WRITABLE) {
        writeEvent(conn);
    }
    if (event->mask & MPR_READABLE) {
        readEvent(conn);
    }
    httpAfterEvent(conn);
}


static void readEvent(HttpConn *conn)
{
    HttpPacket  *packet;
    ssize       size, nbytes;

    if ((packet = getPacket(conn, &size)) == 0) {
        return;
    }
    assert(conn->input == packet);
    conn->newData = 0;

    nbytes = mprReadSocket(conn->sock, mprGetBufEnd(packet->content), size);
    mprTrace(7, "http: readEvent read socket %d bytes", nbytes);

    if (nbytes > 0) {
        mprAdjustBufEnd(packet->content, nbytes);
        conn->newData = nbytes;

    } else if (nbytes == 0) {
        return;

    } else if (nbytes < 0 && mprIsSocketEof(conn->sock)) {
        conn->errorMsg = conn->sock->errorMsg;
        conn->keepAliveCount = -1;
        if (conn->state < HTTP_STATE_PARSED || conn->state == HTTP_STATE_COMPLETE) {
            return;
        }
    }
    do {
        if (!httpPumpRequest(conn, conn->input)) {
            break;
        }
    } while (conn->endpoint && prepForNext(conn));
}


static void writeEvent(HttpConn *conn)
{
    mprTrace(6, "httpProcessWriteEvent, state %d", conn->state);

    if (conn->tx) {
        conn->tx->writeBlocked = 0;
        httpResumeQueue(conn->connectorq);
        httpServiceQueues(conn);
        httpPumpRequest(conn, NULL);
    }
}


PUBLIC void httpUseWorker(HttpConn *conn, MprDispatcher *dispatcher, MprEvent *event)
{
    lock(conn->http);
    conn->oldDispatcher = conn->dispatcher;
    conn->dispatcher = dispatcher;
    conn->worker = 1;
    assert(!conn->workerEvent);
    conn->workerEvent = event;
    unlock(conn->http);
}


PUBLIC void httpUsePrimary(HttpConn *conn)
{
    lock(conn->http);
    assert(conn->worker);
    assert(conn->state == HTTP_STATE_BEGIN);
    assert(conn->oldDispatcher && conn->dispatcher != conn->oldDispatcher);
    conn->dispatcher = conn->oldDispatcher;
    conn->oldDispatcher = 0;
    conn->worker = 0;
    unlock(conn->http);
}


/*
    Steal a connection with open socket from Http and disconnect it from management by Http.
    It is the callers responsibility to call mprCloseSocket when required.
 */
PUBLIC MprSocket *httpStealConn(HttpConn *conn)
{
    MprSocket   *sock;

    sock = conn->sock;
    conn->sock = 0;

    mprRemoveSocketHandler(conn->sock);
    if (conn->http) {
        lock(conn->http);
        httpRemoveConn(conn->http, conn);
        httpDiscardData(conn, HTTP_QUEUE_TX);
        httpDiscardData(conn, HTTP_QUEUE_RX);
        httpSetState(conn, HTTP_STATE_COMPLETE);
        unlock(conn->http);
    }
    return sock;
}


PUBLIC void httpAfterEvent(HttpConn *conn)
{
    if (conn->endpoint) {
        if (conn->keepAliveCount < 0 && (conn->state < HTTP_STATE_PARSED || conn->state == HTTP_STATE_COMPLETE)) {
            httpDestroyConn(conn);
            return;
        } else if (conn->state == HTTP_STATE_COMPLETE) {
            prepForNext(conn);
        }
    } else if (mprIsSocketEof(conn->sock)) {
        return;
    }
    if (!conn->state != HTTP_STATE_RUNNING) {
        httpEnableConnEvents(conn);
    }
}


PUBLIC void httpEnableConnEvents(HttpConn *conn)
{
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    MprEvent    *event;
    MprSocket   *sp;
    int         eventMask;

    mprTrace(7, "EnableConnEvents");
    sp = conn->sock;
    if (!conn->async || !sp || conn->delay) {
        return;
    }
    tx = conn->tx;
    rx = conn->rx;
    eventMask = 0;
    conn->lastActivity = conn->http->now;

    if (conn->workerEvent) {
        event = conn->workerEvent;
        conn->workerEvent = 0;
        mprQueueEvent(conn->dispatcher, event);

    } else {
        if (tx) {
            /*
                Three cases for writable:
                - Connector is blocked on a write
                - The connector queue has data and is not doing a SSL handshake
                - The SSL stack has buffered write data that needs to be sent
             */
            if (tx->writeBlocked || (conn->connectorq && conn->connectorq->count > 0 && !mprSocketHandshaking(sp)) || 
                    mprSocketHasBufferedWrite(sp)) {
                eventMask |= MPR_WRITABLE;
            }
            /*
                Cases for readable: (Not eof) and ...
                - Room in the read queue
                - Reading a form 
                - Buffered data in the SSL stack
             */
            q = conn->readq;
            if (!rx->eof && (q->count < q->max || rx->form || mprSocketHasBufferedRead(sp))) {
                eventMask |= MPR_READABLE;
            }
        } else {
            eventMask |= MPR_READABLE;
        }
        httpSetupWaitHandler(conn, eventMask);
    }
    if (tx && tx->handler && tx->handler->module) {
        tx->handler->module->lastActivity = conn->lastActivity;
    }
}


PUBLIC void httpSetupWaitHandler(HttpConn *conn, int eventMask)
{
    MprSocket   *sp;

    sp = conn->sock;
    if (sp == 0) {
        return;
    }
    if (eventMask) {
        if (sp->handler == 0) {
            mprAddSocketHandler(sp, eventMask, conn->dispatcher, conn->ioCallback, conn, 0);
        } else {
            mprSetSocketDispatcher(sp, conn->dispatcher);
            mprEnableSocketEvents(sp, eventMask);
        }
    } else if (sp->handler) {
        mprWaitOn(sp->handler, eventMask);
    }
}


PUBLIC void httpFollowRedirects(HttpConn *conn, bool follow)
{
    conn->followRedirects = follow;
}


/*
    Get the packet into which to read data. Return in *size the length of data to attempt to read.
 */
static HttpPacket *getPacket(HttpConn *conn, ssize *size)
{
    HttpPacket  *packet;
    MprBuf      *content;
    ssize       psize;

    if ((packet = conn->input) == NULL) {
        /*
            Boost the size of the packet if we have already read a largish amount of data
         */
        psize = (conn->rx && conn->rx->bytesRead > BIT_MAX_BUFFER) ? BIT_MAX_BUFFER * 8 : BIT_MAX_BUFFER;
        conn->input = packet = httpCreateDataPacket(psize);
    } else {
        content = packet->content;
        mprResetBufIfEmpty(content);
        if (mprGetBufSpace(content) < BIT_MAX_BUFFER && mprGrowBuf(content, BIT_MAX_BUFFER) < 0) {
            return 0;
        }
    }
    *size = mprGetBufSpace(packet->content);
    assert(*size > 0);
    return packet;
}


PUBLIC int httpGetAsync(HttpConn *conn)
{
    return conn->async;
}


PUBLIC ssize httpGetChunkSize(HttpConn *conn)
{
    if (conn->tx) {
        return conn->tx->chunkSize;
    }
    return 0;
}


PUBLIC void *httpGetConnContext(HttpConn *conn)
{
    return conn->context;
}


PUBLIC void *httpGetConnHost(HttpConn *conn)
{
    return conn->host;
}


PUBLIC ssize httpGetWriteQueueCount(HttpConn *conn)
{
    return conn->writeq ? conn->writeq->count : 0;
}


PUBLIC void httpResetCredentials(HttpConn *conn)
{
    conn->authType = 0;
    conn->username = 0;
    conn->password = 0;
    httpRemoveHeader(conn, "Authorization");
}


PUBLIC void httpSetAsync(HttpConn *conn, int enable)
{
    conn->async = (enable) ? 1 : 0;
}


PUBLIC void httpSetConnNotifier(HttpConn *conn, HttpNotifier notifier)
{
    conn->notifier = notifier;
    if (conn->readq->first) {
        /* Test first rather than count because we want a readable event for the end packet */
        HTTP_NOTIFY(conn, HTTP_EVENT_READABLE, 0);
    }
}


/*
    password and authType can be null
    User may be a combined user:password
 */
PUBLIC void httpSetCredentials(HttpConn *conn, cchar *username, cchar *password, cchar *authType)
{
    char    *ptok;

    httpResetCredentials(conn);
    if (password == NULL && strchr(username, ':') != 0) {
        conn->username = stok(sclone(username), ":", &ptok);
        conn->password = sclone(ptok);
    } else {
        conn->username = sclone(username);
        conn->password = sclone(password);
    }
    if (authType) {
        conn->authType = sclone(authType);
    }
}


PUBLIC void httpSetKeepAliveCount(HttpConn *conn, int count)
{
    conn->keepAliveCount = count;
}


PUBLIC void httpSetChunkSize(HttpConn *conn, ssize size)
{
    if (conn->tx) {
        conn->tx->chunkSize = size;
    }
}


PUBLIC void httpSetHeadersCallback(HttpConn *conn, HttpHeadersCallback fn, void *arg)
{
    conn->headersCallback = fn;
    conn->headersCallbackArg = arg;
}


PUBLIC void httpSetIOCallback(HttpConn *conn, HttpIOCallback fn)
{
    conn->ioCallback = fn;
}


PUBLIC void httpSetConnContext(HttpConn *conn, void *context)
{
    conn->context = context;
}


PUBLIC void httpSetConnHost(HttpConn *conn, void *host)
{
    conn->host = host;
}


/*
    Set the protocol to use for outbound requests
 */
PUBLIC void httpSetProtocol(HttpConn *conn, cchar *protocol)
{
    if (conn->state < HTTP_STATE_CONNECTED) {
        conn->protocol = sclone(protocol);
    }
}


PUBLIC void httpSetRetries(HttpConn *conn, int count)
{
    conn->retries = count;
}


PUBLIC void httpSetState(HttpConn *conn, int targetState)
{
    int     state;

    if (targetState == conn->state) {
        return;
    }
    if (targetState < conn->state) {
        /* Prevent regressions */
        return;
    }
    for (state = conn->state + 1; state <= targetState; state++) { 
        conn->state = state;
        HTTP_NOTIFY(conn, HTTP_EVENT_STATE, state);
    }
}


#if BIT_MPR_TRACING
static char *events[] = {
    "undefined", "state-change", "readable", "writable", "error", "destroy", "app-open", "app-close",
};
static char *states[] = {
    "undefined", "begin", "connected", "first", "parsed", "content", "ready", "running", "finalized", "complete",
};
#endif

PUBLIC void httpNotify(HttpConn *conn, int event, int arg)
{
    if (conn->notifier) {
        if (MPR->logLevel >= 6) {
            if (event == HTTP_EVENT_STATE) {
                mprTrace(6, "Event: change to state \"%s\"", states[conn->state]);
            } else if (event < 0 || event > HTTP_EVENT_MAX) {
                mprTrace(6, "Event: \"%d\" in state \"%s\"", event, states[conn->state]);
            } else {
                mprTrace(6, "Event: \"%s\" in state \"%s\"", events[event], states[conn->state]);
            }
        }
        (conn->notifier)(conn, event, arg);
    }
}


/*
    Set each timeout arg to -1 to skip. Set to zero for no timeout. Otherwise set to number of msecs.
 */
PUBLIC void httpSetTimeout(HttpConn *conn, MprTicks requestTimeout, MprTicks inactivityTimeout)
{
    if (requestTimeout >= 0) {
        if (requestTimeout == 0) {
            conn->limits->requestTimeout = MAXINT;
        } else {
            conn->limits->requestTimeout = requestTimeout;
        }
    }
    if (inactivityTimeout >= 0) {
        if (inactivityTimeout == 0) {
            conn->limits->inactivityTimeout = MAXINT;
        } else {
            conn->limits->inactivityTimeout = inactivityTimeout;
        }
    }
}


PUBLIC HttpLimits *httpSetUniqueConnLimits(HttpConn *conn)
{
    HttpLimits      *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        *limits = *conn->limits;
        conn->limits = limits;
    }
    return limits;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
