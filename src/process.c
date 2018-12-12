/*
    process.c - Process HTTP request/response.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void addMatchEtag(HttpConn *conn, char *etag);
static void prepErrorDoc(HttpQueue *q);
static bool mapMethod(HttpConn *conn);
static void measureRequest(HttpQueue *q);
static bool parseRange(HttpConn *conn, char *value);
static void parseUri(HttpConn *conn);
static bool processCompletion(HttpQueue *q);
static bool processContent(HttpQueue *q);
static void processFinalized(HttpQueue *q);
static void processFirst(HttpQueue *q);
static void processHeaders(HttpQueue *q);
static void processHttp(HttpQueue *q);
static void processParsed(HttpQueue *q);
static void processReady(HttpQueue *q);
static bool processRunning(HttpQueue *q);
static bool pumpOutput(HttpQueue *q);
static void routeRequest(HttpConn *conn);
static int sendContinue(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC bool httpProcessHeaders(HttpQueue *q)
{
    if (q->conn->state != HTTP_STATE_PARSED) {
        return 0;
    }
    processFirst(q);
    processHeaders(q);
    processParsed(q);
    return 1;
}


PUBLIC void httpProcess(HttpQueue *q)
{
    //  MOB - should have flag if already scheduled?
    mprCreateEvent(q->conn->dispatcher, "http2", 0, processHttp, q->conn->inputq, 0);
}


/*
    HTTP Protocol state machine for HTTP/1 server requests and client responses.
    Process an incoming request/response and drive the state machine.
    This will process only one request/response.
    All socket I/O is non-blocking, and this routine must not block.
 */
static void processHttp(HttpQueue *q)
{
    HttpConn    *conn;
    bool        more;
    int         count;

    if (!q) {
        return;
    }
    conn = q->conn;

    for (count = 0, more = 1; more && count < 10; count++) {
        switch (conn->state) {
        case HTTP_STATE_PARSED:
            httpProcessHeaders(q);
            break;

        case HTTP_STATE_CONTENT:
            more = processContent(q);
            break;

        case HTTP_STATE_READY:
            processReady(q);
            break;

        case HTTP_STATE_RUNNING:
            more = processRunning(q);
            break;

        case HTTP_STATE_FINALIZED:
            processFinalized(q);
            break;

        case HTTP_STATE_COMPLETE:
            more = processCompletion(q);
            break;

        default:
            if (conn->error) {
                httpSetState(conn, HTTP_STATE_FINALIZED);
            } else {
                more = 0;
            }
            break;
        }
        httpServiceQueues(conn->net, HTTP_BLOCK);
    }
    if (conn->complete && httpServerConn(conn)) {
        if (conn->keepAliveCount <= 0 || conn->net->protocol >= 2) {
            httpDestroyConn(conn);
        } else {
            httpResetServerConn(conn);
        }
    }
}


static void processFirst(HttpQueue *q)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;

    net = q->net;
    conn = q->conn;
    rx = conn->rx;

    if (httpIsServer(net)) {
        conn->startMark = mprGetHiResTicks();
        conn->started = conn->http->now;
        conn->http->totalRequests++;
        httpSetState(conn, HTTP_STATE_FIRST);

    } else {
#if TODO /* TODO: 100 Continue */
        if (rx->status != HTTP_CODE_CONTINUE) {
            /*
                Ignore Expect status responses. NOTE: Clients have already created their Tx pipeline.
             */
            httpCreateRxPipeline(conn, NULL);
        }
#endif

    }
    if (rx->flags & HTTP_EXPECT_CONTINUE) {
        sendContinue(q);
        rx->flags &= ~HTTP_EXPECT_CONTINUE;
    }

    if (httpTracing(net) && httpIsServer(net)) {
        httpTrace(conn->trace, "http.rx.headers", "request", "method:'%s', uri:'%s', protocol:'%d'",
            rx->method, rx->uri, conn->net->protocol);
        if (net->protocol >= 2) {
            httpTrace(conn->trace, "http.rx.headers", "context", "\n%s", httpTraceHeaders(q, conn->rx->headers));
        }
    }
}


