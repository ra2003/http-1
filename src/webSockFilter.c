/*
    webSockFilter.c - WebSockets filter support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

#if ME_HTTP_WEB_SOCKETS
/********************************** Locals ************************************/
/*
    Message frame states
 */
#define WS_BEGIN       0
#define WS_EXT_DATA    1
#define WS_MSG         2
#define WS_CLOSED      3

static char *codetxt[16] = {
    "cont", "text", "binary", "reserved", "reserved", "reserved", "reserved", "reserved",
    "close", "ping", "pong", "reserved", "reserved", "reserved", "reserved", "reserved",
};

/*
    Frame format

     Byte 0          Byte 1          Byte 2          Byte 3
     0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
    +-+-+-+-+-------+-+-------------+-------------------------------+
    |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
    |I|S|S|S|  (4)  |A|     (7)     |             (16/63)           |
    |N|V|V|V|       |S|             |   (if payload len==126/127)   |
    | |1|2|3|       |K|             |                               |
    +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
    |     Extended payload length continued, if payload len == 127  |
    + - - - - - - - - - - - - - - - +-------------------------------+
    |                               |Masking-key, if MASK set to 1  |
    +-------------------------------+-------------------------------+
    | Masking-key (continued)       |          Payload Data         |
    +-------------------------------- - - - - - - - - - - - - - - - +
    :                     Payload Data continued ...                :
    + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
    |                     Payload Data continued ...                |
    +---------------------------------------------------------------+

    Single message has
        fin == 1
    Fragmented message has
        fin == 0, opcode != 0
        fin == 0, opcode == 0
        fin == 1, opcode == 0

    Common first byte codes:
        0x9B    Fin | /SET

    NOTE: control frames (opcode >= 8) can be sent between fragmented frames
 */
#define GET_FIN(v)              (((v) >> 7) & 0x1)          /* Final fragment */
#define GET_RSV(v)              (((v) >> 4) & 0x7)          /* Reserved (used for extensions) */
#define GET_CODE(v)             ((v) & 0xf)                 /* Packet opcode */
#define GET_MASK(v)             (((v) >> 7) & 0x1)          /* True if dataMask in frame (client send) */
#define GET_LEN(v)              ((v) & 0x7f)                /* Low order 7 bits of length */

#define SET_FIN(v)              (((v) & 0x1) << 7)
#define SET_MASK(v)             (((v) & 0x1) << 7)
#define SET_CODE(v)             ((v) & 0xf)
#define SET_LEN(len, n)         ((uchar)(((len) >> ((n) * 8)) & 0xff))

/*
    Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>
    See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.
 */
#define UTF8_ACCEPT 0
#define UTF8_REJECT 1

static const uchar utfTable[] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 00..1f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 20..3f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 40..5f
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // 60..7f
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, // 80..9f
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, // a0..bf
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2, // c0..df
    0xa,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x3,0x4,0x3,0x3, // e0..ef
    0xb,0x6,0x6,0x6,0x5,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8,0x8, // f0..ff
    0x0,0x1,0x2,0x3,0x5,0x8,0x7,0x1,0x1,0x1,0x4,0x6,0x1,0x1,0x1,0x1, // s0..s0
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,1,0,1,0,1,1,1,1,1,1, // s1..s2
    1,2,1,1,1,1,1,2,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1, // s3..s4
    1,2,1,1,1,1,1,1,1,2,1,1,1,1,1,1,1,1,1,1,1,1,1,3,1,3,1,1,1,1,1,1, // s5..s6
    1,3,1,1,1,1,1,3,1,3,1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1, // s7..s8
};

/********************************** Forwards **********************************/

static void closeWebSock(HttpQueue *q);
static void incomingWebSockData(HttpQueue *q, HttpPacket *packet);
static void manageWebSocket(HttpWebSocket *ws, int flags);
static int matchWebSock(HttpStream *stream, HttpRoute *route, int dir);
static int openWebSock(HttpQueue *q);
static void outgoingWebSockService(HttpQueue *q);
static int processFrame(HttpQueue *q, HttpPacket *packet);
static void readyWebSock(HttpQueue *q);
static int validUTF8(HttpStream *stream, cchar *str, ssize len);
static bool validateText(HttpStream *stream, HttpPacket *packet);
static void webSockPing(HttpStream *stream);
static void webSockTimeout(HttpStream *stream);

static void traceErrorProc(HttpStream *stream, cchar *fmt, ...);

#define traceError(stream, ...) \
    if (stream->http->traceLevel > 0 && PTOI(mprLookupKey(stream->trace->events, "error")) <= stream->http->traceLevel) { \
        traceErrorProc(stream, __VA_ARGS__); \
    } else

/*********************************** Code *************************************/
/*
   WebSocket Filter initialization
 */
