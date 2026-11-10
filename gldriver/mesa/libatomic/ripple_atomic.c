/*
 * ripple_atomic.c: the libatomic calls clang emits for 32-bit x86 (moose-ripple, M4).
 *
 * On i386 Mac OS X a uint64_t is only 4-byte aligned, so clang can't prove an 8-byte atomic is
 * aligned and calls the generic libatomic functions (__atomic_load, __atomic_compare_exchange,
 * ...). Mac OS X 10.6 has no libatomic; gldriver/mesa/build.sh compiles this file into a
 * static libatomic.a for the i386 Mesa build. 8-byte operations use `lock cmpxchg8b`, which is
 * atomic on any alignment (a misaligned one is slow, not wrong). Other sizes, which clang
 * inlines in practice, fall back to a spinlock.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__i386__)
#error "only needed for i386"
#endif

/* Compare-and-swap of 8 bytes at p; returns the value found. */
static uint64_t
cas8(void *p, uint64_t expected, uint64_t desired)
{
   uint32_t lo = (uint32_t)desired, hi = (uint32_t)(desired >> 32);
   __asm__ __volatile__("lock; cmpxchg8b %1"
                        : "+A"(expected), "+m"(*(volatile uint64_t *)p)
                        : "b"(lo), "c"(hi)
                        : "memory", "cc");
   return expected;
}

static uint64_t
load8(void *p)
{
   return cas8(p, 0, 0);       /* writes 0 only where 0 already was */
}

static volatile int lock_word;

static void
lock(void)
{
   while (__sync_lock_test_and_set(&lock_word, 1))
      while (lock_word)
         __asm__ __volatile__("pause");
}

static void
unlock(void)
{
   __sync_lock_release(&lock_word);
}

/* clang won't let C define its builtins' names: our functions carry the libatomic symbol
 * names through asm labels (Mach-O adds the leading underscore), as compiler-rt does. */
#define AS(name) __asm__("___atomic_" #name)
void ra_load(size_t size, void *src, void *dst, int order) AS(load);
void ra_store(size_t size, void *dst, void *src, int order) AS(store);
void ra_exchange(size_t size, void *obj, void *val, void *ret, int order) AS(exchange);
bool ra_compare_exchange(size_t size, void *obj, void *expected, void *desired,
                         int success, int failure) AS(compare_exchange);
uint64_t ra_load_8(void *p, int order) AS(load_8);
void ra_store_8(void *p, uint64_t v, int order) AS(store_8);
uint64_t ra_exchange_8(void *p, uint64_t v, int order) AS(exchange_8);
bool ra_compare_exchange_8(void *p, uint64_t *expected, uint64_t desired,
                           int success, int failure) AS(compare_exchange_8);
uint64_t ra_fetch_add_8(void *p, uint64_t v, int order) AS(fetch_add_8);
uint64_t ra_fetch_sub_8(void *p, uint64_t v, int order) AS(fetch_sub_8);
uint64_t ra_fetch_and_8(void *p, uint64_t v, int order) AS(fetch_and_8);
uint64_t ra_fetch_or_8(void *p, uint64_t v, int order) AS(fetch_or_8);
uint64_t ra_fetch_xor_8(void *p, uint64_t v, int order) AS(fetch_xor_8);

void
ra_load(size_t size, void *src, void *dst, int order)
{
   if (size == 8) {
      uint64_t v = load8(src);
      memcpy(dst, &v, 8);
      return;
   }
   lock();
   memcpy(dst, src, size);
   unlock();
}

void
ra_store(size_t size, void *dst, void *src, int order)
{
   if (size == 8) {
      uint64_t v, old = load8(dst), seen;
      memcpy(&v, src, 8);
      while ((seen = cas8(dst, old, v)) != old)
         old = seen;
      return;
   }
   lock();
   memcpy(dst, src, size);
   unlock();
}

void
ra_exchange(size_t size, void *obj, void *val, void *ret, int order)
{
   if (size == 8) {
      uint64_t v, old = load8(obj), seen;
      memcpy(&v, val, 8);
      while ((seen = cas8(obj, old, v)) != old)
         old = seen;
      memcpy(ret, &old, 8);
      return;
   }
   lock();
   memcpy(ret, obj, size);
   memcpy(obj, val, size);
   unlock();
}

bool
ra_compare_exchange(size_t size, void *obj, void *expected, void *desired,
                    int success, int failure)
{
   bool ok;

   if (size == 8) {
      uint64_t e, d, seen;
      memcpy(&e, expected, 8);
      memcpy(&d, desired, 8);
      seen = cas8(obj, e, d);
      if (seen == e)
         return true;
      memcpy(expected, &seen, 8);
      return false;
   }
   lock();
   ok = memcmp(obj, expected, size) == 0;
   if (ok)
      memcpy(obj, desired, size);
   else
      memcpy(expected, obj, size);
   unlock();
   return ok;
}

uint64_t ra_load_8(void *p, int order) { return load8(p); }

void
ra_store_8(void *p, uint64_t v, int order)
{
   ra_store(8, p, &v, order);
}

uint64_t
ra_exchange_8(void *p, uint64_t v, int order)
{
   uint64_t old;
   ra_exchange(8, p, &v, &old, order);
   return old;
}

bool
ra_compare_exchange_8(void *p, uint64_t *expected, uint64_t desired,
                      int success, int failure)
{
   return ra_compare_exchange(8, p, expected, &desired, success, failure);
}

#define FETCH_OP(name, expr)                                    \
   uint64_t                                                     \
   ra_fetch_##name##_8(void *p, uint64_t v, int order)           \
   {                                                            \
      uint64_t old = load8(p), seen;                            \
      while ((seen = cas8(p, old, (expr))) != old)              \
         old = seen;                                            \
      return old;                                               \
   }
FETCH_OP(add, old + v)
FETCH_OP(sub, old - v)
FETCH_OP(and, old & v)
FETCH_OP(or, old | v)
FETCH_OP(xor, old ^ v)
