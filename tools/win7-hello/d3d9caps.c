/* Everything a Direct3D 9 program learns about the HAL adapter, in a fixed order, so the
 * answers of two drivers (or of one driver on two Windows versions) can be compared with diff:
 * the adapter identifier, every D3DCAPS9 field (named and raw), device creation with each kind
 * of vertex processing, CheckDeviceFormat for every format, resource type and usage,
 * depth-stencil matches, multisample types and format conversions.
 *   d3d9caps [OUT.txt]      (stdout without an argument)
 * Built for x86 and x64 by build-ui-probes.sh. MIT licence (LICENSE at the repository root).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static FILE *out;

#define FCC(a, b, c, d) ((D3DFORMAT)MAKEFOURCC(a, b, c, d))

struct named_format { const char *name; D3DFORMAT format; };

static const struct named_format formats[] = {
    {"R8G8B8", D3DFMT_R8G8B8}, {"A8R8G8B8", D3DFMT_A8R8G8B8}, {"X8R8G8B8", D3DFMT_X8R8G8B8},
    {"R5G6B5", D3DFMT_R5G6B5}, {"X1R5G5B5", D3DFMT_X1R5G5B5}, {"A1R5G5B5", D3DFMT_A1R5G5B5},
    {"A4R4G4B4", D3DFMT_A4R4G4B4}, {"R3G3B2", D3DFMT_R3G3B2}, {"A8", D3DFMT_A8},
    {"A8R3G3B2", D3DFMT_A8R3G3B2}, {"X4R4G4B4", D3DFMT_X4R4G4B4},
    {"A2B10G10R10", D3DFMT_A2B10G10R10}, {"A8B8G8R8", D3DFMT_A8B8G8R8},
    {"X8B8G8R8", D3DFMT_X8B8G8R8}, {"G16R16", D3DFMT_G16R16}, {"A2R10G10B10", D3DFMT_A2R10G10B10},
    {"A16B16G16R16", D3DFMT_A16B16G16R16}, {"A8P8", D3DFMT_A8P8}, {"P8", D3DFMT_P8},
    {"L8", D3DFMT_L8}, {"A8L8", D3DFMT_A8L8}, {"A4L4", D3DFMT_A4L4}, {"V8U8", D3DFMT_V8U8},
    {"L6V5U5", D3DFMT_L6V5U5}, {"X8L8V8U8", D3DFMT_X8L8V8U8}, {"Q8W8V8U8", D3DFMT_Q8W8V8U8},
    {"V16U16", D3DFMT_V16U16}, {"A2W10V10U10", D3DFMT_A2W10V10U10}, {"UYVY", D3DFMT_UYVY},
    {"R8G8_B8G8", D3DFMT_R8G8_B8G8}, {"YUY2", D3DFMT_YUY2}, {"G8R8_G8B8", D3DFMT_G8R8_G8B8},
    {"DXT1", D3DFMT_DXT1}, {"DXT2", D3DFMT_DXT2}, {"DXT3", D3DFMT_DXT3}, {"DXT4", D3DFMT_DXT4},
    {"DXT5", D3DFMT_DXT5}, {"D16_LOCKABLE", D3DFMT_D16_LOCKABLE}, {"D32", D3DFMT_D32},
    {"D15S1", D3DFMT_D15S1}, {"D24S8", D3DFMT_D24S8}, {"D24X8", D3DFMT_D24X8},
    {"D24X4S4", D3DFMT_D24X4S4}, {"D16", D3DFMT_D16}, {"D32F_LOCKABLE", D3DFMT_D32F_LOCKABLE},
    {"D24FS8", D3DFMT_D24FS8}, {"D32_LOCKABLE", (D3DFORMAT)84}, {"S8_LOCKABLE", (D3DFORMAT)85},
    {"L16", D3DFMT_L16}, {"VERTEXDATA", D3DFMT_VERTEXDATA}, {"INDEX16", D3DFMT_INDEX16},
    {"INDEX32", D3DFMT_INDEX32}, {"Q16W16V16U16", D3DFMT_Q16W16V16U16},
    {"MULTI2_ARGB8", D3DFMT_MULTI2_ARGB8}, {"R16F", D3DFMT_R16F}, {"G16R16F", D3DFMT_G16R16F},
    {"A16B16G16R16F", D3DFMT_A16B16G16R16F}, {"R32F", D3DFMT_R32F}, {"G32R32F", D3DFMT_G32R32F},
    {"A32B32G32R32F", D3DFMT_A32B32G32R32F}, {"CxV8U8", D3DFMT_CxV8U8}, {"A1", (D3DFORMAT)118},
    {"A2B10G10R10_XR_BIAS", (D3DFORMAT)119}, {"BINARYBUFFER", (D3DFORMAT)199},
    /* FourCC formats that vendors' drivers expose and games look for */
    {"INTZ", FCC('I','N','T','Z')}, {"DF16", FCC('D','F','1','6')}, {"DF24", FCC('D','F','2','4')},
    {"RAWZ", FCC('R','A','W','Z')}, {"NULL", FCC('N','U','L','L')}, {"RESZ", FCC('R','E','S','Z')},
    {"ATOC", FCC('A','T','O','C')}, {"SSAA", FCC('S','S','A','A')}, {"NVDB", FCC('N','V','D','B')},
    {"R2VB", FCC('R','2','V','B')}, {"ATI1", FCC('A','T','I','1')}, {"ATI2", FCC('A','T','I','2')},
    {"GET4", FCC('G','E','T','4')}, {"GET1", FCC('G','E','T','1')}, {"NV12", FCC('N','V','1','2')},
    {"AYUV", FCC('A','Y','U','V')}, {"YV12", FCC('Y','V','1','2')}, {"NVCS", FCC('N','V','C','S')},
};

