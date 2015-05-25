/*
    rx.c -- Http receiver. Parses http requests and client responses.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void addMatchEtag(HttpConn *conn, char *etag);
static void delayAwake(HttpConn *conn, MprEvent *event);
static char *getToken(HttpConn *conn, cchar *delim);
static bool getOutput(HttpConn *conn);
static void manageRange(HttpRange *range, int flags);
static void manageRx(HttpRx *rx, int flags);
static bool parseHeaders(HttpConn *conn, HttpPacket *packet);
static bool parseIncoming(HttpConn *conn);
static bool parseRange(HttpConn *conn, char *value);
static bool parseRequestLine(HttpConn *conn, HttpPacket *packet);
static bool parseResponseLine(HttpConn *conn, HttpPacket *packet);
static void parseUri(HttpConn *conn);
static bool processCompletion(HttpConn *conn);
static bool processFinalized(HttpConn *conn);
static bool processContent(HttpConn *conn);
static void parseMethod(HttpConn *conn);
static bool processParsed(HttpConn *conn);
static bool processReady(HttpConn *conn);
static bool processRunning(HttpConn *conn);
static int sendContinue(HttpConn *conn);

/*********************************** Code *************************************/

PUBLIC HttpRx *httpCreateRx(HttpConn *conn)
{
    HttpRx      *rx;

    if ((rx = mprAllocObj(HttpRx, manageRx)) == 0) {
        return 0;
    }
    rx->conn = conn;
    rx->length = -1;
    rx->ifMatch = 1;
    rx->ifModified = 1;
    rx->pathInfo = sclone("/");
    rx->scriptName = mprEmptyString();
    rx->needInputPipeline = httpClientConn(conn);
    rx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
    rx->chunkState = HTTP_CHUNK_UNCHUNKED;
    rx->seqno = ++conn->totalRequests;
    return rx;
}


static void manageRx(HttpRx *rx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(rx->accept);
        mprMark(rx->acceptCharset);
        mprMark(rx->acceptEncoding);
        mprMark(rx->acceptLanguage);
        mprMark(rx->authDetails);
        mprMark(rx->conn);
        mprMark(rx->connection);
        mprMark(rx->contentLength);
        mprMark(rx->cookie);
        mprMark(rx->etags);
        mprMark(rx->extraPath);
        mprMark(rx->files);
        mprMark(rx->headerPacket);
        mprMark(rx->headers);
        mprMark(rx->hostHeader);
        mprMark(rx->inputPipeline);
        mprMark(rx->inputRange);
        mprMark(rx->lang);
        mprMark(rx->method);
        mprMark(rx->mimeType);
        mprMark(rx->origin);
        mprMark(rx->originalMethod);
        mprMark(rx->originalUri);
        mprMark(rx->paramString);
        mprMark(rx->params);
        mprMark(rx->parsedUri);
        mprMark(rx->passwordDigest);
        mprMark(rx->pathInfo);
        mprMark(rx->pragma);
        mprMark(rx->redirect);
        mprMark(rx->referrer);
        mprMark(rx->requestData);
        mprMark(rx->route);
        mprMark(rx->scriptName);
        mprMark(rx->securityToken);
        mprMark(rx->session);
        mprMark(rx->statusMessage);
        mprMark(rx->svars);
        mprMark(rx->target);
        mprMark(rx->upgrade);
        mprMark(rx->uri);
        mprMark(rx->userAgent);
        mprMark(rx->webSocket);
    }
}


PUBLIC void httpDestroyRx(HttpRx *rx)
{
    if (rx->conn) {
        rx->conn->rx = 0;
        rx->conn = 0;
    }
}


/*
    HTTP Protocol state machine for server-side requests and client responses.
    Process an incoming request and drive the state machine. This will process only one request.
    All socket I/O is non-blocking, and this routine must not block. Note: packet may be null.
    Return true if the request is completed successfully.

    MUST only ever be called from httpIOEvent otherwise recursion plays havoc.
 */
PUBLIC void httpProtocol(HttpConn *conn)
{
    bool    canProceed;

    assert(conn);

    conn->lastActivity = conn->http->now;

    do {
        switch (conn->state) {
        case HTTP_STATE_BEGIN:
        case HTTP_STATE_CONNECTED:
            canProceed = parseIncoming(conn);
            break;

        case HTTP_STATE_PARSED:
            canProceed = processParsed(conn);
            break;

        case HTTP_STATE_CONTENT:
            canProceed = processContent(conn);
            break;

        case HTTP_STATE_READY:
            canProceed = processReady(conn);
            break;

        case HTTP_STATE_RUNNING:
            canProceed = processRunning(conn);
            break;

        case HTTP_STATE_FINALIZED:
            canProceed = processFinalized(conn);
            break;

        case HTTP_STATE_COMPLETE:
            canProceed = processCompletion(conn);
            break;

        default:
            canProceed = 0;
            break;
        }
        /*
            This may block briefly if GC is due
         */
        httpServiceQueues(conn, HTTP_BLOCK);

        /*
            This is the primary top-level GC yield for the http engine
         */
        if (mprNeedYield()) {
            mprYield(0);
        }
    } while (canProceed);
}


/*
    Parse the incoming http message. Return true to keep going with this or subsequent request, zero means
    insufficient data to proceed.
 */
static bool parseIncoming(HttpConn *conn)
{
    HttpRx      *rx;
    HttpAddress *address;
    HttpPacket  *packet;
    HttpLimits  *limits;
    ssize       len;
    int64       value;
    char        *start, *end;

    if ((packet = conn->input) == 0) {
        return 0;
    }
    if (mprShouldDenyNewRequests()) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "The server is terminating");
        return 0;
    }
    assert(conn->rx);
    assert(conn->tx);
    rx = conn->rx;
    limits = conn->limits;

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

    if ((len = httpGetPacketLength(packet)) == 0) {
        return 0;
    }
    start = mprGetBufStart(packet->content);
    while (*start == '\r' || *start == '\n') {
        mprGetCharFromBuf(packet->content);
        start = mprGetBufStart(packet->content);
    }
    /*
        Don't start processing until all the headers have been received (delimited by two blank lines)
     */
    if ((end = sncontains(start, "\r\n\r\n", len)) == 0 && (end = sncontains(start, "\n\n", len)) == 0) {
        if (len >= limits->headerSize) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Header too big. Length %zd vs limit %zd", len, limits->headerSize);
        }
        return 0;
    }
    rx->headerPacketLength = len = end - start;

    if (len >= limits->headerSize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, "Header too big. Length %zd vs limit %zd", len,
            limits->headerSize);
        return 0;
    }
    if (httpServerConn(conn)) {
        /* This will set conn->error if it does not validate - keep going to generate a response */
        if (!parseRequestLine(conn, packet)) {
            return 0;
        }
    } else if (!parseResponseLine(conn, packet)) {
        return 0;
    }
    if (!parseHeaders(conn, packet)) {
        return 0;
    }
    if (httpServerConn(conn)) {
        httpMatchHost(conn);
        parseUri(conn);

    } else if (rx->status != HTTP_CODE_CONTINUE) {
        /*
            Ignore Expect status responses. NOTE: Clients have already created their Tx pipeline.
         */
        httpCreateRxPipeline(conn, conn->http->clientRoute);
    }
    if (rx->flags & HTTP_EXPECT_CONTINUE) {
        sendContinue(conn);
        rx->flags &= ~HTTP_EXPECT_CONTINUE;
    }
    httpSetState(conn, HTTP_STATE_PARSED);

    if ((address = conn->address) != 0) {
        if (address->delay) {
            if (address->delayUntil > conn->http->now) {
                /*
                    Defensive counter measure - go slow
                 */
                mprCreateEvent(conn->dispatcher, "delayConn", conn->delay, delayAwake, conn, 0);
                return 0;
            } else {
                address->delay = 0;
                httpTrace(conn, "monitor.delay.stop", "context", "client:'%s'", conn->ip);
            }
        }
    }
    return 1;
}


