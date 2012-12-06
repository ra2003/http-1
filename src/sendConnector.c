/*
    sendConnector.c -- Send file connector. 

    The Sendfile connector supports the optimized transmission of whole static files. It uses operating system 
    sendfile APIs to eliminate reading the document into user space and multiple socket writes. The send connector 
    is not a general purpose connector. It cannot handle dynamic data or ranged requests. It does support chunked requests.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/**************************** Forward Declarations ****************************/
#if !BIT_ROM

static void addPacketForSend(HttpQueue *q, HttpPacket *packet);
static void adjustSendVec(HttpQueue *q, MprOff written);
static MprOff buildSendVec(HttpQueue *q);
static void freeSendPackets(HttpQueue *q, MprOff written);
static void sendClose(HttpQueue *q);

/*********************************** Code *************************************/

PUBLIC int httpOpenSendConnector(Http *http)
{
    HttpStage     *stage;

    mprLog(5, "Open send connector");
    if ((stage = httpCreateConnector(http, "sendConnector", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->open = httpSendOpen;
    stage->close = sendClose;
    stage->outgoingService = httpSendOutgoingService; 
    http->sendConnector = stage;
    return 0;
}


/*  
    Initialize the send connector for a request
 */
PUBLIC void httpSendOpen(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    if (tx->connector != conn->http->sendConnector) {
        httpAssignQueue(q, tx->connector, HTTP_QUEUE_TX);
        tx->connector->open(q);
        return;
    }
    if (!(tx->flags & HTTP_TX_NO_BODY)) {
        assure(tx->fileInfo.valid);
        if (tx->fileInfo.size > conn->limits->transmissionBodySize) {
            httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE,
                "Http transmission aborted. File size exceeds max body of %,Ld bytes", conn->limits->transmissionBodySize);
            return;
        }
        tx->file = mprOpenFile(tx->filename, O_RDONLY | O_BINARY, 0);
        if (tx->file == 0) {
            httpError(conn, HTTP_CODE_NOT_FOUND, "Cannot open document: %s, err %d", tx->filename, mprGetError());
        }
    }
}


static void sendClose(HttpQueue *q)
{
    HttpTx  *tx;

    tx = q->conn->tx;
    if (tx->file) {
        mprCloseFile(tx->file);
        tx->file = 0;
    }
}


PUBLIC void httpSendOutgoingService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpTx      *tx;
    MprFile     *file;
    MprOff      written;
    int         errCode;

    conn = q->conn;
    tx = conn->tx;
    conn->lastActivity = conn->http->now;
    assure(conn->sock);

    if (!conn->sock || tx->finalizedConnector) {
        return;
    }
    if (tx->flags & HTTP_TX_NO_BODY) {
        httpDiscardQueueData(q, 1);
    }
    if ((tx->bytesWritten + q->ioCount) > conn->limits->transmissionBodySize) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_TOO_LARGE | ((tx->bytesWritten) ? HTTP_ABORT : 0),
            "Http transmission aborted. Exceeded max body of %,Ld bytes", conn->limits->transmissionBodySize);
        if (tx->bytesWritten) {
            httpFinalizeConnector(conn);
            return;
        }
    }
    if (q->ioIndex == 0) {
        buildSendVec(q);
    }
    /*
        No need to loop around as send file tries to write as much of the file as possible. 
        If not eof, will always have the socket blocked.
     */
    file = q->ioFile ? tx->file : 0;
    written = mprSendFileToSocket(conn->sock, file, q->ioPos, q->ioCount, q->iovec, q->ioIndex, NULL, 0);
    if (written < 0) {
        errCode = mprGetError();
        if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
            /*  Socket full, wait for an I/O event */
            httpSocketBlocked(conn);
        } else {
            if (errCode != EPIPE && errCode != ECONNRESET && errCode != ENOTCONN) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "sendConnector: error, errCode %d", errCode);
            } else {
                httpDisconnect(conn);
            }
            httpFinalizeConnector(conn);
        }
    } else if (written > 0) {
        tx->bytesWritten += written;
        freeSendPackets(q, written);
        adjustSendVec(q, written);
    }
    LOG(6, "sendConnector: wrote %d, qflags %x", (int) written, q->flags);
    if (q->first && q->first->flags & HTTP_PACKET_END) {
        LOG(6, "sendConnector: end of stream. Finalize connector");
        httpFinalizeConnector(conn);
    } else {
        HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
    }
}


/*  
    Build the IO vector. This connector uses the send file API which permits multiple IO blocks to be written with 
    file data. This is used to write transfer the headers and chunk encoding boundaries. Return the count of bytes to 
    be written. Return -1 for EOF.
 */
