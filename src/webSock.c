/*
    webSock.c - WebSockets support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Locals ************************************/
/*
    Message frame states
 */
#define WS_BEGIN       0
#define WS_EXT_DATA    1                /* Unused */
#define WS_MSG         2
#define WS_CLOSED      3

static char *codetxt[16] = {
    "continuation", "text", "binary", "reserved", "reserved", "reserved", "reserved", "reserved",
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

/********************************** Forwards **********************************/

static void closeWebSock(HttpQueue *q);
static void incomingWebSockData(HttpQueue *q, HttpPacket *packet);
static void manageWebSocket(HttpWebSocket *ws, int flags);
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir);
static void openWebSock(HttpQueue *q);
static void outgoingWebSockService(HttpQueue *q);
static void readyWebSock(HttpQueue *q);
static bool validUTF8(cchar *str, ssize len);
static void webSockPing(HttpConn *conn);
static void webSockTimeout(HttpConn *conn);

/*********************************** Code *************************************/
/* 
   WebSocket Filter initialization
 */
PUBLIC int httpOpenWebSockFilter(Http *http)
{
    HttpStage     *filter;

    assure(http);

    mprLog(5, "Open WebSock filter");
    if ((filter = httpCreateFilter(http, "webSocketFilter", NULL)) == 0) {
        return MPR_ERR_CANT_CREATE;
    }
    http->webSocketFilter = filter;
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
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpWebSocket   *ws;
    HttpRx          *rx;
    HttpTx          *tx;
    char            *kind, *tok;
    cchar           *key, *protocols;
    int             version;

    assure(conn);
    assure(route);
    rx = conn->rx;
    tx = conn->tx;
    assure(rx);
    assure(tx);

    if (!conn->endpoint) {
        if (rx->webSocket) {
            return HTTP_ROUTE_OK;
        } else if (tx->parsedUri && tx->parsedUri->webSockets) {
            /* ws:// URI. Client web sockets */
            if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
                httpMemoryError(conn);
                return HTTP_ROUTE_OK;
            }
            rx->webSocket = ws;
            ws->state = WS_STATE_CONNECTING;
            return HTTP_ROUTE_OK;
        }
        return HTTP_ROUTE_REJECT;
    }
    if (dir & HTTP_STAGE_TX) {
        return rx->webSocket ? HTTP_ROUTE_OK : HTTP_ROUTE_REJECT;
    }
    if (!rx->upgrade || !scaselessmatch(rx->upgrade, "websocket")) {
        return HTTP_ROUTE_REJECT;
    }
    if (!rx->hostHeader || !smatch(rx->method, "GET")) {
        return HTTP_ROUTE_REJECT;
    }
    version = (int) stoi(httpGetHeader(conn, "sec-websocket-version"));
    if (version < WS_VERSION) {
        httpSetHeader(conn, "Sec-WebSocket-Version", "%d", WS_VERSION);
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Version");
        return HTTP_ROUTE_OK;
    }
    if ((key = httpGetHeader(conn, "sec-websocket-key")) == 0) {
        httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocket-Key");
        return HTTP_ROUTE_OK;
    }
    protocols = httpGetHeader(conn, "sec-websocket-protocol");

    if (dir & HTTP_STAGE_RX) {
        if ((ws = mprAllocObj(HttpWebSocket, manageWebSocket)) == 0) {
            httpMemoryError(conn);
            return HTTP_ROUTE_OK;
        }
        rx->webSocket = ws;
        ws->state = WS_STATE_OPEN;
        /* Just select the first protocol */
        if (route->webSocketsProtocol) {
            for (kind = stok(sclone(protocols), " \t,", &tok); kind; kind = stok(NULL, " \t,", &tok)) {
                if (smatch(route->webSocketsProtocol, kind)) {
                    break;
                }
            }
            if (!kind) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Protocol");
                return HTTP_ROUTE_OK;
            }
            ws->subProtocol = sclone(kind);
        } else {
            /* Just pick the first protocol */
            ws->subProtocol = stok(sclone(protocols), " ,", NULL);
        }
        httpSetStatus(conn, HTTP_CODE_SWITCHING);
        httpSetHeader(conn, "Connection", "Upgrade");
        httpSetHeader(conn, "Upgrade", "WebSocket");
        httpSetHeader(conn, "Sec-WebSocket-Accept", mprGetSHABase64(sjoin(key, WS_MAGIC, NULL)));
        if (ws->subProtocol && *ws->subProtocol) {
            httpSetHeader(conn, "Sec-WebSocket-Protocol", ws->subProtocol);
        }
        httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
        httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);

        if (route->webSocketsPingPeriod) {
            ws->pingEvent = mprCreateEvent(conn->dispatcher, "webSocket", route->webSocketsPingPeriod, 
                webSockPing, conn, MPR_EVENT_CONTINUOUS);
        }
        conn->keepAliveCount = -1;
        conn->upgraded = 1;
        rx->eof = 0;
        rx->remainingContent = MAXINT;
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


