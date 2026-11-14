/*
 * RippleVirtioSoundEngine: see RippleVirtioSoundEngine.h. The clip routine (float mix to S16)
 * is in RippleVirtioSoundClip.cpp, which is built with floating point allowed.
 *
 * Timing: Core Audio places its writes from the engine's timestamps, one per pass over the
 * ring. The engine reads the ring a period at a time when the device returns a TX slot, and
 * takes the timestamp when that read wraps to frame 0. The first reads of a run come one or
 * more periods after the start timestamp (the device first plays kPrimeSlots periods of
 * silence), so the reads trail Core Audio's idea of the position: the data has been written
 * by the time it's read, and the erase head, which clears frames behind getCurrentSampleFrame(),
 * only clears frames already read.
 */
#include "RippleVirtioSoundEngine.h"
#include "RippleVirtioSound.h"

#include <IOKit/IOLib.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>

#include <linux/virtio_snd.h>

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
void RippleVirtioSoundEngine::submit(unsigned slot)
{
    UInt32 frame = fNextFrame;
    if (frame == 0) {
        if (fWrapped)
            takeTimeStamp();            // the read is back at the ring's start: one loop done
        fWrapped = true;
    }
    memcpy(fDevice->txData(slot), fRing + frame * kBytesPerFrame, kPeriodBytes);
    fNextFrame = (frame + kPeriodFrames) % kRingFrames;
    if (!fDevice->txSubmit(slot, kPeriodBytes))
        LOG("TX submit failed (slot %u)", slot);
}

void RippleVirtioSoundEngine::txDone(unsigned slot)
{
    if (fRunning)
        submit(slot);
}

IOReturn RippleVirtioSoundEngine::performAudioEngineStart()
{
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
    fWrapped = false;
    fRunning = true;
    // Silence first: the device always holds kPrimeSlots periods, and every returned slot is
    // refilled from the ring.
    for (unsigned s = 0; s < kPrimeSlots; s++) {
        bzero(fDevice->txData(s), kPeriodBytes);
        fDevice->txSubmit(s, kPeriodBytes);
    }
    takeTimeStamp(false);
    if (!fDevice->pcmCommand(VIRTIO_SND_R_PCM_START)) {
        fRunning = false;
        LOG("PCM_START failed");
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

IOReturn RippleVirtioSoundEngine::performAudioEngineStop()
{
    fRunning = false;
    fDevice->pcmCommand(VIRTIO_SND_R_PCM_STOP);
    fDevice->pcmCommand(VIRTIO_SND_R_PCM_RELEASE);  // the device hands back what it still holds
    fDevice->drainTx();
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
