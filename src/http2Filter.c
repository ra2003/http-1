/*
    http2Filter.c - HTTP/2 protocol handling.

    HTTP/2 protocol filter for HTTP/2 frame processing.

    For historical reasons, the HttpStream object is used to implement HTTP2 streams and HttpNet is
    used to implement HTTP2 network connections.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if ME_HTTP_HTTP2
/********************************** Locals ************************************/

#define httpGetPrefixMask(bits) ((1 << (bits)) - 1)
#define httpSetPrefix(bits)     (1 << (bits))

typedef void (*FrameHandler)(HttpQueue *q, HttpPacket *packet);

/************************************ Forwards ********************************/

static void addHeader(HttpStream *stream, cchar *key, cchar *value);
static void checkSendSettings(HttpQueue *q);
static void closeNetworkWhenDone(HttpQueue *q);
static int decodeInt(HttpPacket *packet, uint prefix);
static HttpPacket *defineFrame(HttpQueue *q, HttpPacket *packet, int type, uchar flags, int stream);
static void definePseudoHeaders(HttpStream *stream, HttpPacket *packet);
static void encodeHeader(HttpStream *stream, HttpPacket *packet, cchar *key, cchar *value);
static void encodeInt(HttpPacket *packet, uint prefix, uint bits, uint value);
static void encodeString(HttpPacket *packet, cchar *src, uint lower);
static HttpStream *findStreamObj(HttpNet *net, int stream);
static int getFrameFlags(HttpQueue *q, HttpPacket *packet);
static HttpStream *getStream(HttpQueue *q, HttpPacket *packet);
static void incomingHttp2(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp2(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp2Service(HttpQueue *q);
static void manageFrame(HttpFrame *frame, int flags);
static void parseDataFrame(HttpQueue *q, HttpPacket *packet);
static HttpFrame *parseFrame(HttpQueue *q, HttpPacket *packet);
static void parseGoAwayFrame(HttpQueue *q, HttpPacket *packet);
static void parseHeaderFrame(HttpQueue *q, HttpPacket *packet);
static cchar *parseHeaderField(HttpQueue *q, HttpStream *stream, HttpPacket *packet);
static bool parseHeader(HttpQueue *q, HttpStream *stream, HttpPacket *packet);
static void parseHeaderFrames(HttpQueue *q, HttpStream *stream);
static void parsePriorityFrame(HttpQueue *q, HttpPacket *packet);
static void parsePushFrame(HttpQueue *q, HttpPacket *packet);
static void parsePingFrame(HttpQueue *q, HttpPacket *packet);
static void parseResetFrame(HttpQueue *q, HttpPacket *packet);
static void parseSettingsFrame(HttpQueue *q, HttpPacket *packet);
static void parseWindowFrame(HttpQueue *q, HttpPacket *packet);
static void processDataFrame(HttpQueue *q, HttpPacket *packet);
static void resetStream(HttpStream *stream, cchar *msg, int error);
static ssize resizePacket(HttpQueue *q, ssize max, HttpPacket *packet);
static void sendFrame(HttpQueue *q, HttpPacket *packet);
static void sendGoAway(HttpQueue *q, int status, cchar *fmt, ...);
static void sendPreface(HttpQueue *q);
static void sendReset(HttpQueue *q, HttpStream *stream, int status, cchar *fmt, ...);
static void sendSettings(HttpQueue *q);
static void sendWindowFrame(HttpQueue *q, int stream, ssize size);
static bool validateHeader(cchar *key, cchar *value);

/*
    Frame callback handlers (order matters)
 */
static FrameHandler frameHandlers[] = {
    parseDataFrame,
    parseHeaderFrame,
    parsePriorityFrame,
    parseResetFrame,
    parseSettingsFrame,
    parsePushFrame,
    parsePingFrame,
    parseGoAwayFrame,
    parseWindowFrame,
    /* ContinuationFrame */ parseHeaderFrame,
};

/*
    Just for debug
 */
static char *packetTypes[] = {
    "DATA",
    "HEADERS",
    "PRIORITY",
    "RESET",
    "SETTINGS",
    "PUSH",
    "PING",
    "GOAWAY",
    "WINDOW",
    "CONTINUE",
};

/*********************************** Code *************************************/
/*
    Loadable module initialization
 */
PUBLIC int httpOpenHttp2Filter()
{
    HttpStage     *filter;

    if ((filter = httpCreateStreamector("Http2Filter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->http2Filter = filter;
    filter->incoming = incomingHttp2;
    filter->outgoing = outgoingHttp2;
    filter->outgoingService = outgoingHttp2Service;
    httpCreatePackedHeaders();
    return 0;
}


/*
    Receive and process incoming HTTP/2 packets.
 */
static void incomingHttp2(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream    *stream;
    HttpFrame   *frame;

    net = q->net;

    /*
        Join packets into a single packet for processing. Typically will be only one packet and do nothing.
     */
    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
    checkSendSettings(q);

    /*
        Process frames until can process no more. Initially will be only one packet, but the frame handlers
        may split packets as required and put back the tail for processing here.
     */
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if ((frame = parseFrame(q, packet)) != 0) {
            stream = frame->stream;
            if (net->goaway && stream && (net->lastStreamID && stream->streamID >= net->lastStreamID)) {
                /* Network is being closed. Continue to process existing streams but accept no new streams */
                continue;
            }
            net->frame = frame;
            frameHandlers[frame->type](q, packet);
            net->frame = 0;
            if (stream && stream->disconnect && !stream->destroyed) {
                sendReset(q, stream, HTTP2_INTERNAL_ERROR, "Stream request error %s", stream->errorMsg);
            }
        } else {
            break;
        }
        /*
            Try to push out any pending responses here. This keeps the socketq packet count down.
         */
        httpServiceQueues(net, 0);
    }
    closeNetworkWhenDone(q);
}


/*
    Accept packet for sending
 */
static void outgoingHttp2(HttpQueue *q, HttpPacket *packet)
{
    HttpStream  *stream;

    stream = packet->stream;
    checkSendSettings(q);

    /*
        Determine the HTTP/2 frame type and add to the service queue
     */
    if (packet->flags & HTTP_PACKET_HEADER) {
        if (stream->seenHeader) {
            packet->type = HTTP2_CONT_FRAME;
            stream->seenHeader = 1;
        } else {
            packet->type = HTTP2_HEADERS_FRAME;
        }
    } else if (packet->flags & HTTP_PACKET_DATA) {
        packet->type = HTTP2_DATA_FRAME;
    }
    httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
}


/*
    Service the outgoing queue of packets
 */
static void outgoingHttp2Service(HttpQueue *q)
{
    HttpNet     *net;
    HttpStream    *stream;
    HttpPacket  *packet;
    HttpTx      *tx;
    ssize       len;

    net = q->net;

    for (packet = httpGetPacket(q); packet && !net->error; packet = httpGetPacket(q)) {
        net->lastActivity = net->http->now;
        if (net->outputq->window <= 0) {
            /*
                The output queue has depleted the HTTP/2 transmit window. Flow control and wait for
                a window update message from the peer.
             */
            httpSuspendQueue(q);
            httpPutBackPacket(q, packet);
            break;
        }
        stream = packet->stream;

        /*
            Resize data packets to not exceed the remaining HTTP/2 window flow control credits.
         */
        len = httpGetPacketLength(packet);

        if (packet->flags & HTTP_PACKET_DATA) {
            len = resizePacket(net->outputq, net->outputq->window, packet);
            net->outputq->window -= len;
            assert(net->outputq->window >= 0);
        }
        if (stream && !stream->destroyed) {
            if (stream->streamReset) {
                /* Must not send any more frames on this stream */
                continue;
            }
            if (net->goaway && (net->lastStreamID && stream->streamID >= net->lastStreamID)) {
                /* Network is being closed. Continue to process existing streams but accept no new streams */
                continue;
            }
            if (stream->disconnect) {
                sendReset(q, stream, HTTP2_INTERNAL_ERROR, "Stream request error %s", stream->errorMsg);
                continue;
            }
            stream->lastActivity = stream->http->now;
            tx = stream->tx;

            if (packet->flags & HTTP_PACKET_DATA) {
                if (stream->outputq->window <= 0) {
                    sendReset(q, stream, HTTP2_FLOW_CONTROL_ERROR, "Internal flow control error");
                    return;
                }
            } else if (packet->flags & HTTP_PACKET_END && tx->endData) {
                httpPutPacket(q->net->socketq, packet);
                break;
            }
            /*
                Create and send a HTTP/2 frame
             */
            sendFrame(q, defineFrame(q, packet, packet->type, getFrameFlags(q, packet), stream->streamID));

            /*
                Resume upstream if there is now room
             */
            if (q->count <= q->low && (stream->outputq->flags & HTTP_QUEUE_SUSPENDED)) {
                httpResumeQueue(stream->outputq);
            }
        }
        if (net->outputq->window == 0) {
            httpSuspendQueue(q);
            break;
        }
    }
    closeNetworkWhenDone(q);
}


/*
    Get the HTTP/2 frame flags for this packet
 */
static int getFrameFlags(HttpQueue *q, HttpPacket *packet)
{
    HttpPacket  *first;
    HttpStream    *stream;
    HttpTx      *tx;
    int         flags;

    stream = packet->stream;
    tx = stream->tx;
    flags = 0;
    first = q->first;

    if (packet->flags & HTTP_PACKET_HEADER && !tx->endHeaders) {
        if (!(first && first->flags & HTTP_PACKET_HEADER)) {
            flags |= HTTP2_END_HEADERS_FLAG;
            tx->endHeaders = 1;
        }
        if (first && (first->flags & HTTP_PACKET_END)) {
            tx->endData = 1;
            flags |= HTTP2_END_STREAM_FLAG;
        }
    } else if (packet->flags & HTTP_PACKET_DATA && !tx->endData) {
        if (first && (first->flags & HTTP_PACKET_END)) {
            tx->endData = 1;
            flags |= HTTP2_END_STREAM_FLAG;
        }
    } else if (packet->flags & HTTP_PACKET_END && !tx->endData) {
        /*
            Convert the packet end to a data frame to signify end of stream
         */
        packet->type = HTTP2_DATA_FRAME;
        tx->endData = 1;
        flags |= HTTP2_END_STREAM_FLAG;
    }
    return flags;
}


/*
    Resize a packet to utilize the remaining HTTP/2 window credits. Must not exceed the remaining window size.
 */
static ssize resizePacket(HttpQueue *q, ssize max, HttpPacket *packet)
{
    HttpPacket  *tail;
    ssize       len;

    len = httpGetPacketLength(packet);
    if (len > max) {
        if ((tail = httpSplitPacket(packet, max)) == 0) {
            /* Memory error - centrally reported */
            return len;
        }
        httpPutBackPacket(q, tail);
        len = httpGetPacketLength(packet);
    }
    return len;
}


/*
    Close the network connection on errors of if instructed to go away.
 */
static void closeNetworkWhenDone(HttpQueue *q)
{
    HttpNet     *net;

    net = q->net;
    if (net->error) {
        if (!net->goaway) {
            sendGoAway(net->socketq, HTTP2_PROTOCOL_ERROR, "Closing network");
        }
    }
    if (net->goaway) {
        if (mprGetListLength(net->streams) == 0) {
            /* This ensures a recall on the netConnector IOEvent handler */
            mprDisconnectSocket(net->sock);
        }
    }
}


/*
    Parse an incoming HTTP/2 frame. Return true to keep going with this or subsequent request, zero means
    insufficient data to proceed.
 */
static HttpFrame *parseFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpPacket  *tail;
    HttpFrame   *frame;
    MprBuf      *buf;
    ssize       frameLength, len, size;
    uint32      lenType;
    cchar       *typeStr;
    int         type;

    net = q->net;
    buf = packet->content;

    if (httpGetPacketLength(packet) < sizeof(HTTP2_FRAME_OVERHEAD)) {
        /* Insufficient data */
        httpPutBackPacket(q, packet);
        return 0;
    }
    /*
        Peek at the frame length and type and validate.
     */
    lenType = mprPeekUint32FromBuf(buf);
    len = lenType >> 8;
    if (len > q->packetSize || len > HTTP2_MAX_FRAME_SIZE) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad frame size %d vs %d", len, q->packetSize);
        return 0;
    }
    frameLength = len + HTTP2_FRAME_OVERHEAD;
    size = httpGetPacketLength(packet);

    /*
        Split data for a following frame and put back on the queue for later servicing.
     */
    if (frameLength < size) {
        if ((tail = httpSplitPacket(packet, frameLength)) == 0) {
            /* Memory error - centrally reported */
            return 0;
        }
        httpPutBackPacket(q, tail);
        buf = packet->content;

    } else if (frameLength > size) {
        httpPutBackPacket(q, packet);
        return 0;
    }
    mprAdjustBufStart(packet->content, sizeof(uint32));

    /*
        Parse the various HTTP/2 frame fields and store in a local HttpFrame object.
     */
    if ((frame = mprAllocObj(HttpFrame, manageFrame)) == NULL) {
        /* Memory error - centrally reported */
        return 0;
    }
    packet->data = frame;

    type = lenType & 0xFF;
    frame->type = type;
    frame->flags = mprGetCharFromBuf(buf);
    frame->streamID = mprGetUint32FromBuf(buf) & HTTP_STREAM_MASK;
    frame->stream = findStreamObj(net, frame->streamID);

    if (httpTracing(q->net)) {
        typeStr = (type < HTTP2_MAX_FRAME) ? packetTypes[type] : "unknown";
        mprAdjustBufStart(packet->content, -HTTP2_FRAME_OVERHEAD);
        httpTracePacket(q->net->trace, "http2.rx", "packet", HTTP_TRACE_HEX, packet,
            "frame=%s flags=%x stream=%d length=%zd", typeStr, frame->flags, frame->streamID, httpGetPacketLength(packet));
        mprAdjustBufStart(packet->content, HTTP2_FRAME_OVERHEAD);
    }
    if (frame->streamID && !frame->stream) {
#if TODO
        if (frame->streamID < net->lastStreamID) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Closed stream being reused");
            return 0;
        }
#endif
        if (frame->type == HTTP2_DATA_FRAME) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid frame without a stream type %d, stream %d", frame->type, frame->streamID);
            return 0;
        }
        if (frame->type == HTTP2_RESET_FRAME) {
            /* Just ignore, may get a peer reset after we have already reset */
            return 0;
        }
    }
    if (frame->type < 0 || frame->type >= HTTP2_MAX_FRAME) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid frame type %d", frame->type);
        return 0;
    }
    return frame;
}


