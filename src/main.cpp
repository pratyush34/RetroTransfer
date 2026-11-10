#include "scanner.h"
#include "chunk_engine.h"
#include "chunk_config.h"
#include "compression_engine.h"
#include "reconstruction_engine.h"
#include "manifest_writer.h"
#include "network/transfer_sender.h"
#include "network/transfer_receiver.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using retro::jsonEscape;
using retro::writeChunkManifestJson;

static bool parseSizeArgument(const std::string& text, size_t& value) {
    if (text.empty()) return false;
    for (char rawCharacter : text) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (!std::isdigit(character)) return false;
    }
    try {
        const unsigned long long parsed = std::stoull(text);
        if (parsed == 0 || parsed > static_cast<unsigned long long>(
                std::numeric_limits<size_t>::max()) ||
            parsed > static_cast<unsigned long long>(
                std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        // A chunk is held in memory while it is fingerprinted. Keep CLI settings
        // bounded so a typo cannot request an effectively unbounded allocation.
        constexpr unsigned long long MAX_CHUNK_BYTES = 1024ULL * 1024ULL * 1024ULL;
        if (parsed > MAX_CHUNK_BYTES) return false;
        value = static_cast<size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static bool normalizeJsonFilename(std::string& filename) {
    if (filename.empty()) return false;

    const fs::path path(filename);
    if (path.is_absolute() || path.has_parent_path() ||
        path.filename() == "." || path.filename() == "..") {
        return false;
    }

    if (path.extension().empty()) {
        filename += ".json";
        return true;
    }

    std::string extension = path.extension().string();
    for (char& character : extension) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension == ".json";
}

static bool normalizePayloadFilename(std::string& filename) {
    if (filename.empty()) return false;

    const fs::path path(filename);
    if (path.is_absolute() || path.has_parent_path() ||
        path.filename() == "." || path.filename() == "..") {
        return false;
    }

    if (path.extension().empty()) {
        filename += ".rtpack";
        return true;
    }

    std::string extension = path.extension().string();
    for (char& character : extension) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension == ".rtpack";
}

static bool sameOutputPath(const fs::path& first, const fs::path& second) {
    std::string firstName = first.lexically_normal().generic_string();
    std::string secondName = second.lexically_normal().generic_string();
    if (firstName.size() != secondName.size()) return false;
    return std::equal(firstName.begin(), firstName.end(), secondName.begin(),
                      [](unsigned char left, unsigned char right) {
                          return std::tolower(left) == std::tolower(right);
                      });
}

static bool filesHaveSameContents(const fs::path& first, const fs::path& second) {
    std::error_code firstError;
    std::error_code secondError;
    const uintmax_t firstSize = fs::file_size(first, firstError);
    const uintmax_t secondSize = fs::file_size(second, secondError);
    if (firstError || secondError || firstSize != secondSize) return false;

    std::ifstream firstFile(first, std::ios::binary);
    std::ifstream secondFile(second, std::ios::binary);
    if (!firstFile || !secondFile) return false;
    return std::equal(std::istreambuf_iterator<char>(firstFile),
                      std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(secondFile),
                      std::istreambuf_iterator<char>());
}

static fs::path findIdenticalJsonFile(const fs::path& directory,
                                      const fs::path& candidate,
                                      const fs::path& excludedPath,
                                      std::error_code& error) {
    fs::directory_iterator iterator(directory, error);
    const fs::directory_iterator end;
    if (error) return {};

    while (iterator != end) {
        const fs::directory_entry entry = *iterator;
        if (!entry.is_regular_file(error)) {
            if (error) return {};
        } else {
            std::string extension = entry.path().extension().string();
            for (char& character : extension) {
                character = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character)));
            }
            if (!sameOutputPath(entry.path(), candidate) &&
                !sameOutputPath(entry.path(), excludedPath) &&
                extension == ".json" &&
                filesHaveSameContents(entry.path(), candidate)) {
                return entry.path();
            }
        }
        iterator.increment(error);
        if (error) return {};
    }
    return {};
}

static void writeIndent(std::ostream& output, int depth) {
    output << std::string(static_cast<size_t>(depth * 2), ' ');
}

