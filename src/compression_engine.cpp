#include "compression_engine.h"

#include "fingerprint.h"
#include "huffman_codec.h"

#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

struct ChunkLocation {
    const FileChunkResult* file;
    const ChunkMetadata* chunk;
};

bool matchesChunkInIndex(
    const ChunkMetadata& candidate,
    const std::vector<uint8_t>& candidateBytes,
    const std::unordered_map<std::string,
                             std::vector<ChunkLocation>>& index,
    std::string& error) {
    const auto found = index.find(candidate.fingerprint);
    if (found == index.end()) return false;

    for (const ChunkLocation& location : found->second) {
        if (location.chunk->length != candidate.length) continue;

        std::vector<uint8_t> baselineBytes;
        if (!ChunkEngine::readChunkPayload(location.file->fileInfo.filePath,
                                           location.chunk->offset,
                                           location.chunk->length,
                                           baselineBytes)) {
            error = "Could not reread baseline chunk: " +
                    location.file->fileInfo.filePath;
            return false;
        }
        if (candidateBytes == baselineBytes) return true;
    }
    return false;
}

bool writeBytes(std::ofstream& output, const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return true;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

} // namespace

bool CompressionEngine::compressMissingChunks(const ProjectChunkResult& current,
                                               const ProjectChunkResult& baseline,
                                               const std::string& payloadPath,
                                               CompressionResult& result,
                                               std::string& error) const {
    result = {};
    error.clear();

    std::unordered_map<std::string, std::vector<ChunkLocation>> baselineIndex;
    std::unordered_map<std::string, std::vector<ChunkLocation>> payloadIndex;
    for (const FileChunkResult& file : baseline.files) {
        for (const ChunkMetadata& chunk : file.chunks) {
            baselineIndex[chunk.fingerprint].push_back({&file, &chunk});
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

    std::ofstream output(payloadPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not create compressed payload: " + payloadPath;
        return false;
    }

    static constexpr char header[] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
    output.write(header, sizeof(header));
    if (!output) {
        error = "Could not write compressed payload header: " + payloadPath;
        return false;
    }

    for (const FileChunkResult& file : current.files) {
        for (const ChunkMetadata& chunk : file.chunks) {
            ++result.metrics.totalChunks;
            result.metrics.totalSourceBytes += chunk.length;
            std::vector<uint8_t> raw;
            if (!ChunkEngine::readChunkPayload(file.fileInfo.filePath, chunk.offset,
                                               chunk.length, raw)) {
                error = "Could not reread source chunk: " + file.fileInfo.filePath;
                return false;
            }
            if (compute_fingerprint_hex(raw.data(), raw.size()) != chunk.fingerprint ||
                compute_crc32(raw.data(), raw.size()) != chunk.crc32) {
                error = "Source chunk changed after scanning: " + file.relativePath +
                        " chunk " + std::to_string(chunk.sequenceNumber);
                return false;
            }

            const bool foundInBaseline =
                matchesChunkInIndex(chunk, raw, baselineIndex, error);
            if (!error.empty()) return false;
            if (foundInBaseline) {
                ++result.metrics.reusableChunks;
                result.metrics.reusableBytes += chunk.length;
                continue;
            }

            ++result.metrics.missingChunks;
            result.metrics.selectedInputBytes += chunk.length;
            const bool foundInPayload =
                matchesChunkInIndex(chunk, raw, payloadIndex, error);
            if (!error.empty()) return false;
            if (foundInPayload) {
                ++result.metrics.duplicateChunks;
                result.metrics.duplicateBytesAvoided += chunk.length;
                continue;
            }

            std::vector<uint8_t> encoded;
            if (!HuffmanCodec::compress(raw, encoded, error)) {
                error = "Could not Huffman-compress " + file.relativePath +
                        " chunk " + std::to_string(chunk.sequenceNumber) + ": " + error;
                return false;
            }

            const bool useHuffman = encoded.size() < raw.size();
            const std::vector<uint8_t>& payload = useHuffman ? encoded : raw;
            const std::streampos position = output.tellp();
            if (position < 0) {
                error = "Could not determine payload offset: " + payloadPath;
                return false;
            }
            if (!writeBytes(output, payload)) {
                error = "Failed writing compressed payload: " + payloadPath;
                return false;
            }

            CompressedChunkInfo info;
            info.metadata = chunk;
            info.payloadOffset = static_cast<uint64_t>(
                static_cast<std::streamoff>(position));
            info.storedSize = payload.size();
            info.codec = useHuffman ? "huffman" : "raw";
            result.payloadChunks.push_back(std::move(info));
            payloadIndex[chunk.fingerprint].push_back({&file, &chunk});

            result.metrics.uniqueInputBytes += chunk.length;
            result.metrics.payloadBytes += payload.size();
            if (useHuffman) {
                ++result.metrics.huffmanChunks;
            } else {
                ++result.metrics.storedRawChunks;
            }
        }
    }

    output.close();
    if (!output) {
        error = "Failed closing compressed payload: " + payloadPath;
        return false;
    }
    result.metrics.compressionBytesSaved =
        result.metrics.uniqueInputBytes > result.metrics.payloadBytes
            ? result.metrics.uniqueInputBytes - result.metrics.payloadBytes
            : 0;
    const uint64_t bundleBytes =
        result.metrics.payloadBytes + result.metrics.bundleHeaderBytes;
    result.metrics.totalBytesAvoided =
        result.metrics.totalSourceBytes > bundleBytes
            ? result.metrics.totalSourceBytes - bundleBytes
            : 0;
    return true;
}