static const struct { const char *name; D3DRESOURCETYPE type; } rtypes[] = {
    {"surface", D3DRTYPE_SURFACE}, {"texture", D3DRTYPE_TEXTURE},
    {"volumetexture", D3DRTYPE_VOLUMETEXTURE}, {"cubetexture", D3DRTYPE_CUBETEXTURE},
    {"vertexbuffer", D3DRTYPE_VERTEXBUFFER}, {"indexbuffer", D3DRTYPE_INDEXBUFFER},
};

/* One letter per usage in the format table's columns; '.' where the query fails. */
static const struct { char letter; DWORD usage; } usages[] = {
    {'0', 0}, {'R', D3DUSAGE_RENDERTARGET}, {'Z', D3DUSAGE_DEPTHSTENCIL},
    {'Y', D3DUSAGE_DYNAMIC}, {'A', D3DUSAGE_AUTOGENMIPMAP}, {'M', D3DUSAGE_DMAP},
    {'L', D3DUSAGE_QUERY_LEGACYBUMPMAP}, {'s', D3DUSAGE_QUERY_SRGBREAD},
    {'F', D3DUSAGE_QUERY_FILTER}, {'w', D3DUSAGE_QUERY_SRGBWRITE},
    {'B', D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING}, {'V', D3DUSAGE_QUERY_VERTEXTEXTURE},
    {'W', D3DUSAGE_QUERY_WRAPANDMIP},
    {'b', D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING},
    {'f', D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER},
};

