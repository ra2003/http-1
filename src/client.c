/*
    client.c -- Client side specific support.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************* Forwards ***********************************/

static void setDefaultHeaders(HttpStream *stream);
static int clientRequest(HttpStream *stream, cchar *method, cchar *uri, cchar *data, char **err);

/*********************************** Code *************************************/
/*
    Get the IP:PORT for a request URI
 */
PUBLIC void httpGetUriAddress(HttpUri *uri, cchar **ip, int *port)
{
    Http    *http;

    http = HTTP;

    if (!uri->host) {
        *ip = (http->proxyHost) ? http->proxyHost : http->defaultClientHost;
        *port = (http->proxyHost) ? http->proxyPort : uri->port;
    } else {
        *ip = (http->proxyHost) ? http->proxyHost : uri->host;
        *port = (http->proxyHost) ? http->proxyPort : uri->port;
    }
    if (*port == 0) {
        *port = (uri->secure) ? 443 : http->defaultClientPort;
    }
}


/*
    Determine if the current network connection can handle the current URI without redirection
 */
static bool canUse(HttpNet *net, HttpStream *stream, HttpUri *uri, MprSsl *ssl, cchar *ip, int port)
{
    MprSocket   *sock;

    assert(net);

    if ((sock = net->sock) == 0) {
        return 0;
    }
    if (port != net->port || !smatch(ip, net->ip) || uri->secure != (sock->ssl != 0) || sock->ssl != ssl) {
        return 0;
    }
    if (net->protocol < 2 && stream->keepAliveCount <= 1) {
        return 0;
    }
    return 1;
}


PUBLIC int httpConnect(HttpStream *stream, cchar *method, cchar *url, MprSsl *ssl)
{
    HttpNet     *net;
    HttpTx      *tx;
    HttpUri     *uri;
    cchar       *ip, *protocol;
    int         port;

    assert(stream);
    assert(method && *method);
    assert(url && *url);

    net = stream->net;
    if (httpServerStream(stream)) {
        mprLog("client error", 0, "Cannot call httpConnect() in a server");
        return MPR_ERR_BAD_STATE;
    }
    if (net->protocol <= 0) {
        mprLog("client error", 0, "HTTP protocol to use has not been defined");
        return MPR_ERR_BAD_STATE;
    }
    if (stream->tx == 0 || stream->state != HTTP_STATE_BEGIN) {
        httpResetClientStream(stream, 0);
    }
    tx = stream->tx;
    tx->method = supper(method);
    stream->authRequested = 0;
    stream->startMark = mprGetHiResTicks();

    if ((uri = tx->parsedUri = httpCreateUri(url, HTTP_COMPLETE_URI_PATH)) == 0) {
        return MPR_ERR_BAD_ARGS;
    }
    ssl = uri->secure ? (ssl ? ssl : mprCreateSsl(0)) : 0;
    httpGetUriAddress(uri, &ip, &port);

    if (net->sock) {
        if (net->error) {
            mprCloseSocket(net->sock, 0);
            net->sock = 0;

        } else if (canUse(net, stream, uri, ssl, ip, port)) {
            httpLog(net->trace, "client.connection.reuse", "context", "reuse:%d", stream->keepAliveCount);

        } else {
            if (net->protocol >= 2) {
                if (mprGetListLength(net->streams) > 1) {
                    httpError(stream, HTTP_CODE_COMMS_ERROR, "Cannot use network for %s due to other existing requests", ip);
                    return MPR_ERR_CANT_FIND;
                }
            } else {
                mprCloseSocket(net->sock, 0);
                net->sock = 0;
            }
        }
    }
    if (!net->sock) {
        if (httpConnectNet(net, ip, port, ssl) < 0) {
            return MPR_ERR_CANT_CONNECT;
        }
        stream->net = net;
        stream->sock = net->sock;
        stream->ip = net->ip;
        stream->port = net->port;
        stream->keepAliveCount = (net->protocol >= 2) ? 0 : stream->limits->keepAliveMax;

#if ME_HTTP_WEB_SOCKETS
        if (net->protocol == 1 && uri->webSockets && httpUpgradeWebSocket(stream) < 0) {
            stream->errorMsg = net->errorMsg = net->sock->errorMsg;
            return 0;
        }
#endif
    }

    httpCreatePipeline(stream);
    setDefaultHeaders(stream);
    tx->started = 1;

    httpSetState(stream, HTTP_STATE_CONNECTED);
    httpPumpOutput(stream->writeq);

    httpServiceNetQueues(net, 0);
    httpEnableNetEvents(net);

    protocol = net->protocol < 2 ? "HTTP/1.1" : "HTTP/2";
    httpLog(net->trace, "client.request", "request", "method='%s', url='%s', protocol='%s'", tx->method, url, protocol);
    return 0;
}


