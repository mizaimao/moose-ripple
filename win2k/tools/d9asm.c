/* Assemble Direct3D 9 shader text with D3DX and print the tokens, to check
 * hand-made token streams. d3dx9 is not part of Windows: give its path.
 *
 *   d9asm <d3dx9 dll> <file with shader text>
 *
 * Prints D9ASM tokens=... or the assembler's messages. */
#include <windows.h>
#include <stdio.h>

typedef struct Buffer Buffer;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(Buffer *, REFIID, void **);
    ULONG (WINAPI *AddRef)(Buffer *);
    ULONG (WINAPI *Release)(Buffer *);
    void *(WINAPI *GetBufferPointer)(Buffer *);
    DWORD (WINAPI *GetBufferSize)(Buffer *);
} BufferVtbl;
struct Buffer { const BufferVtbl *v; };
typedef HRESULT (WINAPI *Assemble)(const char *, UINT, const void *, void *, DWORD, Buffer **, Buffer **);

int main(int argc, char **argv)
{
    static char text[65536];
    HMODULE lib;
    Assemble assemble;
    Buffer *code = NULL, *errors = NULL;
    FILE *f;
    size_t n;
    HRESULT hr;
    if (argc < 3) { printf("D9ASM usage: d9asm <d3dx9 dll> <file>\n"); return 2; }
    lib = LoadLibraryA(argv[1]);
    assemble = lib ? (Assemble)GetProcAddress(lib, "D3DXAssembleShader") : NULL;
    if (!assemble) { printf("D9ASM no D3DXAssembleShader in %s\n", argv[1]); return 2; }
    f = fopen(argv[2], "rb");
    if (!f) { printf("D9ASM cannot read %s\n", argv[2]); return 2; }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    hr = assemble(text, (UINT)n, NULL, NULL, 0, &code, &errors);
    if (errors) {
        char *e = (char *)errors->v->GetBufferPointer(errors), *p;
        for (p = e; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
        printf("D9ASM messages %s\n", e);
    }
    if (FAILED(hr) || !code) { printf("D9ASM failed hr=%08lx\n", (unsigned long)hr); return 1; }
    {
        const DWORD *w = (const DWORD *)code->v->GetBufferPointer(code);
        DWORD k, words = code->v->GetBufferSize(code) / 4;
        printf("D9ASM tokens=%lu", (unsigned long)words);
        for (k = 0; k < words; ++k) printf("%s%08lx", k ? "," : " ", (unsigned long)w[k]);
        printf("\n");
    }
    return 0;
}