static void writeDirectory(std::ostream& output, const DirectoryNode& directory, int depth) {
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

static void printUsage(const char* progName) {
    std::cout << "RetroTransfer - Incremental Project Transfer and Chunking System\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " send --manifest <manifest.json> --payload <payload.rtpack> --ip <receiver_ip> [--port <port>]\n";
    std::cout << "  " << progName << " receive [--port <port>] [--output <directory>] [--auto-reconstruct]\n\n";
    std::cout << "  " << progName << " [directory] [scan.json] [options]\n";
    std::cout << "  " << progName << " --reconstruct <chunks.json> --output <directory> [--payload <file.rtpack>]\n\n";
    std::cout << "Network Commands:\n";
    std::cout << "  send                  Stream manifest and payload bundle to remote receiver over TCP\n";
    std::cout << "    --manifest <file>   Path to project chunk manifest JSON\n";
    std::cout << "    --payload <file>    Path to .rtpack payload bundle\n";
    std::cout << "    --ip <address>      Receiver IP address or hostname\n";
    std::cout << "    --port <port>       Receiver port (default: 8080)\n";
    std::cout << "    --timeout <ms>      Connection timeout in ms (default: 10000)\n\n";
    std::cout << "  receive               Run TCP receiver server to accept and store project transfer\n";
    std::cout << "    --port <port>       Port to listen on (default: 8080)\n";
    std::cout << "    --output <dir>      Destination directory for immediate reconstruction\n";
    std::cout << "    --auto-reconstruct  Reconstruct without interactive confirmation\n";
    std::cout << "    --base-dir <dir>    Storage directory for received projects (default: \"received_projects\")\n\n";
    std::cout << "Local Chunking Arguments:\n";
    std::cout << "  [directory]           Root directory to scan (default: \".\")\n";
    std::cout << "  [scan.json]           Legacy positional scan filename inside collection/\n\n";
    std::cout << "Options:\n";
    std::cout << "  --scan-output <name>  Scan JSON filename (default: \"scan.json\")\n";
    std::cout << "  --chunks-output <name> Chunk manifest filename (default: <scan>_chunks.json)\n";
    std::cout << "  --baseline <directory> Compare against a local receiver/project copy\n";
    std::cout << "  --compress-all        Package all unique chunks without a baseline\n";
    std::cout << "  --payload-output <name> Compressed bundle (requires --baseline or --compress-all)\n";
    std::cout << "  --reconstruct <manifest> Rebuild files from a chunk manifest and payload\n";
    std::cout << "  --payload <file.rtpack> Payload override (otherwise loaded beside manifest)\n";
    std::cout << "  --output <directory>  Reconstruction destination; existing files are updated in place\n";
    std::cout << "  --strategy, -s <mode> Chunking strategy: \"adaptive\" (default), \"fixed\", or \"cdc\"\n";
    std::cout << "  --chunk-size <bytes>  Fixed chunk size in bytes (default: 65536)\n";
    std::cout << "  --min-chunk <bytes>   CDC minimum chunk size (default: 16384)\n";
    std::cout << "  --target-chunk <bytes> CDC target chunk size (default: 65536)\n";
    std::cout << "  --max-chunk <bytes>   CDC maximum chunk size (default: 262144)\n";
    std::cout << "  --benchmark           Run repeatable benchmark comparing Fixed, CDC, and Adaptive\n";
    std::cout << "  --no-chunk            Only perform project scan, skip chunking stage\n";
    std::cout << "  --help, -h            Show this help message\n";
}

static void runBenchmark(const DirectoryNode& directoryTree,
                         const std::string& path,
                         const EngineConfig& baseConfig) {
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
        double dedupRatio;
        double totalTimeMs;
        double throughputMBps;
        uint64_t metadataBytes;
    };

    std::vector<BenchRow> rows;

    for (auto strat : strategies) {
        EngineConfig cfg = baseConfig;
        cfg.strategy = strat;
        ChunkEngine engine(cfg);

        ProjectChunkResult res = engine.processScannedProject(directoryTree, path);
        BenchRow row;
        row.name = strategy_to_string(strat);
        row.chunks = res.metrics.totalChunksGenerated;
        row.uniqueFp = res.metrics.uniqueFingerprints;
        row.dedupRatio = res.metrics.internalDedupRatio * 100.0;
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

static int handleSendCommand(int argc, char* argv[]) {
    retro::net::SenderConfig config;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: RetroTransfer send --manifest <manifest.json> --payload <payload.rtpack> --ip <receiver_ip> [--port <port>]\n";
            return 0;
        } else if (arg == "--manifest") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --manifest requires a path argument.\n";
                return 2;
            }
            config.manifestPath = argv[++i];
        } else if (arg == "--payload") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --payload requires a path argument.\n";
                return 2;
            }
            config.payloadPath = argv[++i];
        } else if (arg == "--ip") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --ip requires an IP address or hostname.\n";
                return 2;
            }
            config.receiverIp = argv[++i];
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --port requires a port number.\n";
                return 2;
            }
            try {
                int p = std::stoi(argv[++i]);
                if (p <= 0 || p > 65535) {
                    std::cerr << "Error: Port must be between 1 and 65535.\n";
                    return 2;
                }
                config.receiverPort = static_cast<uint16_t>(p);
            } catch (...) {
                std::cerr << "Error: Invalid port number: " << argv[i] << "\n";
                return 2;
            }
        } else if (arg == "--timeout") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --timeout requires milliseconds value.\n";
                return 2;
            }
            config.connectionTimeoutMs = std::stoi(argv[++i]);
        } else {
            std::cerr << "Error: Unrecognized send option: " << arg << "\n";
            return 2;
        }
    }

    if (config.manifestPath.empty()) {
        std::cerr << "Error: --manifest is required for sending.\n";
        return 2;
    }
    if (config.payloadPath.empty()) {
        std::cerr << "Error: --payload is required for sending.\n";
        return 2;
    }
    if (config.receiverIp.empty()) {
        std::cerr << "Error: --ip is required for sending.\n";
        return 2;
    }

    retro::net::TransferSender sender;
    std::string error;
    if (!sender.send(config, error)) {
        std::cerr << "Error: File transfer failed: " << error << "\n";
        return 1;
    }

    return 0;
}

