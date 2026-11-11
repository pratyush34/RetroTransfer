#ifndef RETROTRANSFER_STORAGE_CHUNK_VERIFIER_H
#define RETROTRANSFER_STORAGE_CHUNK_VERIFIER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retro::storage {

/**
 * @brief Utility class for validating chunk fingerprints, checksums, and stored file integrity.
 */
class ChunkVerifier {
public:
    static constexpr uint32_t DEFAULT_SEED = 42;

    static bool isValidFingerprintFormat(const std::string& fingerprint);

    static std::string computeFingerprint(const uint8_t* data, size_t size, uint32_t seed = DEFAULT_SEED);

    static uint32_t computeCrc32(const uint8_t* data, size_t size);

    static bool verifyData(const uint8_t* data,
                           size_t size,
                           const std::string& expectedFingerprint,
                           uint32_t expectedCrc32,
                           std::string& error);

    static bool verifyFile(const std::string& filePath,
                           const std::string& expectedFingerprint,
                           uint64_t expectedSize,
                           uint32_t expectedCrc32,
                           std::string& error);

    static bool compareDataWithFile(const std::string& filePath,
                                   const uint8_t* data,
                                   size_t size,
                                   std::string& error);
};

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_CHUNK_VERIFIER_H