/*
    Always get a settings frame at the start of any network connection
 */
static void parseSettingsFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpFrame   *frame;
    HttpLimits  *limits;
    MprBuf      *buf;
    uint        field, value;

    net = q->net;
    limits = net->limits;
    buf = packet->content;
    frame = packet->data;

    if (frame->flags & HTTP2_ACK_FLAG || net->goaway) {
        /* Nothing to do */
        return;
    }
    while (httpGetPacketLength(packet) >= HTTP2_SETTINGS_SIZE) {
        field = mprGetUint16FromBuf(buf);
        value = mprGetUint32FromBuf(buf);

        switch (field) {
        case HTTP2_HEADER_TABLE_SIZE_SETTING:
            value = min((int) value, limits->hpackMax);
            httpSetPackedHeadersMax(net->txHeaders, value);
            break;

        case HTTP2_ENABLE_PUSH_SETTING:
            if (value != 0 && value != 1) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid push value");
                return;
            }
            /* Push is not yet supported, we just store the value but do nothing */
            net->push = value;
            break;

        case HTTP2_MAX_STREAMS_SETTING:
            /* Permit peer supporting more streams, but don't ever create more than streamsMax limit */
            if (value <= 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Too many streams setting %d max %d", value, ME_MAX_STREAMS);
                return;
            }
            limits->txStreamsMax = min((int) value, limits->streamsMax);
            break;

        case HTTP2_INIT_WINDOW_SIZE_SETTING:
            if (value < HTTP2_MIN_WINDOW || value > HTTP2_MAX_WINDOW) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid window size setting %d max %d", value, HTTP2_MAX_WINDOW);
                return;
            }
            net->outputq->window = value;
            break;

        case HTTP2_MAX_FRAME_SIZE_SETTING:
            /* Permit peer supporting bigger frame sizes, but don't ever create packets larger than the packetSize limit */
            if (value <= 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid frame size setting %d max %d", value, ME_PACKET_SIZE);
                return;
            }
            if (value < net->outputq->packetSize) {
                net->outputq->packetSize = min(value, ME_PACKET_SIZE);
                #if TODO && TBD
                httpResizePackets(net->outputq);
                #endif
            }
            break;

        case HTTP2_MAX_HEADER_SIZE_SETTING:
            if (value <= 0 || value > ME_MAX_HEADERS) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header size setting %d max %d", value, ME_MAX_HEADERS);
                return;
            }
            if ((int) value < limits->headerSize) {
                limits->headerSize = value;
            }
            break;

        default:
            /* Ignore unknown settings values (per spec) */
            break;
        }
    }
    if (httpGetPacketLength(packet) > 0) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid setting packet length");
        return;
    }
    mprFlushBuf(packet->content);
    sendFrame(q, defineFrame(q, packet, HTTP2_SETTINGS_FRAME, HTTP2_ACK_FLAG, 0));
}


