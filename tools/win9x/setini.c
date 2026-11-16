/*
 * setini: set one value in an .ini file (or WIN.INI), as WritePrivateProfileString does.
 *
 *   setini FILE SECTION KEY VALUE      e.g. setini win.ini DirectDraw debug 9
 *
 * For test VMs: DirectX's debug runtime reads its message level from WIN.INI [DirectDraw].
 * Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 5) {
        printf("usage: setini FILE SECTION KEY VALUE\n");
        return 2;
    }
    if (!WritePrivateProfileString(argv[2], argv[3], argv[4], argv[1])) {
        printf("can't write %s, error %lu\n", argv[1], GetLastError());
        return 1;
    }
    WritePrivateProfileString(NULL, NULL, NULL, argv[1]);  /* flush the cached file */
    printf("[%s] %s=%s in %s\n", argv[2], argv[3], argv[4], argv[1]);
    return 0;
}
