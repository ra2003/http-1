/*
    http2Filter.c - HTTP/2 protocol handling.

    HTTP/2 Protocol state machine for server-side requests and client responses.
    Process an incoming request and drive the state machine. This will process only one request.
    All socket I/O is non-blocking, and this routine must not block. Note: packet may be null.
    Return true if the request is completed successfully.

    For historical reasons, the HttpConn object is used to implement HTTP2 streams and HttpNet is
    used to implement HTTP2 network connections.

    httpProcess is logically part of the http* stage of processing and thus part of this filter.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if ME_HTTP_HTTP2
/********************************** Locals ************************************/

#define httpGetPrefixMask(bits) ((1 << (bits)) - 1)
#define httpSetPrefix(bits)     (1 << (bits))

#define STREAM_MASK             0x7fffffff

typedef void (*FrameHandler)(HttpQueue *q, HttpPacket *packet);

/************************************ Forwards ********************************/

static void addHeader(HttpConn *conn, cchar *key, cchar *value);
static void checkSettings(HttpQueue *q);
static void closeWhenDone(HttpQueue *q);
static int decodeInt(HttpPacket *packet, uint prefix);
static HttpPacket *defineFrame(HttpQueue *q, HttpPacket *packet, int type, uchar flags, int stream);
static void definePseudoHeaders(HttpConn *conn, HttpPacket *packet);
static void encodeHeader(HttpConn *conn, HttpPacket *packet, cchar *key, cchar *value);
static void encodeInt(HttpPacket *packet, uint prefix, uint bits, uint value);
static void encodeString(HttpPacket *packet, cchar *src, uint lower);
static HttpConn *findStream(HttpNet *net, int stream);
static ssize flowControlPacket(HttpQueue *q, ssize max, HttpPacket *packet, bool *done);
static HttpConn *getConn(HttpQueue *q, HttpPacket *packet);
static int getPacketFlags(HttpQueue *q, HttpPacket *packet);
static void incomingHttp2(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp2(HttpQueue *q, HttpPacket *packet);
static void outgoingHttp2Service(HttpQueue *q);
static void manageFrame(HttpFrame *frame, int flags);
static void parseDataFrame(HttpQueue *q, HttpPacket *packet);
static HttpFrame *parseFrame(HttpQueue *q, HttpPacket *packet);
static cchar *parseField(HttpQueue *q, HttpConn *conn, HttpPacket *packet);
static void parseGoAwayFrame(HttpQueue *q, HttpPacket *packet);
static void parseHeaderFrame(HttpQueue *q, HttpPacket *packet);
static void parseHeader(HttpQueue *q, HttpConn *conn, HttpPacket *packet);
static void parseHeaderFrames(HttpQueue *q, HttpConn *conn);
static void parsePriorityFrame(HttpQueue *q, HttpPacket *packet);
static void parsePushFrame(HttpQueue *q, HttpPacket *packet);
static void parsePingFrame(HttpQueue *q, HttpPacket *packet);
static void parseResetFrame(HttpQueue *q, HttpPacket *packet);
static void parseSettingsFrame(HttpQueue *q, HttpPacket *packet);
static void parseWindowFrame(HttpQueue *q, HttpPacket *packet);
static void processDataFrame(HttpQueue *q, HttpPacket *packet);
static void resetConn(HttpConn *conn, cchar *msg);
static void sendFrame(HttpQueue *q, HttpPacket *packet);
static void sendGoAway(HttpQueue *q, int status, cchar *fmt, ...);
static void sendPreface(HttpQueue *q);
static void sendReset(HttpQueue *q, HttpConn *conn, int status, cchar *fmt, ...);
static void sendSettings(HttpQueue *q);
static void sendWindowFrame(HttpQueue *q, int stream, ssize size);
static bool validateHeader(cchar *key, cchar *value);

/*
    Order matters
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

static char *packetTypes[] = {
    "data",
    "headers",
    "priority",
    "reset",
    "settings",
    "push",
    "ping",
    "goaway",
    "window",
    "continue",
};

/*********************************** Code *************************************/
/*
    Loadable module initialization
 */
PUBLIC int httpOpenHttp2Filter()
{
    HttpStage     *filter;

    if ((filter = httpCreateConnector("Http2Filter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->http2Filter = filter;
    filter->incoming = incomingHttp2;
    filter->outgoing = outgoingHttp2;
    filter->outgoingService = outgoingHttp2Service;
    httpCreatePackedHeaders();
    return 0;
}


static void incomingHttp2(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpFrame   *frame;

    net = q->net;

    httpJoinPacketForService(q, packet, HTTP_DELAY_SERVICE);
    checkSettings(q);

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if ((frame = parseFrame(q, packet)) != 0) {
            if (net->goaway && conn && (net->lastStream && conn->stream >= net->lastStream)) {
                /* Network is being closed. Continue to process existing streams but accept no new streams */
                continue;
            }
            net->frame = frame;
            frameHandlers[frame->type](q, packet);
            net->frame = 0;
            conn = frame->conn;
            if (conn && conn->disconnect && !conn->destroyed) {
                sendReset(q, conn, HTTP2_INTERNAL_ERROR, "Stream request error %s", conn->errorMsg);
            }
            mprYield(0);

        } else {
            break;
        }

    }
    closeWhenDone(q);
}


static void outgoingHttp2(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    int         enable;

    net = q->net;
    conn = packet->conn;

    checkSettings(q);

    //  MOB - is this needed now?
    enable = !(q->stage->flags & HTTP_STAGE_HANDLER) || (q->conn->state >= HTTP_STATE_READY) ? 1 : 0;

    if (packet->flags & HTTP_PACKET_HEADER) {
        if (conn->seenHeader) {
            packet->type = HTTP2_CONT_FRAME;
            conn->seenHeader = 1;
        } else {
            packet->type = HTTP2_HEADERS_FRAME;
        }
    } else if (packet->flags & HTTP_PACKET_DATA) {
        packet->type = HTTP2_DATA_FRAME;
    }
    httpPutForService(q, packet, enable);
}


static void outgoingHttp2Service(HttpQueue *q)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpPacket  *packet;
    HttpTx      *tx;
    ssize       len;
    bool        done;

    net = q->net;
    done = 0;

    for (packet = httpGetPacket(q); packet && !net->error && !done; packet = httpGetPacket(q)) {
        net->lastActivity = net->http->now;
        if (net->outputq->max <= 0) {
            httpSuspendQueue(q);
            break;
        }
        len = httpGetPacketLength(packet);
        if (packet->flags & HTTP_PACKET_DATA) {
            len = flowControlPacket(net->outputq, net->outputq->max, packet, &done);
            net->outputq->max -= len;
        }
        conn = packet->conn;
        //  MOB Refactor and simplify
        if (conn && !conn->destroyed) {
            if (conn->streamReset) {
                /* Must not send any more frames on this stream */
                continue;
            }
            if (net->goaway && (net->lastStream && conn->stream >= net->lastStream)) {
                /* Network is being closed. Continue to process existing streams but accept no new streams */
                continue;
            }
            if (conn->disconnect) {
                sendReset(q, conn, HTTP2_INTERNAL_ERROR, "Stream request error %s", conn->errorMsg);
                continue;
            }
            conn->lastActivity = conn->http->now;
            tx = conn->tx;

            if (packet->flags & HTTP_PACKET_DATA) {
                len = flowControlPacket(net->outputq, conn->outputq->max, packet, &done);
                conn->outputq->max -= len;
                if (conn->outputq->max < 0) {
                    sendReset(q, conn, HTTP2_FLOW_CONTROL_ERROR, "Internal flow control error");
                    return;
                }
            }
            sendFrame(q, defineFrame(q, packet, packet->type, getPacketFlags(q, packet), conn->stream));

            if (q->count <= q->low && (conn->outputq->flags & HTTP_QUEUE_SUSPENDED)) {
                httpResumeQueue(conn->outputq);
            }
        }
    }
    closeWhenDone(q);
}


static int getPacketFlags(HttpQueue *q, HttpPacket *packet)
{
    HttpPacket  *first;
    HttpConn    *conn;
    HttpTx      *tx;
    int         flags;

    conn = packet->conn;
    tx = conn->tx;
    flags = 0;
    first = q->first;

    if (packet->flags & HTTP_PACKET_HEADER) {
        if (!(first && first->flags & HTTP_PACKET_HEADER)) {
            flags |= HTTP2_END_HEADERS_FLAG;
        }
    } else {
        if (!tx->streamEnded) {
            if (packet->flags & HTTP_PACKET_END) {
                /*
                    Convert the packet end to a data frame to signify end of stream
                 */
                packet->type = HTTP2_DATA_FRAME;
                tx->streamEnded = 1;
                flags |= HTTP2_END_STREAM_FLAG;
            } else {
                if (first && (first->flags & HTTP_PACKET_END)) {
                    tx->streamEnded = 1;
                    flags |= HTTP2_END_STREAM_FLAG;
                }
            }
        }
    }
    return flags;
}


static ssize flowControlPacket(HttpQueue *q, ssize max, HttpPacket *packet, bool *done)
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
        *done = 1;
    }
    return len;
}


