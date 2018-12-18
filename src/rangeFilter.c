/*
    rangeFilter.c - Ranged request filter.

    This is an output only filter to select a subet range of data to transfer to the client.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Defines ***********************************/

#define HTTP_RANGE_BUFSIZE 128              /* Packet size to hold range boundary */

/********************************** Forwards **********************************/

static HttpPacket *selectBytes(HttpQueue *q, HttpPacket *packet);
static void createRangeBoundary(HttpStream *stream);
static HttpPacket *createRangePacket(HttpStream *stream, HttpRange *range);
static HttpPacket *createFinalRangePacket(HttpStream *stream);
static void manageRange(HttpRange *range, int flags);
static void outgoingRangeService(HttpQueue *q);
static bool fixRangeLength(HttpStream *stream, HttpQueue *q);
static int matchRange(HttpStream *stream, HttpRoute *route, int dir);
static void startRange(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC int httpOpenRangeFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("rangeFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->rangeFilter = filter;
    filter->match = matchRange;
    filter->start = startRange;
    filter->outgoingService = outgoingRangeService;
    return 0;
}


PUBLIC HttpRange *httpCreateRange(HttpStream *stream, MprOff start, MprOff end)
{
    HttpRange     *range;

    if ((range = mprAllocObj(HttpRange, manageRange)) == 0) {
        return 0;
    }
    range->start = start;
    range->end = end;
    range->len = end - start;
    return range;
}


static void manageRange(HttpRange *range, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(range->next);
    }
}


/*
    This is called twice: once for TX and once for RX
 */
static int matchRange(HttpStream *stream, HttpRoute *route, int dir)
{
    assert(stream->rx);

    httpSetHeader(stream, "Accept-Ranges", "bytes");
    if ((dir & HTTP_STAGE_TX) && stream->tx->outputRanges) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


static void startRange(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;

    stream = q->stream;
    tx = stream->tx;
    /*
        The httpContentNotModified routine can set outputRanges to zero if returning not-modified.
     */
    if (tx->outputRanges == 0 || tx->status != HTTP_CODE_OK) {
        httpRemoveQueue(q);
        tx->outputRanges = 0;
    } else {
        tx->status = HTTP_CODE_PARTIAL;
        /*
            More than one range so create a range boundary (like chunking)
         */
        if (tx->outputRanges->next) {
            createRangeBoundary(stream);
        }
    }
}


static void outgoingRangeService(HttpQueue *q)
{
    HttpPacket  *packet;
    HttpStream  *stream;
    HttpTx      *tx;

    stream = q->stream;
    tx = stream->tx;

    if (!(q->flags & HTTP_QUEUE_SERVICED)) {
        /*
            The httpContentNotModified routine can set outputRanges to zero if returning not-modified.
         */
        if (!fixRangeLength(stream, q)) {
            if (!q->servicing) {
                httpRemoveQueue(q);
            }
            tx->outputRanges = 0;
            tx->status = HTTP_CODE_OK;
        }
    }
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (packet->flags & HTTP_PACKET_DATA) {
            if ((packet = selectBytes(q, packet)) == 0) {
                continue;
            }
        } else if (packet->flags & HTTP_PACKET_END) {
            if (tx->rangeBoundary) {
                httpPutPacketToNext(q, createFinalRangePacket(stream));
            }
        }
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        httpPutPacketToNext(q, packet);
    }
}


static HttpPacket *selectBytes(HttpQueue *q, HttpPacket *packet)
{
    HttpRange   *range;
    HttpStream  *stream;
    HttpTx      *tx;
    MprOff      endPacket, length, gap, span;
    ssize       count;

    stream = q->stream;
    tx = stream->tx;

    if ((range = tx->currentRange) == 0) {
        return 0;
    }

    /*
        Process the data packet over multiple ranges ranges until all the data is processed or discarded.
     */
    while (range && packet) {
        length = httpGetPacketLength(packet);
        if (length <= 0) {
            return 0;
        }
        endPacket = tx->rangePos + length;
        if (endPacket < range->start) {
            /* Packet is before the next range, so discard the entire packet and seek forwards */
            tx->rangePos += length;
            return 0;

        } else if (tx->rangePos < range->start) {
            /*  Packet starts before range so skip some data, but some packet data is in range */
            gap = (range->start - tx->rangePos);
            tx->rangePos += gap;
            if (gap < length) {
                httpAdjustPacketStart(packet, (ssize) gap);
            }
            if (tx->rangePos >= range->end) {
                range = tx->currentRange = range->next;
            }
            /* Keep going and examine next range */

        } else {
            /* In range */
            assert(range->start <= tx->rangePos && tx->rangePos < range->end);
            span = min(length, (range->end - tx->rangePos));
            span = max(span, 0);
            count = (ssize) min(span, q->nextQ->packetSize);
            assert(count > 0);
            if (length > count) {
                /* Split packet if packet extends past range */
                httpPutBackPacket(q, httpSplitPacket(packet, count));
            }
            if (tx->rangeBoundary) {
                httpPutPacketToNext(q, createRangePacket(stream, range));
            }
            tx->rangePos += count;
            if (tx->rangePos >= range->end) {
                tx->currentRange = range->next;
            }
            break;
        }
    }
    return packet;
}


