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

PUBLIC void httpDisconnect(HttpConn *conn)
{
    if (conn->sock) {
        mprDisconnectSocket(conn->sock);
    }
    conn->connError = 1;
    conn->error = 1;
    conn->keepAliveCount = -1;
    if (conn->rx) {
        conn->rx->eof = 1;
    }
    if (conn->tx) {
        conn->tx->finalized = 1;
        conn->tx->finalizedOutput = 1;
        conn->tx->finalizedConnector = 1;
    }
}


PUBLIC void httpError(HttpConn *conn, int flags, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt);
    errorv(conn, flags, fmt, args);
    va_end(args);
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
    cchar       *uri, *statusMsg;
    int         status;

    assure(fmt);
    rx = conn->rx;
    tx = conn->tx;

    if (conn == 0) {
        return;
    }
    status = flags & HTTP_CODE_MASK;
    if (status == 0) {
        status = HTTP_CODE_INTERNAL_SERVER_ERROR;
    }
    if (flags & (HTTP_ABORT | HTTP_CLOSE)) {
        conn->keepAliveCount = -1;
    }
    if (flags & HTTP_ABORT) {
        conn->connError = 1;
        if (rx) {
            rx->eof = 1;
        }
    }
    if (!conn->error) {
        conn->error = 1;
        httpOmitBody(conn);
        conn->errorMsg = formatErrorv(conn, status, fmt, args);
        HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, 0);
        if (conn->endpoint && tx && rx) {
            if (tx->flags & HTTP_TX_HEADERS_CREATED) {
                /* 
                    If the response headers have been sent, must let the other side of the failure ... aborting
                    the request is the only way as the status has been sent.
                 */
                flags |= HTTP_ABORT;
            } else {
                if (rx->route && (uri = httpLookupRouteErrorDocument(rx->route, tx->status))) {
                    /*
                        If the response has started or it is an external redirect ... do a redirect
                     */
                    if (sstarts(uri, "http") || tx->flags & HTTP_TX_HEADERS_CREATED) {
                        httpRedirect(conn, HTTP_CODE_MOVED_PERMANENTLY, uri);
                    } else {
                        /*
                            No response started and it is an internal redirect, so we can rerun the request.
                            Set finalized to "cap" any output. processCompletion() in rx.c will rerun the request using
                            the errorDocument.
                         */
                        tx->errorDocument = uri;
                        tx->finalized = tx->finalizedOutput = tx->finalizedConnector = 1;
                    }
                } else {
                    httpAddHeaderString(conn, "Cache-Control", "no-cache");
                    statusMsg = httpLookupStatus(conn->http, status);
                    if (scmp(conn->rx->accept, "text/plain") == 0) {
                        tx->altBody = sfmt("Access Error: %d -- %s\r\n%s\r\n", status, statusMsg, conn->errorMsg);
                    } else {
                        tx->altBody = sfmt("<!DOCTYPE html>\r\n"
                            "<html><head><title>%s</title></head>\r\n"
                            "<body>\r\n<h2>Access Error: %d -- %s</h2>\r\n<pre>%s</pre>\r\n</body>\r\n</html>\r\n",
                            statusMsg, status, statusMsg, mprEscapeHtml(conn->errorMsg));
                    }
                    tx->length = slen(tx->altBody);
                    tx->flags |= HTTP_TX_NO_BODY;
                    httpDiscardData(conn, HTTP_QUEUE_TX);
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
            if (conn->endpoint && conn->tx) {
                conn->tx->status = status;
            } else if (conn->rx) {
                conn->rx->status = status;
            }
        }
        if (conn->rx == 0 || conn->rx->uri == 0) {
            mprLog(2, "\"%s\", status %d: %s.", httpLookupStatus(conn->http, status), status, conn->errorMsg);
        } else {
            mprLog(2, "Error: %s", conn->errorMsg);
        }
    }
    return conn->errorMsg;
}


/*
    Just format conn->errorMsg and set status - nothing more
    NOTE: this is an internal API. Users should use httpError()
 */
PUBLIC void httpFormatError(HttpConn *conn, int status, cchar *fmt, ...)
{
    va_list     args;

    va_start(args, fmt); 
    conn->errorMsg = formatErrorv(conn, status, fmt, args);
    va_end(args); 
}


PUBLIC cchar *httpGetError(HttpConn *conn)
{
    if (conn->errorMsg) {
        return conn->errorMsg;
    } else if (conn->state >= HTTP_STATE_FIRST) {
        return httpLookupStatus(conn->http, conn->rx->status);
    } else {
        return "";
    }
}


PUBLIC void httpMemoryError(HttpConn *conn)
{
    httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Memory allocation error");
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
