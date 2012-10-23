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

/*
    Web Sockets message codes
 */
#define OP_CONT     0x0         /* Continuation */
#define OP_TEXT     0x1         /* Text data */
#define OP_BINARY   0x2         /* Binary data */
#define OP_CONTROL  0x8         /* Start of control codes */
#define OP_CLOSE    0x8         /* Close connection */
#define OP_PING     0x9         /* Ping request */
#define OP_PONG     0xA

static int opcodes[8] = {
    OP_CLOSE, OP_TEXT, OP_BINARY, OP_PING, OP_PONG, OP_CLOSE, OP_CLOSE, OP_CLOSE,
};
static char *codetxt[16] = {
    "continuation", "text", "binary", "reserved", "reserved", "reserved", "reserved", "reserved",
    "close", "ping", "pong", "reserved", "reserved", "reserved", "reserved", "reserved",
};

static char *wscodetxt[] = {
    "close", "text", "binary", "ping", "pong",
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
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir);
static void openWebSock(HttpQueue *q);
static void outgoingWebSockService(HttpQueue *q);
static void readyWebSock(HttpQueue *q);
static bool validUTF8(cchar *str, ssize len);
static void webSockPing(HttpConn *conn);

/*********************************** Code *************************************/
/* 
   Loadable module initialization
 */
PUBLIC int httpOpenWebSockFilter(Http *http)
{
    HttpStage     *filter;

    mprAssert(http);

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
    Match if the filter is required for this request. This is called twice: once for TX and once for RX.
 */
static int matchWebSock(HttpConn *conn, HttpRoute *route, int dir)
{
    HttpRx      *rx;
    HttpTx      *tx;
    char        *kind, *tok;

    mprAssert(conn);
    mprAssert(route);

    rx = conn->rx;
    tx = conn->tx;
    mprAssert(rx);
    mprAssert(tx);

    if (!conn->endpoint && tx->parsedUri && tx->parsedUri->webSockets) {
        /* ws:// URI. Client web sockets */
        return HTTP_ROUTE_OK;
    }
    /*
        Deliberately not checking Origin as it offers illusory security
     */
    if (!smatch(rx->method, "GET") || !rx->hostHeader || !rx->upgrade || !rx->webSockKey || !rx->webSockVersion) {
        return HTTP_ROUTE_REJECT;
    }
    if (dir & HTTP_STAGE_RX) {
        if (rx->upgrade && scaselessmatch(rx->upgrade, "websocket")) {
            if (!rx->webSockKey) {
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Bad Sec-WebSocket-Key");
                return HTTP_ROUTE_REJECT;
            }
            if (rx->webSockVersion < WS_VERSION) {
                httpSetHeader(conn, "Sec-WebSocket-Version", "%d", WS_VERSION);
                httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Version");
                return HTTP_ROUTE_REJECT;
            }
            /* Just select the first protocol */
            if (route->webSocketsProtocol) {
                for (kind = stok(sclone(rx->webSockProtocols), " \t,", &tok); kind; kind = stok(NULL, " \t,", &tok)) {
                    if (smatch(route->webSocketsProtocol, kind)) {
                        break;
                    }
                }
                if (!kind) {
                    httpError(conn, HTTP_ABORT | HTTP_CODE_BAD_REQUEST, "Unsupported Sec-WebSocket-Protocol");
                    return HTTP_ROUTE_REJECT;
                }
                conn->rx->subProtocol = sclone(kind);
            } else {
                /* Just pick the first protocol */
                conn->rx->subProtocol = stok(sclone(rx->webSockProtocols), " ,", NULL);
            }
            httpSetStatus(conn, HTTP_CODE_SWITCHING);
            httpSetHeader(conn, "Connection", "Upgrade");
            httpSetHeader(conn, "Upgrade", "WebSocket");
            httpSetHeader(conn, "Sec-WebSocket-Accept", mprGetSHABase64(sjoin(rx->webSockKey, WS_MAGIC, NULL)));
            httpSetHeader(conn, "Sec-WebSocket-Protocol", conn->rx->subProtocol);
            httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
            httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);

            if (route->webSocketsPingPeriod) {
                rx->pingEvent = mprCreateEvent(conn->dispatcher, "webSocket", route->webSocketsPingPeriod, 
                    webSockPing, conn, MPR_EVENT_CONTINUOUS);
            }
            conn->keepAliveCount = -1;
            conn->upgraded = 1;
            rx->eof = 0;
            rx->remainingContent = MAXINT;
            return HTTP_ROUTE_OK;
        }
    } else if (conn->upgraded) {
        return HTTP_ROUTE_OK;
    }
    return HTTP_ROUTE_REJECT;
}