#define U(field) {#field, offsetof(D3DCAPS9, field), 0}
#define F(field) {#field, offsetof(D3DCAPS9, field), 1}
static const struct { const char *name; size_t offset; int is_float; } caps_fields[] = {
    U(DeviceType), U(AdapterOrdinal), U(Caps), U(Caps2), U(Caps3), U(PresentationIntervals),
    U(CursorCaps), U(DevCaps), U(PrimitiveMiscCaps), U(RasterCaps), U(ZCmpCaps),
    U(SrcBlendCaps), U(DestBlendCaps), U(AlphaCmpCaps), U(ShadeCaps), U(TextureCaps),
    U(TextureFilterCaps), U(CubeTextureFilterCaps), U(VolumeTextureFilterCaps),
    U(TextureAddressCaps), U(VolumeTextureAddressCaps), U(LineCaps), U(MaxTextureWidth),
    U(MaxTextureHeight), U(MaxVolumeExtent), U(MaxTextureRepeat), U(MaxTextureAspectRatio),
    U(MaxAnisotropy), F(MaxVertexW), F(GuardBandLeft), F(GuardBandTop), F(GuardBandRight),
    F(GuardBandBottom), F(ExtentsAdjust), U(StencilCaps), U(FVFCaps), U(TextureOpCaps),
    U(MaxTextureBlendStages), U(MaxSimultaneousTextures), U(VertexProcessingCaps),
    U(MaxActiveLights), U(MaxUserClipPlanes), U(MaxVertexBlendMatrices),
    U(MaxVertexBlendMatrixIndex), F(MaxPointSize), U(MaxPrimitiveCount), U(MaxVertexIndex),
    U(MaxStreams), U(MaxStreamStride), U(VertexShaderVersion), U(MaxVertexShaderConst),
    U(PixelShaderVersion), F(PixelShader1xMaxValue), U(DevCaps2),
    F(MaxNpatchTessellationLevel), U(Reserved5), U(MasterAdapterOrdinal),
    U(AdapterOrdinalInGroup), U(NumberOfAdaptersInGroup), U(DeclTypes), U(NumSimultaneousRTs),
    U(StretchRectFilterCaps), U(VS20Caps.Caps), U(VS20Caps.DynamicFlowControlDepth),
    U(VS20Caps.NumTemps), U(VS20Caps.StaticFlowControlDepth), U(PS20Caps.Caps),
    U(PS20Caps.DynamicFlowControlDepth), U(PS20Caps.NumTemps),
    U(PS20Caps.StaticFlowControlDepth), U(PS20Caps.NumInstructionSlots),
    U(VertexTextureFilterCaps), U(MaxVShaderInstructionsExecuted),
    U(MaxPShaderInstructionsExecuted), U(MaxVertexShader30InstructionSlots),
    U(MaxPixelShader30InstructionSlots),
};

static void print_caps(const char *prefix, const D3DCAPS9 *caps)
{
    for (size_t i = 0; i < sizeof(caps_fields) / sizeof(caps_fields[0]); ++i) {
        DWORD bits;
        memcpy(&bits, (const BYTE *)caps + caps_fields[i].offset, sizeof(bits));
        if (caps_fields[i].is_float) {
            float value;
            memcpy(&value, &bits, sizeof(value));
            fprintf(out, "%s %s = %g (0x%08lx)\n", prefix, caps_fields[i].name, value,
                    (unsigned long)bits);
        } else {
            fprintf(out, "%s %s = 0x%08lx (%lu)\n", prefix, caps_fields[i].name,
                    (unsigned long)bits, (unsigned long)bits);
        }
    }
    const DWORD *raw = (const DWORD *)caps;
    for (size_t i = 0; i < sizeof(*caps) / sizeof(DWORD); i += 8) {
        fprintf(out, "%s raw %03u:", prefix, (unsigned)(i * 4));
        for (size_t j = i; j < i + 8 && j < sizeof(*caps) / sizeof(DWORD); ++j)
            fprintf(out, " %08lx", (unsigned long)raw[j]);
        fprintf(out, "\n");
    }
}

static void print_caps_changes(const char *prefix, const D3DCAPS9 *base, const D3DCAPS9 *caps)
{
    unsigned changed = 0;
    for (size_t i = 0; i < sizeof(caps_fields) / sizeof(caps_fields[0]); ++i) {
        DWORD a, b;
        memcpy(&a, (const BYTE *)base + caps_fields[i].offset, sizeof(a));
        memcpy(&b, (const BYTE *)caps + caps_fields[i].offset, sizeof(b));
        if (a != b) {
            fprintf(out, "%s %s adapter 0x%08lx device 0x%08lx\n", prefix, caps_fields[i].name,
                    (unsigned long)a, (unsigned long)b);
            ++changed;
        }
    }
    fprintf(out, "%s fields differing from the adapter caps: %u\n", prefix, changed);
}

static HWND make_window(void)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "RippleD3D9Caps";
    RegisterClassA(&wc);
    return CreateWindowA(wc.lpszClassName, "d3d9caps", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120,
                         NULL, NULL, wc.hInstance, NULL);
}

