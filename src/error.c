/*
    error.c -- Http error handling
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void errorv(HttpConn *conn, int flags, cchar *fmt, va_list args);
static char *formatErrorv(HttpConn *conn, int status, cchar *fmt, va_list args);

/*********************************** Code *************************************/

PUBLIC void httpBadRequestError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    if (httpServerConn(conn)) {
        httpMonitorEvent(conn, HTTP_COUNTER_BAD_REQUEST_ERRORS, 1);
    }
    errorv(conn, flags, fmt, args);
    va_end(args);
}


PUBLIC void httpLimitError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    if (httpServerConn(conn)) {
        httpMonitorEvent(conn, HTTP_COUNTER_LIMIT_ERRORS, 1);
    }
    errorv(conn, flags, fmt, args);
    va_end(args);
}


PUBLIC void httpError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    errorv(conn, flags, fmt, args);
    va_end(args);
}


static void errorRedirect(HttpConn *conn, cchar *uri)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (sstarts(uri, "http") || tx->flags & HTTP_TX_HEADERS_CREATED) {
        httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, uri);
    } else {
        /*
            No response started and it is an internal redirect, so we can rerun the request.
            Set finalized to "cap" any output. processCompletion() in rx.c will rerun the request using the errorDocument.
         */
        tx->errorDocument = httpLinkAbs(conn, uri);
        tx->finalized = tx->finalizedOutput = tx->finalizedConnector = 1;
    }
}


static void makeAltBody(HttpConn *conn, int status)
{
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *statusMsg, *msg;

    rx = conn->rx;
    tx = conn->tx;
    assert(rx && tx);

    statusMsg = httpLookupStatus(status);
    msg = (rx && rx->route && rx->route->flags & HTTP_ROUTE_SHOW_ERRORS) ?  conn->errorMsg : "";
    if (rx && scmp(rx->accept, "text/plain") == 0) {
        tx->altBody = sfmt("Access Error: %d -- %s\r\n%s\r\n", status, statusMsg, msg);
    } else {
        httpSetContentType(conn, "text/html");
        tx->altBody = sfmt("<!DOCTYPE html>\r\n"
            "<head>\r\n"
            "    <title>%s</title>\r\n"
            "    <link rel=\"shortcut icon\" href=\"data:image/x-icon;,\" type=\"image/x-icon\">\r\n"
            "</head>\r\n"
            "<body>\r\n<h2>Access Error: %d -- %s</h2>\r\n<pre>%s</pre>\r\n</body>\r\n</html>\r\n",
            statusMsg, status, statusMsg, mprEscapeHtml(msg));
    }
    tx->length = slen(tx->altBody);
}


/*
    The current request has an error and cannot complete as normal. This call sets the Http response status and
    overrides the normal output with an alternate error message. If the output has alread started (headers sent), then
    the connection MUST be closed so the client can get some indication the request failed.
 */
static void errorv(HttpConn *conn, int flags, cchar *fmt, va_list args)
{
    HttpRx      *rx;
    HttpTx      *tx;
    cchar       *uri;
    int         status;

    rx = conn->rx;
    tx = conn->tx;

    if (conn == 0 || fmt == 0) {
        return;
    }
    status = flags & HTTP_CODE_MASK;
    if (status == 0) {
        status = HTTP_CODE_INTERNAL_SERVER_ERROR;
    }
    if (flags & (HTTP_ABORT | HTTP_CLOSE)) {
        conn->keepAliveCount = 0;
    }
    if (flags & HTTP_ABORT) {
        conn->connError++;
    }
    if (!conn->error) {
        conn->error++;
        httpOmitBody(conn);
        conn->errorMsg = formatErrorv(conn, status, fmt, args);
        httpTrace(conn, "request.error", "error", "msg:'%s'", conn->errorMsg);
        HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, 0);
        if (httpServerConn(conn)) {
            if (status == HTTP_CODE_NOT_FOUND) {
                httpMonitorEvent(conn, HTTP_COUNTER_NOT_FOUND_ERRORS, 1);
            }
            httpMonitorEvent(conn, HTTP_COUNTER_ERRORS, 1);
        }
        httpSetHeaderString(conn, "Cache-Control", "no-cache");
        if (httpServerConn(conn) && tx && rx) {
            if (tx->flags & HTTP_TX_HEADERS_CREATED) {
                /*
                    If the response headers have been sent, must let the other side of the failure ... aborting
                    the request is the only way as the status has been sent.
                 */
                flags |= HTTP_ABORT;
            } else {
                if (rx->route && (uri = httpLookupRouteErrorDocument(rx->route, tx->status)) && !smatch(uri, rx->uri)) {
                    errorRedirect(conn, uri);
                } else {
                    makeAltBody(conn, status);
                }
            }
        }
        httpFinalize(conn);
    }
    if (flags & HTTP_ABORT) {
        httpDisconnect(conn);
    }
}


/*
    Just format conn->errorMsg and set status - nothing more
    NOTE: this is an internal API. Users should use httpError()
 */
static char *formatErrorv(HttpConn *conn, int status, cchar *fmt, va_list args)
{
    if (conn->errorMsg == 0) {
        conn->errorMsg = sfmtv(fmt, args);
        if (status) {
            if (status < 0) {
                status = HTTP_CODE_INTERNAL_SERVER_ERROR;
            }
            if (httpServerConn(conn) && conn->tx) {
                conn->tx->status = status;
            } else if (conn->rx) {
                conn->rx->status = status;
            }
        }
    }
    return conn->errorMsg;
}


PUBLIC cchar *httpGetError(HttpConn *conn)
{
    if (conn->errorMsg) {
        return conn->errorMsg;
    } else if (conn->state >= HTTP_STATE_FIRST) {
        return httpLookupStatus(conn->rx->status);
    } else {
        return "";
    }
}


PUBLIC void httpMemoryError(HttpConn *conn)
{
    httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Memory allocation error");
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
