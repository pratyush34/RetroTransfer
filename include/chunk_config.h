#ifndef CHUNK_CONFIG_H
#define CHUNK_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Supported chunking strategies in RetroTransfer.
 */
// Chunk payloads are held in memory for hashing. A hard upper bound prevents
// accidental or malformed settings from requesting unbounded allocations.
inline constexpr uint64_t kMaxChunkSizeBytes = 1024ULL * 1024ULL * 1024ULL;

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
    std::string normalized = str;
    for (char& character : normalized) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    if (normalized == "fixed") {
        outStrategy = ChunkStrategy::Fixed;
        return true;
    }
    if (normalized == "cdc") {
        outStrategy = ChunkStrategy::CDC;
        return true;
    }
    if (normalized == "adaptive") {
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
        return chunkSize > 0 && chunkSize <= kMaxChunkSizeBytes;
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
        // Choose the nearest power of two, capped at 2^32. The mask has k
        // low bits set, giving a nominal boundary probability of 1 / 2^k.
        size_t bits = 0;
        size_t value = targetChunkSize;
        while (value > 1 && bits < 32) {
            value >>= 1;
            ++bits;
        }
        if (bits < 32) {
            const uint64_t lower = 1ULL << bits;
            const uint64_t upper = lower << 1;
            const uint64_t target = static_cast<uint64_t>(targetChunkSize);
            if (target - lower > upper - target) {
                ++bits;
            }
        }
        if (bits < 4) bits = 4;
        if (bits > 32) bits = 32;
        mask = (1ULL << bits) - 1ULL;
    }

    bool isValid() const {
        return minChunkSize > 0 &&
               minChunkSize <= targetChunkSize &&
               targetChunkSize <= maxChunkSize &&
               maxChunkSize <= kMaxChunkSizeBytes;
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
