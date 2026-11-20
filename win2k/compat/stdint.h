/* VC6 lacks stdint.h. The vendored virtio and virgl protocol headers
 * need these exact-width integer types on the little-endian x86 guest. */
#ifndef RB2K_COMPAT_STDINT_H
#define RB2K_COMPAT_STDINT_H
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned __int64 uint64_t;
typedef signed __int64 int64_t;
#endif
