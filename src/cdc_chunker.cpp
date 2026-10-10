#include "cdc_chunker.h"
#include "fingerprint.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

// 256 64-bit pseudo-random values for Gear rolling hash (FastCDC standard table)
static const uint64_t GEAR_MATRIX[256] = {
    0x000001000000000bULL, 0x000002000000001bULL, 0x0000030000000023ULL, 0x0000040000000037ULL,
    0x000005000000004fULL, 0x000006000000005dULL, 0x0000070000000065ULL, 0x0000080000000079ULL,
    0x000009000000008bULL, 0x00000a000000009dULL, 0x00000b00000000a7ULL, 0x00000c00000000b3ULL,
    0x00000d00000000c1ULL, 0x00000e00000000dfULL, 0x00000f00000000e9ULL, 0x00001000000000f5ULL,
    0x0000110000000103ULL, 0x000012000000011dULL, 0x0000130000000127ULL, 0x0000140000000131ULL,
    0x000015000000014bULL, 0x0000160000000155ULL, 0x000017000000016fULL, 0x000018000000017dULL,
    0x0000190000000183ULL, 0x00001a0000000199ULL, 0x00001b00000001a5ULL, 0x00001c00000001bdULL,
    0x00001d00000001cbULL, 0x00001e00000001d7ULL, 0x00001f00000001e1ULL, 0x00002000000001fbULL,
    0x000021000000020dULL, 0x0000220000000217ULL, 0x000023000000022bULL, 0x0000240000000235ULL,
    0x0000250000000249ULL, 0x0000260000000253ULL, 0x0000270000000267ULL, 0x0000280000000271ULL,
    0x000029000000028fULL, 0x00002a0000000299ULL, 0x00002b00000002a3ULL, 0x00002c00000002bdULL,
    0x00002d00000002cbULL, 0x00002e00000002d5ULL, 0x00002f00000002e7ULL, 0x00003000000002f1ULL,
    0x0000310000000305ULL, 0x000032000000031fULL, 0x0000330000000329ULL, 0x0000340000000333ULL,
    0x000035000000034dULL, 0x0000360000000357ULL, 0x000037000000036bULL, 0x0000380000000375ULL,
    0x0000390000000389ULL, 0x00003a0000000393ULL, 0x00003b00000003a7ULL, 0x00003c00000003b1ULL,
    0x00003d00000003cbULL, 0x00003e00000003d5ULL, 0x00003f00000003ebULL, 0x00004000000003f7ULL,
    0x0000410000000409ULL, 0x0000420000000413ULL, 0x0000430000000427ULL, 0x0000440000000431ULL,
    0x000045000000044bULL, 0x0000460000000455ULL, 0x000047000000046fULL, 0x0000480000000479ULL,
    0x000049000000048dULL, 0x00004a0000000497ULL, 0x00004b00000004abULL, 0x00004c00000004b5ULL,
    0x00004d00000004c9ULL, 0x00004e00000004d3ULL, 0x00004f00000004e7ULL, 0x00005000000004f1ULL,
    0x000051000000050bULL, 0x0000520000000515ULL, 0x000053000000052fULL, 0x0000540000000539ULL,
    0x000055000000054dULL, 0x0000560000000557ULL, 0x000057000000056bULL, 0x0000580000000575ULL,
    0x0000590000000589ULL, 0x00005a0000000593ULL, 0x00005b00000005a7ULL, 0x00005c00000005b1ULL,
    0x00005d00000005cbULL, 0x00005e00000005d5ULL, 0x00005f00000005e9ULL, 0x00006000000005f3ULL,
    0x0000610000000607ULL, 0x0000620000000611ULL, 0x000063000000062bULL, 0x0000640000000635ULL,
    0x000065000000064fULL, 0x0000660000000659ULL, 0x000067000000066dULL, 0x0000680000000677ULL,
    0x000069000000068bULL, 0x00006a0000000695ULL, 0x00006b00000006a9ULL, 0x00006c00000006b3ULL,
    0x00006d00000006c7ULL, 0x00006e00000006d1ULL, 0x00006f00000006ebULL, 0x00007000000006f5ULL,
    0x0000710000000709ULL, 0x0000720000000713ULL, 0x0000730000000727ULL, 0x0000740000000731ULL,
    0x000075000000074bULL, 0x0000760000000755ULL, 0x000077000000076fULL, 0x0000780000000779ULL,
    0x000079000000078dULL, 0x00007a0000000797ULL, 0x00007b00000007abULL, 0x00007c00000007b5ULL,
    0x00007d00000007c9ULL, 0x00007e00000007d3ULL, 0x00007f00000007e7ULL, 0x00008000000007f1ULL,
    0x0000810000000805ULL, 0x000082000000081fULL, 0x0000830000000829ULL, 0x0000840000000833ULL,
    0x000085000000084dULL, 0x0000860000000857ULL, 0x000087000000086bULL, 0x0000880000000875ULL,
    0x0000890000000889ULL, 0x00008a0000000893ULL, 0x00008b00000008a7ULL, 0x00008c00000008b1ULL,
    0x00008d00000008cbULL, 0x00008e00000008d5ULL, 0x00008f00000008e9ULL, 0x00009000000008f3ULL,
    0x0000910000000907ULL, 0x0000920000000911ULL, 0x000093000000092bULL, 0x0000940000000935ULL,
    0x000095000000094fULL, 0x0000960000000959ULL, 0x000097000000096dULL, 0x0000980000000977ULL,
    0x000099000000098bULL, 0x00009a0000000995ULL, 0x00009b00000009a9ULL, 0x00009c00000009b3ULL,
    0x00009d00000009c7ULL, 0x00009e00000009d1ULL, 0x00009f00000009ebULL, 0x0000a000000009f5ULL,
    0x0000a10000000a09ULL, 0x0000a20000000a13ULL, 0x0000a30000000a27ULL, 0x0000a40000000a31ULL,
    0x0000a50000000a4bULL, 0x0000a60000000a55ULL, 0x0000a70000000a6fULL, 0x0000a80000000a79ULL,
    0x0000a90000000a8dULL, 0x0000aa0000000a97ULL, 0x0000ab0000000aabULL, 0x0000ac0000000ab5ULL,
    0x0000ad0000000ac9ULL, 0x0000ae0000000ad3ULL, 0x0000af0000000ae7ULL, 0x0000b00000000af1ULL,
    0x0000b10000000b05ULL, 0x0000b20000000b1fULL, 0x0000b30000000b29ULL, 0x0000b40000000b33ULL,
    0x0000b50000000b4dULL, 0x0000b60000000b57ULL, 0x0000b70000000b6bULL, 0x0000b80000000b75ULL,
    0x0000b90000000b89ULL, 0x0000ba0000000b93ULL, 0x0000bb0000000ba7ULL, 0x0000bc0000000bb1ULL,
    0x0000bd0000000bcbULL, 0x0000be0000000bd5ULL, 0x0000bf0000000be9ULL, 0x0000c00000000bf3ULL,
    0x0000c10000000c07ULL, 0x0000c20000000c11ULL, 0x0000c30000000c2bULL, 0x0000c40000000c35ULL,
    0x0000c50000000c4fULL, 0x0000c60000000c59ULL, 0x0000c70000000c6dULL, 0x0000c80000000c77ULL,
    0x0000c90000000c8bULL, 0x0000ca0000000c95ULL, 0x0000cb0000000ca9ULL, 0x0000cc0000000cb3ULL,
    0x0000cd0000000cc7ULL, 0x0000ce0000000cd1ULL, 0x0000cf0000000cebULL, 0x0000d00000000cf5ULL,
    0x0000d10000000d09ULL, 0x0000d20000000d13ULL, 0x0000d30000000d27ULL, 0x0000d40000000d31ULL,
    0x0000d50000000d4bULL, 0x0000d60000000d55ULL, 0x0000d70000000d6fULL, 0x0000d80000000d79ULL,
    0x0000d90000000d8dULL, 0x0000da0000000d97ULL, 0x0000db0000000dabULL, 0x0000dc0000000db5ULL,
    0x0000dd0000000dc9ULL, 0x0000de0000000dd3ULL, 0x0000df0000000de7ULL, 0x0000e00000000df1ULL,
    0x0000e10000000e05ULL, 0x0000e20000000e1fULL, 0x0000e30000000e29ULL, 0x0000e40000000e33ULL,
    0x0000e50000000e4dULL, 0x0000e60000000e57ULL, 0x0000e70000000e6bULL, 0x0000e80000000e75ULL,
    0x0000e90000000e89ULL, 0x0000ea0000000e93ULL, 0x0000eb0000000ea7ULL, 0x0000ec0000000eb1ULL,
    0x0000ed0000000ecbULL, 0x0000ee0000000ed5ULL, 0x0000ef0000000ee9ULL, 0x0000f00000000ef3ULL,
    0x0000f10000000f07ULL, 0x0000f20000000f11ULL, 0x0000f30000000f2bULL, 0x0000f40000000f35ULL,
    0x0000f50000000f4fULL, 0x0000f60000000f59ULL, 0x0000f70000000f6dULL, 0x0000f80000000f77ULL,
    0x0000f90000000f8bULL, 0x0000fa0000000f95ULL, 0x0000fb0000000fa9ULL, 0x0000fc0000000fb3ULL,
    0x0000fd0000000fc7ULL, 0x0000fe0000000fd1ULL, 0x0000ff0000000febULL, 0x0001000000000ff5ULL
};