static void webSockPing(HttpConn *conn)
{
    mprAssert(conn->rx);
    httpSendBlock(conn, WS_MSG_PING, NULL, 0, 1);
}


static void webSockTimeout(HttpConn *conn)
{
    httpSendClose(conn, WS_STATUS_POLICY_VIOLATION, "Request timeout");
}


static void openWebSock(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;

    mprAssert(q);
    mprLog(5, "webSocketFilter: Open WebSocket filter");
    conn = q->conn;
    q->packetSize = min(conn->limits->stageBufferSize, q->max);
    conn->rx->closeStatus = WS_STATUS_NO_STATUS;
    conn->timeoutCallback = webSockTimeout;

    if ((packet = httpGetPacket(conn->writeq)) != 0) {
        mprAssert(packet->flags & HTTP_PACKET_HEADER);
        httpPutForService(q, packet, HTTP_SCHEDULE_QUEUE);
    }
    conn->tx->responded = 0;
}

static void closeWebSock(HttpQueue *q)
{
    HttpRx  *rx;

    rx = q->conn->rx;
    if (rx && rx->pingEvent) {
        mprRemoveEvent(rx->pingEvent);
        rx->pingEvent = 0;
    }
}


static void readyWebSock(HttpQueue *q)
{
    if (q->conn->endpoint) {
        HTTP_NOTIFY(q->conn, HTTP_EVENT_APP_OPEN, 0);
    }
}


static int processPacket(HttpQueue *q, HttpPacket *packet)
{
    HttpRx      *rx;
    HttpConn    *conn;
    MprBuf      *content;
    char        *cp;

    conn = q->conn;
    rx = conn->rx;
    content = packet->content;
    mprAssert(content);
    mprLog(4, "webSocketFilter: Process packet \"%s\", data length %d", codetxt[rx->opcode & 0xf], mprGetBufLength(content));

    switch (rx->opcode) {
    case OP_BINARY:
    case OP_TEXT:
        if (rx->maskOffset >= 0) {
            if (rx->opcode == OP_TEXT) {
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ rx->dataMask[rx->maskOffset++ & 0x3];
                }
            } else {
                for (cp = content->start; cp < content->end; cp++) {
                    *cp = *cp ^ rx->dataMask[rx->maskOffset++ & 0x3];
                }
            }
        } 
        if (rx->opcode == OP_TEXT && !validUTF8(content->start, mprGetBufLength(content))) {
            if (!rx->route->ignoreEncodingErrors) {
                mprError("webSocketFilter: Text packet has invalid UTF8");
                return WS_STATUS_INVALID_UTF8;
            }
        }
        packet->type = (rx->opcode == OP_TEXT) ? WS_MSG_TEXT : WS_MSG_BINARY;
        if (rx->opcode == OP_TEXT) {
            mprLog(5, "webSocketFilter: Text packet \"%s\"", content->start);
        }
        if (packet->last) {
            /* Preserve packet boundaries */
            packet->flags |= HTTP_PACKET_SOLO;
            httpPutPacketToNext(q, packet);
            httpServiceQueues(q->conn);
            rx->currentPacket = 0;
        }
        return 0;

    case OP_CLOSE:
        cp = content->start;
        if (httpGetPacketLength(packet) >= 2) {
            rx->closeStatus = ((uchar) cp[0]) << 8 | (uchar) cp[1];
            if (httpGetPacketLength(packet) >= 4) {
                mprAddNullToBuf(content);
                if (rx->maskOffset >= 0) {
                    for (cp = content->start; cp < content->end; cp++) {
                        *cp = *cp ^ rx->dataMask[rx->maskOffset++ & 0x3];
                    }
                }
                rx->closeReason = sclone(&content->start[2]);
            }
        }
        mprLog(5, "webSocketFilter: close status %d, reason \"%s\", closing %d", rx->closeStatus, 
                rx->closeReason, rx->closing);
        if (rx->closing) {
            httpDisconnect(conn);
        } else {
            /* Acknowledge the close. Echo the received status */
            httpSendClose(conn, WS_STATUS_OK, NULL);
            rx->eof = 1;
        }
        /* Advance from the content state */
        httpSetState(conn, HTTP_STATE_READY);
        rx->webSockState = WS_STATE_CLOSED;
        return 0;

    case OP_PING:
        httpSendBlock(conn, WS_MSG_PONG, mprGetBufStart(content), mprGetBufLength(content), 1);
        return 0;

    case OP_PONG:
        /* Do nothing */
        return 0;

    default:
        mprError("webSocketFilter: Bad frame type %d", rx->opcode);
        break;
    }
    /* Should not get here */
    rx->webSockState = WS_STATE_CLOSED;
    return WS_STATUS_PROTOCOL_ERROR;
}


