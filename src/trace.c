/*
    trace.c -- Trace data
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    Event types and trace levels:
    0: debug
    1: request
    2: error, result
    3: context
    4: packet
    5: detail
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void manageTrace(HttpTrace *trace, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(trace->buf);
        mprMark(trace->events);
        mprMark(trace->file);
        mprMark(trace->format);
        mprMark(trace->lastTime);
        mprMark(trace->mutex);
        mprMark(trace->parent);
        mprMark(trace->path);
    }
}

/*
    Parent may be null
 */
PUBLIC HttpTrace *httpCreateTrace(HttpTrace *parent)
{
    HttpTrace   *trace;

    if ((trace = mprAllocObj(HttpTrace, manageTrace)) == 0) {
        return 0;
    }
    if (parent) {
        *trace = *parent;
        trace->parent = parent;
    } else {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return 0;
        }
        mprAddKey(trace->events, "debug", ITOP(0));
        mprAddKey(trace->events, "request", ITOP(1));
        mprAddKey(trace->events, "error", ITOP(2));
        mprAddKey(trace->events, "result", ITOP(2));
        mprAddKey(trace->events, "context", ITOP(3));
        mprAddKey(trace->events, "packet", ITOP(4));
        mprAddKey(trace->events, "detail", ITOP(5));

        /*
            Max log file size
         */
        trace->size = HTTP_TRACE_MAX_SIZE;
        trace->maxContent = MAXINT;
        trace->formatter = httpDetailTraceFormatter;
        trace->logger = httpWriteTraceLogFile;
        trace->mutex = mprCreateLock();
    }
    return trace;
}


PUBLIC void httpSetTraceContentSize(HttpTrace *trace, ssize size)
{
    trace->maxContent = size;
}


PUBLIC void httpSetTraceEventLevel(HttpTrace *trace, cchar *type, int level)
{
    assert(trace);
    mprAddKey(trace->events, type, ITOP(level));
}


PUBLIC int httpGetTraceLevel()
{
    return HTTP->traceLevel;
}


PUBLIC void httpSetTraceFormat(HttpTrace *trace, cchar *format)
{
    trace->format = sclone(format);
}


PUBLIC HttpTraceFormatter httpSetTraceFormatter(HttpTrace *trace, HttpTraceFormatter callback)
{
    HttpTraceFormatter  prior;

    prior = trace->formatter;
    trace->formatter = callback;
    return prior;
}


PUBLIC void httpSetTraceFormatterName(HttpTrace *trace, cchar *name)
{
    HttpTraceFormatter  formatter;

    if (name && smatch(name, "common")) {
        if ((trace->events = mprCreateHash(0, MPR_HASH_STATIC_VALUES)) == 0) {
            return;
        }
        mprAddKey(trace->events, "result", ITOP(0));
        formatter = httpCommonTraceFormatter;

#if FUTURE
    } else if (smatch(name, "simple")) {
       formatter = httpSimpleTraceFormatter;
#endif

    } else {
       formatter = httpDetailTraceFormatter;
    }
    httpSetTraceFormatter(trace, formatter);
}


PUBLIC void httpSetTraceLevel(int level)
{
    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    HTTP->traceLevel = level;
}


PUBLIC void httpSetTraceLogger(HttpTrace *trace, HttpTraceLogger callback)
{
    trace->logger = callback;
}


/*
    Inner routine for httpTrace()
 */
PUBLIC bool httpLogProc(HttpTrace *trace, cchar *event, cchar *type, int flags, cchar *fmt, ...)
{
    va_list     args;

    assert(event && *event);
    assert(type && *type);

    va_start(args, fmt);
    httpFormatTrace(trace, event, type, flags, NULL, 0, fmt, args);
    va_end(args);
    return 1;
}


PUBLIC bool httpTracePacket(HttpTrace *trace, cchar *event, cchar *type, int flags, HttpPacket *packet, cchar *fmt, ...)
{
    va_list     args;
    int         level;

    assert(packet);

    if (!trace || !packet) {
        return 0;
    }
    level = PTOI(mprLookupKey(trace->events, type));
    if (level > HTTP->traceLevel) { \
        return 0;
    }
    va_start(args, fmt);
    httpFormatTrace(trace, event, type, flags | HTTP_TRACE_PACKET, (char*) packet, 0, fmt, args);
    va_end(args);
    return 1;
}


/*
    Trace request body data
 */
PUBLIC bool httpTraceData(HttpTrace *trace, cchar *event, cchar *type, int flags, cchar *buf, ssize len, cchar *fmt, ...)
{
    Http        *http;
    va_list     args;
    int         level;

    assert(trace);
    assert(buf);

    http = HTTP;
    if (http->traceLevel == 0) {
        return 0;
    }
    level = PTOI(mprLookupKey(trace->events, type));
    if (level > http->traceLevel) {
        return 0;
    }
    va_start(args, fmt);
    httpFormatTrace(trace, event, type, flags, buf, len, fmt, args);
    va_end(args);
    return 1;
}


