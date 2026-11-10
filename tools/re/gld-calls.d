/*
 * Log every gld* call into one GL driver bundle, in order, with the first six arguments and
 * the return value, plus the IOKit calls the driver makes in between.
 *   sudo dtrace -q -Z -c PROGRAM -s gld-calls.d DRIVER-MODULE    e.g. GeForceGLDriver
 * -Z -c: dtrace starts the program and adds the driver probes when OpenGL loads the bundle.
 * Lines start with a timestamp: per-CPU buffers come out of order, so `sort -n` the output.
 * Names like glrCompExecuteKernel+0x... on internal code are just the nearest exported symbol.
 */
pid$target:$1:gld*:entry
{
    printf("%d %4d %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x)\n", timestamp, ++n, probefunc,
           arg0, arg1, arg2, arg3, arg4, arg5);
}
pid$target:$1:gld*:return
{ printf("%d        %s -> 0x%x\n", timestamp, probefunc, arg1); }

pid$target:IOKit:IOServiceOpen:entry
{ self->conn = arg3; self->type = arg2; self->caller = ucaller; }
pid$target:IOKit:IOServiceOpen:return
/self->conn/
{
    printf("%d        IOServiceOpen type %d -> conn 0x%x (from %A)\n", timestamp, self->type,
           *(uint32_t *)copyin(self->conn, 4), self->caller);
    self->conn = 0;
}
pid$target:IOKit:IOConnectCallMethod:entry
{
    printf("%d        IOConnectCallMethod conn 0x%x sel %d, %d scalars in, %d bytes in (from %A)\n",
           timestamp, arg0, arg1, arg3, arg5, ucaller);
}
pid$target:IOKit:IOConnectMapMemory:entry
{ printf("%d        IOConnectMapMemory conn 0x%x type %d (from %A)\n", timestamp, arg0, arg1, ucaller); }
pid$target:IOKit:IOConnectAddClient:entry
{ printf("%d        IOConnectAddClient conn 0x%x + 0x%x (from %A)\n", timestamp, arg0, arg1, ucaller); }
