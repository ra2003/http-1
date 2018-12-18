/*
    stream.c -- Request / Response Stream module

    Streams are multiplexed otop HttpNet connections.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */
/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void pickStreamNumber(HttpStream *stream);
static void commonPrep(HttpStream *stream);
static void manageStream(HttpStream *stream, int flags);

/*********************************** Code *************************************/
/*
    Create a new connection object. These are multiplexed onto network objects.

    Use httpCreateNet() to create a network object.
 */

PUBLIC HttpStream *httpCreateStream(HttpNet *net, bool peerCreated)
{
    Http        *http;
    HttpQueue   *q;
    HttpStream  *stream;
    HttpLimits  *limits;
    HttpHost    *host;
    HttpRoute   *route;

    assert(net);
    http = HTTP;
    if ((stream = mprAllocObj(HttpStream, manageStream)) == 0) {
        return 0;
    }
    stream->http = http;
    stream->port = -1;
    stream->started = http->now;
    stream->lastActivity = http->now;
    stream->net = net;
    stream->endpoint = net->endpoint;
    stream->notifier = net->notifier;
    stream->sock = net->sock;
    stream->port = net->port;
    stream->ip = net->ip;
    stream->secure = net->secure;
    stream->peerCreated = peerCreated;
    pickStreamNumber(stream);

    if (net->endpoint) {
        host = mprGetFirstItem(net->endpoint->hosts);
        if (host && (route = host->defaultRoute) != 0) {
            stream->limits = route->limits;
            stream->trace = route->trace;
        } else {
            stream->limits = http->serverLimits;
            stream->trace = http->trace;
        }
    } else {
        stream->limits = http->clientLimits;
        stream->trace = http->trace;
    }
    limits = stream->limits;

    if (!peerCreated && ((net->ownStreams >= limits->txStreamsMax) || (net->ownStreams >= limits->streamsMax))) {
        httpNetError(net, "Attempting to create too many streams for network connection: %d/%d/%d", net->ownStreams,
            limits->txStreamsMax, limits->streamsMax);
        return 0;
    }

    stream->keepAliveCount = (net->protocol >= 2) ? 0 : stream->limits->keepAliveMax;
    stream->dispatcher = net->dispatcher;

    stream->rx = httpCreateRx(stream);
    stream->tx = httpCreateTx(stream, NULL);

    q = stream->rxHead = httpCreateQueueHead(net, stream, "RxHead", HTTP_QUEUE_RX);
    q = httpCreateQueue(net, stream, http->tailFilter, HTTP_QUEUE_RX, q);
    if (net->protocol < 2) {
        q = httpCreateQueue(net, stream, http->chunkFilter, HTTP_QUEUE_RX, q);
    }
    if (httpIsServer(net)) {
        q = httpCreateQueue(net, stream, http->uploadFilter, HTTP_QUEUE_RX, q);
    }
    stream->inputq = stream->rxHead->nextQ;
    stream->readq = stream->rxHead;

    q = stream->txHead = httpCreateQueueHead(net, stream, "TxHead", HTTP_QUEUE_TX);
    if (net->protocol < 2) {
        q = httpCreateQueue(net, stream, http->chunkFilter, HTTP_QUEUE_TX, q);
        q = httpCreateQueue(net, stream, http->tailFilter, HTTP_QUEUE_TX, q);
    } else {
        q = httpCreateQueue(net, stream, http->tailFilter, HTTP_QUEUE_TX, q);
    }
    stream->outputq = q;
    stream->writeq = stream->txHead->nextQ;
    httpTraceQueues(stream);
    httpOpenQueues(stream);

#if ME_HTTP_HTTP2
    /*
        The stream->outputq queue window limit is updated on receipt of the peer settings frame and this defines the maximum amount of
        data we can send without receipt of a window flow control update message.
        The stream->inputq window is defined by net->limits and will be
     */
    httpSetQueueLimits(stream->inputq, limits, -1, -1, -1, -1);
    httpSetQueueLimits(stream->outputq, limits, -1, -1, -1, -1);
#endif
    httpSetState(stream, HTTP_STATE_BEGIN);
    httpAddStream(net, stream);
    if (!peerCreated) {
        net->ownStreams++;
    }
    return stream;
}


/*
    Destroy a connection. This removes the connection from the list of connections.
 */
