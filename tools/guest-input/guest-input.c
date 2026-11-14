/*
 * guest-input: clicks and key presses from inside the guest, for driving an app the same way
 * on every host (a benchmark run needs no pointer device or monitor connection on the host).
 * Run it in the logged-in user's session, over ssh for example.
 *
 *   guest-input "click 446 170" "sleep 5" "down up" "sleep 20" "up up"
 *
 *   move X Y     pointer to X,Y (pixels, origin at the top left of the main display)
 *   click X Y    move there, then a left click
 *   key K        press and release a key
 *   down K       press a key and hold it (until "up K", or until the tool exits)
 *   up K         release it
 *   sleep S      wait S seconds (fractions allowed)
 * K is a key name (up, down, left, right, return, space, esc, tab, a to z, 0 to 9) or a Mac
 * virtual key code as a number.
 */
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int key_code(const char *k)
{
    static const struct { const char *name; int code; } names[] = {
        { "up", 126 }, { "down", 125 }, { "left", 123 }, { "right", 124 }, { "return", 36 },
        { "space", 49 }, { "esc", 53 }, { "tab", 48 },
    };
    /* ANSI layout: virtual key codes of a..z and 0..9 */
    static const int letters[26] = { 0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35,
                                     12, 15, 1, 17, 32, 9, 13, 7, 16, 6 };
    static const int digits[10] = { 29, 18, 19, 20, 21, 23, 22, 26, 28, 25 };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(k, names[i].name))
            return names[i].code;
    if (strlen(k) == 1 && k[0] >= 'a' && k[0] <= 'z')
        return letters[k[0] - 'a'];
    if (strlen(k) == 1 && k[0] >= '0' && k[0] <= '9')
        return digits[k[0] - '0'];
    char *end;
    long n = strtol(k, &end, 0);
    return *k && !*end && n >= 0 && n < 128 ? (int)n : -1;
}

static void post(CGEventRef e)
{
    if (!e) {
        fprintf(stderr, "guest-input: no event (not in a window server session?)\n");
        exit(1);
    }
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
    usleep(30000);
}

static void mouse(CGEventType type, CGPoint p)
{
    post(CGEventCreateMouseEvent(NULL, type, p, kCGMouseButtonLeft));
}

static void key(int code, bool down)
{
    post(CGEventCreateKeyboardEvent(NULL, (CGKeyCode)code, down));
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char verb[16] = "", arg[32] = "";
        double x = 0, y = 0;
        int n = sscanf(argv[i], "%15s %31s %lf", verb, arg, &y);
        if (n >= 2)
            x = atof(arg);
        if (!strcmp(verb, "sleep") && n >= 2) {
            usleep((useconds_t)(x * 1e6));
        } else if ((!strcmp(verb, "move") || !strcmp(verb, "click")) && n == 3) {
            CGPoint p = CGPointMake(x, y);
            mouse(kCGEventMouseMoved, p);
            if (verb[0] == 'c') {
                usleep(100000);
                mouse(kCGEventLeftMouseDown, p);
                usleep(60000);
                mouse(kCGEventLeftMouseUp, p);
            }
        } else if ((!strcmp(verb, "key") || !strcmp(verb, "down") || !strcmp(verb, "up")) &&
                   n == 2 && key_code(arg) >= 0) {
            int code = key_code(arg);
            if (verb[0] != 'u')
                key(code, true);
            if (verb[0] == 'k')
                usleep(60000);
            if (verb[0] != 'd')
                key(code, false);
        } else {
            fprintf(stderr, "guest-input: don't understand \"%s\"\n", argv[i]);
            return 2;
        }
    }
    return 0;
}
