/*
    netConnector.c -- General network connector. 

    The Network connector handles output data (only) from upstream handlers and filters. It uses vectored writes to
    aggregate output packets into fewer actual I/O requests to the O/S. 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/**************************** Forward Declarations ****************************/

static void addPacketForNet(HttpQueue *q, HttpPacket *packet);
static void adjustNetVec(HttpQueue *q, ssize written);
static MprOff buildNetVec(HttpQueue *q);
static void freeNetPackets(HttpQueue *q, ssize written);
static void netClose(HttpQueue *q);
static void netOutgoingService(HttpQueue *q);

/*********************************** Code *************************************/
/*  
    Initialize the net connector
 */
PUBLIC int httpOpenNetConnector(Http *http)
{
    HttpStage     *stage;

    mprTrace(5, "Open net connector");
    if ((stage = httpCreateConnector(http, "netConnector", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->close = netClose;
    stage->outgoingService = netOutgoingService;
    http->netConnector = stage;
    return 0;
}


static void netClose(HttpQueue *q)
{
    HttpTx      *tx;

    tx = q->conn->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


static void netOutgoingService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    ssize       written;
    int         errCode;

    conn = q->conn;
    tx = conn->tx;
    conn->lastActivity = conn->http->now;
    assert(conn->sock);
    
    if (!conn->sock || tx->finalizedConnector) {
        return;
    }
    if (tx->flags & HTTP_TX_NO_BODY) {
        httpDiscardQueueData(q, 1);
    }
    if ((tx->bytesWritten + q->count) > conn->limits->transmissionBodySize) {
        httpError(conn, HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
            "Http transmission aborted. Exceeded transmission max body of %,Ld bytes", conn->limits->transmissionBodySize);
        if (tx->bytesWritten) {
            httpFinalizeConnector(conn);
            return;
        }
    }
#if !BIT_ROM
    if (tx->flags & HTTP_TX_SENDFILE) {
        /* Relay via the send connector */
        if (tx->file == 0) {
            if (tx->flags & HTTP_TX_HEADERS_CREATED) {
                tx->flags &= ~HTTP_TX_SENDFILE;
            } else {
                tx->connector = conn->http->sendConnector;
                httpSendOpen(q);
            }
        }
        if (tx->file) {
            httpSendOutgoingService(q);
            return;
        }
    }
#endif
    while (q->first || q->ioIndex) {
        if (q->ioIndex == 0 && buildNetVec(q) <= 0) {
            break;
        }
        /*  
            Issue a single I/O request to write all the blocks in the I/O vector
         */
        assert(q->ioIndex > 0);
        written = mprWriteSocketVector(conn->sock, q->iovec, q->ioIndex);
        if (written < 0) {
            errCode = mprGetError();
            mprTrace(6, "netConnector: wrote %d, errno %d, qflags %x", (int) written, errCode, q->flags);
            if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
                /*  Socket full, wait for an I/O event */
                httpSocketBlocked(conn);
                break;
            }
            if (errCode == EPROTO && conn->secure) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Can't negotiate SSL with server: %s",
                    conn->sock->errorMsg);
            } else if (errCode != EPIPE && errCode != ECONNRESET && errCode != ECONNABORTED && errCode != ENOTCONN) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "netConnector: Can't write. errno %d", errCode);
            } else {
                httpDisconnect(conn);
            }
            httpFinalizeConnector(conn);
            break;

        } else if (written > 0) {
            mprTrace(6, "netConnector: wrote %d, ask %d, qflags %x", (int) written, q->ioCount, q->flags);
            tx->bytesWritten += written;
            freeNetPackets(q, written);
            adjustNetVec(q, written);

        } else {
            break;
        }
    }
    if (q->first && q->first->flags & HTTP_PACKET_END) {
        mprTrace(6, "netConnector: end of stream. Finalize connector");
        httpFinalizeConnector(conn);
    } else {
        HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
    }
}


/*
    Build the IO vector. Return the count of bytes to be written. Return -1 for EOF.
 */
