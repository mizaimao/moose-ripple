/* A logging stand-in for d3d9.dll, for finding out what a Direct3D 9 program
 * asks for that the runtime refuses. Copy it into the program's folder as
 * d3d9.dll, with the system's d3d9.dll beside it as d3d9real.dll; every other
 * export is forwarded there. It writes d9trace.log in the current folder:
 * format checks for volume and cube textures and all failed ones, every volume
 * texture created, and every texture or cube texture that could not be. */
#include <windows.h>
#include <d3d9.h>

static HANDLE logFile = INVALID_HANDLE_VALUE;
static HRESULT (WINAPI *realCheckFormat)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
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
        patch((void **)&v->CreateDevice, (void *)create_device, (void **)&realCreateDevice);
    }
    return d3d;
}