/*
    Format and emit trace
 */
PUBLIC void httpFormatTrace(HttpTrace *trace, cchar *event, cchar *type, int flags, cchar *buf, ssize len, cchar *fmt, va_list args)
{
    (trace->formatter)(trace, event, type, flags, buf, len, fmt, args);
}


/*
    Low-level write routine to be used only by formatters
 */
PUBLIC void httpWriteTrace(HttpTrace *trace, cchar *buf, ssize len)
{
    (trace->logger)(trace, buf, len);
}


/*
    Format a detailed request message
 */
PUBLIC void httpDetailTraceFormatter(HttpTrace *trace, cchar *event, cchar *type, int flags, cchar *data, ssize len, cchar *fmt, va_list args)
{
    HttpPacket  *packet;
    MprBuf      *buf;
    MprTime     now;
    char        *msg;
    bool        hex;

    assert(trace);
    lock(trace);

    hex = (trace->flags & HTTP_TRACE_HEX) ? 1 : 0;

    if (!trace->buf) {
        trace->buf = mprCreateBuf(0, 0);
    }
    buf = trace->buf;
    mprFlushBuf(buf);

    now = mprGetTime();
    if (trace->lastMark < (now + TPS) || trace->lastTime == 0) {
        trace->lastTime = mprGetDate("%T");
        trace->lastMark = now;
    }

    if (event && type) {
        if (scontains(event, ".tx")) {
            mprPutToBuf(buf, "%s SEND event=%s type=%s", trace->lastTime, event, type);
        } else {
            mprPutToBuf(buf, "%s RECV event=%s type=%s", trace->lastTime, event, type);
        }
    }
    if (fmt) {
        mprPutCharToBuf(buf, ' ');
        msg = sfmtv(fmt, args);
        mprPutStringToBuf(buf, msg);
    }
    if (fmt || event || type) {
        mprPutStringToBuf(buf, "\n");
    }
    if (flags & HTTP_TRACE_PACKET) {
        packet = (HttpPacket*) data;
        if (packet->prefix) {
            len = mprGetBufLength(packet->prefix);
            data = httpMakePrintable(trace, packet->prefix->start, &hex, &len);
            mprPutBlockToBuf(buf, data, len);
        }
        if (packet->content) {
            len = mprGetBufLength(packet->content);
            data = httpMakePrintable(trace, packet->content->start, &hex, &len);
            mprPutBlockToBuf(buf, data, len);
        }
        mprPutStringToBuf(buf, "\n");
    } else if (data && len > 0) {
        data = httpMakePrintable(trace, data, &hex, &len);
        mprPutBlockToBuf(buf, data, len);
        mprPutStringToBuf(buf, "\n");
    }
    httpWriteTrace(trace, mprGetBufStart(buf), mprGetBufLength(buf));
    unlock(trace);
}


#if FUTURE
PUBLIC void httpSimpleTraceFormatter(HttpTrace *trace, cchar *event, cchar *type, int flags cchar *data, ssize len, cchar *fmt, va_list args)
{
    MprBuf      *buf;
    char        *msg;
    bool        hex;

    assert(trace);
    assert(event);
    assert(type);

    lock(trace);
    if (!trace->buf) {
        trace->buf = mprCreateBuf(0, 0);
    }
    buf = trace->buf;
    mprFlushBuf(buf);

    if (data && len > 0) {
        hex = 0;
        data = httpMakePrintable(trace, data, &hex, &len);
        mprPutBlockToBuf(buf, data, len);
    }
    mprPutToBuf(buf, "%s %s", event, type);
    if (fmt) {
        mprPutCharToBuf(buf, ' ');
        msg = sfmtv(fmt, args);
        mprPutStringToBuf(buf, msg);
    }
    mprPutStringToBuf(buf, "\n");

    httpWriteTrace(trace, mprGetBufStart(buf), mprGetBufLength(buf));
    unlock(trace);
}
#endif


/*
    Common Log Formatter (NCSA)
    This formatter only emits messages only for connections at their complete event.
 */
