/*
    queue.c -- Queue support routines. Queues are the bi-directional data flow channels for the pipeline.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void manageQueue(HttpQueue *q, int flags);

/************************************ Code ************************************/

PUBLIC HttpQueue *httpCreateQueueHead(HttpConn *conn, cchar *name)
{
    HttpQueue   *q;

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    httpInitQueue(conn, q, name);
    httpInitSchedulerQueue(q);
    return q;
}


/*
    Create a queue associated with a connection.
    Prev may be set to the previous queue in a pipeline. If so, then the Conn.readq and writeq are updated.
 */
PUBLIC HttpQueue *httpCreateQueue(HttpConn *conn, HttpStage *stage, int dir, HttpQueue *prev)
{
    HttpQueue   *q;

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    q->conn = conn;
    httpInitQueue(conn, q, sfmt("%s-%s", stage->name, dir == HTTP_QUEUE_TX ? "tx" : "rx"));
    httpInitSchedulerQueue(q);
    httpAssignQueue(q, stage, dir);
    if (prev) {
        httpAppendQueue(prev, q);
        if (dir == HTTP_QUEUE_RX) {
            conn->readq = conn->tx->queue[HTTP_QUEUE_RX]->prevQ;
        } else {
            conn->writeq = conn->tx->queue[HTTP_QUEUE_TX]->nextQ;
        }
    }
    return q;
}


static void manageQueue(HttpQueue *q, int flags)
{
    HttpPacket      *packet;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(q->name);
        for (packet = q->first; packet; packet = packet->next) {
            mprMark(packet);
        }
        mprMark(q->last);
        mprMark(q->nextQ);
        mprMark(q->prevQ);
        mprMark(q->stage);
        mprMark(q->conn);
        mprMark(q->scheduleNext);
        mprMark(q->schedulePrev);
        mprMark(q->pair);
        mprMark(q->queueData);
        if (q->nextQ && q->nextQ->stage) {
            /* Not a queue head */
            mprMark(q->nextQ);
        }
    }
}


PUBLIC void httpAssignQueue(HttpQueue *q, HttpStage *stage, int dir)
{
    q->stage = stage;
    q->close = stage->close;
    q->open = stage->open;
    q->start = stage->start;
    if (dir == HTTP_QUEUE_TX) {
        q->put = stage->outgoing;
        q->service = stage->outgoingService;
    } else {
        q->put = stage->incoming;
        q->service = stage->incomingService;
    }
}


PUBLIC void httpInitQueue(HttpConn *conn, HttpQueue *q, cchar *name)
{
    HttpTx      *tx;

    tx = conn->tx;
    q->conn = conn;
    q->nextQ = q;
    q->prevQ = q;
    q->name = sclone(name);
    q->max = conn->limits->bufferSize;
    q->low = q->max / 100 *  5;
    if (tx && tx->chunkSize > 0) {
        q->packetSize = tx->chunkSize;
    } else {
        q->packetSize = q->max;
    }
}


PUBLIC void httpSetQueueLimits(HttpQueue *q, ssize low, ssize max)
{
    q->low = low;
    q->max = max;
}


#if KEEP
/*
    Insert a queue after the previous element
 */
PUBLIC void httpAppendQueueToHead(HttpQueue *head, HttpQueue *q)
{
    q->nextQ = head;
    q->prevQ = head->prevQ;
    head->prevQ->nextQ = q;
    head->prevQ = q;
}
#endif


PUBLIC bool httpIsQueueSuspended(HttpQueue *q)
{
    return (q->flags & HTTP_QUEUE_SUSPENDED) ? 1 : 0;
}


PUBLIC void httpSuspendQueue(HttpQueue *q)
{
    mprTrace(7, "Suspend q %s", q->name);
    q->flags |= HTTP_QUEUE_SUSPENDED;
}


PUBLIC bool httpIsSuspendQueue(HttpQueue *q)
{
    return q->flags & HTTP_QUEUE_SUSPENDED;
}



/*
    Remove all data in the queue. If removePackets is true, actually remove the packet too.
    This preserves the header and EOT packets.
 */