static void setDefaultHeaders(HttpStream *stream)
{
    HttpAuthType    *ap;

    assert(stream);

    if (stream->username && stream->authType && ((ap = httpLookupAuthType(stream->authType)) != 0)) {
        if ((ap->setAuth)(stream, stream->username, stream->password)) {
            stream->authRequested = 1;
        }
    }
    if (stream->net->protocol < 2) {
        if (stream->port != 80 && stream->port != 443) {
            if (schr(stream->ip, ':')) {
                httpAddHeader(stream, "Host", "[%s]:%d", stream->ip, stream->port);
            } else {
                httpAddHeader(stream, "Host", "%s:%d", stream->ip, stream->port);
            }
        } else {
            httpAddHeaderString(stream, "Host", stream->ip);
        }
        if (stream->keepAliveCount > 0) {
            httpSetHeaderString(stream, "Connection", "Keep-Alive");
        } else {
            httpSetHeaderString(stream, "Connection", "close");
        }
    }
    httpAddHeaderString(stream, "Accept", "*/*");
}


/*
    Check the response for authentication failures and redirections. Return true if a retry is requried.
 */
PUBLIC bool httpNeedRetry(HttpStream *stream, cchar **url)
{
    HttpRx          *rx;
    HttpAuthType    *authType;

    assert(stream->rx);

    *url = 0;
    rx = stream->rx;

    if (stream->error || stream->state < HTTP_STATE_FIRST) {
        return 0;
    }
    if (rx->status == HTTP_CODE_UNAUTHORIZED) {
        if (stream->username == 0 || stream->authType == 0) {
            httpError(stream, rx->status, "Authentication required");

        } else if (stream->authRequested && smatch(stream->authType, rx->authType)) {
            httpError(stream, rx->status, "Authentication failed, wrong authentication type");

        } else {
            assert(httpClientStream(stream));
            if (stream->authType && (authType = httpLookupAuthType(stream->authType)) != 0) {
                (authType->parseAuth)(stream, NULL, NULL);
            }
            return 1;
        }

    } else if (HTTP_CODE_MOVED_PERMANENTLY <= rx->status && rx->status <= HTTP_CODE_MOVED_TEMPORARILY && stream->followRedirects) {
        if (rx->redirect) {
            *url = rx->redirect;
            return 1;
        }
        httpError(stream, rx->status, "Missing location header");
        return 0;
    }
    return 0;
}


/*
    Set the request as being a multipart mime upload. This defines the content type and defines a multipart mime boundary
 */
PUBLIC void httpEnableUpload(HttpStream *stream)
{
    stream->boundary = sfmt("--BOUNDARY--%lld", stream->http->now);
    httpSetHeader(stream, "Content-Type", "multipart/form-data; boundary=%s", &stream->boundary[2]);
}


/*
    TODO - need to test these
    Read data. If sync mode, this will block. If async, will never block.
    Will return what data is available up to the requested size.
    Timeout in milliseconds to wait. Set to -1 to use the default inactivity timeout. Set to zero to wait forever.
    Returns a count of bytes read. Returns zero if no data. EOF if returns zero and stream->state is > HTTP_STATE_CONTENT.
 */