/*
    Parse a HTTP header or HTTP header continuation frame
 */
static void parseHeaderFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream    *stream;
    HttpFrame   *frame;
    MprBuf      *buf;
    bool        padded, priority;
    ssize       size, frameLen;
    int         padLen; 

    net = q->net;
    buf = packet->content;
    frame = packet->data;
    padded = frame->flags & HTTP2_PADDED_FLAG;
    priority = frame->flags & HTTP2_PRIORITY_FLAG;

    size = 0;
    if (padded) {
        size++;
    }
    if (priority) {
        /* dependency + weight are ignored */
        size += sizeof(uint32) + 1;
    }
    frameLen = mprGetBufLength(buf);
    if (frameLen <= size) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Incorrect header length");
        return;
    }
    if (padded) {
        padLen = (int) mprGetCharFromBuf(buf);
        if (padLen >= frameLen) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Incorrect padding length");
            return;
        }
        mprAdjustBufEnd(buf, -padLen);
    }
#if FUTURE
    int depend, dword, excl;
    depend = 0;
    weight = HTTP2_DEFAULT_WEIGHT;
    if (priority) {
        dword = mprGetUint32FromBuf(buf);
        depend = dword & 0x7fffffff;
        excl = dword >> 31;
        weight = mprGetCharFromBuf(buf) + 1;
    }
#endif
    if ((frame->streamID % 2) != 1 || (net->lastStreamID && frame->streamID <= net->lastStreamID)) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad sesssion");
        return;
    }
    if ((stream = getStream(q, packet)) != 0) {
         if (frame->flags & HTTP2_END_HEADERS_FLAG) {
            parseHeaderFrames(q, stream);
        }
        /*
            Must only update for a successfully received frame
         */
        if (!net->error && frame->type == HTTP2_HEADERS_FRAME) {
            net->lastStreamID = frame->streamID;
        }
    }
}


