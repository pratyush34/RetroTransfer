#ifndef CHUNK_ENGINE_H
#define CHUNK_ENGINE_H

#include "scanner.h"
#include "chunk_config.h"
#include "chunk_metadata.h"
#include "metrics.h"
#include "fixed_chunker.h"
#include "cdc_chunker.h"
#include "adaptive_policy.h"

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

/**
 * @brief Output container for a single file's chunking results.
 */
struct FileChunkResult {
    FileInfo fileInfo;
    std::string relativePath;
    PolicyDecision decision;
    FileChunkMetrics metrics;
    std::vector<ChunkMetadata> chunks;
};

/**
 * @brief Output container for project-wide chunking results.
 */
struct ProjectChunkResult {
    std::vector<FileChunkResult> files;
    std::vector<ChunkMetadata> allChunks;
    ProjectChunkMetrics metrics;
};

/**
 * @brief Chunking Engine coordinating scanning tree traversal, strategy selection,
 * chunking execution, and metrics collection.
 */
class ChunkEngine {
public:
    explicit ChunkEngine(const EngineConfig& config = EngineConfig{});

    /**
     * @brief Traverses a scanned DirectoryNode tree and processes all eligible files.
     * 
     * @param rootNode The DirectoryNode returned by ProjectScanner::scan().
     * @param rootPath The root directory path used during scanning.
     * @return ProjectChunkResult containing per-file results and aggregate metrics.
     */
    ProjectChunkResult processScannedProject(const DirectoryNode& rootNode,
                                             const std::string& rootPath) const;

    /**
     * @brief Processes an individual file directly.
     */
    bool processFile(const FileInfo& info,
                     const std::string& relativePath,
                     FileChunkResult& outResult) const;

    /**
     * @brief Rereads raw chunk bytes from disk given path, offset, and length.
     * 
     * Keeps chunking memory footprint minimal while allowing downstream
     * deduplication, compression, and socket transfer to load chunks on demand.
     */
    static bool readChunkPayload(const std::string& fullPath,
                                 uint64_t offset,
                                 uint64_t length,
                                 std::vector<uint8_t>& outData);

    /**
     * @brief Compares two sets of chunk metadata (e.g. baseline project vs modified project)
     * to evaluate chunk reuse and incremental transfer efficiency.
     */
    static VersionComparisonMetrics compareVersions(const std::vector<ChunkMetadata>& baseChunks,
                                                     const std::vector<ChunkMetadata>& newChunks);

    const EngineConfig& getConfig() const { return config_; }
    void setConfig(const EngineConfig& config) { config_ = config; }

private:
    EngineConfig config_;
    AdaptivePolicy policy_;

    void collectFiles(const DirectoryNode& node, std::vector<FileInfo>& outFiles) const;
};

#endif // CHUNK_ENGINE_H
