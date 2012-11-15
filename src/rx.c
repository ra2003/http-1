/*
    rx.c -- Http receiver. Parses http requests and client responses.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void addMatchEtag(HttpConn *conn, char *etag);
static char *getToken(HttpConn *conn, cchar *delim);
static void manageRange(HttpRange *range, int flags);
static void manageRx(HttpRx *rx, int flags);
static bool parseHeaders(HttpConn *conn, HttpPacket *packet);
static bool parseIncoming(HttpConn *conn, HttpPacket *packet);
static bool parseRange(HttpConn *conn, char *value);
static bool parseRequestLine(HttpConn *conn, HttpPacket *packet);
static bool parseResponseLine(HttpConn *conn, HttpPacket *packet);
static void processCompletion(HttpConn *conn);
static bool processContent(HttpConn *conn, HttpPacket *packet);
static void parseMethod(HttpConn *conn);
static bool processParsed(HttpConn *conn);
static bool processReady(HttpConn *conn);
static bool processRunning(HttpConn *conn);
static void routeRequest(HttpConn *conn);
static int setParsedUri(HttpConn *conn);

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
    rx->needInputPipeline = !conn->endpoint;
    rx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS);
    rx->chunkState = HTTP_CHUNK_UNCHUNKED;
    rx->traceLevel = -1;
    return rx;
}


static void manageRx(HttpRx *rx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(rx->method);
        mprMark(rx->uri);
        mprMark(rx->pathInfo);
        mprMark(rx->scriptName);
        mprMark(rx->extraPath);
        mprMark(rx->conn);
        mprMark(rx->route);
        mprMark(rx->etags);
        mprMark(rx->headerPacket);
        mprMark(rx->headers);
        mprMark(rx->inputPipeline);
        mprMark(rx->parsedUri);
        mprMark(rx->requestData);
        mprMark(rx->statusMessage);
        mprMark(rx->accept);
        mprMark(rx->acceptCharset);
        mprMark(rx->acceptEncoding);
        mprMark(rx->acceptLanguage);
        mprMark(rx->cookie);
        mprMark(rx->connection);
        mprMark(rx->contentLength);
        mprMark(rx->hostHeader);
        mprMark(rx->pragma);
        mprMark(rx->mimeType);
        mprMark(rx->originalMethod);
        mprMark(rx->originalUri);
        mprMark(rx->redirect);
        mprMark(rx->referrer);
        mprMark(rx->securityToken);
        mprMark(rx->session);
        mprMark(rx->userAgent);
        mprMark(rx->params);
        mprMark(rx->svars);
        mprMark(rx->inputRange);
        mprMark(rx->passDigest);
        mprMark(rx->files);
        mprMark(rx->uploadDir);
        mprMark(rx->paramString);
        mprMark(rx->lang);
        mprMark(rx->target);
        mprMark(rx->upgrade);
        mprMark(rx->webSocket);

    } else if (flags & MPR_MANAGE_FREE) {
        if (rx->conn) {
            rx->conn->rx = 0;
        }
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
    Pump the Http engine.
    Process an incoming request and drive the state machine. This will process only one request.
    All socket I/O is non-blocking, and this routine must not block. Note: packet may be null.
    Return true if the request is completed successfully.
 */
PUBLIC bool httpPumpRequest(HttpConn *conn, HttpPacket *packet)
{
    bool    canProceed;

    assure(conn);
    if (conn->pumping) {
        return 0;
    }
    canProceed = 1;
    conn->pumping = 1;

    while (canProceed) {
        LOG(7, "httpProcess %s, state %d, error %d", conn->dispatcher->name, conn->state, conn->error);
        switch (conn->state) {
        case HTTP_STATE_BEGIN:
        case HTTP_STATE_CONNECTED:
            canProceed = parseIncoming(conn, packet);
            break;

        case HTTP_STATE_PARSED:
            canProceed = processParsed(conn);
            break;

        case HTTP_STATE_CONTENT:
            canProceed = processContent(conn, packet);
            break;

        case HTTP_STATE_READY:
            canProceed = processReady(conn);
            break;

        case HTTP_STATE_RUNNING:
            canProceed = processRunning(conn);
            break;

        case HTTP_STATE_FINALIZED:
            processCompletion(conn);
            break;

        case HTTP_STATE_COMPLETE:
            conn->pumping = 0;
            return !conn->connError;

        default:
            assure(conn->state == HTTP_STATE_COMPLETE);
            break;
        }
        packet = conn->input;
    }
    conn->pumping = 0;
    return 0;
}