PUBLIC void httpDestroyStream(HttpStream *stream)
{
    if (!stream->destroyed && !stream->net->borrowed) {
        HTTP_NOTIFY(stream, HTTP_EVENT_DESTROY, 0);
        if (stream->tx) {
            httpClosePipeline(stream);
        }
        if (stream->activeRequest) {
            httpMonitorEvent(stream, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
            stream->activeRequest = 0;
        }
        httpDisconnectStream(stream);
        if (!stream->peerCreated) {
            stream->net->ownStreams--;
        }
        stream->destroyed = 1;
        httpRemoveStream(stream->net, stream);
    }
}


static void manageStream(HttpStream *stream, int flags)
{
    assert(stream);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(stream->authType);
        mprMark(stream->authData);
        mprMark(stream->boundary);
        mprMark(stream->context);
        mprMark(stream->data);
        mprMark(stream->dispatcher);
        mprMark(stream->ejs);
        mprMark(stream->endpoint);
        mprMark(stream->errorMsg);
        mprMark(stream->grid);
        mprMark(stream->headersCallbackArg);
        mprMark(stream->http);
        mprMark(stream->host);
        mprMark(stream->inputq);
        mprMark(stream->ip);
        mprMark(stream->limits);
        mprMark(stream->mark);
        mprMark(stream->net);
        mprMark(stream->outputq);
        mprMark(stream->password);
        mprMark(stream->pool);
        mprMark(stream->protocols);
        mprMark(stream->readq);
        mprMark(stream->record);
        mprMark(stream->reqData);
        mprMark(stream->rx);
        mprMark(stream->rxHead);
        mprMark(stream->sock);
        mprMark(stream->timeoutEvent);
        mprMark(stream->trace);
        mprMark(stream->tx);
        mprMark(stream->txHead);
        mprMark(stream->user);
        mprMark(stream->username);
        mprMark(stream->writeq);
    }
}

/*
    Prepare for another request for server
    Return true if there is another request ready for serving
 */
PUBLIC void httpResetServerStream(HttpStream *stream)
{
    assert(httpServerStream(stream));
    assert(stream->state == HTTP_STATE_COMPLETE);

    if (stream->net->borrowed) {
        return;
    }
    if (stream->keepAliveCount <= 0) {
        stream->state = HTTP_STATE_BEGIN;
        return;
    }
    if (stream->tx) {
        stream->tx->stream = 0;
    }
    if (stream->rx) {
        stream->rx->stream = 0;
    }
    stream->authType = 0;
    stream->username = 0;
    stream->password = 0;
    stream->user = 0;
    stream->authData = 0;
    stream->encoded = 0;
    stream->rx = httpCreateRx(stream);
    stream->tx = httpCreateTx(stream, NULL);
    commonPrep(stream);
    assert(stream->state == HTTP_STATE_BEGIN);
}


PUBLIC void httpResetClientStream(HttpStream *stream, bool keepHeaders)
{
    MprHash     *headers;

    assert(stream);

    if (stream->net->protocol < 2) {
        if (stream->state > HTTP_STATE_BEGIN && stream->keepAliveCount > 0 && stream->sock && !httpIsEof(stream)) {
            /* Residual data from past request, cannot continue on this socket */
            stream->sock = 0;
        }
    }
    if (stream->tx) {
        stream->tx->stream = 0;
    }
    if (stream->rx) {
        stream->rx->stream = 0;
    }
    headers = (keepHeaders && stream->tx) ? stream->tx->headers: NULL;
    stream->tx = httpCreateTx(stream, headers);
    stream->rx = httpCreateRx(stream);
    commonPrep(stream);
}


static void commonPrep(HttpStream *stream)
{
    HttpQueue   *q, *next;

    if (stream->timeoutEvent) {
        mprRemoveEvent(stream->timeoutEvent);
        stream->timeoutEvent = 0;
    }
    stream->started = stream->http->now;
    stream->lastActivity = stream->http->now;
    stream->error = 0;
    stream->errorMsg = 0;
    stream->state = 0;
    stream->authRequested = 0;
    stream->complete = 0;

    httpTraceQueues(stream);
    for (q = stream->txHead->nextQ; q != stream->txHead; q = next) {
        next = q->nextQ;
        if (q->flags & HTTP_QUEUE_REQUEST) {
            httpRemoveQueue(q);
        } else {
            q->flags &= (HTTP_QUEUE_OPENED | HTTP_QUEUE_OUTGOING);
        }
    }
    stream->writeq = stream->txHead->nextQ;

    for (q = stream->rxHead->nextQ; q != stream->rxHead; q = next) {
        next = q->nextQ;
        if (q->flags & HTTP_QUEUE_REQUEST) {
            httpRemoveQueue(q);
        } else {
            q->flags &= (HTTP_QUEUE_OPENED);
        }
    }
    stream->readq = stream->rxHead;
    httpTraceQueues(stream);

    httpDiscardData(stream, HTTP_QUEUE_TX);
    httpDiscardData(stream, HTTP_QUEUE_RX);

    httpSetState(stream, HTTP_STATE_BEGIN);
    pickStreamNumber(stream);
}


