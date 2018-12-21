/*
    queue.c -- Queue support routines. Queues are the bi-directional data flow channels for the pipeline.
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void initQueue(HttpNet *net, HttpStream *stream, HttpQueue *q, cchar *name, int dir);
static void manageQueue(HttpQueue *q, int flags);
static void serviceQueue(HttpQueue *q);

/************************************ Code ************************************/
/*
    Create a queue head that has no processing callbacks
 */
PUBLIC HttpQueue *httpCreateQueueHead(HttpNet *net, HttpStream *stream, cchar *name, int dir)
{
    HttpQueue   *q;

    assert(net);

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    initQueue(net, stream, q, name, dir);
    httpInitSchedulerQueue(q);
    return q;
}


/*
    Create a queue associated with a connection.
    Prev may be set to the previous queue in a pipeline. If so, then the Conn.readq and writeq are updated.
 */
PUBLIC HttpQueue *httpCreateQueue(HttpNet *net, HttpStream *stream, HttpStage *stage, int dir, HttpQueue *prev)
{
    HttpQueue   *q;

    if ((q = mprAllocObj(HttpQueue, manageQueue)) == 0) {
        return 0;
    }
    initQueue(net, stream, q, stage->name, dir);
    httpInitSchedulerQueue(q);
    httpAssignQueueCallbacks(q, stage, dir);
    if (prev) {
        httpAppendQueue(q, prev);
    }
    return q;
}


static void initQueue(HttpNet *net, HttpStream *stream, HttpQueue *q, cchar *name, int dir)
{
    q->net = net;
    q->stream = stream;
    q->flags = dir == HTTP_QUEUE_TX ? HTTP_QUEUE_OUTGOING : 0;
    q->nextQ = q;
    q->prevQ = q;
    q->name = sfmt("%s-%s", name, dir == HTTP_QUEUE_TX ? "tx" : "rx");
    if (stream && stream->tx && stream->tx->chunkSize > 0) {
        q->packetSize = stream->tx->chunkSize;
    } else {
        q->packetSize = net->limits->packetSize;
    }
    q->max = q->packetSize * ME_QUEUE_MAX_FACTOR;
    q->low = q->packetSize;
}


