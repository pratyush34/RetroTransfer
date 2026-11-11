#include "storage/chunk_verifier.h"
#include "fingerprint_c.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace retro::storage {

bool ChunkVerifier::isValidFingerprintFormat(const std::string& fingerprint) {
    if (fingerprint.size() != 32) return false;
    return std::all_of(fingerprint.begin(), fingerprint.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

std::string ChunkVerifier::computeFingerprint(const uint8_t* data, size_t size, uint32_t seed) {
    char hex[33] = {};
    rt_compute_fingerprint_hex(data, size, seed, hex);
    return std::string(hex);
}

uint32_t ChunkVerifier::computeCrc32(const uint8_t* data, size_t size) {
    return rt_compute_crc32(data, size);
}

bool ChunkVerifier::verifyData(const uint8_t* data,
                               size_t size,
                               const std::string& expectedFingerprint,
                               uint32_t expectedCrc32,
                               std::string& error) {
    if (!isValidFingerprintFormat(expectedFingerprint)) {
        error = "Invalid expected fingerprint format: " + expectedFingerprint;
        return false;
    }

    std::string actualFp = computeFingerprint(data, size);
    if (actualFp != expectedFingerprint) {
        error = "Fingerprint mismatch: expected " + expectedFingerprint + ", got " + actualFp;
        return false;
    }

    uint32_t actualCrc = computeCrc32(data, size);
    if (actualCrc != expectedCrc32) {
        error = "CRC32 checksum mismatch: expected " + std::to_string(expectedCrc32) +
                ", got " + std::to_string(actualCrc);
        return false;
    }

    return true;
}

bool ChunkVerifier::verifyFile(const std::string& filePath,
                               const std::string& expectedFingerprint,
                               uint64_t expectedSize,
                               uint32_t expectedCrc32,
                               std::string& error) {
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
        error = "Chunk file does not exist: " + filePath;
        return false;
    }

    uintmax_t actualSize = fs::file_size(filePath, ec);
    if (ec) {
        error = "Could not get chunk file size: " + ec.message();
        return false;
    }

    if (actualSize != expectedSize) {
        error = "Chunk file size mismatch for " + filePath + ": expected " +
                std::to_string(expectedSize) + " bytes, found " + std::to_string(actualSize);
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        error = "Could not open chunk file: " + filePath;
        return false;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(actualSize));
    if (actualSize > 0) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(actualSize));
        if (file.gcount() != static_cast<std::streamsize>(actualSize)) {
            error = "Incomplete read of chunk file: " + filePath;
            return false;
        }
    }

    return verifyData(buffer.data(), buffer.size(), expectedFingerprint, expectedCrc32, error);
}

bool ChunkVerifier::compareDataWithFile(const std::string& filePath,
                                       const uint8_t* data,
                                       size_t size,
                                       std::string& error) {
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
        error = "File does not exist: " + filePath;
        return false;
    }

    uintmax_t actualSize = fs::file_size(filePath, ec);
    if (ec || actualSize != size) {
        error = "Size mismatch during collision comparison";
        return false;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        error = "Could not open file for collision comparison: " + filePath;
        return false;
    }

    std::vector<uint8_t> buffer(size);
    if (size > 0) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
        if (file.gcount() != static_cast<std::streamsize>(size)) {
            error = "Incomplete read during collision comparison: " + filePath;
            return false;
        }
    }

    if (std::memcmp(buffer.data(), data, size) != 0) {
        error = "Payload collision detected: byte contents differ despite matching fingerprint!";
        return false;
    }

    return true;
}

} // namespace retro::storage