/*
    Open the filter for a new request
 */
static void openWebSock(HttpQueue *q)
{
    HttpConn        *conn;
    HttpWebSocket   *ws;
    HttpPacket      *packet;

    assure(q);
    conn = q->conn;
    ws = conn->rx->webSocket;
    assure(ws);

    mprLog(5, "webSocketFilter: Opening a new request ");
    q->packetSize = min(conn->limits->bufferSize, q->max);
    ws->closeStatus = WS_STATUS_NO_STATUS;
    conn->timeoutCallback = webSockTimeout;

    if ((packet = httpGetPacket(conn->writeq)) != 0) {
        assure(packet->flags & HTTP_PACKET_HEADER);
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
    }
    conn->tx->responded = 0;
}


static void manageWebSocket(HttpWebSocket *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->currentFrame);
        mprMark(ws->currentMessage);
        mprMark(ws->pingEvent);
        mprMark(ws->subProtocol);
        mprMark(ws->closeReason);
    }
}


static void closeWebSock(HttpQueue *q)
{
    HttpWebSocket   *ws;

    if (q->conn && q->conn->rx) {
        ws = q->conn->rx->webSocket;
        assure(ws);
        if (ws && ws->pingEvent) {
            mprRemoveEvent(ws->pingEvent);
            ws->pingEvent = 0;
        }
    }
}


static void readyWebSock(HttpQueue *q)
{
    if (q->conn->endpoint) {
        HTTP_NOTIFY(q->conn, HTTP_EVENT_APP_OPEN, 0);
    }
}


static int processFrame(HttpQueue *q, HttpPacket *packet)
{
    HttpConn        *conn;
    HttpRx          *rx;
    HttpWebSocket   *ws;
    HttpLimits      *limits;
    HttpPacket      *tail;
    MprBuf          *content;
    char            *cp;

    conn = q->conn;
    limits = conn->limits;
    ws = conn->rx->webSocket;
    assure(ws);
    rx = conn->rx;
    assure(packet);
    content = packet->content;
    assure(content);

    mprLog(4, "webSocketFilter: Process packet type %d, \"%s\", data length %d", packet->type, 
        codetxt[packet->type], mprGetBufLength(content));

    switch (packet->type) {
    case WS_MSG_BINARY:
    case WS_MSG_TEXT:
        if (ws->closing) {
            break;
        }
        if (packet->type == WS_MSG_TEXT && !validUTF8(content->start, mprGetBufLength(content))) {
            if (!rx->route->ignoreEncodingErrors) {
                mprError("webSocketFilter: Text packet has invalid UTF8");
                return WS_STATUS_INVALID_UTF8;
            }
        }
        if (packet->type == WS_MSG_TEXT) {
            mprLog(5, "webSocketFilter: Text packet \"%s\"", content->start);
        }
        if (ws->currentMessage) {
            httpJoinPacket(ws->currentMessage, packet);
            ws->currentMessage->last = packet->last;
            packet = ws->currentMessage;
        }
        for (tail = 0; packet; packet = tail, tail = 0) {
            if (httpGetPacketLength(packet) > limits->webSocketsPacketSize) {
                tail = httpSplitPacket(packet, limits->webSocketsPacketSize);
                assure(tail);
                packet->last = 0;
            }
            if (packet->last || tail) {
                packet->flags |= HTTP_PACKET_SOLO;
                ws->messageLength += httpGetPacketLength(packet);
                httpPutPacketToNext(q, packet);
                ws->currentMessage = 0;
            } else {
                ws->currentMessage = packet;
                break;
            }
        } 
        break;

    case WS_MSG_CLOSE:
        cp = content->start;
        if (httpGetPacketLength(packet) >= 2) {
            ws->closeStatus = ((uchar) cp[0]) << 8 | (uchar) cp[1];
            if (httpGetPacketLength(packet) >= 4) {
                mprAddNullToBuf(content);
                if (ws->maskOffset >= 0) {
                    for (cp = content->start; cp < content->end; cp++) {
                        *cp = *cp ^ ws->dataMask[ws->maskOffset++ & 0x3];
                    }
                }
                ws->closeReason = sclone(&content->start[2]);
            }
        }
        mprLog(5, "webSocketFilter: close status %d, reason \"%s\", closing %d", ws->closeStatus, 
                ws->closeReason, ws->closing);
        if (ws->closing) {
            httpDisconnect(conn);
        } else {
            /* Acknowledge the close. Echo the received status */
            httpSendClose(conn, WS_STATUS_OK, NULL);
            rx->eof = 1;
            rx->remainingContent = 0;
        }
        /* Advance from the content state */
        httpSetState(conn, HTTP_STATE_READY);
        ws->state = WS_STATE_CLOSED;
        break;

    case WS_MSG_PING:
        /* Respond with the same content as specified in the ping message */
        httpSendBlock(conn, WS_MSG_PONG, mprGetBufStart(content), mprGetBufLength(content), HTTP_BUFFER);
        break;

    case WS_MSG_PONG:
        /* Do nothing */
        break;

    default:
        mprError("webSocketFilter: Bad message type %d", packet->type);
        ws->state = WS_STATE_CLOSED;
        return WS_STATUS_PROTOCOL_ERROR;
    }
    return 0;
}


