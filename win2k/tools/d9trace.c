/* A logging stand-in for d3d9.dll, for finding out what a Direct3D 9 program
 * asks for that the runtime refuses. Copy it into the program's folder as
 * d3d9.dll, with the system's d3d9.dll beside it as d3d9real.dll; every other
 * export is forwarded there. It writes d9trace.log in the current folder:
 * format checks for volume and cube textures and all failed ones, every volume
 * texture created, every texture or cube texture that could not be, and the
 * first few device caps the program reads (the ones programs test for shader
 * model 2). */
#include <windows.h>
#include <d3d9.h>

static HMODULE self;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static HRESULT (WINAPI *realCheckFormat)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
static HRESULT (WINAPI *realGetCaps)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DCAPS9 *);
static HRESULT (WINAPI *realCreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                         IDirect3DDevice9 **);
static HRESULT (WINAPI *realCreateTexture)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                          IDirect3DTexture9 **, HANDLE *);
static HRESULT (WINAPI *realCreateVolume)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DVolumeTexture9 **, HANDLE *);
static HRESULT (WINAPI *realCreateCube)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                       IDirect3DCubeTexture9 **, HANDLE *);

static void say(const char *text)
{
    DWORD written;
    if (logFile == INVALID_HANDLE_VALUE) {
        logFile = CreateFileA("d9trace.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (logFile == INVALID_HANDLE_VALUE) return;
    }
    WriteFile(logFile, text, lstrlenA(text), &written, NULL);
    FlushFileBuffers(logFile);
}

/* A format code as text: four characters for FOURCC codes, else the number. */
static void format_name(char *out, D3DFORMAT f)
{
    DWORD v = (DWORD)f;
    if (v > 255 && ((v & 0xff) >= ' ' && (v >> 24) >= ' '))
        wsprintfA(out, "'%c%c%c%c'", (int)(v & 0xff), (int)((v >> 8) & 0xff), (int)((v >> 16) & 0xff), (int)(v >> 24));
    else wsprintfA(out, "%lu", v);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) self = module;
    return TRUE;
}

/* The hooks stay in the runtime's vtables: a program that frees this DLL after
 * probing it must not unload them (one reference more, never released). */
static void pin(void)
{
    static int pinned;
    char path[MAX_PATH];
    if (!pinned && self && GetModuleFileNameA(self, path, sizeof(path))) pinned = LoadLibraryA(path) != NULL;
}

static void patch(void **slot, void *hook, void **real)
{
    DWORD old;
    if (*slot == hook) return;
    if (!*real) *real = *slot;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &old)) {
        *slot = hook;
        VirtualProtect(slot, sizeof(*slot), old, &old);
    }
}

static HRESULT WINAPI check_format(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFormat,
                                   DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT format)
{
    HRESULT hr = realCheckFormat(d3d, adapter, type, adapterFormat, usage, resource, format);
    if (FAILED(hr) || resource == D3DRTYPE_VOLUMETEXTURE || resource == D3DRTYPE_CUBETEXTURE) {
        char line[160], name[16];
        format_name(name, format);
        wsprintfA(line, "CheckDeviceFormat resource=%d usage=0x%lx format=%s -> 0x%08lx\r\n", (int)resource,
                  usage, name, (DWORD)hr);
        say(line);
    }
    return hr;
}

static HRESULT WINAPI get_caps(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps)
{
    static int logged;
    HRESULT hr = realGetCaps(d3d, adapter, type, caps);
    if (logged < 4 && SUCCEEDED(hr) && caps) {
        char line[480];
        ++logged;
        wsprintfA(line, "GetDeviceCaps type=%d ps=%08lx vs=%08lx ps20=%lx,%d,%d,%d,%d vs20=%lx,%d,%d,%d "
                  "textures=%lu rts=%lu stages=%lu decltypes=%lx caps2=%lx devcaps=%lx devcaps2=%lx texcaps=%lx "
                  "maxtex=%lux%lu vsconst=%lu psmax=%lu\r\n", (int)type, caps->PixelShaderVersion,
                  caps->VertexShaderVersion, caps->PS20Caps.Caps, caps->PS20Caps.DynamicFlowControlDepth,
                  caps->PS20Caps.NumTemps, caps->PS20Caps.StaticFlowControlDepth, caps->PS20Caps.NumInstructionSlots,
                  caps->VS20Caps.Caps, caps->VS20Caps.DynamicFlowControlDepth, caps->VS20Caps.NumTemps,
                  caps->VS20Caps.StaticFlowControlDepth, caps->MaxSimultaneousTextures, caps->NumSimultaneousRTs,
                  caps->MaxTextureBlendStages, caps->DeclTypes, caps->Caps2, caps->DevCaps, caps->DevCaps2,
                  caps->TextureCaps, caps->MaxTextureWidth, caps->MaxTextureHeight, caps->MaxVertexShaderConst,
                  caps->MaxPixelShader30InstructionSlots);
        say(line);
        {   /* the whole record, 16 words a line, for checks of fields not named above */
            const DWORD *w = (const DWORD *)caps;
            int k, j;
            for (k = 0; k < (int)(sizeof(*caps) / 4); k += 16) {
                int at = wsprintfA(line, "GetDeviceCaps type=%d words %03x:", (int)type, k * 4);
                for (j = k; j < k + 16 && j < (int)(sizeof(*caps) / 4); ++j) at += wsprintfA(line + at, " %08lx", w[j]);
                lstrcpyA(line + at, "\r\n");
                say(line);
            }
        }
    }
    return hr;
}

