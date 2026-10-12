#include "chunk_engine.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <exception>
#include <iostream>
#include <unordered_set>

namespace fs = std::filesystem;

ChunkEngine::ChunkEngine(const EngineConfig& config)
    : config_(config) {
}

void ChunkEngine::collectFiles(const DirectoryNode& node, std::vector<FileInfo>& outFiles) const {
    for (const auto& file : node.files) {
        outFiles.push_back(file);
    }
    for (const auto& subDir : node.directories) {
        collectFiles(subDir, outFiles);
    }
}

static std::string normalizeRelativePath(const std::string& fullPath, const std::string& rootPath) {
    try {
        fs::path p(fullPath);
        fs::path base(rootPath);
        std::string rel = fs::relative(p, base).lexically_normal().generic_string();
        return rel.empty() ? p.filename().generic_string() : rel;
    } catch (...) {
        return fullPath;
    }
}

bool ChunkEngine::processFile(const FileInfo& file,
                              const std::string& relativePath,
                              FileChunkResult& outResult) const {
    outResult.fileInfo = file;
    outResult.relativePath = relativePath;
    outResult.decision = policy_.decide(file, config_.strategy, config_.fixedConfig, config_.cdcConfig);

    bool success = false;
    if (outResult.decision.selectedStrategy == ChunkStrategy::Fixed) {
        FixedChunker chunker(outResult.decision.fixedConfig);
        success = chunker.chunkFile(file.filePath, relativePath, outResult.chunks, outResult.metrics);
    } else {
        CDCChunker chunker(outResult.decision.cdcConfig, config_.ioBufferSize);
        success = chunker.chunkFile(file.filePath, relativePath, outResult.chunks, outResult.metrics);
    }

    outResult.metrics.decisionReason = outResult.decision.reason;
    return success;
}

ProjectChunkResult ChunkEngine::processScannedProject(const DirectoryNode& rootNode,
                                                     const std::string& rootPath) const {
    ProjectChunkResult result;
    auto projectStart = std::chrono::steady_clock::now();

    std::vector<FileInfo> allFiles;
    collectFiles(rootNode, allFiles);

    std::unordered_set<std::string> seenPaths;
    std::unordered_set<std::string> uniqueFingerprints;

    for (const auto& file : allFiles) {
        // Avoid processing identical paths twice
        if (!seenPaths.insert(file.filePath).second) {
            continue;
        }

        std::string relPath = normalizeRelativePath(file.filePath, rootPath);
        FileChunkResult fileRes;
        if (!processFile(file, relPath, fileRes)) {
            std::cerr << "Warning [ChunkEngine]: Skipping inaccessible file: " << file.filePath << "\n";
            continue;
        }

        result.metrics.totalFilesProcessed++;
        result.metrics.totalBytesProcessed += fileRes.metrics.totalChunkBytes;
        result.metrics.totalChunksGenerated += fileRes.chunks.size();
        result.metrics.totalChunkingTimeMs += fileRes.metrics.chunkingDurationMs;
        result.metrics.totalFingerprintingTimeMs += fileRes.metrics.fingerprintingDurationMs;
        result.metrics.totalEstimatedMetadataBytes += fileRes.metrics.estimatedMetadataBytes;

        for (const auto& chunk : fileRes.chunks) {
            if (uniqueFingerprints.find(chunk.fingerprint) != uniqueFingerprints.end()) {
                result.metrics.deduplicatedBytes += chunk.length;
            } else {
                uniqueFingerprints.insert(chunk.fingerprint);
            }
            result.allChunks.push_back(chunk);
        }

        result.files.push_back(std::move(fileRes));
    }

    auto projectEnd = std::chrono::steady_clock::now();
    double totalElapsedMs = std::chrono::duration<double, std::milli>(projectEnd - projectStart).count();

    result.metrics.uniqueFingerprints = uniqueFingerprints.size();
    result.metrics.totalExecutionTimeMs = totalElapsedMs;
    if (result.metrics.totalBytesProcessed > 0) {
        result.metrics.internalDedupRatio = static_cast<double>(result.metrics.deduplicatedBytes) / 
                                            static_cast<double>(result.metrics.totalBytesProcessed);
    }

    if (totalElapsedMs > 0.0) {
        result.metrics.overallThroughputMBps = 
            (static_cast<double>(result.metrics.totalBytesProcessed) / (1024.0 * 1024.0)) / 
            (totalElapsedMs / 1000.0);
    }

    return result;
}

bool ChunkEngine::readChunkPayload(const std::string& fullPath,
                                  uint64_t offset,
                                  uint64_t length,
                                  std::vector<uint8_t>& outData) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        length > kMaxChunkSizeBytes) {
        outData.clear();
        return false;
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) {
        outData.clear();
        return false;
    }

    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        outData.clear();
        return false;
    }

    try {
        outData.resize(static_cast<size_t>(length));
    } catch (const std::exception&) {
        outData.clear();
        return false;
    }

    if (length == 0) return true;
    file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(length));
    if (static_cast<uint64_t>(file.gcount()) != length || file.bad()) {
        outData.clear();
        return false;
    }
    return true;
}

VersionComparisonMetrics ChunkEngine::compareVersions(const std::vector<ChunkMetadata>& baseChunks,
                                                      const std::vector<ChunkMetadata>& newChunks) {
    VersionComparisonMetrics metrics;
    metrics.totalOriginalChunks = baseChunks.size();
    metrics.totalNewChunks = newChunks.size();

    std::unordered_set<std::string> baseFingerprints;
    for (const auto& chunk : baseChunks) {
        baseFingerprints.insert(chunk.fingerprint);
    }

    for (const auto& chunk : newChunks) {
        metrics.totalNewBytes += chunk.length;
        if (baseFingerprints.find(chunk.fingerprint) != baseFingerprints.end()) {
            metrics.matchingChunks++;
            metrics.reusableBytes += chunk.length;
        } else {
            metrics.newChunks++;
            metrics.transferredBytesNeeded += chunk.length;
        }
    }

    if (metrics.totalNewBytes > 0) {
        metrics.reuseRatio = static_cast<double>(metrics.reusableBytes) / static_cast<double>(metrics.totalNewBytes);
        metrics.deduplicationRatio = metrics.reuseRatio;
    }

    return metrics;
}
