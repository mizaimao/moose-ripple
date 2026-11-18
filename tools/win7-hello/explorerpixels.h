/* Small-region observers for Windows 7 Explorer. MIT (LICENSE). */
#ifndef RIPPLE_EXPLORERPIXELS_H
#define RIPPLE_EXPLORERPIXELS_H
#include <windows.h>

struct pixel_run;
/* screen_area is in desktop coordinates. Passing a child also samples its
 * parent's window DC and records when the child's right edge reaches goal. */
struct pixel_run *start_pixels(HWND window, HWND child, LONG goal, RECT screen_area, BOOL sample_screen);
BOOL finish_pixels(struct pixel_run *run, const char *gesture, int round);
#endif
