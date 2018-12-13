/*
    monitor.c -- Monitor and defensive management.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    A note on locking. Unlike most of appweb which effectively runs single-threaded due to the dispatcher,
    this module typically runs the httpMonitorEvent and checkMonitor routines multi-threaded.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static HttpAddress *growAddresses(HttpNet *net, HttpAddress *address, int counterIndex);
static MprTicks lookupTicks(MprHash *args, cchar *key, MprTicks defaultValue);
static void stopMonitors(void);

/************************************ Code ************************************/

PUBLIC int httpAddCounter(cchar *name)
{
    return mprAddItem(HTTP->counters, sclone(name));
}


PUBLIC void httpAddCounters()
{
    Http    *http;

    http = HTTP;
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_CLIENTS, sclone("ActiveClients"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_CONNECTIONS, sclone("ActiveConnections"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_REQUESTS, sclone("ActiveRequests"));
    mprSetItem(http->counters, HTTP_COUNTER_ACTIVE_PROCESSES, sclone("ActiveProcesses"));
    mprSetItem(http->counters, HTTP_COUNTER_BAD_REQUEST_ERRORS, sclone("BadRequestErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_ERRORS, sclone("Errors"));
    mprSetItem(http->counters, HTTP_COUNTER_LIMIT_ERRORS, sclone("LimitErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_MEMORY, sclone("Memory"));
    mprSetItem(http->counters, HTTP_COUNTER_NOT_FOUND_ERRORS, sclone("NotFoundErrors"));
    mprSetItem(http->counters, HTTP_COUNTER_NETWORK_IO, sclone("NetworkIO"));
    mprSetItem(http->counters, HTTP_COUNTER_REQUESTS, sclone("Requests"));
    mprSetItem(http->counters, HTTP_COUNTER_SSL_ERRORS, sclone("SSLErrors"));
}


static void invokeDefenses(HttpMonitor *monitor, MprHash *args)
{
    Http            *http;
    HttpDefense     *defense;
    HttpRemedyProc  remedyProc;
    MprKey          *kp;
    MprHash         *extra;
    int             next;

    http = monitor->http;

    for (ITERATE_ITEMS(monitor->defenses, defense, next)) {
        if ((remedyProc = mprLookupKey(http->remedies, defense->remedy)) == 0) {
            continue;
        }
        extra = mprCloneHash(defense->args);
        for (ITERATE_KEYS(extra, kp)) {
            kp->data = stemplate(kp->data, args);
        }
        mprBlendHash(args, extra);

        if (defense->suppressPeriod) {
            typedef struct SuppressDefense {
                MprTicks    suppressUntil;
            } SuppressDefense;

            SuppressDefense *sd;
            cchar *str = mprHashToString(args, "");
            if (!defense->suppress) {
                defense->suppress = mprCreateHash(0, 0);
            }
            if ((sd = mprLookupKey(defense->suppress, str)) != 0) {
                if (sd->suppressUntil > http->now) {
                    continue;
                }
                sd->suppressUntil = http->now + defense->suppressPeriod;
            } else {
                if ((sd = mprAllocStruct(SuppressDefense)) != 0) {
                    mprAddKey(defense->suppress, str, sd);
                }
                sd->suppressUntil = http->now + defense->suppressPeriod;
            }
        }
        httpTrace(http->trace, "monitor.defense.invoke", "context", "defense:'%s', remedy:'%s'", defense->name, defense->remedy);

        /*  WARNING: yields */
        remedyProc(args);

#if FUTURE
        if (http->monitorCallback) {
            (http->monitorCallback)(monitor, defense, args);
        }
#endif
    }
}


static void checkCounter(HttpMonitor *monitor, HttpCounter *counter, cchar *ip)
{
    MprHash     *args;
    cchar       *address, *fmt, *msg, *subject;
    uint64      period;

    fmt = 0;

    if (monitor->expr == '>') {
        if (counter->value > monitor->limit) {
            fmt = "Monitor%s for \"%s\". Value %lld per %lld secs exceeds limit of %lld.";
        }

    } else if (monitor->expr == '>') {
        if (counter->value < monitor->limit) {
            fmt = "Monitor%s for \"%s\". Value %lld per %lld secs outside limit of %lld.";
        }
    }
    if (fmt) {
        period = monitor->period / 1000;
        address = ip ? sfmt(" %s", ip) : "";
        msg = sfmt(fmt, address, monitor->counterName, counter->value, period, monitor->limit);
        httpTrace(HTTP->trace, "monitor.check", "context", "msg:'%s'", msg);

        subject = sfmt("Monitor %s Alert", monitor->counterName);
        args = mprDeserialize(
            sfmt("{ COUNTER: '%s', DATE: '%s', IP: '%s', LIMIT: %lld, MESSAGE: '%s', PERIOD: %lld, SUBJECT: '%s', VALUE: %lld }",
            monitor->counterName, mprGetDate(NULL), ip, monitor->limit, msg, period, subject, counter->value));
        /*
            WARNING: remedies may yield
         */
        mprAddRoot(args);
        invokeDefenses(monitor, args);
        mprRemoveRoot(args);
    }
    counter->value = 0;
}


PUBLIC void httpPruneMonitors()
{
    Http        *http;
    HttpAddress *address;
    MprTicks    period;
    MprKey      *kp;

    http = HTTP;
    period = max(http->monitorPeriod, ME_HTTP_MONITOR_PERIOD);
    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        if (address->banUntil && address->banUntil < http->now) {
            httpTrace(http->trace, "monitor.ban.stop", "context", "client:'%s'", kp->key);
            address->banUntil = 0;
        }
        if ((address->updated + period) < http->now && address->banUntil == 0) {
            mprRemoveKey(http->addresses, kp->key);
            /* Safe to keep iterating after removal of key */
        }
    }
    unlock(http->addresses);
}


/*
    WARNING: this routine may yield
 */
static void checkMonitor(HttpMonitor *monitor, MprEvent *event)
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     c, *counter;
    MprKey          *kp;

    http = HTTP;
    http->now = mprGetTicks();

    if (monitor->counterIndex == HTTP_COUNTER_MEMORY) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetMem();
        checkCounter(monitor, &c, NULL);

    } else if (monitor->counterIndex == HTTP_COUNTER_ACTIVE_PROCESSES) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetListLength(MPR->cmdService->cmds);
        checkCounter(monitor, &c, NULL);

    } else if (monitor->counterIndex == HTTP_COUNTER_ACTIVE_CLIENTS) {
        memset(&c, 0, sizeof(HttpCounter));
        c.value = mprGetHashLength(http->addresses);
        checkCounter(monitor, &c, NULL);

    } else {
        /*
            Check the monitor for each active client address
         */
        lock(http->addresses);
        for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
            counter = &address->counters[monitor->counterIndex];

            /*
                WARNING: this may allow new addresses to be added or stale addresses to be removed.
                Regardless, because GC is paused, iterating is safe.
             */
            unlock(http->addresses);
            checkCounter(monitor, counter, kp->key);
            lock(http->addresses);
        }
        if (mprGetHashLength(http->addresses) == 0) {
            stopMonitors();
        }
        unlock(http->addresses);
        httpPruneMonitors();
    }
}


