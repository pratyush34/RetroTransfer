#include "scanner.h"
#include "chunk_engine.h"
#include "chunk_config.h"

#include <chrono>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string jsonEscape(const std::string& value) {
    std::string escaped;
    for (char character : value) {
        if (character == '\\' || character == '"') {
            escaped += '\\';
        } else if (character == '\n') {
            escaped += "\\n";
            continue;
        } else if (character == '\r') {
            escaped += "\\r";
            continue;
        } else if (character == '\t') {
            escaped += "\\t";
            continue;
        }
        escaped += character;
    }
    return escaped;
}

static void writeIndent(std::ofstream& output, int depth) {
    output << std::string(depth * 2, ' ');
}

static void writeDirectory(std::ofstream& output, const DirectoryNode& directory, int depth) {
    writeIndent(output, depth);
    output << "{\n";
    writeIndent(output, depth + 1);
    output << "\"name\": \"" << jsonEscape(directory.directoryName) << "\",\n";
    writeIndent(output, depth + 1);
    output << "\"path\": \"" << jsonEscape(directory.directoryPath) << "\",\n";
    writeIndent(output, depth + 1);
    output << "\"directories\": [\n";
    for (size_t index = 0; index < directory.directories.size(); ++index) {
        writeDirectory(output, directory.directories[index], depth + 2);
        if (index + 1 < directory.directories.size()) {
            output << ",";
        }
        output << "\n";
    }
    writeIndent(output, depth + 1);
    output << "],\n";
    writeIndent(output, depth + 1);
    output << "\"files\": [\n";
    for (size_t index = 0; index < directory.files.size(); ++index) {
        const FileInfo& file = directory.files[index];
        writeIndent(output, depth + 2);
        output << "{\"name\": \"" << jsonEscape(file.fileName)
               << "\", \"path\": \"" << jsonEscape(file.filePath)
               << "\", \"size\": " << file.fileSize
               << ", \"type\": \"" << jsonEscape(file.fileType)
               << "\", \"modificationTime\": " << file.modificationTime << "}";
        if (index + 1 < directory.files.size()) {
            output << ",";
        }
        output << "\n";
    }
    writeIndent(output, depth + 1);
    output << "]\n";
    writeIndent(output, depth);
    output << "}";
}

