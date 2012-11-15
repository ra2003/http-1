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

    assure(conn);

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
        if (--conn->keepAliveCount < 0 || port != conn->port || strcmp(ip, conn->ip) != 0 || 
                uri->secure != (conn->sock->ssl != 0) || conn->sock->ssl != ssl) {
            httpCloseConn(conn);
        } else {
            mprLog(4, "Http: reusing keep-alive socket on: %s:%d", ip, port);
        }
    }
    if (conn->sock) {
        return conn;
    }
    if ((sp = mprCreateSocket()) == 0) {
        httpError(conn, HTTP_CODE_COMMS_ERROR, "Can't create socket for %s", uri->uri);
        return 0;
    }
    if ((rc = mprConnectSocket(sp, ip, port, 0)) < 0) {
        httpError(conn, HTTP_CODE_COMMS_ERROR, "Can't open socket on %s:%d", ip, port);
        return 0;
    }
    conn->sock = sp;
    conn->ip = sclone(ip);
    conn->port = port;
    conn->secure = uri->secure;
    conn->keepAliveCount = (conn->limits->keepAliveMax) ? conn->limits->keepAliveMax : -1;

#if BIT_PACK_SSL
    /* Must be done even if using keep alive for repeat SSL requests */
    if (uri->secure) {
        if (ssl == 0) {
            ssl = mprCreateSsl(0);
        }
        if (mprUpgradeSocket(sp, ssl, 0) < 0) {
            conn->errorMsg = sp->errorMsg;
            return 0;
        }
        mprLog(4, "Http: upgrade socket to TLS");
    }
#endif
    if (uri->webSockets && httpUpgradeWebSocket(conn) < 0) {
        conn->errorMsg = sp->errorMsg;
        return 0;
    }
    if ((level = httpShouldTrace(conn, HTTP_TRACE_RX, HTTP_TRACE_CONN, NULL)) >= 0) {
        mprLog(level, "### Outgoing connection from %s:%d to %s:%d", 
            conn->ip, conn->port, conn->sock->ip, conn->sock->port);
    }
    return conn;
}


static void setDefaultHeaders(HttpConn *conn)
{
    HttpAuthType    *authType;

    assure(conn);

    if (smatch(conn->protocol, "HTTP/1.0")) {
        conn->http10 = 1;
    }
    if (conn->authType && (authType = httpLookupAuthType(conn->authType)) != 0) {
        (authType->setAuth)(conn);
        conn->setCredentials = 1;
    }
    if (conn->port != 80) {
        httpAddHeader(conn, "Host", "%s:%d", conn->ip, conn->port);
    } else {
        httpAddHeaderString(conn, "Host", conn->ip);
    }
    if (conn->keepAliveCount > 0) {
        httpSetHeaderString(conn, "Connection", "Keep-Alive");
    } else {
        httpSetHeaderString(conn, "Connection", "close");
    }
}


PUBLIC int httpConnect(HttpConn *conn, cchar *method, cchar *uri, struct MprSsl *ssl)
{
    assure(conn);
    assure(method && *method);
    assure(uri && *uri);

    if (conn->endpoint) {
        httpError(conn, HTTP_CODE_BAD_GATEWAY, "Can't call connect in a server");
        return MPR_ERR_BAD_STATE;
    }
    mprLog(4, "Http: client request: %s %s", method, uri);

    if (conn->tx == 0 || conn->state != HTTP_STATE_BEGIN) {
        /* WARNING: this will erase headers */
        httpPrepClientConn(conn, 0);
    }
    assure(conn->state == HTTP_STATE_BEGIN);
    httpSetState(conn, HTTP_STATE_CONNECTED);
    conn->setCredentials = 0;
    conn->tx->method = supper(method);
    conn->tx->parsedUri = httpCreateUri(uri, 0);
#if BIT_DEBUG
    conn->startMark = mprGetHiResTime();
#endif
    /*
        The receive pipeline is created when parsing the response in parseIncoming()
     */
    httpCreateTxPipeline(conn, conn->http->clientRoute);
    if (openConnection(conn, ssl) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    setDefaultHeaders(conn);
    if (conn->upgraded) {
        /* Push out headers */
        httpServiceQueues(conn);
    }
    return 0;
}


/*  
    Check the response for authentication failures and redirections. Return true if a retry is requried.
 */
PUBLIC bool httpNeedRetry(HttpConn *conn, char **url)
{
    HttpAuthType    *authType;
    HttpRx          *rx;

    assure(conn->rx);

    *url = 0;
    rx = conn->rx;

    if (conn->state < HTTP_STATE_FIRST) {
        return 0;
    }
    if (rx->status == HTTP_CODE_UNAUTHORIZED) {
        if (conn->username == 0) {
            httpFormatError(conn, rx->status, "Authentication required");
        } else if (conn->setCredentials) {
            httpFormatError(conn, rx->status, "Authentication failed");
        } else {
            if (conn->authType && (authType = httpLookupAuthType(conn->authType)) != 0) {
                (authType->parseAuth)(conn);
            }
            return 1;
        }
    } else if (HTTP_CODE_MOVED_PERMANENTLY <= rx->status && rx->status <= HTTP_CODE_MOVED_TEMPORARILY && 
            conn->followRedirects) {
        if (rx->redirect) {
            *url = rx->redirect;
            return 1;
        }
        httpFormatError(conn, rx->status, "Missing location header");
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


static int blockingFileCopy(HttpConn *conn, cchar *path)
{
    MprFile     *file;
    char        buf[MPR_BUFSIZE];
    ssize       bytes, nbytes, offset;

    file = mprOpenFile(path, O_RDONLY | O_BINARY, 0);
    if (file == 0) {
        mprError("Can't open %s", path);
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
            assure(bytes >= 0);
        }
        mprYield(0);
    }
    httpFlushQueue(conn->writeq, 1);
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
                httpFormatError(conn, 0, "Can't open %s", path);
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