static void manageQueue(HttpQueue *q, int flags)
{
    HttpPacket      *packet;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(q->name);
        mprMark(q->nextQ);
        for (packet = q->first; packet; packet = packet->next) {
            mprMark(packet);
        }
        mprMark(q->stream);
        mprMark(q->last);
        mprMark(q->prevQ);
        mprMark(q->stage);
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


/*
    Assign stage callbacks to a queue
 */
PUBLIC void httpAssignQueueCallbacks(HttpQueue *q, HttpStage *stage, int dir)
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


PUBLIC void httpSetQueueLimits(HttpQueue *q, HttpLimits *limits, ssize packetSize, ssize low, ssize max, ssize window)
{
    if (packetSize < 0) {
        packetSize = limits->packetSize;
    }
    if (max < 0) {
        max = q->packetSize * ME_QUEUE_MAX_FACTOR;
    }
    if (low < 0) {
        low = q->packetSize;
    }
    q->packetSize = packetSize;
    q->max = max;
    q->low = low;
    
#if ME_HTTP_HTTP2
    if (window < 0) {
        window = limits->window;
    }
    q->window = window;
#endif
}


PUBLIC void httpPairQueues(HttpQueue *q1, HttpQueue *q2)
{
    q1->pair = q2;
    q2->pair = q1;
}


PUBLIC bool httpIsQueueSuspended(HttpQueue *q)
{
    return (q->flags & HTTP_QUEUE_SUSPENDED) ? 1 : 0;
}


PUBLIC void httpSuspendQueue(HttpQueue *q)
{
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
                //  TODO - should do this in caller or have higher level routine that does this with "stream" as arg
                //  TODO - or should we just set tx->length to zero?
                if (q->stream && q->stream->tx && q->stream->tx->length > 0) {
                    q->stream->tx->length -= len;
                }
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
    Flush queue data toward the connector by scheduling the queue and servicing all scheduled queues.
    Return true if there is room for more data. If blocking is requested, the call will block until
    the queue count falls below the queue max. NOTE: may return early if the inactivityTimeout expires.
    WARNING: may yield.
 */
PUBLIC bool httpFlushQueue(HttpQueue *q, int flags)
{
    HttpNet     *net;
    HttpStream  *stream;
    MprTicks    timeout;
    int         events;

    net = q->net;
    stream = q->stream;

    /*
        Initiate flushing. For HTTP/2 we must process incoming window update frames, so run any pending IO events.
     */
    httpScheduleQueue(q);
    httpServiceNetQueues(net, flags);
    mprWaitForEvent(stream->dispatcher, 0, mprGetEventMark(stream->dispatcher));

    if (net->error) {
        return 1;
    }

    while (q->count > 0 && !stream->error && !net->error) {
        timeout = (flags & HTTP_BLOCK) ? stream->limits->inactivityTimeout : 0;
        if ((events = mprWaitForSingleIO((int) net->sock->fd, MPR_READABLE | MPR_WRITABLE, timeout)) != 0) {
            stream->lastActivity = net->lastActivity = net->http->now;
            if (events & MPR_WRITABLE) {
                net->lastActivity = net->http->now;
                httpResumeQueue(net->socketq);
                httpScheduleQueue(net->socketq);
                httpServiceNetQueues(net, flags);
            }
            /*
                Process HTTP/2 window update messages for flow control
             */
            mprWaitForEvent(stream->dispatcher, 0, mprGetEventMark(stream->dispatcher));
        }
        if (!(flags & HTTP_BLOCK)) {
            break;
        }
    }
    return (q->count < q->max) ? 1 : 0;
}


PUBLIC void httpFlush(HttpStream *stream)
{
    httpFlushQueue(stream->writeq, HTTP_NON_BLOCK);
}


/*
    Flush the write queue. In sync mode, this call may yield.
 */
PUBLIC void httpFlushAll(HttpStream *stream)
{
    httpFlushQueue(stream->writeq, stream->net->async ? HTTP_NON_BLOCK : HTTP_BLOCK);
}


PUBLIC void httpResumeQueue(HttpQueue *q)
{
    if (q && (q->flags & HTTP_QUEUE_SUSPENDED)) {
        q->flags &= ~HTTP_QUEUE_SUSPENDED;
        httpScheduleQueue(q);
    }
    if (q->count == 0 && q->prevQ->flags & HTTP_QUEUE_SUSPENDED) {
        httpResumeQueue(q->prevQ);
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
PUBLIC HttpQueue *httpAppendQueue(HttpQueue *q, HttpQueue *prev)
{
    q->nextQ = prev->nextQ;
    q->prevQ = prev;
    prev->nextQ->prevQ = q;
    prev->nextQ = q;
    return q;
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

    head = q->net->serviceq;

    if (q->scheduleNext == q && !(q->flags & HTTP_QUEUE_SUSPENDED)) {
        q->scheduleNext = head;
        q->schedulePrev = head->schedulePrev;
        head->schedulePrev->scheduleNext = q;
        head->schedulePrev = q;
    }
}


static void serviceQueue(HttpQueue *q)
{
    /*
        Hold the queue for GC while scheduling.
        TODO - this is probably not required as the queue is always linked into a pipeline
     */
    q->net->holdq = q;

    if (q->servicing) {
        q->flags |= HTTP_QUEUE_RESERVICE;
    } else {
        /*
            Since we are servicing this "q" now, we can remove from the schedule queue if it is already queued.
         */
        if (q->net->serviceq->scheduleNext == q) {
            httpGetNextQueueForService(q->net->serviceq);
        }
        if (!(q->flags & HTTP_QUEUE_SUSPENDED)) {
            q->servicing = 1;
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


PUBLIC bool httpServiceQueues(HttpStream *stream, int flags)
{
    return httpServiceNetQueues(stream->net, flags);
}


/*
    Run the queue service routines until there is no more work to be done.
    If flags & HTTP_BLOCK, this routine may block while yielding.  Return true if actual work was done.
 */
PUBLIC bool httpServiceNetQueues(HttpNet *net, int flags)
{
    HttpQueue   *q;
    bool        workDone;

    workDone = 0;

    /*
        If switching to net->queues -- may need some limit on number of iterations
     */
    while ((q = httpGetNextQueueForService(net->serviceq)) != NULL) {
        if (q->servicing) {
            /* Called re-entrantly */
            q->flags |= HTTP_QUEUE_RESERVICE;
        } else {
            assert(q->schedulePrev == q->scheduleNext);
            serviceQueue(q);
            workDone = 1;
        }
        if (mprNeedYield() && (flags & HTTP_BLOCK)) {
            mprYield(0);
        }
    }
    /*
        Always do a yield if requested even if there are no queues to service
     */
    if (mprNeedYield() && (flags & HTTP_BLOCK)) {
        mprYield(0);
    }
    return workDone;
}


/*
    Return true if the next queue will accept this packet. If not, then disable the queue's service procedure.
    This may split the packet if it exceeds the downstreams maximum packet size.
 */
PUBLIC bool httpWillQueueAcceptPacket(HttpQueue *q, HttpQueue *nextQ, HttpPacket *packet)
{
    ssize       room, size;

    size = httpGetPacketLength(packet);
    room = min(nextQ->packetSize, nextQ->max - nextQ->count);
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
        The downstream queue cannot accept this packet, so disable queue and mark the downstream queue as full and service
     */
    httpSuspendQueue(q);
    if (!(nextQ->flags & HTTP_QUEUE_SUSPENDED)) {
        httpScheduleQueue(nextQ);
    }
    return 0;
}


PUBLIC bool httpWillNextQueueAcceptPacket(HttpQueue *q, HttpPacket *packet)
{
    return httpWillQueueAcceptPacket(q, q->nextQ, packet);
}


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


#if ME_DEBUG
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
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