static void closeWhenDone(HttpQueue *q)
{
    HttpNet     *net;

    net = q->net;
    if (net->error) {
        if (!net->goaway) {
            sendGoAway(net->socketq, HTTP2_PROTOCOL_ERROR, "Closing network");
        }
    }
    if (net->goaway) {
        if (mprGetListLength(net->connections) == 0) {
            /* This ensures a recall on the netConnector IOEvent handler */
            mprDisconnectSocket(net->sock);
        }
    }
}


/*
    Parse the incoming http message. Return true to keep going with this or subsequent request, zero means
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
        httpPutBackPacket(q, packet);
        return 0;
    }
    lenType = mprPeekUint32FromBuf(buf);
    len = lenType >> 8;
    if (len > q->packetSize || len > HTTP2_MAX_FRAME_SIZE) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad frame size %d vs %d", len, q->packetSize);
        return 0;
    }
    frameLength = len + HTTP2_FRAME_OVERHEAD;
    size = httpGetPacketLength(packet);
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

    if ((frame = mprAllocObj(HttpFrame, manageFrame)) == NULL) {
        /* Memory error - centrally reported */
        return 0;
    }
    packet->data = frame;

    type = lenType & 0xFF;
    frame->type = type;
    frame->flags = mprGetCharFromBuf(buf);
    frame->stream = mprGetUint32FromBuf(buf) & STREAM_MASK;
    frame->conn = findStream(net, frame->stream);

    if (httpTracing(q->net)) {
        typeStr = (type < HTTP2_MAX_FRAME) ? packetTypes[type] : "unknown";
        mprAdjustBufStart(packet->content, -HTTP2_FRAME_OVERHEAD);
        httpTracePacket(q->net->trace, "http2.rx", "packet", HTTP_TRACE_HEX, packet,
            "frame=%s flags=%x stream=%d length=%zd", typeStr, frame->flags, frame->stream, httpGetPacketLength(packet));
        mprAdjustBufStart(packet->content, HTTP2_FRAME_OVERHEAD);
    }
    if (frame->stream && !frame->conn) {
#if KEEP
        if (frame->stream < net->lastStream) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Closed stream being reused");
            return 0;
        }