/*  
    Parse the incoming http message. Return true to keep going with this or subsequent request, zero means
    insufficient data to proceed.
 */
static bool parseIncoming(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    ssize       len;
    char        *start, *end;

    if (packet == NULL) {
        return 0;
    }
    if (mprShouldDenyNewRequests()) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Server terminating");
        return 0;
    }
    if (!conn->rx) {
        conn->rx = httpCreateRx(conn);
        conn->tx = httpCreateTx(conn, NULL);
    }
    rx = conn->rx;
    if ((len = httpGetPacketLength(packet)) == 0) {
        return 0;
    }
    start = mprGetBufStart(packet->content);

#if FUTURE
    while (*start == '\r' || *start == '\n') {
        mprGetCharFromBuf(packet->content);
        start = mprGetBufStart(packet->content);
    }
#endif

    /*
        Don't start processing until all the headers have been received (delimited by two blank lines)
     */
    if ((end = sncontains(start, "\r\n\r\n", len)) == 0) {
        if (len >= conn->limits->headerSize) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, 
                "Header too big. Length %d vs limit %d", len, conn->limits->headerSize);
        }
        return 0;
    }
    len = end - start;
    mprAddNullToBuf(packet->content);

    if (len >= conn->limits->headerSize) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE, 
            "Header too big. Length %d vs limit %d", len, conn->limits->headerSize);
        return 0;
    }
    if (conn->endpoint) {
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
    if (conn->endpoint) {
        httpMatchHost(conn);
        setParsedUri(conn);

    } else if (rx->status != HTTP_CODE_CONTINUE) {
        /* 
            Ignore Expect status responses. NOTE: Clients have already created their Tx pipeline.
         */
        httpCreateRxPipeline(conn, conn->http->clientRoute);
    }
    httpSetState(conn, HTTP_STATE_PARSED);
    return 1;
}


static void mapMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;

    rx = conn->rx;
    if (rx->flags & HTTP_POST) {
        if ((method = httpGetParam(conn, "-http-method-", 0)) != 0) {
            if (!scaselessmatch(method, rx->method)) {
                mprLog(3, "Change method from %s to %s for %s", rx->method, method, rx->uri);
                httpSetMethod(conn, method);
            }
        }
    }
}


static void routeRequest(HttpConn *conn)
{
    HttpRx  *rx;

    assure(conn->endpoint);

    rx = conn->rx;
    httpAddParams(conn);
    mapMethod(conn);
    httpRouteRequest(conn);  
    httpCreateRxPipeline(conn, rx->route);
    httpCreateTxPipeline(conn, rx->route);
}


/*
    Only called by parseRequestLine
 */
static void traceRequest(HttpConn *conn, HttpPacket *packet)
{
    MprBuf  *content;
    cchar   *endp, *ext, *cp;
    int     len, level;

    content = packet->content;
    ext = 0;
    /*
        Find the Uri extension:   "GET /path.ext HTTP/1.1"
     */
    if ((cp = schr(content->start, ' ')) != 0) {
        if ((cp = schr(++cp, ' ')) != 0) {
            for (ext = --cp; ext > content->start && *ext != '.'; ext--) ;
            ext = (*ext == '.') ? snclone(&ext[1], cp - ext) : 0;
            conn->tx->ext = ext;
        }
    }
    /*
        If tracing header, do entire header including first line
     */
    if ((conn->rx->traceLevel = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, ext)) >= 0) {
        mprLog(4, "New request from %s:%d to %s:%d", conn->ip, conn->port, conn->sock->acceptIp, conn->sock->acceptPort);
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (int) (endp - mprGetBufStart(content) + 4) : 0;
        httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, packet, len, 0);

    } else if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_FIRST, ext)) >= 0) {
        endp = strstr((char*) content->start, "\r\n");
        len = (endp) ? (int) (endp - mprGetBufStart(content) + 2) : 0;
        if (len > 0) {
            content->start[len - 2] = '\0';
            mprLog(level, "%s", content->start);
            content->start[len - 2] = '\r';
        }
    }
    httpValidateLimits(conn->endpoint, HTTP_VALIDATE_OPEN_REQUEST, conn);
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
    char        *uri, *protocol;
    ssize       len;

    rx = conn->rx;
