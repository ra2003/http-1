/*
    tx.c - Http transmitter for server responses and client requests.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/***************************** Forward Declarations ***************************/

static void manageTx(HttpTx *tx, int flags);

/*********************************** Code *************************************/

PUBLIC HttpTx *httpCreateTx(HttpConn *conn, MprHash *headers)
{
    Http        *http;
    HttpNet     *net;
    HttpTx      *tx;

    assert(conn);
    assert(conn->net);

    if ((tx = mprAllocObj(HttpTx, manageTx)) == 0) {
        return 0;
    }
    http = conn->http;
    net = conn->net;
    conn->tx = tx;
    tx->conn = conn;
    tx->status = HTTP_CODE_OK;
    tx->length = -1;
    tx->entityLength = -1;
    tx->chunkSize = -1;
    tx->cookies = mprCreateHash(HTTP_SMALL_HASH_SIZE, 0);

    if (headers) {
        tx->headers = headers;
    } else {
        tx->headers = mprCreateHash(HTTP_SMALL_HASH_SIZE, MPR_HASH_CASELESS | MPR_HASH_STABLE);
        if (httpClientConn(conn)) {
            httpAddHeaderString(conn, "User-Agent", sclone(ME_HTTP_SOFTWARE));
        }
    }
    return tx;
}


PUBLIC void httpDestroyTx(HttpTx *tx)
{
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
    if (tx->conn) {
        tx->conn->tx = 0;
        tx->conn = 0;
    }
}


static void manageTx(HttpTx *tx, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(tx->altBody);
        mprMark(tx->cache);
        mprMark(tx->cacheBuffer);
        mprMark(tx->cachedContent);
        mprMark(tx->conn);
        mprMark(tx->connector);
        mprMark(tx->cookies);
        mprMark(tx->currentRange);
        mprMark(tx->ext);
        mprMark(tx->etag);
        mprMark(tx->errorDocument);
        mprMark(tx->file);
        mprMark(tx->filename);
        mprMark(tx->handler);
        mprMark(tx->headers);
        mprMark(tx->method);
        mprMark(tx->mimeType);
        mprMark(tx->outputPipeline);
        mprMark(tx->outputRanges);
        mprMark(tx->parsedUri);
        mprMark(tx->rangeBoundary);
        mprMark(tx->webSockKey);
    }
}


/*
    Add key/value to the header hash. If already present, update the value
*/
static void updateHdr(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    if (schr(value, '$')) {
        value = httpExpandVars(conn, value);
    }
    mprAddKey(conn->tx->headers, key, value);
}


PUBLIC int httpRemoveHeader(HttpConn *conn, cchar *key)
{
    assert(key && *key);
    if (conn->tx == 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    return mprRemoveKey(conn->tx->headers, key);
}


/*
    Add a http header if not already defined
 */
PUBLIC void httpAddHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    char        *value;
    va_list     vargs;

    assert(key && *key);
    assert(fmt && *fmt);

    if (fmt) {
        va_start(vargs, fmt);
        value = sfmtv(fmt, vargs);
        va_end(vargs);
    } else {
        value = MPR->emptyString;
    }
    if (conn->tx && !mprLookupKey(conn->tx->headers, key)) {
        updateHdr(conn, key, value);
    }
}


/*
    Add a header string if not already defined
 */
PUBLIC void httpAddHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    if (conn->tx && !mprLookupKey(conn->tx->headers, key)) {
        updateHdr(conn, key, sclone(value));
    }
}


/*
   Append a header. If already defined, the value is catenated to the pre-existing value after a ", " separator.
   As per the HTTP/1.1 spec. Except for Set-Cookie which HTTP permits multiple headers but not of the same cookie. Ugh!
 */