#endif
        if ((frame->type == HTTP2_DATA_FRAME || frame->type == HTTP2_RESET_FRAME)) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid frame type %d without a stream %d", frame->type, frame->stream);
            return 0;
        }
    }
    if (frame->type < 0 || frame->type >= HTTP2_MAX_FRAME) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid frame type %d", frame->type);
        return 0;
    }
    return frame;
}


static void parseSettingsFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpFrame   *frame;
    MprBuf      *buf;
    uint        field, value;

    net = q->net;
    buf = packet->content;
    frame = packet->data;

    if (frame->flags & HTTP2_ACK_FLAG) {
        return;
    }
    while (httpGetPacketLength(packet) >= HTTP2_SETTINGS_SIZE) {
        field = mprGetUint16FromBuf(buf);
        value = mprGetUint32FromBuf(buf);

        switch (field) {
        case HTTP2_HEADER_TABLE_SIZE_SETTING:
            httpSetPackedHeadersMax(net->txHeaders, value);
            break;

        case HTTP2_ENABLE_PUSH_SETTING:
            if (value != 0 && value != 1) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid push value");
                return;
            }
            //  Push is not yet supported
            net->push = value;
            break;

        case HTTP2_MAX_STREAMS_SETTING:
            net->limits->streamsMax = min(value, net->limits->streamsMax);
            break;

        case HTTP2_INIT_WINDOW_SIZE_SETTING:
            if (value > HTTP2_MAX_WINDOW) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid window size setting %x max %x", value, HTTP2_MAX_WINDOW);
                return;
            }
            httpSetQueueLimits(net->outputq, -1, -1, value);
            break;

        case HTTP2_MAX_FRAME_SIZE_SETTING:
            if (value > 0 && value < net->outputq->packetSize) {
                net->outputq->packetSize = value;
                #if MOB && TBD
                httpResizePackets(net->outputq);
                #endif
            }
            break;

        case HTTP2_MAX_HEADER_SIZE_SETTING:
            if (value < net->limits->headerSize) {
                net->limits->headerSize = value;
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
    sendFrame(q, defineFrame(q, packet, HTTP2_SETTINGS_FRAME, HTTP2_ACK_FLAG, 0));
}


