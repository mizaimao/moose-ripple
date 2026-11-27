/*
 * cam-check: does the default camera deliver frames, without any OpenGL in the way? Opens it with
 * QTKit (QTCaptureSession, decompressed video output, 32-bit BGRA), counts the frames that arrive
 * in SECONDS (default 5), notes how many differ from the frame before (a checksum of every 64th
 * pixel), and writes the last one as a PPM. A camera picture that Photo Booth shows black or
 * frozen can be told apart from one that never arrives this way.
 *   [CAM_SIZE=WxH] [CAM_AUDIO=1] cam-check [SECONDS] [OUT.ppm]   (default /tmp/cam-check.ppm)
 * CAM_SIZE asks for that picture size. CAM_AUDIO=1 adds the default sound input to the session,
 * as Photo Booth and QuickTime Player do: a microphone that fails stops the video as well (the
 * C930e's microphone through UTM did, issue 11). The last frame is also written as OUT.jpg; it
 * shows wherever the camera points, so keep it out of the repository and reports.
 *   cam-check inputs [NAME]    the sound inputs; with NAME, make that one the default

 * Written for moose-ripple; built without ARC (10.6).
 */
#import <Cocoa/Cocoa.h>
#import <QTKit/QTKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface Counter : NSObject {
@public
    unsigned frames, changed;
    uint32_t last_sum;
    size_t w, h;
    uint8_t *copy;
    size_t copy_size;
}
@end

@implementation Counter
- (void)captureOutput:(QTCaptureOutput *)output didOutputVideoFrame:(CVImageBufferRef)frame
     withSampleBuffer:(QTSampleBuffer *)sample fromConnection:(QTCaptureConnection *)conn
{
    CVPixelBufferLockBaseAddress(frame, 0);
    const uint8_t *base = CVPixelBufferGetBaseAddress(frame);
    size_t rb = CVPixelBufferGetBytesPerRow(frame);
    size_t fw = CVPixelBufferGetWidth(frame), fh = CVPixelBufferGetHeight(frame);
    uint32_t sum = 2166136261u;
    for (size_t y = 0; base && y < fh; y += 8)
        for (size_t x = 0; x < fw; x += 8)
            sum = (sum ^ base[y * rb + x * 4 + 1]) * 16777619u;
    @synchronized (self) {
        frames++;
        if (frames == 1 || sum != last_sum)
            changed++;
        last_sum = sum;
        if (base && fw * 4 <= rb) {
            if (copy_size < fw * fh * 4) {
                free(copy);
                copy_size = fw * fh * 4;
                copy = malloc(copy_size);
            }
            for (size_t y = 0; copy && y < fh; y++)
                memcpy(copy + y * fw * 4, base + y * rb, fw * 4);
            w = fw;
            h = fh;
        }
    }
    CVPixelBufferUnlockBaseAddress(frame, 0);
}
@end

#include <CoreAudio/CoreAudio.h>

/* cam-check inputs [NAME]: the sound inputs, the default one marked with *; with NAME, the first
 * input whose name starts with NAME becomes the default (System Preferences > Sound > Input does
 * the same). */
