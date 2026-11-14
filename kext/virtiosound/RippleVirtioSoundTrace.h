/*
 * A timing trace for RippleVirtioSoundEngine, compiled in only with -DRIPPLE_SND_TRACE
 * (e.g. make virtiosound CXX='clang++ -DRIPPLE_SND_TRACE'). Each engine start clears it; it
 * records the device's TX completions, the ring reads, the timestamps and every clip call (Core
 * Audio's writes into the ring), and each stop publishes it as the engine's "RippleSndTrace"
 * registry property: `ioreg -l -w0 -r -c RippleVirtioSoundEngine -k RippleSndTrace` prints
 * the records as hex. A record is 24 bytes, little-endian:
 *   u64 time (mach_absolute_time, ns on x86), u16 type, u16 flag, u32 a, u32 b, u32 c.
 */
#ifndef RIPPLE_VIRTIO_SOUND_TRACE_H
#define RIPPLE_VIRTIO_SOUND_TRACE_H

#ifdef RIPPLE_SND_TRACE
#include <libkern/OSAtomic.h>
#include <kern/clock.h>

struct RippleSndEvent {
    uint64_t t;
    uint16_t type, flag;
    uint32_t a, b, c;
};

enum {
    kTrStart = 1,   // performAudioEngineStart entered
    kTrRun,         // PCM_START sent                     a = TX slots in flight
    kTrBurst,       // completion during the start burst  a = slot
    kTrAnchor,      // first paced completion: loop 0 stamped here
    kTrRead,        // a period copied from the ring      a = slot, b = first frame
    kTrWrap,        // takeTimeStamp at the ring's start  a = loop count after it
    kTrClip,        // Core Audio wrote into the ring     a = first frame, b = frames,
                    //   c = read position then, flag = 1 if the mix had any non-zero sample
    kTrStop         // performAudioEngineStop
};

enum { kRippleSndTraceLen = 4096 };
extern RippleSndEvent   gRippleSndTrace[kRippleSndTraceLen];
extern volatile SInt32  gRippleSndTraceN;

static inline void rippleSndTrace(uint16_t type, uint16_t flag, uint32_t a, uint32_t b, uint32_t c)
{
    SInt32 i = OSIncrementAtomic(&gRippleSndTraceN);
    if (i < 0 || i >= kRippleSndTraceLen)
        return;
    RippleSndEvent &e = gRippleSndTrace[i];
    e.t = mach_absolute_time();
    e.type = type;
    e.flag = flag;
    e.a = a;
    e.b = b;
    e.c = c;
}
#define SND_TRACE(type, flag, a, b, c) rippleSndTrace((type), (flag), (a), (b), (c))
#else
#define SND_TRACE(type, flag, a, b, c) do { } while (0)
#endif

#endif
