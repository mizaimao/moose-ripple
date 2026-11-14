/*
 * RippleVirtioSoundEngine: see RippleVirtioSoundEngine.h. The clip routine (float mix to S16)
 * is in RippleVirtioSoundClip.cpp, which is built with floating point allowed.
 *
 * Timing: Core Audio places its writes from the engine's timestamps, one per pass over the
 * ring. The engine reads the ring a period at a time when the device returns a TX slot, and
 * takes a timestamp each time a read passes frame 0. Loop 0 is stamped at the start, and the
 * first read (after the device's start burst, see txDone) begins at the frame that matches the
 * time since then, so the reads follow Core Audio's idea of the position from the first one.
 * Core Audio writes kPrimeSlots periods ahead of it (setSampleOffset), so the data has been
 * written by the time it's read, and the erase head, which clears frames behind
 * getCurrentSampleFrame(), only clears frames already read.
 */
#include "RippleVirtioSoundEngine.h"
#include "RippleVirtioSound.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>

#include <linux/virtio_snd.h>

#include "RippleVirtioSoundTrace.h"

#ifdef RIPPLE_SND_TRACE
RippleSndEvent  gRippleSndTrace[kRippleSndTraceLen];
volatile SInt32 gRippleSndTraceN;
#endif

#define super IOAudioEngine
OSDefineMetaClassAndStructors(RippleVirtioSoundEngine, IOAudioEngine)

#define LOG(fmt, ...) IOLog("RippleVirtioSound: " fmt "\n", ##__VA_ARGS__)

enum { kPeriodBytes = RippleVirtioSoundEngine::kPeriodFrames * RippleVirtioSoundEngine::kBytesPerFrame };

bool RippleVirtioSoundEngine::init(RippleVirtioSound *device)
{
    if (!super::init(NULL) || !device)
        return false;
    fDevice = device;
    fVolume = kVolumeSteps;
    fMute = false;
    return true;
}

bool RippleVirtioSoundEngine::initHardware(IOService *provider)
{
    if (!super::initHardware(provider))
        return false;

    IOAudioSampleRate rate = { 48000, 0 };
    setSampleRate(&rate);
    setNumSampleFramesPerBuffer(kRingFrames);
    // How far ahead of the position Core Audio writes. QEMU takes the first slots faster than
    // real time (it fills its own buffer), so reads can run ahead of the timestamps by up to
    // the slots in flight; with one period of offset the end of each period was still unwritten
    // (zeros, every 10.7 ms, measured 2026-11-13).
    setSampleOffset(kPrimeSlots * kPeriodFrames);
    setOutputSampleLatency(kPrimeSlots * kPeriodFrames);
    nanoseconds_to_absolutetime(kPeriodFrames * 1000000000ULL / 48000 / 2, &fHalfPeriodAbs);
    nanoseconds_to_absolutetime(kRingFrames * 1000000000ULL / 48000, &fLoopNominal);
    setDescription("Virtio Sound");

    fRingBytes = kRingFrames * kBytesPerFrame;
    fRing = (UInt8 *)IOMallocAligned(fRingBytes, PAGE_SIZE);
    if (!fRing)
        return false;
    bzero(fRing, fRingBytes);

    fStream = new IOAudioStream;
    if (!fStream || !fStream->initWithAudioEngine(this, kIOAudioStreamDirectionOutput, 1))
        return false;
    fStream->setSampleBuffer(fRing, fRingBytes);
    IOAudioStreamFormat fmt = {
        kChannels, kIOAudioStreamSampleFormatLinearPCM, kIOAudioStreamNumericRepresentationSignedInt,
        16, 16, kIOAudioStreamAlignmentHighByte, kIOAudioStreamByteOrderLittleEndian, true, 0
    };
    fStream->addAvailableFormat(&fmt, &rate, &rate);
    fStream->setFormat(&fmt);
    addAudioStream(fStream);
    fStream->release();

    // Volume (0.75 dB steps, -48 dB to 0 dB) and mute, applied in the clip routine: the device
    // has no controls of its own.
    IOAudioLevelControl *vol = IOAudioLevelControl::createVolumeControl(kVolumeSteps, 0, kVolumeSteps,
        -(48 << 16), 0, kIOAudioControlChannelIDAll, kIOAudioControlChannelNameAll, 0,
        kIOAudioControlUsageOutput);
    if (vol) {
        vol->setValueChangeHandler(volumeChanged, this);
        addDefaultAudioControl(vol);
        vol->release();
    }
    IOAudioToggleControl *mute = IOAudioToggleControl::createMuteControl(false,
        kIOAudioControlChannelIDAll, kIOAudioControlChannelNameAll, 0, kIOAudioControlUsageOutput);
    if (mute) {
        mute->setValueChangeHandler(muteChanged, this);
        addDefaultAudioControl(mute);
        mute->release();
    }
    return true;
}