/*
    Defensive countermesasure - resume output after a delay
 */
static void delayAwake(HttpConn *conn, MprEvent *event)
{
    conn->delay = 0;
    mprCreateEvent(conn->dispatcher, "resumeConn", 0, httpIOEvent, conn, 0);
}


static bool mapMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;

    rx = conn->rx;
    if (rx->flags & HTTP_POST && (method = httpGetParam(conn, "-http-method-", 0)) != 0) {
        if (!scaselessmatch(method, rx->method)) {
            httpTrace(conn, "request.method", "context", "originalMethod:'%s',method:'%s'", rx->method, method);
            httpSetMethod(conn, method);
            return 1;
        }
    }
    return 0;
}


static void parseMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;
    int         methodFlags;

    rx = conn->rx;
    method = rx->method;
    methodFlags = 0;

    switch (method[0]) {
    case 'D':
        if (strcmp(method, "DELETE") == 0) {
            methodFlags = HTTP_DELETE;
        }
        break;

    case 'G':
        if (strcmp(method, "GET") == 0) {
            methodFlags = HTTP_GET;
        }
        break;

    case 'H':
        if (strcmp(method, "HEAD") == 0) {
            methodFlags = HTTP_HEAD;
        }
        break;

    case 'O':
        if (strcmp(method, "OPTIONS") == 0) {
            methodFlags = HTTP_OPTIONS;
        }
        break;

    case 'P':
        if (strcmp(method, "POST") == 0) {
            methodFlags = HTTP_POST;
            rx->needInputPipeline = 1;

        } else if (strcmp(method, "PUT") == 0) {
            methodFlags = HTTP_PUT;
            rx->needInputPipeline = 1;
        }
        break;

    case 'T':
        if (strcmp(method, "TRACE") == 0) {
            methodFlags = HTTP_TRACE;
        }
        break;
    }
    rx->flags |= methodFlags;
}


/*
    Parse the first line of a http request. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Requests look like: METHOD URL HTTP/1.X.
 */
static bool parseRequestLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpLimits  *limits;
    char        *headers, *method, *uri, *protocol, *start;
    MprBuf      *content;
    ssize       len;

    rx = conn->rx;
    limits = conn->limits;

    /*
        These are initially set when the connection is accepted via httpAddConn. Revise to mark a new request.
     */
    conn->startMark = mprGetHiResTicks();
    conn->started = conn->http->now;

    content = packet->content;
    start = content->start;
    headers = httpTracing(conn) ? snclone(start, rx->headerPacketLength) : 0;

    method = getToken(conn, 0);
    rx->originalMethod = rx->method = supper(method);
    parseMethod(conn);

    uri = getToken(conn, 0);
    len = slen(uri);
    if (*uri == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
        return 0;
    } else if (len >= limits->uriSize) {
        httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad request. URI too long. Length %zd vs limit %zd", len, limits->uriSize);
        return 0;
    }
    protocol = getToken(conn, "\r\n");
    conn->protocol = supper(protocol);
    if (smatch(conn->protocol, "HTTP/1.0") || *conn->protocol == 0) {
        if (rx->flags & (HTTP_POST|HTTP_PUT)) {
            rx->remainingContent = HTTP_UNLIMITED;
            rx->needInputPipeline = 1;
        }
        conn->http10 = 1;
        conn->mustClose = 1;
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        conn->protocol = sclone("HTTP/1.0");
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    rx->uri = sclone(uri);
    if (!rx->originalUri) {
        rx->originalUri = rx->uri;
    }
    conn->http->totalRequests++;
    httpSetState(conn, HTTP_STATE_FIRST);

    if (httpTracing(conn)) {
        httpTrace(conn, "rx.first.server", "request", "method:'%s',uri:'%s',protocol:'%s'", 
            rx->method, rx->uri, conn->protocol);
        httpTraceContent(conn, "rx.headers.server", "context", headers, rx->headerPacketLength, NULL);
    }
    return 1;
}


/*
    Parse the first line of a http response. Return true if the first line parsed. This is only called once all the headers
    have been read and buffered. Response status lines look like: HTTP/1.X CODE Message
 */
static bool parseResponseLine(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprBuf      *content;
    cchar       *endp;
    char        *protocol, *status;
    ssize       len;

    rx = conn->rx;
    tx = conn->tx;

    protocol = conn->protocol = supper(getToken(conn, 0));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        conn->http10 = 1;
        if (!scaselessmatch(tx->method, "HEAD")) {
            rx->remainingContent = HTTP_UNLIMITED;
        }
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    status = getToken(conn, 0);
    if (*status == '\0') {
        httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
        return 0;
    }
    rx->status = atoi(status);
    rx->statusMessage = sclone(getToken(conn, "\r\n"));

    len = slen(rx->statusMessage);
    if (len >= conn->limits->uriSize) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_URL_TOO_LARGE,
            "Bad response. Status message too long. Length %zd vs limit %zd", len, conn->limits->uriSize);
        return 0;
    }
    if (httpTracing(conn)) {
        httpTrace(conn, "rx.first.client", "request", "status:%d,protocol:'%s'", rx->status, protocol);
        content = packet->content;
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (int) (endp - content->start + 4) : 0;
        httpTraceContent(conn, "rx.headers.client", "context", content->start, len, NULL);
    }
    if (rx->status == HTTP_CODE_CONTINUE) {
        /* Eat the blank line and wait for the real response */
        mprAdjustBufStart(packet->content, 2);
        return 0;
    }
    return 1;
}


