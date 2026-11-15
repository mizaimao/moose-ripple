/*
 * Functions that Windows 98 lacks (kernel32, and one in the C run-time), for programs and DLLs built with a current MinGW
 * toolchain (llvm-mingw: clang, compiler-rt, libc++, libunwind, mingw-w64).
 *
 * Those libraries call a handful of functions that arrived with Windows XP and Vista. Windows
 * 98 refuses to load a file that imports a function its kernel32.dll doesn't export, so this
 * object defines them itself: each function, and the "__imp__" pointer that code compiled
 * against the import library calls through. Link it as an object file (not from an archive),
 * so that it is taken before the import library:
 *
 *     i686-w64-mingw32-clang ... k32shim.o
 *
 * tools/win9x/check-imports.py lists what a linked file still imports beyond Windows 98 SE.
 * Everything here is built from Windows 95-level functions.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

/* Define NAME under its stdcall symbol and as the import pointer, without the header's
   dllimport declaration getting in the way. */
#define SHIM(ret, name, bytes, args)                                             \
    ret WINAPI shim_##name args __asm__("_" #name "@" #bytes);                   \
    extern void *const imp_##name __asm__("__imp__" #name "@" #bytes);           \
    void *const imp_##name = (void *)shim_##name;                                \
    ret WINAPI shim_##name args

static void wait_a_bit(unsigned *tries)
{
    Sleep(++*tries < 16 ? 0 : 1);
}

/* ---- One-time initialisation: the word is 0 (not run), 1 (running) or 2 (done) --------------- */
typedef BOOL (WINAPI *once_fn)(void *once, void *param, void **context);

SHIM(BOOL, InitOnceExecuteOnce, 16, (LONG volatile *once, once_fn fn, void *param, void **context))
{
    unsigned tries = 0;
    for (;;) {
        LONG state = InterlockedCompareExchange(once, 1, 0);
        if (state == 2)
            return TRUE;
        if (state == 0) {
            BOOL ok = fn((void *)once, param, context);
            InterlockedExchange(once, ok ? 2 : 0);
            return ok;
        }
        wait_a_bit(&tries);
    }
}

/* ---- Slim reader/writer locks: bit 0 is the writer, the rest counts readers (in twos) ------- */
SHIM(void, AcquireSRWLockExclusive, 4, (LONG volatile *lock))
{
    unsigned tries = 0;
    while (InterlockedCompareExchange(lock, 1, 0) != 0)
        wait_a_bit(&tries);
}

SHIM(BOOLEAN, TryAcquireSRWLockExclusive, 4, (LONG volatile *lock))
{
    return InterlockedCompareExchange(lock, 1, 0) == 0;
}

SHIM(void, ReleaseSRWLockExclusive, 4, (LONG volatile *lock))
{
    InterlockedExchange(lock, 0);
}

SHIM(void, AcquireSRWLockShared, 4, (LONG volatile *lock))
{
    unsigned tries = 0;
    for (;;) {
        LONG v = *lock;
        if (!(v & 1) && InterlockedCompareExchange(lock, v + 2, v) == v)
            return;
        wait_a_bit(&tries);
    }
}

SHIM(void, ReleaseSRWLockShared, 4, (LONG volatile *lock))
{
    InterlockedExchangeAdd(lock, -2);
}

/* ---- Condition variables: the word points to a semaphore and a count of waiters without a
   wake-up. Vista's have no "destroy", so neither do these: one small block per variable stays
   until the process ends. ------------------------------------------------------------------- */
struct cv { HANDLE sem; LONG volatile waiters; };

static struct cv *cv_get(void *volatile *word)
{
    struct cv *c = (struct cv *)*word;
    if (!c) {
        c = (struct cv *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *c);
        c->sem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
        if (InterlockedCompareExchangePointer(word, c, NULL) != NULL) {
            CloseHandle(c->sem);
            HeapFree(GetProcessHeap(), 0, c);
            c = (struct cv *)*word;
        }
    }
    return c;
}

SHIM(BOOL, SleepConditionVariableSRW, 16, (void *volatile *word, LONG volatile *lock, DWORD ms, ULONG flags))
{
    struct cv *c = cv_get(word);
    BOOL woken;

    InterlockedIncrement(&c->waiters);
    if (flags & 1) shim_ReleaseSRWLockShared(lock); else shim_ReleaseSRWLockExclusive(lock);
    woken = WaitForSingleObject(c->sem, ms) == WAIT_OBJECT_0;
    while (!woken) {                    /* timed out: take our count back */
        LONG n = c->waiters;
        if (n <= 0) {                   /* someone already woke us: its token is on the way */
            WaitForSingleObject(c->sem, INFINITE);
            woken = TRUE;
        } else if (InterlockedCompareExchange(&c->waiters, n - 1, n) == n)
            break;
    }
    if (flags & 1) shim_AcquireSRWLockShared(lock); else shim_AcquireSRWLockExclusive(lock);
    if (!woken)
        SetLastError(ERROR_TIMEOUT);
    return woken;
}