/*
    Parse header or continuation frames
 */
static void parseHeaderFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpFrame   *frame;
    MprBuf      *buf;
    bool        padded, priority;
    ssize       size, frameLen;
    int         depend, dword, excl, weight, padLen;

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
        /* dependency + weight */
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
    depend = 0;
    weight = HTTP2_DEFAULT_WEIGHT;
    if (priority) {
        dword = mprGetUint32FromBuf(buf);
        depend = dword & 0x7fffffff;
        excl = dword >> 31;
        weight = mprGetCharFromBuf(buf) + 1;
        //  FUTURE - not yet implemented
    }
    if ((frame->stream % 2) != 1 || (net->lastStream && frame->stream <= net->lastStream)) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad sesssion");
        return;
    }
    if ((conn = getConn(q, packet)) != 0) {
         if (frame->flags & HTTP2_END_HEADERS_FLAG) {
            parseHeaderFrames(q, conn);
        }
        /*
            Must only update for a successfully received frame
         */
        if (!net->error && frame->type == HTTP2_HEADERS_FRAME) {
            net->lastStream = frame->stream;
        }
    }
}


/*
    Get / create the connection
 */
static HttpConn *getConn(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpRx      *rx;
    HttpFrame   *frame;

    net = q->net;
    frame = packet->data;
    conn = frame->conn;
    frame = packet->data;
    assert(frame->stream);

    if (!conn && httpIsServer(net)) {
        if ((conn = httpCreateConn(net)) == 0) {
            /* Memory error - centrally reported */
            return 0;
        }
        /*
            Servers create a new connection stream. Note: HttpConn is used for HTTP/2 streams (legacy).
         */
        if (mprGetListLength(net->connections) >= net->limits->requestsPerClientMax) {
            sendReset(q, conn, HTTP2_REFUSED_STREAM, "Too many streams: %s %d/%d", net->ip,
                (int) mprGetListLength(net->connections), net->limits->requestsPerClientMax);
            return 0;
        }
        ///MOB httpMonitorEvent(conn, HTTP_COUNTER_REQUESTS, 1);
        if (mprGetListLength(net->connections) >= net->limits->streamsMax) {
            sendReset(q, conn, HTTP2_REFUSED_STREAM, "Too many streams: %s %d/%d", net->ip,
                (int) mprGetListLength(net->connections), net->limits->streamsMax);
            return 0;
        }
        frame->conn = conn;
        conn->stream = frame->stream;
    }
    if (net->goaway) {
        /* Ignore new streams as the network is going away */
        sendReset(q, conn, HTTP2_REFUSED_STREAM, "Network is going away");
        return 0;
    }
#if FUTURE
    if (depend == frame->stream) {
        sendReset(q, conn, HTTP2_PROTOCOL_ERROR, "Bad stream dependency");
        return 0;
    }
#endif
    if (frame->type == HTTP2_CONT_FRAME && (!conn->rx || !conn->rx->headerPacket)) {
        if (!frame->conn) {
            sendReset(q, conn, HTTP2_REFUSED_STREAM, "Invalid continuation frame");
            return 0;
        }
    }
    rx = conn->rx;
    if (frame->flags & HTTP2_END_STREAM_FLAG) {
        rx->eof = 1;
    }
    if (rx->headerPacket) {
        httpJoinPacket(rx->headerPacket, packet);
    } else {
        rx->headerPacket = packet;
    }
    packet->conn = conn;

    if (httpGetPacketLength(rx->headerPacket) > conn->limits->headerSize) {
        sendReset(q, conn, HTTP2_REFUSED_STREAM, "Header too big, length %ld, limit %ld",
            httpGetPacketLength(rx->headerPacket), conn->limits->headerSize);
        return 0;
    }
    return conn;
}


static void parsePriorityFrame(HttpQueue *q, HttpPacket *packet)
{
    MprBuf  *buf;
    int     dep, exclusive, weight;

    buf = packet->content;
    dep = mprGetUint32FromBuf(buf);
    exclusive = dep & (1 << 31);
    dep &= (1U << 31) - 1;
    weight = mprGetCharFromBuf(buf);
    /*
        Priority frames are not yet implemented: TODO
     */
}


static void parsePushFrame(HttpQueue *q, HttpPacket *packet)
{
    /*
        Push frames are not yet implemented: TODO
     */
}


