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
    HttpStream  *stream;
    HttpRx      *rx;
    ssize       count;

    stream = q->stream;
    rx = stream->rx;

    if (q->net->eof && !rx->eof) {
        httpSetEof(stream);
    }
    count = stream->readq->count + httpGetPacketLength(packet);
    if ((rx->form || !rx->streaming) && count >= stream->limits->rxFormSize && stream->limits->rxFormSize != HTTP_UNLIMITED) {
        httpLimitError(stream, HTTP_CLOSE | HTTP_CODE_REQUEST_TOO_LARGE,
            "Request form of %ld bytes is too big. Limit %lld", count, stream->limits->rxFormSize);
    } else {
        httpPutPacketToNext(q, packet);
    }
    if (rx->eof) {
        httpAddEndInputPacket(stream);
    }
    if (rx->route && stream->readq->first) {
        HTTP_NOTIFY(stream, HTTP_EVENT_READABLE, 0);
    }
}


static void outgoingTail(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream  *stream;
    HttpTx      *tx;
    HttpPacket  *headers, *tail;

    stream = q->stream;
    tx = stream->tx;
    net = q->net;
    stream->lastActivity = stream->http->now;

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
        if (tx->bytesWritten > stream->limits->txBodySize) {
            httpLimitError(stream, HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
                "Http transmission aborted. Exceeded transmission max body of %lld bytes", stream->limits->txBodySize);
        }
    }
    httpPutForService(q, packet, 1);
}


static bool streamCanAbsorb(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpQueue   *nextQ;
    ssize       room, size;

    stream = q->stream;
    nextQ = stream->net->outputq;
    size = httpGetPacketLength(packet);

    /*
        Get the maximum the output stream can absorb that is less than the downstream queue packet size.
     */
#if ME_HTTP_HTTP2
    room = min(nextQ->packetSize, stream->outputq->window);
#else
    room = min(nextQ->packetSize, stream->outputq->max);
#endif
    if (size <= room) {
        return 1;
    }
    if (room > 0) {
        /*
            Resize the packet to fit downstream. This will putback the tail if required.
         */
        httpResizePacket(q, packet, room);
        size = httpGetPacketLength(packet);
        assert(size <= room);
        assert(size <= nextQ->packetSize);
        if (size > 0) {
            return 1;
        }
    }
    /*
        The downstream queue cannot accept this packet, so suspend this queue and schedule the next if required.
     */
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


static void outgoingTailService(HttpQueue *q)
{
    HttpPacket  *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!streamCanAbsorb(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        if (!httpWillQueueAcceptPacket(q, q->net->outputq, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        httpPutPacket(q->net->outputq, packet);
    }
}


/*
    Create an alternate response body for error responses.
 */
static HttpPacket *createAltBodyPacket(HttpQueue *q)
{
    HttpTx      *tx;
    HttpPacket  *packet;

    tx = q->stream->tx;
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