/*
    Parse the request headers. Return true if the header parsed.
 */
static bool parseHeaders(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLimits  *limits;
    MprBuf      *content;
    char        *cp, *key, *value, *tok, *hvalue;
    cchar       *oldValue;
    int         count, keepAliveHeader;

    rx = conn->rx;
    tx = conn->tx;
    rx->headerPacket = packet;
    content = packet->content;
    limits = conn->limits;
    keepAliveHeader = 0;

    for (count = 0; content->start[0] != '\r' && !conn->error; count++) {
        if (count >= limits->headerMax) {
            httpLimitError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Too many headers");
            return 0;
        }
        if ((key = getToken(conn, ":")) == 0 || *key == '\0') {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        value = getToken(conn, "\r\n");
        while (isspace((uchar) *value)) {
            value++;
        }
        if (strspn(key, "%<>/\\") > 0) {
            httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header key value");
            return 0;
        }
        if ((oldValue = mprLookupKey(rx->headers, key)) != 0) {
            hvalue = sfmt("%s, %s", oldValue, value);
        } else {
            hvalue = sclone(value);
        }
        mprAddKey(rx->headers, key, hvalue);

        switch (tolower((uchar) key[0])) {
        case 'a':
            if (strcasecmp(key, "authorization") == 0) {
                value = sclone(value);
                conn->authType = slower(stok(value, " \t", &tok));
                rx->authDetails = sclone(tok);

            } else if (strcasecmp(key, "accept-charset") == 0) {
                rx->acceptCharset = sclone(value);

            } else if (strcasecmp(key, "accept") == 0) {
                rx->accept = sclone(value);

            } else if (strcasecmp(key, "accept-encoding") == 0) {
                rx->acceptEncoding = sclone(value);

            } else if (strcasecmp(key, "accept-language") == 0) {
                rx->acceptLanguage = sclone(value);
            }
            break;

        case 'c':
            if (strcasecmp(key, "connection") == 0) {
                rx->connection = sclone(value);
                if (scaselesscmp(value, "KEEP-ALIVE") == 0) {
                    keepAliveHeader = 1;

                } else if (scaselesscmp(value, "CLOSE") == 0) {
                    conn->keepAliveCount = 0;
                    conn->mustClose = 1;
                }

            } else if (strcasecmp(key, "content-length") == 0) {
                if (rx->length >= 0) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Mulitple content length headers");
                    break;
                }
                rx->length = stoi(value);
                if (rx->length < 0) {
                    httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad content length");
                    return 0;
                }
                rx->contentLength = sclone(value);
                assert(rx->length >= 0);
                if (httpServerConn(conn) || !scaselessmatch(tx->method, "HEAD")) {
                    rx->remainingContent = rx->length;
                    rx->needInputPipeline = 1;
                }

            } else if (strcasecmp(key, "content-range") == 0) {
                /*
                    The Content-Range header is used in the response. The Range header is used in the request.
                    This headers specifies the range of any posted body data
                    Format is:  Content-Range: bytes n1-n2/length
                    Where n1 is first byte pos and n2 is last byte pos
                 */
                char    *sp;
                MprOff  start, end, size;

                start = end = size = -1;
                sp = value;
                while (*sp && !isdigit((uchar) *sp)) {
                    sp++;
                }
                if (*sp) {
                    start = stoi(sp);
                    if ((sp = strchr(sp, '-')) != 0) {
                        end = stoi(++sp);
                        if ((sp = strchr(sp, '/')) != 0) {
                            /*
                                Note this is not the content length transmitted, but the original size of the input of which
                                the client is transmitting only a portion.
                             */
                            size = stoi(++sp);
                        }
                    }
                }
                if (start < 0 || end < 0 || size < 0 || end < start) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad content range");
                    break;
                }
                rx->inputRange = httpCreateRange(conn, start, end);

            } else if (strcasecmp(key, "content-type") == 0) {
                rx->mimeType = sclone(value);
                if (rx->flags & (HTTP_POST | HTTP_PUT)) {
                    if (httpServerConn(conn)) {
                        rx->form = scontains(rx->mimeType, "application/x-www-form-urlencoded") != 0;
                        rx->upload = scontains(rx->mimeType, "multipart/form-data") != 0;
                    }
                } else {
                    rx->form = rx->upload = 0;
                }
            } else if (strcasecmp(key, "cookie") == 0) {
                if (rx->cookie && *rx->cookie) {
                    rx->cookie = sjoin(rx->cookie, "; ", value, NULL);
                } else {
                    rx->cookie = sclone(value);
                }
            }
            break;

        case 'e':
            if (strcasecmp(key, "expect") == 0) {
                /*
                    Handle 100-continue for HTTP/1.1 clients only. This is the only expectation that is currently supported.
                 */
                if (!conn->http10) {
                    if (strcasecmp(value, "100-continue") != 0) {
                        httpBadRequestError(conn, HTTP_CODE_EXPECTATION_FAILED, "Expect header value is not supported");
                    } else {
                        rx->flags |= HTTP_EXPECT_CONTINUE;
                    }
                }
            }
            break;

        case 'h':
            if (strcasecmp(key, "host") == 0) {
                if ((int) strspn(value, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.[]:") 
                        < (int) slen(value)) {
                    httpBadRequestError(conn, HTTP_CODE_BAD_REQUEST, "Bad host header");
                } else {
                    rx->hostHeader = sclone(value);
                }
            }
            break;

        case 'i':
            if ((strcasecmp(key, "if-modified-since") == 0) || (strcasecmp(key, "if-unmodified-since") == 0)) {
                MprTime     newDate = 0;
                char        *cp;
                bool        ifModified = (tolower((uchar) key[3]) == 'm');

                if ((cp = strchr(value, ';')) != 0) {
                    *cp = '\0';
                }
                if (mprParseTime(&newDate, value, MPR_UTC_TIMEZONE, NULL) < 0) {
                    assert(0);
                    break;
                }
                if (newDate) {
                    rx->since = newDate;
                    rx->ifModified = ifModified;
                    rx->flags |= HTTP_IF_MODIFIED;
                }

            } else if ((strcasecmp(key, "if-match") == 0) || (strcasecmp(key, "if-none-match") == 0)) {
                char    *word, *tok;
                bool    ifMatch = (tolower((uchar) key[3]) == 'm');

                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = ifMatch;
                rx->flags |= HTTP_IF_MODIFIED;
                value = sclone(value);
                word = stok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = stok(0, " ,", &tok);
                }

            } else if (strcasecmp(key, "if-range") == 0) {
                char    *word, *tok;
                if ((tok = strchr(value, ';')) != 0) {
                    *tok = '\0';
                }
                rx->ifMatch = 1;
                rx->flags |= HTTP_IF_MODIFIED;
                value = sclone(value);
                word = stok(value, " ,", &tok);
                while (word) {
                    addMatchEtag(conn, word);
                    word = stok(0, " ,", &tok);
                }
            }
            break;

        case 'k':
            /* Keep-Alive: timeout=N, max=1 */
            if (strcasecmp(key, "keep-alive") == 0) {
                if ((tok = scontains(value, "max=")) != 0) {
                    conn->keepAliveCount = atoi(&tok[4]);
                    if (conn->keepAliveCount < 0) {
                        conn->keepAliveCount = 0;
                    }
                    if (conn->keepAliveCount > ME_MAX_KEEP_ALIVE) {
                        conn->keepAliveCount = ME_MAX_KEEP_ALIVE;
                    }
                    /*
                        IMPORTANT: Deliberately close client connections one request early. This encourages a client-led
                        termination and may help relieve excessive server-side TIME_WAIT conditions.
                     */
                    if (httpClientConn(conn) && conn->keepAliveCount == 1) {
                        conn->keepAliveCount = 0;
                    }
                }
            }
            break;

        case 'l':
            if (strcasecmp(key, "location") == 0) {
                rx->redirect = sclone(value);
            }
            break;

        case 'o':
            if (strcasecmp(key, "origin") == 0) {
                rx->origin = sclone(value);
            }
            break;

        case 'p':
            if (strcasecmp(key, "pragma") == 0) {
                rx->pragma = sclone(value);
            }
            break;

        case 'r':
            if (strcasecmp(key, "range") == 0) {
                /*
                    The Content-Range header is used in the response. The Range header is used in the request.
                 */
                if (!parseRange(conn, value)) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad range");
                }
            } else if (strcasecmp(key, "referer") == 0) {
                /* NOTE: yes the header is misspelt in the spec */
                rx->referrer = sclone(value);
            }
            break;

        case 't':
            if (strcasecmp(key, "transfer-encoding") == 0) {
                if (scaselesscmp(value, "chunked") == 0 && !conn->http10) {
                    /*
                        remainingContent will be revised by the chunk filter as chunks are processed and will
                        be set to zero when the last chunk has been received.
                     */
                    rx->flags |= HTTP_CHUNKED;
                    rx->chunkState = HTTP_CHUNK_START;
                    rx->remainingContent = HTTP_UNLIMITED;
                    rx->needInputPipeline = 1;
                }
            }
            break;

        case 'x':
            if (strcasecmp(key, "x-http-method-override") == 0) {
                httpSetMethod(conn, value);

            } else if (strcasecmp(key, "x-own-params") == 0) {
                /*
                    Optimize and don't convert query and body content into params.
                    This is for those who want very large forms and to do their own custom handling.
                 */
                rx->ownParams = 1;
#if ME_DEBUG
            } else if (strcasecmp(key, "x-chunk-size") == 0) {
                tx->chunkSize = atoi(value);
                if (tx->chunkSize <= 0) {
                    tx->chunkSize = 0;
                } else if (tx->chunkSize > conn->limits->chunkSize) {
                    tx->chunkSize = conn->limits->chunkSize;
                }
#endif
            }
            break;

        case 'u':
            if (scaselesscmp(key, "upgrade") == 0) {
                rx->upgrade = sclone(value);
            } else if (strcasecmp(key, "user-agent") == 0) {
                rx->userAgent = sclone(value);
            }
            break;

        case 'w':
            if (strcasecmp(key, "www-authenticate") == 0) {
                cp = value;
                while (*value && !isspace((uchar) *value)) {
                    value++;
                }
                *value++ = '\0';
                conn->authType = slower(cp);
                rx->authDetails = sclone(value);
            }
            break;
        }
    }
    if (rx->form && rx->length >= conn->limits->rxFormSize && conn->limits->rxFormSize != HTTP_UNLIMITED) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
            "Request form of %lld bytes is too big. Limit %lld", rx->length, conn->limits->rxFormSize);
    }
    if (conn->error) {
        /* Cannot continue with keep-alive as the headers have not been correctly parsed */
        conn->keepAliveCount = 0;
        conn->connError = 1;
    }
    if (conn->http10 && !keepAliveHeader) {
        conn->keepAliveCount = 0;
    }
    if (httpClientConn(conn) && conn->mustClose && rx->length < 0) {
        /*
            Google does responses with a body and without a Content-Lenght like this:
                Connection: close
                Location: URI
         */
        rx->remainingContent = rx->redirect ? 0 : HTTP_UNLIMITED;
    }
    if (!(rx->flags & HTTP_CHUNKED)) {
        /*
            Step over "\r\n" after headers.
            Don't do this if chunked so chunking can parse a single chunk delimiter of "\r\nSIZE ...\r\n"
         */
        mprAdjustBufStart(content, 2);
    }
    /*
        Split the headers and retain the data in conn->input. Revise lastRead to the number of data bytes available.
     */
    conn->input = httpSplitPacket(packet, 0);
    conn->lastRead = httpGetPacketLength(conn->input);
    return 1;
}


