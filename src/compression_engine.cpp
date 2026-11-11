#include "compression_engine.h"

#include "compression_engine_c.h"
#include "reconstruction_engine.h"
#include "storage/json_helper.h"
#include "fingerprint_c.h"
#include "huffman_codec_c.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
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

bool CompressionEngine::compressSelectedMissingChunks(
    const ProjectChunkResult& current,
    const std::unordered_set<std::string>& missingFingerprints,
    const std::string& payloadPath,
    CompressionResult& result,
    std::string& error) const {
    result = {};
    error.clear();

    if (missingFingerprints.empty()) {
        result.metrics.bundleHeaderBytes = 8;
        result.metrics.totalChunks = current.metrics.totalChunksGenerated;
        result.metrics.reusableChunks = result.metrics.totalChunks;
        result.metrics.missingChunks = 0;
        result.metrics.reusableBytes = current.metrics.totalBytesProcessed;
        result.metrics.totalSourceBytes = current.metrics.totalBytesProcessed;
        result.metrics.totalBytesAvoided = current.metrics.totalBytesProcessed;

        if (!payloadPath.empty()) {
            fs::path outP(payloadPath);
            if (!outP.parent_path().empty()) {
                std::error_code ec;
                fs::create_directories(outP.parent_path(), ec);
            }
            std::ofstream out(payloadPath, std::ios::binary);
            static const uint8_t header[8] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
            out.write(reinterpret_cast<const char*>(header), 8);
        }
        return true;
    }

    // Baseline contains all chunks NOT in missingFingerprints
    ProjectChunkResult baseline;
    for (const auto& file : current.files) {
        FileChunkResult bFile;
        bFile.fileInfo = file.fileInfo;
        bFile.relativePath = file.relativePath;
        for (const auto& chunk : file.chunks) {
            if (missingFingerprints.find(chunk.fingerprint) == missingFingerprints.end()) {
                bFile.chunks.push_back(chunk);
            }
        }
        if (!bFile.chunks.empty()) {
            baseline.files.push_back(std::move(bFile));
        }
    }
    return compressMissingChunks(current, baseline, payloadPath, result, error);
}

