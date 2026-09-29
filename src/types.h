/*
 * gbemu - a Game Boy emulator in C
 *
 * types.h - fixed-width integer types used across the whole codebase.
 */

#ifndef GB_TYPES_H
#define GB_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t  s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#endif /* GB_TYPES_H */