#if BIT_DEBUG
    conn->startMark = mprGetHiResTime();
#endif
    traceRequest(conn, packet);

    rx->originalMethod = rx->method = supper(getToken(conn, 0));
    parseMethod(conn);

    uri = getToken(conn, 0);
    len = slen(uri);
    if (*uri == '\0') {
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
        return 0;
    } else if (len >= conn->limits->uriSize) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE, 
            "Bad request. URI too long. Length %d vs limit %d", len, conn->limits->uriSize);
        return 0;
    }
    protocol = conn->protocol = supper(getToken(conn, "\r\n"));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        if (rx->flags & (HTTP_POST|HTTP_PUT)) {
            rx->remainingContent = MAXINT;
            rx->needInputPipeline = 1;
        }
        conn->http10 = 1;
        conn->protocol = protocol;
    } else if (strcmp(protocol, "HTTP/1.1") == 0) {
        conn->protocol = protocol;
    } else {
        conn->protocol = sclone("HTTP/1.1");
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    rx->originalUri = rx->uri = sclone(uri);
    httpSetState(conn, HTTP_STATE_FIRST);
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
    int         level, traced;

    rx = conn->rx;
    tx = conn->tx;
    traced = 0;

    if (httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, tx->ext) >= 0) {
        content = packet->content;
        endp = strstr((char*) content->start, "\r\n\r\n");
        len = (endp) ? (int) (endp - mprGetBufStart(content) + 4) : 0;
        httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_HEADER, packet, len, 0);
        traced = 1;
    }
    protocol = conn->protocol = supper(getToken(conn, 0));
    if (strcmp(protocol, "HTTP/1.0") == 0) {
        conn->http10 = 1;
        if (!scaselessmatch(tx->method, "HEAD")) {
            rx->remainingContent = MAXINT;
        }
    } else if (strcmp(protocol, "HTTP/1.1") != 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Unsupported HTTP protocol");
        return 0;
    }
    status = getToken(conn, 0);
    if (*status == '\0') {
        httpError(conn, HTTP_ABORT | HTTP_CODE_NOT_ACCEPTABLE, "Bad response status code");
        return 0;
    }
    rx->status = atoi(status);
    rx->statusMessage = sclone(getToken(conn, "\r\n"));

    len = slen(rx->statusMessage);
    if (len >= conn->limits->uriSize) {
        httpError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_URL_TOO_LARGE, 
            "Bad response. Status message too long. Length %d vs limit %d", len, conn->limits->uriSize);
        return 0;
    }
    if (!traced && (level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_FIRST, tx->ext)) >= 0) {
        mprLog(level, "%s %d %s", protocol, rx->status, rx->statusMessage);
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
    int         count, keepAlive;

    rx = conn->rx;
    tx = conn->tx;
    content = packet->content;
    conn->rx->headerPacket = packet;
    limits = conn->limits;
    keepAlive = (conn->http10) ? 0 : 1;

    for (count = 0; content->start[0] != '\r' && !conn->error; count++) {
        if (count >= limits->headerMax) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Too many headers");
            return 0;
        }
        if ((key = getToken(conn, ":")) == 0 || *key == '\0') {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header format");
            return 0;
        }
        value = getToken(conn, "\r\n");
        while (isspace((uchar) *value)) {
            value++;
        }
        LOG(8, "Key %s, value %s", key, value);
        if (strspn(key, "%<>/\\") > 0) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad header key value");
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
                    keepAlive = 1;
                } else if (scaselesscmp(value, "CLOSE") == 0) {
                    /*  Not really required, but set to 0 to be sure */
                    conn->keepAliveCount = 0;
#if UNUSED && CLASHES
                } else if (scaselesscmp(value, "upgrade") == 0) {
                    rx->upgrade = sclone(value);
#endif
                }

            } else if (strcasecmp(key, "content-length") == 0) {
                if (rx->length >= 0) {
                    httpError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Mulitple content length headers");
                    break;
                }
                rx->length = stoi(value);
                if (rx->length < 0) {
                    httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad content length");
                    return 0;
                }
                if (rx->length >= conn->limits->receiveBodySize) {
                    httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                        "Request content length %,Ld bytes is too big. Limit %,Ld", 
                        rx->length, conn->limits->receiveBodySize);
                    return 0;
                }
                rx->contentLength = sclone(value);
                assure(rx->length >= 0);
                if (conn->endpoint || !scaselessmatch(tx->method, "HEAD")) {
                    rx->remainingContent = rx->length;
                    rx->needInputPipeline = 1;
                }

            } else if (strcasecmp(key, "content-range") == 0) {
                /*
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
                    }
                    if ((sp = strchr(sp, '/')) != 0) {
                        /*
                            Note this is not the content length transmitted, but the original size of the input of which
                            the client is transmitting only a portion.
                         */
                        size = stoi(++sp);
                    }
                }
                if (start < 0 || end < 0 || size < 0 || end <= start) {
                    httpError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad content range");
                    break;
                }
                rx->inputRange = httpCreateRange(conn, start, end);

            } else if (strcasecmp(key, "content-type") == 0) {
                rx->mimeType = sclone(value);
                if (rx->flags & (HTTP_POST | HTTP_PUT)) {
                    rx->form = scontains(rx->mimeType, "application/x-www-form-urlencoded") != 0;
                    rx->upload = scontains(rx->mimeType, "multipart/form-data") != 0;
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
                        httpError(conn, HTTP_CODE_EXPECTATION_FAILED, "Expect header value \"%s\" is unsupported", value);
                    } else {
                        rx->flags |= HTTP_EXPECT_CONTINUE;
                    }
                }
            }
            break;

        case 'h':
            if (strcasecmp(key, "host") == 0) {
                rx->hostHeader = sclone(value);
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
                    assure(0);
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
                keepAlive = 1;
                if ((tok = scontains(value, "max=")) != 0) {
                    conn->keepAliveCount = atoi(&tok[4]);
                    /*  
                        IMPORTANT: Deliberately close the connection one request early. This ensures a client-led 
                        termination and helps relieve server-side TIME_WAIT conditions.
                     */
                    if (conn->keepAliveCount == 1) {
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
                if (!parseRange(conn, value)) {
                    httpError(conn, HTTP_CLOSE | HTTP_CODE_RANGE_NOT_SATISFIABLE, "Bad range");
                }
            } else if (strcasecmp(key, "referer") == 0) {
                /* NOTE: yes the header is misspelt in the spec */
                rx->referrer = sclone(value);
#if UNUSED
            /*
                There is a draft spec for these, but it has bad DOS security implications.
             */
            } else if (strcasecmp(key, "request-timeout") == 0) {
                conn->limits->requestTimeout = stoi(value) * MPR_TICKS_PER_SEC;
                conn->limits->inactivityTimeout = stoi(value) * MPR_TICKS_PER_SEC;
#endif
            }
            break;

        case 's':
#if UNUSED
            if (strcasecmp(key, "sec-websocket-key") == 0) {
                rx->webSockKey = sclone(value);
            } else if (strcasecmp(key, "sec-websocket-extensions") == 0) {
                if (rx->extensions) {
                    rx->extensions = sjoin(rx->extensions, ", ", value, NULL);
                } else {
                    rx->extensions = sclone(value);
                }
            } else if (strcasecmp(key, "sec-websocket-protocol") == 0) {
                rx->webSockProtocols = sclone(value);
            } else if (strcasecmp(key, "sec-websocket-version") == 0) {
                rx->webSockVersion = (int) stoi(value);
            }
#endif
            break;

        case 't':
            if (strcasecmp(key, "transfer-encoding") == 0) {
                if (scaselesscmp(value, "chunked") == 0) {
                    /*  
                        remainingContent will be revised by the chunk filter as chunks are processed and will 
                        be set to zero when the last chunk has been received.
                     */
                    rx->flags |= HTTP_CHUNKED;
                    rx->chunkState = HTTP_CHUNK_START;
                    rx->remainingContent = MAXINT;
                    rx->needInputPipeline = 1;
                }
            }
            break;

        case 'x':
            if (strcasecmp(key, "x-http-method-override") == 0) {
                httpSetMethod(conn, value);
#if BIT_DEBUG
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
    if (!keepAlive) {
        conn->keepAliveCount = 0;
    }
    if (!(rx->flags & HTTP_CHUNKED)) {
        /*  
            Step over "\r\n" after headers. 
            Don't do this if chunked so chunking can parse a single chunk delimiter of "\r\nSIZE ...\r\n"
         */
        mprAdjustBufStart(content, 2);
    }
    return 1;
}


/*
    Sends an 100 Continue response to the client. This bypasses the transmission pipeline, writing directly to the socket.
 */
static int sendContinue(HttpConn *conn)
{
    cchar      *response;

    assure(conn);
    assure(conn->sock);

    /* Write the response to the socket and flush. */
    response = sfmt("%s 100 Continue\r\n\r\n", conn->protocol);
    mprWriteSocket(conn->sock, response, slen(response));
    mprFlushSocket(conn->sock);
    return 0;
}


/*
    Called once the HTTP request/response headers have been parsed
 */
static bool processParsed(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (!rx->form && conn->endpoint) {
        /*
            Routes need to be able to access form data, so forms will route later after all input is received.
         */
        routeRequest(conn);
    }
    /*
        Don't stream input if a form or upload. NOTE: Upload needs the Files[] collection.
     */
    rx->streamInput = !(rx->form || rx->upload);
    /*
        Send a 100 (Continue) response if the client has requested it. If the connection has an error, that takes
        precedence and 100 Continue will not be sent. Also, if the connector has already written bytes to the socket, we
        do not send 100 Continue to avoid corrupting the response.
     */
    if ((rx->flags & HTTP_EXPECT_CONTINUE) && !conn->tx->finalized && !conn->tx->bytesWritten) {
        sendContinue(conn);
        rx->flags &= ~HTTP_EXPECT_CONTINUE;
    }
    if (!conn->endpoint && conn->upgraded && !httpVerifyWebSocketsHandshake(conn)) {
        return 1;
    }
    httpSetState(conn, HTTP_STATE_CONTENT);

    if (rx->streamInput) {
        httpStartPipeline(conn);
    } else if (rx->remainingContent == 0) {
        httpPutPacketToNext(conn->readq, httpCreateEndPacket());
        rx->eof = 1;
        httpSetState(conn, HTTP_STATE_READY);
    }
    httpServiceQueues(conn);
    return 1;
}


static bool processContent(HttpConn *conn, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpQueue   *q;
    MprBuf      *content;
    ssize       nbytes;

    assure(conn);
    if (!packet) {
        httpServiceQueues(conn);
        return 0;
    }
    rx = conn->rx;
    tx = conn->tx;
    content = packet->content;

    q = tx->queue[HTTP_QUEUE_RX];
    VERIFY_QUEUE(q);
    LOG(6, "processContent: packet of %d bytes, remaining %d", mprGetBufLength(content), rx->remainingContent);
    
    /*
        Determine if end of input (end-of-file)
     */
    if (rx->chunkState) {
        nbytes = httpFilterChunkData(q, packet);
        if (rx->chunkState == HTTP_CHUNK_EOF) {
            rx->eof = 1;
            assure(rx->remainingContent == 0);
        }
    } else {
        nbytes = (ssize) min(rx->remainingContent, mprGetBufLength(content));
        if (!conn->upgraded && (rx->remainingContent - nbytes) <= 0) {
            rx->eof = 1;
        }
    }
    if (mprIsSocketEof(conn->sock)) {
        rx->eof = 1;
    }
    if (nbytes > 0) {
        if (!conn->upgraded) {
            rx->remainingContent -= nbytes;
            assure(rx->remainingContent >= 0);
        }
        rx->bytesRead += nbytes;
        if (httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_BODY, tx->ext) >= 0) {
            httpTraceContent(conn, HTTP_TRACE_RX, HTTP_TRACE_BODY, packet, nbytes, rx->bytesRead);
        }
        /*
            Enforce sandbox limits
         */
        if (!conn->error) {
            if (rx->bytesRead >= conn->limits->receiveBodySize) {
                httpError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE, 
                    "Request body of %,Ld bytes is too big. Limit %,Ld", rx->bytesRead, conn->limits->receiveBodySize);

            } else if (rx->form && rx->bytesRead >= conn->limits->receiveFormSize) {
                httpError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE, 
                    "Request form of %,Ld bytes is too big. Limit %,Ld", rx->bytesRead, conn->limits->receiveFormSize);
            }
        }
        /*
            Send packet upstream toward the handler
         */
        if (packet == rx->headerPacket && nbytes > 0) {
            packet = httpSplitPacket(packet, 0);
        }
        if (httpGetPacketLength(packet) > nbytes) {
            /*  Split excess data belonging to the next chunk or pipelined request */
            LOG(7, "processContent: Split packet of %d at %d", httpGetPacketLength(packet), nbytes);
            conn->input = httpSplitPacket(packet, nbytes);
        } else {
            conn->input = 0;
        }
        if (!(tx->finalized && conn->endpoint)) {
            if (rx->form) {
                httpPutForService(q, packet, HTTP_DELAY_SERVICE);
            } else {
                httpPutPacketToNext(q, packet);
            }
        }
    }
    if (rx->eof) {
        if (rx->remainingContent > 0 && !conn->http10) {
            /* Closing is the only way for HTTP/1.0 to signify the end of data */
            httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
        }
        if (rx->form && conn->endpoint) {
            /* Forms wait for all data before routing */
            routeRequest(conn);
            while ((packet = httpGetPacket(q)) != 0) {
                httpPutPacketToNext(q, packet);
            }
        }
        /*
            Send "end" pack to signify eof to the handler
         */
        httpPutPacketToNext(q, httpCreateEndPacket());
        if (!rx->streamInput) {
            httpStartPipeline(conn);
        }
        httpSetState(conn, HTTP_STATE_READY);
        return conn->workerEvent ? 0 : 1;
    }
    httpServiceQueues(conn);
    if (rx->chunkState && nbytes <= 0) {
        /* Insufficient data */
        return 0;
    }
    if (conn->input && httpGetPacketLength(conn->input) > 0) {
        /* More data to process */
        return 1;
    }
    /* Must wait */
    return 0;
}