/*
    Get or create a stream connection
 */
static HttpStream *getStream(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream    *stream;
    HttpRx      *rx;
    HttpFrame   *frame;

    net = q->net;
    frame = packet->data;
    stream = frame->stream;
    frame = packet->data;
    assert(frame->stream);

    if (!stream && httpIsServer(net)) {
        if (net->goaway) {
            /*
                Ignore new streams as the network is going away. Don't send a reset, just ignore.

                sendReset(q, stream, HTTP2_REFUSED_STREAM, "Network is going away");
             */
            return 0;
        }
        if ((stream = httpCreateStream(net, 0)) == 0) {
            /* Memory error - centrally reported */
            return 0;
        }
        stream->streamID = frame->streamID;
        frame->stream = stream;

        /*
            Servers create a new connection stream. Note: HttpStream is used for HTTP/2 streams (legacy).
         */
        if (mprGetListLength(net->streams) >= net->limits->requestsPerClientMax) {
            sendReset(q, stream, HTTP2_REFUSED_STREAM, "Too many streams for IP: %s %d/%d", net->ip,
                (int) mprGetListLength(net->streams), net->limits->requestsPerClientMax);
            return 0;
        }
        ///TODO httpMonitorEvent(stream, HTTP_COUNTER_REQUESTS, 1);
        if (mprGetListLength(net->streams) >= net->limits->streamsMax) {
            sendReset(q, stream, HTTP2_REFUSED_STREAM, "Too many streams for connection: %s %d/%d", net->ip,
                (int) mprGetListLength(net->streams), net->limits->streamsMax);
            return 0;
        }
    }
#if FUTURE
    if (depend == frame->streamID) {
        sendReset(q, stream, HTTP2_PROTOCOL_ERROR, "Bad stream dependency");
        return 0;
    }
#endif
    if (frame->type == HTTP2_CONT_FRAME && (!stream->rx || !stream->rx->headerPacket)) {
        if (!frame->stream) {
            sendReset(q, stream, HTTP2_REFUSED_STREAM, "Invalid continuation frame");
            return 0;
        }
    }
    rx = stream->rx;
    if (frame->flags & HTTP2_END_STREAM_FLAG) {
        rx->eof = 1;
    }
    if (rx->headerPacket) {
        httpJoinPacket(rx->headerPacket, packet);
    } else {
        rx->headerPacket = packet;
    }
    packet->stream = stream;

    if (httpGetPacketLength(rx->headerPacket) > stream->limits->headerSize) {
        sendReset(q, stream, HTTP2_REFUSED_STREAM, "Header too big, length %ld, limit %ld",
            httpGetPacketLength(rx->headerPacket), stream->limits->headerSize);
        return 0;
    }
    return stream;
}


/*
    Priority frames are not yet implemented. They are parsed but not validated or implemented.
 */
static void parsePriorityFrame(HttpQueue *q, HttpPacket *packet)
{
#if FUTURE
    MprBuf  *buf;
    int     dep, exclusive, weight;

    buf = packet->content;
    dep = mprGetUint32FromBuf(buf);
    exclusive = dep & (1 << 31);
    dep &= (1U << 31) - 1;
    weight = mprGetCharFromBuf(buf);
#endif
}


/*
    Push frames are not yet implemented: TODO
 */
static void parsePushFrame(HttpQueue *q, HttpPacket *packet)
{
}


/*
    Receive a ping frame
 */
static void parsePingFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpFrame   *frame;

    if (q->net->goaway) {
        return;
    }
    frame = packet->data;
    if (frame->stream) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad stream in ping frame");
        return;
    }
    if (!(frame->flags & HTTP2_ACK_FLAG)) {
        /* Resend the ping payload with the acknowledgement */
        sendFrame(q, defineFrame(q, packet, HTTP2_PING_FRAME, HTTP2_ACK_FLAG, 0));
    }
}


/*
    Peer is instructing the stream to be closed.
 */
static void parseResetFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpFrame   *frame;
    uint32      error;

    if (httpGetPacketLength(packet) != sizeof(uint32)) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad reset frame");
        return;
    }
    frame = packet->data;
    if (!frame->stream) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad stream in reset frame");
        return;
    }
    frame->stream->streamReset = 1;
    error = mprGetUint32FromBuf(packet->content) & HTTP_STREAM_MASK;
    resetStream(frame->stream, "Stream reset by peer", error);
}


/*
    Receive a GoAway which informs us that this network should not be used anymore.
 */
static void parseGoAwayFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream    *stream;
    MprBuf      *buf;
    cchar       *msg;
    ssize       len;
    int         error, lastStreamID, next;

    net = q->net;
    buf = packet->content;
    lastStreamID = mprGetUint32FromBuf(buf) & HTTP_STREAM_MASK;
    error = mprGetUint32FromBuf(buf);
    len = mprGetBufLength(buf);
    msg = len ? snclone(buf->start, len) : "";
    httpTrace(net->trace, "http2.rx", "context", "msg='Receive GoAway. %s' error=%d lastStream=%d", msg, error, lastStreamID);

    for (ITERATE_ITEMS(net->streams, stream, next)) {
        if (stream->streamID > lastStreamID) {
            resetStream(stream, "Stream reset by peer", HTTP2_REFUSED_STREAM);
        }
    }
    net->goaway = 1;
    net->receivedGoaway = 1;
}


/*
    Receive a window update frame that increases the window size of permissible data to send.
    This is a credit based system for flow control of both the network and the stream.
 */
static void parseWindowFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpStream    *stream;
    HttpFrame   *frame;
    int         increment;

    net = q->net;
    frame = packet->data;
    increment = mprGetUint32FromBuf(packet->content);
    if (frame->stream) {
        if ((stream = frame->stream) != 0) {
            if (increment > (HTTP2_MAX_WINDOW - stream->outputq->window)) {
                sendReset(q, stream, HTTP2_FLOW_CONTROL_ERROR, "Invalid window update for stream %d", stream->streamID);
            } else {
                stream->outputq->window += increment;
                httpResumeQueue(stream->outputq);
            }
        }
    } else {
        if (increment > (HTTP2_MAX_WINDOW + 1 - net->outputq->window)) {
            sendGoAway(q, HTTP2_FLOW_CONTROL_ERROR, "Invalid window update for network");
        } else {
            net->outputq->window += increment;
            httpResumeQueue(net->outputq);
        }
    }
}


