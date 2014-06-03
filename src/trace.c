/*
    trace.c -- Trace data
    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    - How to record timestamps in log file?
 */

/********************************* Includes ***********************************/

#include    "http.h"

/*********************************** Code *************************************/

static void manageTrace(HttpTrace *trace, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(trace->file);
        mprMark(trace->format);
        mprMark(trace->mutex);
        mprMark(trace->path);
    }
}


/*
    Initialize trace to default levels:
    Levels 0-5: Normal numeric trace levels
    Level 2: rx first line, errors
    Level 3: rx headers, tx headers
    Level 4: info, time measurements
    Level 5: rx/tx body
 */
PUBLIC HttpTrace *httpCreateTrace(HttpTrace *parent)
{
    HttpTrace   *trace;
    char        *levels;
    int         i;

    if ((trace = mprAllocObj(HttpTrace, manageTrace)) == 0) {
        return 0;
    }
    if (parent) {
        *trace = *parent;
        trace->parent = parent;
    } else {
        levels = trace->levels;
        for (i = 0; i <= HTTP_TRACE_5; i++) {
            levels[i] = i;
        }
        levels[HTTP_TRACE_RX_FIRST] = 2;
        levels[HTTP_TRACE_ERROR] = 2;
        levels[HTTP_TRACE_CONN] = 3;
        levels[HTTP_TRACE_RX_HEADERS] = 3;
        levels[HTTP_TRACE_TX_FIRST] = 3;
        levels[HTTP_TRACE_TX_HEADERS] = 3;
        levels[HTTP_TRACE_INFO] = 3;
        levels[HTTP_TRACE_RX_BODY] = 5;
        levels[HTTP_TRACE_TX_BODY] = 5;
        levels[HTTP_TRACE_COMPLETE] = 4;
        
        trace->size = HTTP_TRACE_MAX_SIZE;
        trace->formatter = httpDetailTraceFormatter;
        trace->logger = httpWriteTraceLogFile;
        trace->mutex = mprCreateLock();
    }
    return trace;
}


PUBLIC void httpSetTraceFormat(HttpTrace *trace, cchar *format)
{
    trace->format = sclone(format);
}


PUBLIC void httpSetTraceFormatter(HttpTrace *trace, HttpTraceFormatter callback)
{
    trace->formatter = callback;
}


PUBLIC void httpSetTraceLevel(int level)
{
    Http    *http;

    if (level < 0) {
        level = 0;
    } else if (level > 5) {
        level = 5;
    }
    http = MPR->httpService;
    http->traceLevel = level;
}


PUBLIC void httpSetTraceLevels(HttpTrace *trace, char *levels, ssize bodySize)
{
    int     i;

    assert(trace);
    assert(levels);

    for (i = HTTP_TRACE_5 + 1;  i < HTTP_TRACE_MAX_ITEM; i++) {
        if (levels[i] >= 0) {
            trace->levels[i] = levels[i];
        }
    }
    trace->bodySize = bodySize;
}


PUBLIC void httpSetTraceLogger(HttpTrace *trace, HttpTraceLogger callback)
{
    trace->logger = callback;
}


PUBLIC void httpSetTraceType(HttpTrace *trace, cchar *type)
{
    HttpTraceFormatter  formatter;
    int                 i;

    if (type && smatch(type, "common")) {
        for (i = HTTP_TRACE_5; i < HTTP_TRACE_MAX_ITEM; i++) {
            trace->levels[i] = 6;
        }
        trace->levels[HTTP_TRACE_COMPLETE] = 0;
        formatter = httpCommonTraceFormatter;
    } else {
       formatter = httpDetailTraceFormatter;
    }
    httpSetTraceFormatter(trace, formatter);
}


/*
    Trace a simple message
 */
PUBLIC void httpTrace(HttpConn *conn, int event, cchar *fmt, ...)
{
    va_list     ap;

    assert(conn);
    assert(event >= 0);
    assert(fmt && *fmt);

    if (httpShouldTrace(conn, event) && !conn->rx->skipTrace) {
        va_start(ap, fmt);
        httpFormatTrace(conn, event, sfmtv(fmt, ap), 0, 0);
        va_end(ap);
    }
}


/*
    Trace body content
 */ 
PUBLIC void httpTraceContent(HttpConn *conn, int event, cchar *buf, ssize len, cchar *fmt, ...)
{
    va_list     ap;

    assert(conn);
    assert(buf);

    if (!httpShouldTrace(conn, event) || conn->rx->skipTrace) {
        return;
    }
    if ((event == HTTP_TRACE_RX_BODY && (conn->rx->bytesRead >= conn->trace->size)) ||
        (event == HTTP_TRACE_TX_BODY && (conn->tx->bytesWritten >= conn->trace->size))) {
        if (!conn->rx->skipTrace) {
            conn->rx->skipTrace = 1;
            httpTrace(conn, event, "Abbreviating body trace");
        }
        return;
    }
    va_start(ap, fmt);
    httpFormatTrace(conn, event | HTTP_TRACE_CONTENT, sfmtv(fmt, ap), buf, len);
    va_end(ap);
}


/*
    Trace a packet 
 */
