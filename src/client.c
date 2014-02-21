/*
    client.c -- Client side specific support.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************* Forwards ***********************************/

static void setDefaultHeaders(HttpConn *conn);

/*********************************** Code *************************************/

static HttpConn *openConnection(HttpConn *conn, struct MprSsl *ssl)
{
    Http        *http;
    HttpUri     *uri;
    MprSocket   *sp;
    char        *ip;
    int         port, rc, level;

    assert(conn);

    http = conn->http;
    uri = conn->tx->parsedUri;

    if (!uri->host) {
        ip = (http->proxyHost) ? http->proxyHost : http->defaultClientHost;
        port = (http->proxyHost) ? http->proxyPort : http->defaultClientPort;
    } else {
        ip = (http->proxyHost) ? http->proxyHost : uri->host;
        port = (http->proxyHost) ? http->proxyPort : uri->port;
    }
    if (port == 0) {
        port = (uri->secure) ? 443 : 80;
    }
    if (conn && conn->sock) {
        if (conn->keepAliveCount-- <= 0 || port != conn->port || strcmp(ip, conn->ip) != 0 || 
                uri->secure != (conn->sock->ssl != 0) || conn->sock->ssl != ssl) {
            mprCloseSocket(conn->sock, 0);
            conn->sock = 0;
        } else {
            mprLog(4, "Http: reusing keep-alive socket on: %s:%d", ip, port);
        }
    }
    if (conn->sock) {
        return conn;
    }
    if ((sp = mprCreateSocket()) == 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Cannot create socket for %s", uri->uri);
        return 0;
    }
    if ((rc = mprConnectSocket(sp, ip, port, MPR_SOCKET_NODELAY)) < 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Cannot open socket on %s:%d", ip, port);
        return 0;
    }
    conn->sock = sp;
    conn->ip = sclone(ip);
    conn->port = port;
    conn->secure = uri->secure;
    conn->keepAliveCount = (conn->limits->keepAliveMax) ? conn->limits->keepAliveMax : 0;

#if BIT_PACK_SSL
    /* Must be done even if using keep alive for repeat SSL requests */
    if (uri->secure) {
        char *peerName;
        if (ssl == 0) {
            ssl = mprCreateSsl(0);
        }
        peerName = isdigit(uri->host[0]) ? 0 : uri->host;
        if (mprUpgradeSocket(sp, ssl, peerName) < 0) {
            conn->errorMsg = sp->errorMsg;
            mprLog(2, "Cannot upgrade socket for SSL: %s", conn->errorMsg);
            return 0;
        }
    }
#endif
#if BIT_HTTP_WEB_SOCKETS
    if (uri->webSockets && httpUpgradeWebSocket(conn) < 0) {
        conn->errorMsg = sp->errorMsg;
        return 0;
    }
#endif
    if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_CONN, NULL)) >= 0) {
        mprLog(level, "### Outgoing connection to %s:%d", conn->ip, conn->port);
    }
    return conn;
}


static void setDefaultHeaders(HttpConn *conn)
{
    HttpAuthType    *ap;

    assert(conn);

    if (smatch(conn->protocol, "HTTP/1.0")) {
        conn->http10 = 1;
    }
    if (conn->username && conn->authType) {
        if ((ap = httpLookupAuthType(conn->authType)) != 0) {
            if ((ap->setAuth)(conn, conn->username, conn->password)) {
                conn->authRequested = 1;
            }
        }
    }
    if (conn->port != 80 && conn->port != 443) {
        httpAddHeader(conn, "Host", "%s:%d", conn->ip, conn->port);
    } else {
        httpAddHeaderString(conn, "Host", conn->ip);
    }
    httpAddHeaderString(conn, "Accept", "*/*");
    if (conn->keepAliveCount > 0) {
        httpSetHeaderString(conn, "Connection", "Keep-Alive");
    } else {
        httpSetHeaderString(conn, "Connection", "close");
    }
}


