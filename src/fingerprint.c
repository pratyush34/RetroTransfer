#include "fingerprint_c.h"

#include <stdatomic.h>
#include <stdio.h>

static uint32_t crc32_table[256];
static atomic_flag crc32_lock = ATOMIC_FLAG_INIT;
static atomic_bool crc32_table_ready = ATOMIC_VAR_INIT(0);

static uint64_t rotate_left_64(uint64_t value, unsigned count) {
    return (value << count) | (value >> (64 - count));
}

static uint64_t read_little_endian_64(const uint8_t *data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= (uint64_t)data[i] << (i * 8);
    }
    return value;
}

static uint64_t finalize_hash_64(uint64_t value) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    value ^= value >> 33;
    return value;
}

RtFingerprint128 rt_murmurhash3_128(const uint8_t *data,
                                   size_t size,
                                   uint32_t seed) {
    RtFingerprint128 result = {0, 0};
    if (data == NULL && size != 0) {
        return result;
    }

    const size_t block_count = size / 16;
    uint64_t hash1 = seed;
    uint64_t hash2 = seed;
    const uint64_t constant1 = UINT64_C(0x87c37b91114253d5);
    const uint64_t constant2 = UINT64_C(0x4cf5ad432745937f);

    for (size_t i = 0; i < block_count; ++i) {
        uint64_t key1 = read_little_endian_64(data + i * 16);
        uint64_t key2 = read_little_endian_64(data + i * 16 + 8);

        key1 *= constant1;
        key1 = rotate_left_64(key1, 31);
        key1 *= constant2;
        hash1 ^= key1;

        hash1 = rotate_left_64(hash1, 27);
        hash1 += hash2;
        hash1 = hash1 * 5 + UINT64_C(0x52dce729);

        key2 *= constant2;
        key2 = rotate_left_64(key2, 33);
        key2 *= constant1;
        hash2 ^= key2;

        hash2 = rotate_left_64(hash2, 31);
        hash2 += hash1;
        hash2 = hash2 * 5 + UINT64_C(0x38495ab5);
    }

    const uint8_t *tail = data == NULL ? NULL : data + block_count * 16;
    uint64_t key1 = 0;
    uint64_t key2 = 0;
    switch (size & 15) {
        case 15: key2 ^= (uint64_t)tail[14] << 48; /* fall through */
        case 14: key2 ^= (uint64_t)tail[13] << 40; /* fall through */
        case 13: key2 ^= (uint64_t)tail[12] << 32; /* fall through */
        case 12: key2 ^= (uint64_t)tail[11] << 24; /* fall through */
        case 11: key2 ^= (uint64_t)tail[10] << 16; /* fall through */
        case 10: key2 ^= (uint64_t)tail[9] << 8;   /* fall through */
        case 9:
            key2 ^= (uint64_t)tail[8];
            key2 *= constant2;
            key2 = rotate_left_64(key2, 33);
            key2 *= constant1;
            hash2 ^= key2;
            /* fall through */
        case 8: key1 ^= (uint64_t)tail[7] << 56; /* fall through */
        case 7: key1 ^= (uint64_t)tail[6] << 48; /* fall through */
        case 6: key1 ^= (uint64_t)tail[5] << 40; /* fall through */
        case 5: key1 ^= (uint64_t)tail[4] << 32; /* fall through */
        case 4: key1 ^= (uint64_t)tail[3] << 24; /* fall through */
        case 3: key1 ^= (uint64_t)tail[2] << 16; /* fall through */
        case 2: key1 ^= (uint64_t)tail[1] << 8;  /* fall through */
        case 1:
            key1 ^= (uint64_t)tail[0];
            key1 *= constant1;
            key1 = rotate_left_64(key1, 31);
            key1 *= constant2;
            hash1 ^= key1;
            break;
        default:
            break;
    }

    hash1 ^= size;
    hash2 ^= size;
    hash1 += hash2;
    hash2 += hash1;

    hash1 = finalize_hash_64(hash1);
    hash2 = finalize_hash_64(hash2);

    hash1 += hash2;
    hash2 += hash1;
    result.low = hash1;
    result.high = hash2;
    return result;
}

void rt_compute_fingerprint_hex(const uint8_t *data,
                                size_t size,
                                uint32_t seed,
                                char output[33]) {
    if (output == NULL) {
        return;
    }
    const RtFingerprint128 fingerprint = rt_murmurhash3_128(data, size, seed);
    (void)snprintf(output, 33, "%016llx%016llx",
                   (unsigned long long)fingerprint.high,
                   (unsigned long long)fingerprint.low);
}

static void initialize_crc32_table(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) != 0
                      ? (crc >> 1) ^ UINT32_C(0xedb88320)
                      : crc >> 1;
        }
        crc32_table[i] = crc;
    }
}

uint32_t rt_compute_crc32(const uint8_t *data, size_t size) {
    if (data == NULL && size != 0) {
        return 0;
    }
    if (!atomic_load_explicit(&crc32_table_ready, memory_order_acquire)) {
        while (atomic_flag_test_and_set_explicit(&crc32_lock, memory_order_acquire)) {
        }
        if (!atomic_load_explicit(&crc32_table_ready, memory_order_relaxed)) {
            initialize_crc32_table();
            atomic_store_explicit(&crc32_table_ready, 1, memory_order_release);
        }
        atomic_flag_clear_explicit(&crc32_lock, memory_order_release);
    }

    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xff];
    }
    return crc ^ UINT32_C(0xffffffff);
}