PUBLIC int httpOpenWebSockFilter()
{
    HttpStage     *filter;

    if ((filter = httpCreateFilter("webSocketFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    HTTP->webSocketFilter = filter;
    filter->match = matchWebSock;
    filter->open = openWebSock;
    filter->ready = readyWebSock;
    filter->close = closeWebSock;
    filter->outgoingService = outgoingWebSockService;
    filter->incoming = incomingWebSockData;
    return 0;
}


/*
    Match if the filter is required for this request. This is called twice: once for TX and once for RX. RX first.
 */
static int matchWebSock(HttpStream *stream, HttpRoute *route, int dir)
{
    HttpWebSocket   *ws;
    HttpRx          *rx;
    HttpTx          *tx;
    char            *kind, *tok;
    cchar           *key, *protocols;
    int             version;

    assert(stream);
    assert(route);
    rx = stream->rx;
    tx = stream->tx;
    assert(rx);
    assert(tx);

    if (stream->error) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (httpClientStream(stream)) {
        if (rx->webSocket) {
            return HTTP_ROUTE_OK;
        } else if (tx->parsedUri && tx->parsedUri->webSockets) {
            /* ws:// URI. Client web sockets */
            if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
                httpMemoryError(stream);
                return HTTP_ROUTE_OMIT_FILTER;
            }
            rx->webSocket = ws;
            ws->state = WS_STATE_CONNECTING;
            return HTTP_ROUTE_OK;
        }
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (dir & HTTP_STAGE_TX) {
        return rx->webSocket ? HTTP_ROUTE_OK : HTTP_ROUTE_OMIT_FILTER;
    }
    if (!rx->upgrade || !scaselessmatch(rx->upgrade, "websocket")) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (!rx->hostHeader || !smatch(rx->method, "GET")) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if (tx->flags & HTTP_TX_HEADERS_CREATED) {
        return HTTP_ROUTE_OMIT_FILTER;
    }
    version = (int) stoi(httpGetHeader(stream, "sec-websocket-version"));
    if (version < WS_VERSION) {
        httpSetHeader(stream, "Sec-WebSocket-Version", "%d", WS_VERSION);
        httpError(stream, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Version");
        return HTTP_ROUTE_OMIT_FILTER;
    }
    if ((key = httpGetHeader(stream, "sec-websocket-key")) == 0) {
        httpError(stream, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocket-Key");
        return HTTP_ROUTE_OMIT_FILTER;
    }
    protocols = httpGetHeader(stream, "sec-websocket-protocol");

    if (dir & HTTP_STAGE_RX) {
        if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
            httpMemoryError(stream);
            return HTTP_ROUTE_OMIT_FILTER;
        }
        rx->webSocket = ws;
        ws->state = WS_STATE_OPEN;
        ws->preserveFrames = (rx->route->flags & HTTP_ROUTE_PRESERVE_FRAMES) ? 1 : 0;

        /* Just select the first protocol */
        if (route->webSocketsProtocol) {
            for (kind = stok(sclone(protocols), " \t,", &tok); kind; kind = stok(NULL, " \t,", &tok)) {
                if (smatch(route->webSocketsProtocol, kind)) {
                    break;
                }
            }
            if (!kind) {
                httpError(stream, HTTP_CLOSE | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Protocol");
                return HTTP_ROUTE_OMIT_FILTER;
            }
            ws->subProtocol = sclone(kind);
        } else {
            /* Just pick the first protocol */
            ws->subProtocol = stok(sclone(protocols), " ,", NULL);
        }
        httpSetStatus(stream, HTTP_CODE_SWITCHING);
        httpSetHeader(stream, "Connection", "Upgrade");
        httpSetHeader(stream, "Upgrade", "WebSocket");
        httpSetHeaderString(stream, "Sec-WebSocket-Accept", mprGetSHABase64(sjoin(key, WS_MAGIC, NULL)));
        if (ws->subProtocol && *ws->subProtocol) {
            httpSetHeaderString(stream, "Sec-WebSocket-Protocol", ws->subProtocol);
        }
#if !ME_HTTP_WEB_SOCKETS_STEALTH
        httpSetHeader(stream, "X-Request-Timeout", "%lld", stream->limits->requestTimeout / TPS);
        httpSetHeader(stream, "X-Inactivity-Timeout", "%lld", stream->limits->inactivityTimeout / TPS);
#endif
        if (route->webSocketsPingPeriod) {
            ws->pingEvent = mprCreateEvent(stream->dispatcher, "webSocket", route->webSocketsPingPeriod,
                webSockPing, stream, MPR_EVENT_CONTINUOUS);
        }
        stream->keepAliveCount = 0;
        stream->upgraded = 1;
        rx->eof = 0;
        rx->remainingContent = HTTP_UNLIMITED;
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_OMIT_FILTER;
}


/*
    Open the filter for a new request
 */
static int openWebSock(HttpQueue *q)
{
    HttpStream      *stream;
    HttpWebSocket   *ws;

    assert(q);
    stream = q->stream;
    ws = stream->rx->webSocket;
    assert(ws);

    q->packetSize = min(stream->limits->packetSize, q->max);
    ws->closeStatus = WS_STATUS_NO_STATUS;
    stream->timeoutCallback = webSockTimeout;

    /*
        Create an empty data packet to force the headers out
     */
    httpPutPacketToNext(q->pair, httpCreateDataPacket(0));
    stream->tx->responded = 0;
    return 0;
}


static void manageWebSocket(HttpWebSocket *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->currentFrame);
        mprMark(ws->currentMessage);
        mprMark(ws->tailMessage);
        mprMark(ws->pingEvent);
        mprMark(ws->subProtocol);
        mprMark(ws->errorMsg);
        mprMark(ws->closeReason);
        mprMark(ws->data);
    }
}


static void closeWebSock(HttpQueue *q)
{
    HttpWebSocket   *ws;

    if (q->stream && q->stream->rx) {
        ws = q->stream->rx->webSocket;
        assert(ws);
        if (ws) {
            ws->state = WS_STATE_CLOSED;
           if (ws->pingEvent) {
                mprRemoveEvent(ws->pingEvent);
                ws->pingEvent = 0;
           }
        }
    }
}


static void readyWebSock(HttpQueue *q)
{
    if (httpServerStream(q->stream)) {
        HTTP_NOTIFY(q->stream, HTTP_EVENT_APP_OPEN, 0);
    }
}


static void incomingWebSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpStream      *stream;
    HttpWebSocket   *ws;
    HttpPacket      *tail;
    HttpLimits      *limits;
    MprBuf          *content;
    char            *fp, *cp;
    ssize           len, currentFrameLen, offset, frameLen;
    int             i, error, mask, lenBytes, opcode;

    assert(packet);
    stream = q->stream;
    assert(stream->rx);
    ws = stream->rx->webSocket;
    assert(ws);
    limits = stream->limits;

    if (packet->flags & HTTP_PACKET_DATA) {
        /*
            The service queue is used to hold data that is yet to be analyzed.
            The ws->currentFrame holds the current frame that is being read from the service queue.
         */
        httpJoinPacketForService(q, packet, 0);
    }
    httpLogPacket(stream->trace, "request.websockets.data", "packet", 0, packet, "state:%d, frame:%d, length:%zu",
        ws->state, ws->frameState, httpGetPacketLength(packet));

    if (packet->flags & HTTP_PACKET_END) {
        /*
            EOF packet means the socket has been abortively closed
         */
        if (ws->state != WS_STATE_CLOSED) {
            ws->closing = 1;
            ws->frameState = WS_CLOSED;
            ws->state = WS_STATE_CLOSED;
            ws->closeStatus = WS_STATUS_COMMS_ERROR;
            HTTP_NOTIFY(stream, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
            httpError(stream, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
        }
    }
    while ((packet = httpGetPacket(q)) != 0) {
        content = packet->content;
        error = 0;
        switch (ws->frameState) {
        case WS_CLOSED:
            if (httpGetPacketLength(packet) > 0) {
                traceError(stream, "Closed, ignore incoming packet");
            }
            httpFinalize(stream);
            httpSetState(stream, HTTP_STATE_FINALIZED);
            break;

        case WS_BEGIN:
            if (httpGetPacketLength(packet) < 2) {
                /* Need more data */
                httpPutBackPacket(q, packet);
                return;
            }
            fp = content->start;
            if (GET_RSV(*fp) != 0) {
                error = WS_STATUS_PROTOCOL_ERROR;
                traceError(stream, "Protocol error, bad reserved field");
                break;
            }
            packet->last = GET_FIN(*fp);
            opcode = GET_CODE(*fp);
            if (opcode == WS_MSG_CONT) {
                if (!ws->currentMessageType) {
                    traceError(stream, "Protocol error, continuation frame but not prior message");
                    error = WS_STATUS_PROTOCOL_ERROR;
                    break;
                }
            } else if (opcode < WS_MSG_CONTROL && ws->currentMessageType) {
                traceError(stream, "Protocol error, data frame received but expected a continuation frame");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            if (opcode > WS_MSG_PONG) {
                traceError(stream, "Protocol error, bad frame opcode");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            packet->type = opcode;
            if (opcode >= WS_MSG_CONTROL && !packet->last) {
                /* Control frame, must not be fragmented */
                traceError(stream, "Protocol error, fragmented control frame");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            fp++;
            len = GET_LEN(*fp);
            mask = GET_MASK(*fp);
            lenBytes = 1;
            if (len == 126) {
                lenBytes += 2;
                len = 0;
            } else if (len == 127) {
                lenBytes += 8;
                len = 0;
            }
            if (httpGetPacketLength(packet) < (lenBytes + 1 + (mask * 4))) {
                /* Return if we don't have the required packet control fields */
                httpPutBackPacket(q, packet);
                return;
            }
            fp++;
            while (--lenBytes > 0) {
                len <<= 8;
                len += (uchar) *fp++;
            }
            if (packet->type >= WS_MSG_CONTROL && len > WS_MAX_CONTROL) {
                /* Too big */
                traceError(stream, "Protocol error, control frame too big");
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            ws->frameLength = len;
            ws->frameState = WS_MSG;
            ws->maskOffset = mask ? 0 : -1;
            if (mask) {
                for (i = 0; i < 4; i++) {
                    ws->dataMask[i] = *fp++;
                }
            }
            assert(content);
            assert(fp >= content->start);
            mprAdjustBufStart(content, fp - content->start);
            assert(q->count >= 0);
            /*
                Put packet onto the service queue
             */
            httpPutBackPacket(q, packet);
            ws->frameState = WS_MSG;
            break;

        case WS_MSG:
            currentFrameLen = httpGetPacketLength(ws->currentFrame);
            len = httpGetPacketLength(packet);
            if ((currentFrameLen + len) > ws->frameLength) {
                /*
                    Split packet if it contains data for the next frame. Do this even if this frame has no data.
                 */
                offset = ws->frameLength - currentFrameLen;
                if ((tail = httpSplitPacket(packet, offset)) != 0) {
                    content = packet->content;
                    httpPutBackPacket(q, tail);
                    len = httpGetPacketLength(packet);
                }
            }
            if ((currentFrameLen + len) > stream->limits->webSocketsMessageSize) {
                if (httpServerStream(stream)) {
                    httpMonitorEvent(stream, HTTP_COUNTER_LIMIT_ERRORS, 1);
                }
                traceError(stream, "Incoming message is too large, length %zd, max %zd", len, limits->webSocketsMessageSize);
                error = WS_STATUS_MESSAGE_TOO_LARGE;
                break;
            }
            if (ws->maskOffset >= 0) {
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ ws->dataMask[ws->maskOffset++ & 0x3];
                }
            }
            if (packet->type == WS_MSG_CONT && ws->currentFrame) {
                httpJoinPacket(ws->currentFrame, packet);
                packet = ws->currentFrame;
                content = packet->content;
            }
#if FUTURE
            if (packet->type == WS_MSG_TEXT) {
                /*
                    Validate the frame for fast-fail, provided the last frame does not have a partial codepoint.
                 */
                if (!ws->partialUTF) {
                    if (!validateText(stream, packet)) {
                        error = WS_STATUS_INVALID_UTF8;
                        break;
                    }
                    ws->partialUTF = 0;
                }
            }
#endif
            frameLen = httpGetPacketLength(packet);
            assert(frameLen <= ws->frameLength);
            if (frameLen == ws->frameLength) {
                if ((error = processFrame(q, packet)) != 0) {
                    break;
                }
                if (ws->state == WS_STATE_CLOSED) {
                    HTTP_NOTIFY(stream, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
                    httpFinalize(stream);
                    ws->frameState = WS_CLOSED;
                    httpSetState(stream, HTTP_STATE_FINALIZED);
                    break;
                }
                ws->currentFrame = 0;
                ws->frameState = WS_BEGIN;
            } else {
                ws->currentFrame = packet;
            }
            break;

        default:
            traceError(stream, "Protocol error, unknown frame state");
            error = WS_STATUS_PROTOCOL_ERROR;
            break;
        }
        if (error) {
            /*
                Notify of the error and send a close to the peer. The peer may or may not be still there.
             */
            HTTP_NOTIFY(stream, HTTP_EVENT_ERROR, error);
            httpSendClose(stream, error, NULL);
            ws->frameState = WS_CLOSED;
            ws->state = WS_STATE_CLOSED;
            httpFinalize(stream);
            if (!stream->rx->eof) {
                httpSetEof(stream);
            }
            httpSetState(stream, HTTP_STATE_FINALIZED);
            return;
        }
    }
}


static int processFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpStream      *stream;
    HttpRx          *rx;
    HttpWebSocket   *ws;
    HttpLimits      *limits;
    MprBuf          *content;
    ssize           len;
    char            *cp;
    int             validated;

    stream = q->stream;
    limits = stream->limits;
    ws = stream->rx->webSocket;
    assert(ws);
    rx = stream->rx;
    assert(packet);
    content = packet->content;
    validated = 0;
    assert(content);

    mprAddNullToBuf(content);
    httpLog(stream->trace, "websockets.rx.packet", "context", "wsSeq:%d, wsTypeName:'%s', wsType:%d, wsLast:%d, wsLength:%zu",
         ws->rxSeq++, codetxt[packet->type], packet->type, packet->last, mprGetBufLength(content));

    switch (packet->type) {
    case WS_MSG_TEXT:
        httpLogPacket(stream->trace, "websockets.rx.data", "packet", 0, packet, 0);
        /* Fall through */

    case WS_MSG_BINARY:
        ws->messageLength = 0;
        ws->currentMessageType = packet->type;
        /* Fall through */

    case WS_MSG_CONT:
        if (ws->closing) {
            break;
        }
        if (packet->type == WS_MSG_CONT) {
            if (!ws->currentMessageType) {
                traceError(stream, "Bad continuation packet");
                return WS_STATUS_PROTOCOL_ERROR;
            }
            packet->type = ws->currentMessageType;
        }
        /*
            Validate this frame if we don't have a partial codepoint from a prior frame.
         */
        if (packet->type == WS_MSG_TEXT && !ws->partialUTF) {
            if (!validateText(stream, packet)) {
                return WS_STATUS_INVALID_UTF8;
            }
            validated++;
        }
        if (ws->currentMessage && !ws->preserveFrames) {
            httpJoinPacket(ws->currentMessage, packet);
            ws->currentMessage->last = packet->last;
            packet = ws->currentMessage;
            content = packet->content;
            if (packet->type == WS_MSG_TEXT && !validated) {
                if (!validateText(stream, packet)) {
                    return WS_STATUS_INVALID_UTF8;
                }
            }
        }
        /*
            Send what we have if preserving frames or the current messages is over the packet limit size.
            Otherwise, keep buffering.
         */
        for (ws->tailMessage = 0; packet; packet = ws->tailMessage, ws->tailMessage = 0) {
            if (!ws->preserveFrames && (httpGetPacketLength(packet) > limits->webSocketsPacketSize)) {
                ws->tailMessage = httpSplitPacket(packet, limits->webSocketsPacketSize);
                content = packet->content;
                packet->last = 0;
            }
            if (packet->last || ws->tailMessage || ws->preserveFrames) {
                packet->flags |= HTTP_PACKET_SOLO;
                ws->messageLength += httpGetPacketLength(packet);
                if (packet->type == WS_MSG_TEXT) {
                    mprAddNullToBuf(packet->content);
                }
                httpPutPacketToNext(q, packet);
                ws->currentMessage = 0;
            } else {
                ws->currentMessage = packet;
                break;
            }
            if (packet->last) {
                ws->currentMessageType = 0;
            }
        }
        if (stream->readq->first) {
            HTTP_NOTIFY(stream, HTTP_EVENT_READABLE, 0);
        }
        break;

    case WS_MSG_CLOSE:
        cp = content->start;
        if (httpGetPacketLength(packet) == 0) {
            ws->closeStatus = WS_STATUS_OK;
        } else if (httpGetPacketLength(packet) < 2) {
            traceError(stream, "Missing close status");
            return WS_STATUS_PROTOCOL_ERROR;
        } else {
            ws->closeStatus = ((uchar) cp[0]) << 8 | (uchar) cp[1];

            /*
                WebSockets is a hideous spec, as if UTF validation wasn't bad enough, we must invalidate these codes:
                    1004, 1005, 1006, 1012-1016, 2000-2999
             */
            if (ws->closeStatus < 1000 || ws->closeStatus >= 5000 ||
                (1004 <= ws->closeStatus && ws->closeStatus <= 1006) ||
                (1012 <= ws->closeStatus && ws->closeStatus <= 1016) ||
                (1100 <= ws->closeStatus && ws->closeStatus <= 2999)) {
                traceError(stream, "Bad close status %d", ws->closeStatus);
                return WS_STATUS_PROTOCOL_ERROR;
            }
            mprAdjustBufStart(content, 2);
            if (httpGetPacketLength(packet) > 0) {
                ws->closeReason = mprCloneBufMem(content);
                if (!rx->route || !rx->route->ignoreEncodingErrors) {
                    if (validUTF8(stream, ws->closeReason, slen(ws->closeReason)) != UTF8_ACCEPT) {
                        traceError(stream, "Text packet has invalid UTF8");
                        return WS_STATUS_INVALID_UTF8;
                    }
                }
            }
        }
        httpLog(stream->trace, "websockets.rx.close", "context",
            "wsCloseStatus:%d, wsCloseReason:'%s', wsClosing:%d", ws->closeStatus, ws->closeReason, ws->closing);
        if (ws->closing) {
            httpDisconnectStream(stream);
        } else {
            /* Acknowledge the close. Echo the received status */
            httpSendClose(stream, WS_STATUS_OK, "OK");
            if (!stream->rx->eof) {
                httpSetEof(stream);
            }
            rx->remainingContent = 0;
            stream->keepAliveCount = 0;
        }
        ws->state = WS_STATE_CLOSED;
        break;

    case WS_MSG_PING:
        /* Respond with the same content as specified in the ping message */
        len = mprGetBufLength(content);
        len = min(len, WS_MAX_CONTROL);
        httpSendBlock(stream, WS_MSG_PONG, mprGetBufStart(content), mprGetBufLength(content), HTTP_BUFFER);
        break;

    case WS_MSG_PONG:
        /* Do nothing */
        break;

    default:
        traceError(stream, "Bad message type %d", packet->type);
        ws->state = WS_STATE_CLOSED;
        return WS_STATUS_PROTOCOL_ERROR;
    }
    return 0;
}


/*
    Send a text message. Caller must submit valid UTF8.
    Returns the number of data message bytes written. Should equal the length.
 */
PUBLIC ssize httpSend(HttpStream *stream, cchar *fmt, ...)
{
    va_list     args;
    char        *buf;

    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);
    return httpSendBlock(stream, WS_MSG_TEXT, buf, slen(buf), HTTP_BUFFER);
}


/*
    Send a block of data with the specified message type. Set flags to HTTP_MORE to indicate there is more data
    for this message.
 */
PUBLIC ssize httpSendBlock(HttpStream *stream, int type, cchar *buf, ssize len, int flags)
{
    HttpWebSocket   *ws;
    HttpPacket      *packet;
    HttpQueue       *q;
    ssize           room, thisWrite, totalWritten;

    assert(stream);

    ws = stream->rx->webSocket;
    stream->tx->responded = 1;

    /*
        Note: we can come here before the handshake is complete. The data is queued and if the connection handshake
        succeeds, then the data is sent.
     */
    if (!(HTTP_STATE_CONNECTED <= stream->state && stream->state < HTTP_STATE_FINALIZED) || !stream->upgraded || stream->error) {
        return MPR_ERR_BAD_STATE;
    }
    if (type != WS_MSG_CONT && type != WS_MSG_TEXT && type != WS_MSG_BINARY && type != WS_MSG_CLOSE &&
            type != WS_MSG_PING && type != WS_MSG_PONG) {
        traceError(stream, "Bad message type %d", type);
        return MPR_ERR_BAD_ARGS;
    }
    q = stream->writeq;
    if (flags == 0) {
        flags = HTTP_BUFFER;
    }
    if (len < 0) {
        len = slen(buf);
    }
    if (len > stream->limits->webSocketsMessageSize) {
        if (httpServerStream(stream)) {
            httpMonitorEvent(stream, HTTP_COUNTER_LIMIT_ERRORS, 1);
        }
        traceError(stream, "Outgoing message is too large, length %zd max %zd", len, stream->limits->webSocketsMessageSize);
        return MPR_ERR_WONT_FIT;
    }
    totalWritten = 0;
    do {
        if ((room = q->max - q->count) == 0) {
            if (flags & HTTP_NON_BLOCK) {
                break;
            }
        }
        /*
            Break into frames if the user is not preserving frames and has not explicitly specified "more".
            The outgoingWebSockService will encode each packet as a frame.
         */
        if (ws->preserveFrames || (flags & HTTP_MORE)) {
            thisWrite = len;
        } else {
            thisWrite = min(len, stream->limits->webSocketsFrameSize);
        }
        thisWrite = min(thisWrite, q->packetSize);
        if (flags & (HTTP_BLOCK | HTTP_NON_BLOCK)) {
            thisWrite = min(thisWrite, room);
        }
        /*
            Must still send empty packets of zero length
         */
        if ((packet = httpCreateDataPacket(thisWrite)) == 0) {
            return MPR_ERR_MEMORY;
        }
        /*
            Spec requires type to be set only on the first frame
         */
        if (ws->more) {
            type = 0;
        }
        packet->type = type;
        type = 0;
        if (ws->preserveFrames || (flags & HTTP_MORE)) {
            packet->flags |= HTTP_PACKET_SOLO;
        }
        if (thisWrite > 0) {
            if (mprPutBlockToBuf(packet->content, buf, thisWrite) != thisWrite) {
                return MPR_ERR_MEMORY;
            }
        }
        len -= thisWrite;
        buf += thisWrite;
        totalWritten += thisWrite;
        packet->last = (len > 0) ? 0 : !(flags & HTTP_MORE);
        ws->more = !packet->last;
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);

        if (q->count >= q->max) {
            httpFlushQueue(q, flags);
            if (q->count >= q->max && (flags & HTTP_NON_BLOCK)) {
                break;
            }
        }
        if (httpRequestExpired(stream, 0)) {
            return MPR_ERR_TIMEOUT;
        }
    } while (len > 0);

    httpFlushQueue(q, flags);
    if (httpClientStream(stream)) {
        httpEnableNetEvents(stream->net);
    }
    return totalWritten;
}


/*
    The reason string is optional
 */
PUBLIC ssize httpSendClose(HttpStream *stream, int status, cchar *reason)
{
    HttpWebSocket   *ws;
    char            msg[128];
    ssize           len;

    assert(0 <= status && status <= WS_STATUS_MAX);
    ws = stream->rx->webSocket;
    assert(ws);
    if (ws->closing) {
        return 0;
    }
    ws->closing = 1;
    ws->state = WS_STATE_CLOSING;

    if (!(HTTP_STATE_CONNECTED <= stream->state && stream->state < HTTP_STATE_FINALIZED) || !stream->upgraded) {
        /* Ignore closes when already finalized or not yet connected */
        return 0;
    }
    len = 2;
    if (reason) {
        if (slen(reason) >= 124) {
            reason = "WebSockets close message was too big";
            traceError(stream, reason);
        }
        len += slen(reason) + 1;
    }
    msg[0] = (status >> 8) & 0xff;
    msg[1] = status & 0xff;
    if (reason) {
        scopy(&msg[2], len - 2, reason);
    }
    httpLog(stream->trace, "websockets.tx.close", "context", "wsCloseStatus:%d, wsCloseReason:'%s'", status, reason);
    return httpSendBlock(stream, WS_MSG_CLOSE, msg, len, HTTP_BUFFER);
}


/*
    This is the outgoing filter routine. It services packets on the outgoing queue and transforms them into
    WebSockets frames.
 */
static void outgoingWebSockService(HttpQueue *q)
{
    HttpStream      *stream;
    HttpPacket      *packet, *tail;
    HttpWebSocket   *ws;
    char            *ep, *fp, *prefix, dataMask[4];
    ssize           len;
    int             i, mask;

    stream = q->stream;
    ws = stream->rx->webSocket;
    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!(packet->flags & (HTTP_PACKET_END | HTTP_PACKET_HEADER))) {
            if (!(packet->flags & HTTP_PACKET_SOLO)) {
                if (packet->esize > stream->limits->packetSize) {
                    if ((tail = httpResizePacket(q, packet, stream->limits->packetSize)) != 0) {
                        assert(tail->last == packet->last);
                        packet->last = 0;
                    }
                }
                if (!httpWillNextQueueAcceptPacket(q, packet)) {
                    httpPutBackPacket(q, packet);
                    return;
                }
            }
            if (packet->type < 0 || packet->type > WS_MSG_MAX) {
                httpError(stream, HTTP_CODE_INTERNAL_SERVER_ERROR, "Bad WebSocket packet type %d", packet->type);
                break;
            }
            len = httpGetPacketLength(packet);
            packet->prefix = mprCreateBuf(16, 16);
            prefix = packet->prefix->start;
            /*
                Server-side does not mask outgoing data
             */
            mask = httpServerStream(stream) ? 0 : 1;
            *prefix++ = SET_FIN(packet->last) | SET_CODE(packet->type);
            if (len <= WS_MAX_CONTROL) {
                *prefix++ = SET_MASK(mask) | SET_LEN(len, 0);
            } else if (len <= 65535) {
                *prefix++ = SET_MASK(mask) | 126;
                *prefix++ = SET_LEN(len, 1);
                *prefix++ = SET_LEN(len, 0);
            } else {
                *prefix++ = SET_MASK(mask) | 127;
                for (i = 7; i >= 0; i--) {
                    *prefix++ = SET_LEN(len, i);
                }
            }
            if (httpClientStream(stream)) {
                mprGetRandomBytes(dataMask, sizeof(dataMask), 0);
                for (i = 0; i < 4; i++) {
                    *prefix++ = dataMask[i];
                }
                fp = packet->content->start;
                ep = packet->content->end;
                for (i = 0; fp < ep; fp++) {
                    *fp = *fp ^ dataMask[i++ & 0x3];
                }
            }
            *prefix = '\0';
            mprAdjustBufEnd(packet->prefix, prefix - packet->prefix->start);
            httpLogPacket(stream->trace, "websockets.tx.packet", "packet", 0, packet,
                "wsSeqno:%d, wsTypeName:\"%s\", wsType:%d, wsLast:%d, wsLength:%zd",
                ws->txSeq++, codetxt[packet->type], packet->type, packet->last, httpGetPacketLength(packet));
        }
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC cchar *httpGetWebSocketCloseReason(HttpStream *stream)
{
    HttpWebSocket   *ws;

    if (!stream || !stream->rx) {
        return 0;
    }
    if ((ws = stream->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->closeReason;
}


PUBLIC void *httpGetWebSocketData(HttpStream *stream)
{
    return (stream->rx && stream->rx->webSocket) ? stream->rx->webSocket->data : NULL;
}


PUBLIC ssize httpGetWebSocketMessageLength(HttpStream *stream)
{
    HttpWebSocket   *ws;

    if (!stream || !stream->rx) {
        return 0;
    }
    if ((ws = stream->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->messageLength;
}


PUBLIC char *httpGetWebSocketProtocol(HttpStream *stream)
{
    HttpWebSocket   *ws;

    if (!stream || !stream->rx) {
        return 0;
    }
    if ((ws = stream->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->subProtocol;
}


PUBLIC ssize httpGetWebSocketState(HttpStream *stream)
{
    HttpWebSocket   *ws;

    if (!stream || !stream->rx) {
        return 0;
    }
    if ((ws = stream->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->state;
}


PUBLIC bool httpWebSocketOrderlyClosed(HttpStream *stream)
{
    HttpWebSocket   *ws;

    if (!stream || !stream->rx) {
        return 0;
    }
    if ((ws = stream->rx->webSocket) == 0) {
        return 0;
    }
    assert(ws);
    return ws->closeStatus != WS_STATUS_COMMS_ERROR;
}


PUBLIC void httpSetWebSocketData(HttpStream *stream, void *data)
{
    if (stream->rx && stream->rx->webSocket) {
        stream->rx->webSocket->data = data;
    }
}


PUBLIC void httpSetWebSocketProtocols(HttpStream *stream, cchar *protocols)
{
    assert(stream);
    assert(protocols && *protocols);
    stream->protocols = sclone(protocols);
}


PUBLIC void httpSetWebSocketPreserveFrames(HttpStream *stream, bool on)
{
    HttpWebSocket   *ws;

    if ((ws = stream->rx->webSocket) != 0) {
        ws->preserveFrames = on;
    }
}


/*
    Test if a string is a valid unicode string.
    The return state may be UTF8_ACCEPT if all codepoints validate and are complete.
    Return UTF8_REJECT if an invalid codepoint was found.
    Otherwise, return the state for a partial codepoint.
 */
static int validUTF8(HttpStream *stream, cchar *str, ssize len)
{
    uchar   *cp, c;
    uint    state, type;

    state = UTF8_ACCEPT;
    for (cp = (uchar*) str; cp < (uchar*) &str[len]; cp++) {
        c = *cp;
        type = utfTable[c];
        /*
            codepoint = (*state != UTF8_ACCEPT) ? (byte & 0x3fu) | (*codep << 6) : (0xff >> type) & (byte);
         */
        state = utfTable[256 + (state * 16) + type];
        if (state == UTF8_REJECT) {
            traceError(stream, "Invalid UTF8 at offset %d", cp - (uchar*) str);
            break;
        }
    }
    return state;
}


/*
    Validate the UTF8 in a packet. Return false if an invalid codepoint is found.
    If the packet is not the last packet, we alloc incomplete codepoints.
    Set ws->partialUTF if the last codepoint was incomplete.
 */
static bool validateText(HttpStream *stream, HttpPacket *packet)
{
    HttpWebSocket   *ws;
    HttpRx          *rx;
    MprBuf          *content;
    int             state;
    bool            valid;

    rx = stream->rx;
    ws = rx->webSocket;

    /*
        Skip validation if ignoring errors or some frames have already been sent to the callback
     */
    if ((rx->route && rx->route->ignoreEncodingErrors) || ws->messageLength > 0) {
        return 1;
    }
    content = packet->content;
    state = validUTF8(stream, content->start, mprGetBufLength(content));
    ws->partialUTF = state != UTF8_ACCEPT;

    if (packet->last) {
        valid =  state == UTF8_ACCEPT;
    } else {
        valid = state != UTF8_REJECT;
    }
    if (!valid) {
        traceError(stream, "Text packet has invalid UTF8");
    }
    return valid;
}


static void webSockPing(HttpStream *stream)
{
    assert(stream);
    assert(stream->rx);
    /*
        Send a ping. Optimze by sending no data message with it.
     */
    httpSendBlock(stream, WS_MSG_PING, NULL, 0, HTTP_BUFFER);
}


static void webSockTimeout(HttpStream *stream)
{
    assert(stream);
    httpSendClose(stream, WS_STATUS_POLICY_VIOLATION, "Request timeout");
}


/*
    Upgrade a client socket to use Web Sockets. This is called by the client to request a web sockets upgrade.
 */
PUBLIC int httpUpgradeWebSocket(HttpStream *stream)
{
    HttpTx  *tx;
    char    num[16];

    tx = stream->tx;
    assert(httpClientStream(stream));

    httpSetStatus(stream, HTTP_CODE_SWITCHING);
    httpSetHeader(stream, "Upgrade", "websocket");
    httpSetHeader(stream, "Connection", "Upgrade");
    mprGetRandomBytes(num, sizeof(num), 0);
    tx->webSockKey = mprEncode64Block(num, sizeof(num));
    httpSetHeaderString(stream, "Sec-WebSocket-Key", tx->webSockKey);
    httpSetHeaderString(stream, "Sec-WebSocket-Protocol", stream->protocols ? stream->protocols : "chat");
    httpSetHeaderString(stream, "Sec-WebSocket-Version", "13");
    httpSetHeader(stream, "X-Request-Timeout", "%lld", stream->limits->requestTimeout / TPS);
    httpSetHeader(stream, "X-Inactivity-Timeout", "%lld", stream->limits->inactivityTimeout / TPS);

    stream->upgraded = 1;
    stream->keepAliveCount = 0;
    stream->rx->remainingContent = HTTP_UNLIMITED;
    return 0;
}


/*
    Client verification of the server WebSockets handshake response
 */
PUBLIC bool httpVerifyWebSocketsHandshake(HttpStream *stream)
{
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *key, *expected;

    rx = stream->rx;
    tx = stream->tx;
    assert(rx);
    assert(rx->webSocket);
    assert(stream->upgraded);
    assert(httpClientStream(stream));

    rx->webSocket->state = WS_STATE_CLOSED;

    if (rx->status != HTTP_CODE_SWITCHING) {
        httpError(stream, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake status %d", rx->status);
        return 0;
    }
    if (!smatch(httpGetHeader(stream, "connection"), "Upgrade")) {
        httpError(stream, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Connection header");
        return 0;
    }
    if (!smatch(httpGetHeader(stream, "upgrade"), "WebSocket")) {
        httpError(stream, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Upgrade header");
        return 0;
    }
    expected = mprGetSHABase64(sjoin(tx->webSockKey, WS_MAGIC, NULL));
    key = httpGetHeader(stream, "sec-websocket-accept");
    if (!smatch(key, expected)) {
        httpError(stream, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake key\n%s\n%s", key, expected);
        return 0;
    }
    rx->webSocket->state = WS_STATE_OPEN;
    return 1;
}


static void traceErrorProc(HttpStream *stream, cchar *fmt, ...)
{
    HttpWebSocket   *ws;
    va_list         args;

    ws = stream->rx->webSocket;
    va_start(args, fmt);
    ws->errorMsg = sfmtv(fmt, args);
    va_end(args);

    httpLog(stream->trace, "websockets.tx.error", "error", "msg:'%s'", ws->errorMsg);
}

#endif /* ME_HTTP_WEB_SOCKETS */

/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
