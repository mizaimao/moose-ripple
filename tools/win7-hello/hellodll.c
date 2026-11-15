/*
 * A minimal Windows DLL for the toolchain check (docs/windows/toolchain.md): one export,
 * no C runtime. Load it with `rundll32 hellodll.dll,HelloRun`: it shows a message box.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reason; (void)reserved;
    return TRUE;
}

__declspec(dllexport) void CALLBACK HelloRun(HWND hwnd, HINSTANCE inst, LPSTR cmd, int show)
{
    (void)inst; (void)cmd; (void)show;
    MessageBoxA(hwnd, "moose-ripple hello DLL: built off Windows", "moose-ripple", MB_OK);
}
