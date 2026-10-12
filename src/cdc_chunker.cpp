#include "cdc_chunker.h"
#include "fingerprint.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <chrono>
#include <fstream>
#include <iostream>

// A fixed, well-dispersed Gear table. SplitMix64 makes the values deterministic
// across platforms while avoiding the original table's critical flaw: every entry
// was odd, which made (rollingHash & mask) == 0 impossible.
static const std::array<uint64_t, 256> GEAR_MATRIX = [] {
    std::array<uint64_t, 256> table{};
    uint64_t state = 0x243f6a8885a308d3ULL;
    for (uint64_t& value : table) {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        value = z ^ (z >> 31);
    }
    return table;
}();

CDCChunker::CDCChunker(const CDCConfig& config, size_t ioBufferSize)
    : config_(config), ioBufferSize_(ioBufferSize) {
    // The mask is derived from targetChunkSize; recompute it even if callers
    // modified a public config field after construction.
    config_.update_mask();
}

bool CDCChunker::chunkFile(const std::string& fullPath,
                           const std::string& relativePath,
                           std::vector<ChunkMetadata>& outChunks,
                           FileChunkMetrics& outMetrics) const {
    if (!config_.isValid()) {
        std::cerr << "Error [CDCChunker]: Invalid CDC configuration: min=" 
                  << config_.minChunkSize << " target=" << config_.targetChunkSize 
                  << " max=" << config_.maxChunkSize << "\n";
        return false;
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error [CDCChunker]: Could not open file " << fullPath << "\n";
        return false;
    }

    if (ioBufferSize_ == 0 || ioBufferSize_ > kMaxChunkSizeBytes ||
        ioBufferSize_ > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        std::cerr << "Error [CDCChunker]: Invalid I/O buffer size " << ioBufferSize_ << "\n";
        return false;
    }

    auto totalStart = std::chrono::steady_clock::now();
    double totalFpDurationMs = 0.0;

    std::vector<uint8_t> ioBuffer;
    try {
        ioBuffer.resize(ioBufferSize_);
    } catch (const std::exception& ex) {
        std::cerr << "Error [CDCChunker]: Could not allocate I/O buffer: " << ex.what() << "\n";
        return false;
    }

    // Buffer storing currently accumulated chunk bytes (capped by maxChunkSize)
    std::vector<uint8_t> currentChunk;
    try {
        currentChunk.reserve(config_.maxChunkSize);
    } catch (const std::exception& ex) {
        std::cerr << "Error [CDCChunker]: Could not allocate chunk buffer: " << ex.what() << "\n";
        return false;
    }

    uint64_t chunkStartOffset = 0;
    uint64_t rollingHash = 0;
    uint32_t sequence = 0;
    size_t minObserved = 0;
    size_t maxObserved = 0;
    uint64_t totalBytes = 0;
    size_t chunkCountBefore = outChunks.size();

    auto emitChunk = [&](const uint8_t* chunkData, size_t chunkSize, uint64_t offset) {
        if (chunkSize == 0) return;

        if (outChunks.size() == chunkCountBefore) {
            minObserved = chunkSize;
            maxObserved = chunkSize;
        } else {
            minObserved = std::min(minObserved, chunkSize);
            maxObserved = std::max(maxObserved, chunkSize);
        }
        totalBytes += chunkSize;

        auto fpStart = std::chrono::steady_clock::now();
        std::string fp = compute_fingerprint_hex(chunkData, chunkSize);
        uint32_t crc = compute_crc32(chunkData, chunkSize);
        auto fpEnd = std::chrono::steady_clock::now();
        totalFpDurationMs += std::chrono::duration<double, std::milli>(fpEnd - fpStart).count();

        ChunkMetadata chunk;
        chunk.relativePath = relativePath;
        chunk.offset = offset;
        chunk.length = chunkSize;
        chunk.fingerprint = std::move(fp);
        chunk.algorithm = "cdc";
        chunk.sequenceNumber = sequence++;
        chunk.crc32 = crc;
        outChunks.push_back(std::move(chunk));
    };

    while (file) {
        file.read(reinterpret_cast<char*>(ioBuffer.data()), static_cast<std::streamsize>(ioBufferSize_));
        std::streamsize bytesRead = file.gcount();
        if (bytesRead <= 0) {
            break;
        }

        for (std::streamsize i = 0; i < bytesRead; ++i) {
            uint8_t byte = ioBuffer[static_cast<size_t>(i)];
            currentChunk.push_back(byte);

            // Update Gear rolling hash: shift left 1 bit and add lookup value
            rollingHash = (rollingHash << 1) + GEAR_MATRIX[byte];

            size_t currentLen = currentChunk.size();
            if (currentLen >= config_.minChunkSize) {
                // Boundary condition: hash matches mask OR forced cut at maxChunkSize
                if ((rollingHash & config_.mask) == 0 || currentLen >= config_.maxChunkSize) {
                    emitChunk(currentChunk.data(), currentLen, chunkStartOffset);
                    chunkStartOffset += currentLen;
                    currentChunk.clear();
                    rollingHash = 0;
                }
            }
        }
    }

    if (file.bad()) {
        outChunks.resize(chunkCountBefore);
        outMetrics = FileChunkMetrics{};
        std::cerr << "Error [CDCChunker]: Read failure for file " << fullPath << "\n";
        return false;
    }

    // Emit final partial chunk if non-empty
    if (!currentChunk.empty()) {
        emitChunk(currentChunk.data(), currentChunk.size(), chunkStartOffset);
        currentChunk.clear();
    }

    auto totalEnd = std::chrono::steady_clock::now();
    double totalDurationMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    size_t chunksGenerated = outChunks.size() - chunkCountBefore;

    outMetrics.relativePath = relativePath;
    outMetrics.fileSize = totalBytes;
    outMetrics.chunkCount = chunksGenerated;
    outMetrics.totalChunkBytes = totalBytes;
    outMetrics.selectedStrategy = "cdc";
    outMetrics.chunkConfigDesc = "CDC(min=" + std::to_string(config_.minChunkSize) +
                                ",target=" + std::to_string(config_.targetChunkSize) +
                                ",max=" + std::to_string(config_.maxChunkSize) + ")";
    outMetrics.chunkingDurationMs = totalDurationMs - totalFpDurationMs;
    if (outMetrics.chunkingDurationMs < 0.0) outMetrics.chunkingDurationMs = 0.0;
    outMetrics.fingerprintingDurationMs = totalFpDurationMs;
    outMetrics.totalDurationMs = totalDurationMs;
    outMetrics.minChunkSize = chunksGenerated > 0 ? minObserved : 0;
    outMetrics.maxChunkSize = chunksGenerated > 0 ? maxObserved : 0;
    outMetrics.avgChunkSize = chunksGenerated > 0 ? static_cast<double>(totalBytes) / static_cast<double>(chunksGenerated) : 0.0;
    outMetrics.estimatedMetadataBytes = chunksGenerated * (sizeof(ChunkMetadata) + relativePath.size() + 32);

    if (totalFpDurationMs > 0.0) {
        outMetrics.fingerprintThroughputMBps = (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (totalFpDurationMs / 1000.0);
    } else {
        outMetrics.fingerprintThroughputMBps = 0.0;
    }
    if (totalDurationMs > 0.0) {
        outMetrics.totalThroughputMBps = (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (totalDurationMs / 1000.0);
    } else {
        outMetrics.totalThroughputMBps = 0.0;
    }

    return true;
}

bool CDCChunker::chunkBuffer(const uint8_t* buffer,
                             size_t length,
                             const std::string& relativePath,
                             std::vector<ChunkMetadata>& outChunks,
                             FileChunkMetrics& outMetrics) const {
    if (!config_.isValid()) {
        std::cerr << "Error [CDCChunker]: Invalid CDC configuration\n";
        return false;
    }
    if (length > 0 && buffer == nullptr) {
        std::cerr << "Error [CDCChunker]: Null input buffer for non-empty data\n";
        return false;
    }

    auto totalStart = std::chrono::steady_clock::now();
    double totalFpDurationMs = 0.0;

    uint64_t chunkStartOffset = 0;
    uint64_t rollingHash = 0;
    uint32_t sequence = 0;
    size_t minObserved = 0;
    size_t maxObserved = 0;
    size_t chunkCountBefore = outChunks.size();

    auto emitChunk = [&](const uint8_t* chunkData, size_t chunkSize, uint64_t offset) {
        if (chunkSize == 0) return;

        if (outChunks.size() == chunkCountBefore) {
            minObserved = chunkSize;
            maxObserved = chunkSize;
        } else {
            minObserved = std::min(minObserved, chunkSize);
            maxObserved = std::max(maxObserved, chunkSize);
        }

        auto fpStart = std::chrono::steady_clock::now();
        std::string fp = compute_fingerprint_hex(chunkData, chunkSize);
        uint32_t crc = compute_crc32(chunkData, chunkSize);
        auto fpEnd = std::chrono::steady_clock::now();
        totalFpDurationMs += std::chrono::duration<double, std::milli>(fpEnd - fpStart).count();

        ChunkMetadata chunk;
        chunk.relativePath = relativePath;
        chunk.offset = offset;
        chunk.length = chunkSize;
        chunk.fingerprint = std::move(fp);
        chunk.algorithm = "cdc";
        chunk.sequenceNumber = sequence++;
        chunk.crc32 = crc;
        outChunks.push_back(std::move(chunk));
    };

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = buffer[i];
        rollingHash = (rollingHash << 1) + GEAR_MATRIX[byte];

        size_t currentLen = (i - chunkStartOffset) + 1;
        if (currentLen >= config_.minChunkSize) {
            if ((rollingHash & config_.mask) == 0 || currentLen >= config_.maxChunkSize) {
                emitChunk(buffer + chunkStartOffset, currentLen, chunkStartOffset);
                chunkStartOffset += currentLen;
                rollingHash = 0;
            }
        }
    }

    if (chunkStartOffset < length) {
        size_t remaining = length - chunkStartOffset;
        emitChunk(buffer + chunkStartOffset, remaining, chunkStartOffset);
    }

    auto totalEnd = std::chrono::steady_clock::now();
    double totalDurationMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    size_t chunksGenerated = outChunks.size() - chunkCountBefore;

    outMetrics.relativePath = relativePath;
    outMetrics.fileSize = length;
    outMetrics.chunkCount = chunksGenerated;
    outMetrics.totalChunkBytes = length;
    outMetrics.selectedStrategy = "cdc";
    outMetrics.chunkConfigDesc = "CDC(min=" + std::to_string(config_.minChunkSize) +
                                ",target=" + std::to_string(config_.targetChunkSize) +
                                ",max=" + std::to_string(config_.maxChunkSize) + ")";
    outMetrics.chunkingDurationMs = totalDurationMs - totalFpDurationMs;
    if (outMetrics.chunkingDurationMs < 0.0) outMetrics.chunkingDurationMs = 0.0;
    outMetrics.fingerprintingDurationMs = totalFpDurationMs;
    outMetrics.totalDurationMs = totalDurationMs;
    outMetrics.minChunkSize = chunksGenerated > 0 ? minObserved : 0;
    outMetrics.maxChunkSize = chunksGenerated > 0 ? maxObserved : 0;
    outMetrics.avgChunkSize = chunksGenerated > 0 ? static_cast<double>(length) / static_cast<double>(chunksGenerated) : 0.0;
    outMetrics.estimatedMetadataBytes = chunksGenerated * (sizeof(ChunkMetadata) + relativePath.size() + 32);

    if (totalFpDurationMs > 0.0) {
        outMetrics.fingerprintThroughputMBps = (static_cast<double>(length) / (1024.0 * 1024.0)) / (totalFpDurationMs / 1000.0);
    } else {
        outMetrics.fingerprintThroughputMBps = 0.0;
    }
    if (totalDurationMs > 0.0) {
        outMetrics.totalThroughputMBps = (static_cast<double>(length) / (1024.0 * 1024.0)) / (totalDurationMs / 1000.0);
    } else {
        outMetrics.totalThroughputMBps = 0.0;
    }

    return true;
}