PUBLIC void httpAppendHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    va_list     vargs;
    MprKey      *kp;
    char        *value;
    cchar       *cookie;

    if (!conn->tx) {
        return;
    }
    assert(key && *key);
    assert(fmt && *fmt);

    va_start(vargs, fmt);
    value = sfmtv(fmt, vargs);
    va_end(vargs);

    /*
        HTTP permits Set-Cookie to have multiple cookies. Other headers must comma separate multiple values.
        For Set-Cookie, must allow duplicates but not of the same cookie.
     */
    kp = mprLookupKeyEntry(conn->tx->headers, key);
    if (kp) {
        if (scaselessmatch(key, "Set-Cookie")) {
            cookie = stok(sclone(value), "=", NULL);
            while (kp) {
                if (scaselessmatch(kp->key, "Set-Cookie")) {
                    if (sstarts(kp->data, cookie)) {
                        kp->data = value;
                        break;
                    }
                }
                kp = kp->next;
            }
            if (!kp) {
                mprAddDuplicateKey(conn->tx->headers, key, value);
            }
        } else {
            updateHdr(conn, key, sfmt("%s, %s", (char*) kp->data, value));
        }
    } else {
        updateHdr(conn, key, value);
    }
}


/*
   Append a header string. If already defined, the value is catenated to the pre-existing value after a ", " separator.
   As per the HTTP/1.1 spec.
 */
PUBLIC void httpAppendHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    cchar   *oldValue;

    assert(key && *key);
    assert(value && *value);

    if (!conn->tx) {
        return;
    }
    oldValue = mprLookupKey(conn->tx->headers, key);
    if (oldValue) {
        if (scaselessmatch(key, "Set-Cookie")) {
            mprAddDuplicateKey(conn->tx->headers, key, sclone(value));
        } else {
            updateHdr(conn, key, sfmt("%s, %s", oldValue, value));
        }
    } else {
        updateHdr(conn, key, sclone(value));
    }
}


PUBLIC cchar *httpGetTxHeader(HttpConn *conn, cchar *key)
{
    if (conn->rx == 0) {
        assert(conn->rx);
        return 0;
    }
    return mprLookupKey(conn->tx->headers, key);
}


/*
    Set a http header. Overwrite if present.
 */
PUBLIC void httpSetHeader(HttpConn *conn, cchar *key, cchar *fmt, ...)
{
    char        *value;
    va_list     vargs;

    assert(key && *key);
    assert(fmt && *fmt);

    va_start(vargs, fmt);
    value = sfmtv(fmt, vargs);
    va_end(vargs);
    updateHdr(conn, key, value);
}


PUBLIC void httpSetHeaderString(HttpConn *conn, cchar *key, cchar *value)
{
    assert(key && *key);
    assert(value);

    updateHdr(conn, key, sclone(value));
}


/*
    Called by connectors (ONLY) when writing the entire output transmission is complete
 */
PUBLIC void httpFinalizeConnector(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    tx->finalizedConnector = 1;
    tx->finalizedOutput = 1;
}


/*
    Finalize the request. This means the caller is totally completed with the request. They have sent all
    output and have read all input. Further input can be discarded. Note that output may not yet have drained from
    the socket and so the connection state will not be transitioned to FINALIIZED until that happens and all
    remaining input has been dealt with.
 */
PUBLIC void httpFinalize(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx || tx->finalized) {
        return;
    }
    if (conn->rx->session) {
        httpWriteSession(conn);
    }
    httpFinalizeInput(conn);
    httpFinalizeOutput(conn);
    tx->finalized = 1;
}


/*
    The handler has generated the entire transmit body. Note: the data may not yet have drained from
    the pipeline or socket and the caller may not have read all the input body content.
 */
PUBLIC void httpFinalizeOutput(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx || tx->finalizedOutput) {
        return;
    }
    tx->responded = 1;
    tx->finalizedOutput = 1;
    if (!(tx->flags & HTTP_TX_PIPELINE)) {
        /* Tx Pipeline not yet created */
        tx->pendingFinalize = 1;
        return;
    }
    if (tx->finalizedInput) {
        httpFinalize(conn);
    }
    httpPutPacket(conn->writeq, httpCreateEndPacket());
}


/*
    This means the handler has processed all the input
 */
PUBLIC void httpFinalizeInput(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (tx && !tx->finalizedInput) {
        tx->finalizedInput = 1;
        if (tx->finalizedOutput) {
            httpFinalize(conn);
        }
    }
}


