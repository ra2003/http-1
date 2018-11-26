/*
    conn.c -- Connection module to handle individual HTTP connections.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */
/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void pickStreamNumber(HttpConn *conn);
static void commonPrep(HttpConn *conn);
static void manageConn(HttpConn *conn, int flags);

/*********************************** Code *************************************/
/*
    Create a new connection object. These are multiplexed onto network objects.

    Use httpCreateNet() to create a network object.
 */

PUBLIC HttpConn *httpCreateConn(HttpNet *net)
{
    Http        *http;
    HttpQueue   *q;
    HttpConn    *conn;
    HttpLimits  *limits;
    HttpHost    *host;
    HttpRoute   *route;

    assert(net);

    http = HTTP;
    if ((conn = mprAllocObj(HttpConn, manageConn)) == 0) {
        return 0;
    }
    conn->http = http;
    conn->port = -1;
    conn->started = http->now;
    conn->lastActivity = http->now;
    conn->net = net;
    conn->endpoint = net->endpoint;
    conn->notifier = net->notifier;
    conn->sock = net->sock;
    conn->port = net->port;
    conn->ip = net->ip;
    conn->secure = net->secure;
    pickStreamNumber(conn);

    if (net->endpoint) {
        host = mprGetFirstItem(net->endpoint->hosts);
        if (host && (route = host->defaultRoute) != 0) {
            conn->limits = route->limits;
            conn->trace = route->trace;
        } else {
            conn->limits = http->serverLimits;
            conn->trace = http->trace;
        }
    } else {
        conn->limits = http->clientLimits;
        conn->trace = http->trace;
    }
    limits = conn->limits;
    conn->keepAliveCount = (net->protocol >= 2) ? 0 : conn->limits->keepAliveMax;
    conn->dispatcher = net->dispatcher;

    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);

    q = conn->rxHead = httpCreateQueueHead(net, conn, "RxHead", HTTP_QUEUE_RX);
    q = httpCreateQueue(net, conn, http->tailFilter, HTTP_QUEUE_RX, q);
    if (net->protocol < 2) {
        q = httpCreateQueue(net, conn, http->chunkFilter, HTTP_QUEUE_RX, q);
    }
    if (httpIsServer(net)) {
        q = httpCreateQueue(net, conn, http->uploadFilter, HTTP_QUEUE_RX, q);
    }
    conn->inputq = conn->rxHead->nextQ;
    conn->readq = conn->rxHead;

    q = conn->txHead = httpCreateQueueHead(net, conn, "TxHead", HTTP_QUEUE_TX);
    if (net->protocol < 2) {
        q = httpCreateQueue(net, conn, http->chunkFilter, HTTP_QUEUE_TX, q);
        q = httpCreateQueue(net, conn, http->tailFilter, HTTP_QUEUE_TX, q);
    } else {
        q = httpCreateQueue(net, conn, http->tailFilter, HTTP_QUEUE_TX, q);
    }
    conn->outputq = q;
    conn->writeq = conn->txHead->nextQ;
    httpTraceQueues(conn);
    httpOpenQueues(conn);

#if ME_HTTP_HTTP2
    httpSetQueueLimits(conn->inputq, limits->frameSize, -1, net->inputq->max);
    httpSetQueueLimits(conn->outputq, limits->frameSize, -1, net->inputq->max);
#endif
    httpSetState(conn, HTTP_STATE_BEGIN);
    httpAddConn(net, conn);
    return conn;
}


/*
    Destroy a connection. This removes the connection from the list of connections.
 */
PUBLIC void httpDestroyConn(HttpConn *conn)
{
    if (!conn->destroyed && !conn->net->borrowed) {
        HTTP_NOTIFY(conn, HTTP_EVENT_DESTROY, 0);
        if (conn->tx) {
            httpClosePipeline(conn);
        }
        if (conn->activeRequest) {
            httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
            conn->activeRequest = 0;
        }
        httpDisconnectConn(conn);
        httpRemoveConn(conn->net, conn);
        conn->destroyed = 1;
    }
}


static void manageConn(HttpConn *conn, int flags)
{
    assert(conn);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(conn->authType);
        mprMark(conn->authData);
        mprMark(conn->boundary);
        mprMark(conn->context);
        mprMark(conn->data);
        mprMark(conn->dispatcher);
        mprMark(conn->ejs);
        mprMark(conn->endpoint);
        mprMark(conn->errorMsg);
        mprMark(conn->grid);
        mprMark(conn->headersCallbackArg);
        mprMark(conn->http);
        mprMark(conn->host);
        mprMark(conn->inputq);
        mprMark(conn->ip);
        mprMark(conn->limits);
        mprMark(conn->mark);
        mprMark(conn->net);
        mprMark(conn->outputq);
        mprMark(conn->password);
        mprMark(conn->pool);
        mprMark(conn->protocols);
        mprMark(conn->readq);
        mprMark(conn->record);
        mprMark(conn->reqData);
        mprMark(conn->rx);
        mprMark(conn->rxHead);
        mprMark(conn->sock);
        mprMark(conn->timeoutEvent);
        mprMark(conn->trace);
        mprMark(conn->tx);
        mprMark(conn->txHead);
        mprMark(conn->user);
        mprMark(conn->username);
        mprMark(conn->writeq);
    }
}

