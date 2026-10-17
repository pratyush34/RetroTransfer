#include "compression_engine.h"

#include "compression_engine_c.h"

#include <filesystem>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

bool CompressionEngine::compressMissingChunks(const ProjectChunkResult& current,
                                               const ProjectChunkResult& baseline,
                                               const std::string& payloadPath,
                                               CompressionResult& result,
                                               std::string& error) const {
    result = {};
    error.clear();

    std::vector<RtCompressionChunk> currentChunks;
    std::vector<RtCompressionChunk> baselineChunks;
    std::vector<const ChunkMetadata*> currentMetadata;
    for (const FileChunkResult& file : current.files) {
        for (const ChunkMetadata& chunk : file.chunks) {
            currentChunks.push_back({file.fileInfo.filePath.c_str(),
                                     file.relativePath.c_str(),
                                     chunk.fingerprint.c_str(),
                                     chunk.offset,
                                     chunk.length,
                                     chunk.sequenceNumber,
                                     chunk.crc32});
            currentMetadata.push_back(&chunk);
        }
    }
    for (const FileChunkResult& file : baseline.files) {
        for (const ChunkMetadata& chunk : file.chunks) {
            baselineChunks.push_back({file.fileInfo.filePath.c_str(),
                                      file.relativePath.c_str(),
                                      chunk.fingerprint.c_str(),
                                      chunk.offset,
                                      chunk.length,
                                      chunk.sequenceNumber,
                                      chunk.crc32});
        }
    }

    const fs::path outputPath(payloadPath);
    std::error_code directoryError;
    if (!outputPath.parent_path().empty()) {
        fs::create_directories(outputPath.parent_path(), directoryError);
        if (directoryError) {
            error = "Could not create payload directory: " + directoryError.message();
            return false;
        }
    }

    RtCompressionResult cResult{};
    char errorBuffer[1024] = {};
    if (!rt_compress_missing_chunks(currentChunks.data(), currentChunks.size(),
                                    baselineChunks.data(), baselineChunks.size(),
                                    payloadPath.c_str(), &cResult,
                                    errorBuffer, sizeof(errorBuffer))) {
        error = errorBuffer;
        return false;
    }

    result.metrics.totalChunks = cResult.metrics.total_chunks;
    result.metrics.reusableChunks = cResult.metrics.reusable_chunks;
    result.metrics.missingChunks = cResult.metrics.missing_chunks;
    result.metrics.duplicateChunks = cResult.metrics.duplicate_chunks;
    result.metrics.huffmanChunks = cResult.metrics.huffman_chunks;
    result.metrics.storedRawChunks = cResult.metrics.stored_raw_chunks;
    result.metrics.reusableBytes = cResult.metrics.reusable_bytes;
    result.metrics.duplicateBytesAvoided = cResult.metrics.duplicate_bytes_avoided;
    result.metrics.totalSourceBytes = cResult.metrics.total_source_bytes;
    result.metrics.selectedInputBytes = cResult.metrics.selected_input_bytes;
    result.metrics.uniqueInputBytes = cResult.metrics.unique_input_bytes;
    result.metrics.payloadBytes = cResult.metrics.payload_bytes;
    result.metrics.bundleHeaderBytes = cResult.metrics.bundle_header_bytes;
    result.metrics.compressionBytesSaved = cResult.metrics.compression_bytes_saved;
    result.metrics.totalBytesAvoided = cResult.metrics.total_bytes_avoided;
    result.payloadChunks.reserve(cResult.chunk_count);
    for (size_t i = 0; i < cResult.chunk_count; ++i) {
        const RtCompressedChunk& compressed = cResult.chunks[i];
        CompressedChunkInfo info;
        info.metadata = *currentMetadata[compressed.current_index];
        info.payloadOffset = compressed.payload_offset;
        info.storedSize = compressed.stored_size;
        info.codec = compressed.uses_huffman ? "huffman" : "raw";
        result.payloadChunks.push_back(std::move(info));
    }
    rt_free_compression_result(&cResult);
    return true;
}
