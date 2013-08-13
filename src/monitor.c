/*
    monitor.c -- Monitor and defensive management.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.

    A note on locking. Unlike most of appweb which effectively runs single-threaded due to the dispatcher,
    this module typically runs the httpMonitorEvent and checkMonitor routines multi-threaded.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forwards **********************************/

static void stopMonitors();

/************************************ Code ************************************/

PUBLIC int httpAddCounter(cchar *name)
{
    Http    *http;

    http = MPR->httpService;
    return mprAddItem(http->counters, sclone(name));
}


PUBLIC void httpAddCounters()
{
    Http    *http;

    http = MPR->httpService;
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
    mprHold(args);

    for (ITERATE_ITEMS(monitor->defenses, defense, next)) {
        if ((remedyProc = mprLookupKey(http->remedies, defense->remedy)) == 0) {
            continue;
        }
        extra = mprCloneHash(defense->args);
        for (ITERATE_KEYS(extra, kp)) {
            kp->data = stemplate(kp->data, args);
        }
        mprBlendHash(args, extra);
        mprLog(1, "Defense \"%s\" activated. Running remedy \"%s\".", defense->name, defense->remedy);
        remedyProc(args);
    }
    mprRelease(args);
}


static void checkCounter(HttpMonitor *monitor, HttpCounter *counter, cchar *ip)
{
    MprHash     *args;
    cchar       *address, *fmt, *msg, *subject;
    uint64      period;

    fmt = 0;

    if (monitor->expr == '>') {
        if (counter->value > monitor->limit) {
            fmt = "WARNING: Monitor%s for %s at %Ld / %Ld secs exceeds limit of %Ld";
        }

    } else if (monitor->expr == '>') {
        if (counter->value < monitor->limit) {
            fmt = "WARNING: Monitor%s for %s at %Ld / %Ld secs outside limit of %Ld";
        }
    }
    if (fmt) {
        period = monitor->period / 1000;
        address = ip ? sfmt(" %s", ip) : "";
        msg = sfmt(fmt, address, monitor->counterName, counter->value, period, monitor->limit);
        subject = sfmt("Monitor %s Alert", monitor->counterName);
        args = mprDeserialize(
            sfmt("{ COUNTER: '%s', DATE: '%s', IP: '%s', LIMIT: %Ld, MESSAGE: '%s', PERIOD: %Ld, SUBJECT: '%s', VALUE: %Ld }", 
            monitor->counterName, mprGetDate(NULL), ip, monitor->limit, msg, period, subject, counter->value));
        invokeDefenses(monitor, args);
    }
    mprTrace(5, "CheckCounter \"%s\" (%Ld %c limit %Ld) every %Ld secs", monitor->counterName, counter->value, monitor->expr, monitor->limit, 
        monitor->period / 1000);
    counter->value = 0;
}


static void checkMonitor(HttpMonitor *monitor, MprEvent *event)
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     c, *counter;
    MprKey          *kp;
    int             removed;

    http = monitor->http;
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
        do {
            removed = 0;
            for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
                counter = &address->counters[monitor->counterIndex];
                unlock(http->addresses);
                checkCounter(monitor, counter, kp->key);
                lock(http->addresses);
                /*
                    Expire old records
                 */
                if ((address->updated + http->monitorMaxPeriod) < http->now) {
                    mprRemoveKey(http->addresses, kp->key);
                    removed = 1;
                    break;
                }
            }
        } while (removed);
        unlock(http->addresses);

        if (mprGetHashLength(http->addresses) == 0) {
            stopMonitors();
        }
        return;
    }
}