static MprOff buildNetVec(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    HttpPacket  *packet, *prev;

    conn = q->conn;
    tx = conn->tx;

    /*
        Examine each packet and accumulate as many packets into the I/O vector as possible. Leave the packets on the queue 
        for now, they are removed after the IO is complete for the entire packet.
     */
    for (packet = prev = q->first; packet && !(packet->flags & HTTP_PACKET_END); packet = packet->next) {
        if (packet->flags & HTTP_PACKET_HEADER) {
            if (tx->chunkSize <= 0 && q->count > 0 && tx->length < 0) {
                /* Incase no chunking filter and we've not seen all the data yet */
                conn->keepAliveCount = 0;
            }
            httpWriteHeaders(q, packet);
        }
        if (q->ioIndex >= (BIT_MAX_IOVEC - 2)) {
            break;
        }
        if (httpGetPacketLength(packet) > 0 || packet->prefix) {
            addPacketForNet(q, packet);
        } else {
            /* Remove empty packets */
            prev->next = packet->next;
            continue;
        }
        prev = packet;
    }
    return q->ioCount;
}


/*
    Add one entry to the io vector
 */
static void addToNetVector(HttpQueue *q, char *ptr, ssize bytes)
{
    assert(bytes > 0);

    q->iovec[q->ioIndex].start = ptr;
    q->iovec[q->ioIndex].len = bytes;
    q->ioCount += bytes;
    q->ioIndex++;
}


/*
    Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForNet(HttpQueue *q, HttpPacket *packet)
{
    HttpTx      *tx;
    HttpConn    *conn;
    int         item;

    conn = q->conn;
    tx = conn->tx;

    assert(q->count >= 0);
    assert(q->ioIndex < (BIT_MAX_IOVEC - 2));

    if (packet->prefix) {
        addToNetVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (httpGetPacketLength(packet) > 0) {
        addToNetVector(q, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
    }
    item = (packet->flags & HTTP_PACKET_HEADER) ? HTTP_TRACE_HEADER : HTTP_TRACE_BODY;
    if (httpShouldTrace(conn, HTTP_TRACE_TX, item, tx->ext) >= 0) {
        httpTraceContent(conn, HTTP_TRACE_TX, item, packet, 0, (ssize) tx->bytesWritten);
    }
}


static void freeNetPackets(HttpQueue *q, ssize bytes)
{
    HttpPacket  *packet;
    ssize       len;

    assert(q->count >= 0);
    assert(bytes > 0);

    /*
        Loop while data to be accounted for and we have not hit the end of data packet.
        Chunks will have the chunk header in the packet->prefix.
        The final chunk trailer will be in a packet->prefix with no other data content.
        Must leave this routine with the end packet still on the queue and all bytes accounted for.
     */
    while ((packet = q->first) != 0 && !(packet->flags & HTTP_PACKET_END) && bytes > 0) {
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            len = min(len, bytes);
            mprAdjustBufStart(packet->prefix, len);
            bytes -= len;
            /* Prefixes don't count in the q->count. No need to adjust */
            if (mprGetBufLength(packet->prefix) == 0) {
                packet->prefix = 0;
            }
        }
        if (packet->content) {
            len = mprGetBufLength(packet->content);
            len = min(len, bytes);
            mprAdjustBufStart(packet->content, len);
            bytes -= len;
            q->count -= len;
            assert(q->count >= 0);
        }
        if (httpGetPacketLength(packet) == 0) {
            /* Done with this packet - consume it */
            assert(!(packet->flags & HTTP_PACKET_END));
            httpGetPacket(q);
        } else {
            break;
        }
    }
    assert(bytes == 0);
}


/*
    Clear entries from the IO vector that have actually been transmitted. Support partial writes.
 */
static void adjustNetVec(HttpQueue *q, ssize written)
{
    MprIOVec    *iovec;
    ssize       len;
    int         i, j;

    /*
        Cleanup the IO vector
     */
    if (written == q->ioCount) {
        /*
            Entire vector written. Just reset.
         */
        q->ioIndex = 0;
        q->ioCount = 0;

    } else {
        /*
            Partial write of an vector entry. Need to copy down the unwritten vector entries.
         */
        q->ioCount -= written;
        assert(q->ioCount >= 0);
        iovec = q->iovec;
        for (i = 0; i < q->ioIndex; i++) {
            len = iovec[i].len;
            if (written < len) {
                iovec[i].start += written;
                iovec[i].len -= written;
                break;
            } else {
                written -= len;
            }
        }
        /*
            Compact
         */
        for (j = 0; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex = j;
    }
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