static void try_devices(IDirect3D9 *d3d, HWND window, const D3DCAPS9 *adapter_caps)
{
    static const struct { const char *name; DWORD flags; } kinds[] = {
        {"hardware", D3DCREATE_HARDWARE_VERTEXPROCESSING},
        {"hardware-pure", D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE},
        {"mixed", D3DCREATE_MIXED_VERTEXPROCESSING},
        {"software", D3DCREATE_SOFTWARE_VERTEXPROCESSING},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        D3DPRESENT_PARAMETERS pp = {0};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
        pp.hDeviceWindow = window;
        IDirect3DDevice9 *device = NULL;
        HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                             kinds[i].flags, &pp, &device);
        fprintf(out, "device %s flags=0x%08lx create=0x%08lx\n", kinds[i].name,
                (unsigned long)kinds[i].flags, (unsigned long)hr);
        if (FAILED(hr) || !device)
            continue;
        D3DDEVICE_CREATION_PARAMETERS params;
        if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(device, &params)))
            fprintf(out, "device %s behavior=0x%08lx type=%u\n", kinds[i].name,
                    (unsigned long)params.BehaviorFlags, (unsigned)params.DeviceType);
        if (!(kinds[i].flags & D3DCREATE_PUREDEVICE))
            fprintf(out, "device %s software-vertex-processing=%d\n", kinds[i].name,
                    IDirect3DDevice9_GetSoftwareVertexProcessing(device));
        fprintf(out, "device %s available-texture-mem=%u\n", kinds[i].name,
                IDirect3DDevice9_GetAvailableTextureMem(device));
        D3DCAPS9 caps;
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "device %s caps", kinds[i].name);
        if (SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(device, &caps)))
            print_caps_changes(prefix, adapter_caps, &caps);
        IDirect3DDevice9_Release(device);
    }
}

static void check_formats(IDirect3D9 *d3d, D3DFORMAT adapter_format, const char *adapter_name)
{
    fprintf(out, "format legend:");
    for (size_t u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u)
        fprintf(out, " %c=0x%lx", usages[u].letter, (unsigned long)usages[u].usage);
    fprintf(out, "; '.' fails, '-' S_OK with D3DOK_NOAUTOGEN\n");
    for (size_t r = 0; r < sizeof(rtypes) / sizeof(rtypes[0]); ++r) {
        for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
            char answers[sizeof(usages) / sizeof(usages[0]) + 1];
            int any = 0;
            for (size_t u = 0; u < sizeof(usages) / sizeof(usages[0]); ++u) {
                HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                          adapter_format, usages[u].usage,
                                                          rtypes[r].type, formats[f].format);
                answers[u] = hr == D3D_OK ? usages[u].letter :
                             hr == D3DOK_NOAUTOGEN ? '-' : '.';
                any |= SUCCEEDED(hr);
            }
            answers[sizeof(usages) / sizeof(usages[0])] = 0;
            if (any)
                fprintf(out, "format %s %s %s %s\n", adapter_name, rtypes[r].name,
                        formats[f].name, answers);
        }
    }
}

static void check_depth_matches(IDirect3D9 *d3d, D3DFORMAT adapter_format, const char *adapter_name)
{
    static const D3DFORMAT targets[] = {
        D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A2R10G10B10, D3DFMT_G16R16,
        D3DFMT_A16B16G16R16, D3DFMT_R16F, D3DFMT_G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_R32F,
        D3DFMT_G32R32F, D3DFMT_A32B32G32R32F,
    };
    static const D3DFORMAT depths[] = {
        D3DFMT_D16, D3DFMT_D16_LOCKABLE, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D32, D3DFMT_D24FS8,
        D3DFMT_D32F_LOCKABLE, FCC('I','N','T','Z'), FCC('D','F','1','6'), FCC('D','F','2','4'),
    };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        fprintf(out, "depthmatch %s target=%lu:", adapter_name, (unsigned long)targets[t]);
        for (size_t d = 0; d < sizeof(depths) / sizeof(depths[0]); ++d) {
            HRESULT hr = IDirect3D9_CheckDepthStencilMatch(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                           adapter_format, targets[t], depths[d]);
            fprintf(out, " %lu=%s", (unsigned long)depths[d], SUCCEEDED(hr) ? "ok" : "no");
        }
        fprintf(out, "\n");
    }
}

