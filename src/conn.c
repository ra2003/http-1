/*
    conn.c -- Connection module to handle individual HTTP connections.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static HttpPacket *getPacket(HttpConn *conn, ssize *bytesToRead);
static void manageConn(HttpConn *conn, int flags);
static bool prepForNext(HttpConn *conn);

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
    conn->ioCallback = httpIOEvent;

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
    if (!conn->destroyed) {
        HTTP_NOTIFY(conn, HTTP_EVENT_DESTROY, 0);
        if (httpServerConn(conn)) {
            httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, -1);
            if (conn->activeRequest) {
                httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
                conn->activeRequest = 0;
            }
        }
        httpRemoveConn(conn->http, conn);
        conn->input = 0;
        if (conn->tx) {
            httpClosePipeline(conn);
        }
        if (conn->sock) {
            mprLog(4, "Closing socket connection");
            mprCloseSocket(conn->sock, 0);
        }
        if (conn->dispatcher && conn->dispatcher->flags & MPR_DISPATCHER_AUTO) {
            mprDestroyDispatcher(conn->dispatcher);
        }
        conn->destroyed = 1;
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
        mprMark(conn->grid);
        mprMark(conn->record);
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
    }
}


PUBLIC void httpDisconnect(HttpConn *conn)
{
    if (conn->sock) {
        mprDisconnectSocket(conn->sock);
    }
    conn->connError = 1;
    conn->error = 1;
    conn->keepAliveCount = 0;
    if (conn->tx) {
        conn->tx->finalized = 1;
        conn->tx->finalizedOutput = 1;
        conn->tx->finalizedConnector = 1;
    }
    if (conn->rx) {
        httpSetEof(conn);
    }
}


static void connTimeout(HttpConn *conn, MprEvent *event)
{
    HttpLimits  *limits;
    cchar       *msg, *prefix;

    if (conn->destroyed) {
        return;
    }
    assert(conn->tx);
    assert(conn->rx);

    limits = conn->limits;
    msg = 0;
    assert(limits);
    mprLog(5, "Inactive connection timed out");

    if (conn->timeoutCallback) {
        (conn->timeoutCallback)(conn);
    }
    if (!conn->connError) {
        prefix = (conn->state == HTTP_STATE_BEGIN) ? "Idle connection" : "Request";
        if (conn->timeout == HTTP_PARSE_TIMEOUT) {
            msg = sfmt("%s exceeded parse headers timeout of %Ld sec", prefix, limits->requestParseTimeout  / 1000);

        } else if (conn->timeout == HTTP_INACTIVITY_TIMEOUT) {
            msg = sfmt("%s exceeded inactivity timeout of %Ld sec", prefix, limits->inactivityTimeout / 1000);

        } else if (conn->timeout == HTTP_REQUEST_TIMEOUT) {
            msg = sfmt("%s exceeded timeout %d sec", prefix, limits->requestTimeout / 1000);
        }
        if (conn->rx && (conn->state > HTTP_STATE_CONNECTED)) {
            mprTrace(5, "  State %d, uri %s", conn->state, conn->rx->uri);
        }
        if (conn->state < HTTP_STATE_FIRST) {
            httpDisconnect(conn);
            if (msg) {
                mprLog(5, msg);
            }
        } else {
            httpError(conn, HTTP_CODE_REQUEST_TIMEOUT, msg);
        }
    }
    if (httpClientConn(conn)) {
        httpDestroyConn(conn);
    } else {
        httpEnableConnEvents(conn);
    }
}


PUBLIC void httpScheduleConnTimeout(HttpConn *conn) 
{
    if (!conn->timeoutEvent && !conn->destroyed) {
        /* 
            Will run on the HttpConn dispatcher unless shutting down and it is destroyed already 
         */
        conn->timeoutEvent = mprCreateEvent(conn->dispatcher, "connTimeout", 0, connTimeout, conn, 0);
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

    if (conn->keepAliveCount <= 0) {
        conn->state = HTTP_STATE_BEGIN;
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


#if KEEP
/* 
    Eat remaining input incase last request did not consume all data 
 */
static void consumeLastRequest(HttpConn *conn)
{
    char    junk[4096];

    if (conn->state >= HTTP_STATE_FIRST) {
        while (!httpIsEof(conn) && !httpRequestExpired(conn, 0)) {
            if (httpRead(conn, junk, sizeof(junk)) <= 0) {
                break;
            }
        }
    }
    if (HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_COMPLETE) {
        conn->keepAliveCount = 0;
    }
}
#endif


PUBLIC void httpPrepClientConn(HttpConn *conn, bool keepHeaders)
{
    MprHash     *headers;

    assert(conn);
    if (conn->keepAliveCount > 0 && conn->sock) {
        if (!httpIsEof(conn)) {
            conn->sock = 0;
        }
    } else {
        conn->input = 0;
    }
    conn->connError = 0;
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
    int64       value;
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

    if ((value = httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_CONNECTIONS, 1)) > conn->limits->connectionsMax) {
        mprError("Too many concurrent connections %d/%d", (int) value, conn->limits->connectionsMax);
        httpDestroyConn(conn);
        return 0;
    }
    if (mprGetHashLength(http->addresses) > conn->limits->clientMax) {
        mprError("Too many concurrent clients %d/%d", mprGetHashLength(http->addresses), conn->limits->clientMax);
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
            mprLog(2, "Cannot upgrade socket for SSL: %s", sock->errorMsg);
            mprCloseSocket(sock, 0);
            httpMonitorEvent(conn, HTTP_COUNTER_SSL_ERRORS, 1); 
            httpDestroyConn(conn);
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
PUBLIC void httpIOEvent(HttpConn *conn, MprEvent *event)
{
    HttpPacket  *packet;
    ssize       size;

    if (conn->destroyed) {
        /* Connection has been destroyed */
        return;
    }
    assert(conn->dispatcher == event->dispatcher);
    assert(conn->tx);
    assert(conn->rx);

    mprTrace(6, "httpIOEvent for fd %d, mask %d", conn->sock->fd, event->mask);
    if ((event->mask & MPR_WRITABLE) && conn->connectorq) {
        httpResumeQueue(conn->connectorq);
    }
    if (event->mask & MPR_READABLE) {
        if ((packet = getPacket(conn, &size)) != 0) {
            conn->lastRead = mprReadSocket(conn->sock, mprGetBufEnd(packet->content), size);
            if (conn->lastRead > 0) {
                mprAdjustBufEnd(packet->content, conn->lastRead);
            } else if (conn->lastRead < 0 && mprIsSocketEof(conn->sock)) {
                conn->errorMsg = conn->sock->errorMsg;
                conn->keepAliveCount = 0;
                conn->lastRead = 0;
            }
            mprTrace(6, "http: readEvent read socket %d bytes", conn->lastRead);
        }
    }
    /*
        Process one or more complete requests in the packet
     */
    do {
        /* This is and must be the only place httpProtocol is ever called */
        httpProtocol(conn);
    } while (conn->endpoint && conn->state == HTTP_STATE_COMPLETE && prepForNext(conn));

    /*
        When a request completes, prepForNext will reset the state to HTTP_STATE_BEGIN
     */
    if (conn->endpoint && conn->keepAliveCount <= 0 && conn->state < HTTP_STATE_PARSED) {
        httpDestroyConn(conn);
    } else if (conn->async && !mprIsSocketEof(conn->sock) && !conn->delay) {
        httpEnableConnEvents(conn);
    }
}


PUBLIC void httpEnableConnEvents(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    MprEvent    *event;
    MprSocket   *sp;
    int         eventMask;

    sp = conn->sock;
    rx = conn->rx;
    tx = conn->tx;

    if (conn->workerEvent) {
        /* TODO: This is never used */
        event = conn->workerEvent;
        conn->workerEvent = 0;
        mprQueueEvent(conn->dispatcher, event);
        return;
    }
    eventMask = 0;
    if (rx) {
        if (conn->connError || 
           (tx->writeBlocked) || 
           (conn->connectorq && (conn->connectorq->count > 0 || conn->connectorq->ioCount > 0)) || 
           (httpQueuesNeedService(conn)) || 
           (mprSocketHasBufferedWrite(sp)) ||
           (rx->eof && tx->finalized && conn->state < HTTP_STATE_FINALIZED)) {
            if (!mprSocketHandshaking(sp)) {
                /* Must not pollute the data stream if the SSL stack is still doing manual handshaking */
                eventMask |= MPR_WRITABLE;
            }
        }
        q = conn->readq;
        if (!rx->eof && (q->count < q->max || rx->form || mprSocketHasBufferedRead(sp))) {
            eventMask |= MPR_READABLE;
        }
    } else {
        eventMask |= MPR_READABLE;
    }
    httpSetupWaitHandler(conn, eventMask);
}


/*
    TODO - this is never used
 */
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
    Steal the socket object from a connection. This disconnects the socket from management by the Http service.
    It is the callers responsibility to call mprCloseSocket when required.
    Harder than it looks. We clone the socket, steal the socket handle and set the connection socket handle to invalid.
    This preserves the HttpConn.sock object for the connection and returns a new MprSocket for the caller.
 */
PUBLIC MprSocket *httpStealSocket(HttpConn *conn)
{
    MprSocket   *sock;

    assert(conn->sock);
    assert(!conn->destroyed);

    if (!conn->destroyed) {
        lock(conn->http);
        sock = mprCloneSocket(conn->sock);
        (void) mprStealSocketHandle(conn->sock);
        mprRemoveSocketHandler(conn->sock);
        httpRemoveConn(conn->http, conn);
        httpDiscardData(conn, HTTP_QUEUE_TX);
        httpDiscardData(conn, HTTP_QUEUE_RX);
        httpSetState(conn, HTTP_STATE_COMPLETE);
        /* This will cause httpIOEvent to regard this as a client connection and not destroy this connection */
        conn->endpoint = 0;
        conn->async = 0;
        unlock(conn->http);
        return sock;
    }
    return 0;
}


/*
    Steal the O/S socket handle a connection's socket. This disconnects the socket handle from management by the connection.
    It is the callers responsibility to call close() on the Socket when required.
    Note: this does not change the state of the connection. 
 */
PUBLIC Socket httpStealSocketHandle(HttpConn *conn)
{
    return mprStealSocketHandle(conn->sock);
}


PUBLIC void httpSetupWaitHandler(HttpConn *conn, int eventMask)
{
    MprSocket   *sp;

    sp = conn->sock;
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
            mprMemoryError(0);
            conn->keepAliveCount = 0;
            conn->state = HTTP_STATE_BEGIN;
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
    Test if a request has expired relative to the default inactivity and request timeout limits.
    Set timeout to a non-zero value to apply an overriding smaller timeout
    Set timeout to a value in msec. If timeout is zero, override default limits and wait forever. 
    If timeout is < 0, use default inactivity and duration timeouts. If timeout is > 0, then use this timeout as an additional
    timeout.
 */
PUBLIC bool httpRequestExpired(HttpConn *conn, MprTicks timeout)
{
    HttpLimits  *limits;
    MprTicks    inactivityTimeout, requestTimeout;

    limits = conn->limits;
    if (mprGetDebugMode() || timeout == 0) {
        inactivityTimeout = requestTimeout = MPR_MAX_TIMEOUT;
    } else if (timeout < 0) {
        inactivityTimeout = limits->inactivityTimeout;
        requestTimeout = limits->requestTimeout;
    } else {
        inactivityTimeout = min(limits->inactivityTimeout, timeout);
        requestTimeout = min(limits->requestTimeout, timeout);
    }
    if (mprGetRemainingTicks(conn->started, requestTimeout) < 0) {
        if (requestTimeout != timeout) {
            mprLog(4, "http: Request duration exceeded timeout of %d secs", requestTimeout / 1000);
        }
        return 1;
    }
    if (mprGetRemainingTicks(conn->lastActivity, inactivityTimeout) < 0) {
        if (inactivityTimeout != timeout) {
            mprLog(4, "http: Request timed out due to inactivity of %d secs", inactivityTimeout / 1000);
        }
        return 1;
    }
    return 0;
}


PUBLIC void httpSetConnData(HttpConn *conn, void *data)
{
    conn->data = data;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

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
