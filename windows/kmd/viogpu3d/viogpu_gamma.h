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
    VioGpuGamma() : m_pending(NULL)
    {
    }
    ~VioGpuGamma();
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

    // Between one surface and the next there is nothing to draw. Ramps that arrive then are
    // kept in order (Hold) and applied once a surface is there again, as far apart as they came (Next), so
    // that a fade is seen whole when its first frame comes late: at logon Windows raises the ramp
    // for half a second from the desktop switch on, and an emulated guest's compositor takes
    // longer than that to produce the desktop's first frame.
    void Hold(BOOLEAN hold);
    void Next();

  private:
    BOOLEAN Create(UINT width, UINT height, UINT format);
    void UploadTable(const UCHAR *table);
    void Submit(UINT *words, UINT count);

    VioGpuAdapter *m_pAdapter;
    KSPIN_LOCK m_lock;
    UCHAR m_table[3][256]; // the ramp's top eight bits
    BOOLEAN m_identity;
    BOOLEAN m_dirty;
    enum
    {
        PENDING = 64
    };
    UCHAR (*m_pending)[3][256]; // ramps that wait for a surface, a ring
    ULONGLONG m_pendingTime[PENDING]; // when each came (interrupt time)
    UINT m_pendingHead;
    UINT m_pendingCount;
    BOOLEAN m_hold;
    BOOLEAN m_replaying;   // the waiting ramps are being applied, each as much later
    ULONGLONG m_replayShift; // as the first of them was

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