static void incomingWebSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn        *conn;
    HttpWebSocket   *ws;
    HttpPacket      *tail;
    HttpLimits      *limits;
    MprBuf          *content;
    char            *fp, *cp;
    ssize           len, currentFrameLen, offset, frameLen;
    int             i, error, mask, lenBytes, opcode;

    assure(packet);
    conn = q->conn;
    assure(conn->rx);
    ws = conn->rx->webSocket;
    assure(ws);
    limits = conn->limits;
    VERIFY_QUEUE(q);

    if (packet->flags & HTTP_PACKET_DATA) {
        /*
            The service queue is used to hold data that is yet to be analyzed.
            The ws->currentFrame holds the current frame that is being read from the service queue.
         */
        httpJoinPacketForService(q, packet, 0);
    }
    mprLog(4, "webSocketFilter: incoming data. State %d, Frame state %d, Length: %d", 
        ws->state, ws->frameState, httpGetPacketLength(packet));

    if (packet->flags & HTTP_PACKET_END) {
        /* EOF packet means the socket has been abortively closed */
        ws->closing = 1;
        ws->frameState = WS_CLOSED;
        ws->state = WS_STATE_CLOSED;
        ws->closeStatus = WS_STATUS_COMMS_ERROR;
        HTTP_NOTIFY(conn, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
        httpError(conn, HTTP_ABORT | HTTP_CODE_COMMS_ERROR, "Connection lost");
    }
    while ((packet = httpGetPacket(q)) != 0) {
        content = packet->content;
        error = 0;
        mprLog(5, "webSocketFilter: incoming data, frame state %d", ws->frameState);
        switch (ws->frameState) {
        case WS_CLOSED:
            if (httpGetPacketLength(packet) > 0) {
                mprLog(5, "webSocketFilter: closed, ignore incoming packet");
            }
            httpFinalize(conn);
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
                break;
            }
            packet->last = GET_FIN(*fp);
            opcode = GET_CODE(*fp);
            if (opcode) {
                if (opcode > WS_MSG_PONG) {
                    error = WS_STATUS_PROTOCOL_ERROR;
                    break;
                }
                packet->type = opcode;
                if (opcode >= WS_MSG_CONTROL && !packet->last) {
                    /* Control frame, must not be fragmented */
                    error = WS_STATUS_PROTOCOL_ERROR;
                    break;
                }
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
            if (httpGetPacketLength(packet) < (lenBytes + (mask * 4))) {
                /* Return if we don't have the required packet control fields */
                httpPutBackPacket(q, packet);
                return;
            }
            fp++;
            while (--lenBytes > 0) {
                len <<= 8;
                len += (uchar) *fp++;
            }
            ws->frameLength = len;
            ws->frameState = WS_MSG;
            ws->maskOffset = mask ? 0 : -1;
            if (mask) {
                for (i = 0; i < 4; i++) {
                    ws->dataMask[i] = *fp++;
                }
            }
            assure(content);
            assure(fp >= content->start);
            mprAdjustBufStart(content, fp - content->start);
            assure(q->count >= 0);
            ws->frameState = WS_MSG;
            mprLog(5, "webSocketFilter: Begin new packet \"%s\", last %d, mask %d, length %d", codetxt[opcode & 0xf],
                packet->last, mask, len);
            /* Keep packet on queue as we need the packet->type */
            httpPutBackPacket(q, packet);
            if (httpGetPacketLength(packet) == 0) {
                return;
            }
            break;

        case WS_MSG:
            currentFrameLen = httpGetPacketLength(ws->currentFrame);
            len = httpGetPacketLength(packet);
            if ((currentFrameLen + len) > ws->frameLength) {
                /*
                    Split packet if it contains data for the next frame
                 */
                offset = ws->frameLength - currentFrameLen;
                if ((tail = httpSplitPacket(packet, offset)) != 0) {
                    tail->last = 0;
                    tail->type = 0;
                    httpPutBackPacket(q, tail);
                    mprLog(6, "webSocketFilter: Split data packet, %d/%d", ws->frameLength, httpGetPacketLength(tail));
                    len = httpGetPacketLength(packet);
                }
            }
            if ((currentFrameLen + len) > conn->limits->webSocketsMessageSize) {
                mprError("webSocketFilter: Incoming message is too large %d/%d", len, limits->webSocketsMessageSize);
                error = WS_STATUS_MESSAGE_TOO_LARGE;
                break;
            }
            if (packet->type == WS_MSG_CONT && ws->currentFrame) {
                mprLog(6, "webSocketFilter: Joining data packet %d/%d", currentFrameLen, len);
                httpJoinPacket(ws->currentFrame, packet);
                packet = ws->currentFrame;
                content = packet->content;
            }
            frameLen = httpGetPacketLength(packet);
            assure(frameLen <= ws->frameLength);
            if (frameLen == ws->frameLength) {
                /*
                    Got a cmplete frame 
                 */
                assure(packet->type);
                if (ws->maskOffset >= 0) {
                    for (cp = content->start; cp < content->end; cp++) {
                        *cp = *cp ^ ws->dataMask[ws->maskOffset++ & 0x3];
                    }
                } 
                if ((error = processFrame(q, packet)) != 0) {
                    break;
                }
                if (ws->state == WS_STATE_CLOSED) {
                    HTTP_NOTIFY(conn, HTTP_EVENT_APP_CLOSE, ws->closeStatus);
                    httpFinalize(conn);
                    ws->frameState = WS_CLOSED;
                    break;
                }
                ws->currentFrame = 0;
                ws->frameState = WS_BEGIN;
            } else {
                ws->currentFrame = packet;
            }
            break;

#if KEEP
        case WS_EXT_DATA:
            assure(packet);
            mprLog(5, "webSocketFilter: EXT DATA - RESERVED");
            ws->frameState = WS_MSG;
            break;
#endif

        default:
            error = WS_STATUS_PROTOCOL_ERROR;
            break;
        }
        if (error) {
            /*
                Notify of the error and send a close to the peer. The peer may or may not be still there.
                Want to wait for a possible close response message, so don't finalize here.
             */
            mprError("webSocketFilter: WebSockets error Status %d", error);
            HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, error);
            httpSendClose(conn, error, NULL);
            ws->frameState = WS_CLOSED;
            ws->state = WS_STATE_CLOSED;
            return;
        }
    }
}