/*
    In the ready state after all content has been received
 */
static bool processReady(HttpConn *conn)
{
    httpServiceQueues(conn);
    httpReadyHandler(conn);
    httpSetState(conn, HTTP_STATE_RUNNING);
    return 1;
}


/*
    Note: may be called multiple times in response to output I/O events
 */
static bool processRunning(HttpConn *conn)
{
    HttpQueue   *q;
    HttpTx      *tx;
    int         canProceed;

    q = conn->writeq;
    tx = conn->tx;
    canProceed = 1;
    httpServiceQueues(conn);

    if (conn->endpoint) {
        /* Server side */
        if (tx->finalized) {
            if (tx->finalizedConnector) {
                /* Request complete and output complete */
                httpSetState(conn, HTTP_STATE_FINALIZED);
            } else {
                /* Still got output to do. Wait for Tx I/O event. Do suspend incase handler not using auto-flow routines */
                tx->writeBlocked = 1;
                httpSuspendQueue(q);
                httpEnableConnEvents(q->conn);
                canProceed = 0;
                assure(conn->state < HTTP_STATE_FINALIZED);
            }

        } else if (!httpGetMoreOutput(conn)) {
            /* Request not complete yet. No process callback defined */
            canProceed = 0;
            assure(conn->state < HTTP_STATE_FINALIZED);

        } else if (conn->state >= HTTP_STATE_FINALIZED) {
            /* This happens when httpGetMoreOutput calls writable on windows which then completes the request */
            canProceed = 1;

        } else if (q->count < q->low) {
            if (q->count == 0) {
                /* Queue is empty and data may have drained above in httpServiceQueues. Yield to reclaim memory. */
                mprYield(0);
            }
            if (q->flags & HTTP_QUEUE_SUSPENDED) {
                httpResumeQueue(q);
            }
            /* Need to give events a chance to run. Otherwise can ping/pong suspend to resume */
            canProceed = 0;

        } else {
            /* Wait for output to drain */
            tx->writeBlocked = 1;
            httpSuspendQueue(q);
            httpEnableConnEvents(q->conn);
            canProceed = 0;
            assure(conn->state < HTTP_STATE_FINALIZED);
        }
    } else {
        /* Client side */
        httpServiceQueues(conn);
        if (conn->upgraded) {
            canProceed = 0;
            assure(conn->state < HTTP_STATE_FINALIZED);
        } else {
            httpFinalize(conn);
            if (tx->finalized && conn->rx->eof) {
                httpSetState(conn, HTTP_STATE_FINALIZED);
            } else {
                assure(0);
            }
        }
    }
    return canProceed;
}


