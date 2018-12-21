/*
    netConnector.c -- General network connector.

    The Network connector handles I/O from upstream handlers and filters. It uses vectored writes to
    aggregate output packets into fewer actual I/O requests to the O/S.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/**************************** Forward Declarations ****************************/

static void addPacketForNet(HttpQueue *q, HttpPacket *packet);
static void addToNetVector(HttpQueue *q, char *ptr, ssize bytes);
static void adjustNetVec(HttpQueue *q, ssize written);
static MprOff buildNetVec(HttpQueue *q);
static void freeNetPackets(HttpQueue *q, ssize written);
static HttpPacket *getPacket(HttpNet *net, ssize *size);
static void netOutgoing(HttpQueue *q, HttpPacket *packet);
static void netOutgoingService(HttpQueue *q);
static HttpPacket *readPacket(HttpNet *net);
static void resumeEvents(HttpNet *net, MprEvent *event);
static int sleuthProtocol(HttpNet *net, HttpPacket *packet);

/*********************************** Code *************************************/
/*
    Initialize the net connector
 */
PUBLIC int httpOpenNetConnector()
{
    HttpStage     *stage;

    if ((stage = httpCreateStreamector("netConnector", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    stage->outgoing = netOutgoing;
    stage->outgoingService = netOutgoingService;
    HTTP->netConnector = stage;
    return 0;
}


/*
    Accept a new client connection on a new socket. This is invoked from acceptNet in endpoint.c
    and will come arrive on a worker thread with a new dispatcher dedicated to this connection.
 */
PUBLIC HttpNet *httpAccept(HttpEndpoint *endpoint, MprEvent *event)
{
    HttpNet     *net;
    HttpAddress *address;
    HttpLimits  *limits;
    MprSocket   *sock;
    int64       value;

    assert(event);
    assert(event->dispatcher);
    assert(endpoint);

    if (mprShouldDenyNewRequests()) {
        return 0;
    }
    sock = event->sock;

    if ((net = httpCreateNet(event->dispatcher, endpoint, -1, HTTP_NET_ASYNC)) == 0) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    httpBindSocket(net, sock);
    limits = net->limits;

    if ((address = httpMonitorAddress(net, 0)) == 0) {
        mprCloseSocket(sock, 0);
        return 0;
    }
    if ((value = httpMonitorNetEvent(net, HTTP_COUNTER_ACTIVE_CONNECTIONS, 1)) > limits->connectionsMax) {
        mprLog("net info", 3, "Too many concurrent connections, active: %d, max:%d", (int) value - 1, limits->connectionsMax);
        httpDestroyNet(net);
        return 0;
    }
    address = net->address;
    if (address && address->banUntil) {
        if (address->banUntil < net->http->now) {
            mprLog("net info", 3, "Stop ban for client %s", net->ip);
            address->banUntil = 0;
        } else {
            mprLog("net info", 3, "Network connection refused, client banned: %s", address->banMsg ? address->banMsg : "");
            //  TODO - address->banStatus not implemented
            httpDestroyNet(net);
            return 0;
        }
    }
#if ME_COM_SSL
    if (endpoint->ssl) {
        if (mprUpgradeSocket(sock, endpoint->ssl, 0) < 0) {
            httpMonitorNetEvent(net, HTTP_COUNTER_SSL_ERRORS, 1);
            mprLog("net error", 0, "Cannot upgrade socket, %s", sock->errorMsg);
            httpDestroyNet(net);
            return 0;
        }
    }
#endif
    event->mask = MPR_READABLE;
    event->timestamp = net->http->now;
    (net->ioCallback)(net, event);
    return net;
}


/*
    Handle IO on the network. Initially the dispatcher will be set to the server->dispatcher and the first
    I/O event will be handled on the server thread (or main thread). A request handler may create a new
    net->dispatcher and transfer execution to a worker thread if required.
 */
PUBLIC void httpIOEvent(HttpNet *net, MprEvent *event)
{
    HttpPacket  *packet;

    if (net->destroyed) {
        /* Network connection has been destroyed */
        return;
    }
    net->lastActivity = net->http->now;
    if (event->mask & MPR_WRITABLE) {
        httpResumeQueue(net->socketq);
        httpScheduleQueue(net->socketq);
    }
    packet = 0;

    if (event->mask & MPR_READABLE) {
        packet = readPacket(net);
    }
    if (packet) {
        if (!net->protocol) {
            int protocol = sleuthProtocol(net, packet);
            httpSetNetProtocol(net, protocol);
        }
        if (net->protocol) {
            httpPutPacket(net->inputq, packet);
        }
    }
    httpServiceNetQueues(net, 0);

    if (httpIsServer(net) && (net->error || net->eof)) {
        httpDestroyNet(net);
    } else if (httpIsClient(net) && net->eof) {
        httpNetClosed(net);
    } else if (net->async && !net->delay) {
        httpEnableNetEvents(net);
    }
}


static int sleuthProtocol(HttpNet *net, HttpPacket *packet)
{
#if ME_HTTP_HTTP2
    MprBuf      *buf;
    ssize       len;
    int         protocol;

    buf = packet->content;
    protocol = 0;

    if ((len = mprGetBufLength(buf)) < (sizeof(HTTP2_PREFACE) - 1)) {
        return 0;
    }
    if (memcmp(buf->start, HTTP2_PREFACE, sizeof(HTTP2_PREFACE) - 1) != 0) {
        protocol = 1;
    } else {
        mprAdjustBufStart(buf, strlen(HTTP2_PREFACE));
        protocol = 2;
        httpTrace(net->trace, "net.rx", "context", "msg:'Detected HTTP/2 preface'");
    }
    return protocol;
#else
    return 1;
#endif
}


/*
    Read data from the peer. This will use an existing packet on the inputq or allocate a new packet if required to
    hold the data. Socket error messages are stored in net->errorMsg.
 */
static HttpPacket *readPacket(HttpNet *net)
{
    HttpPacket  *packet;
    ssize       size, lastRead;

    if ((packet = getPacket(net, &size)) != 0) {
        lastRead = mprReadSocket(net->sock, mprGetBufEnd(packet->content), size);
        net->eof = mprIsSocketEof(net->sock);

#if ME_COM_SSL
        if (net->sock->secured && !net->secure && net->sock->cipher) {
            MprSocket   *sock;
            net->secure = 1;
            sock = net->sock;
            if (sock->peerCert) {
                httpTrace(net->trace, "net.ssl", "context",
                    "msg:'Connection secured', cipher:'%s', peerName:'%s', subject:'%s', issuer:'%s', session:'%s'",
                    sock->cipher, sock->peerName, sock->peerCert, sock->peerCertIssuer, sock->session);
            } else {
                httpTrace(net->trace, "net.ssl", "context", "msg:'Connection secured', cipher:'%s', session:'%s'", sock->cipher, sock->session);
            }
            if (mprGetLogLevel() >= 5) {
                mprLog("info http ssl", 6, "SSL State: %s", mprGetSocketState(sock));
            }
        }
#endif
        if (lastRead > 0) {
            mprAdjustBufEnd(packet->content, lastRead);
            return packet;
        }
        if (lastRead < 0 && net->eof) {
            net->eof = 1;
            return 0;
        }
    }
    return 0;
}


/*
    Get the packet into which to read data. Return in *size the length of data to attempt to read.
 */
static HttpPacket *getPacket(HttpNet *net, ssize *lenp)
{
    HttpPacket  *packet;
    MprBuf      *buf;
    ssize       size;

#if ME_HTTP_HTTP2
    if (net->protocol < 2) {
        size = net->inputq ? net->inputq->packetSize : ME_PACKET_SIZE;
    } else {
        size = (net->inputq ? net->inputq->packetSize : HTTP2_MIN_FRAME_SIZE) + HTTP2_FRAME_OVERHEAD;
    }
#else
    size = net->inputq ? net->inputq->packetSize : ME_PACKET_SIZE;
#endif
    if (!net->inputq || (packet = httpGetPacket(net->inputq)) == NULL) {
        if ((packet = httpCreateDataPacket(size)) == 0) {
            return 0;
        }
    }
    buf = packet->content;
    mprResetBufIfEmpty(buf);
    if (mprGetBufSpace(buf) < size && mprGrowBuf(buf, size) < 0) {
        return 0;
    }
    *lenp = mprGetBufSpace(buf);
    assert(*lenp > 0);
    return packet;
}


PUBLIC int httpGetNetEventMask(HttpNet *net)
{
    MprSocket   *sock;
    int         eventMask;

    if ((sock = net->sock) == 0) {
        return 0;
    }
    eventMask = 0;

    if (httpQueuesNeedService(net) || mprSocketHasBufferedWrite(sock) ||
            (net->socketq && (net->socketq->count > 0 || net->socketq->ioCount > 0))) {
        if (!mprSocketHandshaking(sock)) {
            /* Must wait to write until handshaking is complete */
            eventMask |= MPR_WRITABLE;
        }
    }
    if (mprSocketHasBufferedRead(sock) || !net->inputq || (net->inputq->count < net->inputq->max)) {
        /*
            TODO - how to mitigate against a ping flood?
            Was testing if !writeBlocked before adding MPR_READABLE, but this is always required for HTTP/2 to read window frames.
         */
        if (mprSocketHandshaking(sock) || !net->eof) {
            eventMask |= MPR_READABLE;
        }
    }
    return eventMask;
}


static bool netBanned(HttpNet *net)
{
    HttpAddress     *address;

    if ((address = net->address) != 0 && address->delay) {
        if (address->delayUntil > net->http->now) {
            /*
                Defensive counter measure - go slow
             */
            mprCreateEvent(net->dispatcher, "delayConn", net->delay, resumeEvents, net, 0);
            httpTrace(net->trace, "monitor.delay.stop", "context", "msg:'Suspend I/O',client:'%s'", net->ip);
            return 1;
        } else {
            address->delay = 0;
            httpTrace(net->trace, "monitor.delay.stop", "context", "msg:'Resume I/O',client:'%s'", net->ip);
        }
    }
    return 0;
}


/*
    Defensive countermesasure - resume output after a delay
 */
static void resumeEvents(HttpNet *net, MprEvent *event)
{
    net->delay = 0;
    mprCreateEvent(net->dispatcher, "resumeConn", 0, httpEnableNetEvents, net, 0);
}


PUBLIC void httpEnableNetEvents(HttpNet *net)
{
    if (mprShouldAbortRequests() || net->borrowed || net->error || netBanned(net)) {
        return;
    }
    /*
        Used by ejs
     */
    if (net->workerEvent) {
        MprEvent *event = net->workerEvent;
        net->workerEvent = 0;
        mprQueueEvent(net->dispatcher, event);
        return;
    }
    httpSetupWaitHandler(net, httpGetNetEventMask(net));
}


PUBLIC void httpSetupWaitHandler(HttpNet *net, int eventMask)
{
    MprSocket   *sp;

    if ((sp = net->sock) == 0) {
        return;
    }
    if (eventMask) {
        if (sp->handler == 0) {
            mprAddSocketHandler(sp, eventMask, net->dispatcher, net->ioCallback, net, 0);
        } else {
            mprSetSocketDispatcher(sp, net->dispatcher);
            mprEnableSocketEvents(sp, eventMask);
        }
        if (sp->flags & (MPR_SOCKET_BUFFERED_READ | MPR_SOCKET_BUFFERED_WRITE)) {
            mprRecallWaitHandler(sp->handler);
        }
    } else if (sp->handler) {
        mprWaitOn(sp->handler, eventMask);
    }
    net->eventMask = eventMask;
}


static void checkLen(HttpQueue *q)
{
    HttpPacket  *packet;
    static int  maxCount = 0;
    int         count = 0;

    for (packet = q->first; packet; packet = packet->next) {
        count++;
    }
    if (count > maxCount) {
        maxCount = count;
        if (maxCount > 50) {
            // print("XX Qcount %ld, count %d, blocked %d, goaway %d, received goaway %d, eof %d", q->count, count, net->writeBlocked, net->goaway, net->receivedGoaway, net->eof);
        }
    }
}


static void netOutgoing(HttpQueue *q, HttpPacket *packet)
{
    assert(q == q->net->socketq);

    if (q->net->socketq) {
        httpPutForService(q->net->socketq, packet, HTTP_SCHEDULE_QUEUE);
    }
    checkLen(q);
}


static void netOutgoingService(HttpQueue *q)
{
    HttpNet     *net;
    ssize       written;
    int         errCode;

    net = q->net;
    net->writeBlocked = 0;

    while (q->first || q->ioIndex) {
        if (q->ioIndex == 0 && buildNetVec(q) <= 0) {
            freeNetPackets(q, 0);
            break;
        }
        written = mprWriteSocketVector(net->sock, q->iovec, q->ioIndex);
        if (written < 0) {
            errCode = mprGetError();
            if (errCode == EAGAIN || errCode == EWOULDBLOCK) {
                /*  Socket full, wait for an I/O event */
                net->writeBlocked = 1;
                break;
            }
            if (errCode == EPROTO && net->secure) {
                httpNetError(net, "Cannot negotiate SSL with server: %s", net->sock->errorMsg);
            } else {
                httpNetError(net, "netConnector: Cannot write. errno %d", errCode);
            }
            net->eof = 1;
            net->error = 1;
            break;

        } else if (written > 0) {
            freeNetPackets(q, written);
            adjustNetVec(q, written);

        } else {
            /* Socket full or SSL negotiate */
            break;
        }
    }
}


/*
    Build the IO vector. Return the count of bytes to be written. Return -1 for EOF.
 */
static MprOff buildNetVec(HttpQueue *q)
{
    HttpPacket  *packet;

    /*
        Examine each packet and accumulate as many packets into the I/O vector as possible. Leave the packets on
        the queue for now, they are removed after the IO is complete for the entire packet.
     */
     for (packet = q->first; packet; packet = packet->next) {
        if (q->ioIndex >= (ME_MAX_IOVEC - 2)) {
            break;
        }
        if (httpGetPacketLength(packet) > 0 || packet->prefix) {
            addPacketForNet(q, packet);
        }
    }
    return q->ioCount;
}


/*
    Add a packet to the io vector. Return the number of bytes added to the vector.
 */
static void addPacketForNet(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;

    net = q->net;
    assert(q->count >= 0);
    assert(q->ioIndex < (ME_MAX_IOVEC - 2));

    net->bytesWritten += httpGetPacketLength(packet);
    if (packet->prefix && mprGetBufLength(packet->prefix) > 0) {
        addToNetVector(q, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix));
    }
    if (packet->content && mprGetBufLength(packet->content) > 0) {
        addToNetVector(q, mprGetBufStart(packet->content), mprGetBufLength(packet->content));
    }
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


static void freeNetPackets(HttpQueue *q, ssize bytes)
{
    HttpPacket  *packet;
    HttpStream  *stream;
    ssize       len;

    assert(q->count >= 0);
    assert(bytes >= 0);

    while ((packet = q->first) != 0) {
        if (packet->flags & HTTP_PACKET_END) {
            if ((stream = packet->stream) != 0) {
                httpFinalizeConnector(stream);
                mprCreateEvent(q->net->dispatcher, "endRequest", 0, httpProcess, stream->inputq, 0);
            }
        } else if (bytes > 0) {
            if (packet->prefix) {
                len = mprGetBufLength(packet->prefix);
                len = min(len, bytes);
                mprAdjustBufStart(packet->prefix, len);
                bytes -= len;
                /* Prefixes don't count in the q->count. No need to adjust */
                if (mprGetBufLength(packet->prefix) == 0) {
                    /* Ensure the prefix is not resent if all the content is not sent */
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
        }
        if (httpGetPacketLength(packet) == 0 && !packet->prefix) {
            /* Done with this packet - consume it. Important for flow control. */
            httpGetPacket(q);
        } else {
            /* Packet still has data to be written */
            break;
        }
    }
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
            Compact the vector
         */
        for (j = 0; i < q->ioIndex; ) {
            iovec[j++] = iovec[i++];
        }
        q->ioIndex = j;
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