/*
    Once the hader and all continuation frames are received, they are joined into a single rx->headerPacket.
 */
static void parseHeaderFrames(HttpQueue *q, HttpStream *stream)
{
    HttpNet     *net;
    HttpPacket  *packet;
    HttpRx      *rx;

    net = stream->net;
    rx = stream->rx;
    packet = rx->headerPacket;
    while (httpGetPacketLength(packet) > 0 && !net->error && !net->goaway && !stream->error) {
        if (!parseHeader(q, stream, packet)) {
            sendReset(q, stream, HTTP2_STREAM_CLOSED, "Cannot parse headers");
            break;
        }
    }
    if (!net->goaway) {
        if (!stream->error) {
            stream->state = HTTP_STATE_PARSED;
        }
        httpProcessHeaders(stream->inputq);
        httpProcess(stream->inputq);
    }
}


/*
    Parse the next header item in the packet of headers
 */
static bool parseHeader(HttpQueue *q, HttpStream *stream, HttpPacket *packet)
{
    HttpNet     *net;
    MprBuf      *buf;
    MprKeyValue *kp;
    cchar       *name, *value;
    uchar       ch;
    int         index, max;

    net = stream->net;
    buf = packet->content;

    /*
        Decode the type of header record. It can be:
        1. Fully indexed header field.
        2. Literal header that should be added to the header table.
        3. Literal header without updating the header table.
     */
    ch = mprLookAtNextCharInBuf(buf);
    if ((ch >> 7) == 1) {
        /*
            Fully indexed header field
         */
        index = decodeInt(packet, 7);
        if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return 0;
        }
        addHeader(stream, kp->key, kp->value);

    } else if ((ch >> 6) == 1) {
        /*
            Literal header and add to index
         */
        if ((index = decodeInt(packet, 6)) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return 0;
        } else if (index > 0) {
            if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Unknown header index");
                return 0;
            }
            name = kp->key;
        } else {
            name = parseHeaderField(q, stream, packet);
        }
        value = parseHeaderField(q, stream, packet);
        if (!name || !value) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header name/value");
            return 0;
        }
        addHeader(stream, name, value);
        if (httpAddPackedHeader(net->rxHeaders, name, value) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Cannot fit header in hpack table");
            return 0;
        }

    } else if ((ch >> 5) == 1) {
        /* Dynamic table max size update */
        max = decodeInt(packet, 5);
        if (httpSetPackedHeadersMax(net->rxHeaders, max) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Cannot add indexed header");
            return 0;
        }

    } else /* if ((ch >> 4) == 1 || (ch >> 4) == 0)) */ {
        /* Literal header field without indexing */
        if ((index = decodeInt(packet, 4)) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return 0;
        } else if (index > 0) {
            if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Unknown header index");
                return 0;
            }
            name = kp->key;
        } else {
            name = parseHeaderField(q, stream, packet);
        }
        value = parseHeaderField(q, stream, packet);
        if (!name || !value) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header name/value");
            return 0;
        }
        addHeader(stream, name, value);
    }
    return 1;
}


/*
    Parse a single header field
 */
static cchar *parseHeaderField(HttpQueue *q, HttpStream *stream, HttpPacket *packet)
{
    MprBuf      *buf;
    cchar       *value;
    int         huff, len;

    buf = packet->content;

    huff = ((uchar) mprLookAtNextCharInBuf(buf)) >> 7;
    len = decodeInt(packet, 7);
    if (len < 0 || len > mprGetBufLength(buf)) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header field length");
        return 0;
    }
    if (huff) {
        /*
            Huffman encoded
         */
        if ((value = httpHuffDecode((uchar*) mprGetBufStart(buf), len)) == 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid encoded header field");
            return 0;
        }
    } else {
        /* Literal */
        value = snclone(buf->start, len);
    }
    mprAdjustBufStart(buf, len);
    return value;
}


/*
    Add a header key/value pair to the set of headers for the stream (stream)
 */
static void addHeader(HttpStream *stream, cchar *key, cchar *value)
{
    HttpRx      *rx;
    HttpLimits  *limits;
    ssize       len;

    rx = stream->rx;
    limits = stream->limits;

    if (!validateHeader(key, value)) {
        return;
    }
    if (key[0] == ':') {
        if (key[1] == 'a' && smatch(key, ":authority")) {
            mprAddKey(stream->rx->headers, "host", value);

        } else if (key[1] == 'm' && smatch(key, ":method")) {
            rx->originalMethod = rx->method = supper(value);
            httpParseMethod(stream);

        } else if (key[1] == 'p' && smatch(key, ":path")) {
            len = slen(value);
            if (*value == '\0') {
                httpBadRequestError(stream, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
                return;
            } else if (len >= limits->uriSize) {
                httpLimitError(stream, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
                    "Bad request. URI too long. Length %zd vs limit %d", len, limits->uriSize);
                return;
            }
            rx->uri = sclone(value);
            if (!rx->originalUri) {
                rx->originalUri = rx->uri;
            }
        } else if (key[1] == 's') {
            if (smatch(key, ":status")) {
                rx->status = atoi(value);

            } else if (smatch(key, ":scheme")) {
                ;
            }
        }
    } else {
        if (scaselessmatch(key, "set-cookie")) {
            mprAddDuplicateKey(rx->headers, key, value);
        } else {
            mprAddKey(rx->headers, key, value);
        }
    }
}


/*
    Briefly, do some validation
 */
static bool validateHeader(cchar *key, cchar *value)
{
    uchar   *cp, c;

    if (!key || *key == 0 || !value || !value) {
        return 0;
    }
    if (*key == ':') {
        key++;
    }
    for (cp = (uchar*) key; *cp; cp++) {
        c = *cp;
        if (('a' <= c && c <= 'z') || c == '-' || c == '_' || ('0' <= c && c <= '9')) {
            continue;
        }
        if (c == '\0' || c == '\n' || c == '\r' || c == ':' || ('A' <= c && c <= 'Z')) {
            // mprLog("info http", 5, "Invalid header name %s", key);
            return 0;
        }
    }
    for (cp = (uchar*) value; *cp; cp++) {
        c = *cp;
        if (c == '\0' || c == '\n' || c == '\r') {
            // mprLog("info http", 5, "Invalid header value %s", value);
            return 0;
        }
    }
    return 1;
}


/*
    Receive an application data frame
 */
static void parseDataFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpFrame   *frame;
    HttpStream  *stream;
    HttpLimits  *limits;
    MprBuf      *buf;
    ssize       len, padLen, frameLen;
    int         padded;

    net = q->net;
    limits = net->limits;
    buf = packet->content;
    frame = packet->data;
    len = httpGetPacketLength(packet);
    stream = frame->stream;
    assert(stream);

    if (stream->streamReset) {
        sendReset(q, stream, HTTP2_STREAM_CLOSED, "Received data on closed stream %d", stream->streamID);
        return;
    }
    padded = frame->flags & HTTP2_PADDED_FLAG;
    if (padded) {
        frameLen = mprGetBufLength(buf);
        padLen = (int) mprGetCharFromBuf(buf);
        if (padLen >= frameLen) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Incorrect padding length");
            return;
        }
        mprAdjustBufEnd(buf, -padLen);
    }
    processDataFrame(q, packet);

    /*
        Network flow control, do after processing the data frame incase the stream is now complete.
     */
    if (len > net->inputq->window) {
        sendGoAway(q, HTTP2_FLOW_CONTROL_ERROR, "Peer exceeded flow control window");
        return;
    }
    net->inputq->window -= len;
    if (net->inputq->window <= net->inputq->packetSize) {
        /*
            Update the remote window size for network flow control
         */
        sendWindowFrame(q, 0, limits->window - net->inputq->window);
        net->inputq->window = limits->window;
    }

    /*
        Stream flow control
     */
    if (!stream->destroyed) {
        if (len > stream->inputq->window) {
            sendReset(q, stream, HTTP2_FLOW_CONTROL_ERROR, "Receive data exceeds window for stream");
            return;
        }
        stream->inputq->window -= len;
        if (stream->inputq->window <= net->inputq->packetSize) {
            /*
                Update the remote window size for stream flow control
             */
            sendWindowFrame(q, stream->streamID, limits->window - stream->inputq->window);
            stream->inputq->window = limits->window;
        }
    }
}