/*
    Create a range boundary packet
 */
static HttpPacket *createRangePacket(HttpStream *stream, HttpRange *range)
{
    HttpPacket  *packet;
    HttpTx      *tx;
    char        *length;

    tx = stream->tx;

    length = (tx->entityLength >= 0) ? itos(tx->entityLength) : "*";
    packet = httpCreatePacket(HTTP_RANGE_BUFSIZE);
    packet->flags |= HTTP_PACKET_RANGE | HTTP_PACKET_DATA;
    mprPutToBuf(packet->content,
        "\r\n--%s\r\n"
        "Content-Range: bytes %lld-%lld/%s\r\n\r\n",
        tx->rangeBoundary, range->start, range->end - 1, length);
    return packet;
}


/*
    Create a final range packet that follows all the data
 */
static HttpPacket *createFinalRangePacket(HttpStream *stream)
{
    HttpPacket  *packet;
    HttpTx      *tx;

    tx = stream->tx;

    packet = httpCreatePacket(HTTP_RANGE_BUFSIZE);
    packet->flags |= HTTP_PACKET_RANGE | HTTP_PACKET_DATA;
    mprPutToBuf(packet->content, "\r\n--%s--\r\n", tx->rangeBoundary);
    return packet;
}


/*
    Create a range boundary. This is required if more than one range is requested.
 */
static void createRangeBoundary(HttpStream *stream)
{
    HttpTx      *tx;
    int         when;

    tx = stream->tx;
    assert(tx->rangeBoundary == 0);
    when = (int) stream->http->now;
    tx->rangeBoundary = sfmt("%08X%08X", PTOI(tx) + PTOI(stream) * when, when);
}


/*
    Ensure all the range limits are within the entity size limits. Fixup negative ranges.
 */
static bool fixRangeLength(HttpStream *stream, HttpQueue *q)
{
    HttpTx      *tx;
    HttpRange   *range;
    MprOff      length;
    cchar       *value;

    tx = stream->tx;
    length = tx->entityLength ? tx->entityLength : tx->length;
    if (length <= 0) {
        if ((value = mprLookupKey(tx->headers, "Content-Length")) != 0) {
            length = stoi(value);
        }
        if (length < 0 && tx->chunkSize < 0) {
            if (q->last->flags & HTTP_PACKET_END) {
                if (q->count > 0) {
                    length = q->count;
                }
            }
        }
        if (length < 0) {
            return 0;
        }
    }
    for (range = tx->outputRanges; range; range = range->next) {
        /*
                Range: 0-49             first 50 bytes
                Range: 50-99,200-249    Two 50 byte ranges from 50 and 200
                Range: -50              Last 50 bytes
                Range: 1-               Skip first byte then emit the rest
         */
        if (length) {
            if (range->end > length) {
                range->end = length;
            }
            if (range->start > length) {
                range->start = length;
            }
        }
        if (range->start < 0) {
            if (length <= 0) {
                /*
                    Cannot compute an offset from the end as we don't know the entity length and it is not
                    always possible or wise to buffer all the output.
                 */
                return 0;
            }
            /* select last -range-end bytes */
            range->start = length - range->end + 1;
            range->end = length;
        }
        if (range->end < 0) {
            if (length <= 0) {
                return 0;
            }
            range->end = length - range->end - 1;
        }
        range->len = (int) (range->end - range->start);
    }
    return 1;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
