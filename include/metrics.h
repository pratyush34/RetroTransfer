#ifndef METRICS_H
#define METRICS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Performance and operational metrics collected for an individual file.
 */
struct FileChunkMetrics {
    std::string relativePath;
    uint64_t fileSize = 0;
    size_t chunkCount = 0;
    uint64_t totalChunkBytes = 0;

    std::string selectedStrategy;
    std::string decisionReason;
    std::string chunkConfigDesc;

    double chunkingDurationMs = 0.0;
    double fingerprintingDurationMs = 0.0;
    double totalDurationMs = 0.0;

    size_t minChunkSize = 0;
    size_t maxChunkSize = 0;
    double avgChunkSize = 0.0;

    uint64_t estimatedMetadataBytes = 0;
    double fingerprintThroughputMBps = 0.0;
    double totalThroughputMBps = 0.0;
};

/**
 * @brief Aggregated metrics for an entire project or scan run.
 */
struct ProjectChunkMetrics {
    size_t totalFilesProcessed = 0;
    uint64_t totalBytesProcessed = 0;
    size_t totalChunksGenerated = 0;
    size_t uniqueFingerprints = 0;

    double totalChunkingTimeMs = 0.0;
    double totalFingerprintingTimeMs = 0.0;
    double totalExecutionTimeMs = 0.0;

    uint64_t totalEstimatedMetadataBytes = 0;
    double overallThroughputMBps = 0.0;

    // Deduplication baseline within the current project
    uint64_t deduplicatedBytes = 0;
    double internalDedupRatio = 0.0; // deduplicatedBytes / totalBytesProcessed
};

/**
 * @brief Version comparison metrics when evaluating two revisions/transfers.
 */
struct VersionComparisonMetrics {
    size_t totalOriginalChunks = 0;
    size_t totalNewChunks = 0;
    size_t matchingChunks = 0;
    size_t newChunks = 0;

    uint64_t totalNewBytes = 0;
    uint64_t reusableBytes = 0;
    uint64_t transferredBytesNeeded = 0;

    double reuseRatio = 0.0;      // reusableBytes / totalNewBytes
    double deduplicationRatio = 0.0; // 1.0 - (transferredBytesNeeded / totalNewBytes)
};

#endif // METRICS_H