SHIM(void, WakeConditionVariable, 4, (void *volatile *word))
{
    struct cv *c = (struct cv *)*word;
    while (c) {
        LONG n = c->waiters;
        if (n <= 0)
            return;
        if (InterlockedCompareExchange(&c->waiters, n - 1, n) == n) {
            ReleaseSemaphore(c->sem, 1, NULL);
            return;
        }
    }
}

SHIM(void, WakeAllConditionVariable, 4, (void *volatile *word))
{
    struct cv *c = (struct cv *)*word;
    if (c) {
        LONG n = InterlockedExchange(&c->waiters, 0);
        if (n > 0)
            ReleaseSemaphore(c->sem, n, NULL);
    }
}

/* ---- Fiber-local storage: thread-local storage does, as nothing here uses fibers. The
   callback (run when a thread ends) is not called: what it would free stays allocated. ------ */
SHIM(DWORD, FlsAlloc, 4, (void *callback))
{
    (void)callback;
    return TlsAlloc();
}

SHIM(void *, FlsGetValue, 4, (DWORD index))
{
    return TlsGetValue(index);
}

SHIM(BOOL, FlsSetValue, 8, (DWORD index, void *value))
{
    return TlsSetValue(index, value);
}

SHIM(BOOL, FlsFree, 4, (DWORD index))
{
    return TlsFree(index);
}

/* ---- Small things ----------------------------------------------------------------------------- */
SHIM(DWORD, GetActiveProcessorCount, 4, (WORD group))
{
    SYSTEM_INFO si;
    (void)group;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

SHIM(DWORD, GetThreadId, 4, (HANDLE thread))
{
    return thread == GetCurrentThread() ? GetCurrentThreadId() : 0;
}

/* The list of loaded modules (the C++ run-time looks through it for a function's unwind
   tables). Windows 98 has no psapi; its tool-help functions give the same list. */
SHIM(BOOL, K32EnumProcessModules, 16, (HANDLE process, HMODULE *list, DWORD bytes, DWORD *needed))
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32 me;
    DWORD n = 0;

    (void)process;
    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    me.dwSize = sizeof me;
    if (Module32First(snap, &me)) {
        do {
            if ((n + 1) * sizeof(HMODULE) <= bytes)
                list[n] = me.hModule;
            n++;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    if (needed)
        *needed = n * sizeof(HMODULE);
    return TRUE;
}

SHIM(DWORD, GetCurrentProcessorNumber, 0, (void))
{
    return 0;
}

/* Callers only ask "is there a console to write messages to": yes when the process has a
   standard output (Windows 98 has no way to get the console's window). */
SHIM(HWND, GetConsoleWindow, 0, (void))
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    return (out && out != INVALID_HANDLE_VALUE) ? (HWND)1 : NULL;
}

struct memory_status_ex {
    DWORD length, load;
    ULONGLONG total_phys, avail_phys, total_page, avail_page, total_virtual, avail_virtual,
              avail_extended;
};

SHIM(BOOL, GlobalMemoryStatusEx, 4, (struct memory_status_ex *out))
{
    MEMORYSTATUS ms;
    ms.dwLength = sizeof ms;
    GlobalMemoryStatus(&ms);
    out->load = ms.dwMemoryLoad;
    out->total_phys = ms.dwTotalPhys;       out->avail_phys = ms.dwAvailPhys;
    out->total_page = ms.dwTotalPageFile;   out->avail_page = ms.dwAvailPageFile;
    out->total_virtual = ms.dwTotalVirtual; out->avail_virtual = ms.dwAvailVirtual;
    out->avail_extended = 0;
    return TRUE;
}

/* ---- Condition variables used with a critical section ------------------------------------------ */
SHIM(void, InitializeConditionVariable, 4, (void *volatile *word))
{
    *word = NULL;
}

SHIM(BOOL, SleepConditionVariableCS, 12, (void *volatile *word, CRITICAL_SECTION *cs, DWORD ms))
{
    struct cv *c = cv_get(word);
    BOOL woken;

    InterlockedIncrement(&c->waiters);
    LeaveCriticalSection(cs);
    woken = WaitForSingleObject(c->sem, ms) == WAIT_OBJECT_0;
    while (!woken) {
        LONG n = c->waiters;
        if (n <= 0) {
            WaitForSingleObject(c->sem, INFINITE);
            woken = TRUE;
        } else if (InterlockedCompareExchange(&c->waiters, n - 1, n) == n)
            break;
    }
    EnterCriticalSection(cs);
    if (!woken)
        SetLastError(ERROR_TIMEOUT);
    return woken;
}

/* ---- C run-time: Windows 98's msvcrt.dll is Visual C++ 6's ------------------------------------- */
/* qsort_s (Microsoft's argument order: the context first): a heap sort, in place. */
typedef int (__cdecl *qsort_s_cmp)(void *context, const void *a, const void *b);

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, size_t start, size_t end, size_t width, qsort_s_cmp cmp, void *context)
{
    size_t root = start;
    for (;;) {
        size_t child = 2 * root + 1, pick = root;
        if (child >= end)
            return;
        if (cmp(context, base + pick * width, base + child * width) < 0)
            pick = child;
        if (child + 1 < end && cmp(context, base + pick * width, base + (child + 1) * width) < 0)
            pick = child + 1;
        if (pick == root)
            return;
        swap_bytes(base + root * width, base + pick * width, width);
        root = pick;
    }
}

