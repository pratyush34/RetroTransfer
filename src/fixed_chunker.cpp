#include "fixed_chunker.h"
#include "fingerprint.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <exception>

FixedChunker::FixedChunker(const FixedChunkConfig& config)
    : config_(config) {
}

bool FixedChunker::chunkFile(const std::string& fullPath,
                             const std::string& relativePath,
                             std::vector<ChunkMetadata>& outChunks,
                             FileChunkMetrics& outMetrics) const {
    if (!config_.isValid()) {
        std::cerr << "Error [FixedChunker]: Invalid chunk size " << config_.chunkSize << "\n";
        return false;
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error [FixedChunker]: Could not open file " << fullPath << "\n";
        return false;
    }

    auto totalStart = std::chrono::steady_clock::now();
    double totalFpDurationMs = 0.0;

    std::vector<uint8_t> buffer;
    try {
        buffer.resize(config_.chunkSize);
    } catch (const std::exception& ex) {
        std::cerr << "Error [FixedChunker]: Could not allocate chunk buffer: " << ex.what() << "\n";
        return false;
    }
    uint64_t currentOffset = 0;
    uint32_t sequence = 0;
    size_t minChunk = 0;
    size_t maxChunk = 0;
    uint64_t totalBytes = 0;
    size_t chunkCountBefore = outChunks.size();

    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(config_.chunkSize));
        std::streamsize bytesRead = file.gcount();
        if (bytesRead <= 0) {
            break;
        }

        const size_t chunkSizeActual = static_cast<size_t>(bytesRead);
        totalBytes += chunkSizeActual;

        if (outChunks.size() == chunkCountBefore) {
            minChunk = chunkSizeActual;
            maxChunk = chunkSizeActual;
        } else {
            minChunk = std::min(minChunk, chunkSizeActual);
            maxChunk = std::max(maxChunk, chunkSizeActual);
        }

        // Fingerprint timing
        auto fpStart = std::chrono::steady_clock::now();
        std::string fp = compute_fingerprint_hex(buffer.data(), chunkSizeActual);
        uint32_t crc = compute_crc32(buffer.data(), chunkSizeActual);
        auto fpEnd = std::chrono::steady_clock::now();
        totalFpDurationMs += std::chrono::duration<double, std::milli>(fpEnd - fpStart).count();

        ChunkMetadata chunk;
        chunk.relativePath = relativePath;
        chunk.offset = currentOffset;
        chunk.length = chunkSizeActual;
        chunk.fingerprint = std::move(fp);
        chunk.algorithm = "fixed";
        chunk.sequenceNumber = sequence++;
        chunk.crc32 = crc;

        currentOffset += chunkSizeActual;
        outChunks.push_back(std::move(chunk));
    }

    if (file.bad()) {
        outChunks.resize(chunkCountBefore);
        outMetrics = FileChunkMetrics{};
        std::cerr << "Error [FixedChunker]: Read failure for file " << fullPath << "\n";
        return false;
    }

    auto totalEnd = std::chrono::steady_clock::now();
    double totalDurationMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    size_t chunksGenerated = outChunks.size() - chunkCountBefore;

    // Fill metrics
    outMetrics.relativePath = relativePath;
    outMetrics.fileSize = totalBytes;
    outMetrics.chunkCount = chunksGenerated;
    outMetrics.totalChunkBytes = totalBytes;
    outMetrics.selectedStrategy = "fixed";
    outMetrics.chunkConfigDesc = "FixedSize=" + std::to_string(config_.chunkSize);
    outMetrics.chunkingDurationMs = totalDurationMs - totalFpDurationMs;
    if (outMetrics.chunkingDurationMs < 0.0) outMetrics.chunkingDurationMs = 0.0;
    outMetrics.fingerprintingDurationMs = totalFpDurationMs;
    outMetrics.totalDurationMs = totalDurationMs;
    outMetrics.minChunkSize = chunksGenerated > 0 ? minChunk : 0;
    outMetrics.maxChunkSize = chunksGenerated > 0 ? maxChunk : 0;
    outMetrics.avgChunkSize = chunksGenerated > 0 ? static_cast<double>(totalBytes) / static_cast<double>(chunksGenerated) : 0.0;

    // Estimated metadata overhead: approx 128 bytes per chunk record
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

bool FixedChunker::chunkBuffer(const uint8_t* buffer,
                               size_t length,
                               const std::string& relativePath,
                               std::vector<ChunkMetadata>& outChunks,
                               FileChunkMetrics& outMetrics) const {
    if (!config_.isValid()) {
        std::cerr << "Error [FixedChunker]: Invalid chunk size " << config_.chunkSize << "\n";
        return false;
    }
    if (length > 0 && buffer == nullptr) {
        std::cerr << "Error [FixedChunker]: Null input buffer for non-empty data\n";
        return false;
    }

    auto totalStart = std::chrono::steady_clock::now();
    double totalFpDurationMs = 0.0;

    uint64_t currentOffset = 0;
    uint32_t sequence = 0;
    size_t minChunk = 0;
    size_t maxChunk = 0;
    size_t chunkCountBefore = outChunks.size();

    while (currentOffset < length) {
        size_t bytesToTake = std::min(config_.chunkSize, length - static_cast<size_t>(currentOffset));

        if (outChunks.size() == chunkCountBefore) {
            minChunk = bytesToTake;
            maxChunk = bytesToTake;
        } else {
            minChunk = std::min(minChunk, bytesToTake);
            maxChunk = std::max(maxChunk, bytesToTake);
        }

        const uint8_t* chunkPtr = buffer + currentOffset;

        auto fpStart = std::chrono::steady_clock::now();
        std::string fp = compute_fingerprint_hex(chunkPtr, bytesToTake);
        uint32_t crc = compute_crc32(chunkPtr, bytesToTake);
        auto fpEnd = std::chrono::steady_clock::now();
        totalFpDurationMs += std::chrono::duration<double, std::milli>(fpEnd - fpStart).count();

        ChunkMetadata chunk;
        chunk.relativePath = relativePath;
        chunk.offset = currentOffset;
        chunk.length = bytesToTake;
        chunk.fingerprint = std::move(fp);
        chunk.algorithm = "fixed";
        chunk.sequenceNumber = sequence++;
        chunk.crc32 = crc;

        currentOffset += bytesToTake;
        outChunks.push_back(std::move(chunk));
    }

    auto totalEnd = std::chrono::steady_clock::now();
    double totalDurationMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    size_t chunksGenerated = outChunks.size() - chunkCountBefore;

    outMetrics.relativePath = relativePath;
    outMetrics.fileSize = length;
    outMetrics.chunkCount = chunksGenerated;
    outMetrics.totalChunkBytes = length;
    outMetrics.selectedStrategy = "fixed";
    outMetrics.chunkConfigDesc = "FixedSize=" + std::to_string(config_.chunkSize);
    outMetrics.chunkingDurationMs = totalDurationMs - totalFpDurationMs;
    if (outMetrics.chunkingDurationMs < 0.0) outMetrics.chunkingDurationMs = 0.0;
    outMetrics.fingerprintingDurationMs = totalFpDurationMs;
    outMetrics.totalDurationMs = totalDurationMs;
    outMetrics.minChunkSize = chunksGenerated > 0 ? minChunk : 0;
    outMetrics.maxChunkSize = chunksGenerated > 0 ? maxChunk : 0;
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
