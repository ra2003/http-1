/*
    passHandler.c -- Pass through handler

    This handler simply relays all content to a network connector. It is used for the ErrorHandler and 
    when there is no handler defined. It is configured as the "passHandler" and "errorHandler".
    It also handles OPTIONS and TRACE methods for all.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

static void handleTrace(HttpConn *conn);

/*********************************** Code *************************************/

static void startPass(HttpQueue *q)
{
    mprTrace(5, "Start passHandler");
    if (q->conn->rx->flags & HTTP_TRACE) {
        handleTrace(q->conn);
    }
}


static void readyPass(HttpQueue *q)
{
    httpFinalize(q->conn);
}


static void readyError(HttpQueue *q)
{
    if (!q->conn->error) {
        httpError(q->conn, HTTP_CODE_SERVICE_UNAVAILABLE, "The requested resource is not available");
    }
    httpFinalize(q->conn);
}


PUBLIC void httpHandleOptions(HttpConn *conn)
{
    httpSetHeaderString(conn, "Allow", httpGetRouteMethods(conn->rx->route));
    httpFinalize(conn);
}


static void handleTrace(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q;
    HttpPacket  *traceData, *headers;

    /*
        Create a dummy set of headers to use as the response body. Then reset so the connector will create 
        the headers in the normal fashion. Need to be careful not to have a content length in the headers in the body.
     */
    tx = conn->tx;
    q = conn->writeq;
    headers = q->first;
    tx->flags |= HTTP_TX_NO_LENGTH;
    httpWriteHeaders(q, headers);
    traceData = httpCreateDataPacket(httpGetPacketLength(headers) + 128);
    tx->flags &= ~(HTTP_TX_NO_LENGTH | HTTP_TX_HEADERS_CREATED);
    q->count -= httpGetPacketLength(headers);
    assert(q->count == 0);
    mprFlushBuf(headers->content);
    mprPutToBuf(traceData->content, mprGetBufStart(q->first->content));
    httpSetContentType(conn, "message/http");
    httpPutForService(q, traceData, HTTP_DELAY_SERVICE);
    httpFinalize(conn);
}


#if DEPRECATE || 1
PUBLIC void httpHandleOptionsTrace(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;
    if (rx->flags & HTTP_OPTIONS) {
        httpHandleOptions(conn);
    } else if (rx->flags & HTTP_TRACE) {
        handleTrace(conn);
    }
}
#endif


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

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

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
