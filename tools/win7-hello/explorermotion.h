/* Windows 7 UI motion probe. MIT (LICENSE). */
#ifndef RIPPLE_EXPLORERMOTION_H
#define RIPPLE_EXPLORERMOTION_H
#include <windows.h>
#include <evntprov.h>

extern const GUID motion_provider;
enum window_gesture { MOVE_WINDOW, RESIZE_WINDOW, CHECK_SEARCH_BAND };
BOOL measure_window_motion(HWND window, REGHANDLE trace, enum window_gesture gesture, int round);
#endif