static void pickStreamNumber(HttpStream *stream)
{
#if ME_HTTP_HTTP2
    HttpNet     *net;

    net = stream->net;
    if (net->protocol >= 2 && !httpIsServer(net)) {
        stream->streamID = net->nextStreamID;
        net->nextStreamID += 2;
        if (stream->streamID >= HTTP2_MAX_STREAM) {
            //TODO - must recreate connection. Cannot use this connection any more.
        }
    }
#endif
}


PUBLIC void httpDisconnectStream(HttpStream *stream)
{
    HttpTx      *tx;

    tx = stream->tx;
    stream->error++;
    if (tx) {
        tx->responded = 1;
        tx->finalized = 1;
        tx->finalizedOutput = 1;
        tx->finalizedConnector = 1;
    }
    if (stream->rx) {
        httpSetEof(stream);
    }
    if (stream->net->protocol < 2) {
        mprDisconnectSocket(stream->sock);
    }
}


static void connTimeout(HttpStream *stream, MprEvent *mprEvent)
{
    HttpLimits  *limits;
    cchar       *event, *msg, *prefix;

    if (stream->destroyed) {
        return;
    }
    assert(stream->tx);
    assert(stream->rx);

    msg = 0;
    event = 0;
    limits = stream->limits;
    assert(limits);

    if (stream->timeoutCallback) {
        (stream->timeoutCallback)(stream);
    }
    prefix = (stream->state == HTTP_STATE_BEGIN) ? "Idle connection" : "Request";
    if (stream->timeout == HTTP_PARSE_TIMEOUT) {
        msg = sfmt("%s exceeded parse headers timeout of %lld sec", prefix, limits->requestParseTimeout  / 1000);
        event = "timeout.parse";

    } else if (stream->timeout == HTTP_INACTIVITY_TIMEOUT) {
        if (httpClientStream(stream)) {
            msg = sfmt("%s exceeded inactivity timeout of %lld sec", prefix, limits->inactivityTimeout / 1000);
            event = "timeout.inactivity";
        }

    } else if (stream->timeout == HTTP_REQUEST_TIMEOUT) {
        msg = sfmt("%s exceeded timeout %lld sec", prefix, limits->requestTimeout / 1000);
        event = "timeout.duration";
    }
    if (stream->state < HTTP_STATE_FIRST) {
        if (msg) {
            httpTrace(stream->trace, event, "error", "msg:'%s'", msg);
            stream->errorMsg = msg;
        }
        httpDisconnectStream(stream);

    } else {
        httpError(stream, HTTP_CODE_REQUEST_TIMEOUT, "%s", msg);
    }
}


PUBLIC void httpStreamTimeout(HttpStream *stream)
{
    if (!stream->timeoutEvent && !stream->destroyed) {
        /*
            Will run on the HttpStream dispatcher unless shutting down and it is destroyed already
         */
        stream->timeoutEvent = mprCreateEvent(stream->dispatcher, "connTimeout", 0, connTimeout, stream, 0);
    }
}


PUBLIC void httpFollowRedirects(HttpStream *stream, bool follow)
{
    stream->followRedirects = follow;
}


PUBLIC ssize httpGetChunkSize(HttpStream *stream)
{
    if (stream->tx) {
        return stream->tx->chunkSize;
    }
    return 0;
}


PUBLIC void *httpGetStreamContext(HttpStream *stream)
{
    return stream->context;
}


PUBLIC void *httpGetStreamHost(HttpStream *stream)
{
    return stream->host;
}


PUBLIC ssize httpGetWriteQueueCount(HttpStream *stream)
{
    return stream->writeq ? stream->writeq->count : 0;
}


PUBLIC void httpResetCredentials(HttpStream *stream)
{
    stream->authType = 0;
    stream->username = 0;
    stream->password = 0;
    httpRemoveHeader(stream, "Authorization");
}


PUBLIC void httpSetStreamNotifier(HttpStream *stream, HttpNotifier notifier)
{
    stream->notifier = notifier;
    /*
        Only issue a readable event if streaming or already routed
     */
    if (stream->readq->first && stream->rx->route) {
        HTTP_NOTIFY(stream, HTTP_EVENT_READABLE, 0);
    }
}


/*
    Password and authType can be null
    User may be a combined user:password
 */
PUBLIC void httpSetCredentials(HttpStream *stream, cchar *username, cchar *password, cchar *authType)
{
    char    *ptok;

    httpResetCredentials(stream);
    if (password == NULL && strchr(username, ':') != 0) {
        stream->username = ssplit(sclone(username), ":", &ptok);
        stream->password = sclone(ptok);
    } else {
        stream->username = sclone(username);
        stream->password = sclone(password);
    }
    if (authType) {
        stream->authType = sclone(authType);
    }
}


PUBLIC void httpSetKeepAliveCount(HttpStream *stream, int count)
{
    stream->keepAliveCount = count;
}


PUBLIC void httpSetChunkSize(HttpStream *stream, ssize size)
{
    if (stream->tx) {
        stream->tx->chunkSize = size;
    }
}