void __cdecl shim_qsort_s(void *base, size_t num, size_t width, qsort_s_cmp cmp, void *context)
    __asm__("_qsort_s");
extern void *const imp_qsort_s __asm__("__imp__qsort_s");
void *const imp_qsort_s = (void *)shim_qsort_s;

void __cdecl shim_qsort_s(void *base, size_t num, size_t width, qsort_s_cmp cmp, void *context)
{
    size_t i;

    if (num < 2 || !width)
        return;
    for (i = num / 2; i-- > 0; )
        sift_down((char *)base, i, num, width, cmp, context);
    for (i = num - 1; i > 0; i--) {
        swap_bytes((char *)base, (char *)base + i * width, width);
        sift_down((char *)base, 0, i, width, cmp, context);
    }
}

/* ---- Functions Windows 98 has, but that behave differently ------------------------------------- */
/* TlsGetValue: on Windows NT it sets the last error to "success" when it returns, so that a
   stored NULL can be told from a failure; the compiler's run-time (emulated thread-local
   storage) relies on that and aborts otherwise. Windows 98 leaves the last error alone. */
SHIM(void *, TlsGetValue, 4, (DWORD index))
{
    static void *(WINAPI *real)(DWORD);
    void *value;

    if (!real)
        real = (void *(WINAPI *)(DWORD))GetProcAddress(GetModuleHandleA("KERNEL32.DLL"), "TlsGetValue");
    value = real(index);
    if (!value)
        SetLastError(ERROR_SUCCESS);
    return value;
}

/* ---- Emulated thread-local storage (-femulated-tls) ------------------------------------------ */
/* The compiler turns every access to a thread-local variable into a call of
   __emutls_get_address with the variable's control block. This definition takes the place of
   the compiler run-time's own, which checks the last error after TlsGetValue (see above) and
   needs one-time initialisation and aligned allocation from the C run-time; this one needs
   the process heap and one thread-local storage slot per program or DLL.
   What a thread allocated stays allocated when the thread ends. */
struct emutls_control {
    size_t size, align;
    uintptr_t index;                    /* 0 until first used, then 1, 2, ... */
    void *initial;                      /* the initial value, NULL for all zeroes */
};

struct emutls_array {
    uintptr_t count;
    void *object[1];
};

static DWORD emutls_slot = TLS_OUT_OF_INDEXES;
static LONG volatile emutls_lock;
static uintptr_t emutls_objects;
static void *(WINAPI *emutls_get)(DWORD);

static uintptr_t emutls_index(struct emutls_control *c)
{
    uintptr_t index;

    while (InterlockedCompareExchange(&emutls_lock, 1, 0) != 0)
        Sleep(0);
    if (emutls_slot == TLS_OUT_OF_INDEXES) {
        emutls_get = (void *(WINAPI *)(DWORD))GetProcAddress(GetModuleHandleA("KERNEL32.DLL"),
                                                             "TlsGetValue");
        emutls_slot = TlsAlloc();
    }
    if (!c->index)
        c->index = ++emutls_objects;
    index = c->index;
    InterlockedExchange(&emutls_lock, 0);
    return index;
}

void *__emutls_get_address(struct emutls_control *c);
void *__emutls_get_address(struct emutls_control *c)
{
    uintptr_t index = c->index;
    struct emutls_array *a;
    void *p;

    if (!index)
        index = emutls_index(c);
    a = (struct emutls_array *)emutls_get(emutls_slot);
    if (!a || a->count < index) {
        uintptr_t count = (index + 15) & ~(uintptr_t)15;
        SIZE_T bytes = sizeof *a + count * sizeof(void *);
        a = a ? (struct emutls_array *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, a, bytes)
              : (struct emutls_array *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
        a->count = count;
        TlsSetValue(emutls_slot, a);
    }
    p = a->object[index - 1];
    if (!p) {
        size_t align = c->align < sizeof(void *) ? sizeof(void *) : c->align;
        char *raw = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, c->size + align);
        size_t i;
        p = (void *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
        if (c->initial) {
            for (i = 0; i < c->size; i++)
                ((char *)p)[i] = ((const char *)c->initial)[i];
        }
        a->object[index - 1] = p;
    }
    return p;
}