PUBLIC void httpDiscardQueueData(HttpQueue *q, bool removePackets)
{
    HttpPacket  *packet, *prev, *next;
    ssize       len;

    if (q == 0) {
        return;
    }
    for (prev = 0, packet = q->first; packet; packet = next) {
        next = packet->next;
        if (packet->flags & (HTTP_PACKET_RANGE | HTTP_PACKET_DATA)) {
            if (removePackets) {
                if (prev) {
                    prev->next = next;
                } else {
                    q->first = next;
                }
                if (packet == q->last) {
                    q->last = prev;
                }
                q->count -= httpGetPacketLength(packet);
                assert(q->count >= 0);
                continue;
            } else {
                len = httpGetPacketLength(packet);
                q->conn->tx->length -= len;
                q->count -= len;
                assert(q->count >= 0);
                if (packet->content) {
                    mprFlushBuf(packet->content);
                }
            }
        }
        prev = packet;
    }
}


/*
    Flush queue data by scheduling the queue and servicing all scheduled queues. Return true if there is room for more data.
    If blocking is requested, the call will block until the queue count falls below the queue max.
    WARNING: Be very careful when using blocking == true. Should only be used by end applications and not by middleware.
 */
PUBLIC bool httpFlushQueue(HttpQueue *q, int flags)
{
    HttpConn    *conn;
    HttpTx      *tx;

    conn = q->conn;
    tx = conn->tx;

    /*
        Initiate flushing
     */
    httpScheduleQueue(q);
    httpServiceQueues(conn, flags);

    if (flags & HTTP_BLOCK) {
        /*
            Blocking mode: Fully drain the pipeline. This blocks until the connector has written all the data to the O/S socket.
         */
        while (tx->writeBlocked || conn->connectorq->count > 0 || conn->connectorq->ioCount) {
            if (conn->connError) {
                break;
            }
            assert(!tx->finalizedConnector);
            assert(conn->connectorq->count > 0 || conn->connectorq->ioCount);
            if (!mprWaitForSingleIO((int) conn->sock->fd, MPR_WRITABLE, conn->limits->inactivityTimeout)) {
                break;
            }
            conn->lastActivity = conn->http->now;
            httpResumeQueue(conn->connectorq);
            httpServiceQueues(conn, flags);
        }
    }
    return (q->count < q->max) ? 1 : 0;
}


PUBLIC void httpResumeQueue(HttpQueue *q)
{
    if (q) {
        q->flags &= ~HTTP_QUEUE_SUSPENDED;
        httpScheduleQueue(q);
    }
}


PUBLIC HttpQueue *httpFindPreviousQueue(HttpQueue *q)
{
    while (q->prevQ && q->prevQ->stage && q->prevQ != q) {
        q = q->prevQ;
        if (q->service) {
            return q;
        }
    }
    return 0;
}


PUBLIC HttpQueue *httpGetNextQueueForService(HttpQueue *q)
{
    HttpQueue     *next;

    if (q->scheduleNext != q) {
        next = q->scheduleNext;
        next->schedulePrev->scheduleNext = next->scheduleNext;
        next->scheduleNext->schedulePrev = next->schedulePrev;
        next->schedulePrev = next->scheduleNext = next;
        return next;
    }
    return 0;
}


/*
    Return the number of bytes the queue will accept. Always positive.
 */
PUBLIC ssize httpGetQueueRoom(HttpQueue *q)
{
    assert(q->max > 0);
    assert(q->count >= 0);

    if (q->count >= q->max) {
        return 0;
    }
    return q->max - q->count;
}


PUBLIC void httpInitSchedulerQueue(HttpQueue *q)
{
    q->scheduleNext = q;
    q->schedulePrev = q;
}


/*
    Append a queue after the previous element
 */
PUBLIC void httpAppendQueue(HttpQueue *prev, HttpQueue *q)
{
    q->nextQ = prev->nextQ;
    q->prevQ = prev;
    prev->nextQ->prevQ = q;
    prev->nextQ = q;
}


PUBLIC bool httpIsQueueEmpty(HttpQueue *q)
{
    return q->first == 0;
}