void RippleVirtioSoundEngine::free()
{
    if (fRing)
        IOFreeAligned(fRing, fRingBytes);
    super::free();
}

void RippleVirtioSoundEngine::stop(IOService *provider)
{
    fRunning = false;
    super::stop(provider);
}

IOReturn RippleVirtioSoundEngine::volumeChanged(OSObject *target, IOAudioControl *, SInt32, SInt32 newValue)
{
    RippleVirtioSoundEngine *e = OSDynamicCast(RippleVirtioSoundEngine, target);
    if (e)
        e->fVolume = newValue < 0 ? 0 : (newValue > kVolumeSteps ? kVolumeSteps : newValue);
    return kIOReturnSuccess;
}

IOReturn RippleVirtioSoundEngine::muteChanged(OSObject *target, IOAudioControl *, SInt32, SInt32 newValue)
{
    RippleVirtioSoundEngine *e = OSDynamicCast(RippleVirtioSoundEngine, target);
    if (e)
        e->fMute = newValue != 0;
    return kIOReturnSuccess;
}

// ---- Running ------------------------------------------------------------------------------------

// Copy the next period of the ring into a slot and give it to the device. Work loop thread.
// A read need not start on a period boundary (see txDone), so it may cross the ring's end.
// Each pass over the ring gets a timestamp near the time frame 0 is read (smoothed, below),
// taken once that time has passed.
void RippleVirtioSoundEngine::submit(unsigned slot, uint64_t now)
{
    if (fWrapPending && now >= fWrapTime) {
        fWrapPending = false;
        takeTimeStamp(true, (AbsoluteTime *)&fWrapTime);
        fLastStamp = fWrapTime;
        SND_TRACE(kTrWrap, 0, status ? status->fCurrentLoopCount : 0,
                  (uint32_t)(int32_t)(fWrapErr / 1000),
                  (uint32_t)((fWrapTime - fStartTime) / 1000));
    }
    UInt32 frame = fNextFrame;
    UInt32 first = kRingFrames - frame;
    if (first > kPeriodFrames)
        first = kPeriodFrames;
    SND_TRACE(kTrRead, 0, slot, frame, 0);
    UInt8 *dst = (UInt8 *)fDevice->txData(slot);
    memcpy(dst, fRing + frame * kBytesPerFrame, first * kBytesPerFrame);
    if (first < kPeriodFrames)
        memcpy(dst + first * kBytesPerFrame, fRing, (kPeriodFrames - first) * kBytesPerFrame);
    UInt32 next = frame + kPeriodFrames;
    if (next >= kRingFrames) {
        next -= kRingFrames;
        // When frame 0 is read, counting from this read's own time. Under emulation that time
        // jitters by up to about 15 ms (a ring pass measured 143 to 195 ms in UTM, 2026-11-14),
        // and Core Audio answers a pass that looks several percent long by jumping its timeline,
        // writing about 11 ms of silence. So the timestamp follows the measured time through a
        // small phase-locked loop: it moves an eighth of the way from the predicted time to the
        // measured one, and the predicted pass length an eighth of that. A big error (a stall)
        // is followed at once.
        uint64_t d;
        nanoseconds_to_absolutetime((uint64_t)(kRingFrames - frame) * 1000000000ULL / 48000, &d);
        uint64_t measured = now + d;
        uint64_t predicted = fLastStamp + fLoopAbs;
        int64_t err = (int64_t)(measured - predicted);
        int64_t limit = (int64_t)(fLoopNominal / 4);
        if (err > limit || err < -limit) {
            fWrapTime = measured;
        } else {
            fWrapTime = predicted + err / 8;
            fLoopAbs += err / 64;
            if (fLoopAbs > fLoopNominal + fLoopNominal / 100)
                fLoopAbs = fLoopNominal + fLoopNominal / 100;
            else if (fLoopAbs < fLoopNominal - fLoopNominal / 100)
                fLoopAbs = fLoopNominal - fLoopNominal / 100;
        }
        fWrapErr = err;
        fWrapPending = true;
    }
    fNextFrame = next;
    if (!fDevice->txSubmit(slot, kPeriodBytes))
        LOG("TX submit failed (slot %u)", slot);
}

