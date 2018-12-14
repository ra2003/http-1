/*
    chunkFilter.c - Transfer chunk endociding filter.

    This is an output only filter to chunk encode output before writing to the client.
    Input chunking is handled in httpProcess()/processContent(). In the future, it would
    be nice to move that functionality here as an input filter.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void incomingChunk(HttpQueue *q, HttpPacket *packet);
static bool needChunking(HttpQueue *q);
static void outgoingChunkService(HttpQueue *q);
static void setChunkPrefix(HttpQueue *q, HttpPacket *packet);

/*********************************** Code *************************************/
/*
   Loadable module initialization
 */
PUBLIC int httpOpenChunkFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("chunkFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->chunkFilter = filter;
    filter->flags |= HTTP_STAGE_INTERNAL;
    filter->incoming = incomingChunk;
    filter->outgoingService = outgoingChunkService;
    return 0;
}


PUBLIC void httpInitChunking(HttpStream *stream)
{
    HttpRx      *rx;

    rx = stream->rx;

    /*
        remainingContent will be revised by the chunk filter as chunks are processed and will
        be set to zero when the last chunk has been received.
     */
    rx->flags |= HTTP_CHUNKED;
    rx->chunkState = HTTP_CHUNK_START;
    rx->remainingContent = HTTP_UNLIMITED;
    rx->needInputPipeline = 1;
}


/*
    Filter chunk headers and leave behind pure data. This is called for chunked and unchunked data.
    Unchunked data is simply passed upstream. Chunked data format is:
        Chunk spec <CRLF>
        Data <CRLF>
        Chunk spec (size == 0) <CRLF>
        <CRLF>
    Chunk spec is: "HEX_COUNT; chunk length DECIMAL_COUNT\r\n". The "; chunk length DECIMAL_COUNT is optional.
    As an optimization, use "\r\nSIZE ...\r\n" as the delimiter so that the CRLF after data does not special consideration.
    Achive this by parseHeaders reversing the input start by 2.

    Return number of bytes available to read.
    NOTE: may set rx->eof and return 0 bytes on EOF.
 */

static void incomingChunk(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;
    HttpPacket  *tail;
    HttpRx      *rx;
    MprBuf      *buf;
    ssize       chunkSize, len, nbytes;
    char        *start, *cp;
    int         bad;

    stream = q->stream;
    rx = stream->rx;

    if (rx->chunkState == HTTP_CHUNK_UNCHUNKED) {
        len = httpGetPacketLength(packet);
        nbytes = min(rx->remainingContent, httpGetPacketLength(packet));
        rx->remainingContent -= nbytes;
        if (rx->remainingContent <= 0) {
            httpSetEof(stream);
#if HTTP_PIPELINING
            /* HTTP/1.1 pipelining is not implemented reliably by modern browsers */
            if (nbytes < len && (tail = httpSplitPacket(packet, nbytes)) != 0) {
                httpPutPacket(stream->inputq, tail);
            }
#endif
        }
        httpPutPacketToNext(q, packet);
        return;
    }
    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
    for (packet = httpGetPacket(q); packet && !stream->error && !rx->eof; packet = httpGetPacket(q)) {
        while (packet && !stream->error && !rx->eof) {
            switch (rx->chunkState) {
            case HTTP_CHUNK_UNCHUNKED:
                httpError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk state");
                return;

            case HTTP_CHUNK_DATA:
                len = httpGetPacketLength(packet);
                nbytes = min(rx->remainingContent, len);
                rx->remainingContent -= nbytes;
                if (nbytes < len && (tail = httpSplitPacket(packet, nbytes)) != 0) {
                    httpPutPacketToNext(q, packet);
                    packet = tail;
                } else if (len > 0) {
                    httpPutPacketToNext(q, packet);
                    packet = 0;
                }
                if (rx->remainingContent <= 0) {
                    /* End of chunk - prep for the next chunk */
                    rx->remainingContent = ME_BUFSIZE;
                    rx->chunkState = HTTP_CHUNK_START;
                }
                if (!packet) {
                    break;
                }
                /* Fall through */

            case HTTP_CHUNK_START:
                /*
                    Validate:  "\r\nSIZE.*\r\n"
                 */
                buf = packet->content;
                if (mprGetBufLength(buf) < 5) {
                    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
                    return;
                }
                start = mprGetBufStart(buf);
                bad = (start[0] != '\r' || start[1] != '\n');
                for (cp = &start[2]; cp < buf->end && *cp != '\n'; cp++) {}
                if (cp >= buf->end || (*cp != '\n' && (cp - start) < 80)) {
                    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
                    return;
                }
                bad += (cp[-1] != '\r' || cp[0] != '\n');
                if (bad) {
                    httpError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
                    return;
                }
                chunkSize = (int) stoiradix(&start[2], 16, NULL);
                if (!isxdigit((uchar) start[2]) || chunkSize < 0) {
                    httpError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
                    return;
                }
                if (chunkSize == 0) {
                    /*
                        Last chunk. Consume the final "\r\n".
                     */
                    if ((cp + 2) >= buf->end) {
                        return;
                    }
                    cp += 2;
                    bad += (cp[-1] != '\r' || cp[0] != '\n');
                    if (bad) {
                        httpError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad final chunk specification");
                        return;
                    }
                }
                mprAdjustBufStart(buf, (cp - start + 1));
                /* Remaining content is set to the next chunk size */
                rx->remainingContent = chunkSize;
                if (chunkSize == 0) {
                    rx->chunkState = HTTP_CHUNK_EOF;
                    httpSetEof(stream);
                } else if (rx->eof) {
                    rx->chunkState = HTTP_CHUNK_EOF;
                } else {
                    rx->chunkState = HTTP_CHUNK_DATA;
                }
                break;

            default:
                httpError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk state %d", rx->chunkState);
                return;
            }
        }
#if HTTP_PIPELINING
        /* HTTP/1.1 pipelining is not implemented reliably by modern browsers */
        if (packet && httpGetPacketLength(packet)) {
            httpPutPacket(stream->inputq, tail);
        }
#endif
    }
    if (packet) {
        /* Transfer END packet */
        httpPutPacketToNext(q, packet);
    }
}


