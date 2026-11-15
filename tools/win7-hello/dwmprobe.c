/*
 * The questions Windows 7's Desktop Window Manager asks about the display adapter before it
 * composes the desktop, asked in the same order and with the same arguments
 * (docs/windows/kmd-port-notes.md): which Direct3D 10 interfaces the adapter's driver has, which
 * WDDM version the kernel driver is, and whether a Direct3D 10.1 device can be made at each
 * feature level from 10.1 down to 9.1. The compositor takes the first level that works and
 * reports "WDDM is not in use" when none does. Writes dwmprobe.txt next to the program.
 * MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <stdio.h>

static const GUID iid_factory = { 0x7b7166ec, 0x21c7, 0x44ae, { 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69 } };
static const GUID iid_device = { 0x9b7e4c0f, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
static const GUID iid_device1 = { 0x9b7e4c8f, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };

/* gdi32's doors to the display kernel, with only the fields used here. Taken from gdi32.dll by
 * name at run time: dxgi.dll exports functions of the same names that only say "not
 * implemented", and a linker may pick those. */
struct open_gdi { WCHAR name[32]; UINT adapter; LUID luid; UINT source; };
struct query_info { UINT adapter; UINT type; void *data; UINT size; };
struct close_adapter { UINT adapter; };
typedef LONG (WINAPI *kmt_fn)(void *);
#define QUERY_DRIVER_VERSION 13

/* The flags the compositor passes: single-threaded, no internal threads, a null from Map
 * allowed, layers not changed from the registry. */
#define DWM_DEVICE_FLAGS 0x99

/* From d3d10_1.dll at run time: not every toolchain has an import library for it */
typedef HRESULT (WINAPI *create_device1_fn)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1,
                                           UINT, ID3D10Device1 **);

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    static const D3D10_FEATURE_LEVEL1 levels[] = {
        D3D10_FEATURE_LEVEL_10_1, D3D10_FEATURE_LEVEL_10_0, D3D10_FEATURE_LEVEL_9_3,
        D3D10_FEATURE_LEVEL_9_2, D3D10_FEATURE_LEVEL_9_1,
    };
    FILE *f = fopen("dwmprobe.txt", "wb");
    create_device1_fn create_device1;
    HMODULE gdi = LoadLibraryA("gdi32.dll");
    kmt_fn kmt_open = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName");
    kmt_fn kmt_query = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    kmt_fn kmt_close = (kmt_fn)(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    LARGE_INTEGER version;
    struct open_gdi og;
    HRESULT hr;
    unsigned i;
    (void)inst; (void)prev; (void)cmd; (void)show;

    if (!f)
        return 1;
    setvbuf(f, NULL, _IONBF, 0);   /* every line reaches the file, also if the next call crashes */
    hr = CreateDXGIFactory(&iid_factory, (void **)&factory);
    fprintf(f, "DXGI factory: 0x%08lx\r\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
    fprintf(f, "adapter 0: 0x%08lx\r\n", (unsigned long)hr);
    if (FAILED(hr))
        return 1;
    hr = IDXGIAdapter_GetDesc(adapter, &desc);
    if (SUCCEEDED(hr))
        fprintf(f, "  %ls, vendor %04x device %04x, %lu MB video memory, %lu MB shared\r\n", desc.Description,
                desc.VendorId, desc.DeviceId, (unsigned long)(desc.DedicatedVideoMemory >> 20),
                (unsigned long)(desc.SharedSystemMemory >> 20));
    version.QuadPart = 0;
    hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &iid_device1, &version);
    fprintf(f, "Direct3D 10.1 interface: 0x%08lx, driver version %08lx%08lx\r\n", (unsigned long)hr,
            (unsigned long)version.HighPart, (unsigned long)version.LowPart);
    version.QuadPart = 0;
    hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &iid_device, &version);
    fprintf(f, "Direct3D 10 interface: 0x%08lx, driver version %08lx%08lx\r\n", (unsigned long)hr,
            (unsigned long)version.HighPart, (unsigned long)version.LowPart);

    memset(&og, 0, sizeof(og));
    for (i = 0; ; i++) {
        DISPLAY_DEVICEW dd;
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0))
            break;
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            lstrcpynW(og.name, dd.DeviceName, 32);
            break;
        }
    }
    if (kmt_open && kmt_query && kmt_close && kmt_open(&og) >= 0) {
        UINT wddm = 0;
        struct query_info q = { og.adapter, QUERY_DRIVER_VERSION, &wddm, sizeof(wddm) };
        struct close_adapter c = { og.adapter };
        LONG st = kmt_query(&q);
        fprintf(f, "kernel driver's WDDM version: 0x%08lx, %u (1105 is WDDM 1.1; below it the compositor "
                "stays at feature level 9.3)\r\n", (unsigned long)st, wddm);
        kmt_close(&c);
    } else {
        fprintf(f, "kernel driver's WDDM version: the adapter of the primary display (%ls) did not open\r\n", og.name);
    }

    create_device1 = (create_device1_fn)(void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1");
    if (!create_device1) {
        fprintf(f, "no D3D10CreateDevice1 in d3d10_1.dll\r\n");
        return 1;
    }
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        ID3D10Device1 *dev = NULL;
        fprintf(f, "device at feature level 0x%04x: ", (unsigned)levels[i]);
        hr = create_device1(adapter, D3D10_DRIVER_TYPE_HARDWARE, NULL, DWM_DEVICE_FLAGS, levels[i],
                            D3D10_1_SDK_VERSION, &dev);
        fprintf(f, "0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr) && dev) {
            fprintf(f, ", made, level 0x%04x", (unsigned)ID3D10Device1_GetFeatureLevel(dev));
            ID3D10Device1_Release(dev);
        }
        fprintf(f, "\r\n");
    }
    IDXGIAdapter_Release(adapter);
    IDXGIFactory_Release(factory);
    fclose(f);
    return 0;
}