static MprOff buildSendVec(HttpQueue *q)
{
    HttpPacket  *packet, *prev;

    assure(q->ioIndex == 0);
    q->ioCount = 0;
    q->ioFile = 0;

    /*  
        Examine each packet and accumulate as many packets into the I/O vector as possible. Can only have one data packet at
        a time due to the limitations of the sendfile API (on Linux). And the data packet must be after all the 
        vector entries. Leave the packets on the queue for now, they are removed after the IO is complete for the 
        entire packet.
     */
    for (packet = prev = q->first; packet && !(packet->flags & HTTP_PACKET_END); packet = packet->next) {
        if (packet->flags & HTTP_PACKET_HEADER) {
            httpWriteHeaders(q, packet);
        }
        if (q->ioFile || q->ioIndex >= (HTTP_MAX_IOVEC - 2)) {
            /* Only one file entry allowed */
            break;
        }
        if (packet->prefix || packet->esize || httpGetPacketLength(packet) > 0) {
            addPacketForSend(q, packet);
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
static void addToSendVector(HttpQueue *q, char *ptr, ssize bytes)
{
    assure(ptr > 0);
    assure(bytes > 0);

    q->iovec[q->ioIndex].start = ptr;
    q->iovec[q->ioIndex].len = bytes;
    q->ioCount += bytes;
    q->ioIndex++;
}


/*  
    Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForSend(HttpQueue *q, HttpPacket *packet)
{
    HttpTx      *tx;
    HttpConn    *conn;
    int         item;

    conn = q->conn;
    tx = conn->tx;
    
    assure(q->count >= 0);
    assure(q->ioIndex < (HTTP_MAX_IOVEC - 2));

    if (packet->prefix) {
        addToSendVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (packet->esize > 0) {
        assure(q->ioFile == 0);
        q->ioFile = 1;
        q->ioCount += packet->esize;

    } else if (httpGetPacketLength(packet) > 0) {
        /*
            Header packets have actual content. File data packets are virtual and only have a count.
         */
        addToSendVector(q, mprGetBufStart(packet->content), httpGetPacketLength(packet));
        item = (packet->flags & HTTP_PACKET_HEADER) ? HTTP_TRACE_HEADER : HTTP_TRACE_BODY;
        if (httpShouldTrace(conn, HTTP_TRACE_TX, item, tx->ext) >= 0) {
            httpTraceContent(conn, HTTP_TRACE_TX, item, packet, 0, tx->bytesWritten);
        }
    }
}


static void freeSendPackets(HttpQueue *q, MprOff bytes)
{
    HttpPacket  *packet;
    ssize       len;

    assure(q->first);
    assure(q->count >= 0);
    assure(bytes >= 0);

    /*
        Loop while data to be accounted for and we have not hit the end of data packet
        There should be 2-3 packets on the queue. A header packet for the HTTP response headers, an optional
        data packet with packet->esize set to the size of the file, and an end packet with no content.
        Must leave this routine with the end packet still on the queue and all bytes accounted for.
     */
    while ((packet = q->first) != 0 && !(packet->flags & HTTP_PACKET_END) && bytes > 0) {
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            len = (ssize) min(len, bytes);
            mprAdjustBufStart(packet->prefix, len);
            bytes -= len;
            /* Prefixes don't count in the q->count. No need to adjust */
            if (mprGetBufLength(packet->prefix) == 0) {
                packet->prefix = 0;
            }
        }
        if (packet->esize) {
            len = (ssize) min(packet->esize, bytes);
            packet->esize -= len;
            packet->epos += len;
            bytes -= len;
            assure(packet->esize >= 0);
#if UNUSED
            if (packet->esize) {
                /* Still more file to write, but this was all the socket could absorb */
                assure(bytes == 0);
                break;
            }
#endif

        } else if ((len = httpGetPacketLength(packet)) > 0) {
            /* Header packets come here */
            len = (ssize) min(len, bytes);
            mprAdjustBufStart(packet->content, len);
            bytes -= len;
            q->count -= len;
            assure(q->count >= 0);
        }
        if (packet->esize == 0 && httpGetPacketLength(packet) == 0) {
            /* Done with this packet - consume it */
            assure(!(packet->flags & HTTP_PACKET_END));
            httpGetPacket(q);
        } else {
            break;
        }
    }
    assure(bytes == 0);
}


/*  
    Clear entries from the IO vector that have actually been transmitted. This supports partial writes due to the socket
    being full. Don't come here if we've seen all the packets and all the data has been completely written. ie. small files
    don't come here.
 */
static void adjustSendVec(HttpQueue *q, MprOff written)
{
    MprIOVec    *iovec;
    ssize       len;
    int         i, j;

    iovec = q->iovec;
    for (i = 0; i < q->ioIndex; i++) {
        len = iovec[i].len;
        if (written < len) {
            iovec[i].start += (ssize) written;
            iovec[i].len -= (ssize) written;
            return;
        }
        written -= len;
        q->ioCount -= len;
        for (j = i + 1; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex--;
        i--;
    }
    if (written > 0 && q->ioFile) {
        /* All remaining data came from the file */
        q->ioPos += written;
    }
    q->ioIndex = 0;
    q->ioCount = 0;
    q->ioFile = 0;
}


#else
PUBLIC int httpOpenSendConnector(Http *http) { return 0; }
PUBLIC void httpSendOpen(HttpQueue *q) {}
PUBLIC void httpSendOutgoingService(HttpQueue *q) {}
#endif /* !BIT_ROM */

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