static void writeChunkManifestJson(std::ofstream& out,
                                   const ProjectChunkResult& result,
                                   const EngineConfig& config,
                                   const std::string& rootPath) {
    out << "{\n";
    out << "  \"rootPath\": \"" << jsonEscape(rootPath) << "\",\n";
    out << "  \"selectedStrategy\": \"" << strategy_to_string(config.strategy) << "\",\n";
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

    out << "  \"files\": [\n";
    for (size_t i = 0; i < result.files.size(); ++i) {
        const auto& f = result.files[i];
        out << "    {\n";
        out << "      \"path\": \"" << jsonEscape(f.relativePath) << "\",\n";
        out << "      \"size\": " << f.fileInfo.fileSize << ",\n";
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

static void printUsage(const char* progName) {
    std::cout << "RetroTransfer - Project Scanner and Adaptive Chunking Engine\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " [directory] [output.json] [options]\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  [directory]           Root directory to scan (default: \".\")\n";
    std::cout << "  [output.json]         Filename for scan output inside collection/ (default: \"scan.json\")\n\n";
    std::cout << "Options:\n";
    std::cout << "  --strategy, -s <mode> Chunking strategy: \"adaptive\" (default), \"fixed\", or \"cdc\"\n";
    std::cout << "  --chunk-size <bytes>  Fixed chunk size in bytes (default: 65536)\n";
    std::cout << "  --min-chunk <bytes>   CDC minimum chunk size (default: 16384)\n";
    std::cout << "  --target-chunk <bytes>CDC target chunk size (default: 65536)\n";
    std::cout << "  --max-chunk <bytes>   CDC maximum chunk size (default: 262144)\n";
    std::cout << "  --benchmark           Run repeatable benchmark comparing Fixed, CDC, and Adaptive\n";
    std::cout << "  --no-chunk            Only perform project scan, skip chunking stage\n";
    std::cout << "  --help, -h            Show this help message\n";
}

static void runBenchmark(const DirectoryNode& directoryTree, const std::string& path) {
    std::cout << "\n============================================================\n";
    std::cout << "        RETROTRANSFER CHUNKING STRATEGY BENCHMARK           \n";
    std::cout << "============================================================\n";

    std::vector<ChunkStrategy> strategies = {
        ChunkStrategy::Fixed,
        ChunkStrategy::CDC,
        ChunkStrategy::Adaptive
    };

    struct BenchRow {
        std::string name;
        size_t chunks;
        size_t uniqueFp;
        uint64_t dedupBytes;
        double dedupRatio;
        double chunkTimeMs;
        double fpTimeMs;
        double totalTimeMs;
        double throughputMBps;
        uint64_t metadataBytes;
    };

    std::vector<BenchRow> rows;

    for (auto strat : strategies) {
        EngineConfig cfg;
        cfg.strategy = strat;
        ChunkEngine engine(cfg);

        ProjectChunkResult res = engine.processScannedProject(directoryTree, path);
        BenchRow row;
        row.name = strategy_to_string(strat);
        row.chunks = res.metrics.totalChunksGenerated;
        row.uniqueFp = res.metrics.uniqueFingerprints;
        row.dedupBytes = res.metrics.deduplicatedBytes;
        row.dedupRatio = res.metrics.internalDedupRatio * 100.0;
        row.chunkTimeMs = res.metrics.totalChunkingTimeMs;
        row.fpTimeMs = res.metrics.totalFingerprintingTimeMs;
        row.totalTimeMs = res.metrics.totalExecutionTimeMs;
        row.throughputMBps = res.metrics.overallThroughputMBps;
        row.metadataBytes = res.metrics.totalEstimatedMetadataBytes;
        rows.push_back(row);
    }

    std::cout << std::left 
              << std::setw(12) << "Strategy"
              << std::setw(10) << "Chunks"
              << std::setw(12) << "Unique FPs"
              << std::setw(12) << "Dedup %"
              << std::setw(14) << "Time (ms)"
              << std::setw(18) << "Throughput(MB/s)"
              << std::setw(16) << "Metadata (KB)"
              << "\n";
    std::cout << std::string(84, '-') << "\n";

    for (const auto& r : rows) {
        std::cout << std::left 
                  << std::setw(12) << r.name
                  << std::setw(10) << r.chunks
                  << std::setw(12) << r.uniqueFp
                  << std::setw(12) << (std::to_string(static_cast<int>(r.dedupRatio * 10) / 10.0) + "%")
                  << std::setw(14) << std::fixed << std::setprecision(2) << r.totalTimeMs
                  << std::setw(18) << std::fixed << std::setprecision(2) << r.throughputMBps
                  << std::setw(16) << (r.metadataBytes / 1024)
                  << "\n";
    }
    std::cout << "============================================================\n\n";
}

int main(int argc, char* argv[]) {
    std::string path = ".";
    std::string outputName = "scan.json";
    bool customOutputName = false;
    bool enableChunking = true;
    bool benchmarkMode = false;

    EngineConfig config;

    // Parse command line arguments
    int positionalIdx = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--no-chunk") {
            enableChunking = false;
        } else if (arg == "--benchmark") {
            benchmarkMode = true;
        } else if ((arg == "--strategy" || arg == "-s") && i + 1 < argc) {
            std::string stratStr = argv[++i];
            if (!string_to_strategy(stratStr, config.strategy)) {
                std::cerr << "Warning: Unknown strategy '" << stratStr << "'. Using adaptive.\n";
            }
        } else if (arg == "--chunk-size" && i + 1 < argc) {
            config.fixedConfig.chunkSize = std::stoull(argv[++i]);
        } else if (arg == "--min-chunk" && i + 1 < argc) {
            config.cdcConfig.minChunkSize = std::stoull(argv[++i]);
            config.cdcConfig.update_mask();
        } else if (arg == "--target-chunk" && i + 1 < argc) {
            config.cdcConfig.targetChunkSize = std::stoull(argv[++i]);
            config.cdcConfig.update_mask();
        } else if (arg == "--max-chunk" && i + 1 < argc) {
            config.cdcConfig.maxChunkSize = std::stoull(argv[++i]);
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Warning: Unrecognized option: " << arg << "\n";
        } else {
            if (positionalIdx == 0) {
                path = arg;
                positionalIdx++;
            } else if (positionalIdx == 1) {
                outputName = arg;
                customOutputName = true;
                positionalIdx++;
            }
        }
    }

    const fs::path collectionFolder = "collection";
    const fs::path outputPath = collectionFolder / fs::path(outputName).filename();

    // 1. Project Scanner Stage
    std::cout << "[Stage 1/2] Scanning directory: " << path << " ...\n";
    ProjectScanner scanner(path);
    const DirectoryNode directoryTree = scanner.scan();

    fs::create_directories(collectionFolder);
    std::ofstream output(outputPath);
    if (!output) {
        std::cerr << "Error: Could not create output file: " << outputPath << "\n";
        return 1;
    }

    writeDirectory(output, directoryTree, 0);
    output << "\n";
    std::cout << "Saved scan data to " << outputPath << "\n";

    // 2. Chunking Engine Stage
    if (enableChunking) {
        std::cout << "[Stage 2/2] Running Chunking Engine (Mode: " 
                  << strategy_to_string(config.strategy) << ") ...\n";

        ChunkEngine engine(config);
        ProjectChunkResult chunkResult = engine.processScannedProject(directoryTree, path);

        // Save chunk manifest
        fs::path chunkOutFilename = outputPath.stem().string() + "_chunks.json";
        fs::path chunkOutputPath = collectionFolder / chunkOutFilename;
        std::ofstream chunkOut(chunkOutputPath);
        if (chunkOut) {
            writeChunkManifestJson(chunkOut, chunkResult, config, path);
            std::cout << "Saved chunk manifest to " << chunkOutputPath << "\n";
        }

        std::cout << "\n--- Chunking Engine Summary ---\n";
        std::cout << "Files processed    : " << chunkResult.metrics.totalFilesProcessed << "\n";
        std::cout << "Total bytes        : " << chunkResult.metrics.totalBytesProcessed << " bytes ("
                  << std::fixed << std::setprecision(2)
                  << (chunkResult.metrics.totalBytesProcessed / (1024.0 * 1024.0)) << " MB)\n";
        std::cout << "Chunks generated   : " << chunkResult.metrics.totalChunksGenerated << "\n";
        std::cout << "Unique fingerprints: " << chunkResult.metrics.uniqueFingerprints << "\n";
        std::cout << "Internal dedup bytes: " << chunkResult.metrics.deduplicatedBytes << " bytes ("
                  << std::fixed << std::setprecision(2)
                  << (chunkResult.metrics.internalDedupRatio * 100.0) << "%)\n";
        std::cout << "Chunking time      : " << std::fixed << std::setprecision(2)
                  << chunkResult.metrics.totalChunkingTimeMs << " ms\n";
        std::cout << "Fingerprinting time: " << std::fixed << std::setprecision(2)
                  << chunkResult.metrics.totalFingerprintingTimeMs << " ms\n";
        std::cout << "Total engine time  : " << std::fixed << std::setprecision(2)
                  << chunkResult.metrics.totalExecutionTimeMs << " ms\n";
        std::cout << "Overall throughput : " << std::fixed << std::setprecision(2)
                  << chunkResult.metrics.overallThroughputMBps << " MB/s\n";
        std::cout << "--------------------------------\n";

        if (benchmarkMode) {
            runBenchmark(directoryTree, path);
        }
    }

    return 0;
}