PUBLIC int httpIsFinalized(HttpConn *conn)
{
    return conn->tx->finalized;
}


PUBLIC int httpIsOutputFinalized(HttpConn *conn)
{
    return conn->tx->finalizedOutput;
}


PUBLIC int httpIsInputFinalized(HttpConn *conn)
{
    return conn->tx->finalizedInput;
}


/*
    This formats a response and sets the altBody. The response is not HTML escaped.
    This is the lowest level for formatResponse.
 */
PUBLIC ssize httpFormatResponsev(HttpConn *conn, cchar *fmt, va_list args)
{
    HttpTx      *tx;
    cchar       *body;

    tx = conn->tx;
    tx->responded = 1;
    body = fmt ? sfmtv(fmt, args) : conn->errorMsg;
    tx->altBody = body;
    tx->length = slen(tx->altBody);
    tx->flags |= HTTP_TX_NO_BODY;
    httpDiscardData(conn, HTTP_QUEUE_TX);
    return (ssize) tx->length;
}


/*
    This formats a response and sets the altBody. The response is not HTML escaped.
 */
PUBLIC ssize httpFormatResponse(HttpConn *conn, cchar *fmt, ...)
{
    va_list     args;
    ssize       rc;

    va_start(args, fmt);
    rc = httpFormatResponsev(conn, fmt, args);
    va_end(args);
    return rc;
}


/*
    This formats a complete response. Depending on the Accept header, the response will be either HTML or plain text.
    The response is not HTML escaped. This calls httpFormatResponse.
 */
PUBLIC ssize httpFormatResponseBody(HttpConn *conn, cchar *title, cchar *fmt, ...)
{
    va_list     args;
    cchar       *msg, *body;

    va_start(args, fmt);
    body = fmt ? sfmtv(fmt, args) : conn->errorMsg;

    if (scmp(conn->rx->accept, "text/plain") == 0) {
        msg = body;
    } else {
        msg = sfmt(
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body>\r\n%s\r\n</body>\r\n</html>\r\n",
            title, body);
    }
    va_end(args);
    return httpFormatResponse(conn, "%s", msg);
}


PUBLIC void *httpGetQueueData(HttpConn *conn)
{
    HttpQueue     *q;

    q = conn->writeq;
    return q->nextQ->queueData;
}


PUBLIC void httpOmitBody(HttpConn *conn)
{
    HttpTx  *tx;

    tx = conn->tx;
    if (tx && !(tx->flags & HTTP_TX_HEADERS_CREATED)) {
        tx->flags |= HTTP_TX_NO_BODY;
        tx->length = -1;
        httpDiscardData(conn, HTTP_QUEUE_TX);
    }
}


/*
    Redirect the user to another URI. The targetUri may or may not have a scheme or hostname.
 */
PUBLIC void httpRedirect(HttpConn *conn, int status, cchar *targetUri)
{
    HttpTx          *tx;
    HttpRx          *rx;
    HttpUri         *base, *canonical;
    cchar           *msg;

    assert(targetUri);
    rx = conn->rx;
    tx = conn->tx;

    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        mprLog("error", 0, "Headers already created, so redirect ignored: %s", targetUri);
        return;
    }
    tx->status = status;
    msg = httpLookupStatus(status);

    canonical = conn->host->canonical;
    if (canonical) {
        base = httpCloneUri(rx->parsedUri, 0);
        if (canonical->host) {
            base->host = canonical->host;
        }
        if (canonical->port) {
            base->port = canonical->port;
        }
    } else {
        base = rx->parsedUri;
    }
    /*
        Expand the target for embedded tokens. Resolve relative to the current request URI.
     */
    targetUri = httpUriToString(httpResolveUri(conn, base, httpLinkUri(conn, targetUri, 0)), 0);

    if (300 <= status && status <= 399) {
        httpSetHeader(conn, "Location", "%s", targetUri);
        httpFormatResponse(conn,
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body><h1>%s</h1>\r\n<p>The document has moved <a href=\"%s\">here</a>.</p></body></html>\r\n",
            msg, msg, targetUri);
        httpTrace(conn->trace, "http.redirect", "context", "status:%d,location:'%s'", status, targetUri);
    } else {
        httpFormatResponse(conn,
            "<!DOCTYPE html>\r\n"
            "<html><head><title>%s</title></head>\r\n"
            "<body><h1>%s</h1>\r\n</body></html>\r\n",
            msg, msg);
    }
    httpFinalize(conn);
    tx->handler = conn->http->passHandler;
}


