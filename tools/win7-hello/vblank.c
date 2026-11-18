/* Measure 180 vertical blank waits on the Windows 7 display adapter.
 * MIT licence (LICENSE at the repository root).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

/* Windows 7 d3dkmthk.h layouts for the three gdi32 entry points. */
struct open_adapter { HDC dc; UINT adapter; LUID luid; UINT source; };
struct wait_vblank { UINT adapter, device, source; };
typedef LONG (WINAPI *open_fn)(struct open_adapter *);
typedef LONG (WINAPI *wait_fn)(const struct wait_vblank *);
typedef LONG (WINAPI *close_fn)(const UINT *);

static int compare(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(void)
{
    enum { count = 180 };
    double gaps[count];
    LARGE_INTEGER frequency, first, last, now;
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    open_fn open_adapter = (open_fn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
    wait_fn wait_blank = (wait_fn)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
    close_fn close_adapter = (close_fn)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    struct open_adapter adapter = {0};
    adapter.dc = CreateDCA("DISPLAY", NULL, NULL, NULL);
    int rc = 1;
    if (!adapter.dc || !open_adapter || !wait_blank || !close_adapter ||
        !QueryPerformanceFrequency(&frequency) || open_adapter(&adapter) < 0) {
        fprintf(stderr, "Could not open the display adapter.\n");
        goto done;
    }
    struct wait_vblank wait = {adapter.adapter, 0, adapter.source};
    for (int i = 0; i <= count; ++i) {
        LONG status = wait_blank(&wait);
        if (status < 0) {
            fprintf(stderr, "Vertical blank wait failed (0x%08lx).\n", (unsigned long)status);
            goto close;
        }
        QueryPerformanceCounter(&now);
        if (i == 0) first = now;
        else gaps[i - 1] = (now.QuadPart - last.QuadPart) * 1000.0 / frequency.QuadPart;
        last = now;
    }
    qsort(gaps, count, sizeof(*gaps), compare);
    printf("%u waits, %.1f Hz, median %.1f ms, p95 %.1f ms\n", (unsigned)count,
           count * (double)frequency.QuadPart / (last.QuadPart - first.QuadPart),
           (gaps[count / 2 - 1] + gaps[count / 2]) / 2, gaps[(95 * count + 99) / 100 - 1]);
    rc = 0;
close:
    close_adapter(&adapter.adapter);
done:
    if (adapter.dc) DeleteDC(adapter.dc);
    return rc;
}