bool CompressionEngine::createSelectivePayloadFromExisting(
    const std::string& sourceManifestPath,
    const std::string& sourcePayloadPath,
    const std::unordered_set<std::string>& missingFingerprints,
    const std::string& outPayloadPath,
    CompressionResult& result,
    std::string& error) const {
    result = {};
    error.clear();

    fs::path outPath(outPayloadPath);
    if (!outPath.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
    }

    std::ofstream out(outPayloadPath, std::ios::binary);
    if (!out) {
        error = "Failed to open output payload: " + outPayloadPath;
        return false;
    }
    static const uint8_t header[8] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
    out.write(reinterpret_cast<const char*>(header), 8);
    result.metrics.bundleHeaderBytes = 8;
    result.metrics.payloadBytes = 8;

    if (missingFingerprints.empty()) {
        return true;
    }

    // Try extracting from source payload if available
    std::map<std::string, ExtractedChunk> extractedMap;
    if (!sourcePayloadPath.empty() && fs::exists(sourcePayloadPath)) {
        std::vector<ExtractedChunk> extractedList;
        std::string extErr;
        if (ReconstructionEngine::extractPayloadChunks(sourceManifestPath, sourcePayloadPath, extractedList, extErr)) {
            for (auto& chk : extractedList) {
                extractedMap.emplace(chk.fingerprint, std::move(chk));
            }
        }
    }

    // Parse manifest to get chunk metadata
    std::string manifestContent;
    std::ifstream mf(sourceManifestPath);
    if (!mf) {
        error = "Could not open manifest: " + sourceManifestPath;
        return false;
    }
    std::stringstream ss;
    ss << mf.rdbuf();
    manifestContent = ss.str();

    retro::storage::StorageJsonParser parser(manifestContent);
    retro::storage::StorageJsonValue manifestJson;
    if (!parser.parse(manifestJson, error)) {
        return false;
    }

    std::string rootPath;
    auto rootIt = manifestJson.object.find("rootPath");
    if (rootIt != manifestJson.object.end()) {
        rootPath = rootIt->second.asString();
    }
    auto filesIt = manifestJson.object.find("files");
    if (filesIt == manifestJson.object.end() || !filesIt->second.isArray()) {
        error = "Manifest missing 'files' array: " + sourceManifestPath;
        return false;
    }
    const auto& filesArr = filesIt->second.array;

    std::unordered_set<std::string> writtenInPayload;
    uint64_t currentOffset = 8;

    for (const auto& fVal : filesArr) {
        if (!fVal.isObject()) continue;
        auto pIt = fVal.object.find("path");
        std::string relPath = (pIt != fVal.object.end()) ? pIt->second.asString() : "";
        auto cIt = fVal.object.find("chunks");
        if (cIt == fVal.object.end() || !cIt->second.isArray()) continue;
        const auto& chunksArr = cIt->second.array;

        for (const auto& cVal : chunksArr) {
            if (!cVal.isObject()) continue;
            auto fpIt = cVal.object.find("fingerprint");
            auto lenIt = cVal.object.find("length");
            auto crcIt = cVal.object.find("crc32");
            auto offIt = cVal.object.find("offset");
            auto seqIt = cVal.object.find("seq");
            if (fpIt == cVal.object.end() || lenIt == cVal.object.end() || crcIt == cVal.object.end()) {
                continue;
            }

            std::string fp = fpIt->second.asString();
            uint64_t len = lenIt->second.asUint64();
            uint32_t crc = static_cast<uint32_t>(crcIt->second.asUint64());
            uint64_t srcOff = (offIt != cVal.object.end()) ? offIt->second.asUint64() : 0;
            uint32_t seq = (seqIt != cVal.object.end()) ? static_cast<uint32_t>(seqIt->second.asUint64()) : 0;

            result.metrics.totalChunks++;
            result.metrics.totalSourceBytes += len;

            if (missingFingerprints.find(fp) == missingFingerprints.end()) {
                result.metrics.reusableChunks++;
                result.metrics.reusableBytes += len;
                continue;
            }

            result.metrics.missingChunks++;
            result.metrics.selectedInputBytes += len;

            if (writtenInPayload.find(fp) != writtenInPayload.end()) {
                result.metrics.duplicateChunks++;
                result.metrics.duplicateBytesAvoided += len;
                continue;
            }

            // Get chunk bytes
            std::vector<uint8_t> chunkBytes;
            auto it = extractedMap.find(fp);
            if (it != extractedMap.end()) {
                chunkBytes = it->second.data;
            } else {
                // Read from source file on disk
                fs::path fullPath = fs::path(rootPath) / relPath;
                if (!fs::exists(fullPath)) {
                    fullPath = fs::path(sourceManifestPath).parent_path() / relPath;
                }
                std::ifstream srcFile(fullPath, std::ios::binary);
                if (!srcFile) {
                    error = "Could not find chunk source file: " + fullPath.string();
                    return false;
                }
                srcFile.seekg(static_cast<std::streamoff>(srcOff));
                chunkBytes.resize(static_cast<size_t>(len));
                srcFile.read(reinterpret_cast<char*>(chunkBytes.data()), static_cast<std::streamsize>(len));
                if (static_cast<size_t>(srcFile.gcount()) != len) {
                    error = "Short read while reading chunk " + fp;
                    return false;
                }
            }

            // Verify integrity
            if (chunkBytes.size() != len || rt_compute_crc32(chunkBytes.data(), chunkBytes.size()) != crc) {
                error = "Integrity check failed for missing chunk " + fp;
                return false;
            }

            // Compress with Huffman
            uint8_t* encoded = nullptr;
            size_t encodedSize = 0;
            char huffErr[256] = {};
            bool huffOk = rt_huffman_compress(chunkBytes.data(), chunkBytes.size(), &encoded, &encodedSize, huffErr, sizeof(huffErr));
            bool useHuffman = huffOk && (encodedSize < chunkBytes.size());

            const uint8_t* writePtr = useHuffman ? encoded : chunkBytes.data();
            size_t writeSize = useHuffman ? encodedSize : chunkBytes.size();

            out.write(reinterpret_cast<const char*>(writePtr), writeSize);
            if (!out) {
                if (encoded) free(encoded);
                error = "Failed writing chunk to selective payload: " + outPayloadPath;
                return false;
            }

            CompressedChunkInfo cInfo;
            cInfo.metadata.relativePath = relPath;
            cInfo.metadata.sequenceNumber = seq;
            cInfo.metadata.fingerprint = fp;
            cInfo.metadata.offset = srcOff;
            cInfo.metadata.length = len;
            cInfo.metadata.crc32 = crc;
            cInfo.payloadOffset = currentOffset;
            cInfo.storedSize = writeSize;
            cInfo.codec = useHuffman ? "huffman" : "raw";
            result.payloadChunks.push_back(std::move(cInfo));

            currentOffset += writeSize;
            result.metrics.payloadBytes += writeSize;
            result.metrics.uniqueInputBytes += len;
            if (useHuffman) {
                result.metrics.huffmanChunks++;
                result.metrics.compressionBytesSaved += (len - writeSize);
            } else {
                result.metrics.storedRawChunks++;
            }
            if (encoded) free(encoded);
            writtenInPayload.insert(fp);
        }
    }

    result.metrics.totalBytesAvoided = result.metrics.reusableBytes + result.metrics.duplicateBytesAvoided + result.metrics.compressionBytesSaved;
    return true;
}