PUBLIC void httpSetContentLength(HttpConn *conn, MprOff length)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->length = length;
}


/*
    Set lifespan < 0 to delete the cookie in the client.
    Set lifespan == 0 for no expiry.
    WARNING: Some browsers (Chrome, Firefox) do not delete session cookies when you exit the browser.
 */
PUBLIC void httpSetCookie(HttpConn *conn, cchar *name, cchar *value, cchar *path, cchar *cookieDomain,
    MprTicks lifespan, int flags)
{
    HttpRx      *rx;
    cchar       *domain, *domainAtt;
    char        *cp, *expiresAtt, *expires, *secure, *httpOnly, *sameSite;

    rx = conn->rx;
    if (path == 0) {
        path = "/";
    }
    /*
        Note: Cookies do not respect port numbers, so we ignore them here.
        Note: Modern browsers will give subdomains the cookies defined for a top-level domain.
        Note: A leading dot in the top-level domain is not required anymore.
        Note: Browsers may store top-level domain cookies with a leading dot in their cooke store (chrome).
     */
    domain = 0;
    if (cookieDomain) {
        /*
            Omit domain if set to empty string
        */
        if (*cookieDomain) {
            domain = (char*) cookieDomain;
        }
    } else if (rx->hostHeader) {
        if (mprParseSocketAddress(rx->hostHeader, &domain, NULL, NULL, 0) < 0) {
            mprLog("error http", 4, "Bad host header for cookie: %s", rx->hostHeader);
            return;
        }
    }
    domainAtt = domain ? "; domain=" : "";
    if (domain) {
        /*
            Domains must have at least one dot, so we prefix with a dot here if one is not present.
         */
        if (!strchr(domain, '.')) {
            if (smatch(domain, "localhost")) {
                domainAtt = domain = "";
            } else {
                domain = sjoin(".", domain, NULL);
            }
        }
    } else {
        domain = "";
    }
    if (lifespan) {
        expiresAtt = "; expires=";
        expires = mprFormatUniversalTime(MPR_HTTP_DATE, mprGetTime() + lifespan);
    } else {
        expires = expiresAtt = "";
    }
    secure = (conn->secure & (flags & HTTP_COOKIE_SECURE)) ? "; secure" : "";
    httpOnly = (flags & HTTP_COOKIE_HTTP) ?  "; httponly" : "";
    sameSite = "";
    if (flags & HTTP_COOKIE_SAME_LAX) {
        sameSite = "; SameSite=Lax";
    } else if (flags & HTTP_COOKIE_SAME_STRICT) {
        sameSite = "; SameSite=Strict";
    }
    mprAddKey(conn->tx->cookies, name,
        sjoin(value, "; path=", path, domainAtt, domain, expiresAtt, expires, secure, httpOnly, sameSite, NULL));

    if ((cp = mprLookupKey(conn->tx->headers, "Cache-Control")) == 0 || !scontains(cp, "no-cache")) {
        httpAppendHeader(conn, "Cache-Control", "no-cache=\"set-cookie\"");
    }
}


PUBLIC void httpRemoveCookie(HttpConn *conn, cchar *name)
{
    HttpRoute   *route;
    cchar       *cookie, *url;

    route = conn->rx->route;
    url = (route->prefix && *route->prefix) ? route->prefix : "/";
    cookie = route->cookie ? route->cookie : HTTP_SESSION_COOKIE;
    httpSetCookie(conn, cookie, "", url, NULL, 1, 0);
}


