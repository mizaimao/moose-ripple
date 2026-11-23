/* Vista VMR9 needs an initially unlocked YUY2 video-processor destination. MIT. */
#define _WIN32_WINNT 0x0600
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <initguid.h>
#include <dxva2api.h>
#include <stdio.h>
#include <string.h>

static const BYTE input[2][8] = {
    {32, 64, 48, 160, 96, 100, 112, 192},
    {130, 80, 144, 128, 180, 112, 200, 144}
};

static HRESULT check_frame(IDirectXVideoProcessor *processor, IDirect3DSurface9 *src,
                           IDirect3DSurface9 *dst, BOOL stretch)
{
    DXVA2_VideoProcessBltParams blt = {0};
    DXVA2_VideoSample sample = {0};
    SetRect(&blt.TargetRect, 0, 0, 8, 4);
    blt.BackgroundColor.Y = 16u << 8;
    blt.BackgroundColor.Cb = blt.BackgroundColor.Cr = 128u << 8;
    blt.BackgroundColor.Alpha = 0xffff;
    blt.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    blt.Alpha.Value = blt.ProcAmpValues.Contrast.Value = blt.ProcAmpValues.Saturation.Value = 1;
    sample.SrcSurface = src;
    sample.End = 333333;
    sample.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    sample.PlanarAlpha.Value = 1;
    SetRect(&sample.SrcRect, 0, 0, 4, 2);
    if (stretch)
        sample.DstRect = blt.TargetRect;
    else
        SetRect(&sample.DstRect, 2, 1, 6, 3);
    HRESULT hr = IDirectXVideoProcessor_VideoProcessBlt(processor, dst, &blt, &sample, 1, NULL);
    if (FAILED(hr)) return hr;

    D3DLOCKED_RECT lock;
    hr = IDirect3DSurface9_LockRect(dst, &lock, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) return hr;
    unsigned errors = 0;
    for (int y = 0; y < 4; y++) {
        const BYTE *row = (const BYTE *)lock.pBits + y * lock.Pitch;
        for (int x = 0; x < 8; x++) {
            BYTE luma = 16, chroma = 128;
            if (stretch || (x >= 2 && x < 6 && y >= 1 && y < 3)) {
                const int sx = stretch ? x / 2 : x - 2;
                const int sy = stretch ? y / 2 : y - 1;
                luma = input[sy][2 * sx];
                chroma = input[sy][4 * (sx / 2) + (x % 2 ? 3 : 1)];
            }
            errors += (row[2 * x] != luma) + (row[2 * x + 1] != chroma);
        }
    }
    hr = IDirect3DSurface9_UnlockRect(dst);
    printf("YUY2 %s: %u of 64 bytes differ\n", stretch ? "stretch" : "copy with border", errors);
    return FAILED(hr) ? hr : (errors ? E_FAIL : S_OK);
}

#define CHECK(call) do { hr = (call); if (FAILED(hr)) { \
    printf("%s returned %08lx\n", #call, (unsigned long)hr); goto done; } } while (0)

int main(void)
{
    const D3DFORMAT fmt = (D3DFORMAT)MAKEFOURCC('Y', 'U', 'Y', '2');
    HWND window = CreateWindowA("STATIC", "Vista video processor", WS_OVERLAPPEDWINDOW,
                                0, 0, 320, 240, NULL, NULL, GetModuleHandle(NULL), NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *device = NULL;
    IDirectXVideoProcessorService *service = NULL;
    IDirectXVideoProcessor *processor = NULL;
    IDirect3DSurface9 *src = NULL, *dst = NULL;
    HRESULT hr = E_FAIL;
    if (!window || !d3d) goto done;
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    CHECK(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, window, D3DCREATE_MULTITHREADED |
          D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device));
    CHECK(DXVA2CreateVideoService(device, &IID_IDirectXVideoProcessorService, (void **)&service));
    DXVA2_VideoDesc desc = {0};
    desc.SampleWidth = 4;
    desc.SampleHeight = 2;
    desc.Format = fmt;
    desc.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    desc.InputSampleFreq.Numerator = desc.OutputFrameFreq.Numerator = 30;
    desc.InputSampleFreq.Denominator = desc.OutputFrameFreq.Denominator = 1;
    DXVA2_VideoProcessorCaps caps;
    CHECK(IDirectXVideoProcessorService_GetVideoProcessorCaps(service,
          &DXVA2_VideoProcProgressiveDevice, &desc, fmt, &caps));
    CHECK(IDirectXVideoProcessorService_CreateVideoProcessor(service,
          &DXVA2_VideoProcProgressiveDevice, &desc, fmt, 0, &processor));
    CHECK(IDirectXVideoProcessorService_CreateSurface(service, 4, 2, 0, fmt, caps.InputPool,
          0, DXVA2_VideoProcessorRenderTarget, &src, NULL));
    CHECK(IDirectXVideoProcessorService_CreateSurface(service, 8, 4, 0, fmt, D3DPOOL_DEFAULT,
          0, DXVA2_VideoProcessorRenderTarget, &dst, NULL));
    D3DLOCKED_RECT lock;
    CHECK(IDirect3DSurface9_LockRect(src, &lock, NULL, 0));
    memcpy(lock.pBits, input[0], sizeof(input[0]));
    memcpy((BYTE *)lock.pBits + lock.Pitch, input[1], sizeof(input[1]));
    CHECK(IDirect3DSurface9_UnlockRect(src));
    CHECK(check_frame(processor, src, dst, FALSE));
    CHECK(check_frame(processor, src, dst, TRUE));
done:
    if (dst) IDirect3DSurface9_Release(dst);
    if (src) IDirect3DSurface9_Release(src);
    if (processor) IDirectXVideoProcessor_Release(processor);
    if (service) IDirectXVideoProcessorService_Release(service);
    if (device) IDirect3DDevice9_Release(device);
    if (d3d) IDirect3D9_Release(d3d);
    if (window) DestroyWindow(window);
    return FAILED(hr);
}
