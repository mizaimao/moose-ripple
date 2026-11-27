/*
 * RippleGamma: the display's gamma table, applied by the driver.
 *
 * Mac OS X hands the framebuffer a gamma table (IOFramebuffer::setGammaTable) and counts on it:
 * its fades at login and logout, at display sleep, around a resolution change and when a
 * program captures the display are ramps of that table, and a display profile's calibration
 * curve is loaded into it. virtio-gpu has no gamma control. So while a table other than the
 * identity is set, the screen is a second image: what would be on screen, drawn through the
 * table by a small virgl context of the driver's own (a rectangle and one table lookup per
 * colour channel). RippleFramebuffer decides when to draw and what to scan out.
 *
 * Needs a host with virgl; without one the table is not applied.
 */
#ifndef RIPPLE_GAMMA_H
#define RIPPLE_GAMMA_H

#include <IOKit/IOBufferMemoryDescriptor.h>
#include "VirtioGPUDevice.h"

class RippleGamma
{
    VirtioGPUDevice          *fDev;
    IOBufferMemoryDescriptor *fCmd;      // the command buffer, physically contiguous
    uint64_t                  fCmdPA;
    uint8_t   fTable[3][256];            // the table's top eight bits, per channel
    bool      fIdentity, fDirty, fTableSent;
    uint32_t  fCtx;                      // the context (0: nothing made on the host yet)
    uint32_t  fImage, fTableRes, fVertices;
    uint32_t  fWidth, fHeight, fFormat;  // of the image
    bool      fTop;                      // the image's rows run top first, as its source's do

    bool create(uint32_t width, uint32_t height, uint32_t format, bool top);

public:
    bool init(VirtioGPUDevice *dev);
    void free();                         // host objects and memory

    // The table as IOFramebuffer::setGammaTable gets it: channelCount tables (one for all
    // channels, or red, green, blue) of dataCount entries, each dataWidth bits wide in a byte
    // or a 16-bit word. Returns true if what is shown has to change.
    bool setTable(uint32_t channelCount, uint32_t dataCount, uint32_t dataWidth, const void *data);
    bool active() const { return !fIdentity; }

    // Draw region r of resource `src` (srcWidth x srcHeight, virgl format number, `top`: its
    // rows run top first, as a 2D resource's do) through the table. Returns the image to scan
    // out (r.width x r.height, rows as the source's), or 0 if it could not be drawn.
    uint32_t draw(uint32_t src, uint32_t srcWidth, uint32_t srcHeight, uint32_t format, bool top,
                  const VirtioGPUDevice::Rect &r);
    // Give up what was made on the host (a mode change; the next draw makes it again).
    void destroy();
    // A check (`gamma-test probe`): row `row` of the image last drawn, counted from the top,
    // read back from the host and described in `out` (each byte's mean over the row, eight
    // texels). A screenshot in the guest never shows the table, and not every host can show
    // its display to a script (UTM). False if nothing has been drawn or the read failed.
    bool probe(uint32_t row, char *out, size_t outLen);
};

#endif
