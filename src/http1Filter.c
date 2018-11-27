/*
    http1Filter.c - HTTP/1 protocol handling.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static HttpConn *findConn(HttpQueue *q);
static char *getToken(HttpPacket *packet, cchar *delim);
static bool gotHeaders(HttpQueue *q, HttpPacket *packet);
static void incomingHttp1(HttpQueue *q, HttpPacket *packet);
static bool monitorActiveRequests(HttpConn *conn);
static void outgoingHttp1(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp1Service(HttpQueue *q);
static HttpPacket *parseFields(HttpQueue *q, HttpPacket *packet);
static HttpPacket *parseHeaders(HttpQueue *q, HttpPacket *packet);
static void parseRequestLine(HttpQueue *q, HttpPacket *packet);
static void parseResponseLine(HttpQueue *q, HttpPacket *packet);
static void tracePacket(HttpQueue *q, HttpPacket *packet);

/*********************************** Code *************************************/
/*
   Loadable module initialization
 */
PUBLIC int httpOpenHttp1Filter()
{
    HttpStage     *filter;

    if ((filter = httpCreateConnector("Http1Filter", NULL)) == 0) {
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
    HttpConn    *conn;

    conn = findConn(q);

    /*
        There will typically be no packets on the queue, so this will be fast
     */
    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);

    for (packet = httpGetPacket(q); packet && !conn->error; packet = httpGetPacket(q)) {
        if (httpTracing(q->net)) {
            httpTracePacket(q->net->trace, "http1.rx", "packet", 0, packet, NULL);
        }
        if (conn->state < HTTP_STATE_PARSED) {
            if ((packet = parseHeaders(q, packet)) != 0) {
                if (conn->state < HTTP_STATE_PARSED) {
                    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
                    break;
                }
            }
            httpProcess(conn->inputq);
        }
        if (packet) {
            httpPutPacket(conn->inputq, packet);
        }
        httpProcess(conn->inputq);
    }
}


static void outgoingHttp1(HttpQueue *q, HttpPacket *packet)
{
    httpPutForService(q, packet, 1);
}


static void outgoingHttp1Service(HttpQueue *q)
{
    HttpPacket  *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        tracePacket(q, packet);
        httpPutPacket(q->net->socketq, packet);
    }
}


static void tracePacket(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    cchar       *type;


    net = q->net;
    type = (packet->type & HTTP_PACKET_HEADER) ? "headers" : "data";
    if (httpTracing(net) && !net->skipTrace) {
        if (net->bytesWritten >= net->trace->maxContent) {
            httpTrace(net->trace, "http1.tx", "packet", "msg: 'Abbreviating packet trace'");
            net->skipTrace = 1;
        } else {
            httpTracePacket(net->trace, "http1.tx", "packet", HTTP_TRACE_HEX, packet, "type=%s, length=%zd,", type, httpGetPacketLength(packet));
        }
    } else {
        httpTrace(net->trace, "http1.tx", "packet", "type=%s, length=%zd,", type, httpGetPacketLength(packet));
    }
}


static HttpPacket *parseHeaders(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;
    HttpLimits  *limits;

    conn = q->conn;
    net = conn->net;
    assert(conn->rx);
    assert(conn->tx);
    rx = conn->rx;
    limits = conn->limits;

    if (!monitorActiveRequests(conn)) {
        return 0;
    }
    if (!gotHeaders(q, packet)) {
        /* Don't yet have a complete header */
        return packet;
    }
    rx->headerPacket = packet;

    if (httpServerConn(conn)) {
        parseRequestLine(q, packet);
    } else {
        parseResponseLine(q, packet);
    }
    return parseFields(q, packet);
}


static bool monitorActiveRequests(HttpConn *conn)
{
    HttpLimits  *limits;
    int64       value;

    limits = conn->limits;

    //  TODO - find a better place for this?  Where does http2 do this
    if (httpServerConn(conn) && !conn->activeRequest) {
        /*
            ErrorDocuments may come through here twice so test activeRequest to keep counters valid.
         */
        conn->activeRequest = 1;
        if ((value = httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, 1)) >= limits->requestsPerClientMax) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_SERVICE_UNAVAILABLE,
                "Too many concurrent requests for client: %s %d/%d", conn->ip, (int) value, limits->requestsPerClientMax);
            return 0;
        }
        httpMonitorEvent(conn, HTTP_COUNTER_REQUESTS, 1);
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
    HttpConn    *conn;
    HttpLimits  *limits;
    cchar       *end, *start;
    ssize       len;

    conn = q->conn;
    limits = conn->limits;
    start = eatBlankLines(packet);
    len = httpGetPacketLength(packet);

    if ((end = sncontains(start, "\r\n\r\n", len)) != 0 || (end = sncontains(start, "\n\n", len)) != 0) {
        len = end - start;
    }
    if (len >= limits->headerSize || len >= q->max) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
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
    HttpConn    *conn;
    HttpRx      *rx;
    HttpLimits  *limits;
    MprBuf      *content;
    char        *method, *uri, *protocol, *start;
    ssize       len;

    conn = q->conn;
    rx = conn->rx;
    limits = conn->limits;

    content = packet->content;
    start = content->start;

    method = getToken(packet, NULL);
    rx->originalMethod = rx->method = supper(method);
    httpParseMethod(conn);

    uri = getToken(packet, NULL);
    len = slen(uri);
    if (*uri == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
        return;
    } else if (len >= limits->uriSize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
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
        conn->net->protocol = 0;
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return;
    } else {
        conn->net->protocol = 1;
    }
    httpSetState(conn, HTTP_STATE_FIRST);
}