void RippleVirtioSoundEngine::txDone(unsigned slot)
{
    if (!fRunning)
        return;
    // At the start QEMU takes several slots at once to fill its own buffer. Reading the ring
    // during that burst would make the first loop look short, and Core Audio would later jump
    // its timeline to correct it (a click, 'clipIfNecessary ... adjusting' in the kernel log).
    // So the burst gets silence, and the ring is first read at the first completion that comes
    // at the device's own pace, from the frame that matches the time since the start timestamp.
    // Loop 0 keeps that start timestamp: moving it to the first read made Core Audio re-sync
    // and write about 100 frames of silence 10 ms into every sound (measured 2026-11-14).
    uint64_t now = mach_absolute_time();
    if (!fReading) {
        bool paced = fLastDone && now - fLastDone > fHalfPeriodAbs;
        fLastDone = now;
        if (!paced) {
            SND_TRACE(kTrBurst, 0, slot, 0, 0);
            bzero(fDevice->txData(slot), kPeriodBytes);
            fDevice->txSubmit(slot, kPeriodBytes);
            return;
        }
        fReading = true;
        uint64_t ns;
        absolutetime_to_nanoseconds(now - fStartTime, &ns);
        uint64_t frame = ns * 48000 / 1000000000ULL;
        if (frame >= kRingFrames) {
            // The device took longer than a ring to settle: start loop 0 over from here.
            LOG("first paced completion after %llu us, loop 0 restarted", ns / 1000);
            takeTimeStamp(false, (AbsoluteTime *)&now);
            fLastStamp = now;
            frame = 0;
        }
        fNextFrame = (UInt32)frame;
        SND_TRACE(kTrAnchor, 0, slot, (uint32_t)frame, 0);
    }
    submit(slot, now);
}

IOReturn RippleVirtioSoundEngine::performAudioEngineStart()
{
#ifdef RIPPLE_SND_TRACE
    gRippleSndTraceN = 0;
#endif
    SND_TRACE(kTrStart, 0, 0, 0, 0);
    fDevice->drainTx();
    if (fDevice->txInFlight())
        LOG("start with %u TX slots still at the device", fDevice->txInFlight());
    if (!fDevice->pcmSetParams(fRingBytes, kPeriodBytes, kChannels, VIRTIO_SND_PCM_FMT_S16,
                               VIRTIO_SND_PCM_RATE_48000) ||
        !fDevice->pcmCommand(VIRTIO_SND_R_PCM_PREPARE)) {
        LOG("stream setup failed");
        return kIOReturnError;
    }
    fNextFrame = 0;
    fReading = false;
    fWrapPending = false;
    fLastDone = 0;
    bzero(fRing, fRingBytes);        // no samples left over from the last run
    fRunning = true;
    // Silence first: the device always holds kPrimeSlots periods, and every returned slot is
    // refilled from the ring.
    for (unsigned s = 0; s < kPrimeSlots; s++) {
        bzero(fDevice->txData(s), kPeriodBytes);
        fDevice->txSubmit(s, kPeriodBytes);
    }
    fStartTime = mach_absolute_time();
    takeTimeStamp(false, (AbsoluteTime *)&fStartTime);
    fLastStamp = fStartTime;
    fLoopAbs = fLoopNominal;
    if (!fDevice->pcmCommand(VIRTIO_SND_R_PCM_START)) {
        fRunning = false;
        LOG("PCM_START failed");
        return kIOReturnError;
    }
    SND_TRACE(kTrRun, 0, fDevice->txInFlight(), 0, 0);
    return kIOReturnSuccess;
}

IOReturn RippleVirtioSoundEngine::performAudioEngineStop()
{
    fRunning = false;
    fDevice->pcmCommand(VIRTIO_SND_R_PCM_STOP);
    fDevice->pcmCommand(VIRTIO_SND_R_PCM_RELEASE);  // the device hands back what it still holds
    fDevice->drainTx();
#ifdef RIPPLE_SND_TRACE
    SND_TRACE(kTrStop, 0, 0, 0, 0);
    SInt32 n = gRippleSndTraceN;
    if (n > kRippleSndTraceLen)
        n = kRippleSndTraceLen;
    OSData *d = OSData::withBytes(gRippleSndTrace, n * sizeof(RippleSndEvent));
    if (d) {
        setProperty("RippleSndTrace", d);
        d->release();
    }
#endif
    return kIOReturnSuccess;
}

UInt32 RippleVirtioSoundEngine::getCurrentSampleFrame()
{
    return fNextFrame;
}

IOReturn RippleVirtioSoundEngine::performFormatChange(IOAudioStream *, const IOAudioStreamFormat *,
                                                     const IOAudioSampleRate *newSampleRate)
{
    if (newSampleRate && newSampleRate->whole != 48000)
        return kIOReturnUnsupported;
    return kIOReturnSuccess;
}