PUBLIC ssize httpReadBlock(HttpStream *stream, char *buf, ssize size, MprTicks timeout, int flags)
{
    HttpPacket  *packet;
    HttpQueue   *q;
    HttpLimits  *limits;
    MprBuf      *content;
    MprTicks    start, delay;
    ssize       nbytes, len;
    int64       dispatcherMark;

    q = stream->readq;
    assert(q->count >= 0);
    assert(size >= 0);
    limits = stream->limits;

    if (flags == 0) {
        flags = stream->net->async ? HTTP_NON_BLOCK : HTTP_BLOCK;
    }
    if (timeout < 0) {
        timeout = limits->inactivityTimeout;
    } else if (timeout == 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    if (flags & HTTP_BLOCK) {
        start = stream->http->now;
        dispatcherMark = mprGetEventMark(stream->dispatcher);
        while (q->count <= 0 && !stream->error && (stream->state <= HTTP_STATE_CONTENT)) {
            if (httpRequestExpired(stream, -1)) {
                break;
            }
            delay = min(limits->inactivityTimeout, mprGetRemainingTicks(start, timeout));
            //  TODO - review
            httpEnableNetEvents(stream->net);
            mprWaitForEvent(stream->dispatcher, delay, dispatcherMark);
            if (mprGetRemainingTicks(start, timeout) <= 0) {
                break;
            }
            dispatcherMark = mprGetEventMark(stream->dispatcher);
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
    if (nbytes == 0 && httpRequestExpired(stream, -1)) {
        return MPR_ERR_TIMEOUT;
    }
    return nbytes;
}


/*
    Read with standard connection timeouts and in blocking mode for clients, non-blocking for server-side
 */
PUBLIC ssize httpRead(HttpStream *stream, char *buf, ssize size)
{
    return httpReadBlock(stream, buf, size, -1, 0);
}


PUBLIC char *httpReadString(HttpStream *stream)
{
    HttpRx      *rx;
    ssize       sofar, nbytes, remaining;
    char        *content;

    rx = stream->rx;
    if (rx->length < 0) {
        return 0;
    }
    remaining = (ssize) min(MAXSSIZE, rx->length);

    if (remaining > 0) {
        if ((content = mprAlloc(remaining + 1)) == 0) {
            return 0;
        }
        sofar = 0;
        while (remaining > 0) {
            nbytes = httpRead(stream, &content[sofar], remaining);
            if (nbytes < 0) {
                return 0;
            }
            sofar += nbytes;
            remaining -= nbytes;
        }
    } else {
        content = NULL;
        sofar = 0;
        while (1) {
            if ((content = mprRealloc(content, sofar + ME_BUFSIZE)) == 0) {
                return 0;
            }
            nbytes = httpRead(stream, &content[sofar], ME_BUFSIZE);
            if (nbytes < 0) {
                return 0;
            } else if (nbytes == 0) {
                break;
            }
            sofar += nbytes;
        }
    }
    content[sofar] = '\0';
    return content;
}


/*
    Convenience method to issue a client http request.
    Assumes the Mpr and Http services are created and initialized.
 */
PUBLIC HttpStream *httpRequest(cchar *method, cchar *uri, cchar *data, int protocol, char **err)
{
    HttpNet         *net;
    HttpStream      *stream;
    MprDispatcher   *dispatcher;

    assert(err);
    dispatcher = mprCreateDispatcher("httpRequest", MPR_DISPATCHER_AUTO);
    mprStartDispatcher(dispatcher);

    net = httpCreateNet(dispatcher, NULL, protocol, 0);
    if ((stream = httpCreateStream(net, 0)) == 0) {
        return 0;
    }
    mprAddRoot(stream);

    if (clientRequest(stream, method, uri, data, err) < 0) {
        mprRemoveRoot(stream);
        httpDestroyNet(net);
        return 0;
    }
    mprRemoveRoot(stream);
    return stream;
}


static int clientRequest(HttpStream *stream, cchar *method, cchar *uri, cchar *data, char **err)
{
    ssize   len;

    /*
       Open a connection to issue the request. Then finalize the request output - this forces the request out.
     */
    *err = 0;
    if (httpConnect(stream, method, uri, NULL) < 0) {
        *err = sfmt("Cannot connect to %s", uri);
        return MPR_ERR_CANT_CONNECT;
    }
    if (data) {
        len = slen(data);
        if (httpWriteBlock(stream->writeq, data, len, HTTP_BLOCK) != len) {
            *err = sclone("Cannot write request body data");
            return MPR_ERR_CANT_WRITE;
        }
    }
    httpFinalizeOutput(stream);
    if (httpWait(stream, HTTP_STATE_CONTENT, MPR_MAX_TIMEOUT) < 0) {
        *err = sclone("No response");
        return MPR_ERR_BAD_STATE;
    }
    return 0;
}


static int blockingFileCopy(HttpStream *stream, cchar *path)
{
    MprFile     *file;
    char        buf[ME_BUFSIZE];
    ssize       bytes, nbytes, offset;

    file = mprOpenFile(path, O_RDONLY | O_BINARY, 0);
    if (file == 0) {
        mprLog("error http client", 0, "Cannot open %s", path);
        return MPR_ERR_CANT_OPEN;
    }
    mprAddRoot(file);
    while ((bytes = mprReadFile(file, buf, sizeof(buf))) > 0) {
        offset = 0;
        while (bytes > 0) {
            if ((nbytes = httpWriteBlock(stream->writeq, &buf[offset], bytes, HTTP_BLOCK)) < 0) {
                mprCloseFile(file);
                mprRemoveRoot(file);
                return MPR_ERR_CANT_WRITE;
            }
            bytes -= nbytes;
            offset += nbytes;
            assert(bytes >= 0);
        }
    }
    httpFlushQueue(stream->writeq, HTTP_BLOCK);
    mprCloseFile(file);
    mprRemoveRoot(file);
    return 0;
}


/*
    Write upload data. This routine blocks. If you need non-blocking ... cut and paste.
    TODO - what about non-blocking upload
 */
PUBLIC ssize httpWriteUploadData(HttpStream *stream, MprList *fileData, MprList *formData)
{
    char    *path, *pair, *key, *value, *name;
    cchar   *type;
    ssize   rc;
    int     next;

    rc = 0;
    if (formData) {
        for (rc = next = 0; rc >= 0 && (pair = mprGetNextItem(formData, &next)) != 0; ) {
            key = ssplit(sclone(pair), "=", &value);
            rc += httpWrite(stream->writeq, "%s\r\nContent-Disposition: form-data; name=\"%s\";\r\n", stream->boundary, key);
            rc += httpWrite(stream->writeq, "Content-Type: application/x-www-form-urlencoded\r\n\r\n%s\r\n", value);
        }
    }
    if (fileData) {
        for (rc = next = 0; rc >= 0 && (path = mprGetNextItem(fileData, &next)) != 0; ) {
            if (!mprPathExists(path, R_OK)) {
                httpError(stream, HTTP_CODE_NOT_FOUND, "Cannot open %s", path);
                return MPR_ERR_CANT_OPEN;
            }
            name = mprGetPathBase(path);
            rc += httpWrite(stream->writeq, "%s\r\nContent-Disposition: form-data; name=\"file%d\"; filename=\"%s\"\r\n",
                stream->boundary, next - 1, name);
            if ((type = mprLookupMime(MPR->mimeTypes, path)) != 0) {
                rc += httpWrite(stream->writeq, "Content-Type: %s\r\n", mprLookupMime(MPR->mimeTypes, path));
            }
            httpWrite(stream->writeq, "\r\n");
            if (blockingFileCopy(stream, path) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            rc += httpWrite(stream->writeq, "\r\n");
        }
    }
    rc += httpWrite(stream->writeq, "%s--\r\n--", stream->boundary);
    return rc;
}


/*
    Wait for the connection to reach a given state.
    Should only be used on the client side.
    @param state Desired state. Set to zero if you want to wait for one I/O event.
    @param timeout Timeout in msec. If timeout is zero, wait forever. If timeout is < 0, use default inactivity
        and duration timeouts.
 */
PUBLIC int httpWait(HttpStream *stream, int state, MprTicks timeout)
{
    HttpLimits  *limits;
    MprTicks    delay, start;
    int64       dispatcherMark;
    int         justOne;

    limits = stream->limits;
    if (httpServerStream(stream)) {
        return MPR_ERR_BAD_STATE;
    }
    if (stream->state <= HTTP_STATE_BEGIN) {
        return MPR_ERR_BAD_STATE;
    }
    if (state == 0) {
        /* Wait for just one I/O event */
        state = HTTP_STATE_FINALIZED;
        justOne = 1;
    } else {
        justOne = 0;
    }
    if (stream->error) {
        if (stream->state >= state) {
            return 0;
        }
        return MPR_ERR_BAD_STATE;
    }
    if (timeout < 0) {
        timeout = limits->requestTimeout;
    } else if (timeout == 0) {
        timeout = MPR_MAX_TIMEOUT;
    }
    if (state > HTTP_STATE_CONTENT) {
        httpFinalizeOutput(stream);
    }
    start = stream->http->now;
    dispatcherMark = mprGetEventMark(stream->dispatcher);

    //  TODO - how does this work with http2?
    while (stream->state < state && !stream->error && !mprIsSocketEof(stream->sock)) {
        if (httpRequestExpired(stream, -1)) {
            return MPR_ERR_TIMEOUT;
        }
        //  TODO - review
        httpEnableNetEvents(stream->net);
        delay = min(limits->inactivityTimeout, mprGetRemainingTicks(start, timeout));
        delay = max(delay, 0);
        mprWaitForEvent(stream->dispatcher, delay, dispatcherMark);
        if (justOne || (mprGetRemainingTicks(start, timeout) <= 0)) {
            break;
        }
        dispatcherMark = mprGetEventMark(stream->dispatcher);
    }
    if (stream->error) {
        return MPR_ERR_NOT_READY;
    }
    if (stream->state < state) {
        if (mprGetRemainingTicks(start, timeout) <= 0) {
            return MPR_ERR_TIMEOUT;
        }
        if (!justOne) {
            return MPR_ERR_CANT_READ;
        }
    }
    stream->lastActivity = stream->http->now;
    return 0;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