#if BIT_DEBUG
static void measure(HttpConn *conn)
{
    MprTicks    elapsed;
    HttpTx      *tx;
    cchar       *uri;
    int         level;

    tx = conn->tx;
    if (conn->rx == 0 || tx == 0) {
        return;
    }
    uri = (conn->endpoint) ? conn->rx->uri : tx->parsedUri->path;
   
    if ((level = httpShouldTrace(conn, HTTP_TRACE_TX, HTTP_TRACE_TIME, tx->ext)) >= 0) {
        elapsed = mprGetTicks() - conn->started;
#if MPR_HIGH_RES_TIMER
        if (elapsed < 1000) {
            mprLog(level, "TIME: Request %s took %,d msec %,d ticks", uri, elapsed, mprGetHiResTime() - conn->startMark);
        } else
#endif
            mprLog(level, "TIME: Request %s took %,d msec", uri, elapsed);
    }
}
#else
#define measure(conn)
#endif


static void processCompletion(HttpConn *conn)
{
    HttpRx      *rx;

    assure(conn->tx->finalized);
    assure(conn->tx->finalizedOutput);
    assure(conn->tx->finalizedConnector);
    rx = conn->rx;
    httpDestroyPipeline(conn);
    measure(conn);
    if (conn->endpoint && rx) {
        assure(rx->route);
        if (rx->route && rx->route->log) {
            httpLogRequest(conn);
        }
        httpValidateLimits(conn->endpoint, HTTP_VALIDATE_CLOSE_REQUEST, conn);
    }
    assure(conn->state == HTTP_STATE_FINALIZED);
    httpSetState(conn, HTTP_STATE_COMPLETE);
}