static int manageMonitor(HttpMonitor *monitor, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(monitor->counterName);
        mprMark(monitor->defenses);
        mprMark(monitor->timer);
    }
    return 0;
}


PUBLIC int httpAddMonitor(cchar *counterName, cchar *expr, uint64 limit, MprTicks period, cchar *defenses)
{
    Http            *http;
    HttpMonitor     *monitor, *mp;
    HttpDefense     *defense;
    MprList         *defenseList;
    cchar           *def;
    char            *tok;
    int             counterIndex, next;

    http = HTTP;
    if (period < HTTP_MONITOR_MIN_PERIOD) {
        return MPR_ERR_BAD_ARGS;
    }
    if ((counterIndex = mprLookupStringItem(http->counters, counterName)) < 0) {
        mprLog("error http monitor", 0, "Cannot find counter %s", counterName);
        return MPR_ERR_CANT_FIND;
    }
    for (ITERATE_ITEMS(http->monitors, mp, next)) {
        if (mp->counterIndex == counterIndex) {
            mprLog("error http monitor", 0, "Monitor already exists for counter %s", counterName);
            return MPR_ERR_ALREADY_EXISTS;
        }
    }
    if ((monitor = mprAllocObj(HttpMonitor, manageMonitor)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if ((defenseList = mprCreateList(-1, MPR_LIST_STABLE)) == 0) {
        return MPR_ERR_MEMORY;
    }
    tok = sclone(defenses);
    while ((def = stok(tok, " \t", &tok)) != 0) {
        if ((defense = mprLookupKey(http->defenses, def)) == 0) {
            mprLog("error http monitor", 0, "Cannot find Defense \"%s\"", def);
            return MPR_ERR_CANT_FIND;
        }
        mprAddItem(defenseList, defense);
    }
    monitor->counterIndex = counterIndex;
    monitor->counterName = mprGetItem(http->counters, monitor->counterIndex);
    monitor->expr = (expr && *expr == '<') ? '<' : '>';
    monitor->limit = limit;
    monitor->period = period;
    monitor->defenses = defenseList;
    monitor->http = http;
    http->monitorPeriod = min(http->monitorPeriod, period);
    mprAddItem(http->monitors, monitor);
    return 0;
}


static void manageAddress(HttpAddress *address, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(address->banMsg);
    }
}


static void startMonitors()
{
    HttpMonitor     *monitor;
    Http            *http;
    int             next;

    if (mprGetDebugMode()) {
        return;
    }
    http = HTTP;
    lock(http);
    if (!http->monitorsStarted) {
        for (ITERATE_ITEMS(http->monitors, monitor, next)) {
            if (!monitor->timer) {
                monitor->timer = mprCreateTimerEvent(NULL, "monitor", monitor->period, checkMonitor, monitor, 0);
            }
        }
        http->monitorsStarted = 1;
    }
    unlock(http);
}


static void stopMonitors()
{
    HttpMonitor     *monitor;
    Http            *http;
    int             next;

    http = HTTP;
    lock(http);
    if (http->monitorsStarted) {
        for (ITERATE_ITEMS(http->monitors, monitor, next)) {
            if (monitor->timer) {
                mprStopContinuousEvent(monitor->timer);
                monitor->timer = 0;
            }
        }
        http->monitorsStarted = 0;
    }
    unlock(http);
}


PUBLIC HttpAddress *httpMonitorAddress(HttpNet *net, int counterIndex)
{
    Http            *http;
    HttpAddress     *address;
    int             count;
    static int      seqno = 0;

    address = net->address;
    if (address) {
        return address;
    }
    http = net->http;
    count = mprGetHashLength(http->addresses);
    if (count > net->limits->clientMax) {
        mprLog("net info", 3, "Too many concurrent clients, active: %d, max:%d", count, net->limits->clientMax);
        return 0;
    }
    if (counterIndex <= 0) {
        counterIndex = HTTP_COUNTER_MAX;
    }
    lock(http->addresses);
    address = mprLookupKey(http->addresses, net->ip);
    if ((address = growAddresses(net, address, counterIndex)) == 0) {
        unlock(http->addresses);
        return 0;
    }
    address->seqno = ++seqno;
    mprAddKey(http->addresses, net->ip, address);

    net->address = address;
    if (!http->monitorsStarted) {
        startMonitors();
    }
    unlock(http->addresses);
    return address;
}


static HttpAddress *growAddresses(HttpNet *net, HttpAddress *address, int counterIndex)
{
    int     ncounters;

    if (!address || address->ncounters <= counterIndex) {
        ncounters = ((counterIndex + 0xF) & ~0xF);
        if (address) {
            address = mprRealloc(address, sizeof(HttpAddress) * ncounters * sizeof(HttpCounter));
            memset(&address[address->ncounters], 0, (ncounters - address->ncounters) * sizeof(HttpCounter));
        } else {
            address = mprAllocBlock(sizeof(HttpAddress) * ncounters * sizeof(HttpCounter), MPR_ALLOC_MANAGER | MPR_ALLOC_ZERO);
            mprSetManager(address, (MprManager) manageAddress);
        }
        address->ncounters = ncounters;
    }
    return address;
}


PUBLIC int64 httpMonitorNetEvent(HttpNet *net, int counterIndex, int64 adj)
{
    HttpAddress     *address;
    HttpCounter     *counter;

    if ((address = httpMonitorAddress(net, counterIndex)) == 0) {
        return 0;
    }
    counter = &address->counters[counterIndex];
    mprAtomicAdd64((int64*) &counter->value, adj);
    /*
        Tolerated race with "updated" and the return value
     */
    address->updated = net->http->now;
    return counter->value;
}


PUBLIC int64 httpMonitorEvent(HttpStream *stream, int counterIndex, int64 adj)
{
    return httpMonitorNetEvent(stream->net, counterIndex, adj);
}


static int manageDefense(HttpDefense *defense, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(defense->name);
        mprMark(defense->remedy);
        mprMark(defense->args);
        mprMark(defense->suppress);
    }
    return 0;
}