/*
    Process the frame and add to the stream input queue
 */
static void processDataFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpFrame   *frame;
    HttpStream    *stream;

    frame = packet->data;
    stream = frame->stream;

    if (frame->flags & HTTP2_END_STREAM_FLAG) {
        stream->rx->eof = 1;
    }
    if (httpGetPacketLength(packet) > 0) {
        httpPutPacket(stream->inputq, packet);
    }
    httpProcess(stream->inputq);
}


/*
    Shutdown a network. This is not necessarily an error. Peer should open a new network.
    Continue processing current streams, but stop processing any new streams.
 */
static void sendGoAway(HttpQueue *q, int status, cchar *fmt, ...)
{
    HttpNet     *net;
    HttpPacket  *packet;
    HttpStream    *stream;
    MprBuf      *buf;
    va_list     ap;
    cchar       *msg;
    int         next;

    net = q->net;
    if (net->goaway) {
        return;
    }
    if ((packet = httpCreatePacket(HTTP2_GOAWAY_SIZE)) == 0) {
        return;
    }
    va_start(ap, fmt);
    net->errorMsg = msg = sfmtv(fmt, ap);
    httpTrace(net->trace, "http2.tx", "error", "Send network goAway, lastStream=%d, status=%d, msg='%s'", net->lastStreamID, status, msg);
    va_end(ap);

    buf = packet->content;
    mprPutUint32ToBuf(buf, status);
    mprPutUint32ToBuf(buf, net->lastStreamID);
    mprPutStringToBuf(buf, msg);
    sendFrame(q, defineFrame(q, packet, HTTP2_GOAWAY_FRAME, 0, 0));

    for (ITERATE_ITEMS(q->net->streams, stream, next)) {
        if (stream->streamID > net->lastStreamID) {
            resetStream(stream, "Stream terminated", HTTP2_REFUSED_STREAM);
        }
    }
    net->goaway = 1;
}


/*
    Public API to terminate a network connection
 */
PUBLIC void httpSendGoAway(HttpNet *net, int status, cchar *fmt, ...)
{
    va_list     ap;
    cchar       *msg;

    va_start(ap, fmt);
    msg = sfmtv(fmt, ap);
    va_end(ap);
    sendGoAway(net->outputq, status, "%s", msg);
}


/*
    Send a ping packet. Some intermediaries or peers may use pings to keep a connection alive.
 */
PUBLIC bool sendPing(HttpQueue *q, uchar *data)
{
    HttpPacket  *packet;

    if ((packet = httpCreatePacket(HTTP2_WINDOW_SIZE)) == 0) {
        return 0;
    }
    mprPutBlockToBuf(packet->content, (char*) data, 64);
    sendFrame(q, defineFrame(q, packet, HTTP2_PING_FRAME, 0, 0));
    return 1;
}


/*
    Immediately close a stream. The peer is informed and the stream is disconnected.
 */
static void sendReset(HttpQueue *q, HttpStream *stream, int status, cchar *fmt, ...)
{
    HttpPacket  *packet;
    va_list     ap;
    char        *msg;

    assert(stream);

    if (stream->streamReset || stream->destroyed) {
        return;
    }
    if ((packet = httpCreatePacket(HTTP2_RESET_SIZE)) == 0) {
        return;
    }
    va_start(ap, fmt);
    msg = sfmtv(fmt, ap);
    httpTrace(stream->trace, "http2.tx", "context", "Send stream reset, stream=%d, status=%d, msg='%s'", stream->streamID, status, msg);
    va_end(ap);

    mprPutUint32ToBuf(packet->content, status);
    sendFrame(q, defineFrame(q, packet, HTTP2_RESET_FRAME, 0, stream->streamID));

    httpError(stream, HTTP_CODE_COMMS_ERROR, "%s", msg);
    stream->streamReset = 1;
    httpProcess(stream->inputq);
}


/*
    Mark a stream as being reset (terminated)
 */
static void resetStream(HttpStream *stream, cchar *msg, int error)
{
    httpTrace(stream->trace, "http2.rx", "context", "msg='Receive GoAway. %s' error=%d", msg, error);
    if (error) {
        httpError(stream, HTTP_CODE_COMMS_ERROR, "%s", msg);
    }
    httpProcess(stream->inputq);
}


/*
    A stream must exchange settings before it is used
 */
static void checkSendSettings(HttpQueue *q)
{
    HttpNet     *net;

    net = q->net;

    if (!net->init) {
        sendSettings(q);
        net->init = 1;
    }
}


/*
    Clients must send a preface before settings
 */