static HRESULT WINAPI create_texture(IDirect3DDevice9 *device, UINT width, UINT height, UINT levels, DWORD usage,
                                     D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared)
{
    HRESULT hr = realCreateTexture(device, width, height, levels, usage, format, pool, texture, shared);
    if (FAILED(hr)) {
        char line[160], name[16];
        format_name(name, format);
        wsprintfA(line, "CreateTexture %ux%u levels=%u usage=0x%lx format=%s pool=%d -> 0x%08lx\r\n", width, height,
                  levels, usage, name, (int)pool, (DWORD)hr);
        say(line);
    }
    return hr;
}

static HRESULT WINAPI create_volume(IDirect3DDevice9 *device, UINT width, UINT height, UINT depth, UINT levels,
                                    DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DVolumeTexture9 **volume,
                                    HANDLE *shared)
{
    HRESULT hr = realCreateVolume(device, width, height, depth, levels, usage, format, pool, volume, shared);
    char line[160], name[16];
    format_name(name, format);
    wsprintfA(line, "CreateVolumeTexture %ux%ux%u levels=%u usage=0x%lx format=%s pool=%d -> 0x%08lx\r\n", width,
              height, depth, levels, usage, name, (int)pool, (DWORD)hr);
    say(line);
    return hr;
}

static HRESULT WINAPI create_cube(IDirect3DDevice9 *device, UINT edge, UINT levels, DWORD usage, D3DFORMAT format,
                                  D3DPOOL pool, IDirect3DCubeTexture9 **cube, HANDLE *shared)
{
    HRESULT hr = realCreateCube(device, edge, levels, usage, format, pool, cube, shared);
    if (FAILED(hr)) {
        char line[160], name[16];
        format_name(name, format);
        wsprintfA(line, "CreateCubeTexture %u levels=%u usage=0x%lx format=%s pool=%d -> 0x%08lx\r\n", edge, levels,
                  usage, name, (int)pool, (DWORD)hr);
        say(line);
    }
    return hr;
}

static HRESULT WINAPI create_device(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                    D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device)
{
    HRESULT hr = realCreateDevice(d3d, adapter, type, window, flags, params, device);
    char line[96];
    wsprintfA(line, "CreateDevice type=%d flags=0x%lx -> 0x%08lx\r\n", (int)type, flags, (DWORD)hr);
    say(line);
    if (SUCCEEDED(hr) && *device) {
        IDirect3DDevice9Vtbl *v = (*device)->lpVtbl;
        patch((void **)&v->CreateTexture, (void *)create_texture, (void **)&realCreateTexture);
        patch((void **)&v->CreateVolumeTexture, (void *)create_volume, (void **)&realCreateVolume);
        patch((void **)&v->CreateCubeTexture, (void *)create_cube, (void **)&realCreateCube);
    }
    return hr;
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT version)
{
    static IDirect3D9 *(WINAPI *real)(UINT);
    IDirect3D9 *d3d;
    char line[64];
    pin();
    if (!real) {
        HMODULE m = LoadLibraryA("d3d9real.dll");
        real = m ? (IDirect3D9 *(WINAPI *)(UINT))GetProcAddress(m, "Direct3DCreate9") : NULL;
        if (!real) { say("d3d9real.dll missing\r\n"); return NULL; }
    }
    d3d = real(version);
    wsprintfA(line, "Direct3DCreate9 %u -> %p\r\n", version, (void *)d3d);
    say(line);
    if (d3d) {
        IDirect3D9Vtbl *v = d3d->lpVtbl;
        patch((void **)&v->CheckDeviceFormat, (void *)check_format, (void **)&realCheckFormat);
        patch((void **)&v->GetDeviceCaps, (void *)get_caps, (void **)&realGetCaps);
        patch((void **)&v->CreateDevice, (void *)create_device, (void **)&realCreateDevice);
    }
    return d3d;
}