static void check_multisample(IDirect3D9 *d3d)
{
    static const D3DFORMAT targets[] = {
        D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A16B16G16R16F,
        D3DFMT_A32B32G32R32F, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16,
    };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        for (int windowed = 0; windowed <= 1; ++windowed) {
            fprintf(out, "multisample format=%lu windowed=%d:", (unsigned long)targets[t], windowed);
            for (int type = D3DMULTISAMPLE_NONMASKABLE; type <= D3DMULTISAMPLE_16_SAMPLES; ++type) {
                DWORD quality = 0;
                HRESULT hr = IDirect3D9_CheckDeviceMultiSampleType(
                    d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, targets[t], windowed,
                    (D3DMULTISAMPLE_TYPE)type, &quality);
                if (SUCCEEDED(hr))
                    fprintf(out, " %d(q%lu)", type, (unsigned long)quality);
            }
            fprintf(out, "\n");
        }
    }
}

static void check_conversions(IDirect3D9 *d3d)
{
    static const D3DFORMAT sources[] = {
        D3DFMT_YUY2, D3DFMT_UYVY, FCC('N','V','1','2'), FCC('A','Y','U','V'), FCC('Y','V','1','2'),
        D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A16B16G16R16F,
    };
    static const D3DFORMAT targets[] = {
        D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_A16B16G16R16F, D3DFMT_YUY2,
    };
    for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); ++s) {
        fprintf(out, "conversion source=%lu:", (unsigned long)sources[s]);
        for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
            HRESULT hr = IDirect3D9_CheckDeviceFormatConversion(d3d, D3DADAPTER_DEFAULT,
                                                                D3DDEVTYPE_HAL, sources[s],
                                                                targets[t]);
            fprintf(out, " %lu=%s", (unsigned long)targets[t], SUCCEEDED(hr) ? "ok" : "no");
        }
        fprintf(out, "\n");
    }
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out)
        return 2;
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(out, "Direct3DCreate9 failed\n");
        return 1;
    }
    OSVERSIONINFOA os;
    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    GetVersionExA(&os);
    fprintf(out, "process %u-bit, Windows %lu.%lu.%lu\n", (unsigned)(sizeof(void *) * 8),
            (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
            (unsigned long)os.dwBuildNumber);
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id)))
        fprintf(out, "adapter driver=%s description=%s version=%u.%u.%u.%u vendor=%04lx device=%04lx\n",
                id.Driver, id.Description, HIWORD(id.DriverVersion.HighPart),
                LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart),
                LOWORD(id.DriverVersion.LowPart), (unsigned long)id.VendorId,
                (unsigned long)id.DeviceId);
    D3DDISPLAYMODE mode;
    if (SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode)))
        fprintf(out, "display mode %ux%u format=%lu\n", mode.Width, mode.Height,
                (unsigned long)mode.Format);
    HMODULE umd = GetModuleHandleA("viogpu_d3d10.dll");
    char path[MAX_PATH] = "";
    if (umd)
        GetModuleFileNameA(umd, path, sizeof(path));

    D3DCAPS9 caps;
    HRESULT hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    fprintf(out, "adapter caps hr=0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr))
        print_caps("adapter caps", &caps);

    HWND window = make_window();
    if (window && SUCCEEDED(hr))
        try_devices(d3d, window, &caps);
    if (!path[0] && (umd = GetModuleHandleA("viogpu_d3d10.dll")))
        GetModuleFileNameA(umd, path, sizeof(path));
    fprintf(out, "user-mode driver %s\n", path[0] ? path : "(not loaded in this process)");

    static const struct named_format adapters[] = {
        {"X8R8G8B8", D3DFMT_X8R8G8B8}, {"R5G6B5", D3DFMT_R5G6B5}, {"X1R5G5B5", D3DFMT_X1R5G5B5},
    };
    for (size_t a = 0; a < sizeof(adapters) / sizeof(adapters[0]); ++a) {
        HRESULT type = IDirect3D9_CheckDeviceType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                  adapters[a].format, adapters[a].format, FALSE);
        fprintf(out, "devicetype %s fullscreen=0x%08lx\n", adapters[a].name, (unsigned long)type);
        if (SUCCEEDED(type)) {
            check_formats(d3d, adapters[a].format, adapters[a].name);
            check_depth_matches(d3d, adapters[a].format, adapters[a].name);
        }
    }
    check_multisample(d3d);
    check_conversions(d3d);
    fprintf(out, "end\n");
    if (window)
        DestroyWindow(window);
    IDirect3D9_Release(d3d);
    if (out != stdout)
        fclose(out);
    return 0;
}
