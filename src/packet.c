/*
    packet.c -- Queue support routines. Queues are the bi-directional data flow channels for the pipeline.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void managePacket(HttpPacket *packet, int flags);

/************************************ Code ************************************/
/*  
    Create a new packet. If size is -1, then also create a default growable buffer -- 
    used for incoming body content. If size > 0, then create a non-growable buffer 
    of the requested size.
 */
PUBLIC HttpPacket *httpCreatePacket(ssize size)
{
    HttpPacket  *packet;

    if ((packet = mprAllocObj(HttpPacket, managePacket)) == 0) {
        return 0;
    }
    if (size != 0) {
        if ((packet->content = mprCreateBuf(size < 0 ? HTTP_BUFSIZE: (ssize) size, -1)) == 0) {
            return 0;
        }
    }
    return packet;
}


static void managePacket(HttpPacket *packet, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(packet->prefix);
        mprMark(packet->content);
        /* Don't mark next packet. List owner will mark */
    }
}


PUBLIC HttpPacket *httpCreateDataPacket(ssize size)
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(size)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_DATA;
    return packet;
}


PUBLIC HttpPacket *httpCreateEntityPacket(MprOff pos, MprOff size, HttpFillProc fill)
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_DATA;
    packet->epos = pos;
    packet->esize = size;
    packet->fill = fill;
    return packet;
}


PUBLIC HttpPacket *httpCreateEndPacket()
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_END;
    return packet;
}


PUBLIC HttpPacket *httpCreateHeaderPacket()
{
    HttpPacket    *packet;

    if ((packet = httpCreatePacket(HTTP_BUFSIZE)) == 0) {
        return 0;
    }
    packet->flags = HTTP_PACKET_HEADER;
    return packet;
}


PUBLIC HttpPacket *httpClonePacket(HttpPacket *orig)
{
    HttpPacket  *packet;

    if ((packet = httpCreatePacket(0)) == 0) {
        return 0;
    }
    if (orig->content) {
        packet->content = mprCloneBuf(orig->content);
    }
    if (orig->prefix) {
        packet->prefix = mprCloneBuf(orig->prefix);
    }
    packet->flags = orig->flags;
    packet->type = orig->type;
    packet->last = orig->last;
    packet->esize = orig->esize;
    packet->epos = orig->epos;
    packet->fill = orig->fill;
    return packet;
}


PUBLIC void httpAdjustPacketStart(HttpPacket *packet, MprOff size)
{
    if (packet->esize) {
        packet->epos += size;
        packet->esize -= size;
    } else if (packet->content) {
        mprAdjustBufStart(packet->content, (ssize) size);
    }
}


PUBLIC void httpAdjustPacketEnd(HttpPacket *packet, MprOff size)
{
    if (packet->esize) {
        packet->esize += size;
    } else if (packet->content) {
        mprAdjustBufEnd(packet->content, (ssize) size);
    }
}


PUBLIC HttpPacket *httpGetPacket(HttpQueue *q)
{
    HttpQueue     *prev;
    HttpPacket    *packet;

    while (q->first) {
        if ((packet = q->first) != 0) {
            q->first = packet->next;
            packet->next = 0;
            q->count -= httpGetPacketLength(packet);
            assure(q->count >= 0);
            if (packet == q->last) {
                q->last = 0;
                assure(q->first == 0);
            }
            if (q->first == 0) {
                assure(q->last == 0);
            }
        }
        if (q->count < q->low) {
            prev = httpFindPreviousQueue(q);
            if (prev && prev->flags & HTTP_QUEUE_SUSPENDED) {
                /*
                    This queue was full and now is below the low water mark. Back-enable the previous queue.
                 */
                httpResumeQueue(prev);
            }
        }
        return packet;
    }
    return 0;
}


/*  
    Test if the packet is too too large to be accepted by the downstream queue.
 */
PUBLIC bool httpIsPacketTooBig(HttpQueue *q, HttpPacket *packet)
{
    ssize   size;
    
    size = mprGetBufLength(packet->content);
    return size > q->max || size > q->packetSize;
}


/*  
    Join a packet onto the service queue. This joins packet content data.
 */
PUBLIC void httpJoinPacketForService(HttpQueue *q, HttpPacket *packet, bool serviceQ)
{
    if (q->first == 0) {
        /*  Just use the service queue as a holding queue while we aggregate the post data.  */
        httpPutForService(q, packet, HTTP_DELAY_SERVICE);

    } else {
        /* Skip over the header packet */
        if (q->first && q->first->flags & HTTP_PACKET_HEADER) {
            packet = q->first->next;
            q->first = packet;
        } else {
            /* Aggregate all data into one packet and free the packet.  */
            httpJoinPacket(q->first, packet);
        }
        q->count += httpGetPacketLength(packet);
    }
    VERIFY_QUEUE(q);
    if (serviceQ && !(q->flags & HTTP_QUEUE_SUSPENDED))  {
        httpScheduleQueue(q);
    }
}


/*  
    Join two packets by pulling the content from the second into the first.
    WARNING: this will not update the queue count. Assumes the either both are on the queue or neither. 
 */
PUBLIC int httpJoinPacket(HttpPacket *packet, HttpPacket *p)
{
    ssize   len;

    assure(packet->esize == 0);
    assure(p->esize == 0);
    assure(!(packet->flags & HTTP_PACKET_SOLO));
    assure(!(p->flags & HTTP_PACKET_SOLO));

    len = httpGetPacketLength(p);
    if (mprPutBlockToBuf(packet->content, mprGetBufStart(p->content), (ssize) len) != len) {
        assure(0);
        return MPR_ERR_MEMORY;
    }
    return 0;
}


