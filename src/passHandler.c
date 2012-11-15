/*
    passHandler.c -- Pass through handler

    This handler simply relays all content to a network connector. It is used for the ErrorHandler and 
    when there is no handler defined. It is configured as the "passHandler" and "errorHandler".

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void startPass(HttpQueue *q)
{
    mprLog(5, "Start passHandler");
    if (q->conn->rx->flags & (HTTP_OPTIONS | HTTP_TRACE)) {
        httpHandleOptionsTrace(q->conn, "");
    }
}


static void readyPass(HttpQueue *q)
{
    httpFinalize(q->conn);
}


static void readyError(HttpQueue *q)
{
    if (!q->conn->error) {
        /*
            The ErrorHandler emits this error always
         */
        httpError(q->conn, HTTP_CODE_SERVICE_UNAVAILABLE, "The requested resource is not available");
    }
    httpFinalize(q->conn);
}


/*
    Handle Trace and Options requests. Handlers can do this themselves if they desire, but typically
    all Trace/Options requests come here.
 */
PUBLIC void httpHandleOptionsTrace(HttpConn *conn, cchar *methods)
{
    HttpRx      *rx;
    HttpTx      *tx;
    HttpRoute   *route;
    HttpQueue   *q;
    HttpPacket  *traceData, *headers;
    MprKey      *method;

    tx = conn->tx;
    rx = conn->rx;
    route = rx->route;

    if (rx->flags & HTTP_TRACE) {
        /* The trace method is disabled by default unless 'TraceMethod on' is specified */
        if (!(route->flags & HTTP_ROUTE_TRACE_METHOD)) {
            tx->status = HTTP_CODE_NOT_ACCEPTABLE;
            httpFormatResponseBody(conn, "Trace Request Denied", "The TRACE method is disabled for this resource.");
        } else {
            /*
                Create a dummy set of headers to use as the response body. Then reset so the connector will
                create the headers in the normal fashion. Need to be careful not to have a content length in the
                headers in the body.
             */
            q = conn->writeq;
            headers = q->first;
            tx->flags |= HTTP_TX_NO_LENGTH;
            httpWriteHeaders(q, headers);
            traceData = httpCreateDataPacket(httpGetPacketLength(headers) + 128);
            tx->flags &= ~(HTTP_TX_NO_LENGTH | HTTP_TX_HEADERS_CREATED);
            q->count -= httpGetPacketLength(headers);
            assure(q->count == 0);
            mprFlushBuf(headers->content);
            mprPutFmtToBuf(traceData->content, mprGetBufStart(q->first->content));
            httpSetContentType(conn, "message/http");
            httpPutForService(q, traceData, HTTP_DELAY_SERVICE);
        }

    } else if (rx->flags & HTTP_OPTIONS) {
        if (rx->route->methods) {
            methods = 0;
            for (ITERATE_KEYS(route->methods, method)) {
                methods = (methods) ? sjoin(methods, ",", method->key, 0) : method->key;
            }
            httpSetHeader(conn, "Allow", "%s", methods);
        } else {
            httpSetHeader(conn, "Allow", "OPTIONS,%s%s", (route->flags & HTTP_ROUTE_TRACE_METHOD) ? "TRACE," : "", methods);
        }
        assure(tx->length <= 0);
    }
    httpFinalize(conn);
}


PUBLIC int httpOpenPassHandler(Http *http)
{
    HttpStage     *stage;

    if ((stage = httpCreateHandler(http, "passHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    http->passHandler = stage;
    stage->start = startPass;
    stage->ready = readyPass;

    /*
        PassHandler is an alias as the ErrorHandler too
     */
    if ((stage = httpCreateHandler(http, "errorHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->start = startPass;
    stage->ready = readyError;
    return 0;
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
