/*
    tailFilter.c -- Filter for the start/end of request pipeline.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static HttpPacket *createAltBodyPacket(HttpQueue *q);
static void incomingTail(HttpQueue *q, HttpPacket *packet);
static void outgoingTail(HttpQueue *q, HttpPacket *packet);
static void outgoingTailService(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC int httpOpenTailFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("tailFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->tailFilter = filter;
    filter->incoming = incomingTail;
    filter->outgoing = outgoingTail;
    filter->outgoingService = outgoingTailService;
    return 0;
}


static void incomingTail(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;

    conn = q->conn;
    rx = conn->rx;
    MprBuf *bp = packet->content;

    /*
        Detect Rx EOF. If using HTTP/1 chunk encoded input, the ChunkFilter will do this.
        Otherwise we're responsibile here to detect EOF.
     */
    if (q->net->eof) {
        httpSetEof(conn);
    }
    if (rx->chunkState == HTTP_CHUNK_UNCHUNKED) {
        rx->remainingContent -= httpGetPacketLength(packet);
        if (rx->remainingContent <= 0) {
            httpSetEof(conn);
        }
    }
    httpPutPacketToNext(q, packet);
    if (rx->eof) {
        httpPutPacketToNext(q, httpCreateEndPacket());
    }
    if (conn->readq->first) {
        bp = conn->readq->first->content;
        HTTP_NOTIFY(conn, HTTP_EVENT_READABLE, 0);
    }
}


static void outgoingTail(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpTx      *tx;
    HttpPacket  *headers, *tail;

    conn = q->conn;
    tx = conn->tx;
    net = q->net;
    conn->lastActivity = conn->http->now;

    if (!(tx->flags & HTTP_TX_HEADERS_CREATED)) {
        headers = httpCreateHeaders(q, NULL);
        while (httpGetPacketLength(headers) > net->outputq->packetSize) {
            tail = httpSplitPacket(headers, net->outputq->packetSize);
            httpPutForService(q, headers, 1);
            headers = tail;
        }
        httpPutForService(q, headers, 1);
        if (tx->altBody) {
            httpPutForService(q, createAltBodyPacket(q), 1);
        }
    }
    if (packet->flags & HTTP_PACKET_DATA) {
        tx->bytesWritten += httpGetPacketLength(packet);
        if (tx->bytesWritten > conn->limits->txBodySize) {
            httpLimitError(conn, HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
                "Http transmission aborted. Exceeded transmission max body of %lld bytes", conn->limits->txBodySize);
        }
    }
    httpPutForService(q, packet, 1);
}


static void outgoingTailService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpNet     *net;
    HttpPacket  *packet;

    conn = q->conn;
    net = conn->net;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!httpWillQueueAcceptPacket(q, net->outputq, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        httpPutPacket(net->outputq, packet);
    }
}


/*
    Create an alternate response body for error responses.
 */
static HttpPacket *createAltBodyPacket(HttpQueue *q)
{
    HttpTx      *tx;
    HttpPacket  *packet;

    tx = q->conn->tx;
    packet = httpCreateDataPacket(slen(tx->altBody));
    mprPutStringToBuf(packet->content, tx->altBody);
    return packet;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