/*
    Called once the HTTP request/response headers have been parsed
 */
static bool processParsed(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;

    rx = conn->rx;
    tx = conn->tx;

    if (httpServerConn(conn)) {
        httpAddQueryParams(conn);
        rx->streaming = httpGetStreaming(conn->host, rx->mimeType, rx->uri);
        if (rx->streaming) {
            httpRouteRequest(conn);
        }
        /*
            Delay testing rxBodySize till after routing for streaming requests. This way, recieveBodySize
            can be defined per route.
         */
        if (!rx->upload && rx->length >= conn->limits->rxBodySize && conn->limits->rxBodySize != HTTP_UNLIMITED) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Request content length %lld bytes is too big. Limit %lld", rx->length, conn->limits->rxBodySize);
            return 0;
        }
        if (rx->streaming) {
            httpCreatePipeline(conn);
            /*
                Delay starting uploads until the files are extracted.
             */
            if (!rx->upload) {
                httpStartPipeline(conn);
            }
        }
#if ME_HTTP_WEB_SOCKETS
    } else {
        if (conn->upgraded && !httpVerifyWebSocketsHandshake(conn)) {
            httpSetState(conn, HTTP_STATE_FINALIZED);
            return 1;
        }
#endif
    }
    httpSetState(conn, HTTP_STATE_CONTENT);
    if (rx->remainingContent == 0) {
        httpSetEof(conn);
    }
    if (rx->eof && tx->started) {
        q = tx->queue[HTTP_QUEUE_RX];
        httpPutPacketToNext(q, httpCreateEndPacket());
        httpSetState(conn, HTTP_STATE_READY);
    }
    return 1;
}