PUBLIC int httpConnect(HttpConn *conn, cchar *method, cchar *uri, struct MprSsl *ssl)
{
    assert(conn);
    assert(method && *method);
    assert(uri && *uri);

    if (httpServerConn(conn)) {
        httpError(conn, HTTP_CODE_BAD_GATEWAY, "Cannot call connect in a server");
        return MPR_ERR_BAD_STATE;
    }
    mprLog(4, "Http: client request: %s %s", method, uri);

    if (conn->tx == 0 || conn->state != HTTP_STATE_BEGIN) {
        /* WARNING: this will erase headers */
        httpPrepClientConn(conn, 0);
    }
    assert(conn->state == HTTP_STATE_BEGIN);
    conn->tx->parsedUri = httpCreateUri(uri, HTTP_COMPLETE_URI_PATH);

    if (openConnection(conn, ssl) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    conn->authRequested = 0;
    conn->tx->method = supper(method);
    conn->startMark = mprGetHiResTicks();
    /*
        The receive pipeline is created when parsing the response in parseIncoming()
     */
    httpCreateTxPipeline(conn, conn->http->clientRoute);
    httpSetState(conn, HTTP_STATE_CONNECTED);
    setDefaultHeaders(conn);
    return 0;
}


/*
    Check the response for authentication failures and redirections. Return true if a retry is requried.
 */
PUBLIC bool httpNeedRetry(HttpConn *conn, char **url)
{
    HttpRx          *rx;
    HttpTx          *tx;
    HttpAuthType    *authType;

    assert(conn->rx);

    *url = 0;
    rx = conn->rx;
    tx = conn->tx;

    if (conn->state < HTTP_STATE_FIRST) {
        return 0;
    }
    if (rx->status == HTTP_CODE_UNAUTHORIZED) {
        if (conn->username == 0 || conn->authType == 0) {
            httpError(conn, rx->status, "Authentication required");

        } else if (conn->authRequested && smatch(conn->authType, tx->authType)) {
            httpError(conn, rx->status, "Authentication failed");
        } else {
            assert(httpClientConn(conn));
            if (conn->authType && (authType = httpLookupAuthType(conn->authType)) != 0) {
                (authType->parseAuth)(conn, NULL, NULL);
            }
            return 1;
        }
    } else if (HTTP_CODE_MOVED_PERMANENTLY <= rx->status && rx->status <= HTTP_CODE_MOVED_TEMPORARILY && conn->followRedirects) {
        if (rx->redirect) {
            *url = rx->redirect;
            return 1;
        }
        httpError(conn, rx->status, "Missing location header");
        return -1;
    }
    return 0;
}


/*
    Set the request as being a multipart mime upload. This defines the content type and defines a multipart mime boundary
 */
PUBLIC void httpEnableUpload(HttpConn *conn)
{
    conn->boundary = sfmt("--BOUNDARY--%Ld", conn->http->now);
    httpSetHeader(conn, "Content-Type", "multipart/form-data; boundary=%s", &conn->boundary[2]);
}


/*
    Read data. If sync mode, this will block. If async, will never block.
    Will return what data is available up to the requested size. 
    Timeout in milliseconds to wait. Set to -1 to use the default inactivity timeout. Set to zero to wait forever.
    Returns a count of bytes read. Returns zero if no data. EOF if returns zero and conn->state is > HTTP_STATE_CONTENT.
 */
PUBLIC ssize httpReadBlock(HttpConn *conn, char *buf, ssize size, MprTicks timeout, int flags)
{
    HttpPacket  *packet;
    HttpQueue   *q;
    MprBuf      *content;
    ssize       nbytes, len;

    q = conn->readq;
    assert(q->count >= 0);
    assert(size >= 0);
    if (flags == 0) {
        flags = conn->async ? HTTP_NON_BLOCK : HTTP_BLOCK;
    }
    if (timeout <= 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    timeout = min(timeout, conn->limits->inactivityTimeout);

    if (flags & HTTP_BLOCK) {
        while (q->count <= 0 && !conn->error && (conn->state <= HTTP_STATE_CONTENT) && !httpRequestExpired(conn, timeout)) {
            httpEnableConnEvents(conn);
            assert(httpClientConn(conn));
            mprWaitForEvent(conn->dispatcher, timeout);
        }
    }
    for (nbytes = 0; size > 0 && q->count > 0; ) {
        if ((packet = q->first) == 0) {
            break;
        }
        content = packet->content;
        len = mprGetBufLength(content);
        len = min(len, size);
        assert(len <= q->count);
        if (len > 0) {
            len = mprGetBlockFromBuf(content, buf, len);
            assert(len <= q->count);
        }
        buf += len;
        size -= len;
        q->count -= len;
        assert(q->count >= 0);
        nbytes += len;
        if (mprGetBufLength(content) == 0) {
            httpGetPacket(q);
        }
        if (flags & HTTP_NON_BLOCK) {
            break;
        }
    }
    assert(q->count >= 0);
    if (nbytes < size) {
        buf[nbytes] = '\0';
    }
    return nbytes;
}


/*
    Read with standard connection timeouts and in blocking mode for clients, non-blocking for server-side
 */
PUBLIC ssize httpRead(HttpConn *conn, char *buf, ssize size)
{
    return httpReadBlock(conn, buf, size, -1, 0);
}


PUBLIC char *httpReadString(HttpConn *conn)
{
    HttpRx      *rx;
    ssize       sofar, nbytes, remaining;
    char        *content;

    rx = conn->rx;
    remaining = (ssize) min(MAXSSIZE, rx->length);

    if (remaining > 0) {
        if ((content = mprAlloc(remaining + 1)) == 0) {
            return 0;
        }
        sofar = 0;
        while (remaining > 0) {
            nbytes = httpRead(conn, &content[sofar], remaining);
            if (nbytes < 0) {
                return 0;
            }
            sofar += nbytes;
            remaining -= nbytes;
        }
    } else {
        content = mprAlloc(BIT_MAX_BUFFER);
        sofar = 0;
        while (1) {
            nbytes = httpRead(conn, &content[sofar], BIT_MAX_BUFFER);
            if (nbytes < 0) {
                return 0;
            } else if (nbytes == 0) {
                break;
            }
            sofar += nbytes;
            content = mprRealloc(content, sofar + BIT_MAX_BUFFER);
        }
    }
    content[sofar] = '\0';
    return content;
}


/*  
    Issue a client http request.
    Assumes the Mpr and Http services are created and initialized.
 */
PUBLIC HttpConn *httpRequest(cchar *method, cchar *uri, cchar *data, char **err)
{
    Http        *http;
    HttpConn    *conn;
    ssize       len;

    http = MPR->httpService;

    if (err) {
        *err = 0;
    }
    conn = httpCreateConn(http, NULL, NULL);
    mprAddRoot(conn);

    /* 
       Open a connection to issue the request. Then finalize the request output - this forces the request out.
     */
    if (httpConnect(conn, method, uri, NULL) < 0) {
        mprRemoveRoot(conn);
        *err = sfmt("Cannot connect to %s", uri);
        return 0;
    }
    if (data) {
        len = slen(data);
        if (httpWriteBlock(conn->writeq, data, len, HTTP_BLOCK) != len) {
            *err = sclone("Cannot write request body data");
        }
    }
    httpFinalizeOutput(conn);
    if (httpWait(conn, HTTP_STATE_CONTENT, MPR_MAX_TIMEOUT) < 0) {
        mprRemoveRoot(conn);
        *err = sclone("No response");
        return 0;
    }
    mprRemoveRoot(conn);
    return conn;
}


static int blockingFileCopy(HttpConn *conn, cchar *path)
{
    MprFile     *file;
    char        buf[BIT_MAX_BUFFER];
    ssize       bytes, nbytes, offset;

    file = mprOpenFile(path, O_RDONLY | O_BINARY, 0);
    if (file == 0) {
        mprError("Cannot open %s", path);
        return MPR_ERR_CANT_OPEN;
    }
    mprAddRoot(file);
    while ((bytes = mprReadFile(file, buf, sizeof(buf))) > 0) {
        offset = 0;
        while (bytes > 0) {
            if ((nbytes = httpWriteBlock(conn->writeq, &buf[offset], bytes, HTTP_BLOCK)) < 0) {
                mprCloseFile(file);
                mprRemoveRoot(file);
                return MPR_ERR_CANT_WRITE;
            }
            bytes -= nbytes;
            offset += nbytes;
            assert(bytes >= 0);
        }
    }
    httpFlushQueue(conn->writeq, HTTP_BLOCK);
    mprCloseFile(file);
    mprRemoveRoot(file);
    return 0;
}


/*
    Write upload data. This routine blocks. If you need non-blocking ... cut and paste.
 */
PUBLIC ssize httpWriteUploadData(HttpConn *conn, MprList *fileData, MprList *formData)
{
    char    *path, *pair, *key, *value, *name;
    cchar   *type;
    ssize   rc;
    int     next;

    rc = 0;
    if (formData) {
        for (rc = next = 0; rc >= 0 && (pair = mprGetNextItem(formData, &next)) != 0; ) {
            key = stok(sclone(pair), "=", &value);
            rc += httpWrite(conn->writeq, "%s\r\nContent-Disposition: form-data; name=\"%s\";\r\n", conn->boundary, key);
            rc += httpWrite(conn->writeq, "Content-Type: application/x-www-form-urlencoded\r\n\r\n%s\r\n", value);
        }
    }
    if (fileData) {
        for (rc = next = 0; rc >= 0 && (path = mprGetNextItem(fileData, &next)) != 0; ) {
            if (!mprPathExists(path, R_OK)) {
                httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot open %s", path);
                return MPR_ERR_CANT_OPEN;
            }
            name = mprGetPathBase(path);
            rc += httpWrite(conn->writeq, "%s\r\nContent-Disposition: form-data; name=\"file%d\"; filename=\"%s\"\r\n", 
                conn->boundary, next - 1, name);
            if ((type = mprLookupMime(MPR->mimeTypes, path)) != 0) {
                rc += httpWrite(conn->writeq, "Content-Type: %s\r\n", mprLookupMime(MPR->mimeTypes, path));
            }
            httpWrite(conn->writeq, "\r\n");
            if (blockingFileCopy(conn, path) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            rc += httpWrite(conn->writeq, "\r\n");
        }
    }
    rc += httpWrite(conn->writeq, "%s--\r\n--", conn->boundary);
    return rc;
}


/*
    Wait for the connection to reach a given state.
    Should only be used on the client side.
    @param state Desired state. Set to zero if you want to wait for one I/O event.
    @param timeout Timeout in msec. If timeout is zero, wait forever. If timeout is < 0, use default inactivity 
        and duration timeouts.
 */
PUBLIC int httpWait(HttpConn *conn, int state, MprTicks timeout)
{
    int     justOne;

    if (httpServerConn(conn)) {
        mprError("Should not call httpWait on the server side");
        return MPR_ERR_BAD_STATE;
    }
    if (state == 0) {
        state = HTTP_STATE_FINALIZED;
        justOne = 1;
    } else {
        justOne = 0;
    }
    if (conn->state <= HTTP_STATE_BEGIN) {
        return MPR_ERR_BAD_STATE;
    }
    if (conn->error) {
        if (conn->state >= state) {
            return 0;
        }
        return MPR_ERR_BAD_STATE;
    }
    if (timeout < 0) {
        timeout = conn->limits->inactivityTimeout;
    } else if (timeout == 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    if (state > HTTP_STATE_CONTENT) {
        httpFinalizeOutput(conn);
    }
    while (conn->state < state && !conn->error && !mprIsSocketEof(conn->sock) && !httpRequestExpired(conn, timeout)) {
        httpEnableConnEvents(conn);
        assert(httpClientConn(conn));
        mprWaitForEvent(conn->dispatcher, min(conn->limits->inactivityTimeout, timeout));
        if (justOne) break;
    }
    if (conn->error) {
        return MPR_ERR_CANT_CONNECT;
    }
    if (!justOne && conn->state < state) {
        return httpRequestExpired(conn, timeout) ? MPR_ERR_TIMEOUT : MPR_ERR_CANT_READ;
    }
    conn->lastActivity = conn->http->now;
    return 0;
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