static void sendPreface(HttpQueue *q)
{
    HttpPacket  *packet;

    if ((packet = httpCreatePacket(HTTP2_PREFACE_SIZE)) == 0) {
        return;
    }
    packet->flags = 0;
    mprPutBlockToBuf(packet->content, HTTP2_PREFACE, HTTP2_PREFACE_SIZE);
    httpPutPacket(q->net->socketq, packet);
}


/*
    Send a settings packet before using the stream
 */
static void sendSettings(HttpQueue *q)
{
    HttpNet     *net;
    HttpPacket  *packet;
    ssize       size;

    net = q->net;
    if (!net->init && httpIsClient(net)) {
        sendPreface(q);
    }
    //  TODO - set to the number of settings
    if ((packet = httpCreatePacket(HTTP2_SETTINGS_SIZE * 3)) == 0) {
        return;
    }
    mprPutUint16ToBuf(packet->content, HTTP2_MAX_STREAMS_SETTING);
    mprPutUint32ToBuf(packet->content, net->limits->streamsMax - net->ownStreams);

    mprPutUint16ToBuf(packet->content, HTTP2_INIT_WINDOW_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, (uint32) net->inputq->window);

    mprPutUint16ToBuf(packet->content, HTTP2_MAX_FRAME_SIZE_SETTING);
    size = max(net->inputq->packetSize, HTTP2_MIN_FRAME_SIZE);
    mprPutUint32ToBuf(packet->content, (uint32) size);

#if FUTURE
    mprPutUint16ToBuf(packet->content, HTTP2_HEADER_TABLE_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, HTTP2_TABLE_SIZE);
    mprPutUint16ToBuf(packet->content, HTTP2_MAX_HEADER_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, (uint32) net->limits->headerSize);
    mprPutUint16ToBuf(packet->content, HTTP2_ENABLE_PUSH_SETTING);
    mprPutUint32ToBuf(packet->content, 0);
#endif

    sendFrame(q, defineFrame(q, packet, HTTP2_SETTINGS_FRAME, 0, 0));
}


static void sendWindowFrame(HttpQueue *q, int stream, ssize inc)
{
    HttpPacket  *packet;

    if ((packet = httpCreatePacket(HTTP2_WINDOW_SIZE)) == 0) {
        return;
    }
    mprPutUint32ToBuf(packet->content, (uint32) inc);
    sendFrame(q, defineFrame(q, packet, HTTP2_WINDOW_FRAME, 0, stream));
}


/*
    Populate the HTTP headers as a HTTP/2 header packet in the given packet
    This is called from the tailFilter and the packet is then split into packetSize chunks and passed to outgoingHttp2.
    There, the relevant HTTP/2 packet type is assigned HTTP2_HEADERS_FRAME or HTTP2_CONT_FRAME.
 */
PUBLIC void httpCreateHeaders2(HttpQueue *q, HttpPacket *packet)
{
    HttpStream    *stream;
    HttpTx      *tx;
    MprKey      *kp;

    assert(packet->flags == HTTP_PACKET_HEADER);

    stream = packet->stream;
    tx = stream->tx;
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->responded = 1;

    httpPrepareHeaders(stream);
    definePseudoHeaders(stream, packet);
    if (httpTracing(q->net)) {
        httpTrace(stream->trace, "http2.tx", "headers", "\n%s", httpTraceHeaders(q, stream->tx->headers));
    }

    /*
        Not emitting any padding, dependencies or weights.
     */
    for (ITERATE_KEYS(tx->headers, kp)) {
        if (kp->key[0] == ':') {
            if (smatch(kp->key, ":status")) {
                switch (tx->status) {
                case 200:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_200); break;
                case 204:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_204); break;
                case 206:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_206); break;
                case 304:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_304); break;
                case 400:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_400); break;
                case 404:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_404); break;
                case 500:
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_STATUS_500); break;
                default:
                    encodeHeader(stream, packet, kp->key, kp->data);
                }
            } else if (smatch(kp->key, ":method")){
                if (smatch(kp->data, "GET")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_METHOD_GET);
                } else if (smatch(kp->data, "POST")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_METHOD_POST);
                } else {
                    encodeHeader(stream, packet, kp->key, kp->data);
                }
            } else if (smatch(kp->key, ":path")) {
                if (smatch(kp->data, "/")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_PATH_ROOT);
                } else if (smatch(kp->data, "/index.html")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_PATH_INDEX);
                } else {
                    encodeHeader(stream, packet, kp->key, kp->data);
                }
            } else {
                encodeHeader(stream, packet, kp->key, kp->data);
            }
        }
    }
    for (ITERATE_KEYS(tx->headers, kp)) {
        if (kp->key[0] != ':') {
            encodeHeader(stream, packet, kp->key, kp->data);
        }
    }
}


/*
    Define the pseudo headers for status, method, scheme and authority.
 */
static void definePseudoHeaders(HttpStream *stream, HttpPacket *packet)
{
    HttpUri     *parsedUri;
    HttpTx      *tx;
    Http        *http;
    cchar       *authority, *path;

    http = stream->http;
    tx = stream->tx;

    if (httpServerStream(stream)) {
        httpAddHeaderString(stream, ":status", itos(tx->status));

    } else {
        authority = stream->rx->hostHeader ? stream->rx->hostHeader : stream->ip;
        httpAddHeaderString(stream, ":method", tx->method);
        httpAddHeaderString(stream, ":scheme", stream->secure ? "https" : "http");
        httpAddHeaderString(stream, ":authority", authority);

        parsedUri = tx->parsedUri;
        if (http->proxyHost && *http->proxyHost) {
            if (parsedUri->query && *parsedUri->query) {
                path = sfmt("http://%s:%d%s?%s", http->proxyHost, http->proxyPort, parsedUri->path, parsedUri->query);
            } else {
                path = sfmt("http://%s:%d%s", http->proxyHost, http->proxyPort, parsedUri->path);
            }
        } else {
            if (parsedUri->query && *parsedUri->query) {
                path = sfmt("%s?%s", parsedUri->path, parsedUri->query);
            } else {
                path = parsedUri->path;
            }
        }
        httpAddHeaderString(stream, ":path", path);
    }
}


/*
    Encode headers using the HPACK and huffman encoding.
 */
