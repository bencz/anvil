#ifndef ANVIL_ENDIAN_H
#define ANVIL_ENDIAN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AnvilEndianness {
    ANVIL_ENDIANNESS_HOST,
    ANVIL_ENDIANNESS_LITTLE,
    ANVIL_ENDIANNESS_BIG
} AnvilEndianness;

static inline bool anvil_host_is_little_endian(void) {
    uint16_t x = 1;
    return *((uint8_t*)&x) == 1;
}

static inline AnvilEndianness anvil_host_endianness(void) {
    return anvil_host_is_little_endian() ? ANVIL_ENDIANNESS_LITTLE : ANVIL_ENDIANNESS_BIG;
}

static inline uint16_t anvil_swap16(uint16_t x) {
    return (x >> 8) | (x << 8);
}

static inline uint32_t anvil_swap32(uint32_t x) {
    return ((x >> 24) & 0x000000FF) |
           ((x >> 8)  & 0x0000FF00) |
           ((x << 8)  & 0x00FF0000) |
           ((x << 24) & 0xFF000000);
}

static inline uint64_t anvil_swap64(uint64_t x) {
    return ((x >> 56) & 0x00000000000000FFULL) |
           ((x >> 40) & 0x000000000000FF00ULL) |
           ((x >> 24) & 0x0000000000FF0000ULL) |
           ((x >> 8)  & 0x00000000FF000000ULL) |
           ((x << 8)  & 0x000000FF00000000ULL) |
           ((x << 24) & 0x0000FF0000000000ULL) |
           ((x << 40) & 0x00FF000000000000ULL) |
           ((x << 56) & 0xFF00000000000000ULL);
}

static inline uint16_t anvil_to_target16(uint16_t x, AnvilEndianness target) {
    AnvilEndianness host = anvil_host_endianness();
    if (host == target || target == ANVIL_ENDIANNESS_HOST) return x;
    return anvil_swap16(x);
}

static inline uint32_t anvil_to_target32(uint32_t x, AnvilEndianness target) {
    AnvilEndianness host = anvil_host_endianness();
    if (host == target || target == ANVIL_ENDIANNESS_HOST) return x;
    return anvil_swap32(x);
}

static inline uint64_t anvil_to_target64(uint64_t x, AnvilEndianness target) {
    AnvilEndianness host = anvil_host_endianness();
    if (host == target || target == ANVIL_ENDIANNESS_HOST) return x;
    return anvil_swap64(x);
}

static inline int16_t anvil_to_target_i16(int16_t x, AnvilEndianness target) {
    return (int16_t)anvil_to_target16((uint16_t)x, target);
}

static inline int32_t anvil_to_target_i32(int32_t x, AnvilEndianness target) {
    return (int32_t)anvil_to_target32((uint32_t)x, target);
}

static inline int64_t anvil_to_target_i64(int64_t x, AnvilEndianness target) {
    return (int64_t)anvil_to_target64((uint64_t)x, target);
}

#ifdef __cplusplus
}
#endif

#endif
