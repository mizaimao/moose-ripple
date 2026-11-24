/* Keep the screen saver and the display's idle power-off away for the rest of this logon
 * session, for a long unattended test. A full-screen Direct3D program that loses the screen to
 * the screen saver between two of its scenes stops its run: on an emulated processor a
 * benchmark that takes a few minutes on real hardware runs past the screen saver's wait, and
 * the test scripts send no keyboard or mouse input while it runs.
 *   keep-awake.exe          switches the screen saver off and prints the settings found
 *   keep-awake.exe show     only prints them
 * The display and sleep timers belong to the power plan: "powercfg -change -monitor-timeout-ac 0"
 * and "powercfg -change -standby-timeout-ac 0", elevated.
 * Build with llvm-mingw clang keep-awake.c -O2 -luser32.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    BOOL active = FALSE;
    UINT wait = 0;
    if (!SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &active, 0) ||
        !SystemParametersInfoA(SPI_GETSCREENSAVETIMEOUT, 0, &wait, 0)) {
        printf("keep-awake: cannot read the screen saver settings: error %lu\n", GetLastError());
        return 1;
    }
    printf("keep-awake: screen saver %s, wait %u s\n", active ? "on" : "off", wait);
    if (argc > 1 && !strcmp(argv[1], "show"))
        return 0;
    /* For this session only: no SPIF_UPDATEINIFILE, the user's saved setting stays as it was. */
    if (!SystemParametersInfoA(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, SPIF_SENDCHANGE)) {
        printf("keep-awake: cannot switch the screen saver off: error %lu\n", GetLastError());
        return 1;
    }
    SystemParametersInfoA(SPI_GETSCREENSAVEACTIVE, 0, &active, 0);
    printf("keep-awake: screen saver now %s\n", active ? "on" : "off");
    return active ? 1 : 0;
}