/*
    Filter the packet data and determine the number of useful bytes in the packet.
    The packet may be split if it contains chunk data for the next chunk.
    Set *more to true if there is more useful (non-chunk header) data to be processed.
    Packet may be null.
 */
static ssize filterPacket(HttpConn *conn, HttpPacket *packet, int *more)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprOff      size;
    HttpLimits  *limits;
    ssize       nbytes;

    rx = conn->rx;
    tx = conn->tx;
    limits = conn->limits;
    *more = 0;

    if (mprIsSocketEof(conn->sock) || conn->connError) {
        httpSetEof(conn);
    }
    if (rx->chunkState) {
        nbytes = httpFilterChunkData(tx->queue[HTTP_QUEUE_RX], packet);
        if (rx->chunkState == HTTP_CHUNK_EOF) {
            httpSetEof(conn);
            assert(rx->remainingContent == 0);
        }
    } else {
        nbytes = (ssize) min(rx->remainingContent, conn->lastRead);
        if (!conn->upgraded && (rx->remainingContent - nbytes) <= 0) {
            httpSetEof(conn);
        }
    }
    conn->lastRead = 0;

    assert(nbytes >= 0);
    rx->bytesRead += nbytes;
    if (!conn->upgraded) {
        if (rx->remainingContent != HTTP_UNLIMITED) {
            rx->remainingContent -= nbytes;
        }
        assert(rx->remainingContent >= 0);
    }

    if (limits->rxBodySize < HTTP_UNLIMITED) {
        /*
            Enforce sandbox limits
         */
        size = rx->bytesRead - rx->bytesUploaded;
        if (size >= limits->rxBodySize) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Receive body of %lld bytes (sofar) is too big. Limit %lld", size, limits->rxBodySize);

        } else if (rx->form && size >= limits->rxFormSize) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Receive form of %lld bytes (sofar) is too big. Limit %lld", size, limits->rxFormSize);
        }
    }
    if (packet && httpTracing(conn)) {
        httpTraceBody(conn, 0, packet, nbytes);
    }
    if (rx->eof) {
#if GITHUB_BUG || 1
        /*
            GitHub is doing a 302 redirection with a "Transfer-Encoding" with a "Connection:close" header without any body
         */
        if (conn->mustClose && (rx->chunkState && rx->chunkState != HTTP_CHUNK_EOF)) {
            rx->chunkState = HTTP_CHUNK_EOF;
        }
#endif
        if ((rx->remainingContent > 0 && (rx->length > 0 || !conn->mustClose)) ||
            (rx->chunkState && rx->chunkState != HTTP_CHUNK_EOF)) {
            /* Closing is the only way for HTTP/1.0 to signify the end of data */
            httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
            return 0;
        }
        if (nbytes > 0 && httpGetPacketLength(packet) > nbytes) {
            conn->input = httpSplitPacket(packet, nbytes);
            *more = 1;
        }
    } else {
        if (rx->chunkState && nbytes > 0 && httpGetPacketLength(packet) > nbytes) {
            /* Split data for next chunk */
            conn->input = httpSplitPacket(packet, nbytes);
            *more = 1;
        }
    }
    return nbytes;
}


static bool processContent(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    HttpPacket  *packet;
    ssize       nbytes;
    int         moreData;

    assert(conn);
    rx = conn->rx;
    tx = conn->tx;

    q = tx->queue[HTTP_QUEUE_RX];
    packet = conn->input;
    /* Packet may be null */

    if ((nbytes = filterPacket(conn, packet, &moreData)) > 0) {
        if (!tx->finalized) {
            if (rx->inputPipeline) {
                httpPutPacketToNext(q, packet);
            } else {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            }
        }
        if (packet == conn->input) {
            conn->input = 0;
        }
    }
    if (rx->eof) {
        if (conn->state < HTTP_STATE_FINALIZED) {
            if (httpServerConn(conn)) {
                if (!rx->route) {
                    if (httpAddBodyParams(conn) < 0) {
                        httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad request parameters");
                    } else {
                        mapMethod(conn);
                    }
                    httpRouteRequest(conn);
                    httpCreatePipeline(conn);
                    /*
                        Transfer buffered input body data into the pipeline
                     */
                    while ((packet = httpGetPacket(q)) != 0) {
                        httpPutPacketToNext(q, packet);
                    }
                }
                httpPutPacketToNext(q, httpCreateEndPacket());
                if (!tx->started) {
                    httpStartPipeline(conn);
                }
            } else {
                httpPutPacketToNext(q, httpCreateEndPacket());
            }
            httpSetState(conn, HTTP_STATE_READY);
        }
        return 1;
    }
    if (tx->started) {
        /*
            Some requests (websockets) remain in the content state while still generating output
         */
        moreData += getOutput(conn);
    }
    return (conn->connError || moreData || mprNeedYield());
}


/*
    In the ready state after all content has been received
 */
static bool processReady(HttpConn *conn)
{
    httpReadyHandler(conn);
    httpSetState(conn, HTTP_STATE_RUNNING);
    if (httpClientConn(conn) && !conn->upgraded) {
        httpFinalize(conn);
    }
    return 1;
}


static bool processRunning(HttpConn *conn)
{
    assert(conn->rx->eof);

    if (conn->tx->finalized && conn->tx->finalizedConnector) {
        httpSetState(conn, HTTP_STATE_FINALIZED);
        return 1;
    }
    if (httpServerConn(conn)) {
        return getOutput(conn) || httpQueuesNeedService(conn) || mprNeedYield();
    }
    return 0;
}


/*
    Get more output by invoking the handler's writable callback. Called by processRunning.
    Also issues an HTTP_EVENT_WRITABLE for application level notification.
 */
static bool getOutput(HttpConn *conn)
{
    HttpQueue   *q;
    HttpTx      *tx;
    ssize       count;

    tx = conn->tx;
    if (tx->started && !tx->writeBlocked) {
        q = conn->writeq;
        count = q->count;
        if (!tx->finalizedOutput) {
            HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
            if (tx->handler->writable) {
                tx->handler->writable(q);
            }
        }
        if (count != q->count) {
            return 1;
        }
    }
    return 0;
}