/*
    Prepare for another request for server
    Return true if there is another request ready for serving
 */
PUBLIC void httpResetServerConn(HttpConn *conn)
{
    assert(httpServerConn(conn));
    assert(conn->state == HTTP_STATE_COMPLETE);

    if (conn->net->borrowed) {
        return;
    }
    if (conn->keepAliveCount <= 0) {
        conn->state = HTTP_STATE_BEGIN;
        return;
    }
    if (conn->tx) {
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
}


PUBLIC void httpResetClientConn(HttpConn *conn, bool keepHeaders)
{
    MprHash     *headers;

    assert(conn);

    if (conn->net->protocol < 2) {
        if (conn->state > HTTP_STATE_BEGIN && conn->keepAliveCount > 0 && conn->sock && !httpIsEof(conn)) {
            /* Residual data from past request, cannot continue on this socket */
            conn->sock = 0;
        }
    }
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


static void commonPrep(HttpConn *conn)
{
    HttpQueue   *q, *next;

    if (conn->timeoutEvent) {
        mprRemoveEvent(conn->timeoutEvent);
        conn->timeoutEvent = 0;
    }
    conn->lastActivity = conn->http->now;
    conn->error = 0;
    conn->errorMsg = 0;
    conn->state = 0;
    conn->authRequested = 0;
    conn->complete = 0;

    httpTraceQueues(conn);
    for (q = conn->txHead->nextQ; q != conn->txHead; q = next) {
        next = q->nextQ;
        if (q->flags & HTTP_QUEUE_REQUEST) {
            httpRemoveQueue(q);
        } else {
            q->flags &= (HTTP_QUEUE_OPENED | HTTP_QUEUE_OUTGOING);
        }
    }
    conn->writeq = conn->txHead->nextQ;

    for (q = conn->rxHead->nextQ; q != conn->rxHead; q = next) {
        next = q->nextQ;
        if (q->flags & HTTP_QUEUE_REQUEST) {
            httpRemoveQueue(q);
        } else {
            q->flags &= (HTTP_QUEUE_OPENED);
        }
    }
    conn->readq = conn->rxHead;
    httpTraceQueues(conn);

    httpDiscardData(conn, HTTP_QUEUE_TX);
    httpDiscardData(conn, HTTP_QUEUE_RX);

    httpSetState(conn, HTTP_STATE_BEGIN);
    pickStreamNumber(conn);
}


static void pickStreamNumber(HttpConn *conn)
{
#if ME_HTTP_HTTP2
    HttpNet     *net;

    net = conn->net;
    if (net->protocol >= 2 && !httpIsServer(net)) {
        conn->stream = net->nextStream;
        net->nextStream += 2;
        if (conn->stream >= HTTP2_MAX_STREAM) {
            //TODO - must recreate connection. Cannot use this connection any more.
        }
    }
#endif
}


PUBLIC void httpDisconnectConn(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    conn->error++;
    if (tx) {
        tx->responded = 1;
        tx->finalized = 1;
        tx->finalizedOutput = 1;
        tx->finalizedConnector = 1;
    }
    if (conn->rx) {
        httpSetEof(conn);
    }
    if (conn->net->protocol < 2) {
        mprDisconnectSocket(conn->sock);
    }
}


static void connTimeout(HttpConn *conn, MprEvent *mprEvent)
{
    HttpLimits  *limits;
    cchar       *event, *msg, *prefix;

    if (conn->destroyed) {
        return;
    }
    assert(conn->tx);
    assert(conn->rx);

    msg = 0;
    event = 0;
    limits = conn->limits;
    assert(limits);

    if (conn->timeoutCallback) {
        (conn->timeoutCallback)(conn);
    }
    prefix = (conn->state == HTTP_STATE_BEGIN) ? "Idle connection" : "Request";
    if (conn->timeout == HTTP_PARSE_TIMEOUT) {
        msg = sfmt("%s exceeded parse headers timeout of %lld sec", prefix, limits->requestParseTimeout  / 1000);
        event = "timeout.parse";

    } else if (conn->timeout == HTTP_INACTIVITY_TIMEOUT) {
        if (httpClientConn(conn)) {
            msg = sfmt("%s exceeded inactivity timeout of %lld sec", prefix, limits->inactivityTimeout / 1000);
            event = "timeout.inactivity";
        }

    } else if (conn->timeout == HTTP_REQUEST_TIMEOUT) {
        msg = sfmt("%s exceeded timeout %lld sec", prefix, limits->requestTimeout / 1000);
        event = "timeout.duration";
    }
    if (conn->state < HTTP_STATE_FIRST) {
        if (msg) {
            httpTrace(conn->trace, event, "error", "msg:'%s'", msg);
            conn->errorMsg = msg;
        }
        httpDisconnectConn(conn);

    } else {
        httpError(conn, HTTP_CODE_REQUEST_TIMEOUT, "%s", msg);
    }
}


PUBLIC void httpConnTimeout(HttpConn *conn)
{
    if (!conn->timeoutEvent && !conn->destroyed) {
        /*
            Will run on the HttpConn dispatcher unless shutting down and it is destroyed already
         */
        conn->timeoutEvent = mprCreateEvent(conn->dispatcher, "connTimeout", 0, connTimeout, conn, 0);
    }
}


PUBLIC void httpFollowRedirects(HttpConn *conn, bool follow)
{
    conn->followRedirects = follow;
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


PUBLIC void httpSetConnNotifier(HttpConn *conn, HttpNotifier notifier)
{
    conn->notifier = notifier;
    /*
        Only issue a readable event if streaming or already routed
     */
    if (conn->readq->first && conn->rx->route) {
        HTTP_NOTIFY(conn, HTTP_EVENT_READABLE, 0);
    }
}


/*
    Password and authType can be null
    User may be a combined user:password
 */
PUBLIC void httpSetCredentials(HttpConn *conn, cchar *username, cchar *password, cchar *authType)
{
    char    *ptok;

    httpResetCredentials(conn);
    if (password == NULL && strchr(username, ':') != 0) {
        conn->username = ssplit(sclone(username), ":", &ptok);
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


PUBLIC void httpSetConnContext(HttpConn *conn, void *context)
{
    conn->context = context;
}


PUBLIC void httpSetConnHost(HttpConn *conn, void *host)
{
    conn->host = host;
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


PUBLIC void httpNotify(HttpConn *conn, int event, int arg)
{
    if (conn->notifier) {
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
            conn->limits->requestTimeout = HTTP_UNLIMITED;
        } else {
            conn->limits->requestTimeout = requestTimeout;
        }
    }
    if (inactivityTimeout >= 0) {
        if (inactivityTimeout == 0) {
            conn->limits->inactivityTimeout = HTTP_UNLIMITED;
            // TODO - need separate timeouts for net
            conn->net->limits->inactivityTimeout = HTTP_UNLIMITED;
        } else {
            conn->limits->inactivityTimeout = inactivityTimeout;
            // TODO - need separate timeouts for net
            conn->net->limits->inactivityTimeout = inactivityTimeout;
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
    If timeout is < 0, use default inactivity and duration timeouts.
    If timeout is > 0, then use this timeout as an additional timeout.
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
            httpTrace(conn->trace, "timeout.duration", "error",
                "msg:'Request cancelled exceeded max duration',timeout:%lld", requestTimeout / 1000);
        }
        return 1;
    }
    if (mprGetRemainingTicks(conn->lastActivity, inactivityTimeout) < 0) {
        if (inactivityTimeout != timeout) {
            httpTrace(conn->trace, "timeout.inactivity", "error",
                "msg:'Request cancelled due to inactivity',timeout:%lld", inactivityTimeout / 1000);
        }
        return 1;
    }
    return 0;
}


PUBLIC void httpSetConnData(HttpConn *conn, void *data)
{
    conn->data = data;
}


PUBLIC void httpSetConnReqData(HttpConn *conn, void *data)
{
    conn->reqData = data;
}


PUBLIC void httpTraceQueues(HttpConn *conn)
{
#if DEBUG
    HttpQueue   *q;

    print("");
    if (conn->inputq) {
        printf("%s ", conn->rxHead->name);
        for (q = conn->rxHead->prevQ; q != conn->rxHead; q = q->prevQ) {
            printf("%s ", q->name);
        }
        printf(" <- INPUT\n");
    }
    if (conn->outputq) {
        printf("%s ", conn->txHead->name);
        for (q = conn->txHead->nextQ; q != conn->txHead; q = q->nextQ) {
            printf("%s ", q->name);
        }
        printf("-> OUTPUT\n");
    }
    print("");
    printf("READ   %s\n", conn->readq->name);
    printf("WRITE  %s\n", conn->writeq->name);
    printf("INPUT  %s\n", conn->inputq->name);
    printf("OUTPUT %s\n", conn->outputq->name);
#endif
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
