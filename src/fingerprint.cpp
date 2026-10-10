#include "fingerprint.h"

#include <cstdio>
#include <cstring>

static inline uint64_t rotl64(uint64_t x, int8_t r) {
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t getblock64(const uint8_t* p, size_t i) {
    uint64_t val = 0;
    std::memcpy(&val, p + (i * 8), sizeof(uint64_t));
    return val;
}

static inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

std::string Fingerprint128::to_hex() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(high),
                  static_cast<unsigned long long>(low));
    return std::string(buf);
}

Fingerprint128 murmurhash3_128(const uint8_t* data, size_t len, uint32_t seed) {
    const size_t nblocks = len / 16;
    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    // Body: process 16-byte blocks
    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1 = getblock64(data, i * 2 + 0);
        uint64_t k2 = getblock64(data, i * 2 + 1);

        k1 *= c1;
        k1 = rotl64(k1, 31);
        k1 *= c2;
        h1 ^= k1;

        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2 = rotl64(k2, 33);
        k2 *= c1;
        h2 ^= k2;

        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }

    // Tail: remaining 1..15 bytes
    const uint8_t* tail = data + nblocks * 16;
    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch (len & 15) {
        case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
        case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
        case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
        case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
        case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
        case 10: k2 ^= static_cast<uint64_t>(tail[ 9]) << 8;  [[fallthrough]];
        case  9: k2 ^= static_cast<uint64_t>(tail[ 8]) << 0;
                 k2 *= c2;
                 k2 = rotl64(k2, 33);
                 k2 *= c1;
                 h2 ^= k2;
                 [[fallthrough]];
        case  8: k1 ^= static_cast<uint64_t>(tail[ 7]) << 56; [[fallthrough]];
        case  7: k1 ^= static_cast<uint64_t>(tail[ 6]) << 48; [[fallthrough]];
        case  6: k1 ^= static_cast<uint64_t>(tail[ 5]) << 40; [[fallthrough]];
        case  5: k1 ^= static_cast<uint64_t>(tail[ 4]) << 32; [[fallthrough]];
        case  4: k1 ^= static_cast<uint64_t>(tail[ 3]) << 24; [[fallthrough]];
        case  3: k1 ^= static_cast<uint64_t>(tail[ 2]) << 16; [[fallthrough]];
        case  2: k1 ^= static_cast<uint64_t>(tail[ 1]) << 8;  [[fallthrough]];
        case  1: k1 ^= static_cast<uint64_t>(tail[ 0]) << 0;
                 k1 *= c1;
                 k1 = rotl64(k1, 31);
                 k1 *= c2;
                 h1 ^= k1;
                 break;
        default:
                 break;
    }

    // Finalization mix
    h1 ^= len;
    h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    Fingerprint128 result;
    result.low = h1;
    result.high = h2;
    return result;
}

std::string compute_fingerprint_hex(const uint8_t* data, size_t len, uint32_t seed) {
    return murmurhash3_128(data, len, seed).to_hex();
}

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

uint32_t compute_crc32(const uint8_t* data, size_t len) {
    if (!crc32_table_initialized) {
        init_crc32();
    }
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFU;
}