static void encodeHeader(HttpStream *stream, HttpPacket *packet, cchar *key, cchar *value)
{
    HttpNet     *net;
    int         index;
    bool        indexedValue;

    net = stream->net;
    stream->tx->headerSize = 0;

    if ((index = httpLookupPackedHeader(net->txHeaders, key, value, &indexedValue)) > 0) {
        if (indexedValue) {
            /* Fully indexed header key and value */
            encodeInt(packet, httpSetPrefix(7), 7, index);
        } else {
            encodeInt(packet, httpSetPrefix(6), 6, index);
            index = httpAddPackedHeader(net->txHeaders, key, value);
            encodeString(packet, value, 0);
        }
    } else {
        index = httpAddPackedHeader(net->txHeaders, key, value);
        encodeInt(packet, httpSetPrefix(6), 6, 0);
        encodeString(packet, key, 1);
        encodeString(packet, value, 0);
#if FUTURE
        //  no indexing
        encodeInt(packet, 0, 4, 0);
        encodeString(packet, key, 1);
        encodeString(packet, value, 0);
#endif
    }
}


/*
    Decode a HPACK encoded integer
 */
static int decodeInt(HttpPacket *packet, uint bits)
{
    MprBuf      *buf;
    uchar       *bp, *end, *start;
    uint        mask, shift, value;
    int         done;

    if (bits < 0 || bits > 8 || !packet || httpGetPacketLength(packet) == 0) {
        return MPR_ERR_BAD_STATE;
    }
    buf = packet->content;
    bp = start = (uchar*) mprGetBufStart(buf);
    end = (uchar*) mprGetBufEnd(buf);

    mask = httpGetPrefixMask(bits);
    value = *bp++ & mask;
    if (value == mask) {
        value = 0;
        shift = 0;
        do {
            if (bp >= end) {
                return MPR_ERR_BAD_STATE;
            }
            done = (*bp & 0x80) != 0x80;
            value += (*bp++ & 0x7f) << shift;
            shift += 7;
        } while (!done);
        value += mask;
    }
    mprAdjustBufStart(buf, bp - start);
    return value;
}


/*
    Encode an integer using HPACK.
 */
static void encodeInt(HttpPacket *packet, uint flags, uint bits, uint value)
{
    MprBuf      *buf;
    uint        mask;

    buf = packet->content;
    mask = (1 << bits) - 1;

    if (value < mask) {
        mprPutCharToBuf(buf, flags | value);
    } else {
        mprPutCharToBuf(buf, flags | mask);
        value -= mask;
        while (value >= 128) {
            mprPutCharToBuf(buf, value % 128 + 128);
            value /= 128;
        }
        mprPutCharToBuf(buf, (uchar) value);
    }
}


/*
    Encode a string using HPACK.
 */
static void encodeString(HttpPacket *packet, cchar *src, uint lower)
{
    MprBuf      *buf;
    cchar       *cp;
    char        *dp, *encoded;
    ssize       len, hlen, extra;

    buf = packet->content;
    len = slen(src);

    /*
        Encode the string in the buffer. Allow some extra space incase the huff encoding is bigger then src and
        some room after the end of the buffer for an encoded integer length.
     */
    extra = 16;
    if (mprGetBufSpace(buf) < (len + extra)) {
        mprGrowBuf(buf, (len + extra) - mprGetBufSpace(buf));
    }
    /*
        Leave room at the end of the buffer for an encoded integer.
     */
    encoded = mprGetBufEnd(buf) + (extra / 2);
    hlen = httpHuffEncode(src, slen(src), encoded, lower);
    assert((encoded + hlen) < buf->endbuf);
    assert(hlen < len);

    if (hlen > 0) {
        encodeInt(packet, HTTP2_ENCODE_HUFF, 7, (uint) hlen);
        memmove(mprGetBufEnd(buf), encoded, hlen);
        mprAdjustBufEnd(buf, hlen);
    } else {
        encodeInt(packet, 0, 7, (uint) len);
        if (lower) {
            for (cp = src, dp = mprGetBufEnd(buf); cp < &src[len]; cp++) {
                *dp++ = tolower(*cp);
            }
            assert(dp < buf->endbuf);
        } else {
            memmove(mprGetBufEnd(buf), src, len);
        }
        mprAdjustBufEnd(buf, len);
    }
    assert(buf->end < buf->endbuf);
}


/*
    Define a frame in the given packet. If null, allocate a packet.
 */
static HttpPacket *defineFrame(HttpQueue *q, HttpPacket *packet, int type, uchar flags, int stream)
{
    HttpNet     *net;
    MprBuf      *buf;
    ssize       length;
    cchar       *typeStr;

    net = q->net;
    if (!packet) {
        packet = httpCreatePacket(0);
    }
    packet->type = type;
    if ((buf = packet->prefix) == 0) {
        buf = packet->prefix = mprCreateBuf(HTTP2_FRAME_OVERHEAD, HTTP2_FRAME_OVERHEAD);
    }
    length = httpGetPacketLength(packet);

    /*
        Not yet supporting priority or weight
     */
    mprPutUint32ToBuf(buf, (((uint32) length) << 8 | type));
    mprPutCharToBuf(buf, flags);
    mprPutUint32ToBuf(buf, stream);

    typeStr = (type < HTTP2_MAX_FRAME) ? packetTypes[type] : "unknown";
    if (httpTracing(net) && !net->skipTrace) {
        if (net->bytesWritten >= net->trace->maxContent) {
            httpTrace(net->trace, "http2.tx", "packet", "msg: 'Abbreviating packet trace'");
            net->skipTrace = 1;
        } else {
            httpTracePacket(net->trace, "http2.tx", "packet", HTTP_TRACE_HEX, packet,
                "frame=%s, flags=%x, stream=%d, length=%zd,", typeStr, flags, stream, length);
        }
    } else {
        httpTrace(net->trace, "http2.tx", "packet", "frame=%s, flags=%x, stream=%d, length=%zd,", typeStr, flags, stream, length);
    }
    return packet;
}


/*
    Send a HTTP/2 packet downstream to the network
 */
static void sendFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;

    net = q->net;
    if (packet && !net->goaway && !net->eof && !net->error) {
        httpPutPacket(q->net->socketq, packet);
    }
}


/*
    Find a HttpStream stream using the HTTP/2 stream ID
 */
static HttpStream *findStreamObj(HttpNet *net, int streamID)
{
    HttpStream    *stream;
    int         next;

    for (ITERATE_ITEMS(net->streams, stream, next)) {
        if (stream->streamID == streamID) {
            return stream;
        }
    }
    return 0;
}


/*
    Garbage collector callback.
 */
static void manageFrame(HttpFrame *frame, int flags)
{
    assert(frame);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(frame->stream);
    }
}


#endif /* ME_HTTP_HTTP2 */
/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
