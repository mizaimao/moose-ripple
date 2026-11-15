/*
 * hlsl2dxbc IN.hlsl ENTRY PROFILE NAME OUT.h: compile one HLSL entry point with D3DCompile
 * (d3dcompiler_47.dll; on Linux, Wine's, which uses vkd3d-shader) and write the DXBC blob as a
 * C array named NAME. Used to embed d3d10test.c's shaders. MIT licence.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    long n;
    char *src;
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;
    const unsigned char *p;
    SIZE_T i, len;

    if (argc != 6) { fprintf(stderr, "usage: hlsl2dxbc IN.hlsl ENTRY PROFILE NAME OUT.h\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    src = malloc(n + 1);
    fread(src, 1, n, f); src[n] = 0; fclose(f);
    hr = D3DCompile(src, n, argv[1], NULL, NULL, argv[2], argv[3], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "D3DCompile %s %s failed 0x%08lx: %s\n", argv[2], argv[3], (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        return 1;
    }
    p = ID3D10Blob_GetBufferPointer(code);
    len = ID3D10Blob_GetBufferSize(code);
    f = fopen(argv[5], "wb");
    fprintf(f, "/* %s, entry %s, profile %s: DXBC made by hlsl2dxbc (D3DCompile). */\n", argv[1], argv[2], argv[3]);
    fprintf(f, "static const unsigned char %s[%u] = {", argv[4], (unsigned)len);
    for (i = 0; i < len; i++)
        fprintf(f, "%s0x%02x,", i % 16 ? " " : "\n    ", p[i]);
    fprintf(f, "\n};\n");
    fclose(f);
    printf("%s: %u bytes\n", argv[5], (unsigned)len);
    return 0;
}