static void incomingWebSockData(HttpQueue *q, HttpPacket *packet)
{
    HttpConn    *conn;
    HttpRx      *rx;
    HttpPacket  *tail;
    HttpLimits  *limits;
    char        *fp;
    ssize       len;
    int         i, error, mask, lenBytes;

    conn = q->conn;
    rx = conn->rx;
    limits = conn->limits;
    mprAssert(packet);
    mprLog(4, "webSocketFilter: incoming data. Packet type: %d", packet->type);

    if (packet->flags & HTTP_PACKET_END) {
        /* EOF packet means the socket has been abortively closed */
        rx->closing = 1;
        rx->frameState = WS_CLOSED;
        rx->webSockState = WS_STATE_CLOSED;
        rx->closeStatus = WS_STATUS_COMMS_ERROR;
    }
    while (1) {
        mprLog(5, "webSocketFilter: frame state %d", rx->frameState);
        error = 0;
        switch (rx->frameState) {
        case WS_CLOSED:
            mprLog(5, "webSocketFilter: incoming closed. Finalizing");
            HTTP_NOTIFY(conn, HTTP_EVENT_APP_CLOSE, rx->closeStatus);
            /* Finalize for safety. The handler/callback should have done this above */
            httpComplete(conn);
            return;

        case WS_BEGIN:
            if (httpGetPacketLength(packet) < 2) {
                /* Need more data */
                return;
            }
            fp = packet->content->start;
            if (GET_RSV(*fp) != 0) {
                error = WS_STATUS_PROTOCOL_ERROR;
                break;
            }
            packet->last = GET_FIN(*fp);
            rx->opcode = GET_CODE(*fp);
            fp++;
            len = GET_LEN(*fp);
            mask = GET_MASK(*fp);
            lenBytes = 1;
            if (len == 126) {
                lenBytes += 2;
            } else if (len == 127) {
                lenBytes += 8;
            }
            if (httpGetPacketLength(packet) < (lenBytes + (mask * 4))) {
                /* Return if we don't have the required packet control fields */
                return;
            }
            fp++;
            if (rx->opcode) {
                if (rx->opcode < OP_CONTROL) {
                    rx->messageCode = rx->opcode;
                } else {
                    /* Control frame, must not be fragmented */
                    if (!packet->last) {
                        error = WS_STATUS_PROTOCOL_ERROR;
                        break;
                    }
                }
            }
            while (--lenBytes > 0) {
                len <<= 8;
                len += (uchar) *fp++;
            }
            rx->frameLength = len;
            rx->frameState = WS_MSG;
            rx->maskOffset = mask ? 0 : -1;
            if (mask) {
                for (i = 0; i < 4; i++) {
                    rx->dataMask[i] = *fp++;
                }
            }
            mprAssert(packet->content);
            mprAdjustBufStart(packet->content, fp - packet->content->start);
            rx->frameState = WS_MSG;
            mprLog(5, "webSocketFilter: Begin new packet \"%s\", last %d, mask %d, length %d", codetxt[rx->opcode & 0xf],
                packet->last, mask, len);
            break;

#if UNUSED && KEEP && FUTURE
        case WS_EXT_DATA:
            mprAssert(packet);
            mprLog(5, "webSocketFilter: EXT DATA - RESERVED");
            rx->frameState = WS_MSG;
            break;
#endif

        case WS_MSG:
            mprAssert(packet);
            len = min(httpGetPacketLength(packet), rx->frameLength);

            if (packet->type == WS_MSG_TEXT || packet->type == WS_MSG_BINARY) {
                if (rx->currentPacket) {
                    mprLog(6, "webSocketFilter: Joining data packet %d/%d", httpGetPacketLength(rx->currentPacket),
                        httpGetPacketLength(packet));
                    if (rx->currentPacket->type != packet->type) {
                        mprError("WebSocket has frames of types: %d and %d", rx->currentPacket->type, packet->type);
                        rx->currentPacket = packet = 0;
                        error = WS_STATUS_UNSUPPORTED_TYPE;
                    } else {
                        /*
                            Join message packets. Note control packets can come between frames for a single message.
                         */
                        if (packet->type == WS_MSG_TEXT || packet->type == WS_MSG_BINARY) {
                            httpJoinPacket(rx->currentPacket, packet);
                            packet = rx->currentPacket;
                        }
                    }
                } else {
                    rx->currentPacket = packet;
                }
                if (httpGetPacketLength(rx->currentPacket) > conn->limits->webSocketsMessageSize) {
                    error = WS_STATUS_MESSAGE_TOO_LARGE;
                    mprError("webSocketFilter: Incoming message is too large %d/%d", len, limits->webSocketsMessageSize);
                    packet = rx->currentPacket = 0;
                    error = WS_STATUS_MESSAGE_TOO_LARGE;
                }
            }

            /*
                Split packet if it contains data for the next frame. Do this even if discarding a frame.
             */
            tail = 0;
            if (httpGetPacketLength(packet) > rx->frameLength) {
                if ((tail = httpSplitPacket(packet, rx->frameLength)) != 0) {
                    httpPutBackPacket(q, tail);
                    mprLog(6, "webSocketFilter: Split data packet, %d/%d", rx->frameLength, httpGetPacketLength(tail));
                }
            }
            if (error || rx->closing) {
                /* Discard message if closing */
                rx->frameState = WS_BEGIN;
            } else {
                /*
                    Wait till we have the final frame in a message before processing, unless too largen in which case
                    the message will be split over multiple packets. The final packet has packet->last set.
                 */
                len = httpGetPacketLength(packet);
                if ((packet->last && len == rx->frameLength) || len >= limits->webSocketsPacketSize) {
                    if (len >= limits->webSocketsPacketSize) {
                        mprError("webSocketFilter: Packet size exceeds limit %d/%d", len, limits->webSocketsPacketSize); 
                        error = WS_STATUS_MESSAGE_TOO_LARGE;
                    } else {
                        error = processPacket(q, packet);
                        rx->frameState = (rx->webSockState == WS_STATE_CLOSED) ? WS_CLOSED : WS_BEGIN;
                    }
                } else if (!tail) {
                    break;
                }
            }
            packet = tail;
        }
        if (error) {
            mprError("webSocketFilter: WebSockets error Status %d", error);
            httpSendClose(conn, error, NULL);
            rx->frameState = WS_CLOSED;
            rx->webSockState = WS_STATE_CLOSED;
            HTTP_NOTIFY(conn, HTTP_EVENT_ERROR, error);
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
    return httpSendBlock(conn, WS_MSG_TEXT, buf, slen(buf), 1);
}


/*
    Send a block of data with the specified message type. Set last to true for the last block of a logical message.
 */
PUBLIC ssize httpSendBlock(HttpConn *conn, int type, cchar *buf, ssize len, bool last)
{
    HttpPacket  *packet;
    ssize       thisLen;

    if (len < 0) {
        len = slen(buf);
    }
    if (len > conn->limits->webSocketsMessageSize) {
        mprError("webSocketFilter: Outgoing message is too large %d/%d", len, conn->limits->webSocketsMessageSize);
        return MPR_ERR_WONT_FIT;
    }
    mprLog(5, "webSocketFilter: Sending message \"%s\", len %d", wscodetxt[type & 0xf], len);
    while (len > 0) {
        /*
            Break into frames. Note: downstream may also fragment packets.
         */
        thisLen = min(len, conn->limits->webSocketsFrameSize);
        if ((packet = httpCreateDataPacket(thisLen)) == 0) {
            return MPR_ERR_MEMORY;
        }
        packet->type = type;
        packet->last = (thisLen == len) ? last : 0;
        if (mprPutBlockToBuf(packet->content, buf, len) != len) {
            return MPR_ERR_MEMORY;
        }
        len -= thisLen;
        httpPutForService(conn->writeq, packet, HTTP_SCHEDULE_QUEUE);
    }
    httpServiceQueues(conn);
    return len;
}


/*
    The reason string is optional
 */
PUBLIC void httpSendClose(HttpConn *conn, int status, cchar *reason)
{
    HttpRx      *rx;
    char        msg[128];
    ssize       len;

    rx = conn->rx;
    if (rx->closing) {
        return;
    }
    rx->closing = 1;
    rx->webSockState = WS_STATE_CLOSING;
    len = 2;
    if (reason) {
        if (slen(reason) >= 124) {
            reason = "Web sockets close reason message was too big";
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
    httpSendBlock(conn, WS_MSG_CLOSE, msg, len, 1);
}


static void outgoingWebSockService(HttpQueue *q)
{
    HttpConn    *conn;
    HttpPacket  *packet;
    char        *ep, *fp, *prefix, dataMask[4];
    ssize       len;
    int         i, mask, code;

    conn = q->conn;
    mprLog(6, "webSocketFilter: outgoing service");

    for (packet = httpGetPacket(q); packet; packet = httpGetPacket(q)) {
        if (!(packet->flags & (HTTP_PACKET_END | HTTP_PACKET_HEADER))) {
            httpResizePacket(q, packet, conn->limits->stageBufferSize);
            if (!httpWillNextQueueAcceptPacket(q, packet)) {
                httpPutBackPacket(q, packet);
                return;
            }
            len = httpGetPacketLength(packet);
            packet->prefix = mprCreateBuf(16, 16);
            code = opcodes[packet->type & 0x7];
            prefix = packet->prefix->start;
            mask = conn->endpoint ? 0 : 1;
            *prefix++ = SET_FIN(packet->last) | SET_CODE(code);
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


PUBLIC char *httpGetWebSocketProtocol(HttpConn *conn)
{
    return conn->rx->subProtocol;
}


PUBLIC char *httpGetWebSocketCloseReason(HttpConn *conn)
{
    return conn->rx->closeReason;
}


PUBLIC bool httpWebSocketOrderlyClosed(HttpConn *conn)
{
    return conn->rx->closeStatus != WS_STATUS_COMMS_ERROR;
}


PUBLIC void httpSetWebSocketProtocols(HttpConn *conn, cchar *protocols)
{
    assure(protocols && *protocols);
    conn->protocols = sclone(protocols);
}


static bool validUTF8(cchar *str, ssize len)
{
    cuchar      *cp, *end;
    int         nbytes, i;
  
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
        mprAssert(nbytes >= 1);
    } 
    return 1;
}


/*
    Upgrade a client socket to use Web Sockets
 */
PUBLIC int httpUpgradeWebSocket(HttpConn *conn)
{
    char    num[16];

    mprLog(5, "webSocketFilter: Upgrade socket");
    httpSetStatus(conn, HTTP_CODE_SWITCHING);
    httpSetHeader(conn, "Upgrade", "websocket");
    httpSetHeader(conn, "Connection", "Upgrade");
    mprGetRandomBytes(num, sizeof(num), 0);
    conn->tx->webSockKey = mprEncode64Block(num, sizeof(num));
    httpSetHeader(conn, "Sec-WebSocket-Key", conn->tx->webSockKey);
    httpSetHeader(conn, "Sec-WebSocket-Protocol", conn->protocols ? conn->protocols : "chat");
    httpSetHeader(conn, "Sec-WebSocket-Version", "13");
    httpSetHeader(conn, "X-Request-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);
    httpSetHeader(conn, "X-Inactivity-Timeout", "%Ld", conn->limits->requestTimeout / MPR_TICKS_PER_SEC);

    conn->upgraded = 1;
    conn->keepAliveCount = -1;
    conn->rx->remainingContent = MAXINT;
    conn->rx->webSockState = WS_STATE_CONNECTING;
    return 0;
}


PUBLIC bool httpVerifyWebSocketsHandshake(HttpConn *conn)
{
    HttpRx          *rx;
    HttpTx          *tx;
    cchar           *key, *expected;

    rx = conn->rx;
    tx = conn->tx;

    if (rx->status != HTTP_CODE_SWITCHING) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake status %d", rx->status);
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "Connection"), "Upgrade")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Connection header");
        return 0;
    }
    if (!smatch(httpGetHeader(conn, "Upgrade"), "WebSocket")) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket Upgrade header");
        return 0;
    }
    expected = mprGetSHABase64(sjoin(tx->webSockKey, WS_MAGIC, NULL));
    key = httpGetHeader(conn, "Sec-WebSocket-Accept");
    if (!smatch(key, expected)) {
        httpError(conn, HTTP_CODE_BAD_HANDSHAKE, "Bad WebSocket handshake key\n%s\n%s", key, expected);
        return 0;
    }
    mprLog(4, "WebSockets handsake verified");
    conn->rx->webSockState = WS_STATE_OPEN;
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
