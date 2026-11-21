#ifndef TEST_INTEGRITY_FINGERPRINT_HPP
#define TEST_INTEGRITY_FINGERPRINT_HPP

#include "fingerprint_c.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static void test_crc32_empty_buffer() {
    uint32_t crc = rt_compute_crc32(nullptr, 0);
    TEST_ASSERT(crc == 0, "CRC32 of empty/null buffer should be 0");

    const uint8_t dummy = 0xAA;
    crc = rt_compute_crc32(&dummy, 0);
    TEST_ASSERT(crc == 0, "CRC32 of 0 length buffer should be 0");
}

static void test_crc32_standard_vectors_and_single_byte() {
    const char* str = "123456789";
    uint32_t crc = rt_compute_crc32(reinterpret_cast<const uint8_t*>(str), 9);
    TEST_ASSERT(crc == 0xCBF43926, "CRC32 standard vector mismatch");

    const uint8_t byte0 = 0x00;
    const uint32_t crc0 = rt_compute_crc32(&byte0, 1);
    TEST_ASSERT(crc0 != 0, "CRC32 of single byte 0x00 should not be 0");

    const uint8_t byteFF = 0xFF;
    const uint32_t crcFF = rt_compute_crc32(&byteFF, 1);
    TEST_ASSERT(crcFF != 0, "CRC32 of single byte 0xFF should not be 0");
    TEST_ASSERT(crc0 != crcFF, "CRC32 of 0x00 and 0xFF must differ");
}

static void test_crc32_bit_flip_sensitivity() {
    std::vector<uint8_t> buffer(1024, 0x5A);
    const uint32_t originalCrc = rt_compute_crc32(buffer.data(), buffer.size());

    // Test bit flips across various positions
    const size_t testPositions[] = {0, 1, 15, 128, 512, 1023};
    for (size_t pos : testPositions) {
        for (int bit = 0; bit < 8; ++bit) {
            buffer[pos] ^= (1 << bit);
            const uint32_t flippedCrc = rt_compute_crc32(buffer.data(), buffer.size());
            TEST_ASSERT(flippedCrc != originalCrc,
                        "CRC32 failed to detect single bit flip");
            buffer[pos] ^= (1 << bit); // restore
        }
    }
}

static void test_murmurhash3_empty_and_seeds() {
    RtFingerprint128 fp1 = rt_murmurhash3_128(nullptr, 0, 42);
    RtFingerprint128 fp2 = rt_murmurhash3_128(nullptr, 0, 43);
    // Different seeds must yield different hashes even on empty input
    TEST_ASSERT(fp1.low != fp2.low || fp1.high != fp2.high,
                "MurmurHash3 with different seeds on empty data must produce different results");

    const uint8_t data[] = "RetroTransfer Deterministic Hashing Test Buffer";
    const size_t len = sizeof(data) - 1;
    RtFingerprint128 fpA = rt_murmurhash3_128(data, len, 100);
    RtFingerprint128 fpB = rt_murmurhash3_128(data, len, 100);
    TEST_ASSERT(fpA.low == fpB.low && fpA.high == fpB.high,
                "MurmurHash3 must be strictly deterministic with identical inputs");

    RtFingerprint128 fpC = rt_murmurhash3_128(data, len, 200);
    TEST_ASSERT(fpA.low != fpC.low || fpA.high != fpC.high,
                "MurmurHash3 with different seeds must produce different hashes");
}

static void test_fingerprint_hex_format() {
    const uint8_t sample[] = "Test data for hex format check";
    char hexOut[33] = {};
    rt_compute_fingerprint_hex(sample, sizeof(sample) - 1, 42, hexOut);

    TEST_ASSERT(strlen(hexOut) == 32, "Hex output must be exactly 32 characters long");
    for (int i = 0; i < 32; ++i) {
        const char c = hexOut[i];
        const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        TEST_ASSERT(isHex, "Hex output must contain only lowercase hexadecimal digits");
    }
    TEST_ASSERT(hexOut[32] == '\0', "Hex output must be null terminated");
}

static void test_fingerprint_single_byte_mutation() {
    std::vector<uint8_t> buffer(65536, 0x33);
    char hex1[33] = {};
    char hex2[33] = {};

    rt_compute_fingerprint_hex(buffer.data(), buffer.size(), 42, hex1);
    buffer[32768] ^= 0x01; // mutate one bit in the middle
    rt_compute_fingerprint_hex(buffer.data(), buffer.size(), 42, hex2);

    TEST_ASSERT(std::string(hex1) != std::string(hex2),
                "MurmurHash3 fingerprint must change on single-byte mutation in 64KB block");
}

#endif // TEST_INTEGRITY_FINGERPRINT_HPP