static void setCorsHeaders(HttpConn *conn)
{
    HttpRoute   *route;
    cchar       *origin;

    route = conn->rx->route;

    /*
        Cannot use wildcard origin response if allowing credentials
     */
    if (*route->corsOrigin && !route->corsCredentials) {
        httpSetHeaderString(conn, "Access-Control-Allow-Origin", route->corsOrigin);
    } else {
        origin = httpGetHeader(conn, "Origin");
        httpSetHeaderString(conn, "Access-Control-Allow-Origin", origin ? origin : "*");
    }
    if (route->corsCredentials) {
        httpSetHeaderString(conn, "Access-Control-Allow-Credentials", "true");
    }
    if (route->corsHeaders) {
        httpSetHeaderString(conn, "Access-Control-Allow-Headers", route->corsHeaders);
    }
    if (route->corsMethods) {
        httpSetHeaderString(conn, "Access-Control-Allow-Methods", route->corsMethods);
    }
    if (route->corsAge) {
        httpSetHeader(conn, "Access-Control-Max-Age", "%d", route->corsAge);
    }
}


PUBLIC HttpPacket *httpCreateHeaders(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;

    conn = q->conn;

    if (!packet) {
        packet = httpCreateHeaderPacket();
        packet->conn = q->conn;
    }
#if ME_HTTP_HTTP2
    if (q->net->protocol >= 2) {
        httpCreateHeaders2(q, packet);
    } else {
        httpCreateHeaders1(q, packet);
    }
#else
    httpCreateHeaders1(q, packet);
#endif
    return packet;
}


/*
    Define headers for httpWriteHeaders. This defines standard headers.
 */
PUBLIC void httpPrepareHeaders(HttpConn *conn)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpRange   *range;
    MprKeyValue *item;
    MprKey      *kp;
    MprOff      length;
    int         next;

    rx = conn->rx;
    tx = conn->tx;
    route = rx->route;

    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->flags |= HTTP_TX_HEADERS_CREATED;

    if (conn->headersCallback) {
        /* Must be before headers below */
        (conn->headersCallback)(conn->headersCallbackArg);
    }

    /*
        Create headers for cookies
     */
    for (ITERATE_KEYS(tx->cookies, kp)) {
        httpAppendHeaderString(conn, "Set-Cookie", sjoin(kp->key, "=", kp->data, NULL));
    }

    /*
        Mandatory headers that must be defined here use httpSetHeader which overwrites existing values.
     */
    httpAddHeaderString(conn, "Date", conn->http->currentDate);

    if (tx->ext && route) {
        if (conn->error) {
            tx->mimeType = sclone("text/html");
        } else if ((tx->mimeType = (char*) mprLookupMime(route->mimeTypes, tx->ext)) == 0) {
            tx->mimeType = sclone("text/html");
        }
        httpAddHeaderString(conn, "Content-Type", tx->mimeType);
    }
    if (tx->etag) {
        httpAddHeader(conn, "ETag", "%s", tx->etag);
    }
    length = tx->length > 0 ? tx->length : 0;
    if (rx->flags & HTTP_HEAD) {
        conn->tx->flags |= HTTP_TX_NO_BODY;
        httpDiscardData(conn, HTTP_QUEUE_TX);
        if (tx->chunkSize <= 0) {
            httpAddHeader(conn, "Content-Length", "%lld", length);
        }

    } else if (tx->chunkSize > 0) {
        httpSetHeaderString(conn, "Transfer-Encoding", "chunked");

    } else if (httpServerConn(conn)) {
        /* Server must not emit a content length header for 1XX, 204 and 304 status */
        if (!((100 <= tx->status && tx->status <= 199) || tx->status == 204 || tx->status == 304 || tx->flags & HTTP_TX_NO_LENGTH)) {
            if (length > 0 || (length == 0 && conn->net->protocol < 2)) {
                httpAddHeader(conn, "Content-Length", "%lld", length);
            }
        }

    } else if (tx->length > 0) {
        /* client with body */
        httpAddHeader(conn, "Content-Length", "%lld", length);
    }
    if (tx->outputRanges) {
        if (tx->outputRanges->next == 0) {
            range = tx->outputRanges;
            if (tx->entityLength > 0) {
                httpSetHeader(conn, "Content-Range", "bytes %lld-%lld/%lld", range->start, range->end - 1, tx->entityLength);
            } else {
                httpSetHeader(conn, "Content-Range", "bytes %lld-%lld/*", range->start, range->end - 1);
            }
        } else {
            tx->mimeType = sfmt("multipart/byteranges; boundary=%s", tx->rangeBoundary);
            httpSetHeaderString(conn, "Content-Type", tx->mimeType);
        }
        httpSetHeader(conn, "Accept-Ranges", "bytes");
    }
    if (httpServerConn(conn)) {
        if (!(route->flags & HTTP_ROUTE_STEALTH)) {
            httpAddHeaderString(conn, "Server", conn->http->software);
        }
        if (conn->net->protocol < 2) {
            /*
                If keepAliveCount == 1
             */
            if (--conn->keepAliveCount > 0) {
                assert(conn->keepAliveCount >= 1);
                httpAddHeaderString(conn, "Connection", "Keep-Alive");
                if (!(route->flags & HTTP_ROUTE_STEALTH) || 1) {
                    httpAddHeader(conn, "Keep-Alive", "timeout=%lld, max=%d", conn->limits->inactivityTimeout / 1000,
                        conn->keepAliveCount);
                }
            } else {
                /* Tell the peer to close the connection */
                httpAddHeaderString(conn, "Connection", "close");
            }
        }
        if (route->flags & HTTP_ROUTE_CORS) {
            setCorsHeaders(conn);
        }
        /*
            Apply route headers
         */
        for (ITERATE_ITEMS(route->headers, item, next)) {
            if (item->flags == HTTP_ROUTE_ADD_HEADER) {
                httpAddHeaderString(conn, item->key, item->value);
            } else if (item->flags == HTTP_ROUTE_APPEND_HEADER) {
                httpAppendHeaderString(conn, item->key, item->value);
            } else if (item->flags == HTTP_ROUTE_REMOVE_HEADER) {
                httpRemoveHeader(conn, item->key);
            } else if (item->flags == HTTP_ROUTE_SET_HEADER) {
                httpSetHeaderString(conn, item->key, item->value);
            }
        }
    }
}


