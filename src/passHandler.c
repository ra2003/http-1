/*
    passHandler.c -- Pass through handler

    This handler simply relays all content to a network connector. It is used for the ErrorHandler and
    when there is no handler defined. It is configured as the "passHandler" and "errorHandler".
    It also handles OPTIONS and TRACE methods for all.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void handleTraceMethod(HttpConn *conn);
static void readyError(HttpQueue *q);
static void readyPass(HttpQueue *q);
static void startPass(HttpQueue *q);

/*********************************** Code *************************************/

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
    stage->ready = readyError;
    return 0;
}


static void startPass(HttpQueue *q)
{
    HttpConn    *conn;

    conn = q->conn;

    if (conn->rx->flags & HTTP_TRACE && !conn->error) {
        handleTraceMethod(conn);
    }
}


static void readyPass(HttpQueue *q)
{
    httpFinalize(q->conn);
    httpScheduleQueue(q);
}


static void readyError(HttpQueue *q)
{
    if (!q->conn->error) {
        httpError(q->conn, HTTP_CODE_NOT_FOUND, "The requested resource is not available");
    }
    httpFinalize(q->conn);
    httpScheduleQueue(q);
}


PUBLIC void httpHandleOptions(HttpConn *conn)
{
    httpSetHeaderString(conn, "Allow", httpGetRouteMethods(conn->rx->route));
    httpFinalize(conn);
}


static void handleTraceMethod(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q;
    HttpPacket  *traceData;

    tx = conn->tx;
    q = conn->writeq;


    /*
        Create a dummy set of headers to use as the response body. Then reset so the connector will create
        the headers in the normal fashion. Need to be careful not to have a content length in the headers in the body.
     */
    tx->flags |= HTTP_TX_NO_LENGTH;
    httpDiscardData(conn, HTTP_QUEUE_TX);
    traceData = httpCreateDataPacket(q->packetSize);
    httpCreateHeaders1(q, traceData);
    tx->flags &= ~(HTTP_TX_NO_LENGTH | HTTP_TX_HEADERS_CREATED);

    httpSetContentType(conn, "message/http");
    httpPutPacketToNext(q, traceData);
    httpFinalize(conn);
}

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
