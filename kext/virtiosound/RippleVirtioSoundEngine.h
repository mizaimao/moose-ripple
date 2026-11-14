/*
 * RippleVirtioSoundEngine: Core Audio's side of RippleVirtioSound. One output stream, 48 kHz,
 * 16-bit stereo. Core Audio mixes into a ring of kRingFrames frames; the engine copies a period
 * of the ring into a TX slot whenever the device returns one, so the device's pace (QEMU's
 * audio backend taking data at the host's rate) is the clock. A timestamp is taken each time
 * the copy position wraps to the ring's start.
 */
#ifndef RIPPLE_VIRTIO_SOUND_ENGINE_H
#define RIPPLE_VIRTIO_SOUND_ENGINE_H

#include <IOKit/audio/IOAudioEngine.h>

class RippleVirtioSound;
class IOAudioStream;
class IOAudioControl;

class RippleVirtioSoundEngine : public IOAudioEngine {
    OSDeclareDefaultStructors(RippleVirtioSoundEngine)

public:
    enum {
        kChannels     = 2,
        kBytesPerFrame = 4,               // S16 stereo
        kPeriodFrames = 512,              // 10.7 ms at 48 kHz
        kPeriods      = 16,
        kRingFrames   = kPeriodFrames * kPeriods,
        kPrimeSlots   = 4,                // slots in flight: about 43 ms queued at QEMU
        kVolumeSteps  = 64                // 0.75 dB per step, -48 dB .. 0 dB
    };

    bool init(RippleVirtioSound *device);
    virtual bool initHardware(IOService *provider);
    virtual void free();
    virtual void stop(IOService *provider);

    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    virtual UInt32   getCurrentSampleFrame();
    virtual IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat,
                                         const IOAudioSampleRate *newSampleRate);
    virtual IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame,
                                       UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat,
                                       IOAudioStream *audioStream);

    // The device returned a TX slot (called on the audio device's work loop).
    void txDone(unsigned slot);

    // Read by the clip routine (RippleVirtioSoundClip.cpp).
    volatile SInt32 fVolume;              // 0 .. kVolumeSteps
    volatile bool   fMute;

private:
    RippleVirtioSound *fDevice;
    IOAudioStream     *fStream;
    UInt8             *fRing;             // kRingFrames frames, Core Audio's sample buffer
    UInt32             fRingBytes;
    volatile UInt32    fNextFrame;        // next ring frame to copy into a slot
    bool               fRunning;
    bool               fWrapped;          // the copy position has left frame 0 once this run

    void submit(unsigned slot);
    static IOReturn volumeChanged(OSObject *target, IOAudioControl *control, SInt32 oldValue, SInt32 newValue);
    static IOReturn muteChanged(OSObject *target, IOAudioControl *control, SInt32 oldValue, SInt32 newValue);
};

#endif