static void createErrorRequest(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpPacket  *packet;
    MprBuf      *buf;
    char        *cp, *headers, *originalUri;
    int         key;

    rx = conn->rx;
    tx = conn->tx;
    if (!rx->headerPacket) {
        return;
    }
    httpTrace(conn, "request.errordoc", "context", "location:'%s',status:%d", tx->errorDocument, tx->status);

    originalUri = rx->uri;
    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);

    /* Preserve the old status */
    conn->tx->status = tx->status;
    conn->rx->originalUri = originalUri;
    conn->error = 0;
    conn->errorMsg = 0;
    conn->upgraded = 0;
    conn->worker = 0;

    packet = httpCreateDataPacket(ME_MAX_BUFFER);
    mprPutToBuf(packet->content, "%s %s %s\r\n", rx->method, tx->errorDocument, conn->protocol);
    /*
        Sever the old Rx and Tx for GC
     */
    rx->conn = 0;
    tx->conn = 0;

    /*
        Reconstruct the headers. Change nulls to '\r', ' ', or ':' as appropriate
     */
    key = 0;
    headers = 0;
    /*
        Ensure buffer always has a trailing null (one past buf->end)
     */
    buf = rx->headerPacket->content;
    mprAddNullToBuf(buf);
    for (cp = buf->data; cp < &buf->end[-1]; cp++) {
        if (*cp == '\0') {
            if (cp[1] == '\n') {
                if (!headers) {
                    headers = &cp[2];
                }
                *cp = '\r';
                if (&cp[4] <= buf->end && cp[2] == '\r' && cp[3] == '\n') {
                    cp[4] = '\0';
                }
                key = 0;
            } else if (!key) {
                *cp = ':';
                key = 1;
            } else {
                *cp = ' ';
            }
        }
    }
    if (!headers || headers >= buf->end) {
        headers = "\r\n";
    }
    mprPutStringToBuf(packet->content, headers);
    conn->input = packet;
    conn->state = HTTP_STATE_CONNECTED;
}


static bool processFinalized(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;

    rx = conn->rx;
    tx = conn->tx;
    assert(tx->finalized);
    assert(tx->finalizedOutput);
    assert(tx->finalizedConnector);

#if ME_TRACE_MEM
    mprDebug(1, "Request complete, status %d, error %d, connError %d, %s%s, memsize %.2f MB",
        tx->status, conn->error, conn->connError, rx->hostHeader, rx->uri, mprGetMem() / 1024 / 1024.0);
#endif
    httpClosePipeline(conn);

    if (httpServerConn(conn) && rx) {
        httpMonitorEvent(conn, HTTP_COUNTER_NETWORK_IO, tx->bytesWritten);
    }
    httpSetState(conn, HTTP_STATE_COMPLETE);
    if (tx->errorDocument && !conn->connError && !smatch(tx->errorDocument, rx->uri)) {
        createErrorRequest(conn);
    }
    return 1;
}


static bool processCompletion(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTicks    elapsed;
    MprOff      received;
    int         status;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->session) {
        httpWriteSession(conn);
    }
    if (httpServerConn(conn) && conn->activeRequest) {
        httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
        conn->activeRequest = 0;
    }
    elapsed = mprGetTicks() - conn->started;
    if (httpTracing(conn)) {
        status = conn->endpoint ? tx->status : rx->status;
        received = rx->headerPacketLength + rx->bytesRead;
#if MPR_HIGH_RES_TIMER
        httpTrace(conn, 
            "request.completion", "result",
            "status:%d,error:%d,connError:%d,elapsed:%llu,elapsedTicks:%llu,received:%lld,sent:%lld",
            status, conn->error, conn->connError, elapsed, mprGetHiResTicks() - conn->startMark, 
            received, tx->bytesWritten);
#else
        httpTrace(conn, "request.completion", "result", 
            "status:%d,error:%d,connError:%d,elapsed:%llu,received:%lld,sent:%lld",
            status, conn->error, conn->connError, elapsed, received, tx->bytesWritten);
#endif
    }
    if (conn->http->requestCallback) {
        (conn->http->requestCallback)(conn);
    }
    return 0;
}


PUBLIC void httpSetRequestCallback(HttpRequestCallback callback)
{
    if (HTTP) {
        HTTP->requestCallback = callback;
    }
}


/*
    Used by ejscript Request.close
 */
PUBLIC void httpCloseRx(HttpConn *conn)
{
    if (conn->rx && !conn->rx->remainingContent) {
        /* May not have consumed all read data, so cannot be assured the next request will be okay */
        conn->keepAliveCount = 0;
    }
    if (httpClientConn(conn)) {
        httpEnableConnEvents(conn);
    }
}


PUBLIC bool httpContentNotModified(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    MprTime     modified;
    bool        same;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->flags & HTTP_IF_MODIFIED) {
        /*
            If both checks, the last modification time and etag, claim that the request doesn't need to be
            performed, skip the transfer.
         */
        assert(tx->fileInfo.valid);
        modified = (MprTime) tx->fileInfo.mtime * TPS;
        same = httpMatchModified(conn, modified) && httpMatchEtag(conn, tx->etag);
        if (tx->outputRanges && !same) {
            tx->outputRanges = 0;
        }
        return same;
    }
    return 0;
}


PUBLIC HttpRange *httpCreateRange(HttpConn *conn, MprOff start, MprOff end)
{
    HttpRange     *range;

    if ((range = mprAllocObj(HttpRange, manageRange)) == 0) {
        return 0;
    }
    range->start = start;
    range->end = end;
    range->len = end - start;
    return range;
}


static void manageRange(HttpRange *range, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(range->next);
    }
}


PUBLIC MprOff httpGetContentLength(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->length;
}


PUBLIC cchar *httpGetCookies(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->cookie;
}


PUBLIC cchar *httpGetCookie(HttpConn *conn, cchar *name)
{
    HttpRx  *rx;
    cchar   *cookie;
    char    *cp, *value;
    ssize   nlen;
    int     quoted;

    assert(conn);
    rx = conn->rx;
    assert(rx);

    if ((cookie = rx->cookie) == 0 || name == 0 || *name == '\0') {
        return 0;
    }
    nlen = slen(name);
    while ((value = strstr(cookie, name)) != 0) {
        /* Ignore corrupt cookies of the form "name=;" */
        if ((value == cookie || value[-1] == ' ' || value[-1] == ';') && value[nlen] == '=' && value[nlen+1] != ';') {
            break;
        }
        cookie += nlen;
    }
    if (value == 0) {
        return 0;
    }
    value += nlen;
    while (isspace((uchar) *value) || *value == '=') {
        value++;
    }
    quoted = 0;
    if (*value == '"') {
        value++;
        quoted++;
    }
    for (cp = value; *cp; cp++) {
        if (quoted) {
            if (*cp == '"' && cp[-1] != '\\') {
                break;
            }
        } else {
            if ((*cp == ',' || *cp == ';') && cp[-1] != '\\') {
                break;
            }
        }
    }
    return snclone(value, cp - value);
}


PUBLIC cchar *httpGetHeader(HttpConn *conn, cchar *key)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return mprLookupKey(conn->rx->headers, slower(key));
}


