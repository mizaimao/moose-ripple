/*
 * Kernel-side view of one program's IOKit user-client traffic, from the moment it starts
 * (catches WindowServer at login, before pid probes could attach).
 *   sudo dtrace -s qe-kernel-iokit.d WindowServer SECONDS
 * Prints each user-client open (class from the newUserClient function that ran), each
 * user client's class (vtable symbol) the first time a method goes to it, the first call of each
 * (client, selector, input shape) with its first scalars, and counts at the end.
 * fbt probes on xnu-1504 (10.6.8): is_io_service_open_extended, is_io_connect_method.
 * (M5 R2.)
 */
#pragma D option quiet
#pragma D option bufsize=16m
#pragma D option dynvarsize=16m

dtrace:::BEGIN { start = timestamp; }

fbt::is_io_service_open_extended:entry
/execname == $$1/
{
    printf("%6d ms open: type %d, service %p %a\n", (timestamp - start) / 1000000, arg2, arg0,
        *(uint64_t *)arg0);
}

fbt::*newUserClient*:entry
/execname == $$1/
{
    printf("%6d ms   newUserClient: %s:%s\n", (timestamp - start) / 1000000, probemod, probefunc);
}

/* The class of each user client: its vtable symbol, the first time a call goes to it. */
fbt::is_io_connect_method:entry,
fbt::is_io_connect_method_*:entry,
fbt::is_io_connect_async_method:entry
/execname == $$1 && !seenclass[arg0]/
{
    seenclass[arg0] = 1;
    printf("%6d ms client %p is %a\n", (timestamp - start) / 1000000, arg0, *(uint64_t *)arg0);
}

fbt::is_io_connect_method:entry
/execname == $$1/
{
    @calls[arg0, arg1, (uint32_t)arg3, (uint32_t)arg5, arg7] = count();
}

fbt::is_io_connect_method:entry
/execname == $$1 && !seen[arg0, arg1, (uint32_t)arg3, (uint32_t)arg5]/
{
    seen[arg0, arg1, (uint32_t)arg3, (uint32_t)arg5] = 1;
    this->s = (uint64_t *)arg2;
    printf("%6d ms call: client %p sel %d scalars %d [%x %x %x %x] struct-in %d ool-in %d\n",
        (timestamp - start) / 1000000, arg0, arg1, (uint32_t)arg3,
        arg3 > 0 ? this->s[0] : 0, arg3 > 1 ? this->s[1] : 0,
        arg3 > 2 ? this->s[2] : 0, arg3 > 3 ? this->s[3] : 0,
        (uint32_t)arg5, arg7);
}

/* Older entry points (IOConnectMethod*, async): counts only. */
fbt::is_io_connect_method_*:entry,
fbt::is_io_connect_async_method:entry
/execname == $$1/
{
    @legacy[probefunc, arg0, arg1] = count();
}

fbt::is_io_connect_map_memory_into_task:entry
/execname == $$1/
{
    @maps[arg0, arg1] = count();
}

tick-1s
/++secs >= $2/
{
    printf("\n== method calls: client, selector, scalars, struct-in bytes, ool-in bytes, count\n");
    printa("%p %d %d %d %d %@d\n", @calls);
    printf("\n== older/async entry points: function, client, selector, count\n");
    printa("%s %p %d %@d\n", @legacy);
    printf("\n== memory maps: client, memory type, count\n");
    printa("%p %d %@d\n", @maps);
    exit(0);
}
