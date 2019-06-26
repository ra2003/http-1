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
    if (q->conn->rx->flags & HTTP_TRACE) {
        handleTrace(q->conn);
    }
}


static void readyPass(HttpQueue *q)
{
    httpFinalizeOutput(q->conn);
}


static void errorPass(HttpQueue *q)
{
    if (!q->conn->error) {
        httpError(q->conn, HTTP_CODE_NOT_FOUND, "The requested resource is not available");
    }
    httpFinalizeOutput(q->conn);
}


PUBLIC void httpHandleOptions(HttpConn *conn)
{
    httpSetHeaderString(conn, "Allow", httpGetRouteMethods(conn->rx->route));
    httpFinalizeOutput(conn);
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
    httpDiscardData(conn, HTTP_QUEUE_TX);
    traceData = httpCreateDataPacket(httpGetPacketLength(headers) + 128);
    tx->flags &= ~(HTTP_TX_NO_LENGTH | HTTP_TX_HEADERS_CREATED);
    q->count -= httpGetPacketLength(headers);
    assert(q->count == 0);
    mprFlushBuf(headers->content);
    mprPutStringToBuf(traceData->content, mprGetBufStart(q->first->content));
    httpSetContentType(conn, "message/http");
    httpPutForService(q, traceData, HTTP_DELAY_SERVICE);
    httpFinalize(conn);
}


static void incomingPass(HttpQueue *q, HttpPacket *packet)
{
    /* Simply discard incoming data */
}


PUBLIC int httpOpenPassHandler()
{
    HttpStage     *stage;

    if ((stage = httpCreateHandler("passHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->passHandler = stage;
    stage->start = startPass;
    stage->ready = readyPass;

    /*
        PassHandler is an alias as the ErrorHandler too
     */
    if ((stage = httpCreateHandler("errorHandler", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->start = startPass;
    stage->ready = errorPass;
    stage->incoming = incomingPass;
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