static int manageMonitor(HttpMonitor *monitor, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
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

    http = MPR->httpService;
    if (period < HTTP_MONITOR_MIN_PERIOD) {
        return MPR_ERR_BAD_ARGS;
    }
    if ((counterIndex = mprLookupStringItem(http->counters, counterName)) < 0) {
        mprError("Cannot find counter %s", counterName);
        return MPR_ERR_CANT_FIND;
    }
    for (ITERATE_ITEMS(http->monitors, mp, next)) {
        if (mp->counterIndex == counterIndex) {
            mprError("Monitor already exists for counter %s", counterName);
            return MPR_ERR_ALREADY_EXISTS;
        }
    }
    if ((monitor = mprAllocObj(HttpMonitor, manageMonitor)) == 0) {
        return MPR_ERR_MEMORY;
    }
    if ((defenseList = mprCreateList(0, -1)) == 0) {
        return MPR_ERR_MEMORY;
    }
    tok = sclone(defenses);
    while ((def = stok(tok, " \t", &tok)) != 0) {
        if ((defense = mprLookupKey(http->defenses, def)) == 0) {
            mprError("Cannot find defense \"%s\"", def);
            return 0;
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
    http->monitorMinPeriod = min(http->monitorMinPeriod, period);
    http->monitorMaxPeriod = max(http->monitorMaxPeriod, period);
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
    http = MPR->httpService;
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
    mprTrace(4, "Start monitors: min %Ld, max %Ld",  http->monitorMinPeriod, http->monitorMaxPeriod);
}


static void stopMonitors() 
{
    HttpMonitor     *monitor;
    Http            *http;
    int             next;

    mprTrace(4, "Stop monitors");
    http = MPR->httpService;
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


/*
    Register a monitor event
    This code is very carefully coded for maximum speed without using locks. 
    There are some tolerated race conditions.
 */
PUBLIC int64 httpMonitorEvent(HttpConn *conn, int counterIndex, int64 adj)
{
    Http            *http;
    HttpAddress     *address;
    HttpCounter     *counter;
    int             ncounters;

    assert(conn->endpoint);
    http = conn->http;
    address = conn->address;

    if (!address) {
        lock(http->addresses);
        address = mprLookupKey(http->addresses, conn->ip);
        if (!address || address->ncounters <= counterIndex) {
            ncounters = ((counterIndex + 0xF) & ~0xF);
            if (address) {
                address = mprRealloc(address, sizeof(HttpAddress) * ncounters * sizeof(HttpCounter));
            } else {
                address = mprAllocMem(sizeof(HttpAddress) * ncounters * sizeof(HttpCounter), MPR_ALLOC_MANAGER | MPR_ALLOC_ZERO);
                mprSetManager(address, (MprManager) manageAddress);
            }
            if (!address) {
                return 0;
            }
            address->ncounters = ncounters;
            mprAddKey(http->addresses, conn->ip, address);
        }
        conn->address = address;
        if (!http->monitorsStarted) {
            startMonitors();
        }
        unlock(http->addresses);
    }
    counter = &address->counters[counterIndex];
    mprAtomicAdd64((int64*) &counter->value, adj);
    /* Tolerated race with "updated" and the return value */
    address->updated = http->now;
    return counter->value;
}


static int manageDefense(HttpDefense *defense, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(defense->name);
        mprMark(defense->remedy);
        mprMark(defense->args);
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
    return defense;
}


/*
    Remedy can be set via REMEDY= in the remedyArgs
 */
PUBLIC int httpAddDefense(cchar *name, cchar *remedy, cchar *remedyArgs)
{
    Http        *http;
    MprHash     *args;
    MprList     *list;
    char        *arg, *key, *value;
    int         next;

    assert(name && *name);

    http = MPR->httpService;
    args = mprCreateHash(0, 0);
    list = stolist(remedyArgs);
    for (ITERATE_ITEMS(list, arg, next)) {
        key = stok(arg, "=", &value);
        mprAddKey(args, key, strim(value, "\"'", 0));
    }
    if (!remedy) {
        remedy = mprLookupKey(args, "REMEDY");
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

    http = MPR->httpService;
    mprRawLog(0, "Monitor Counters:\n");
    mprRawLog(0, "Memory counter     %,Ld\n", mprGetMem());
    mprRawLog(0, "Active processes   %,Ld\n", mprGetListLength(MPR->cmdService->cmds));
    mprRawLog(0, "Active clients     %,Ld\n", mprGetHashLength(http->addresses));

    lock(http->addresses);
    for (ITERATE_KEY_DATA(http->addresses, kp, address)) {
        mprRawLog(0, "Client             %s\n", kp->key);
        for (i = 0; i < address->ncounters; i++) {
            counter = &address->counters[i];
            name = mprGetItem(http->counters, i);
            if (name == NULL) {
                break;
            }
            mprRawLog(0, "  Counter          %s = %,Ld\n", name, counter->value);
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

    http = MPR->httpService;
    if ((address = mprLookupKey(http->addresses, ip)) == 0) {
        mprLog(1, "Cannot find client %s to ban", ip);
        return MPR_ERR_CANT_FIND;
    }
    banUntil = http->now + period;
    address->banUntil = max(banUntil, address->banUntil);
    address->banMsg = msg;
    address->banStatus = status;
    mprLog(1, "Client %s banned for %Ld secs. %s", ip, period / 1000, address->banMsg ? address->banMsg : "");
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
        period = lookupTicks(args, "PERIOD", BIT_HTTP_BAN_PERIOD);
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

#if DEBUG_IDE && BIT_UNIX_LIKE
    unsetenv("DYLD_LIBRARY_PATH");
    unsetenv("DYLD_FRAMEWORK_PATH");
#endif
    if ((cmd = mprCreateCmd(NULL)) == 0) {
        return;
    }
    command = sclone(mprLookupKey(args, "CMD"));
    data = 0;
    if (scontains(command, "|")) {
        data = stok(command, "|", &command);
        data = stemplate(data, args);
    }
    mprTrace(1, "Run cmd remedy: %s", command);
    command = strim(command, " \t", MPR_TRIM_BOTH);
    if ((background = (sends(command, "&"))) != 0) {
        command = strim(command, "&", MPR_TRIM_END);
    }
    argc = mprMakeArgv(command, &argv, 0);
    cmd->stdoutBuf = mprCreateBuf(BIT_MAX_BUFFER, -1);
    cmd->stderrBuf = mprCreateBuf(BIT_MAX_BUFFER, -1);
    if (mprStartCmd(cmd, argc, argv, NULL, MPR_CMD_DETACH | MPR_CMD_IN) < 0) {
        mprError("Cannot start command: %s", command);
        return;
    }
    if (data && mprWriteCmdBlock(cmd, MPR_CMD_STDIN, data, -1) < 0) {
        mprError("Cannot write to command: %s", command);
        return;
    }
    mprFinalizeCmd(cmd);
    if (!background) {
        rc = mprWaitForCmd(cmd, BIT_HTTP_REMEDY_TIMEOUT);
        status = mprGetCmdExitStatus(cmd);
        if (rc < 0 || status != 0) {
            mprError("Email remedy failed. Error: %s\nResult: %s", mprGetBufStart(cmd->stderrBuf), mprGetBufStart(cmd->stdoutBuf));
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

    http = MPR->httpService;
    if ((ip = mprLookupKey(args, "IP")) != 0) {
        if ((address = mprLookupKey(http->addresses, ip)) != 0) {
            delayUntil = http->now + lookupTicks(args, "PERIOD", BIT_HTTP_DELAY_PERIOD);
            address->delayUntil = max(delayUntil, address->delayUntil);
            delay = (int) lookupTicks(args, "DELAY", BIT_HTTP_DELAY);
            address->delay = max(delay, address->delay);
            mprLog(0, "%s", mprLookupKey(args, "MESSAGE"));
            mprLog(0, "Initiate delay of %d for IP address %s", address->delay, ip);
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
    cchar   *uri, *msg, *method;
    char    *response, *err;
    int     status;

    uri = mprLookupKey(args, "URI");
    if ((method = mprLookupKey(args, "METHOD")) == 0) {
        method = "POST";
    }
    msg = smatch(method, "POST") ? mprLookupKey(args, "MESSAGE") : 0;
    status = httpRequest(method, uri, msg, &response, &err);
    if (status < 0) {
        mprError("%s", err);
        return;
    }
    if (status != HTTP_CODE_OK) {
        mprError("Remedy URI %s responded with status %d\n%s", status, response);
    }
}


static void logRemedy(MprHash *args)
{
    mprLog(0, "%s", mprLookupKey(args, "MESSAGE"));
}


static void restartRemedy(MprHash *args)
{
    mprError("RestartRemedy: Restarting ...");
    mprRestart();
}


PUBLIC int httpAddRemedy(cchar *name, HttpRemedyProc remedy)
{
    Http    *http;

    http = MPR->httpService;
    mprAddKey(http->remedies, name, remedy);
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
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

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
