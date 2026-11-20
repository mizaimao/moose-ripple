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

/* OSR2.5 lacks these three exports. Preserve newer systems' implementations
   and the caller's last-error value across the internal symbol lookup. */
static FARPROC optional_kernel_proc(const char *name)
{
    DWORD saved = GetLastError();
    FARPROC fn = GetProcAddress(GetModuleHandleA("KERNEL32.DLL"), name);
    SetLastError(saved);
    return fn;
}

SHIM(BOOL, IsDebuggerPresent, 0, (void))
{
    typedef BOOL (WINAPI *debugger_proc)(void);
    debugger_proc fn = (debugger_proc)optional_kernel_proc("IsDebuggerPresent");
    return fn ? fn() : FALSE;
}

SHIM(BOOL, SwitchToThread, 0, (void))
{
    typedef BOOL (WINAPI *yield_proc)(void);
    yield_proc fn = (yield_proc)optional_kernel_proc("SwitchToThread");
    if (fn)
        return fn();
    /* libc++ ignores the return value. Let another thread run. */
    Sleep(1);
    return FALSE;
}

SHIM(BOOL, TryEnterCriticalSection, 4, (CRITICAL_SECTION *cs))
{
    typedef BOOL (WINAPI *try_proc)(CRITICAL_SECTION *);
    try_proc fn = (try_proc)optional_kernel_proc("TryEnterCriticalSection");
    /* libc++'s recursive_mutex::try_lock permits a spurious failure.
       A failed attempt neither blocks nor alters another thread's lock. */
    return fn ? fn(cs) : FALSE;
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
   callback (libc++'s thread-specific storage behind std::thread frees its data with one) is
   kept and run for a thread's values by k32shim_thread_exit (below), which the DLL calls when a
   thread ends. ------------------------------------------------------------------------------- */
typedef void (WINAPI *fls_callback)(void *);

#define FLS_CALLBACKS 16

static struct {
    DWORD index;
    fls_callback callback;              /* NULL: the entry is free */
} fls_callbacks[FLS_CALLBACKS];
static LONG volatile fls_lock;

static void fls_enter(void)
{
    unsigned tries = 0;
    while (InterlockedCompareExchange(&fls_lock, 1, 0) != 0)
        wait_a_bit(&tries);
}

SHIM(DWORD, FlsAlloc, 4, (fls_callback callback))
{
    DWORD index = TlsAlloc();
    int i;

    if (index == TLS_OUT_OF_INDEXES || !callback)
        return index;
    fls_enter();
    for (i = 0; i < FLS_CALLBACKS; i++) {
        if (!fls_callbacks[i].callback) {
            fls_callbacks[i].index = index;
            fls_callbacks[i].callback = callback;
            break;
        }
    }
    InterlockedExchange(&fls_lock, 0);
    /* With every entry taken, the callback is not kept: what it would free stays allocated. */
    return index;
}

SHIM(void *, FlsGetValue, 4, (DWORD index))
{
    return TlsGetValue(index);
}

SHIM(BOOL, FlsSetValue, 8, (DWORD index, void *value))
{
    return TlsSetValue(index, value);
}

/* Windows runs the callback for every thread's value here; this frees the slot only (no
   library linked here frees an index with a callback while threads hold values in it). */
SHIM(BOOL, FlsFree, 4, (DWORD index))
{
    int i;

    fls_enter();
    for (i = 0; i < FLS_CALLBACKS; i++) {
        if (fls_callbacks[i].callback && fls_callbacks[i].index == index)
            fls_callbacks[i].callback = NULL;
    }
    InterlockedExchange(&fls_lock, 0);
    return TlsFree(index);
}

static void emutls_thread_exit(void);

/* mingw-w64's list of C++ thread_local destructors (__cxa_thread_atexit), which its DLL start-up
   code runs after the DLL's own entry point. Weak: only a DLL's start-up code defines it. NULL
   until a thread_local with a destructor is linked in (none is in Mesa 24.3.4). */
extern PIMAGE_TLS_CALLBACK __mingw_cxa_tls_callback_ptr __attribute__((weak));

/* The DLL that links this object calls this from its entry point when a thread ends
   (DLL_THREAD_DETACH), last. It runs the thread's C++ thread_local destructors (here, as they
   may use thread-local variables; the start-up code's own call afterwards finds none left) and
   its fiber-local storage callbacks, as Windows does, then frees the thread's emulated
   thread-local variables. A thread-local variable used after this (by code that runs later in
   the thread's exit, such as another DLL calling into this one) gets a new copy, which is then
   not freed. */
void k32shim_thread_exit(void);
void k32shim_thread_exit(void)
{
    int round, i, ran;

    if (&__mingw_cxa_tls_callback_ptr && __mingw_cxa_tls_callback_ptr)
        __mingw_cxa_tls_callback_ptr(NULL, DLL_THREAD_DETACH, NULL);
    /* A callback may set values again: a few rounds, as the C library does for its
       thread-specific data. */
    for (round = 0; round < 4; round++) {
        ran = 0;
        for (i = 0; i < FLS_CALLBACKS; i++) {
            fls_callback callback;
            DWORD index;
            void *value;

            fls_enter();
            callback = fls_callbacks[i].callback;
            index = fls_callbacks[i].index;
            InterlockedExchange(&fls_lock, 0);
            if (!callback)
                continue;
            value = TlsGetValue(index);
            if (value) {
                TlsSetValue(index, NULL);
                callback(value);
                ran = 1;
            }
        }
        if (!ran)
            break;
    }
    emutls_thread_exit();
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
   k32shim_thread_exit frees what a thread allocated when the thread ends. */
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
        /* The block's own address goes in the word before the variable, for freeing. */
        size_t align = c->align < sizeof(void *) ? sizeof(void *) : c->align;
        char *raw = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      c->size + align + sizeof(void *));
        size_t i;
        p = (void *)(((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1));
        ((void **)p)[-1] = raw;
        if (c->initial) {
            for (i = 0; i < c->size; i++)
                ((char *)p)[i] = ((const char *)c->initial)[i];
        }
        a->object[index - 1] = p;
    }
    return p;
}

/* Free the calling thread's variables and its array (k32shim_thread_exit). */
static void emutls_thread_exit(void)
{
    struct emutls_array *a;
    uintptr_t i;

    if (emutls_slot == TLS_OUT_OF_INDEXES || !emutls_get)
        return;
    a = (struct emutls_array *)emutls_get(emutls_slot);
    if (!a)
        return;
    TlsSetValue(emutls_slot, NULL);
    for (i = 0; i < a->count; i++) {
        if (a->object[i])
            HeapFree(GetProcessHeap(), 0, ((void **)a->object[i])[-1]);
    }
    HeapFree(GetProcessHeap(), 0, a);
}

/* libc++abi's i686 exception state uses static PE TLS in the prebuilt toolchain.
   Windows 95 cannot dynamically load that DLL. Compile its two accessors here so
   Clang uses this driver's per-thread storage and thread-exit cleanup instead. */
struct __cxa_exception;
struct __cxa_eh_globals {
    struct __cxa_exception *caughtExceptions;
    unsigned int uncaughtExceptions;
};
_Static_assert(sizeof(struct __cxa_eh_globals) == 8, "32-bit libc++abi exception state");
static __thread struct __cxa_eh_globals eh_globals;

struct __cxa_eh_globals *__cdecl __cxa_get_globals(void)
{
    return &eh_globals;
}

struct __cxa_eh_globals *__cdecl __cxa_get_globals_fast(void)
{
    return &eh_globals;
}