PUBLIC char *httpGetHeadersFromHash(MprHash *hash)
{
    MprKey      *kp;
    char        *headers, *cp;
    ssize       len;

    for (len = 0, kp = 0; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        len += strlen(kp->key) + 2 + strlen(kp->data) + 1;
    }
    if ((headers = mprAlloc(len + 1)) == 0) {
        return 0;
    }
    for (kp = 0, cp = headers; (kp = mprGetNextKey(hash, kp)) != 0; ) {
        strcpy(cp, kp->key);
        cp += strlen(cp);
        *cp++ = ':';
        *cp++ = ' ';
        strcpy(cp, kp->data);
        cp += strlen(cp);
        *cp++ = '\n';
    }
    *cp = '\0';
    return headers;
}


PUBLIC char *httpGetHeaders(HttpConn *conn)
{
    return httpGetHeadersFromHash(conn->rx->headers);
}


PUBLIC MprHash *httpGetHeaderHash(HttpConn *conn)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return conn->rx->headers;
}


PUBLIC cchar *httpGetQueryString(HttpConn *conn)
{
    return (conn->rx && conn->rx->parsedUri) ? conn->rx->parsedUri->query : 0;
}


PUBLIC int httpGetStatus(HttpConn *conn)
{
    return (conn->rx) ? conn->rx->status : 0;
}


PUBLIC char *httpGetStatusMessage(HttpConn *conn)
{
    return (conn->rx) ? conn->rx->statusMessage : 0;
}


PUBLIC void httpSetMethod(HttpConn *conn, cchar *method)
{
    conn->rx->method = sclone(method);
    parseMethod(conn);
}


static void parseUri(HttpConn *conn)
{
    HttpRx      *rx;
    HttpUri     *up;
    cchar       *hostname;

    rx = conn->rx;
    if (httpSetUri(conn, rx->uri) < 0) {
        httpBadRequestError(conn, HTTP_CODE_BAD_REQUEST, "Bad URL");
        rx->parsedUri = httpCreateUri("", 0);

    } else {
        /*
            Complete the URI based on the connection state.
            Must have a complete scheme, host, port and path.
         */
        up = rx->parsedUri;
        up->scheme = sclone(conn->secure ? "https" : "http");
        hostname = rx->hostHeader ? rx->hostHeader : conn->host->name;
        if (!hostname) {
            hostname = conn->sock->acceptIp;
        }
        if (mprParseSocketAddress(hostname, &up->host, NULL, NULL, 0) < 0) {
            if (!conn->error) {
                httpBadRequestError(conn, HTTP_CODE_BAD_REQUEST, "Bad host");
            }
        } else {
            up->port = conn->sock->listenSock->port;
        }
    }
}


PUBLIC int httpSetUri(HttpConn *conn, cchar *uri)
{
    HttpRx      *rx;
    HttpUri     *parsedUri;
    char        *pathInfo;

    rx = conn->rx;
    if ((parsedUri = httpCreateUri(uri, 0)) == 0 || !parsedUri->valid) {
        return MPR_ERR_BAD_ARGS;
    }
    if (parsedUri->host && !rx->hostHeader) {
        rx->hostHeader = parsedUri->host;
    }
    if ((pathInfo = httpValidateUriPath(parsedUri->path)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    rx->pathInfo = pathInfo;
    rx->uri = parsedUri->path;
    conn->tx->ext = httpGetExt(conn);

    /*
        Start out with no scriptName and the entire URI in the pathInfo. Stages may rewrite.
     */
    rx->scriptName = mprEmptyString();
    rx->parsedUri = parsedUri;
    return 0;
}


PUBLIC ssize httpGetReadCount(HttpConn *conn)
{
    return conn->readq->count;
}


PUBLIC bool httpIsEof(HttpConn *conn)
{
    return conn->rx == 0 || conn->rx->eof;
}


PUBLIC cchar *httpGetBodyInput(HttpConn *conn)
{
    HttpQueue   *q;
    HttpRx      *rx;
    MprBuf      *content;

    rx = conn->rx;
    if (!rx->eof) {
        return 0;
    }
    q = conn->readq;
    if (q->first) {
        httpJoinPackets(q, -1);
        if ((content = q->first->content) != 0) {
            mprAddNullToBuf(content);
            return mprGetBufStart(content);
        }
    }
    return 0;
}


/*
    Set the connector as write blocked and cannot proceed.
 */
PUBLIC void httpSocketBlocked(HttpConn *conn)
{
    conn->tx->writeBlocked = 1;
}


static void addMatchEtag(HttpConn *conn, char *etag)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->etags == 0) {
        rx->etags = mprCreateList(-1, MPR_LIST_STABLE);
    }
    mprAddItem(rx->etags, sclone(etag));
}


/*
    Get the next input token. The content buffer is advanced to the next token. This routine always returns a
    non-null token. The empty string means the delimiter was not found. The delimiter is a string to match and not
    a set of characters. If null, it means use white space (space or tab) as a delimiter.
 */
