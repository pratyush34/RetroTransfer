#ifndef RETROTRANSFER_FINGERPRINT_C_H
#define RETROTRANSFER_FINGERPRINT_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t low;
    uint64_t high;
} RtFingerprint128;

RtFingerprint128 rt_murmurhash3_128(const uint8_t *data,
                                   size_t size,
                                   uint32_t seed);
void rt_compute_fingerprint_hex(const uint8_t *data,
                               size_t size,
                               uint32_t seed,
                               char output[33]);
uint32_t rt_compute_crc32(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