static void outgoingChunkService(HttpQueue *q)
{
    HttpStream  *stream;
    HttpPacket  *packet, *finalChunk;
    HttpTx      *tx;

    stream = q->stream;
    tx = stream->tx;

    if (!(q->flags & HTTP_QUEUE_SERVICED)) {
        tx->needChunking = needChunking(q);
    }
    if (!tx->needChunking) {
        httpDefaultOutgoingServiceStage(q);
        return;
    }
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (packet->flags & HTTP_PACKET_DATA) {
            httpPutBackPacket(q, packet);
            httpJoinPackets(q, tx->chunkSize);
            packet = httpGetPacket(q);
            if (httpGetPacketLength(packet) > tx->chunkSize) {
                httpResizePacket(q, packet, tx->chunkSize);
            }
        }
        if (!httpWillNextQueueAcceptPacket(q, packet)) {
            httpPutBackPacket(q, packet);
            return;
        }
        if (packet->flags & HTTP_PACKET_DATA) {
            setChunkPrefix(q, packet);

        } else if (packet->flags & HTTP_PACKET_END) {
            /* Insert a packet for the final chunk */
            finalChunk = httpCreateDataPacket(0);
            setChunkPrefix(q, finalChunk);
            httpPutPacketToNext(q, finalChunk);
        }
        httpPutPacketToNext(q, packet);
    }
}


static bool needChunking(HttpQueue *q)
{
    HttpStream  *stream;
    HttpTx      *tx;
    cchar       *value;

    stream = q->stream;
    tx = stream->tx;

    if (stream->net->protocol >= 2 || stream->upgraded) {
        return 0;
    }
    /*
        If we don't know the content length yet (tx->length < 0) and if the last packet is the end packet. Then
        we have all the data. Thus we can determine the actual content length and can bypass the chunk handler.
     */
    if (tx->length < 0 && (value = mprLookupKey(tx->headers, "Content-Length")) != 0) {
        tx->length = stoi(value);
    }
    if (tx->length < 0 && tx->chunkSize < 0) {
        if (q->last->flags & HTTP_PACKET_END) {
            if (q->count > 0) {
                tx->length = q->count;
            }
        } else {
            tx->chunkSize = min(stream->limits->chunkSize, q->max);
        }
    }
    if (tx->flags & HTTP_TX_USE_OWN_HEADERS || stream->net->protocol != 1) {
        tx->chunkSize = -1;
    }
    return tx->chunkSize > 0;
}


static void setChunkPrefix(HttpQueue *q, HttpPacket *packet)
{
    if (packet->prefix) {
        return;
    }
    packet->prefix = mprCreateBuf(32, 32);
    /*
        NOTE: prefixes don't count in the queue length. No need to adjust q->count
     */
    if (httpGetPacketLength(packet)) {
        mprPutToBuf(packet->prefix, "\r\n%zx\r\n", httpGetPacketLength(packet));
    } else {
        mprPutStringToBuf(packet->prefix, "\r\n0\r\n\r\n");
    }
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