static int sound_inputs(const char *want)
{
    AudioObjectPropertyAddress all = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMaster };
    AudioObjectPropertyAddress def = { kAudioHardwarePropertyDefaultInputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMaster };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &all, 0, NULL, &size) || !size)
        return 1;
    AudioDeviceID *ids = malloc(size), current = 0;
    UInt32 csize = sizeof(current);
    if (!ids || AudioObjectGetPropertyData(kAudioObjectSystemObject, &all, 0, NULL, &size, ids))
        return 1;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, NULL, &csize, &current);
    bool chosen = false;
    for (UInt32 i = 0; i < size / sizeof(AudioDeviceID); i++) {
        AudioObjectPropertyAddress streams = { kAudioDevicePropertyStreams,
                                               kAudioDevicePropertyScopeInput,
                                               kAudioObjectPropertyElementMaster };
        UInt32 ssize = 0;
        if (AudioObjectGetPropertyDataSize(ids[i], &streams, 0, NULL, &ssize) || !ssize)
            continue;                       /* no input side */
        AudioObjectPropertyAddress pname = { kAudioObjectPropertyName,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMaster };
        CFStringRef name = NULL;
        UInt32 nsize = sizeof(name);
        char buf[256] = "";
        if (!AudioObjectGetPropertyData(ids[i], &pname, 0, NULL, &nsize, &name) && name) {
            CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
            CFRelease(name);
        }
        if (want && !chosen && !strncmp(buf, want, strlen(want))) {
            chosen = true;
            OSStatus st = AudioObjectSetPropertyData(kAudioObjectSystemObject, &def, 0, NULL,
                                                     sizeof(ids[i]), &ids[i]);
            printf("default input now %s (status %d)\n", buf, (int)st);
            if (!st)
                current = ids[i];
        }
        printf("%s %s\n", ids[i] == current ? "*" : " ", buf);
    }
    free(ids);
    if (want && !chosen)
        printf("no input named %s...\n", want);
    return want && !chosen;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "inputs"))
        return sound_inputs(argc > 2 ? argv[2] : NULL);
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double seconds = argc > 1 ? atof(argv[1]) : 5;
    const char *out = argc > 2 ? argv[2] : "/tmp/cam-check.ppm";
    NSError *err = nil;

    NSArray *devices = [QTCaptureDevice inputDevicesWithMediaType:QTMediaTypeVideo];
    NSArray *muxed = [QTCaptureDevice inputDevicesWithMediaType:QTMediaTypeMuxed];
    printf("video devices: %lu, muxed: %lu\n", (unsigned long)[devices count],
           (unsigned long)[muxed count]);
    for (QTCaptureDevice *d in devices)
        printf("  %s (%s)\n", [[d localizedDisplayName] UTF8String], [[d modelUniqueID] UTF8String]);
    QTCaptureDevice *dev = [QTCaptureDevice defaultInputDeviceWithMediaType:QTMediaTypeVideo];
    if (!dev) {
        printf("no default video device\n");
        return 1;
    }
    if (![dev open:&err]) {
        printf("open failed: %s\n", [[err localizedDescription] UTF8String]);
        return 1;
    }
    QTCaptureSession *session = [[QTCaptureSession alloc] init];
    QTCaptureDeviceInput *input = [[QTCaptureDeviceInput alloc] initWithDevice:dev];
    if (![session addInput:input error:&err]) {
        printf("add input failed: %s\n", [[err localizedDescription] UTF8String]);
        return 1;
    }
    QTCaptureDecompressedVideoOutput *output = [[QTCaptureDecompressedVideoOutput alloc] init];
    /* CAM_SIZE=WxH asks for that size (Photo Booth works at 640x480); the camera may switch
     * to another streaming mode for it. */
    NSMutableDictionary *attrs = [NSMutableDictionary dictionaryWithObject:
        [NSNumber numberWithUnsignedInt:kCVPixelFormatType_32BGRA]
        forKey:(id)kCVPixelBufferPixelFormatTypeKey];
    unsigned rw = 0, rh = 0;
    if (getenv("CAM_SIZE") && sscanf(getenv("CAM_SIZE"), "%ux%u", &rw, &rh) == 2) {
        [attrs setObject:[NSNumber numberWithUnsignedInt:rw] forKey:(id)kCVPixelBufferWidthKey];
        [attrs setObject:[NSNumber numberWithUnsignedInt:rh] forKey:(id)kCVPixelBufferHeightKey];
    }
    [output setPixelBufferAttributes:attrs];
    Counter *counter = [[Counter alloc] init];
    [output setDelegate:counter];
    if (![session addOutput:output error:&err]) {
        printf("add output failed: %s\n", [[err localizedDescription] UTF8String]);
        return 1;
    }
    /* CAM_AUDIO=1: the default sound input too, as Photo Booth and QuickTime Player open it. */
    if (getenv("CAM_AUDIO")) {
        QTCaptureDevice *mic = [QTCaptureDevice defaultInputDeviceWithMediaType:QTMediaTypeSound];
        printf("sound input: %s\n", mic ? [[mic localizedDisplayName] UTF8String] : "none");
        if (mic && ![mic open:&err])
            printf("sound input open failed: %s (%ld)\n", [[err localizedDescription] UTF8String],
                   (long)[err code]);
        else if (mic && ![session addInput:[[QTCaptureDeviceInput alloc] initWithDevice:mic] error:&err])
            printf("add sound input failed: %s (%ld)\n", [[err localizedDescription] UTF8String],
                   (long)[err code]);
        QTCaptureAudioPreviewOutput *ao = [[QTCaptureAudioPreviewOutput alloc] init];
        [ao setVolume:0];
        if (![session addOutput:ao error:&err])
            printf("add sound output failed: %s (%ld)\n", [[err localizedDescription] UTF8String],
                   (long)[err code]);
    }
    [[NSNotificationCenter defaultCenter] addObserverForName:QTCaptureSessionRuntimeErrorNotification
        object:session queue:nil usingBlock:^(NSNotification *n) {
            NSError *e = [[n userInfo] objectForKey:QTCaptureSessionErrorKey];
            printf("session error: %s (%ld)\n", [[e localizedDescription] UTF8String], (long)[e code]);
        }];
    [session startRunning];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    [session stopRunning];

    @synchronized (counter) {
        printf("%s: %u frames in %.1f s (%.1f a second), %u of them different from the one "
               "before; %zux%zu\n", [[dev localizedDisplayName] UTF8String], counter->frames,
               seconds, counter->frames / seconds, counter->changed, counter->w, counter->h);
        if (counter->copy && counter->w && counter->h) {
            unsigned long bright = 0;
            FILE *f = fopen(out, "wb");
            if (f)
                fprintf(f, "P6\n%zu %zu\n255\n", counter->w, counter->h);
            for (size_t i = 0; i < counter->w * counter->h; i++) {
                const uint8_t *p = counter->copy + i * 4;
                uint8_t rgb[3] = { p[2], p[1], p[0] };
                bright += rgb[0] + rgb[1] + rgb[2];
                if (f)
                    fwrite(rgb, 1, 3, f);
            }
            if (f)
                fclose(f);
            printf("last frame: mean brightness %.1f of 255, written to %s\n",
                   bright / 3.0 / (counter->w * counter->h), out);
            /* The same frame as a JPEG next to it (OUT.jpg), drawn by the CPU, to look at. */
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
                initWithBitmapDataPlanes:NULL pixelsWide:counter->w pixelsHigh:counter->h
                bitsPerSample:8 samplesPerPixel:3 hasAlpha:NO isPlanar:NO
                colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:counter->w * 3 bitsPerPixel:24];
            uint8_t *dst = [rep bitmapData];
            for (size_t i = 0; dst && i < counter->w * counter->h; i++) {
                const uint8_t *p = counter->copy + i * 4;
                dst[i * 3] = p[2];
                dst[i * 3 + 1] = p[1];
                dst[i * 3 + 2] = p[0];
            }
            NSData *jpg = [rep representationUsingType:NSJPEGFileType properties:nil];
            NSString *jp = [[NSString stringWithUTF8String:out] stringByAppendingString:@".jpg"];
            [jpg writeToFile:jp atomically:NO];
            [rep release];
        }
    }
    [dev close];
    [pool release];
    return 0;
}