static HttpDefense *createDefense(cchar *name, cchar *remedy, MprHash *args)
{
    HttpDefense     *defense;

    if ((defense = mprAllocObj(HttpDefense, manageDefense)) == 0) {
        return 0;
    }
    defense->name = sclone(name);
    defense->remedy = sclone(remedy);
    defense->args = args;
    defense->suppressPeriod = lookupTicks(args, "SUPPRESS", 0);
    return defense;
}


/*
    Remedy can also be set via REMEDY= in the remedyArgs
 */
PUBLIC int httpAddDefense(cchar *name, cchar *remedy, cchar *remedyArgs)
{
    Http        *http;
    MprHash     *args;
    MprList     *list;
    char        *arg, *key, *value;
    int         next;

    assert(name && *name);

    http = HTTP;
    args = mprCreateHash(0, MPR_HASH_STABLE);
    list = stolist(remedyArgs);
    for (ITERATE_ITEMS(list, arg, next)) {
        key = ssplit(arg, "=", &value);
        mprAddKey(args, key, strim(value, "\"'", 0));
    }
    if (!remedy) {
        remedy = mprLookupKey(args, "REMEDY");
    }
    mprAddKey(http->defenses, name, createDefense(name, remedy, args));
    return 0;
}


PUBLIC int httpAddDefenseFromJson(cchar *name, cchar *remedy, MprJson *jargs)
{
    Http        *http;
    MprHash     *args;
    MprJson     *arg;
    int         next;

    assert(name && *name);

    http = HTTP;
    args = mprCreateHash(0, MPR_HASH_STABLE);
    for (ITERATE_JSON(jargs, arg, next)) {
        mprAddKey(args, arg->name, arg->value);
        if (smatch(arg->name, "remedy")) {
            remedy = arg->value;
        }
    }
    mprAddKey(http->defenses, name, createDefense(name, remedy, args));
    return 0;
}