PUBLIC void httpTracePacket(HttpConn *conn, int event, HttpPacket *packet, cchar *fmt, ...)
{
    va_list     ap;
    cchar       *msg;

    assert(conn);
    assert(packet);

    if (fmt) {
        va_start(ap, fmt);
        msg = sfmtv(fmt, ap);
        va_end(ap);
    } else {
        if (event == HTTP_TRACE_RX_HEADERS) {
            msg = "rx headers";
        } else if (event == HTTP_TRACE_RX_BODY) {
            msg = "rx body";
        } else if (event == HTTP_TRACE_TX_HEADERS) {
            msg = "tx headers";
        } else if (event == HTTP_TRACE_TX_BODY) {
            msg = "tx body";
        } else {
            msg = 0;
        }
    }
    if (packet->prefix) {
        mprAddNullToBuf(packet->prefix);
        httpTraceContent(conn, event, mprGetBufStart(packet->prefix), mprGetBufLength(packet->prefix), msg);
    }
    if (packet->content) {
        mprAddNullToBuf(packet->content);
        httpTraceContent(conn, event, mprGetBufStart(packet->content), httpGetPacketLength(packet), msg);
    }
}


PUBLIC void httpFormatTrace(HttpConn *conn, int event, cchar *msg, cchar *buf, ssize len)
{
    (conn->trace->formatter)(conn, event, msg, buf, len);
}


/*
    Low-level write routine to be used only by formatters
 */
PUBLIC void httpWriteTrace(HttpConn *conn, cchar *buf, ssize len)
{
    (conn->trace->logger)(conn, buf, len);
}


/*
    Get a printable version of a buffer. Return a pointer to the start of printable data.
    This will use the tx or rx mime type if possible.
    Skips UTF encoding prefixes
 */
static cchar *makePrintable(HttpConn *conn, int event, cchar *buf, ssize *lenp)
{
    cchar   *start, *cp, *digits;
    char    *data, *dp;
    ssize   len;
    int     i;

    /*
        Fast path, check the mime type
     */ 
    if (event == HTTP_TRACE_RX_BODY) {
        if (sstarts(mprLookupMime(0, conn->rx->mimeType), "text/")) {
            return buf;
        }
    } else if (event == HTTP_TRACE_TX_BODY) {
        if (sstarts(mprLookupMime(0, conn->tx->mimeType), "text/")) {
            return buf;
        }
    }
    start = buf;
    len = *lenp;
    if (len > 3 && start[0] == (char) 0xef && start[1] == (char) 0xbb && start[2] == (char) 0xbf) {
        /* Step over UTF encoding */
        start += 3;
        *lenp -= 3;
    }
    for (i = 0; i < len; i++) {
        if (!isprint((uchar) start[i]) && start[i] != '\n' && start[i] != '\r' && start[i] != '\t') {
            data = mprAlloc(len * 3 + ((len / 16) + 1) + 1);
            digits = "0123456789ABCDEF";
            for (i = 0, cp = start, dp = data; cp < &start[len]; cp++) {
                *dp++ = digits[(*cp >> 4) & 0x0f];
                *dp++ = digits[*cp & 0x0f];
                *dp++ = ' ';
                if ((++i % 16) == 0) {
                    *dp++ = '\n';
                }
            }
            *dp++ = '\n';
            *dp = '\0';
            start = data;
            *lenp = dp - start;
            break;
        }
    }
    return start;
}


/*
    Format a detailed request message
 */
PUBLIC void httpDetailTraceFormatter(HttpConn *conn, int event, cchar *msg, cchar *buf, ssize len)
{
    char    *boundary, prefix[64];
    int     client;

    assert(conn);
    assert(event >= 0);
    assert(msg && *msg);

    client = conn->address ? conn->address->seqno : 0;
    fmt(prefix, sizeof(prefix), "\n<%d-%d-%d> ", client, conn->seqno, conn->rx->seqno);
    httpWriteTrace(conn, prefix, slen(prefix));
    httpWriteTrace(conn, msg, slen(msg));

    if (buf) {
        boundary = ", --details--\n";
        httpWriteTrace(conn, boundary, slen(boundary));
        buf = makePrintable(conn, event, buf, &len);
        httpWriteTrace(conn, buf, len);
        httpWriteTrace(conn, &boundary[2], slen(boundary) - 2);
    } else {
        httpWriteTrace(conn, "\n", 1);
    }
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
            if (trace->flags & MPR_LOG_ANEW) {
                mode |= O_TRUNC;
            }
            if (smatch(trace->path, "stdout")) {
                file = MPR->stdOutput;
            } else if (smatch(trace->path, "stderr")) {
                file = MPR->stdError;
            } else if ((file = mprOpenFile(trace->path, mode, 0664)) == 0) {
                mprLog("http trace", 0, "Cannot open log file %s", trace->path);
                return MPR_ERR_CANT_OPEN;
            }
        }
        trace->file = file;
        trace->flags &= ~MPR_LOG_ANEW;
    }
    return 0;
}


PUBLIC int httpStartTracing(cchar *path)
{
    Http        *http;
    HttpTrace   *trace;
    char        *lspec;

    if ((http = MPR->httpService) == 0 || http->trace == 0) {
        return MPR_ERR_BAD_STATE;
    }
    trace = http->trace;
    trace->path = stok(sclone(path), ":", &lspec);
    http->traceLevel = (int) stoi(lspec);
    httpSetTraceLogFile(trace, trace->path, 0, 0, 0, 0);
    return 0;
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
    trace->path = sclone(path);
    trace->size = size;
    return httpOpenTraceLogFile(trace);
}


/*
    Write a message to the trace log
 */
PUBLIC void httpWriteTraceLogFile(HttpConn *conn, cchar *buf, ssize len)
{
    HttpTrace   *trace;
    static int  skipCheck = 0;

    trace = conn->trace;
    lock(trace);
    if (trace->backupCount > 0) {
        if ((++skipCheck % 50) == 0) {
            backupTraceLogFile(trace);
            if (!trace->file && httpOpenTraceLogFile(trace) < 0) {
                unlock(trace);
                return;
            }
        }
    }
    mprWriteFile(trace->file, buf, len);
    unlock(trace);
}


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
