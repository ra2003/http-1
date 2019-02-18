/*
    http1Filter.c - HTTP/1 protocol handling.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static HttpStream *findStream(HttpQueue *q);
static char *getToken(HttpPacket *packet, cchar *delim);
static bool gotHeaders(HttpQueue *q, HttpPacket *packet);
static void incomingHttp1(HttpQueue *q, HttpPacket *packet);
static bool monitorActiveRequests(HttpStream *stream);
static void outgoingHttp1(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp1Service(HttpQueue *q);
static HttpPacket *parseFields(HttpQueue *q, HttpPacket *packet);
static HttpPacket *parseHeaders(HttpQueue *q, HttpPacket *packet);
static void parseRequestLine(HttpQueue *q, HttpPacket *packet);
static void parseResponseLine(HttpQueue *q, HttpPacket *packet);
static void logPacket(HttpQueue *q, HttpPacket *packet);

/*********************************** Code *************************************/
/*
   Loadable module initialization
 */
PUBLIC int httpOpenHttp1Filter()
{
    HttpStage     *filter;

    if ((filter = httpCreateStreamector("Http1Filter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->http1Filter = filter;
    filter->incoming = incomingHttp1;
    filter->outgoing = outgoingHttp1;
    filter->outgoingService = outgoingHttp1Service;
    return 0;
}


/*
    The queue is the net->inputq == netHttp-rx
 */
static void incomingHttp1(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;

    stream = findStream(q);

    /*
        There will typically be no packets on the queue, so this will be fast
     */
    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);

    for (packet = httpGetPacket(q); packet && !stream->error; packet = httpGetPacket(q)) {
        if (httpTracing(q->net)) {
            httpLogPacket(q->net->trace, "http1.rx", "packet", 0, packet, NULL);
        }
        if (stream->state < HTTP_STATE_PARSED) {
            if ((packet = parseHeaders(q, packet)) != 0) {
                if (stream->state < HTTP_STATE_PARSED) {
                    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
                    break;
                }
                httpProcessHeaders(stream->inputq);
            }
        }
        if (packet) {
            httpPutPacket(stream->inputq, packet);
        }
    }
    httpProcess(stream->inputq);
}


static void outgoingHttp1(HttpQueue *q, HttpPacket *packet)
{
    httpPutForService(q, packet, 1);
}


static void outgoingHttp1Service(HttpQueue *q)
{
    HttpStream  *stream;
    HttpPacket  *packet;

    stream = q->stream;
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillQueueAcceptPacket(q, q->net->socketq, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        logPacket(q, packet);
        httpPutPacket(q->net->socketq, packet);
        if (stream && q->count <= q->low && (stream->outputq->flags & HTTP_QUEUE_SUSPENDED)) {
            httpResumeQueue(stream->outputq);
        }
    }
}


static void logPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    cchar       *type;
    ssize       len;

    net = q->net;
    type = (packet->type & HTTP_PACKET_HEADER) ? "headers" : "data";
    len = httpGetPacketLength(packet) + mprGetBufLength(packet->prefix);
    if (httpTracing(net) && !net->skipTrace) {
        if (net->bytesWritten >= net->trace->maxContent) {
            httpLog(net->trace, "http1.tx", "packet", "msg: 'Abbreviating packet trace'");
            net->skipTrace = 1;
        } else {
            httpLogPacket(net->trace, "http1.tx", "packet", HTTP_TRACE_HEX, packet, "type=%s, length=%zd,", type, len);
        }
    } else {
        httpLog(net->trace, "http1.tx", "packet", "type=%s, length=%zd,", type, len);
    }
}


static HttpPacket *parseHeaders(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpRx      *rx;

    stream = q->stream;
    assert(stream->rx);
    assert(stream->tx);
    rx = stream->rx;

    if (!monitorActiveRequests(stream)) {
        return 0;
    }
    if (!gotHeaders(q, packet)) {
        /* Don't yet have a complete header */
        return packet;
    }
    rx->headerPacket = packet;

    if (httpServerStream(stream)) {
        parseRequestLine(q, packet);
    } else {
        parseResponseLine(q, packet);
    }
    return parseFields(q, packet);
}


static bool monitorActiveRequests(HttpStream *stream)
{
    HttpLimits  *limits;
    int64       value;

    limits = stream->limits;

    //  TODO - find a better place for this?  Where does http2 do this
    if (httpServerStream(stream) && !stream->activeRequest) {
        /*
            ErrorDocuments may come through here twice so test activeRequest to keep counters valid.
         */
        stream->activeRequest = 1;
        if ((value = httpMonitorEvent(stream, HTTP_COUNTER_ACTIVE_REQUESTS, 1)) >= limits->requestsPerClientMax) {
            httpError(stream, HTTP_ABORT | HTTP_CODE_SERVICE_UNAVAILABLE,
                "Too many concurrent requests for client: %s %d/%d", stream->ip, (int) value, limits->requestsPerClientMax);
            return 0;
        }
        httpMonitorEvent(stream, HTTP_COUNTER_REQUESTS, 1);
    }
    return 1;
}


static cchar *eatBlankLines(HttpPacket *packet)
{
    cchar   *start;

    start = mprGetBufStart(packet->content);
    while (*start == '\r' || *start == '\n') {
        if (mprGetCharFromBuf(packet->content) < 0) {
            break;
        }
        start = mprGetBufStart(packet->content);
    }
    return start;
}


static bool gotHeaders(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpLimits  *limits;
    cchar       *end, *start;
    ssize       len;

    stream = q->stream;
    limits = stream->limits;
    start = eatBlankLines(packet);
    len = httpGetPacketLength(packet);

    if ((end = sncontains(start, "\r\n\r\n", len)) != 0 || (end = sncontains(start, "\n\n", len)) != 0) {
        len = end - start;
    }
    if (len >= limits->headerSize || len >= q->max) {
        httpLimitError(stream, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
            "Header too big. Length %zd vs limit %d", len, limits->headerSize);
        return 0;
    }
    if (!end) {
        return 0;
    }
    return 1;
}


/*
    Parse the first line of a http request. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Requests look like: METHOD URL HTTP/1.X.
 */
static void parseRequestLine(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpRx      *rx;
    HttpLimits  *limits;
    char        *method, *uri, *protocol;
    ssize       len;

    stream = q->stream;
    rx = stream->rx;
    limits = stream->limits;

    method = getToken(packet, NULL);
    rx->originalMethod = rx->method = supper(method);
    httpParseMethod(stream);

    uri = getToken(packet, NULL);
    len = slen(uri);
    if (*uri == '\0') {
        httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
        return;
    } else if (len >= limits->uriSize) {
        httpLimitError(stream, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad request. URI too long. Length %zd vs limit %d", len, limits->uriSize);
        return;
    }
    rx->uri = sclone(uri);
    if (!rx->originalUri) {
        rx->originalUri = rx->uri;
    }
    protocol = getToken(packet, "\r\n");
    protocol = supper(protocol);
    if (smatch(protocol, "HTTP/1.0") || *protocol == 0) {
        if (rx->flags & (HTTP_POST|HTTP_PUT)) {
            rx->remainingContent = HTTP_UNLIMITED;
            rx->needInputPipeline = 1;
        }
        stream->net->protocol = 0;
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return;
    } else {
        stream->net->protocol = 1;
    }
    httpSetState(stream, HTTP_STATE_FIRST);
}


/*
    Parse the first line of a http response. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Response status lines look like: HTTP/1.X CODE Message
 */
static void parseResponseLine(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream  *stream;
    HttpRx      *rx;
    HttpTx      *tx;
    char        *protocol, *status;
    ssize       len;

    net = q->net;
    stream = q->stream;
    rx = stream->rx;
    tx = stream->tx;

    protocol = supper(getToken(packet, NULL));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        net->protocol = 0;
        if (!scaselessmatch(tx->method, "HEAD")) {
            rx->remainingContent = HTTP_UNLIMITED;
        }
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return;
    }
    status = getToken(packet, NULL);
    if (*status == '\0') {
        httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
        return;
    }
    rx->status = atoi(status);
    rx->statusMessage = sclone(getToken(packet, "\r\n"));

    len = slen(rx->statusMessage);
    if (len >= stream->limits->uriSize) {
        httpLimitError(stream, HTTP_CLOSE | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad response. Status message too long. Length %zd vs limit %d", len, stream->limits->uriSize);
        return;
    }
    if (rx->status == HTTP_CODE_CONTINUE) {
        /* Eat the blank line and wait for the real response */
        mprAdjustBufStart(packet->content, 2);
        return;
    }
}


/*
    Parse the header fields and return a following body packet if present.
    Return zero on errors.
 */
static HttpPacket *parseFields(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpRx      *rx;
    HttpLimits  *limits;
    char        *key, *value;
    int         count;

    stream = q->stream;
    rx = stream->rx;

    limits = stream->limits;

    for (count = 0; packet->content->start[0] != '\r' && !stream->error; count++) {
        if (count >= limits->headerMax) {
            httpLimitError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Too many headers");
            return 0;
        }
        if ((key = getToken(packet, ":")) == 0 || *key == '\0') {
            httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        value = getToken(packet, "\r\n");
        while (isspace((uchar) *value)) {
            value++;
        }
        if (strspn(key, "%<>/\\") > 0) {
            httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header key value");
            return 0;
        }
        if (scaselessmatch(key, "set-cookie")) {
            mprAddDuplicateKey(rx->headers, key, sclone(value));
        } else {
            mprAddKey(rx->headers, key, sclone(value));
        }
    }
    /*
        Split the headers and retain the data for later. Step over "\r\n" after headers except if chunked
        so chunking can parse a single chunk delimiter of "\r\nSIZE ...\r\n"
     */
    if (smatch(httpGetHeader(stream, "transfer-encoding"), "chunked")) {
        httpInitChunking(stream);
    } else {
        if (mprGetBufLength(packet->content) < 2) {
            httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        mprAdjustBufStart(packet->content, 2);
    }
    httpSetState(stream, HTTP_STATE_PARSED);
    /*
        If data remaining, it is body post data
     */
    return httpGetPacketLength(packet) ? packet : 0;
}


/*
    Get the next input token. The content buffer is advanced to the next token. This routine always returns a
    non-null token. The empty string means the delimiter was not found. The delimiter is a string to match and not
    a set of characters. If null, it means use white space (space or tab) as a delimiter.
 */
static char *getToken(HttpPacket *packet, cchar *delim)
{
    MprBuf  *buf;
    char    *token, *endToken, *nextToken;

    buf = packet->content;
    nextToken = mprGetBufEnd(buf);

    for (token = mprGetBufStart(buf); (*token == ' ' || *token == '\t') && token < nextToken; token++) {}
    if (token >= nextToken) {
        return "";
    }
    if (delim == 0) {
        delim = " \t";
        if ((endToken = strpbrk(token, delim)) != 0) {
            nextToken = endToken + strspn(endToken, delim);
            *endToken = '\0';
        }
    } else {
        if ((endToken = strstr(token, delim)) != 0) {
            *endToken = '\0';
            /* Only eat one occurence of the delimiter */
            nextToken = endToken + strlen(delim);
        }
    }
    buf->start = nextToken;
    return token;
}


PUBLIC void httpCreateHeaders1(HttpQueue *q, HttpPacket *packet)
{
    Http        *http;
    HttpStream  *stream;
    HttpTx      *tx;
    HttpUri     *parsedUri;
    MprKey      *kp;
    MprBuf      *buf;

    stream = q->stream;
    http = stream->http;
    tx = stream->tx;
    buf = packet->content;

    tx->responded = 1;

    if (tx->chunkSize <= 0 && q->count > 0 && tx->length < 0) {
        /* No content length and there appears to be output data -- must close connection to signify EOF */
        stream->keepAliveCount = 0;
    }
    if ((tx->flags & HTTP_TX_USE_OWN_HEADERS) && !stream->error) {
        /* Cannot count on content length */
        stream->keepAliveCount = 0;
        return;
    }
    httpPrepareHeaders(stream);

    if (httpServerStream(stream)) {
        mprPutStringToBuf(buf, httpGetProtocol(stream->net));
        mprPutCharToBuf(buf, ' ');
        mprPutIntToBuf(buf, tx->status);
        mprPutCharToBuf(buf, ' ');
        mprPutStringToBuf(buf, httpLookupStatus(tx->status));
        /* Server tracing of status happens in the "complete" event */

    } else {
        mprPutStringToBuf(buf, tx->method);
        mprPutCharToBuf(buf, ' ');
        parsedUri = tx->parsedUri;
        if (http->proxyHost && *http->proxyHost) {
            if (parsedUri->query && *parsedUri->query) {
                mprPutToBuf(buf, "http://%s:%d%s?%s %s", http->proxyHost, http->proxyPort,
                    parsedUri->path, parsedUri->query, httpGetProtocol(stream->net));
            } else {
                mprPutToBuf(buf, "http://%s:%d%s %s", http->proxyHost, http->proxyPort, parsedUri->path,
                    httpGetProtocol(stream->net));
            }
        } else {
            if (parsedUri->query && *parsedUri->query) {
                mprPutToBuf(buf, "%s?%s %s", parsedUri->path, parsedUri->query, httpGetProtocol(stream->net));
            } else {
                mprPutStringToBuf(buf, parsedUri->path);
                mprPutCharToBuf(buf, ' ');
                mprPutStringToBuf(buf, httpGetProtocol(stream->net));
            }
        }
    }
    mprPutStringToBuf(buf, "\r\n");

    if (httpTracing(q->net)) {
        httpLog(stream->trace, "http.tx.headers", "headers", "\n%s", httpTraceHeaders(q, stream->tx->headers));
    }
    /*
        Output headers
     */
    kp = mprGetFirstKey(stream->tx->headers);
    while (kp) {
        mprPutStringToBuf(packet->content, kp->key);
        mprPutStringToBuf(packet->content, ": ");
        if (kp->data) {
            mprPutStringToBuf(packet->content, kp->data);
        }
        mprPutStringToBuf(packet->content, "\r\n");
        kp = mprGetNextKey(stream->tx->headers, kp);
    }
    /*
        By omitting the "\r\n" delimiter after the headers, chunks can emit "\r\nSize\r\n" as a single chunk delimiter
     */
    if (tx->chunkSize <= 0) {
        mprPutStringToBuf(buf, "\r\n");
    }
    tx->headerSize = mprGetBufLength(buf);
    tx->flags |= HTTP_TX_HEADERS_CREATED;
}


static HttpStream *findStream(HttpQueue *q)
{
    HttpStream  *stream;

    if (!q->stream) {
        if ((stream = httpCreateStream(q->net, 1)) == 0) {
            /* Memory error - centrally reported */
            return 0;
        }
        q->stream = stream;
        q->pair->stream = stream;
    } else {
        stream = q->stream;
    }
    return stream;
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
