/*
 * moose-ripple shim: the few Linux type names the vendored virtio headers use, for the
 * macOS 10.6 kernel (x86_64, little-endian, so the __le and __virtio types are plain
 * integers). Not from Linux.
 */
#ifndef RIPPLE_LINUX_TYPES_H
#define RIPPLE_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;
typedef uint64_t __be64;
#define __bitwise   /* sparse annotation in Linux; nothing here */
#endif