#if UNUSED
PUBLIC void httpSetEntityLength(HttpConn *conn, int64 len)
{
    HttpTx      *tx;

    tx = conn->tx;
    tx->entityLength = len;
    if (tx->outputRanges == 0) {
        tx->length = len;
    }
}
#endif


/*
    Low level routine to set the filename to serve. The filename may be outside the route documents, so caller
    must take care if the HTTP_TX_NO_CHECK flag is used.  This will update HttpTx.ext and HttpTx.fileInfo.
    This does not implement per-language directories. For that, see httpMapFile.
 */
PUBLIC bool httpSetFilename(HttpConn *conn, cchar *filename, int flags)
{
    HttpTx      *tx;
    MprPath     *info;

    assert(conn);

    tx = conn->tx;
    info = &tx->fileInfo;
    tx->flags &= ~(HTTP_TX_NO_CHECK | HTTP_TX_NO_MAP);
    tx->flags |= (flags & (HTTP_TX_NO_CHECK | HTTP_TX_NO_MAP));

    if (filename == 0) {
        tx->filename = 0;
        tx->ext = 0;
        info->checked = info->valid = 0;
        return 0;
    }
#if !ME_ROM
    if (!(tx->flags & HTTP_TX_NO_CHECK)) {
        if (!mprIsAbsPathContained(filename, conn->rx->route->documents)) {
            info->checked = 1;
            info->valid = 0;
            httpError(conn, HTTP_CODE_BAD_REQUEST, "Filename outside published documents");
            return 0;
        }
    }
#endif
    if (!tx->ext || tx->ext[0] == '\0') {
        tx->ext = httpGetPathExt(filename);
    }
    mprGetPathInfo(filename, info);
    if (info->valid) {
        tx->etag = itos(info->inode + info->size + info->mtime);
    }
    tx->filename = sclone(filename);

    if (tx->flags & HTTP_TX_PIPELINE) {
        /* Filename being revised after pipeline created */
        httpTrace(conn->trace, "http.document", "context", "filename:'%s'", tx->filename);
    }
    return info->valid;
}