static int handleReceiveCommand(int argc, char* argv[]) {
    retro::net::ReceiverConfig config;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: RetroTransfer receive [--port <port>] [--output <directory>] [--auto-reconstruct] [--base-dir <dir>]\n";
            return 0;
        } else if (arg == "--port") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --port requires a port number.\n";
                return 2;
            }
            try {
                int p = std::stoi(argv[++i]);
                if (p <= 0 || p > 65535) {
                    std::cerr << "Error: Port must be between 1 and 65535.\n";
                    return 2;
                }
                config.port = static_cast<uint16_t>(p);
            } catch (...) {
                std::cerr << "Error: Invalid port number: " << argv[i] << "\n";
                return 2;
            }
        } else if (arg == "--output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --output requires a directory path.\n";
                return 2;
            }
            config.autoReconstructDir = argv[++i];
        } else if (arg == "--auto-reconstruct") {
            config.interactive = false;
        } else if (arg == "--base-dir") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --base-dir requires a directory path.\n";
                return 2;
            }
            config.baseStorageDir = argv[++i];
        } else {
            std::cerr << "Error: Unrecognized receive option: " << arg << "\n";
            return 2;
        }
    }

    retro::net::TransferReceiver receiver;
    retro::net::ReceivedFiles files;
    std::string error;
    if (!receiver.receive(config, files, error)) {
        std::cerr << "Error: Receiver failed: " << error << "\n";
        return 1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "send" || std::string(argv[1]) == "--send")) {
        return handleSendCommand(argc, argv);
    }
    if (argc > 1 && (std::string(argv[1]) == "receive" || std::string(argv[1]) == "--receive")) {
        return handleReceiveCommand(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "--reconstruct") {
        if (argc < 3 || std::string(argv[2]).rfind("--", 0) == 0) {
            std::cerr << "Error: --reconstruct requires a chunk manifest path.\n";
            return 2;
        }
        const std::string manifestPath = argv[2];
        std::string payloadPath;
        std::string outputPath;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            if ((argument == "--payload" || argument == "--output") &&
                i + 1 >= argc) {
                std::cerr << "Error: " << argument << " requires a value.\n";
                return 2;
            }
            if (argument == "--payload") {
                payloadPath = argv[++i];
            } else if (argument == "--output") {
                outputPath = argv[++i];
            } else {
                std::cerr << "Error: Unrecognized reconstruction option: "
                          << argument << "\n";
                return 2;
            }
        }
        if (outputPath.empty()) {
            std::cerr << "Error: --reconstruct requires --output <directory>.\n";
            return 2;
        }
        ReconstructionEngine reconstruction;
        ReconstructionResult result;
        std::string error;
        if (!reconstruction.reconstruct(manifestPath, payloadPath, outputPath,
                                        result, error)) {
            std::cerr << "Error: Reconstruction failed: " << error << "\n";
            return 1;
        }
        std::cout << "Reconstructed files: " << result.filesWritten << "\n";
        std::cout << "Chunks from payload: " << result.chunksWritten << "\n";
        std::cout << "Chunks reused      : " << result.chunksReused << "\n";
        std::cout << "Output directory   : " << fs::absolute(outputPath) << "\n";
        return 0;
    }

    std::string path = ".";
    std::string scanOutputName = "scan.json";
    std::string chunksOutputName;
    std::string baselinePath;
    std::string payloadOutputName;
    bool compressAllChunks = false;
    bool scanOutputSpecified = false;
    bool chunksOutputSpecified = false;
    bool payloadOutputSpecified = false;
    bool enableChunking = true;
    bool benchmarkMode = false;

    EngineConfig config;

    // Parse command-line arguments and fail clearly on malformed values.
    int positionalIdx = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--no-chunk") {
            enableChunking = false;
        } else if (arg == "--benchmark") {
            benchmarkMode = true;
        } else if (arg == "--baseline") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --baseline requires a directory path.\n";
                return 2;
            }
            baselinePath = argv[++i];
        } else if (arg == "--compress-all") {
            compressAllChunks = true;
        } else if (arg == "--payload-output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --payload-output requires a .rtpack filename.\n";
                return 2;
            }
            payloadOutputName = argv[++i];
            if (!normalizePayloadFilename(payloadOutputName)) {
                std::cerr << "Error: --payload-output requires a .rtpack filename without a directory.\n";
                return 2;
            }
            payloadOutputSpecified = true;
        } else if (arg == "--scan-output" || arg == "--chunks-output") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a JSON filename.\n";
                return 2;
            }
            std::string& outputName = arg == "--scan-output"
                ? scanOutputName
                : chunksOutputName;
            outputName = argv[++i];
            if (!normalizeJsonFilename(outputName)) {
                std::cerr << "Error: " << arg
                          << " requires a filename ending in .json (directories are not allowed).\n";
                return 2;
            }
            if (arg == "--scan-output") {
                scanOutputSpecified = true;
            } else {
                chunksOutputSpecified = true;
            }
        } else if (arg == "--strategy" || arg == "-s") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a strategy value.\n";
                return 2;
            }
            const std::string strategyText = argv[++i];
            if (!string_to_strategy(strategyText, config.strategy)) {
                std::cerr << "Error: Unknown strategy '" << strategyText
                          << "'. Choose adaptive, fixed, or cdc.\n";
                return 2;
            }
        } else if (arg == "--chunk-size" || arg == "--min-chunk" ||
                   arg == "--target-chunk" || arg == "--max-chunk") {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " requires a positive byte count (max 1 GiB).\n";
                return 2;
            }
            size_t parsed = 0;
            const std::string valueText = argv[++i];
            if (!parseSizeArgument(valueText, parsed)) {
                std::cerr << "Error: Invalid value '" << valueText << "' for " << arg
                          << ". Use a positive integer up to 1073741824 bytes.\n";
                return 2;
            }
            if (arg == "--chunk-size") {
                config.fixedConfig.chunkSize = parsed;
            } else if (arg == "--min-chunk") {
                config.cdcConfig.minChunkSize = parsed;
                config.cdcConfig.update_mask();
            } else if (arg == "--target-chunk") {
                config.cdcConfig.targetChunkSize = parsed;
                config.cdcConfig.update_mask();
            } else {
                config.cdcConfig.maxChunkSize = parsed;
            }
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Error: Unrecognized option: " << arg << "\n";
            return 2;
        } else {
            if (positionalIdx == 0) {
                path = arg;
                ++positionalIdx;
            } else if (positionalIdx == 1) {
                if (scanOutputSpecified) {
                    std::cerr << "Error: Specify the scan output either positionally or with --scan-output, not both.\n";
                    return 2;
                }
                scanOutputName = arg;
                if (!normalizeJsonFilename(scanOutputName)) {
                    std::cerr << "Error: Scan output must be a JSON filename without a directory.\n";
                    return 2;
                }
                scanOutputSpecified = true;
                ++positionalIdx;
            } else {
                std::cerr << "Error: Unexpected positional argument: " << arg << "\n";
                return 2;
            }
        }
    }

    if (!config.fixedConfig.isValid() || !config.cdcConfig.isValid()) {
        std::cerr << "Error: Invalid chunk configuration. Require 0 < min <= target <= max; "
                     "all sizes must be at most 1 GiB.\n";
        return 2;
    }

    if (!baselinePath.empty() && !enableChunking) {
        std::cerr << "Error: --baseline requires chunking; remove --no-chunk.\n";
        return 2;
    }
    if (compressAllChunks && !baselinePath.empty()) {
        std::cerr << "Error: --compress-all cannot be combined with --baseline.\n";
        return 2;
    }
    const bool selectiveCompressionEnabled =
        compressAllChunks || !baselinePath.empty();
    if (compressAllChunks && !enableChunking) {
        std::cerr << "Error: --compress-all requires chunking; remove --no-chunk.\n";
        return 2;
    }
    if (payloadOutputSpecified && !selectiveCompressionEnabled) {
        std::cerr << "Error: --payload-output requires --baseline or --compress-all.\n";
        return 2;
    }

    std::error_code rootError;
    if (!fs::exists(path, rootError) || rootError || !fs::is_directory(path, rootError) || rootError) {
        std::cerr << "Error: Scan root is not an accessible directory: " << path << "\n";
        return 1;
    }
    if (!baselinePath.empty()) {
        rootError.clear();
        if (!fs::exists(baselinePath, rootError) || rootError ||
            !fs::is_directory(baselinePath, rootError) || rootError) {
            std::cerr << "Error: Baseline is not an accessible directory: "
                      << baselinePath << "\n";
            return 1;
        }
    }

    const fs::path collectionFolder = "collection";
    const fs::path outputPath = collectionFolder / scanOutputName;
    if (!chunksOutputSpecified) {
        chunksOutputName = outputPath.stem().string() + "_chunks.json";
    }
    const fs::path chunkOutputPath = collectionFolder / chunksOutputName;
    if (selectiveCompressionEnabled && !payloadOutputSpecified) {
        payloadOutputName = outputPath.stem().string() + "_payload.rtpack";
    }
    const fs::path payloadOutputPath = collectionFolder / payloadOutputName;
    if (enableChunking && sameOutputPath(outputPath, chunkOutputPath)) {
        std::cerr << "Error: Scan and chunk manifest filenames must be different.\n";
        return 2;
    }
    if (enableChunking && selectiveCompressionEnabled &&
        (sameOutputPath(outputPath, payloadOutputPath) ||
         sameOutputPath(chunkOutputPath, payloadOutputPath))) {
        std::cerr << "Error: Payload filename must differ from scan and chunk manifest filenames.\n";
        return 2;
    }

    // 1. Project Scanner Stage
    std::cout << "[Stage 1/2] Scanning directory: " << path << " ...\n";
    ProjectScanner scanner(path);
    const DirectoryNode directoryTree = scanner.scan();

    std::error_code outputDirError;
    fs::create_directories(collectionFolder, outputDirError);
    if (outputDirError) {
        std::cerr << "Error: Could not create output directory " << collectionFolder
                  << ": " << outputDirError.message() << "\n";
        return 1;
    }
    const bool outputPreviouslyExisted = fs::exists(outputPath);
    std::ofstream output(outputPath);
    if (!output) {
        std::cerr << "Error: Could not create output file: " << outputPath << "\n";
        return 1;
    }
    writeDirectory(output, directoryTree, 0);
    output << "\n";
    output.close();
    if (!output) {
        std::cerr << "Error: Failed while writing scan output: " << outputPath << "\n";
        return 1;
    }

    std::error_code duplicateCheckError;
    const fs::path duplicateScan = outputPreviouslyExisted
        ? fs::path{}
        : findIdenticalJsonFile(collectionFolder, outputPath, chunkOutputPath,
                                duplicateCheckError);
    if (duplicateCheckError) {
        std::cerr << "Error: Could not check for duplicate scan files in "
                  << collectionFolder << ": " << duplicateCheckError.message() << "\n";
        return 1;
    }
    if (!duplicateScan.empty()) {
        std::error_code removeError;
        if (!fs::remove(outputPath, removeError) || removeError) {
            std::cerr << "Error: Could not remove newly created duplicate scan "
                      << outputPath << ": " << removeError.message() << "\n";
            return 1;
        }
        std::cout << "Scan is identical to existing file " << duplicateScan
                  << "; skipped creating duplicate " << outputPath << "\n";
    } else {
        std::cout << "Saved scan data to " << outputPath << "\n";
    }

    // 2. Chunking Engine Stage
    if (enableChunking) {
        std::cout << "[Stage 2/2] Running Chunking Engine (Mode: " 
                  << strategy_to_string(config.strategy) << ") ...\n";

        ChunkEngine engine(config);
        ProjectChunkResult chunkResult = engine.processScannedProject(directoryTree, path);

        CompressionResult compressionResult;
        const CompressionResult* compression = nullptr;
        if (selectiveCompressionEnabled) {
            std::cout << (compressAllChunks
                ? "[Stage 3/3] Compressing all unique project chunks...\n"
                : "[Stage 3/3] Comparing baseline chunks and compressing missing chunks...\n");
            ProjectChunkResult baselineResult;
            if (!baselinePath.empty()) {
                ProjectScanner baselineScanner(baselinePath);
                const DirectoryNode baselineTree = baselineScanner.scan();
                baselineResult =
                    engine.processScannedProject(baselineTree, baselinePath);
            }

            CompressionEngine compressor;
            std::string compressionError;
            if (!compressor.compressMissingChunks(chunkResult, baselineResult,
                                                  payloadOutputPath.string(),
                                                  compressionResult,
                                                  compressionError)) {
                std::cerr << "Error: Selective compression failed: "
                          << compressionError << "\n";
                return 1;
            }
            compression = &compressionResult;
            const CompressionMetrics& metrics = compressionResult.metrics;
            std::cout << "Reusable chunks    : " << metrics.reusableChunks << "\n";
            std::cout << "Missing chunks     : " << metrics.missingChunks << "\n";
            std::cout << "Repeated missing   : " << metrics.duplicateChunks
                      << " (" << metrics.duplicateBytesAvoided << " bytes omitted)\n";
            std::cout << "Huffman / raw      : " << metrics.huffmanChunks << " / "
                      << metrics.storedRawChunks << "\n";
            std::cout << "Compression saved  : "
                      << metrics.compressionBytesSaved << " bytes\n";
            std::cout << "Total bytes avoided: " << metrics.totalBytesAvoided
                      << " bytes\n";
            std::cout << "Saved payload      : " << payloadOutputPath << "\n";
        }

        // Save chunk manifest
        std::ofstream chunkOut(chunkOutputPath);
        if (!chunkOut) {
            std::cerr << "Error: Could not create chunk manifest: " << chunkOutputPath << "\n";
            return 1;
        }
        writeChunkManifestJson(chunkOut, chunkResult, config, path, baselinePath,
                               payloadOutputName, compression);
        chunkOut.close();
        if (!chunkOut) {
            std::cerr << "Error: Failed while writing chunk manifest: " << chunkOutputPath << "\n";
            return 1;
        }
        std::cout << "Saved chunk manifest to " << chunkOutputPath << "\n";

        std::cout << "\n--- Chunking Engine Summary ---\n";
        std::cout << "Files processed    : " << chunkResult.metrics.totalFilesProcessed << "\n";
        std::cout << "Total bytes        : " << chunkResult.metrics.totalBytesProcessed << " bytes ("
                  << std::fixed << std::setprecision(2)
                  << (static_cast<double>(chunkResult.metrics.totalBytesProcessed) / (1024.0 * 1024.0)) << " MB)\n";
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
            runBenchmark(directoryTree, path, config);
        }
    }

    return 0;
}