PUBLIC void httpCommonTraceFormatter(HttpTrace *trace, cchar *type, cchar *event, int flags, cchar *data, ssize len, cchar *msg, va_list args)
{
    HttpStream  *stream;
    HttpRx      *rx;
    HttpTx      *tx;
    MprBuf      *buf;
    cchar       *fmt, *cp, *qualifier, *timeText, *value;
    char        c, keyBuf[256];
    int         buflen;

    assert(trace);
    assert(type && *type);
    assert(event && *event);

    stream = (HttpStream*) data;
    if (!stream || len != 0) {
        return;
    }
    if (!smatch(event, "result")) {
        return;
    }
    rx = stream->rx;
    tx = stream->tx;
    fmt = trace->format;
    if (fmt == 0 || fmt[0] == '\0') {
        fmt = ME_HTTP_LOG_FORMAT;
    }
    buflen = ME_MAX_URI + 256;
    buf = mprCreateBuf(buflen, buflen);

    while ((c = *fmt++) != '\0') {
        if (c != '%' || (c = *fmt++) == '%') {
            mprPutCharToBuf(buf, c);
            continue;
        }
        switch (c) {
        case 'a':                           /* Remote IP */
            mprPutStringToBuf(buf, stream->ip);
            break;

        case 'A':                           /* Local IP */
            mprPutStringToBuf(buf, stream->sock->listenSock->ip);
            break;

        case 'b':
            if (tx->bytesWritten == 0) {
                mprPutCharToBuf(buf, '-');
            } else {
                mprPutIntToBuf(buf, tx->bytesWritten);
            }
            break;

        case 'B':                           /* Bytes written (minus headers) */
            mprPutIntToBuf(buf, (tx->bytesWritten - tx->headerSize));
            break;

        case 'h':                           /* Remote host */
            mprPutStringToBuf(buf, stream->ip);
            break;

        case 'l':                           /* user identity - unknown */
            mprPutCharToBuf(buf, '-');
            break;

        case 'n':                           /* Local host */
            mprPutStringToBuf(buf, rx->parsedUri->host);
            break;

        case 'O':                           /* Bytes written (including headers) */
            mprPutIntToBuf(buf, tx->bytesWritten);
            break;

        case 'r':                           /* First line of request */
            mprPutToBuf(buf, "%s %s %s", rx->method, rx->uri, httpGetProtocol(stream->net));
            break;

        case 's':                           /* Response code */
            mprPutIntToBuf(buf, tx->status);
            break;

        case 't':                           /* Time */
            mprPutCharToBuf(buf, '[');
            timeText = mprFormatLocalTime(MPR_DEFAULT_DATE, mprGetTime());
            mprPutStringToBuf(buf, timeText);
            mprPutCharToBuf(buf, ']');
            break;

        case 'u':                           /* Remote username */
            mprPutStringToBuf(buf, stream->username ? stream->username : "-");
            break;

        case '{':                           /* Header line "{header}i" */
            qualifier = fmt;
            if ((cp = schr(qualifier, '}')) != 0) {
                fmt = &cp[1];
                switch (*fmt++) {
                case 'i':
                    sncopy(keyBuf, sizeof(keyBuf), qualifier, cp - qualifier);
                    value = (char*) mprLookupKey(rx->headers, keyBuf);
                    mprPutStringToBuf(buf, value ? value : "-");
                    break;
                default:
                    mprPutSubStringToBuf(buf, qualifier, qualifier - cp);
                }

            } else {
                mprPutCharToBuf(buf, c);
            }
            break;

        case '>':
            if (*fmt == 's') {
                fmt++;
                mprPutIntToBuf(buf, tx->status);
            }
            break;

        default:
            mprPutCharToBuf(buf, c);
            break;
        }
    }
    mprPutCharToBuf(buf, '\n');
    httpWriteTrace(trace, mprBufToString(buf), mprGetBufLength(buf));
}

/************************************** TraceLogFile **************************/

static int backupTraceLogFile(HttpTrace *trace)
{
    MprPath     info;

    assert(trace->path);

    if (trace->file == MPR->logFile) {
        return 0;
    }
    if (trace->backupCount > 0 || (trace->flags & MPR_LOG_ANEW)) {
        lock(trace);
        if (trace->path && trace->parent && smatch(trace->parent->path, trace->path)) {
            unlock(trace);
            return backupTraceLogFile(trace->parent);
        }
        mprGetPathInfo(trace->path, &info);
        if (info.valid && ((trace->flags & MPR_LOG_ANEW) || info.size > trace->size)) {
            if (trace->file) {
                mprCloseFile(trace->file);
                trace->file = 0;
            }
            if (trace->backupCount > 0) {
                mprBackupLog(trace->path, trace->backupCount);
            }
        }
        unlock(trace);
    }
    return 0;
}


/*
    Open the request log file
 */
