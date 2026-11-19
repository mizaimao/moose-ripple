/*
 * SPDX-License-Identifier: MIT
 *
 * The display's gamma ramp, applied by the driver. Written for moose-ripple.
 */
#pragma once
#include "helper.h"

class VioGpuAdapter;
class VioGpuAllocation;

// The display's gamma ramp, shown.
//
// Windows fades the screen with the ramp (at logon: to black in half a second, the desktop
// switch, and back), and programs set it for brightness. virtio-gpu has no gamma control. So
// while a ramp other than the identity is set, the screen is a second image: the surface that
// would be on screen, drawn through the ramp by a small virgl context of the driver's own. The
// flip thread redraws it whenever the surface, a piece of it or the ramp changed, and puts the
// surface itself back on screen once the ramp is the identity again.
class VioGpuGamma
{
  public:
    void Init(VioGpuAdapter *adapter);

    // The ramp Windows wants, three times 256 sixteen-bit values; all NULL for the identity.
    // From any thread at PASSIVE_LEVEL.
    void SetRamp(const USHORT *red, const USHORT *green, const USHORT *blue);

    // The rest is the flip thread's, or runs once that thread is gone.
    BOOLEAN Active();  // a ramp other than the identity is set
    BOOLEAN Changed(); // and it changed since the screen was last drawn through it
    BOOLEAN Showing()
    {
        return m_showing;
    }
    // Draw SOURCE through the ramp and show that
    void Show(VioGpuAllocation *source);
    // The caller puts the surface itself back on screen
    void Hide()
    {
        m_showing = FALSE;
    }
    // Free what was made on the host
    void Destroy();

  private:
    BOOLEAN Create(UINT width, UINT height, UINT format);
    void UploadTable(const UCHAR *table);
    void Submit(UINT *words, UINT count);

    VioGpuAdapter *m_pAdapter;
    KSPIN_LOCK m_lock;
    UCHAR m_table[3][256]; // the ramp's top eight bits
    BOOLEAN m_identity;
    BOOLEAN m_dirty;

    UINT m_ctx; // the context; 0: nothing made yet
    UINT m_shadow;
    UINT m_lut;
    UINT m_vertices;
    UINT m_width;
    UINT m_height;
    UINT m_format;
    BOOLEAN m_tableSent;
    BOOLEAN m_showing;
};
