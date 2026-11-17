/*
 * threadtest: start many short-lived threads that each draw one OpenGL frame, and watch the
 * program's memory. A driver (or its run-time) that keeps something per thread after the
 * thread ends grows without limit in a long-running program that starts threads now and then.
 *
 *   threadtest [count] [same|threads] [steps]
 *       count    default 300
 *       same     all in the main thread, one after the other (tells a per-thread leak from a
 *                per-context one); "threads" (the default) starts a thread for each
 *       steps    how far each one goes, to find where memory is kept: 1 creates and deletes
 *                the context, 2 also makes it current, 3 (the default) also draws and checks;
 *                4 draws and checks, then the thread ends with its context still current
 *                and its window open (Windows closes the window, the main thread deletes the
 *                context), as a careless program does; not with "same"
 *
 * Each thread: a hidden window, ChoosePixelFormat and SetPixelFormat, wglCreateContext,
 * wglMakeCurrent, a clear and a triangle, glFinish, a pixel read back and checked, then
 * everything released before the thread ends. Every 50 threads the program prints its
 * committed memory (VirtualQuery over its private address space) and Windows' free system
 * resources. Exit status 0 when every frame was right and the memory grew by less than
 * 1 KB a thread from thread 50 to the last (the first 50 warm up caches).
 * Built by tools/win9x/build-tools.sh.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

static int failures;
static int steps = 3;
static HGLRC left_current;                      /* step 4: a context its thread left current */

static unsigned long committed_kb(void)
{
    MEMORY_BASIC_INFORMATION m;
    unsigned char *p = (unsigned char *)0x00010000;
    unsigned long total = 0;

    while (p < (unsigned char *)0x7FFF0000 && VirtualQuery(p, &m, sizeof m) == sizeof m) {
        if (m.State == MEM_COMMIT)
            total += (unsigned long)(m.RegionSize >> 10);
        if (!m.RegionSize)
            break;
        p = (unsigned char *)m.BaseAddress + m.RegionSize;
    }
    return total;
}

static void resources(int done, unsigned long kb)
{
    typedef int (WINAPI *free_fn)(int);
    static free_fn free_resources;
    static int looked;

    if (!looked) {
        HMODULE lib = LoadLibrary("rsrc32.dll");
        free_resources = lib ? (free_fn)GetProcAddress(lib, "_MyGetFreeSystemResources32@4") : NULL;
        looked = 1;
    }
    printf("%4d threads: committed %lu KB", done, kb);
    if (free_resources)
        printf(", free resources: system %d%%, GDI %d%%, USER %d%%", free_resources(0),
               free_resources(1), free_resources(2));
    printf("\n");
    fflush(stdout);
}

static DWORD WINAPI worker(LPVOID arg)
{
    PIXELFORMATDESCRIPTOR pfd;
    HWND w;
    HDC dc;
    HGLRC rc = NULL;
    int fmt;
    unsigned char px[4] = { 0, 0, 0, 0 };

    (void)arg;
    w = CreateWindow("threadtest", "", WS_POPUP, 0, 0, 64, 64, NULL, NULL,
                     GetModuleHandle(NULL), NULL);
    if (!w) {
        failures++;
        return 1;
    }
    dc = GetDC(w);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 16;
    fmt = ChoosePixelFormat(dc, &pfd);
    if (!fmt || !SetPixelFormat(dc, fmt, &pfd) || !(rc = wglCreateContext(dc)) ||
        (steps >= 2 && !wglMakeCurrent(dc, rc))) {
        printf("thread %lu: no OpenGL context (format %d)\n", GetCurrentThreadId(), fmt);
        failures++;
    } else if (steps == 2) {
        wglMakeCurrent(NULL, NULL);
    } else if (steps >= 3) {
        glViewport(0, 0, 64, 64);
        glClearColor(0.0f, 0.4f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1.0f, 1.0f, 1.0f);
        glVertex2f(-0.2f, 0.9f);
        glVertex2f(-0.1f, 0.9f);
        glVertex2f(-0.15f, 0.8f);
        glEnd();
        glFinish();
        glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (px[0] > 8 || px[1] < 94 || px[1] > 110 || px[2] > 8) {
            printf("thread %lu: pixel %d %d %d, not the clear colour\n", GetCurrentThreadId(),
                   px[0], px[1], px[2]);
            failures++;
        }
        if (steps >= 4) {
            left_current = rc;                  /* the main thread deletes it */
            return 0;
        }
        wglMakeCurrent(NULL, NULL);
    }
    if (rc)
        wglDeleteContext(rc);
    ReleaseDC(w, dc);
    DestroyWindow(w);
    return 0;
}

int main(int argc, char **argv)
{
    WNDCLASS wc;
    int n = argc > 1 ? atoi(argv[1]) : 300, i;
    int same = argc > 2 && !strcmp(argv[2], "same");
    unsigned long kb0 = 0, kb = 0;

    if (argc > 3)
        steps = atoi(argv[3]);
    if (same && steps >= 4) {
        printf("step 4 needs threads\n");
        return 2;
    }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "threadtest";
    RegisterClass(&wc);
    resources(0, committed_kb());
    for (i = 1; i <= n; i++) {
        DWORD id;
        HANDLE t;
        if (same) {
            worker(NULL);
        } else {
            t = CreateThread(NULL, 0, worker, NULL, 0, &id);
            if (!t) {
                printf("thread %d: CreateThread failed\n", i);
                failures++;
                break;
            }
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
            if (left_current) {
                if (!wglDeleteContext(left_current)) {
                    printf("thread %d: its context, left current, can't be deleted\n", i);
                    failures++;
                }
                left_current = NULL;
            }
        }
        if (i % 50 == 0 || i == n) {
            kb = committed_kb();
            if (i == 50)
                kb0 = kb;
            resources(i, kb);
        }
    }
    if (n > 50 && kb > kb0) {
        unsigned long per = (kb - kb0) * 1024UL / (unsigned long)(n - 50);
        printf("growth after the first 50: %lu KB, %lu bytes a thread\n", kb - kb0, per);
        if (per >= 1024)
            failures++;
    }
    printf(failures ? "threadtest FAILED (%d)\n" : "threadtest passed\n", failures);
    return failures ? 1 : 0;
}