/*
    Send a text message. Caller must submit valid UTF8.
    Returns the number of data message bytes written. Should equal the length.
 */
PUBLIC ssize httpSend(HttpConn *conn, cchar *fmt, ...)
{
    va_list     args;
    char        *buf;

    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);
    return httpSendBlock(conn, WS_MSG_TEXT, buf, slen(buf), HTTP_BUFFER);
}


/*
    Send a block of data with the specified message type. Set last to true for the last block of a logical message.
    WARNING: this absorbs all data. The caller should ensure they don't write too much by checking conn->writeq->count.
 */
PUBLIC ssize httpSendBlock(HttpConn *conn, int type, cchar *buf, ssize len, int flags)
{
    HttpPacket  *packet;
    HttpQueue   *q;
    ssize       thisWrite, totalWritten;

    assure(conn);
    assure(buf);

    /*
        Note: we can come here before the handshake is complete. The data is queued and if the connection handshake 
        succeeds, then the data is sent.
     */
    assure(HTTP_STATE_CONNECTED <= conn->state && conn->state < HTTP_STATE_FINALIZED);

    if (type < 0 || type > WS_MSG_PONG) {
        mprError("webSocketFilter: httpSendBlock: bad message type %d", type);
        return MPR_ERR_BAD_ARGS;
    }
    q = conn->writeq;
    if (flags == 0) {
        flags = HTTP_BUFFER;
    }
    if (len < 0) {
        len = slen(buf);
    }
    if (len > conn->limits->webSocketsMessageSize) {
        mprError("webSocketFilter: Outgoing message is too large %d/%d", len, conn->limits->webSocketsMessageSize);
        return MPR_ERR_WONT_FIT;
    }
    mprLog(5, "webSocketFilter: Sending message type \"%s\", len %d", codetxt[type & 0xf], len);
    totalWritten = 0;
    do {
        /*
            Break into frames. Note: downstream may also fragment packets.
            The outgoing service routine will convert every packet into a frame.
         */
        thisWrite = min(len, conn->limits->webSocketsFrameSize);
        thisWrite = min(thisWrite, q->packetSize);
        if (flags & (HTTP_BLOCK | HTTP_NON_BLOCK)) {
            thisWrite = min(thisWrite, q->max - q->count);
        }
        if ((packet = httpCreateDataPacket(thisWrite)) == 0) {
            return MPR_ERR_MEMORY;
        }
        packet->type = type;
        if (thisWrite > 0) {
            if (mprPutBlockToBuf(packet->content, buf, thisWrite) != thisWrite) {
                return MPR_ERR_MEMORY;
            }
            len -= thisWrite;
            buf += thisWrite;
            totalWritten += thisWrite;
        }
        packet->last = (len > 0) ? 0 : !(flags & HTTP_MORE);
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);

        if (q->count >= q->max) {
            httpFlushQueue(q, 0);
            if (q->count >= q->max) {
                if (flags & HTTP_NON_BLOCK) {
                    break;
                } else if (flags & HTTP_BLOCK) {
                    while (q->count >= q->max) {
                        assure(conn->limits->inactivityTimeout > 10);
                        mprWaitForEvent(conn->dispatcher, conn->limits->inactivityTimeout);
                    }
                }
            }
        }
    } while (len > 0);
    httpServiceQueues(conn);
    return totalWritten;
}


