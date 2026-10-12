#ifndef FIXED_CHUNKER_H
#define FIXED_CHUNKER_H

#include "chunk_config.h"
#include "chunk_metadata.h"
#include "metrics.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Fixed-size chunking engine implementation.
 * 
 * Splits incoming binary streams into uniform-sized chunks of exactly chunkSize bytes,
 * with the last chunk accommodating any remaining partial bytes.
 * Handles empty files, single-chunk files, and exact multiples gracefully.
 */
class FixedChunker {
public:
    explicit FixedChunker(const FixedChunkConfig& config = FixedChunkConfig{});

    /**
     * @brief Chunks a file from disk incrementally using binary streaming.
     * 
     * @param fullPath Absolute or valid relative filesystem path to the file.
     * @param relativePath Relative path identifier to record in chunk metadata.
     * @param outChunks Output vector where generated chunk metadata will be appended.
     * @param outMetrics Output structure recording performance and execution metrics.
     * @return true on success, false if file could not be opened or read.
     */
    bool chunkFile(const std::string& fullPath,
                   const std::string& relativePath,
                   std::vector<ChunkMetadata>& outChunks,
                   FileChunkMetrics& outMetrics) const;

    /**
     * @brief Chunks an in-memory byte buffer (useful for testing and memory streams).
     */
    bool chunkBuffer(const uint8_t* buffer,
                     size_t length,
                     const std::string& relativePath,
                     std::vector<ChunkMetadata>& outChunks,
                     FileChunkMetrics& outMetrics) const;

    const FixedChunkConfig& getConfig() const { return config_; }
    void setConfig(const FixedChunkConfig& config) { config_ = config; }

private:
    FixedChunkConfig config_;
};

#endif // FIXED_CHUNKER_H