PUBLIC void httpDumpCounters()
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     *counter;
    MprKey          *kp;
    cchar           *name;
    int             i;

    http = HTTP;
    mprLog(0, 0, "Monitor Counters:\n");
    mprLog(0, 0, "Memory counter     %'zd\n", mprGetMem());
    mprLog(0, 0, "Active processes   %d\n", mprGetListLength(MPR->cmdService->cmds));
    mprLog(0, 0, "Active clients     %d\n", mprGetHashLength(http->addresses));

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        mprLog(0, 0, "Client             %s\n", kp->key);
        for (i = 0; i < address->ncounters; i++) {
            counter = &address->counters[i];
            name = mprGetItem(http->counters, i);
            if (name == NULL) {
                break;
            }
            mprLog(0, 0, "  Counter          %s = %'lld\n", name, counter->value);
        }
    }
    unlock(http->addresses);
}


/************************************ Remedies ********************************/

PUBLIC int httpBanClient(cchar *ip, MprTicks period, int status, cchar *msg)
{
    Http            *http;
    HttpAddress     *address;
    MprTicks        banUntil;

    http = HTTP;
    if ((address = mprLookupKey(http->addresses, ip)) == 0) {
        mprLog("error http monitor", 1, "Cannot find client %s to ban", ip);
        return MPR_ERR_CANT_FIND;
    }
    if (address->banUntil < http->now) {
        httpTrace(http->trace, "monitor.ban.start", "error", "client:'%s', duration:%lld", ip, period / 1000);
    }
    banUntil = http->now + period;
    address->banUntil = max(banUntil, address->banUntil);
    if (msg && *msg) {
        address->banMsg = sclone(msg);
    }
    address->banStatus = status;
    return 0;
}


static MprTicks lookupTicks(MprHash *args, cchar *key, MprTicks defaultValue)
{
    cchar   *s;
    return ((s = mprLookupKey(args, key)) ? httpGetTicks(s) : defaultValue);
}


static void banRemedy(MprHash *args)
{
    MprTicks    period;
    cchar       *ip, *banStatus, *msg;
    int         status;

    if ((ip = mprLookupKey(args, "IP")) != 0) {
        period = lookupTicks(args, "PERIOD", ME_HTTP_BAN_PERIOD);
        msg = mprLookupKey(args, "MESSAGE");
        status = ((banStatus = mprLookupKey(args, "STATUS")) != 0) ? atoi(banStatus) : 0;
        httpBanClient(ip, period, status, msg);
    }
}