/*
    Parse the first line of a http response. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Response status lines look like: HTTP/1.X CODE Message
 */
static void parseResponseLine(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    char        *protocol, *status;
    ssize       len;

    net = q->net;
    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;

    protocol = supper(getToken(packet, NULL));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        net->protocol = 0;
        if (!scaselessmatch(tx->method, "HEAD")) {
            rx->remainingContent = HTTP_UNLIMITED;
        }
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return;
    }
    status = getToken(packet, NULL);
    if (*status == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
        return;
    }
    rx->status = atoi(status);
    rx->statusMessage = sclone(getToken(packet, "\r\n"));

    len = slen(rx->statusMessage);
    if (len >= conn->limits->uriSize) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad response. Status message too long. Length %zd vs limit %d", len, conn->limits->uriSize);
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
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLimits  *limits;
    char        *key, *value;
    int         count, keepAliveHeader;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;

    limits = conn->limits;
    keepAliveHeader = 0;

    for (count = 0; packet->content->start[0] != '\r' && !conn->error; count++) {
        if (count >= limits->headerMax) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Too many headers");
            return 0;
        }
        if ((key = getToken(packet, ":")) == 0 || *key == '\0') {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        value = getToken(packet, "\r\n");
        while (isspace((uchar) *value)) {
            value++;
        }
        if (strspn(key, "%<>/\\") > 0) {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header key value");
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
    if (smatch(httpGetHeader(conn, "transfer-encoding"), "chunked")) {
        httpInitChunking(conn);
    } else {
        mprAdjustBufStart(packet->content, 2);
    }
    httpSetState(conn, HTTP_STATE_PARSED);
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
    HttpConn    *conn;
    HttpTx      *tx;
    HttpUri     *parsedUri;
    MprKey      *kp;
    MprBuf      *buf;

    conn = q->conn;
    http = conn->http;
    tx = conn->tx;
    buf = packet->content;

    tx->responded = 1;

    if (tx->chunkSize <= 0 && q->count > 0 && tx->length < 0) {
        /* No content length and there appears to be output data -- must close connection to signify EOF */
        conn->keepAliveCount = 0;
    }
    if ((tx->flags & HTTP_TX_USE_OWN_HEADERS) && !conn->error) {
        /* Cannot count on content length */
        conn->keepAliveCount = 0;
        return;
    }
    httpPrepareHeaders(conn);

    if (httpServerConn(conn)) {
        mprPutStringToBuf(buf, httpGetProtocol(conn->net));
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
                    parsedUri->path, parsedUri->query, httpGetProtocol(conn->net));
            } else {
                mprPutToBuf(buf, "http://%s:%d%s %s", http->proxyHost, http->proxyPort, parsedUri->path,
                    httpGetProtocol(conn->net));
            }
        } else {
            if (parsedUri->query && *parsedUri->query) {
                mprPutToBuf(buf, "%s?%s %s", parsedUri->path, parsedUri->query, httpGetProtocol(conn->net));
            } else {
                mprPutStringToBuf(buf, parsedUri->path);
                mprPutCharToBuf(buf, ' ');
                mprPutStringToBuf(buf, httpGetProtocol(conn->net));
            }
        }
    }
    mprPutStringToBuf(buf, "\r\n");

    /*
        Output headers
     */
    kp = mprGetFirstKey(conn->tx->headers);
    while (kp) {
        mprPutStringToBuf(packet->content, kp->key);
        mprPutStringToBuf(packet->content, ": ");
        if (kp->data) {
            mprPutStringToBuf(packet->content, kp->data);
        }
        mprPutStringToBuf(packet->content, "\r\n");
        kp = mprGetNextKey(conn->tx->headers, kp);
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


static HttpConn *findConn(HttpQueue *q)
{
    HttpConn    *conn;

    if (!q->conn) {
        if ((conn = httpCreateConn(q->net)) == 0) {
            /* Memory error - centrally reported */
            return 0;
        }
        q->conn = conn;
    } else {
        conn = q->conn;
    }
    return conn;
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