PUBLIC void httpSetHeadersCallback(HttpStream *stream, HttpHeadersCallback fn, void *arg)
{
    stream->headersCallback = fn;
    stream->headersCallbackArg = arg;
}


PUBLIC void httpSetStreamContext(HttpStream *stream, void *context)
{
    stream->context = context;
}


PUBLIC void httpSetStreamHost(HttpStream *stream, void *host)
{
    stream->host = host;
}


PUBLIC void httpSetState(HttpStream *stream, int targetState)
{
    int     state;

    if (targetState == stream->state) {
        return;
    }
    if (targetState < stream->state) {
        /* Prevent regressions */
        return;
    }
    for (state = stream->state + 1; state <= targetState; state++) {
        stream->state = state;
        HTTP_NOTIFY(stream, HTTP_EVENT_STATE, state);
    }
}


PUBLIC void httpNotify(HttpStream *stream, int event, int arg)
{
    if (stream->notifier) {
        (stream->notifier)(stream, event, arg);
    }
}


/*
    Set each timeout arg to -1 to skip. Set to zero for no timeout. Otherwise set to number of msecs.
 */
PUBLIC void httpSetTimeout(HttpStream *stream, MprTicks requestTimeout, MprTicks inactivityTimeout)
{
    if (requestTimeout >= 0) {
        if (requestTimeout == 0) {
            stream->limits->requestTimeout = HTTP_UNLIMITED;
        } else {
            stream->limits->requestTimeout = requestTimeout;
        }
    }
    if (inactivityTimeout >= 0) {
        if (inactivityTimeout == 0) {
            stream->limits->inactivityTimeout = HTTP_UNLIMITED;
            // TODO - need separate timeouts for net
            stream->net->limits->inactivityTimeout = HTTP_UNLIMITED;
        } else {
            stream->limits->inactivityTimeout = inactivityTimeout;
            // TODO - need separate timeouts for net
            stream->net->limits->inactivityTimeout = inactivityTimeout;
        }
    }
}


PUBLIC HttpLimits *httpSetUniqueStreamLimits(HttpStream *stream)
{
    HttpLimits      *limits;

    if ((limits = mprAllocStruct(HttpLimits)) != 0) {
        *limits = *stream->limits;
        stream->limits = limits;
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
PUBLIC bool httpRequestExpired(HttpStream *stream, MprTicks timeout)
{
    HttpLimits  *limits;
    MprTicks    inactivityTimeout, requestTimeout;

    limits = stream->limits;
    if (mprGetDebugMode() || timeout == 0) {
        inactivityTimeout = requestTimeout = MPR_MAX_TIMEOUT;

    } else if (timeout < 0) {
        inactivityTimeout = limits->inactivityTimeout;
        requestTimeout = limits->requestTimeout;

    } else {
        inactivityTimeout = min(limits->inactivityTimeout, timeout);
        requestTimeout = min(limits->requestTimeout, timeout);
    }

    if (mprGetRemainingTicks(stream->started, requestTimeout) < 0) {
        if (requestTimeout != timeout) {
            httpTrace(stream->trace, "timeout.duration", "error",
                "msg:'Request cancelled exceeded max duration',timeout:%lld", requestTimeout / 1000);
        }
        return 1;
    }
    if (mprGetRemainingTicks(stream->lastActivity, inactivityTimeout) < 0) {
        if (inactivityTimeout != timeout) {
            httpTrace(stream->trace, "timeout.inactivity", "error",
                "msg:'Request cancelled due to inactivity',timeout:%lld", inactivityTimeout / 1000);
        }
        return 1;
    }
    return 0;
}


PUBLIC void httpSetStreamData(HttpStream *stream, void *data)
{
    stream->data = data;
}


PUBLIC void httpSetStreamReqData(HttpStream *stream, void *data)
{
    stream->reqData = data;
}


PUBLIC void httpTraceQueues(HttpStream *stream)
{
#if DEBUG
    HttpQueue   *q;

    print("");
    if (stream->inputq) {
        printf("%s ", stream->rxHead->name);
        for (q = stream->rxHead->prevQ; q != stream->rxHead; q = q->prevQ) {
            printf("%s ", q->name);
        }
        printf(" <- INPUT\n");
    }
    if (stream->outputq) {
        printf("%s ", stream->txHead->name);
        for (q = stream->txHead->nextQ; q != stream->txHead; q = q->nextQ) {
            printf("%s ", q->name);
        }
        printf("-> OUTPUT\n");
    }
    print("");
    printf("READ   %s\n", stream->readq->name);
    printf("WRITE  %s\n", stream->writeq->name);
    printf("INPUT  %s\n", stream->inputq->name);
    printf("OUTPUT %s\n", stream->outputq->name);
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