static void cmdRemedy(MprHash *args)
{
    MprCmd      *cmd;
    cchar       **argv;
    char        *command, *data;
    int         rc, status, argc, background;

#if DEBUG_IDE && ME_UNIX_LIKE
    unsetenv("DYLD_LIBRARY_PATH");
    unsetenv("DYLD_FRAMEWORK_PATH");
#endif
    if ((cmd = mprCreateCmd(NULL)) == 0) {
        return;
    }
    command = sclone(mprLookupKey(args, "CMD"));
    data = 0;
    if (scontains(command, "|")) {
        data = ssplit(command, "|", &command);
        data = stemplate(data, args);
    }
    command = strim(command, " \t", MPR_TRIM_BOTH);
    if ((background = ((sends(command, "&"))) != 0)) {
        command = strim(command, "&", MPR_TRIM_END);
    }
    argc = mprMakeArgv(command, &argv, 0);
    cmd->stdoutBuf = mprCreateBuf(ME_BUFSIZE, -1);
    cmd->stderrBuf = mprCreateBuf(ME_BUFSIZE, -1);

    httpTrace(HTTP->trace, "monitor.remedy.cmd", "context", "remedy:'%s'", command);
    if (mprStartCmd(cmd, argc, argv, NULL, MPR_CMD_DETACH | MPR_CMD_IN) < 0) {
        httpTrace(HTTP->trace, "monitor.rememdy.cmd.error", "error", "msg:'Cannot start command. %s'", command);
        return;
    }
    if (data) {
        if (mprWriteCmdBlock(cmd, MPR_CMD_STDIN, data, -1) < 0) {
            httpTrace(HTTP->trace, "monitor.remedy.cmd.error", "error", "msg:'Cannot write to command. %s'", command);
            return;
        }
    }
    mprFinalizeCmd(cmd);
    if (!background) {
        rc = mprWaitForCmd(cmd, ME_HTTP_REMEDY_TIMEOUT);
        status = mprGetCmdExitStatus(cmd);
        if (rc < 0 || status != 0) {
            httpTrace(HTTP->trace, "monitor.remedy.cmd.error", "error", "msg:'Remedy failed. %s. %s', command: '%s'",
                mprGetBufStart(cmd->stderrBuf), mprGetBufStart(cmd->stdoutBuf), command);
            return;
        }
        mprDestroyCmd(cmd);
    }
}


static void delayRemedy(MprHash *args)
{
    Http            *http;
    HttpAddress     *address;
    MprTicks        delayUntil;
    cchar           *ip;
    int             delay;

    http = HTTP;
    if ((ip = mprLookupKey(args, "IP")) != 0) {
        if ((address = mprLookupKey(http->addresses, ip)) != 0) {
            delayUntil = http->now + lookupTicks(args, "PERIOD", ME_HTTP_DELAY_PERIOD);
            address->delayUntil = max(delayUntil, address->delayUntil);
            delay = (int) lookupTicks(args, "DELAY", ME_HTTP_DELAY);
            address->delay = max(delay, address->delay);
            httpTrace(http->trace, "monitor.delay.start", "context", "client:'%s', delay:%d", ip, address->delay);
        }
    }
}


static void emailRemedy(MprHash *args)
{
    if (!mprLookupKey(args, "FROM")) {
        mprAddKey(args, "FROM", "admin");
    }
    mprAddKey(args, "CMD", "To: ${TO}\nFrom: ${FROM}\nSubject: ${SUBJECT}\n${MESSAGE}\n\n| sendmail -t");
    cmdRemedy(args);
}


static void httpRemedy(MprHash *args)
{
    HttpStream    *stream;
    cchar       *uri, *msg, *method;
    char        *err;
    int         status;

    uri = mprLookupKey(args, "URI");
    if ((method = mprLookupKey(args, "METHOD")) == 0) {
        method = "POST";
    }
    msg = smatch(method, "POST") ? mprLookupKey(args, "MESSAGE") : 0;
    if ((stream = httpRequest(method, uri, msg, HTTP_1_1, &err)) == 0) {
        httpTrace(HTTP->trace, "monitor.remedy.http.error", "error", "msg:'%s'", err);
        return;
    }
    status = httpGetStatus(stream);
    if (status != HTTP_CODE_OK) {
        httpTrace(HTTP->trace, "monitor.remedy.http.error", "error", "status:%d, uri:'%s'", status, uri);
    }
}

/*
    Write to the error log
 */
static void logRemedy(MprHash *args)
{
    mprLog("error http monitor", 0, "%s", (char*) mprLookupKey(args, "MESSAGE"));
}


static void restartRemedy(MprHash *args)
{
    mprLog("info http monitor", 0, "RestartRemedy: Restarting ...");
    mprRestart();
}


PUBLIC int httpAddRemedy(cchar *name, HttpRemedyProc remedy)
{
    mprAddKey(HTTP->remedies, name, remedy);
    return 0;
}


PUBLIC int httpAddRemedies()
{
    httpAddRemedy("ban", banRemedy);
    httpAddRemedy("cmd", cmdRemedy);
    httpAddRemedy("delay", delayRemedy);
    httpAddRemedy("email", emailRemedy);
    httpAddRemedy("http", httpRemedy);
    httpAddRemedy("log", logRemedy);
    httpAddRemedy("restart", restartRemedy);
    return 0;
}


/*
    Copyright (c) Embedthis Software. All Rights Reserved.
    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.
 */