static void parsePingFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpFrame   *frame;

    frame = packet->data;
    if (frame->conn) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad stream in ping frame");
        return;
    }
    if (!(frame->flags & HTTP2_ACK_FLAG)) {
        sendFrame(q, defineFrame(q, packet, HTTP2_PING_FRAME, HTTP2_ACK_FLAG, 0));
    }
}


/*
    Close a stream in case of an error
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
    if (!frame->conn) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad stream in reset frame");
        return;
    }
    error = mprGetUint32FromBuf(packet->content) & STREAM_MASK;
    frame->conn->streamReset = 1;
    resetConn(frame->conn, "Stream reset by peer");
}


/*
    Receive a GoAway which informs us that this network should not be used anymore.
 */
static void parseGoAwayFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    MprBuf      *buf;
    cchar       *msg;
    ssize       len;
    int         error, lastStream, next;

    buf = packet->content;
    lastStream = mprGetUint32FromBuf(buf) & STREAM_MASK;
    error = mprGetUint32FromBuf(buf);
    len = mprGetBufLength(buf);
    msg = len ? snclone(buf->start, len) : "";
    httpTrace(q->net->trace, "http2.rx", "context", "msg='Receive GoAway. %s' error=%d lastStream=%d", msg, error, lastStream);

    for (ITERATE_ITEMS(q->net->connections, conn, next)) {
        if (conn->stream > lastStream) {
            resetConn(conn, "Stream reset by peer");
        }
    }
    q->net->goaway = 1;
}


/*
    The window frame increases the window size of permissible data to send.
 */
static void parseWindowFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpConn    *conn;
    HttpFrame   *frame;
    int         increment;

    net = q->net;
    frame = packet->data;
    increment = mprGetUint32FromBuf(packet->content);
    if (frame->stream) {
        if ((conn = frame->conn) != 0) {
            if (increment > (HTTP2_MAX_WINDOW - conn->outputq->max)) {
                sendReset(q, conn, HTTP2_FLOW_CONTROL_ERROR, "Invalid window update for stream %d", conn->stream);
            } else {
                conn->outputq->max += increment;
                httpResumeQueue(conn->outputq);
            }
        }
    } else {
        if (increment > (HTTP2_MAX_WINDOW + 1 - net->outputq->max)) {
            sendGoAway(q, HTTP2_FLOW_CONTROL_ERROR, "Invalid window update for network");
        } else {
            net->outputq->max += increment;
            httpResumeQueue(net->outputq);
        }
    }
}


static void parseHeaderFrames(HttpQueue *q, HttpConn *conn)
{
    HttpPacket  *packet;
    HttpRx      *rx;

    rx = conn->rx;
    packet = rx->headerPacket;
    while (httpGetPacketLength(packet) > 0 && !conn->error && !conn->net->error) {
        parseHeader(q, conn, packet);
    }
    if (!q->net->goaway) {
        if (!conn->error) {
            conn->state = HTTP_STATE_PARSED;
        }
        httpProcess(conn->inputq);
    }
}


static void parseHeader(HttpQueue *q, HttpConn *conn, HttpPacket *packet)
{
    HttpNet     *net;
    MprBuf      *buf;
    MprKeyValue *kp;
    cchar       *name, *value;
    uchar       ch;
    int         index, max;

    net = conn->net;
    buf = packet->content;

    ch = mprLookAtNextCharInBuf(buf);
    if ((ch >> 7) == 1) {
        /*
            Fully indexed header field
         */
        index = decodeInt(packet, 7);
        if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return;
        }
        addHeader(conn, kp->key, kp->value);

    } else if ((ch >> 6) == 1) {
        /*
            Literal header and add to index
         */
        if ((index = decodeInt(packet, 6)) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return;
        } else if (index > 0) {
            if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Unknown header index");
                return;
            }
            name = kp->key;
        } else {
            name = parseField(q, conn, packet);
        }
        value = parseField(q, conn, packet);
        if (!name || !value) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header name/value");
            return;
        }
        addHeader(conn, name, value);
        httpAddPackedHeader(net->rxHeaders, name, value);

    } else if ((ch >> 5) == 1) {
        /* Dynamic table max size update */
        max = decodeInt(packet, 5);
        if (httpSetPackedHeadersMax(net->rxHeaders, max) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Cannot add indexed header");
            return;
        }

    } else /* if ((ch >> 4) == 1 || (ch >> 4) == 0)) */ {
        /* Literal header field without indexing */
        if ((index = decodeInt(packet, 4)) < 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Bad header prefix");
            return;
        } else if (index > 0) {
            if ((kp = httpGetPackedHeader(net->rxHeaders, index)) == 0) {
                sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Unknown header index");
                return;
            }
            name = kp->key;
        } else {
            name = parseField(q, conn, packet);
        }
        value = parseField(q, conn, packet);
        if (!name || !value) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header name/value");
            return;
        }
        addHeader(conn, name, value);
    }
}


