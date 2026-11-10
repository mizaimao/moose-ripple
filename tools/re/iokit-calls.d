/*
 * Aggregate one process's IOKit user-client traffic for a while, by caller.
 *   sudo dtrace -q -p PID -s iokit-calls.d SECONDS
 * Output: IOServiceOpen client types, IOConnect* calls by (function, connection, selector),
 * memory maps and AddClient links, each with the function that called into IOKit.
 */
pid$target:IOKit:IOServiceOpen:entry
{ self->conn = arg3; self->type = arg2; self->caller = ucaller; }
pid$target:IOKit:IOServiceOpen:return
/self->conn/
{
    printf("open type=%d conn=0x%x by %A\n", self->type,
        *(uint32_t *)copyin(self->conn, 4), self->caller);
    self->conn = 0;
}

pid$target:IOKit:IOConnectCallMethod:entry,
pid$target:IOKit:IOConnectCallAsyncMethod:entry
{ @calls[probefunc, arg0, arg1, ufunc(ucaller)] = count(); }

pid$target:IOKit:IOConnectMapMemory:entry
{ @maps[arg0, arg1, ufunc(ucaller)] = count(); }

pid$target:IOKit:IOConnectAddClient:entry
{ @adds[arg0, arg1, ufunc(ucaller)] = count(); }

tick-1s
/++secs >= $1/
{
    printf("\n== calls: function, connection, selector, caller, count\n");
    printa("%s 0x%x %d %A %@d\n", @calls);
    printf("\n== maps: connection, memory type, caller, count\n");
    printa("0x%x %d %A %@d\n", @maps);
    printf("\n== AddClient: connection, other connection, caller, count\n");
    printa("0x%x 0x%x %A %@d\n", @adds);
    exit(0);
}