/*
    The reason string is optional
 */
PUBLIC void httpSendClose(HttpConn *conn, int status, cchar *reason)
{
    HttpWebSocket   *ws;
    char            msg[128];
    ssize           len;

    assure(0 <= status && status <= WS_STATUS_MAX);
    ws = conn->rx->webSocket;
    assure(ws);
    if (ws->closing) {
        return;
    }
    ws->closing = 1;
    ws->state = WS_STATE_CLOSING;
    len = 2;
    if (reason) {
        if (slen(reason) >= 124) {
            reason = "WebSockets reason message was too big";
            mprError(reason);
        }
        len += slen(reason) + 1;
    }
    msg[0] = (status >> 8) & 0xff;
    msg[1] = status & 0xff;
    if (reason) {
        scopy(&msg[2], len - 2, reason);
    }
    mprLog(5, "webSocketFilter: sendClose, status %d reason \"%s\"", status, reason);
    httpSendBlock(conn, WS_MSG_CLOSE, msg, len, HTTP_BUFFER);
}


/*
    This is the outgoing filter routine. It services packets on the outgoing queue and transforms them into 
    WebSockets frames.
 */
static void outgoingWebSockService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;
    char        *ep, *fp, *prefix, dataMask[4];
    ssize       len;
    int         i, mask;

    conn = q->conn;
    mprLog(6, "webSocketFilter: outgoing service");

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!(packet->flags & (HTTP_PACKET_END | HTTP_PACKET_HEADER))) {
            httpResizePacket(q, packet, conn->limits->bufferSize);
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            if (packet->type < 0 || packet->type > WS_MSG_MAX) {
                httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Bad WebSocket packet type %d", packet->type);
                break;
            }
            len = httpGetPacketLength(packet);
            packet->prefix = mprCreateBuf(16, 16);
            prefix = packet->prefix->start;
            /*
                Server-side does not mask outgoing data
             */
            mask = conn->endpoint ? 0 : 1;
            *prefix++ = SET_FIN(packet->last) | SET_CODE(packet->type);
            if (len <= 125) {
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
            if (!conn->endpoint) {
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
            mprLog(6, "webSocketFilter: outgoing service, data packet len %d", httpGetPacketLength(packet));
        }
        httpPutPacketToNext(q, packet);
    }
}


PUBLIC char *httpGetWebSocketCloseReason(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assure(ws);
    return ws->closeReason;
}


PUBLIC ssize httpGetWebSocketMessageLength(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assure(ws);
    return ws->messageLength;
}