/*
    Used by ejscript Request.close
 */
PUBLIC void httpCloseRx(HttpConn *conn)
{
    if (conn->rx && !conn->rx->remainingContent) {
        /* May not have consumed all read data, so can't be assured the next request will be okay */
        conn->keepAliveCount = -1;
    }
    if (conn->state < HTTP_STATE_FINALIZED) {
        httpPumpRequest(conn, NULL);
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
        assure(tx->fileInfo.valid);
        modified = (MprTime) tx->fileInfo.mtime * MPR_TICKS_PER_SEC;
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
        assure(conn->rx);
        return 0;
    }
    return conn->rx->length;
}


PUBLIC cchar *httpGetCookies(HttpConn *conn)
{
    if (conn->rx == 0) {
        assure(conn->rx);
        return 0;
    }
    return conn->rx->cookie;
}


PUBLIC cchar *httpGetHeader(HttpConn *conn, cchar *key)
{
    if (conn->rx == 0) {
        assure(conn->rx);
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
        assure(conn->rx);
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


static int setParsedUri(HttpConn *conn)
{
    HttpRx      *rx;
    char        *cp;
    cchar       *hostname;

    rx = conn->rx;
    if (httpSetUri(conn, rx->uri) < 0 || rx->pathInfo[0] != '/') {
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad URL");
        return MPR_ERR_BAD_ARGS;
    }
    /*
        Complete the URI based on the connection state.
        Must have a complete scheme, host, port and path.
     */
    rx->parsedUri->scheme = sclone(conn->secure ? "https" : "http");
    hostname = rx->hostHeader ? rx->hostHeader : conn->host->name;
    if (!hostname) {
        hostname = conn->sock->acceptIp;
    }
    rx->parsedUri->host = sclone(hostname);
    if ((cp = strchr(rx->parsedUri->host, ':')) != 0) {
        *cp = '\0';
    }
    rx->parsedUri->port = conn->sock->listenSock->port;
    return 0;
}


PUBLIC int httpSetUri(HttpConn *conn, cchar *uri)
{
    HttpRx      *rx;
    char        *pathInfo;

    rx = conn->rx;
    if ((rx->parsedUri = httpCreateUri(uri, 0)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    pathInfo = httpNormalizeUriPath(mprUriDecode(rx->parsedUri->path));
    if (pathInfo[0] != '/') {
        return MPR_ERR_BAD_ARGS;
    }
    rx->pathInfo = pathInfo;
    rx->uri = rx->parsedUri->path;
    conn->tx->ext = httpGetExt(conn);
    /*
        Start out with no scriptName and the entire URI in the pathInfo. Stages may rewrite.
     */
    rx->scriptName = mprEmptyString();
    return 0;
}


/*
    Wait for the connection to reach a given state.
    @param state Desired state. Set to zero if you want to wait for one I/O event.
    @param timeout Timeout in msec. If timeout is zer, wait forever. If timeout is < 0, use default inactivity 
        and duration timeouts.
 */
PUBLIC int httpWait(HttpConn *conn, int state, MprTicks timeout)
{
    MprTicks    mark, remaining, inactivityTimeout;
    int         eventMask, saveAsync, justOne, workDone;

    if (state == 0) {
        state = HTTP_STATE_FINALIZED;
        justOne = 1;
    } else {
        justOne = 0;
    }
    if (conn->state <= HTTP_STATE_BEGIN) {
        assure(conn->state >= HTTP_STATE_BEGIN);
        return MPR_ERR_BAD_STATE;
    } 
    if (conn->input && httpGetPacketLength(conn->input) > 0) {
        httpPumpRequest(conn, conn->input);
    }
    assure(conn->sock);
    if (conn->error || !conn->sock) {
        return MPR_ERR_BAD_STATE;
    }
    mark = mprGetTicks();
    if (mprGetDebugMode()) {
        inactivityTimeout = timeout = MPR_MAX_TIMEOUT;
    } else {
        inactivityTimeout = timeout < 0 ? conn->limits->inactivityTimeout : MPR_MAX_TIMEOUT;
        if (timeout < 0) {
            timeout = conn->limits->requestTimeout;
        }
    }
    saveAsync = conn->async;
    conn->async = 1;

    eventMask = MPR_READABLE;
    if (!conn->tx->finalizedConnector) {
        eventMask |= MPR_WRITABLE;
    }
    if (conn->state < state) {
        httpSetupWaitHandler(conn, eventMask);
    }
    remaining = timeout;
    do {
        workDone = httpServiceQueues(conn);
        if (conn->state < state) {
            mprWaitForEvent(conn->dispatcher, min(inactivityTimeout, remaining));
        }
        if (conn->sock && mprIsSocketEof(conn->sock) && !workDone) {
            break;
        }
        remaining = mprGetRemainingTicks(mark, timeout);
    } while (!justOne && !conn->error && conn->state < state && remaining > 0);

    conn->async = saveAsync;
    if (conn->sock == 0 || conn->error) {
        return MPR_ERR_CANT_CONNECT;
    }
    if (!justOne && conn->state < state) {
        return (remaining <= 0) ? MPR_ERR_TIMEOUT : MPR_ERR_CANT_READ;
    }
    return 0;
}


/*  
    Set the connector as write blocked and can't proceed.
 */
PUBLIC void httpSocketBlocked(HttpConn *conn)
{
    mprLog(7, "Socket full, waiting to drain.");
    conn->tx->writeBlocked = 1;
}


static void addMatchEtag(HttpConn *conn, char *etag)
{
    HttpRx   *rx;

    rx = conn->rx;
    if (rx->etags == 0) {
        rx->etags = mprCreateList(-1, 0);
    }
    mprAddItem(rx->etags, etag);
}


/*
    Get the next input token. The content buffer is advanced to the next token. This routine always returns a
    non-zero token. The empty string means the delimiter was not found. The delimiter is a string to match and not
    a set of characters. If null, it means use white space (space or tab) as a delimiter. 
 */
static char *getToken(HttpConn *conn, cchar *delim)
{
    MprBuf  *buf;
    char    *token, *endToken, *nextToken;

    buf = conn->input->content;
    token = mprGetBufStart(buf);
    nextToken = mprGetBufEnd(buf);
    for (token = mprGetBufStart(buf); (*token == ' ' || *token == '\t') && token < mprGetBufEnd(buf); token++) {}

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
    if (value == 0) {
        return 0;
    }
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
        tok = stok(value, ",", &value);
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
            /* This range goes to the end, so can't have another range afterwards */
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


//  MOB - can this just use the default compare
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
    list = mprCreateList(-1, 0);
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
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

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
