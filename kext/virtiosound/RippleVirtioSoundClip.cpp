/*
 * The clip routine: Core Audio's float mix (-1.0 .. 1.0, interleaved) to the ring's signed
 * 16-bit little-endian samples, with the software volume and mute. Built without -msoft-float
 * (Makefile), like the clip routines of Apple's IOAudioFamily drivers; nothing else in this
 * kext uses floating point.
 */
#include "RippleVirtioSoundEngine.h"
#include "RippleVirtioSoundTrace.h"

IOReturn RippleVirtioSoundEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf,
                                                    UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                                    const IOAudioStreamFormat *streamFormat,
                                                    IOAudioStream *)
{
    UInt32 channels = streamFormat->fNumChannels;
    const float *in = (const float *)mixBuf + firstSampleFrame * channels;
    SInt16 *out = (SInt16 *)sampleBuf + firstSampleFrame * channels;
    UInt32 n = numSampleFrames * channels;
#ifdef RIPPLE_SND_TRACE
    uint16_t sound = 0;
    for (UInt32 i = 0; i < n && !sound; i++)
        sound = in[i] != 0.0f;
    SND_TRACE(kTrClip, sound, firstSampleFrame, numSampleFrames, fNextFrame);
#endif

    float gain = 0.0f;
    if (!fMute) {
        SInt32 v = fVolume;
        if (v > 0) {
            gain = 1.0f;                            // 0 dB at kVolumeSteps, 0.75 dB a step down
            for (SInt32 i = v; i < kVolumeSteps; i++)
                gain *= 0.917276f;                  // 10^(-0.75/20)
        }
    }
    float scale = gain * 32767.0f;
    for (UInt32 i = 0; i < n; i++) {
        float s = in[i] * scale;
        if (s > 32767.0f)
            s = 32767.0f;
        else if (s < -32768.0f)
            s = -32768.0f;
        out[i] = (SInt16)s;
    }
    return kIOReturnSuccess;
}
