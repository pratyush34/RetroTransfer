#include "manifest_writer.h"

#include <iomanip>

namespace retro {

std::string jsonEscape(const std::string& value) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20) {
                    escaped += "\\u00";
                    escaped += HEX[(character >> 4) & 0x0f];
                    escaped += HEX[character & 0x0f];
                } else {
                    escaped += static_cast<char>(character);
                }
        }
    }
    return escaped;
}

void writeChunkManifestJson(std::ostream& out,
                            const ProjectChunkResult& result,
                            const EngineConfig& config,
                            const std::string& rootPath,
                            const std::string& baselinePath,
                            const std::string& payloadName,
                            const CompressionResult* compression) {
    out << "{\n";
    out << "  \"rootPath\": \"" << jsonEscape(rootPath) << "\",\n";
    out << "  \"selectedStrategy\": \"" << strategy_to_string(config.strategy) << "\",\n";
    out << "  \"chunking\": {\n";
    out << "    \"fixedChunkSize\": " << config.fixedConfig.chunkSize << ",\n";
    out << "    \"cdcMinChunkSize\": " << config.cdcConfig.minChunkSize << ",\n";
    out << "    \"cdcTargetChunkSize\": " << config.cdcConfig.targetChunkSize << ",\n";
    out << "    \"cdcMaxChunkSize\": " << config.cdcConfig.maxChunkSize << "\n";
    out << "  },\n";
    out << "  \"summary\": {\n";
    out << "    \"totalFiles\": " << result.metrics.totalFilesProcessed << ",\n";
    out << "    \"totalBytes\": " << result.metrics.totalBytesProcessed << ",\n";
    out << "    \"totalChunks\": " << result.metrics.totalChunksGenerated << ",\n";
    out << "    \"uniqueFingerprints\": " << result.metrics.uniqueFingerprints << ",\n";
    out << "    \"deduplicatedBytes\": " << result.metrics.deduplicatedBytes << ",\n";
    out << "    \"internalDedupRatio\": " << std::fixed << std::setprecision(4) 
        << result.metrics.internalDedupRatio << ",\n";
    out << "    \"totalChunkingTimeMs\": " << std::fixed << std::setprecision(2) 
        << result.metrics.totalChunkingTimeMs << ",\n";
    out << "    \"totalFingerprintingTimeMs\": " << std::fixed << std::setprecision(2) 
        << result.metrics.totalFingerprintingTimeMs << ",\n";
    out << "    \"totalExecutionTimeMs\": " << std::fixed << std::setprecision(2) 
        << result.metrics.totalExecutionTimeMs << ",\n";
    out << "    \"overallThroughputMBps\": " << std::fixed << std::setprecision(2) 
        << result.metrics.overallThroughputMBps << ",\n";
    out << "    \"estimatedMetadataBytes\": " << result.metrics.totalEstimatedMetadataBytes << "\n";
    out << "  },\n";
    if (compression != nullptr) {
        const CompressionMetrics& metrics = compression->metrics;
        out << "  \"selectiveCompression\": {\n";
        out << "    \"baselineRoot\": \""
            << jsonEscape(baselinePath.empty()
                              ? "(none; all current chunks are packaged)"
                              : baselinePath)
            << "\",\n";
        out << "    \"payloadFile\": \"" << jsonEscape(payloadName) << "\",\n";
        out << "    \"payloadFormat\": \"RTPK-v1\",\n";
        out << "    \"codec\": \"canonical-huffman-per-chunk-with-raw-fallback\",\n";
        out << "    \"totalChunks\": " << metrics.totalChunks << ",\n";
        out << "    \"reusableChunks\": " << metrics.reusableChunks << ",\n";
        out << "    \"missingChunks\": " << metrics.missingChunks << ",\n";
        out << "    \"duplicateChunks\": " << metrics.duplicateChunks << ",\n";
        out << "    \"huffmanChunks\": " << metrics.huffmanChunks << ",\n";
        out << "    \"rawChunks\": " << metrics.storedRawChunks << ",\n";
        out << "    \"reusableBytes\": " << metrics.reusableBytes << ",\n";
        out << "    \"duplicateBytesAvoided\": "
            << metrics.duplicateBytesAvoided << ",\n";
        out << "    \"totalSourceBytes\": " << metrics.totalSourceBytes << ",\n";
        out << "    \"selectedInputBytes\": " << metrics.selectedInputBytes << ",\n";
        out << "    \"uniqueInputBytes\": " << metrics.uniqueInputBytes << ",\n";
        out << "    \"payloadBytes\": " << metrics.payloadBytes << ",\n";
        out << "    \"bundleHeaderBytes\": " << metrics.bundleHeaderBytes << ",\n";
        out << "    \"compressionBytesSaved\": "
            << metrics.compressionBytesSaved << ",\n";
        out << "    \"totalBytesAvoided\": " << metrics.totalBytesAvoided << ",\n";
        out << "    \"payloadChunks\": [\n";
        for (size_t i = 0; i < compression->payloadChunks.size(); ++i) {
            const CompressedChunkInfo& chunk = compression->payloadChunks[i];
            out << "      {\"path\": \"" << jsonEscape(chunk.metadata.relativePath)
                << "\", \"seq\": " << chunk.metadata.sequenceNumber
                << ", \"fingerprint\": \"" << chunk.metadata.fingerprint
                << "\", \"sourceOffset\": " << chunk.metadata.offset
                << ", \"originalSize\": " << chunk.metadata.length
                << ", \"payloadOffset\": " << chunk.payloadOffset
                << ", \"storedSize\": " << chunk.storedSize
                << ", \"codec\": \"" << chunk.codec
                << "\", \"crc32\": " << chunk.metadata.crc32 << "}";
            if (i + 1 < compression->payloadChunks.size()) out << ",";
            out << "\n";
        }
        out << "    ]\n";
        out << "  },\n";
    }

    out << "  \"files\": [\n";
    for (size_t i = 0; i < result.files.size(); ++i) {
        const auto& f = result.files[i];
        out << "    {\n";
        out << "      \"path\": \"" << jsonEscape(f.relativePath) << "\",\n";
        out << "      \"size\": " << f.metrics.fileSize << ",\n";
        out << "      \"strategy\": \"" << f.metrics.selectedStrategy << "\",\n";
        out << "      \"reason\": \"" << jsonEscape(f.decision.reason) << "\",\n";
        out << "      \"chunkCount\": " << f.chunks.size() << ",\n";
        out << "      \"avgChunkSize\": " << std::fixed << std::setprecision(1) << f.metrics.avgChunkSize << ",\n";
        out << "      \"minChunkSize\": " << f.metrics.minChunkSize << ",\n";
        out << "      \"maxChunkSize\": " << f.metrics.maxChunkSize << ",\n";
        out << "      \"totalTimeMs\": " << std::fixed << std::setprecision(3) << f.metrics.totalDurationMs << ",\n";
        out << "      \"chunks\": [\n";
        for (size_t c = 0; c < f.chunks.size(); ++c) {
            const auto& chunk = f.chunks[c];
            out << "        {\"seq\": " << chunk.sequenceNumber
                << ", \"offset\": " << chunk.offset
                << ", \"length\": " << chunk.length
                << ", \"fingerprint\": \"" << chunk.fingerprint << "\""
                << ", \"crc32\": " << chunk.crc32
                << ", \"algorithm\": \"" << chunk.algorithm << "\"}";
            if (c + 1 < f.chunks.size()) out << ",";
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
        if (i + 1 < result.files.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
}

} // namespace retro