/*
    Join queue packets up to the maximum of the given size and the downstream queue packet size.
    WARNING: this will not update the queue count.
 */
PUBLIC void httpJoinPackets(HttpQueue *q, ssize size)
{
    HttpPacket  *packet, *first;
    ssize       len;

    if (size < 0) {
        size = MAXINT;
    }
    if ((first = q->first) != 0 && first->next) {
        if (first->flags & HTTP_PACKET_HEADER) {
            /* Step over a header packet */
            first = first->next;
        }
        for (packet = first->next; packet; packet = packet->next) {
            if (packet->content == 0 || (len = httpGetPacketLength(packet)) == 0) {
                break;
            }
            assure(!(packet->flags & HTTP_PACKET_END));
            httpJoinPacket(first, packet);
            /* Unlink the packet */
            first->next = packet->next;
            if (q->last == packet) {
                q->last = first;
            }
        }
    }
}


PUBLIC void httpPutPacket(HttpQueue *q, HttpPacket *packet)
{
    assure(packet);
    assure(q->put);

    q->put(q, packet);
}


/*  
    Pass to the next stage in the pipeline
 */
PUBLIC void httpPutPacketToNext(HttpQueue *q, HttpPacket *packet)
{
    assure(packet);
    assure(q->nextQ->put);

    q->nextQ->put(q->nextQ, packet);
}


PUBLIC void httpPutPackets(HttpQueue *q)
{
    HttpPacket    *packet;

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        httpPutPacketToNext(q, packet);
    }
}


/*  
    Put the packet back at the front of the queue
 */
PUBLIC void httpPutBackPacket(HttpQueue *q, HttpPacket *packet)
{
    assure(packet);
    assure(packet->next == 0);
    assure(q->count >= 0);
    
    if (packet) {
        packet->next = q->first;
        if (q->first == 0) {
            q->last = packet;
        }
        q->first = packet;
        q->count += httpGetPacketLength(packet);
    }
}


/*  
    Put a packet on the service queue.
 */
PUBLIC void httpPutForService(HttpQueue *q, HttpPacket *packet, bool serviceQ)
{
    assure(packet);
   
    q->count += httpGetPacketLength(packet);
    packet->next = 0;
    
    if (q->first) {
        q->last->next = packet;
        q->last = packet;
    } else {
        q->first = packet;
        q->last = packet;
    }
    if (serviceQ && !(q->flags & HTTP_QUEUE_SUSPENDED))  {
        httpScheduleQueue(q);
    }
}


/*  
    Resize and possibly split a packet so it fits in the downstream queue. Put back the 2nd portion of the split packet 
    on the queue. Ensure that the packet is not larger than "size" if it is greater than zero. If size < 0, then
    use the default packet size. 
 */
PUBLIC int httpResizePacket(HttpQueue *q, HttpPacket *packet, ssize size)
{
    HttpPacket  *tail;
    ssize       len;
    
    if (size <= 0) {
        size = MAXINT;
    }
    if (packet->esize > size) {
        if ((tail = httpSplitPacket(packet, size)) == 0) {
            return MPR_ERR_MEMORY;
        }
    } else {
        /*  
            Calculate the size that will fit downstream
         */
        len = packet->content ? httpGetPacketLength(packet) : 0;
        size = min(size, len);
        size = min(size, q->nextQ->max);
        size = min(size, q->nextQ->packetSize);
        if (size == 0 || size == len) {
            return 0;
        }
        if ((tail = httpSplitPacket(packet, size)) == 0) {
            return MPR_ERR_MEMORY;
        }
    }
    httpPutBackPacket(q, tail);
    return 0;
}


/*
    Split a packet at a given offset and return a new packet containing the data after the offset.
    The prefix data remains with the original packet. 
 */
PUBLIC HttpPacket *httpSplitPacket(HttpPacket *orig, ssize offset)
{
    HttpPacket  *packet;
    ssize       count, size;

    /* Must not be in a queue */
    assure(orig->next == 0);

    if (orig->esize) {
        if ((packet = httpCreateEntityPacket(orig->epos + offset, orig->esize - offset, orig->fill)) == 0) {
            return 0;
        }
        orig->esize = offset;

    } else {
        if (offset >= httpGetPacketLength(orig)) {
            assure(offset < httpGetPacketLength(orig));
            return 0;
        }
        /*
            OPT - A large packet will often be resized by splitting into chunks that the
            downstream queues will accept. This causes many allocations that are a small delta less than the large
            packet 
            Better:
                - If offset is < size/2
                    - Allocate packet size == offset
                    - Set orig->content =   
                    - packet->content = orig->content
                        httpAdjustPacketStart(packet, offset) 
                    - orig->content = Packet(size == offset)
                        copy from packet
                Adjust the content->start
         */
        count = httpGetPacketLength(orig) - offset;
        size = max(count, HTTP_BUFSIZE);
        size = HTTP_PACKET_ALIGN(size);
        if ((packet = httpCreateDataPacket(size)) == 0) {
            return 0;
        }
        httpAdjustPacketEnd(orig, (ssize) -count);
        if (mprPutBlockToBuf(packet->content, mprGetBufEnd(orig->content), (ssize) count) != count) {
            return 0;
        }
#if BIT_DEBUG
        mprAddNullToBuf(orig->content);
#endif
    }
    packet->flags = orig->flags;
    packet->type = orig->type;
    packet->last = orig->last;
    return packet;
}


bool httpIsLastPacket(HttpPacket *packet) 
{
    return packet->last;
}


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
