#ifndef CHUNK_CONFIG_H
#define CHUNK_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Supported chunking strategies in RetroTransfer.
 */
enum class ChunkStrategy {
    Fixed,      // Fixed-size chunking (baseline)
    CDC,        // Content-Defined Chunking (Gear rolling hash)
    Adaptive    // Workload-aware adaptive selection per file
};

inline std::string strategy_to_string(ChunkStrategy strategy) {
    switch (strategy) {
        case ChunkStrategy::Fixed:    return "fixed";
        case ChunkStrategy::CDC:      return "cdc";
        case ChunkStrategy::Adaptive: return "adaptive";
    }
    return "unknown";
}

inline bool string_to_strategy(const std::string& str, ChunkStrategy& outStrategy) {
    if (str == "fixed" || str == "FIXED") {
        outStrategy = ChunkStrategy::Fixed;
        return true;
    }
    if (str == "cdc" || str == "CDC") {
        outStrategy = ChunkStrategy::CDC;
        return true;
    }
    if (str == "adaptive" || str == "ADAPTIVE") {
        outStrategy = ChunkStrategy::Adaptive;
        return true;
    }
    return false;
}

/**
 * @brief Configuration parameters for Fixed-size chunking.
 */
struct FixedChunkConfig {
    size_t chunkSize = 64 * 1024; // Default: 64 KiB

    bool isValid() const {
        return chunkSize > 0;
    }
};

/**
 * @brief Configuration parameters for Content-Defined Chunking (CDC).
 */
struct CDCConfig {
    size_t minChunkSize = 16 * 1024;  // Default: 16 KiB
    size_t targetChunkSize = 64 * 1024; // Default: 64 KiB
    size_t maxChunkSize = 256 * 1024; // Default: 256 KiB
    uint64_t mask = 0;

    CDCConfig() {
        update_mask();
    }

    CDCConfig(size_t minSz, size_t targetSz, size_t maxSz)
        : minChunkSize(minSz), targetChunkSize(targetSz), maxChunkSize(maxSz) {
        update_mask();
    }

    /**
     * @brief Computes mask such that expected boundary frequency is 1/targetChunkSize.
     * For average chunk size 2^k, mask has k bits set (2^k - 1).
     */
    void update_mask() {
        if (targetChunkSize == 0) {
            mask = 0;
            return;
        }
        // Find power of 2 closest to targetChunkSize
        size_t bits = 0;
        size_t val = targetChunkSize;
        while (val > 1) {
            val >>= 1;
            bits++;
        }
        if (bits > 32) bits = 32;
        if (bits < 4) bits = 4;
        mask = (1ULL << bits) - 1ULL;
    }

    bool isValid() const {
        return minChunkSize > 0 &&
               minChunkSize <= targetChunkSize &&
               targetChunkSize <= maxChunkSize;
    }
};

/**
 * @brief Overall engine configuration.
 */
struct EngineConfig {
    ChunkStrategy strategy = ChunkStrategy::Adaptive;
    FixedChunkConfig fixedConfig;
    CDCConfig cdcConfig;
    size_t ioBufferSize = 64 * 1024; // 64 KiB stream buffer
};

#endif // CHUNK_CONFIG_H
