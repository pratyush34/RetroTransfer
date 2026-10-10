#ifndef CDC_CHUNKER_H
#define CDC_CHUNKER_H

#include "chunk_config.h"
#include "chunk_metadata.h"
#include "metrics.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Content-Defined Chunking (CDC) engine using Gear rolling hash.
 * 
 * Algorithm Description:
 * Gear-based Content-Defined Chunking determines chunk boundaries based on data content
 * rather than fixed byte positions. It uses a 256-entry 64-bit random lookup table (Gear matrix).
 * For each byte b in the input stream, the rolling hash state is updated via:
 * 
 *     rolling_hash = (rolling_hash << 1) + GEAR_MATRIX[b]
 * 
 * Boundary Conditions:
 * 1. Minimum chunk enforcement: No boundary is triggered until current chunk size >= minChunkSize.
 * 2. Content boundary: When size >= minChunkSize, a boundary is declared if:
 *         (rolling_hash & mask) == 0
 *    The mask is configured to yield an expected average chunk size of targetChunkSize.
 * 3. Maximum chunk enforcement: If size reaches maxChunkSize without a content boundary,
 *    a hard boundary is forced to cap chunk size.
 * 4. Stream termination: Any remaining bytes at EOF form the final partial chunk.
 * 
 * This design is deterministic, robust against byte-shift insertions/deletions,
 * and processes files incrementally with memory bounded by maxChunkSize.
 */
class CDCChunker {
public:
    explicit CDCChunker(const CDCConfig& config = CDCConfig{});

    /**
     * @brief Chunks a file from disk incrementally.
     * 
     * @param fullPath Path to the input file.
     * @param relativePath Relative identifier recorded in metadata.
     * @param outChunks Output vector where metadata for generated chunks is appended.
     * @param outMetrics Output performance and diagnostic metrics.
     * @return true on success, false if file could not be opened or read.
     */
    bool chunkFile(const std::string& fullPath,
                   const std::string& relativePath,
                   std::vector<ChunkMetadata>& outChunks,
                   FileChunkMetrics& outMetrics) const;

    /**
     * @brief Chunks an in-memory byte buffer.
     */
    bool chunkBuffer(const uint8_t* buffer,
                     size_t length,
                     const std::string& relativePath,
                     std::vector<ChunkMetadata>& outChunks,
                     FileChunkMetrics& outMetrics) const;

    const CDCConfig& getConfig() const { return config_; }
    void setConfig(const CDCConfig& config) { config_ = config; }

private:
    CDCConfig config_;
};

#endif // CDC_CHUNKER_H