static void processHeaders(HttpQueue *q)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    HttpLimits  *limits;
    MprKey      *kp;
    char        *cp, *key, *value, *tok;
    int         keepAliveHeader;

    net = q->net;
    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;
    limits = conn->limits;
    keepAliveHeader = 0;

    for (ITERATE_KEYS(rx->headers, kp)) {
        key = kp->key;
        value = (char*) kp->data;
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
            if (strcasecmp(key, "connection") == 0 && net->protocol < 2) {
                rx->connection = sclone(value);
                if (scaselesscmp(value, "KEEP-ALIVE") == 0) {
                    keepAliveHeader = 1;

                } else if (scaselesscmp(value, "CLOSE") == 0) {
                    conn->keepAliveCount = 0;
                }

            } else if (strcasecmp(key, "content-length") == 0) {
                if (rx->length >= 0) {
                    httpBadRequestError(conn, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Mulitple content length headers");
                    break;
                }
                rx->length = stoi(value);
                if (rx->length < 0) {
                    httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad content length");
                    return;
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
                        rx->json = sstarts(rx->mimeType, "application/json");
                        rx->upload = scontains(rx->mimeType, "multipart/form-data") != 0;
                    }
                }
            } else if (strcasecmp(key, "cookie") == 0) {
                /* Should be only one cookie header really with semicolon delimmited key/value pairs */
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
                    Handle 100-continue for HTTP/1.1+ clients only. This is the only expectation that is currently supported.
                 */
                if (conn->net->protocol > 0) {
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
#if DONE_IN_HTTP1
            if (strcasecmp(key, "transfer-encoding") == 0 && conn->net->protocol == 1) {
                if (scaselesscmp(value, "chunked") == 0) {
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
#endif
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
            } else if (strcasecmp(key, "x-chunk-size") == 0 && net->protocol < 2) {
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
    if (net->protocol == 0 && !keepAliveHeader) {
        conn->keepAliveCount = 0;
    }

}


/*
    Called once the HTTP request/response headers have been parsed
 */
static void processParsed(HttpQueue *q)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *hostname;

    net = q->net;
    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;

    if (httpServerConn(conn)) {
        hostname = rx->hostHeader;
        if (schr(rx->hostHeader, ':')) {
            mprParseSocketAddress(rx->hostHeader, &hostname, NULL, NULL, 0);
        }
        if ((conn->host = httpMatchHost(net, hostname)) == 0) {
            conn->host = mprGetFirstItem(net->endpoint->hosts);
            httpError(conn, HTTP_CODE_NOT_FOUND, "No listening endpoint for request for %s", rx->hostHeader);
        }
        if (!rx->originalUri) {
            rx->originalUri = rx->uri;
        }
        parseUri(conn);
    }

    if (rx->form && rx->length >= conn->limits->rxFormSize && conn->limits->rxFormSize != HTTP_UNLIMITED) {
        httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
            "Request form of %lld bytes is too big. Limit %lld", rx->length, conn->limits->rxFormSize);
    }
    if (conn->error) {
        /* Cannot reliably continue with keep-alive as the headers have not been correctly parsed */
        conn->keepAliveCount = 0;
    }
    if (httpClientConn(conn) && conn->keepAliveCount <= 0 && rx->length < 0 && rx->chunkState == HTTP_CHUNK_UNCHUNKED) {
        /*
            Google does responses with a body and without a Content-Length like this:
                Connection: close
                Location: URI
         */
        rx->remainingContent = rx->redirect ? 0 : HTTP_UNLIMITED;
    }

    if (httpIsServer(conn->net)) {
        //  TODO is rx->length getting set for HTTP/2?
        if (!rx->upload && rx->length >= conn->limits->rxBodySize && conn->limits->rxBodySize != HTTP_UNLIMITED) {
            httpLimitError(conn, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
                "Request content length %lld bytes is too big. Limit %lld", rx->length, conn->limits->rxBodySize);
            return;
        }
        httpAddQueryParams(conn);
        rx->streaming = httpGetStreaming(conn->host, rx->mimeType, rx->uri);
        if (rx->streaming && httpServerConn(conn)) {
            /*
                Disable upload if streaming, used by PHP to stream input and process file upload in PHP.
             */
            rx->upload = 0;
            routeRequest(conn);
        }
    } else {
#if ME_HTTP_WEB_SOCKETS
        if (conn->upgraded && !httpVerifyWebSocketsHandshake(conn)) {
            httpSetState(conn, HTTP_STATE_FINALIZED);
            return;
        }
#endif
    }
    httpSetState(conn, HTTP_STATE_CONTENT);
    if (rx->remainingContent == 0) {
        httpSetEof(conn);
        httpFinalizeInput(conn);
    }
}


static void routeRequest(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    httpRouteRequest(conn);
    httpCreatePipeline(conn);
    httpStartPipeline(conn);
    httpStartHandler(conn);
}


static bool pumpOutput(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    HttpQueue   *wq;
    ssize       count;

    conn = q->conn;
    tx = conn->tx;

    if (tx->started && !conn->net->writeBlocked) {
        wq = conn->writeq;
        count = wq->count;
        if (!tx->finalizedOutput) {
            HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
            if (tx->handler->writable) {
                tx->handler->writable(wq);
            }
        }
        return (wq->count - count) ? 0 : 1;
    }
    return 0;
}


static bool processContent(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpPacket  *packet;

    conn = q->conn;
    rx = conn->rx;

    if (rx->eof) {
        if (httpServerConn(conn)) {
            if (httpAddBodyParams(conn) < 0) {
                httpError(conn, HTTP_CODE_BAD_REQUEST, "Bad request parameters");
                return 1;
            }
            mapMethod(conn);
            if (!rx->route) {
                routeRequest(conn);
            }
            while ((packet = httpGetPacket(conn->rxHead)) != 0) {
                httpPutPacket(conn->readq, packet);
            }
        }
        httpSetState(conn, HTTP_STATE_READY);
    }
    if (rx->eof || !httpServerConn(conn)) {
        if (conn->readq->first) {
            HTTP_NOTIFY(conn, HTTP_EVENT_READABLE, 0);
        }
    }
    return pumpOutput(q) || rx->eof;
}


/*
    In the ready state after all content has been received
 */
static void processReady(HttpQueue *q)
{
    HttpConn    *conn;

    conn = q->conn;
    httpReadyHandler(conn);
    httpSetState(conn, HTTP_STATE_RUNNING);
    if (httpClientConn(conn) && !conn->upgraded) {
        httpFinalize(conn);
    }
}


static bool processRunning(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    if (tx->finalized && tx->finalizedConnector) {
        httpSetState(conn, HTTP_STATE_FINALIZED);
        return 1;
    }
    return pumpOutput(q);
}


static void processFinalized(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;

    tx->finalized = 1;
    tx->finalizedOutput = 1;
    tx->finalizedInput = 1;

#if ME_TRACE_MEM
    mprDebug(1, "Request complete, status %d, error %d, connError %d, %s%s, memsize %.2f MB",
        tx->status, conn->error, conn->net->error, rx->hostHeader, rx->uri, mprGetMem() / 1024 / 1024.0);
#endif
    if (httpServerConn(conn) && rx) {
        httpMonitorEvent(conn, HTTP_COUNTER_NETWORK_IO, tx->bytesWritten);
    }
    if (httpServerConn(conn) && conn->activeRequest) {
        httpMonitorEvent(conn, HTTP_COUNTER_ACTIVE_REQUESTS, -1);
        conn->activeRequest = 0;
    }
    measureRequest(q);

    if (rx->session) {
        httpWriteSession(conn);
    }
    httpClosePipeline(conn);

    if (conn->net->eof) {
        if (!conn->errorMsg) {
            conn->errorMsg = conn->sock->errorMsg ? conn->sock->errorMsg : sclone("Server close");
        }
        httpTrace(conn->trace, "http.connection.close", "network", "msg:'%s'", conn->errorMsg);
    }
    if (tx->errorDocument && !smatch(tx->errorDocument, rx->uri)) {
        prepErrorDoc(q);
    } else {
        conn->complete = 1;
        httpSetState(conn, HTTP_STATE_COMPLETE);
    }
}


static bool processCompletion(HttpQueue *q)
{
    HttpConn    *conn;

    conn = q->conn;
    if (conn->http->requestCallback) {
        (conn->http->requestCallback)(conn);
    }
    return 0;
}


static void measureRequest(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;
    MprTicks    elapsed;
    MprOff      received;
    int         status;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;

    elapsed = mprGetTicks() - conn->started;
    if (httpTracing(q->net)) {
        status = httpServerConn(conn) ? tx->status : rx->status;
        received = httpGetPacketLength(rx->headerPacket) + rx->bytesRead;
#if MPR_HIGH_RES_TIMER
        httpTraceData(conn->trace,
            "http.completion", "result", 0, (void*) conn, 0, "status:%d, error:%d, elapsed:%llu, ticks:%llu, received:%lld, sent:%lld",
            status, conn->error, elapsed, mprGetHiResTicks() - conn->startMark, received, tx->bytesWritten);
#else
        httpTraceData(conn->trace, "http.completion", "result", 0, (void*) conn, 0, "status:%d, error:%d, elapsed:%llu, received:%lld, sent:%lld",
            status, conn->error, elapsed, received, tx->bytesWritten);
#endif
    }
}


static void prepErrorDoc(HttpQueue *q)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpTx      *tx;

    conn = q->conn;
    rx = conn->rx;
    tx = conn->tx;
    if (!rx->headerPacket || conn->errorDoc) {
        return;
    }
    httpTrace(conn->trace, "http.errordoc", "context", "location:'%s', status:%d", tx->errorDocument, tx->status);

    httpClosePipeline(conn);
    httpDiscardData(conn, HTTP_QUEUE_RX);
    httpDiscardData(conn, HTTP_QUEUE_TX);

    conn->rx = httpCreateRx(conn);
    conn->tx = httpCreateTx(conn, NULL);

    conn->rx->headers = rx->headers;
    conn->rx->method = rx->method;
    conn->rx->originalUri = rx->uri;
    conn->rx->uri = (char*) tx->errorDocument;
    conn->tx->status = tx->status;
    rx->conn = tx->conn = 0;

    conn->error = 0;
    conn->errorMsg = 0;
    conn->upgraded = 0;
    conn->state = HTTP_STATE_PARSED;
    conn->errorDoc = 1;
    conn->keepAliveCount = 0;

    parseUri(conn);
    routeRequest(conn);
}


PUBLIC void httpSetMethod(HttpConn *conn, cchar *method)
{
    conn->rx->method = sclone(method);
    httpParseMethod(conn);
}


static bool mapMethod(HttpConn *conn)
{
    HttpRx      *rx;
    cchar       *method;

    rx = conn->rx;
    if (rx->flags & HTTP_POST && (method = httpGetParam(conn, "-http-method-", 0)) != 0) {
        if (!scaselessmatch(method, rx->method)) {
            httpTrace(conn->trace, "http.mapMethod", "context", "originalMethod:'%s', method:'%s'", rx->method, method);
            httpSetMethod(conn, method);
            return 1;
        }
    }
    return 0;
}


PUBLIC ssize httpGetReadCount(HttpConn *conn)
{
    return conn->readq->count;
}


PUBLIC cchar *httpGetBodyInput(HttpConn *conn)
{
    HttpQueue   *q;
    HttpRx      *rx;
    MprBuf      *content;

    rx = conn->rx;
    if (!conn->rx->eof) {
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
        if ((range = httpCreateRange(conn, 0, 0)) == 0) {
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
            Complete the URI based on the connection state. Must have a complete scheme, host, port and path.
         */
        up = rx->parsedUri;
        up->scheme = sclone(conn->secure ? "https" : "http");
        hostname = rx->hostHeader ? rx->hostHeader : conn->host->name;
        if (!hostname) {
            hostname = conn->sock->acceptIp;
        }
        if (mprParseSocketAddress(hostname, &up->host, NULL, NULL, 0) < 0 || up->host == 0 || *up->host == '\0') {
            if (!conn->error) {
                httpBadRequestError(conn, HTTP_CODE_BAD_REQUEST, "Bad host");
            }
        } else {
            up->port = conn->sock->listenSock->port;
        }
    }
}


/*
    Sends an 100 Continue response to the client. This bypasses the transmission pipeline, writing directly to the socket.
 */
static int sendContinue(HttpQueue *q)
{
    HttpConn    *conn;
    cchar       *response;
    int         mode;

    conn = q->conn;
    assert(conn);

    if (!conn->tx->finalized && !conn->tx->bytesWritten) {
        response = sfmt("%s 100 Continue\r\n\r\n", httpGetProtocol(conn->net));
        mode = mprGetSocketBlockingMode(conn->sock);
        mprWriteSocket(conn->sock, response, slen(response));
        mprSetSocketBlockingMode(conn->sock, mode);
        mprFlushSocket(conn->sock);
    }
    return 0;
}


PUBLIC cchar *httpTraceHeaders(HttpQueue *q, MprHash *headers)
{
    MprBuf  *buf;
    MprKey  *kp;

    buf = mprCreateBuf(0, 0);
    for (ITERATE_KEYS(headers, kp)) {
        if (*kp->key == '=') {
            mprPutToBuf(buf, ":%s: %s\n", &kp->key[1], kp->data);
        } else {
            mprPutToBuf(buf, "%s: %s\n", kp->key, kp->data);
        }
    }
    mprAddNullToBuf(buf);
    return mprGetBufStart(buf);
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