static void addHeader(HttpConn *conn, cchar *key, cchar *value)
{
    HttpRx      *rx;
    HttpLimits  *limits;
    ssize       len;

    rx = conn->rx;
    limits = conn->limits;

    if (!validateHeader(key, value)) {
        return;
    }
    if (key[0] == ':') {
        if (key[1] == 'a' && smatch(key, ":authority")) {
            mprAddKey(conn->rx->headers, "host", value);

        } else if (key[1] == 'm' && smatch(key, ":method")) {
            rx->originalMethod = rx->method = supper(value);
            httpParseMethod(conn);

        } else if (key[1] == 'p' && smatch(key, ":path")) {
            len = slen(value);
            if (*value == '\0') {
                httpBadRequestError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad HTTP request. Empty URI");
                return;
            } else if (len >= limits->uriSize) {
                httpLimitError(conn, HTTP_ABORT | HTTP_CODE_REQUEST_URL_TOO_LARGE,
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
        mprAddKey(conn->rx->headers, key, value);
    }
}


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
            mprLog("info http", 5, "Invalid header name %s", key);
            return 0;
        }
    }
    for (cp = (uchar*) value; *cp; cp++) {
        c = *cp;
        if (c == '\0' || c == '\n' || c == '\r') {
            mprLog("info http", 5, "Invalid header value %s", value);
            return 0;
        }
    }
    return 1;
}


static cchar *parseField(HttpQueue *q, HttpConn *conn, HttpPacket *packet)
{
    HttpNet     *net;
    HttpFrame   *frame;
    MprBuf      *buf;
    cchar       *value;
    int         huff, len;

    net = conn->net;
    frame = packet->data;
    buf = packet->content;

    huff = ((uchar) mprLookAtNextCharInBuf(buf)) >> 7;
    len = decodeInt(packet, 7);
    if (len < 0 || len > mprGetBufLength(buf)) {
        sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid header field length");
        return 0;
    }
    if (huff) {
        if ((value = httpHuffDecode((uchar*) mprGetBufStart(buf), len)) == 0) {
            sendGoAway(q, HTTP2_PROTOCOL_ERROR, "Invalid encoded header field");
            return 0;
        }
    } else {
        value = snclone(buf->start, len);
    }
    mprAdjustBufStart(buf, len);
    return value;
}


static void parseDataFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpNet     *net;
    HttpFrame   *frame;
    HttpConn    *conn;
    HttpLimits  *limits;
    MprBuf      *buf;
    ssize       len, padLen, frameLen;
    int         padded;

    net = q->net;
    limits = net->limits;
    buf = packet->content;
    frame = packet->data;
    len = httpGetPacketLength(packet);
    conn = frame->conn;
    assert(conn);

    if (conn->streamReset) {
        sendReset(q, conn, HTTP2_STREAM_CLOSED, "Received data on closed stream %d", conn->stream);
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

    /*
        Network flow control
     */
    if (len > net->inputq->max) {
        sendGoAway(q, HTTP2_FLOW_CONTROL_ERROR, "Peer exceeded flow control window");
        return;
    }
    net->inputq->max -= len;
    if (net->inputq->max <= net->inputq->packetSize) {
        sendWindowFrame(q, 0, limits->windowSize - net->inputq->max);
        httpSetQueueLimits(net->inputq, -1, -1, limits->windowSize);
    }

    /*
        Stream flow control
     */
    if (len > conn->inputq->max) {
        sendReset(q, conn, HTTP2_FLOW_CONTROL_ERROR, "Receive data exceeds window for stream");
        return;
    }
    conn->inputq->max -= len;
    if (conn->inputq->max <= net->inputq->packetSize) {
        sendWindowFrame(q, conn->stream, limits->windowSize - conn->inputq->max);
        httpSetQueueLimits(conn->inputq, -1, -1, limits->windowSize);
    }
    processDataFrame(q, packet);
}


static void processDataFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpFrame   *frame;
    HttpConn    *conn;

    frame = packet->data;
    conn = frame->conn;

    if (frame->flags & HTTP2_END_STREAM_FLAG) {
        conn->rx->eof = 1;
    }
    httpPutPacket(conn->inputq, packet);
    httpProcess(conn->inputq);
}