static char *getToken(HttpConn *conn, cchar *delim)
{
    MprBuf  *buf;
    char    *token, *endToken, *nextToken;

    buf = conn->input->content;
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


/*
    Match the entity's etag with the client's provided etag.
 */
PUBLIC bool httpMatchEtag(HttpConn *conn, char *requestedEtag)
{
    HttpRx  *rx;
    char    *tag;
    int     next;

    rx = conn->rx;
    if (rx->etags == 0) {
        return 1;
    }
    if (requestedEtag == 0) {
        return 0;
    }
    for (next = 0; (tag = mprGetNextItem(rx->etags, &next)) != 0; ) {
        if (strcmp(tag, requestedEtag) == 0) {
            return (rx->ifMatch) ? 0 : 1;
        }
    }
    return (rx->ifMatch) ? 1 : 0;
}


/*
    If an IF-MODIFIED-SINCE was specified, then return true if the resource has not been modified. If using
    IF-UNMODIFIED, then return true if the resource was modified.
 */
PUBLIC bool httpMatchModified(HttpConn *conn, MprTime time)
{
    HttpRx   *rx;

    rx = conn->rx;

    if (rx->since == 0) {
        /*  If-Modified or UnModified not supplied. */
        return 1;
    }
    if (rx->ifModified) {
        /*  Return true if the file has not been modified.  */
        return !(time > rx->since);
    } else {
        /*  Return true if the file has been modified.  */
        return (time > rx->since);
    }
}


/*
    Format is:  Range: bytes=n1-n2,n3-n4,...
    Where n1 is first byte pos and n2 is last byte pos

    Examples:
        Range: bytes=0-49             first 50 bytes
        Range: bytes=50-99,200-249    Two 50 byte ranges from 50 and 200
        Range: bytes=-50              Last 50 bytes
        Range: bytes=1-               Skip first byte then emit the rest

    Return 1 if more ranges, 0 if end of ranges, -1 if bad range.
 */
static bool parseRange(HttpConn *conn, char *value)
{
    HttpTx      *tx;
    HttpRange   *range, *last, *next;
    char        *tok, *ep;

    tx = conn->tx;
    value = sclone(value);

    /*
        Step over the "bytes="
     */
    stok(value, "=", &value);

    for (last = 0; value && *value; ) {
        if ((range = mprAllocObj(HttpRange, manageRange)) == 0) {
            return 0;
        }
        /*
            A range "-7" will set the start to -1 and end to 8
         */
        if ((tok = stok(value, ",", &value)) == 0) {
            return 0;
        }
        if (*tok != '-') {
            range->start = (ssize) stoi(tok);
        } else {
            range->start = -1;
        }
        range->end = -1;

        if ((ep = strchr(tok, '-')) != 0) {
            if (*++ep != '\0') {
                /*
                    End is one beyond the range. Makes the math easier.
                 */
                range->end = (ssize) stoi(ep) + 1;
            }
        }
        if (range->start >= 0 && range->end >= 0) {
            range->len = (int) (range->end - range->start);
        }
        if (last == 0) {
            tx->outputRanges = range;
        } else {
            last->next = range;
        }
        last = range;
    }

    /*
        Validate ranges
     */
    for (range = tx->outputRanges; range; range = range->next) {
        if (range->end != -1 && range->start >= range->end) {
            return 0;
        }
        if (range->start < 0 && range->end < 0) {
            return 0;
        }
        next = range->next;
        if (range->start < 0 && next) {
            /* This range goes to the end, so cannot have another range afterwards */
            return 0;
        }
        if (next) {
            if (range->end < 0) {
                return 0;
            }
            if (next->start >= 0 && range->end > next->start) {
                return 0;
            }
        }
    }
    conn->tx->currentRange = tx->outputRanges;
    return (last) ? 1: 0;
}


PUBLIC void httpSetEof(HttpConn *conn)
{
    conn->rx->eof = 1;
}


PUBLIC void httpSetStageData(HttpConn *conn, cchar *key, cvoid *data)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->requestData == 0) {
        rx->requestData = mprCreateHash(-1, 0);
    }
    mprAddKey(rx->requestData, key, data);
}


PUBLIC cvoid *httpGetStageData(HttpConn *conn, cchar *key)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->requestData == 0) {
        return NULL;
    }
    return mprLookupKey(rx->requestData, key);
}


PUBLIC char *httpGetPathExt(cchar *path)
{
    char    *ep, *ext;

    if ((ext = strrchr(path, '.')) != 0) {
        ext = sclone(++ext);
        for (ep = ext; *ep && isalnum((uchar) *ep); ep++) {
            ;
        }
        *ep = '\0';
    }
    return ext;
}


/*
    Get the request extension. Look first at the URI pathInfo. If no extension, look at the filename if defined.
    Return NULL if no extension.
 */
PUBLIC char *httpGetExt(HttpConn *conn)
{
    HttpRx  *rx;
    char    *ext;

    rx = conn->rx;
    if ((ext = httpGetPathExt(rx->pathInfo)) == 0) {
        if (conn->tx->filename) {
            ext = httpGetPathExt(conn->tx->filename);
        }
    }
    return ext;
}


//  FUTURE - can this just use the default compare
static int compareLang(char **s1, char **s2)
{
    return scmp(*s1, *s2);
}


PUBLIC HttpLang *httpGetLanguage(HttpConn *conn, MprHash *spoken, cchar *defaultLang)
{
    HttpRx      *rx;
    HttpLang    *lang;
    MprList     *list;
    cchar       *accept;
    char        *nextTok, *tok, *quality, *language;
    int         next;

    rx = conn->rx;
    if (rx->lang) {
        return rx->lang;
    }
    if (spoken == 0) {
        return 0;
    }
    list = mprCreateList(-1, MPR_LIST_STABLE);
    if ((accept = httpGetHeader(conn, "Accept-Language")) != 0) {
        for (tok = stok(sclone(accept), ",", &nextTok); tok; tok = stok(nextTok, ",", &nextTok)) {
            language = stok(tok, ";", &quality);
            if (quality == 0) {
                quality = "1";
            }
            mprAddItem(list, sfmt("%03d %s", (int) (atof(quality) * 100), language));
        }
        mprSortList(list, (MprSortProc) compareLang, 0);
        for (next = 0; (language = mprGetNextItem(list, &next)) != 0; ) {
            if ((lang = mprLookupKey(rx->route->languages, &language[4])) != 0) {
                rx->lang = lang;
                return lang;
            }
        }
    }
    if (defaultLang && (lang = mprLookupKey(rx->route->languages, defaultLang)) != 0) {
        rx->lang = lang;
        return lang;
    }
    return 0;
}


/*
    Trim extra path information after the uri extension. This is used by CGI and PHP only. The strategy is to
    heuristically find the script name in the uri. This is assumed to be the original uri up to and including
    first path component containing a "." Any path information after that is regarded as extra path.
    WARNING: Extra path is an old, unreliable, CGI specific technique. Do not use directories with embedded periods.
 */
PUBLIC void httpTrimExtraPath(HttpConn *conn)
{
    HttpRx      *rx;
    char        *cp, *extra;
    ssize       len;

    rx = conn->rx;
    if (!(rx->flags & (HTTP_OPTIONS | HTTP_TRACE))) {
        if ((cp = strchr(rx->pathInfo, '.')) != 0 && (extra = strchr(cp, '/')) != 0) {
            len = extra - rx->pathInfo;
            if (0 < len && len < slen(rx->pathInfo)) {
                rx->extraPath = sclone(&rx->pathInfo[len]);
                rx->pathInfo[len] = '\0';
            }
        }
        if ((cp = strchr(rx->target, '.')) != 0 && (extra = strchr(cp, '/')) != 0) {
            len = extra - rx->target;
            if (0 < len && len < slen(rx->target)) {
                rx->target[len] = '\0';
            }
        }
    }
}


/*
    Sends an 100 Continue response to the client. This bypasses the transmission pipeline, writing directly to the socket.
 */
static int sendContinue(HttpConn *conn)
{
    cchar      *response;
    int         mode;

    assert(conn);

    if (!conn->tx->finalized && !conn->tx->bytesWritten) {
        response = sfmt("%s 100 Continue\r\n\r\n", conn->protocol);
        mode = mprGetSocketBlockingMode(conn->sock);
        mprWriteSocket(conn->sock, response, slen(response));
        mprSetSocketBlockingMode(conn->sock, mode);
        mprFlushSocket(conn->sock);
    }
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software. All Rights Reserved.

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