PUBLIC void httpRemoveQueue(HttpQueue *q)
{
    q->prevQ->nextQ = q->nextQ;
    q->nextQ->prevQ = q->prevQ;
    q->prevQ = q->nextQ = q;
}


PUBLIC void httpScheduleQueue(HttpQueue *q)
{
    HttpQueue     *head;

    assert(q->conn);
    head = q->conn->serviceq;

    if (q->scheduleNext == q && !(q->flags & HTTP_QUEUE_SUSPENDED)) {
        q->scheduleNext = head;
        q->schedulePrev = head->schedulePrev;
        head->schedulePrev->scheduleNext = q;
        head->schedulePrev = q;
    }
}


PUBLIC void httpServiceQueue(HttpQueue *q)
{
    q->conn->currentq = q;

    if (q->servicing) {
        q->flags |= HTTP_QUEUE_RESERVICE;
    } else {
        /*
            Since we are servicing this "q" now, we can remove from the schedule queue if it is already queued.
         */
        if (q->conn->serviceq->scheduleNext == q) {
            httpGetNextQueueForService(q->conn->serviceq);
        }
        if (!(q->flags & HTTP_QUEUE_SUSPENDED)) {
            q->servicing = 1;
            mprTrace(7, "Service queue %s", q->name);
            q->service(q);
            if (q->flags & HTTP_QUEUE_RESERVICE) {
                q->flags &= ~HTTP_QUEUE_RESERVICE;
                httpScheduleQueue(q);
            }
            q->flags |= HTTP_QUEUE_SERVICED;
            q->servicing = 0;
        }
    }
}


/*
    Return true if the next queue will accept this packet. If not, then disable the queue's service procedure.
    This may split the packet if it exceeds the downstreams maximum packet size.
 */
PUBLIC bool httpWillNextQueueAcceptPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpQueue   *nextQ;
    ssize       size;

    nextQ = q->nextQ;
    size = httpGetPacketLength(packet);
    if (size <= nextQ->packetSize && (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    httpResizePacket(q, packet, 0);
    size = httpGetPacketLength(packet);
    assert(size <= nextQ->packetSize);
    /* 
        Packet size is now acceptable. Accept the packet if the queue is mostly empty (< low) or if the 
        packet will fit entirely under the max or if the queue.
        NOTE: queue maximums are advisory. We choose to potentially overflow the max here to optimize the case where
        the queue may have say one byte and a max size packet would overflow by 1.
     */
    if (nextQ->count < nextQ->low || (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    /*
        The downstream queue cannot accept this packet, so disable queue and mark the downstream queue as full and service 
     */
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


#if KEEP
PUBLIC bool httpWillQueueAcceptPacket(HttpQueue *q, HttpPacket *packet, bool split)
{
    ssize       size;

    size = httpGetPacketLength(packet);
    if (size <= q->packetSize && (size + q->count) <= q->max) {
        return 1;
    }
    if (split) {
        httpResizePacket(q, packet, 0);
        size = httpGetPacketLength(packet);
        assert(size <= q->packetSize);
        if ((size + q->count) <= q->max) {
            return 1;
        }
    }
    /*
        The downstream queue is full, so disable the queue and mark the downstream queue as full and service 
     */
    if (!(q->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(q);
    }
    return 0;
}
#endif


/*
    Return true if the next queue will accept a certain amount of data. If not, then disable the queue's service procedure.
    Will not split the packet.
 */
PUBLIC bool httpWillNextQueueAcceptSize(HttpQueue *q, ssize size)
{
    HttpQueue   *nextQ;

    nextQ = q->nextQ;
    if (size <= nextQ->packetSize && (size + nextQ->count) <= nextQ->max) {
        return 1;
    }
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


#if BIT_DEBUG
PUBLIC bool httpVerifyQueue(HttpQueue *q)
{
    HttpPacket  *packet;
    ssize       count;

    count = 0;
    for (packet = q->first; packet; packet = packet->next) {
        if (packet->next == 0) {
            assert(packet == q->last);
        }
        count += httpGetPacketLength(packet);
    }
    assert(count == q->count);
    return count <= q->count;
}
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2014. All Rights Reserved.

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