CDCChunker::CDCChunker(const CDCConfig& config)
    : config_(config) {
    if (config_.mask == 0) {
        config_.update_mask();
    }
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

    auto totalStart = std::chrono::steady_clock::now();
    double totalFpDurationMs = 0.0;

    const size_t ioBufferSize = 64 * 1024;
    std::vector<uint8_t> ioBuffer(ioBufferSize);

    // Buffer storing currently accumulated chunk bytes (capped by maxChunkSize)
    std::vector<uint8_t> currentChunk;
    currentChunk.reserve(config_.maxChunkSize);

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
        file.read(reinterpret_cast<char*>(ioBuffer.data()), static_cast<std::streamsize>(ioBufferSize));
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
    outMetrics.avgChunkSize = chunksGenerated > 0 ? static_cast<double>(totalBytes) / chunksGenerated : 0.0;
    outMetrics.estimatedMetadataBytes = chunksGenerated * (sizeof(ChunkMetadata) + relativePath.size() + 32);

    if (totalFpDurationMs > 0.0) {
        outMetrics.fingerprintThroughputMBps = (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (totalFpDurationMs / 1000.0);
    }
    if (totalDurationMs > 0.0) {
        outMetrics.totalThroughputMBps = (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / (totalDurationMs / 1000.0);
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
    outMetrics.avgChunkSize = chunksGenerated > 0 ? static_cast<double>(length) / chunksGenerated : 0.0;
    outMetrics.estimatedMetadataBytes = chunksGenerated * (sizeof(ChunkMetadata) + relativePath.size() + 32);

    if (totalFpDurationMs > 0.0) {
        outMetrics.fingerprintThroughputMBps = (static_cast<double>(length) / (1024.0 * 1024.0)) / (totalFpDurationMs / 1000.0);
    }
    if (totalDurationMs > 0.0) {
        outMetrics.totalThroughputMBps = (static_cast<double>(length) / (1024.0 * 1024.0)) / (totalDurationMs / 1000.0);
    }

    return true;
}