/*
    Shutdown a network. This is not necessarily an error. Peer should open a new network.
    Continue processing current streams, but stop processing any new streams.
 */
static void sendGoAway(HttpQueue *q, int status, cchar *fmt, ...)
{
    HttpNet     *net;
    HttpPacket  *packet;
    HttpConn    *conn;
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
    msg = sfmtv(fmt, ap);
    mprLog("info http2", 3, "Send network goAway, lastStream=%d, status=%d, msg='%s'", net->lastStream, status, msg);
    va_end(ap);

    buf = packet->content;
    mprPutUint32ToBuf(buf, status);
    mprPutUint32ToBuf(buf, net->lastStream);
    mprPutStringToBuf(buf, msg);
    sendFrame(q, defineFrame(q, packet, HTTP2_GOAWAY_FRAME, 0, 0));

    for (ITERATE_ITEMS(q->net->connections, conn, next)) {
        if (conn->stream > net->lastStream) {
            resetConn(conn, "Stream terminated");
        }
    }
    net->goaway = 1;
}


PUBLIC void httpSendGoAway(HttpNet *net, int status, cchar *fmt, ...)
{
    va_list     ap;
    cchar       *msg;

    va_start(ap, fmt);
    msg = sfmtv(fmt, ap);
    va_end(ap);
    sendGoAway(net->outputq, status, "%s", msg);
}


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
static void sendReset(HttpQueue *q, HttpConn *conn, int status, cchar *fmt, ...)
{
    HttpPacket  *packet;
    va_list     ap;
    char        *msg;

    assert(conn);

    if (conn->streamReset || conn->destroyed) {
        return;
    }
    if ((packet = httpCreatePacket(HTTP2_RESET_SIZE)) == 0) {
        return;
    }
    va_start(ap, fmt);
    msg = sfmtv(fmt, ap);
    va_end(ap);

    mprPutUint32ToBuf(packet->content, status);
    mprLog("info http2", 3, "Send stream reset, stream=%d, status=%d, msg='%s'", conn->stream, status, msg);
    sendFrame(q, defineFrame(q, packet, HTTP2_RESET_FRAME, 0, conn->stream));

    conn->streamReset = 1;
    resetConn(conn, msg);
}


static void resetConn(HttpConn *conn, cchar *msg)
{
    httpError(conn, HTTP_CODE_COMMS_ERROR, "%s", msg);
    httpProcess(conn->inputq);
}


static void checkSettings(HttpQueue *q)
{
    HttpNet     *net;

    net = q->net;

    if (!net->init) {
        sendSettings(q);
        net->init = 1;
    }
}


static void sendPreface(HttpQueue *q)
{
    HttpNet     *net;
    HttpPacket  *packet;

    net = q->net;
    if ((packet = httpCreatePacket(HTTP2_PREFACE_SIZE)) == 0) {
        return;
    }
    packet->flags = 0;
    mprPutBlockToBuf(packet->content, HTTP2_PREFACE, HTTP2_PREFACE_SIZE);
    httpPutPacket(q->net->socketq, packet);
}


