#ifndef COMPRESSION_ENGINE_H
#define COMPRESSION_ENGINE_H

#include "chunk_engine.h"

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

struct CompressedChunkInfo {
    ChunkMetadata metadata;
    uint64_t payloadOffset = 0;
    uint64_t storedSize = 0;
    std::string codec;
};

struct CompressionMetrics {
    size_t totalChunks = 0;
    size_t reusableChunks = 0;
    size_t missingChunks = 0;
    size_t duplicateChunks = 0;
    size_t huffmanChunks = 0;
    size_t storedRawChunks = 0;
    uint64_t reusableBytes = 0;
    uint64_t duplicateBytesAvoided = 0;
    uint64_t totalSourceBytes = 0;
    uint64_t selectedInputBytes = 0;
    uint64_t uniqueInputBytes = 0;
    uint64_t payloadBytes = 0;
    uint64_t bundleHeaderBytes = 8;
    uint64_t compressionBytesSaved = 0;
    uint64_t totalBytesAvoided = 0;
};

struct CompressionResult {
    std::vector<CompressedChunkInfo> payloadChunks;
    CompressionMetrics metrics;
};

class CompressionEngine {
public:
    bool compressMissingChunks(const ProjectChunkResult& current,
                               const ProjectChunkResult& baseline,
                               const std::string& payloadPath,
                               CompressionResult& result,
                               std::string& error) const;

    /*
     * Compress only the chunks requested by missingFingerprints.
     * Reuses existing Huffman compression and raw-data fallback.
     */
    bool compressSelectedMissingChunks(const ProjectChunkResult& current,
                                       const std::unordered_set<std::string>& missingFingerprints,
                                       const std::string& payloadPath,
                                       CompressionResult& result,
                                       std::string& error) const;

    /*
     * Create a selective .rtpack payload from an existing manifest and/or existing payload,
     * packaging only the specified missing chunk fingerprints.
     */
    bool createSelectivePayloadFromExisting(const std::string& sourceManifestPath,
                                            const std::string& sourcePayloadPath,
                                            const std::unordered_set<std::string>& missingFingerprints,
                                            const std::string& outPayloadPath,
                                            CompressionResult& result,
                                            std::string& error) const;
};

#endif
