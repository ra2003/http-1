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

static int matchChunk(HttpConn *conn, HttpRoute *route, int dir);
static int openChunk(HttpQueue *q);
static void incomingChunk(HttpQueue *q, HttpPacket *packet);
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
    filter->match = matchChunk;
    filter->open = openChunk;
    filter->incoming = incomingChunk;
    filter->outgoingService = outgoingChunkService;
    return 0;
}


/*
    This is called twice: once for TX and once for RX
 */
static int matchChunk(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpRx  *rx;
    HttpTx  *tx;

    rx = conn->rx;
    tx = conn->tx;

    if (conn->net->protocol == 2 || conn->upgraded || (tx->parsedUri && tx->parsedUri->webSockets)) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (dir & HTTP_STAGE_TX) {
        /*
            If content length is defined, don't need chunking - but only if chunking not explicitly asked for.
            Disable chunking if explicitly turned off via the X_APPWEB_CHUNK_SIZE header which may set the
            chunk size to zero.
         */
        if ((tx->length >= 0 && tx->chunkSize < 0) || tx->chunkSize == 0) {
            return HTTP_ROUTE_OMIT_FILTER;
        }
    } else {
        if (conn->state >= HTTP_STATE_PARSED && rx->chunkState == HTTP_CHUNK_UNCHUNKED) {
            return HTTP_ROUTE_OMIT_FILTER;
        }
    }
    return HTTP_ROUTE_OK;
}


static int openChunk(HttpQueue *q)
{
    q->packetSize = min(q->conn->limits->bufferSize, q->max);
    return 0;
}


PUBLIC void httpInitChunking(HttpConn *conn)
{
    HttpRx      *rx;

    rx = conn->rx;

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
    Chunked data format is:
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
    HttpNet     *net;
    HttpConn    *conn;
    HttpPacket  *tail;
    HttpRx      *rx;
    MprBuf      *buf;
    ssize       chunkSize, len, nbytes;
    char        *start, *cp;
    int         bad;

    conn = q->conn;
    net = q->net;
    rx = conn->rx;

    if (rx->chunkState == HTTP_CHUNK_UNCHUNKED) {
        httpPutPacketToNext(q, packet);
        return;
    }

    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
    for (packet = httpGetPacket(q); packet && !conn->error && !rx->eof; packet = httpGetPacket(q)) {
        while (packet && !conn->error && !rx->eof) {
            switch (rx->chunkState) {
            case HTTP_CHUNK_UNCHUNKED:
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk state");
                return;

            case HTTP_CHUNK_DATA:
                len = httpGetPacketLength(packet);
                nbytes = min(rx->remainingContent, len);
                rx->remainingContent -= nbytes;
                if (nbytes < len && (tail = httpSplitPacket(packet, nbytes)) != 0) {
                    httpPutPacketToNext(q, packet);
                    packet = tail;
                } else {
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
                    httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
                    return;
                }
                chunkSize = (int) stoiradix(&start[2], 16, NULL);
                if (!isxdigit((uchar) start[2]) || chunkSize < 0) {
                    httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk specification");
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
                        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad final chunk specification");
                        return;
                    }
                }
                mprAdjustBufStart(buf, (cp - start + 1));
                /* Remaining content is set to the next chunk size */
                rx->remainingContent = chunkSize;
                if (chunkSize == 0) {
                    rx->chunkState = HTTP_CHUNK_EOF;
                    httpSetEof(conn);
                } else if (rx->eof) {
                    rx->chunkState = HTTP_CHUNK_EOF;
                } else {
                    rx->chunkState = HTTP_CHUNK_DATA;
                }
                break;

            default:
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad chunk state %d", rx->chunkState);
                return;
            }
        }
    }
    if (packet) {
        /* Transfer END packet */
        httpPutPacketToNext(q, packet);
    }
}


static void outgoingChunkService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet, *finalChunk;
    HttpTx      *tx;
    cchar       *value;

    conn = q->conn;
    tx = conn->tx;

    if (!(q->flags & HTTP_QUEUE_SERVICED)) {
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
                tx->chunkSize = min(conn->limits->chunkSize, q->max);
            }
        }
        if (tx->flags & HTTP_TX_USE_OWN_HEADERS || conn->net->protocol != 1) {
            tx->chunkSize = -1;
        }
    }
    if (tx->chunkSize <= 0 || conn->upgraded) {
        httpDefaultOutgoingServiceStage(q);
    } else {
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