static void sendSettings(HttpQueue *q)
{
    HttpNet     *net;
    HttpPacket  *packet;

    net = q->net;
    if (!net->init && httpIsClient(net)) {
        sendPreface(q);
    }
    //  MOB - set to the number of settings
    if ((packet = httpCreatePacket(HTTP2_SETTINGS_SIZE * 3)) == 0) {
        return;
    }
#if MOB /* Reenable */
    mprPutUint16ToBuf(packet->content, HTTP2_HEADER_TABLE_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, HTTP2_TABLE_SIZE);
#if MOB
    mprPutUint16ToBuf(packet->content, HTTP2_MAX_HEADER_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, (uint32) net->limits->headerSize);
#endif
#endif

#if MOB
    mprPutUint16ToBuf(packet->content, HTTP2_ENABLE_PUSH_SETTING);
    mprPutUint32ToBuf(packet->content, 0);
#endif

    //  MOB - configurable
    mprPutUint16ToBuf(packet->content, HTTP2_MAX_STREAMS_SETTING);
    mprPutUint32ToBuf(packet->content, net->limits->streamsMax);

    mprPutUint16ToBuf(packet->content, HTTP2_INIT_WINDOW_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, (uint32) net->inputq->max);

    mprPutUint16ToBuf(packet->content, HTTP2_MAX_FRAME_SIZE_SETTING);
    mprPutUint32ToBuf(packet->content, (uint32) net->inputq->packetSize);

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


PUBLIC void httpCreateHeaders2(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpTx      *tx;
    MprKey      *kp;

    assert(packet->flags == HTTP_PACKET_HEADER);

    conn = packet->conn;
    tx = conn->tx;
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return;
    }
    tx->responded = 1;

    httpDefineHeaders(conn);
    definePseudoHeaders(conn, packet);
    if (httpTracing(q->net)) {
        httpTrace(conn->trace, "http2.tx", "headers", "\n%s", httpTraceHeaders(q, conn->tx->headers));
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
                    encodeHeader(conn, packet, kp->key, kp->data);
                }
            } else if (smatch(kp->key, ":method")){
                if (smatch(kp->data, "GET")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_METHOD_GET);
                } else if (smatch(kp->data, "POST")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_METHOD_POST);
                } else {
                    encodeHeader(conn, packet, kp->key, kp->data);
                }
            } else if (smatch(kp->key, ":path")) {
                if (smatch(kp->data, "/")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_PATH_ROOT);
                } else if (smatch(kp->data, "/index.html")) {
                    encodeInt(packet, httpSetPrefix(7), 7, HTTP2_PATH_INDEX);
                } else {
                    encodeHeader(conn, packet, kp->key, kp->data);
                }
            } else {
                encodeHeader(conn, packet, kp->key, kp->data);
            }
        }
    }
    for (ITERATE_KEYS(tx->headers, kp)) {
        if (kp->key[0] != ':') {
            encodeHeader(conn, packet, kp->key, kp->data);
        }
    }
}


static void definePseudoHeaders(HttpConn *conn, HttpPacket *packet)
{
    HttpUri     *parsedUri;
    HttpTx      *tx;
    Http        *http;
    cchar       *authority, *path;

    http = conn->http;
    tx = conn->tx;

    if (httpServerConn(conn)) {
        httpAddHeaderString(conn, ":status", itos(tx->status));

    } else {
        authority = conn->rx->hostHeader ? conn->rx->hostHeader : conn->ip;
        httpAddHeaderString(conn, ":method", tx->method);
        httpAddHeaderString(conn, ":scheme", conn->secure ? "https" : "http");
        httpAddHeaderString(conn, ":authority", authority);

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
        httpAddHeaderString(conn, ":path", path);
    }
}


static void encodeHeader(HttpConn *conn, HttpPacket *packet, cchar *key, cchar *value)
{
    HttpNet     *net;
    int         index;
    bool        indexedValue;

    net = conn->net;
    conn->tx->headerSize = 0;

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
#if KEEP && MOB
        //  no indexing
        encodeInt(packet, 0, 4, 0);
        encodeString(packet, key, 1);
        encodeString(packet, value, 0);
#endif
    }
}


static int decodeInt(HttpPacket *packet, uint bits)
{
    MprBuf      *buf;
    uchar       *bp, *end, *start;
    uint        mask, shift, value;
    int         done;

    assert(0 < bits && bits <= 8);
    assert(packet && httpGetPacketLength(packet) > 0);

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


static void encodeInt(HttpPacket *packet, uint flags, uint bits, uint value)
{
    MprBuf      *buf;
    int         mask;

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

    if (httpTracing(net) && !net->skipTrace) {
        if (net->bytesWritten >= net->trace->maxContent) {
            httpTrace(net->trace, "tx.body.data", "packet", "msg: 'Abbreviating packet trace'");
            net->skipTrace = 1;
        } else {
            typeStr = (type < HTTP2_MAX_FRAME) ? packetTypes[type] : "unknown";
            httpTracePacket(net->trace, "http2.tx", "packet", HTTP_TRACE_HEX, packet,
                "frame=%s, flags=%x, stream=%d, length=%zd,", typeStr, flags, stream, length);
        }
    } else {
        httpTrace(net->trace, "http2.tx", "packet", "frame=%s, flags=%x, stream=%d, length=%zd,", typeStr, flags, stream, length);
    }
    return packet;
}


static void sendFrame(HttpQueue *q, HttpPacket *packet)
{
    httpPutPacket(q->net->socketq, packet);
}


static HttpConn *findStream(HttpNet *net, int stream)
{
    HttpConn    *conn;
    int         next;

    for (ITERATE_ITEMS(net->connections, conn, next)) {
        if (conn->stream == stream) {
            assert(conn->ejs == 0);
            return conn;
        }
    }
    return 0;
}


static void manageFrame(HttpFrame *frame, int flags)
{
    assert(frame);

    if (flags & MPR_MANAGE_MARK) {
        mprMark(frame->conn);
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
