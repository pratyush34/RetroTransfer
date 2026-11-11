#ifndef RETROTRANSFER_RECONSTRUCTION_ENGINE_H
#define RETROTRANSFER_RECONSTRUCTION_ENGINE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ReconstructionResult {
    size_t filesWritten = 0;
    size_t chunksWritten = 0;
    size_t chunksReused = 0;
};

struct ExtractedChunk {
    std::string fingerprint;
    uint64_t originalSize = 0;
    uint32_t crc32 = 0;
    std::string codec;
    std::vector<uint8_t> data;
};

class ReconstructionEngine {
public:
    /*
     * Reconstruct project files from manifest and payload.
     * If persistentStorageDir is provided, missing chunks will also be looked up
     * in the persistent chunk repository.
     */
    bool reconstruct(const std::string& manifestPath,
                     const std::string& payloadPath,
                     const std::string& outputDirectory,
                     ReconstructionResult& result,
                     std::string& error,
                     const std::string& persistentStorageDir = "") const;

    /*
     * Extracts and validates all payload chunks from a manifest and payload bundle.
     * Reuses the existing Huffman decoder and MurmurHash3 / CRC32 integrity verification.
     */
    static bool extractPayloadChunks(const std::string& manifestPath,
                                     const std::string& payloadPath,
                                     std::vector<ExtractedChunk>& outChunks,
                                     std::string& error);
};

#endif // RETROTRANSFER_RECONSTRUCTION_ENGINE_H