PUBLIC char *httpGetWebSocketProtocol(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assure(ws);
    return ws->subProtocol;
}


PUBLIC ssize httpGetWebSocketState(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assure(ws);
    return ws->state;
}


PUBLIC bool httpWebSocketOrderlyClosed(HttpConn *conn)
{
    HttpWebSocket   *ws;

    if (!conn || !conn->rx) {
        return 0;
    }
    if ((ws = conn->rx->webSocket) == 0) {
        return 0;
    }
    assure(ws);
    return ws->closeStatus != WS_STATUS_COMMS_ERROR;
}


PUBLIC void httpSetWebSocketProtocols(HttpConn *conn, cchar *protocols)
{
    assure(conn);
    assure(protocols && *protocols);
    conn->protocols = sclone(protocols);
}


static bool validUTF8(cchar *str, ssize len)
{
    cuchar      *cp, *end;
    int         nbytes, i;
  
    assure(str);
    cp = (cuchar*) str;
    end = (cuchar*) &str[len];
    for (; cp < end && *cp; cp += nbytes) {
        if (!(*cp & 0x80)) {
            nbytes = 1;
        } else if ((*cp & 0xc0) == 0x80) {
            return 0;
        } else if ((*cp & 0xe0) == 0xc0) {
            nbytes = 2;
        } else if ((*cp & 0xf0) == 0xe0) {
            nbytes = 3;
        } else if ((*cp & 0xf8) == 0xf0) {
            nbytes = 4;
        } else if ((*cp & 0xfc) == 0xf8) {
            nbytes = 5;
        } else if ((*cp & 0xfe) == 0xfc) {
            nbytes = 6;
        } else {
            nbytes = 1;
        }
        for (i = 1; i < nbytes; i++) {
            if ((cp[i] & 0xc0) != 0x80) {
                return 0;
            }
        }
        assure(nbytes >= 1);
    } 
    return 1;
}


static void webSockPing(HttpConn *conn)
{
    assure(conn);
    assure(conn->rx);
    /*
        Send a ping. Optimze by sending no data message with it.
     */
    httpSendBlock(conn, WS_MSG_PING, NULL, 0, HTTP_BUFFER);
}


static void webSockTimeout(HttpConn *conn)
{
    assure(conn);
    httpSendClose(conn, WS_STATUS_POLICY_VIOLATION, "Request timeout");
}


/*
    Upgrade a client socket to use Web Sockets. This is called by the client to request a web sockets upgrade.
 */
PUBLIC int httpUpgradeWebSocket(HttpConn *conn)
{
    HttpTx  *tx;
    char    num[16];

    assure(!conn->endpoint);
    tx = conn->tx;
    mprLog(5, "webSocketFilter: Upgrade socket");
    httpSetStatus(conn, HTTP_CODE_SWITCHING);
    httpSetHeader(conn, "Upgrade", "websocket");
    httpSetHeader(conn, "Connection", "Upgrade");
    mprGetRandomBytes(num, sizeof(num), 0);
    tx->webSockKey = mprEncode64Block(num, sizeof(num));
    httpSetHeader(conn, "Sec-WebSocket-Key", tx->webSockKey);
    httpSetHeader(conn, "Sec-WebSocket-Protocol", conn->protocols ? conn->protocols : "chat");
    httpSetHeader(conn, "Sec-WebSocket-Version", "13");
    httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
    httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
    conn->upgraded = 1;
    conn->keepAliveCount = -1;
    conn->rx->remainingContent = MAXINT;
    return 0;
}


/*
    Client verification of the server WebSockets handshake response
 */
PUBLIC bool httpVerifyWebSocketsHandshake(HttpConn *conn)
{
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *key, *expected;

    assure(!conn->endpoint);
    rx = conn->rx;
    tx = conn->tx;
    assure(rx);
    assure(rx->webSocket);
    assure(conn->upgraded);

    if (rx->status != HTTP_CODE_SWITCHING) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake status %d", rx->status);
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "connection"), "Upgrade")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Connection header");
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "upgrade"), "WebSocket")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Upgrade header");
        return 0;
    }
    expected = mprGetSHABase64(sjoin(tx->webSockKey, WS_MAGIC, NULL));
    key = httpGetHeader(conn, "sec-websocket-accept");
    if (!smatch(key, expected)) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake key\n%s\n%s", key, expected);
        return 0;
    }
    rx->webSocket->state = WS_STATE_OPEN;
    mprLog(4, "WebSockets handsake verified");
    return 1;
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
