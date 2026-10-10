#ifndef FINGERPRINT_H
#define FINGERPRINT_H

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Represents a 128-bit hash output from MurmurHash3_x64_128.
 */
struct Fingerprint128 {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const Fingerprint128& other) const {
        return low == other.low && high == other.high;
    }

    bool operator!=(const Fingerprint128& other) const {
        return !(*this == other);
    }

    /**
     * @brief Formats the 128-bit hash as a deterministic 32-character lowercase hex string.
     */
    std::string to_hex() const;
};

/**
 * @brief Computes 128-bit MurmurHash3 (x64_128) over arbitrary binary byte buffers.
 * 
 * NOTE ON SECURITY & COLLISIONS:
 * MurmurHash3 is an extremely fast, high-quality non-cryptographic hash with
 * excellent avalanche characteristics and a 128-bit output space. While collision
 * probability is vanishingly small for typical chunk workloads, non-cryptographic hashes
 * should not be treated as absolute security guarantees. The deduplication layer
 * must support byte-by-byte verification before discarding duplicate chunks.
 * 
 * @param data Pointer to raw byte buffer (supports binary & embedded nulls).
 * @param len Byte length of the buffer.
 * @param seed Hash seed (default: 42).
 * @return Fingerprint128 structure with 128-bit hash.
 */
Fingerprint128 murmurhash3_128(const uint8_t* data, size_t len, uint32_t seed = 42);

/**
 * @brief Computes MurmurHash3_x64_128 and directly returns the 32-char hex string.
 */
std::string compute_fingerprint_hex(const uint8_t* data, size_t len, uint32_t seed = 42);

/**
 * @brief Computes standard IEEE 802.3 CRC32 checksum for corruption detection.
 * 
 * Used for transmission integrity verification, distinct from chunk identity fingerprinting.
 * 
 * @param data Pointer to raw byte buffer.
 * @param len Byte length.
 * @return 32-bit CRC checksum.
 */
uint32_t compute_crc32(const uint8_t* data, size_t len);

#endif // FINGERPRINT_H
