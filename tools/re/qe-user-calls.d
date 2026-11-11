/*
 * User-space view of one running process's graphics calls for SECONDS seconds: the GL driver
 * bundle's gld* entry points, the 2D (GA) plugin's functions, IOKit's IOAccel* surface calls
 * and OpenGL's CGL* calls, with each function's first call (arguments) and counts.
 *   sudo dtrace -s qe-user-calls.d -p PID GLMOD GAMOD SECONDS
 *   e.g. -p 66 GeForceGLDriver GeForceGA 15
 * (M5 R2.)
 */
#pragma D option quiet
#pragma D option bufsize=16m

dtrace:::BEGIN { start = timestamp; }

pid$target:$1:gld*:entry,
pid$target:$2::entry,
pid$target:IOKit:IOAccel*:entry,
pid$target:OpenGL:CGL*:entry
/!seen[probemod, probefunc]/
{
    seen[probemod, probefunc] = 1;
    printf("%6d ms first %s:%s(%x, %x, %x, %x) from %A\n", (timestamp - start) / 1000000,
        probemod, probefunc, arg0, arg1, arg2, arg3, ucaller);
}

pid$target:$1:gld*:entry,
pid$target:$2::entry,
pid$target:IOKit:IOAccel*:entry,
pid$target:OpenGL:CGL*:entry
{
    @n[probemod, probefunc] = count();
}

tick-1s
/++secs >= $3/
{
    printf("\n== counts: module, function, calls\n");
    printa("%s %s %@d\n", @n);
    exit(0);
}