PUBLIC int httpOpenTraceLogFile(HttpTrace *trace)
{
    MprFile     *file;
    int         mode;

    if (!trace->file && trace->path) {
        if (smatch(trace->path, "-")) {
            file = MPR->logFile;
        } else {
            backupTraceLogFile(trace);
            mode = O_CREAT | O_WRONLY | O_TEXT;
            mode |= (trace->flags & MPR_LOG_ANEW) ? O_TRUNC : O_APPEND;
            if (smatch(trace->path, "stdout")) {
                file = MPR->stdOutput;
            } else if (smatch(trace->path, "stderr")) {
                file = MPR->stdError;
            } else if ((file = mprOpenFile(trace->path, mode, 0664)) == 0) {
                mprLog("error http trace", 0, "Cannot open log file %s", trace->path);
                return MPR_ERR_CANT_OPEN;
            }
        }
        trace->file = file;
        trace->flags &= ~MPR_LOG_ANEW;
    }
    return 0;
}


/*
    Start tracing when instructed via a command line option.
 */
PUBLIC int httpStartTracing(cchar *traceSpec)
{
    HttpTrace   *trace;
    char        *lspec;

    if (HTTP == 0 || HTTP->trace == 0 || traceSpec == 0 || *traceSpec == '\0') {
        assert(HTTP);
        return MPR_ERR_BAD_STATE;
    }
    trace = HTTP->trace;
    trace->flags = MPR_LOG_ANEW | MPR_LOG_CMDLINE;
    trace->path = stok(sclone(traceSpec), ":", &lspec);
    HTTP->traceLevel = (int) stoi(lspec);
    return httpOpenTraceLogFile(trace);
}


/*
    Configure the trace log file
 */
PUBLIC int httpSetTraceLogFile(HttpTrace *trace, cchar *path, ssize size, int backup, cchar *format, int flags)
{
    assert(trace);
    assert(path && *path);

    if (format == NULL || *format == '\0') {
        format = ME_HTTP_LOG_FORMAT;
    }
    trace->backupCount = backup;
    trace->flags = flags;
    trace->format = sclone(format);
    trace->size = size;
    trace->path = sclone(path);
    return httpOpenTraceLogFile(trace);
}


/*
    Write a message to the trace log
 */
PUBLIC void httpWriteTraceLogFile(HttpTrace *trace, cchar *buf, ssize len)
{
    static int  skipCheck = 0;

    lock(trace);
    if (trace->backupCount > 0) {
        if ((++skipCheck % 50) == 0) {
            backupTraceLogFile(trace);
        }
    }
    if (!trace->file && trace->path && httpOpenTraceLogFile(trace) < 0) {
        unlock(trace);
        return;
    }
	if (trace->file) {
		mprWriteFile(trace->file, buf, len);
	}
    unlock(trace);
}


/*
    Get a printable version of a buffer. Return a pointer to the start of printable data.
    This will use the tx or rx mime type if possible.
    Skips UTF encoding prefixes
 */
PUBLIC cchar *httpMakePrintable(HttpTrace *trace, cchar *buf, bool *hex, ssize *lenp)
{
    cchar   *start, *cp, *digits, *sol;
    char    *data, *dp;
    ssize   len, bsize, lines;
    int     i, j;

    start = buf;
    len = *lenp;
    if (len > 3 && start[0] == (char) 0xef && start[1] == (char) 0xbb && start[2] == (char) 0xbf) {
        /* Step over UTF encoding */
        start += 3;
        *lenp -= 3;
    }
    for (i = 0; i < len; i++) {
        if (*hex || (!isprint((uchar) start[i]) && start[i] != '\n' && start[i] != '\r' && start[i] != '\t')) {
            /*
                Round up lines, 4 chars per byte plush 3 chars per line (||\n)
             */
            lines = len / 16 + 1;
            bsize = ((lines * 16) * 4) + (lines * 5) + 2;
            data = mprAlloc(bsize);
            digits = "0123456789ABCDEF";
            for (i = 0, cp = start, dp = data; cp < &start[len]; ) {
                sol = cp;
                for (j = 0; j < 16 && cp < &start[len]; j++, cp++) {
                    *dp++ = digits[(*cp >> 4) & 0x0f];
                    *dp++ = digits[*cp & 0x0f];
                    *dp++ = ' ';
                }
                for (; j < 16; j++) {
                    *dp++ = ' '; *dp++ = ' '; *dp++ = ' ';
                }
                *dp++ = ' '; *dp++ = ' '; *dp++ = '|';
                for (j = 0, cp = sol; j < 16 && cp < &start[len]; j++, cp++) {
                    *dp++ = isprint(*cp) ? *cp : '.';
                }
                for (; j < 16; j++) {
                    *dp++ = ' ';
                }
                *dp++ = '|';
                *dp++ = '\n';
                assert((dp - data) <= bsize);
            }
            *dp = '\0';
            assert((dp - data) <= bsize);
            start = data;
            *lenp = dp - start;
            *hex = 1;
            break;
        }
    }
    return start;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
