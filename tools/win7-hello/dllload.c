/*
 * dllload DLL [SYMBOL]...: load a DLL (resolving all its imports) and look up symbols in it,
 * for checking that driver DLLs built off Windows load on the target Windows. Prints one line
 * per step and writes them to dllload.txt. MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f = fopen("dllload.txt", "ab");
    HMODULE m;
    int i, bad = 0;
    char line[512];

    if (argc < 2) { fprintf(stderr, "usage: dllload DLL [SYMBOL]...\n"); return 2; }
    m = LoadLibraryA(argv[1]);
    snprintf(line, sizeof(line), "%s: %s (error %lu)\r\n", argv[1], m ? "loaded" : "NOT loaded",
             m ? 0UL : GetLastError());
    fputs(line, stdout); if (f) fputs(line, f);
    if (!m) bad = 1;
    for (i = 2; m && i < argc; i++) {
        FARPROC p = GetProcAddress(m, argv[i]);
        snprintf(line, sizeof(line), "  %s: %s\r\n", argv[i], p ? "found" : "missing");
        fputs(line, stdout); if (f) fputs(line, f);
        if (!p) bad = 1;
    }
    if (f) fclose(f);
    return bad;
}