PUBLIC void httpSetResponded(HttpConn *conn)
{
    conn->tx->responded = 1;
}


PUBLIC void httpSetStatus(HttpConn *conn, int status)
{
    conn->tx->status = status;
    conn->tx->responded = 1;
}


PUBLIC void httpSetContentType(HttpConn *conn, cchar *mimeType)
{
    conn->tx->mimeType = sclone(mimeType);
    httpSetHeaderString(conn, "Content-Type", conn->tx->mimeType);
}



PUBLIC bool httpFileExists(HttpConn *conn)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (!tx->fileInfo.checked) {
        mprGetPathInfo(tx->filename, &tx->fileInfo);
    }
    return tx->fileInfo.valid;
}


/*
    Write a block of data. This is the lowest level write routine for data. This will buffer the data and flush if
    the queue buffer is full. Flushing is done by calling httpFlushQueue which will service queues as required.
 */
PUBLIC ssize httpWriteBlock(HttpQueue *q, cchar *buf, ssize len, int flags)
{
    HttpPacket  *packet;
    HttpConn    *conn;
    HttpTx      *tx;
    ssize       totalWritten, packetSize, thisWrite;

    assert(q == q->conn->writeq);
    conn = q->conn;
    tx = conn->tx;

    if (tx == 0 || tx->finalizedOutput) {
        return MPR_ERR_CANT_WRITE;
    }
    if (flags == 0) {
        flags = HTTP_BUFFER;
    }
    tx->responded = 1;

    for (totalWritten = 0; len > 0; ) {
        if (conn->state >= HTTP_STATE_FINALIZED || conn->net->error) {
            return MPR_ERR_CANT_WRITE;
        }
        if (q->last && (q->last != q->first) && (q->last->flags & HTTP_PACKET_DATA) && mprGetBufSpace(q->last->content) > 0) {
            packet = q->last;
        } else {
            packetSize = (tx->chunkSize > 0) ? tx->chunkSize : q->packetSize;
            if ((packet = httpCreateDataPacket(packetSize)) == 0) {
                return MPR_ERR_MEMORY;
            }
            httpPutPacket(q, packet);
        }
        assert(mprGetBufSpace(packet->content) > 0);
        thisWrite = min(len, mprGetBufSpace(packet->content));
        if (flags & (HTTP_BLOCK | HTTP_NON_BLOCK)) {
            thisWrite = min(thisWrite, q->max - q->count);
        }
        if (thisWrite > 0) {
            if ((thisWrite = mprPutBlockToBuf(packet->content, buf, thisWrite)) == 0) {
                return MPR_ERR_MEMORY;
            }
            buf += thisWrite;
            len -= thisWrite;
            q->count += thisWrite;
            totalWritten += thisWrite;
        }
        if (q->count >= q->max) {
            httpFlushQueue(q, flags);
            if (q->count >= q->max && (flags & HTTP_NON_BLOCK)) {
                break;
            }
        }
    }
    if (conn->error) {
        return MPR_ERR_CANT_WRITE;
    }
    if (httpClientConn(conn)) {
        httpEnableNetEvents(conn->net);
    }
    return totalWritten;
}


PUBLIC ssize httpWriteString(HttpQueue *q, cchar *s)
{
    return httpWriteBlock(q, s, strlen(s), HTTP_BUFFER);
}


PUBLIC ssize httpWriteSafeString(HttpQueue *q, cchar *s)
{
    return httpWriteString(q, mprEscapeHtml(s));
}


PUBLIC ssize httpWrite(HttpQueue *q, cchar *fmt, ...)
{
    va_list     vargs;
    char        *buf;

    va_start(vargs, fmt);
    buf = sfmtv(fmt, vargs);
    va_end(vargs);
    return httpWriteString(q, buf);
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
