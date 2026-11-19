/* FL9_3 hardware occlusion query against a 16x16 draw and CPU readback. MIT. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <d3d10shader.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { HRESULT check_hr = (call); if (FAILED(check_hr)) { \
    printf("FAIL %s = %08lx\n", #call, (unsigned long)check_hr); goto done; } } while (0)

static const char source[] =
    "float4 vs(float4 p:POSITION):SV_Position { return p; }"
    "float4 ps():SV_Target { return float4(1,0,0,1); }";

static HRESULT compile_pair(HMODULE module, BOOL legacy, ID3DBlob **vs, ID3DBlob **ps)
{
    union {
        FARPROC addr;
        __typeof__(&D3DCompile) modern;
        __typeof__(&D3D10CompileShader) old;
    } compiler;
    compiler.addr = GetProcAddress(module, legacy ? "D3D10CompileShader" : "D3DCompile");
    if (!compiler.addr) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    for (int i = 0; i < 2; ++i) {
        ID3DBlob *messages = NULL;
        const char *entry = i ? "ps" : "vs";
        const char *profile = i ? "ps_4_0_level_9_3" : "vs_4_0_level_9_3";
        ID3DBlob **out = i ? ps : vs;
        HRESULT hr = legacy
            ? compiler.old(source, sizeof(source)-1, NULL, NULL, NULL, entry, profile, 0, out, &messages)
            : compiler.modern(source, sizeof(source)-1, NULL, NULL, NULL, entry, profile, 0, 0, out, &messages);
        if (messages) {
            printf("%s: %.*s\n", profile, (int)ID3D10Blob_GetBufferSize(messages),
                   (char *)ID3D10Blob_GetBufferPointer(messages));
            ID3D10Blob_Release(messages);
        }
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static HMODULE shaders(ID3DBlob **vs, ID3DBlob **ps)
{
    const char *names[] = {"d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll",
                           "d3dcompiler_42.dll", "d3dcompiler_41.dll", "d3d10.dll"};
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i) {
        HMODULE m = LoadLibraryA(names[i]);
        if (!m) continue;
        HRESULT hr = compile_pair(m, !strcmp(names[i], "d3d10.dll"), vs, ps);
        printf("shader compiler=%s compile=%08lx\n", names[i], (unsigned long)hr);
        if (SUCCEEDED(hr)) return m;
        if (*vs) { ID3D10Blob_Release(*vs); *vs = NULL; }
        if (*ps) { ID3D10Blob_Release(*ps); *ps = NULL; }
        FreeLibrary(m);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11Texture2D *rt = NULL, *copy = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11Buffer *vb = NULL;
    ID3D11RasterizerState *raster = NULL;
    ID3D11Query *query = NULL;
    ID3DBlob *vs_code = NULL, *ps_code = NULL;
    HMODULE compiler = NULL;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_3;
    D3D_FEATURE_LEVEL actual = 0;
    D3D11_TEXTURE2D_DESC td = {0};
    D3D11_BUFFER_DESC bd = {0};
    D3D11_SUBRESOURCE_DATA initial = {0};
    D3D11_RASTERIZER_DESC rd = {0};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0};
    D3D11_VIEWPORT viewport = {0, 0, 32, 32, 0, 1};
    const D3D11_INPUT_ELEMENT_DESC element = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0
    };
    const float corners[][4] = {
        {-.5f,.5f,.5f,1}, {.5f,.5f,.5f,1}, {-.5f,-.5f,.5f,1}, {.5f,-.5f,.5f,1}
    };
    const UINT stride = sizeof(corners[0]), offset = 0;
    const float black[] = {0, 0, 0, 1};
    BOOL visibility = FALSE, force = FALSE, aero = FALSE;
    const char *expected = NULL;
    int rc = 1, mismatches = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--force-level9")) force = TRUE;
        else if (!strcmp(argv[i], "--expect-visibility")) visibility = TRUE;
        else if (!strcmp(argv[i], "--expect-supported")) visibility = FALSE;
        else if (!expected && argv[i][0] != '-') expected = argv[i];
        else {
            puts("usage: d3d11query [--force-level9] [--expect-supported|--expect-visibility] [expected-full-UMD-path]");
            return 2;
        }
    }
    if (force && _putenv("RIPPLE_NO_D3D10=1")) return 2;
    HRESULT hr = DwmIsCompositionEnabled(&aero);
    printf("process=%u-bit process_forced_level9=%d composition_hr=%08lx enabled=%d expectation=%s\n",
           (unsigned)(sizeof(void *)*8), getenv("RIPPLE_NO_D3D10") != NULL, (unsigned long)hr,
           (int)aero, visibility ? "visibility" : "exact");
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           &level, 1, D3D11_SDK_VERSION, &dev, &actual, &ctx);
    BOOL level9_module = GetModuleHandleA("d3d10level9.dll") != NULL;
    printf("D3D11CreateDevice hr=%08lx actual=%04x level9_module=%d\n", (unsigned long)hr,
           (unsigned)actual, (int)level9_module);
    HMODULE umd = GetModuleHandleA("viogpu_d3d10.dll");
    char path[MAX_PATH] = "";
    DWORD length = umd ? GetModuleFileNameA(umd, path, sizeof(path)) : 0;
    path[sizeof(path)-1] = '\0';
    printf("UMD=%s\n", length ? path : "(other adapter)");
    if (FAILED(hr) || !dev || !ctx || actual != level || (force && !level9_module) ||
        (expected && (!length || length >= sizeof(path) || _stricmp(path, expected)))) goto done;
    compiler = shaders(&vs_code, &ps_code);
    if (!compiler) { puts("No available shader compiler accepted both FL9_3 profiles."); goto done; }
    CHECK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vs_code),
          ID3D10Blob_GetBufferSize(vs_code), NULL, &vs));
    CHECK(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(ps_code),
          ID3D10Blob_GetBufferSize(ps_code), NULL, &ps));
    CHECK(ID3D11Device_CreateInputLayout(dev, &element, 1, ID3D10Blob_GetBufferPointer(vs_code),
          ID3D10Blob_GetBufferSize(vs_code), &layout));
    td.Width = td.Height = 32;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt));
    CHECK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv));
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &copy));
    bd.ByteWidth = sizeof(corners);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    initial.pSysMem = corners;
    CHECK(ID3D11Device_CreateBuffer(dev, &bd, &initial, &vb));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CHECK(ID3D11Device_CreateRasterizerState(dev, &rd, &raster));
    CHECK(ID3D11Device_CreateQuery(dev, &qd, &query));
    ID3D11DeviceContext_RSSetState(ctx, raster);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    for (unsigned pass = 0; pass < 3; ++pass) {
        const BOOL draw = pass != 1;
        UINT64 samples = ~(UINT64)0;
        D3D11_MAPPED_SUBRESOURCE map;
        unsigned red = 0, bad = 0;
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
        ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)query);
        if (draw) ID3D11DeviceContext_Draw(ctx, 4, 0);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)query);
        ID3D11DeviceContext_Flush(ctx);
        DWORD start = GetTickCount();
        do {
            hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)query, &samples,
                                             sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_FALSE) break;
            Sleep(1);
        } while (GetTickCount()-start < 10000);
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)copy, (ID3D11Resource *)rt);
        CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)copy, 0, D3D11_MAP_READ, 0, &map));
        for (unsigned y = 0; y < 32; ++y) {
            const BYTE *row = (const BYTE *)map.pData + y*map.RowPitch;
            for (unsigned x = 0; x < 32; ++x) {
                const BYTE *p = row + x*4;
                const BOOL inside = draw && x >= 8 && x < 24 && y >= 8 && y < 24;
                red += p[0] == 255 && !p[1] && !p[2];
                bad += p[0] != (inside ? 255 : 0) || p[1] || p[2] || p[3] != 255;
            }
        }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)copy, 0);
        /* visibility: a host that can only say whether anything was visible answers with a
         * stand-in count for "something"; any count but zero is right then */
        UINT64 want = visibility ? (red != 0) : red;
        printf("pass=%u red=%u wrong_pixels=%u GetData=%08lx samples=%llu expected=%s%llu\n",
               pass, red, bad, (unsigned long)hr, (unsigned long long)samples,
               visibility && red ? "any but " : "", (unsigned long long)(visibility && red ? 0 : want));
        mismatches += bad != 0 || hr != S_OK ||
            (visibility ? (samples != 0) != (red != 0) : samples != want);
    }
    rc = mismatches != 0;
done:
    if (ctx) ID3D11DeviceContext_ClearState(ctx);
    if (query) ID3D11Query_Release(query);
    if (raster) ID3D11RasterizerState_Release(raster);
    if (vb) ID3D11Buffer_Release(vb);
    if (layout) ID3D11InputLayout_Release(layout);
    if (ps) ID3D11PixelShader_Release(ps);
    if (vs) ID3D11VertexShader_Release(vs);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (copy) ID3D11Texture2D_Release(copy);
    if (rt) ID3D11Texture2D_Release(rt);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    if (vs_code) ID3D10Blob_Release(vs_code);
    if (ps_code) ID3D10Blob_Release(ps_code);
    if (compiler) FreeLibrary(compiler);
    puts(rc ? "FAIL" : "PASS");
    return rc;
}
