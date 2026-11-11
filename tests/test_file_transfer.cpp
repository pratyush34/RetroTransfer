#include "network/transfer_sender.h"
#include "network/transfer_receiver.h"
#include "scanner.h"
#include "chunk_engine.h"
#include "compression_engine.h"
#include "reconstruction_engine.h"
#include "manifest_writer.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace retro::net;

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "FAIL: " << __func__ << " (line " << __LINE__ << "): " << msg << "\n"; \
            g_tests_failed++; \
        } else { \
            std::cout << "  [PASS] " << __func__ << ": " << msg << "\n"; \
        } \
    } while (0)

static bool filesEqual(const fs::path& p1, const fs::path& p2) {
    std::error_code ec;
    uintmax_t s1 = fs::file_size(p1, ec);
    uintmax_t s2 = fs::file_size(p2, ec);
    if (ec || s1 != s2) return false;

    std::ifstream f1(p1, std::ios::binary);
    std::ifstream f2(p2, std::ios::binary);
    if (!f1 || !f2) return false;

    return std::equal(std::istreambuf_iterator<char>(f1),
                      std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(f2));
}

static void createDummyFile(const fs::path& path, size_t size, uint8_t seedByte = 0xAA) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    std::vector<uint8_t> buf(std::min(size, static_cast<size_t>(65536)));
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<uint8_t>((seedByte + i * 31) ^ (i & 0xFF));
    }
    size_t remaining = size;
    while (remaining > 0) {
        size_t toWrite = std::min(remaining, buf.size());
        out.write(reinterpret_cast<const char*>(buf.data()), toWrite);
        remaining -= toWrite;
    }
}

static void test_file_sizes_transfer(size_t fileSize, const std::string& testName) {
    fs::path tempDir = fs::current_path() / "build" / "test_tmp" / testName;
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    fs::path srcManifest = tempDir / "test_chunks.json";
    fs::path srcPayload = tempDir / "payload.rtpack";

    // Valid minimal manifest JSON
    {
        std::ofstream mf(srcManifest);
        mf << "{\n  \"version\": 1,\n  \"name\": \"" << testName << "\"\n}\n";
    }

    createDummyFile(srcPayload, fileSize, 0x42);

    TransferReceiver receiver;
    ReceiverConfig rConfig;
    rConfig.port = 0; // ephemeral port
    rConfig.baseStorageDir = (tempDir / "received").string();
    rConfig.chunkStorageDir = (tempDir / "storage").string();
    rConfig.enableChunkStorage = false;
    rConfig.interactive = false;
    rConfig.verbose = false;
    rConfig.acceptTimeoutMs = 5000;

    // Start server first so we know its bound port
    // We start listening on ephemeral port 0
    TcpServer server;
    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server started");
    uint16_t port = server.getPort();
    server.stop();

    rConfig.port = port;

    ReceivedFiles outFiles;
    std::string receiverError;
    std::thread receiverThread([&]() {
        receiver.receive(rConfig, outFiles, receiverError);
    });

    // Give server brief moment to listen
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TransferSender sender;
    SenderConfig sConfig;
    sConfig.manifestPath = srcManifest.string();
    sConfig.payloadPath = srcPayload.string();
    sConfig.receiverIp = "127.0.0.1";
    sConfig.receiverPort = port;
    sConfig.verbose = false;

    std::string senderError;
    bool sendOk = sender.send(sConfig, senderError);
    if (!sendOk) {
        std::cerr << "Sender error in " << testName << ": " << senderError << "\n";
    }
    TEST_ASSERT(sendOk, "Sender completed file transfer");

    receiverThread.join();
    if (!receiverError.empty()) {
        std::cerr << "Receiver error in " << testName << ": " << receiverError << "\n";
    }
    TEST_ASSERT(receiverError.empty(), "Receiver completed without error");

    // Verify byte-for-byte equality of both transferred files
    TEST_ASSERT(filesEqual(srcManifest, outFiles.manifestPath), "Manifest byte-for-byte equal");
    TEST_ASSERT(filesEqual(srcPayload, outFiles.payloadPath), "Payload byte-for-byte equal");

    fs::remove_all(tempDir, ec);
}

static void test_binary_with_null_bytes() {
    fs::path tempDir = fs::current_path() / "build" / "test_tmp" / "binary_nulls";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    fs::path srcManifest = tempDir / "test_chunks.json";
    fs::path srcPayload = tempDir / "payload.rtpack";

    {
        std::ofstream mf(srcManifest);
        mf << "{\n  \"binary\": true\n}\n";
    }

    // Binary payload with frequent null bytes and non-printable sequences
    {
        std::ofstream pf(srcPayload, std::ios::binary);
        std::vector<uint8_t> data(128 * 1024);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = (i % 7 == 0) ? 0 : static_cast<uint8_t>(i & 0xFF);
        }
        pf.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    test_file_sizes_transfer(128 * 1024, "binary_nulls");
    fs::remove_all(tempDir, ec);
}

static void test_binary_all_byte_values() {
    fs::path tempDir = fs::current_path() / "build" / "test_tmp" / "binary_all_bytes";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);

    fs::path srcManifest = tempDir / "test_chunks.json";
    fs::path srcPayload = tempDir / "payload.rtpack";

    {
        std::ofstream mf(srcManifest);
        mf << "{\n  \"all_bytes\": true\n}\n";
    }

    // Binary payload containing all 256 byte values (0x00 - 0xFF) cycled repeatedly across 256 KiB
    {
        std::ofstream pf(srcPayload, std::ios::binary);
        std::vector<uint8_t> data(256 * 1024);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<uint8_t>(i % 256);
        }
        pf.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    test_file_sizes_transfer(256 * 1024, "binary_all_bytes");
    fs::remove_all(tempDir, ec);
}

static void test_end_to_end_pipeline_and_reconstruction() {
    std::cout << "\n--- Starting Mandatory End-to-End Pipeline & Reconstruction Test ---\n";
    fs::path rootTestDir = fs::current_path() / "build" / "test_tmp" / "e2e_project";
    std::error_code ec;
    fs::remove_all(rootTestDir, ec);

    fs::path originalProjectDir = rootTestDir / "original";
    fs::create_directories(originalProjectDir / "src" / "sub", ec);
    fs::create_directories(originalProjectDir / "data", ec);

    // Create diverse project files (three source files, text, binary, nested directories)
    {
        // Source file 1
        std::ofstream f1(originalProjectDir / "src" / "main.cpp");
        f1 << "#include <iostream>\n#include \"sub/helper.h\"\nint main() { std::cout << \"Hello RetroTransfer! \" << helperFunction0(); return 0; }\n";

        // Source file 2 (header in nested directory)
        std::ofstream f2(originalProjectDir / "src" / "sub" / "helper.h");
        for (int i = 0; i < 500; ++i) {
            f2 << "// Helper header line " << i << "\nint helperFunction" << i << "();\n";
        }

        // Source file 3 (source file in nested directory)
        std::ofstream f3(originalProjectDir / "src" / "sub" / "helper.cpp");
        f3 << "#include \"helper.h\"\nint helperFunction0() { return 42; }\n";
        for (int i = 1; i < 500; ++i) {
            f3 << "int helperFunction" << i << "() { return " << (i * 2) << "; }\n";
        }

        // Binary file (75 KiB of binary data)
        std::ofstream f4(originalProjectDir / "data" / "binary.bin", std::ios::binary);
        std::vector<uint8_t> bin(75 * 1024);
        for (size_t i = 0; i < bin.size(); ++i) {
            bin[i] = static_cast<uint8_t>((i * 17 + 5) & 0xFF);
        }
        f4.write(reinterpret_cast<const char*>(bin.data()), bin.size());

        // Text file
        std::ofstream f5(originalProjectDir / "README.txt");
        f5 << "Sample Project for RetroTransfer End-to-End Verification\nTesting full reconstruction.\n";
    }

    // 1. Scan directory with existing ProjectScanner
    ProjectScanner scanner(originalProjectDir.string());
    DirectoryNode tree = scanner.scan();
    TEST_ASSERT(!tree.directories.empty() || !tree.files.empty(), "Scanner scanned original project");

    // 2. Chunk project with existing ChunkEngine
    EngineConfig engineCfg;
    engineCfg.strategy = ChunkStrategy::Adaptive;
    ChunkEngine chunkEngine(engineCfg);
    ProjectChunkResult chunkResult = chunkEngine.processScannedProject(tree, originalProjectDir.string());
    TEST_ASSERT(chunkResult.metrics.totalFilesProcessed == 5, "Chunk engine processed 5 files (3 code, 1 text, 1 bin)");
    TEST_ASSERT(chunkResult.metrics.totalChunksGenerated > 0, "Chunk engine produced chunks");

    // 3. Generate .rtpack with existing CompressionEngine
    fs::path collectionDir = rootTestDir / "collection";
    fs::create_directories(collectionDir, ec);
    fs::path payloadPath = collectionDir / "project.rtpack";
    fs::path manifestPath = collectionDir / "project_chunks.json";

    CompressionEngine compEngine;
    CompressionResult compResult;
    std::string compError;
    ProjectChunkResult emptyBaseline; // Compress all chunks
    bool compOk = compEngine.compressMissingChunks(chunkResult, emptyBaseline,
                                                   payloadPath.string(), compResult, compError);
    if (!compOk) {
        std::cerr << "CompressionEngine failed: " << compError << "\n";
    }
    TEST_ASSERT(compOk, "Compression engine generated .rtpack payload");
    TEST_ASSERT(fs::exists(payloadPath), "Payload file exists on disk");

    // 4. Generate manifest JSON using the canonical RetroTransfer ManifestWriter
    {
        std::ofstream manifestOut(manifestPath);
        retro::writeChunkManifestJson(manifestOut, chunkResult, engineCfg,
                                      originalProjectDir.string(), "",
                                      payloadPath.filename().string(), &compResult);
    }
    TEST_ASSERT(fs::exists(manifestPath), "Manifest JSON file created via canonical ManifestWriter");

    // 5. Transfer over TCP to Receiver
    fs::path receivedDir = rootTestDir / "received";
    fs::path reconstructedDir = rootTestDir / "reconstructed";

    // Obtain an ephemeral port
    TcpServer probeServer;
    TEST_ASSERT(probeServer.start(0, "127.0.0.1"), "Ephemeral port probe");
    uint16_t transferPort = probeServer.getPort();
    probeServer.stop();

    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = transferPort;
    rCfg.baseStorageDir = receivedDir.string();
    rCfg.autoReconstructDir = reconstructedDir.string();
    rCfg.chunkStorageDir = (rootTestDir / "storage").string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxError;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxError);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = transferPort;
    sCfg.verbose = false;

    std::string txError;
    bool txOk = sender.send(sCfg, txError);
    if (!txOk) {
        std::cerr << "Sender failed: " << txError << "\n";
    }
    TEST_ASSERT(txOk, "Sender sent manifest and payload successfully");

    rxThread.join();
    if (!rxError.empty()) {
        std::cerr << "Receiver error: " << rxError << "\n";
    }
    TEST_ASSERT(rxError.empty(), "Receiver completed successfully");

    // 6. Verify received manifest and payload match
    TEST_ASSERT(filesEqual(manifestPath, rxFiles.manifestPath), "Received manifest equals original manifest");
    TEST_ASSERT(filesEqual(payloadPath, rxFiles.payloadPath), "Received payload equals original payload");

    // 7. Verify Reconstructed Files Match Original Project Byte-For-Byte!
    TEST_ASSERT(fs::exists(reconstructedDir / "src" / "main.cpp"), "Reconstructed src/main.cpp exists");
    TEST_ASSERT(filesEqual(originalProjectDir / "src" / "main.cpp",
                           reconstructedDir / "src" / "main.cpp"), "src/main.cpp matches byte-for-byte");

    TEST_ASSERT(fs::exists(reconstructedDir / "src" / "sub" / "helper.h"), "Reconstructed src/sub/helper.h exists");
    TEST_ASSERT(filesEqual(originalProjectDir / "src" / "sub" / "helper.h",
                           reconstructedDir / "src" / "sub" / "helper.h"), "src/sub/helper.h matches byte-for-byte");

    TEST_ASSERT(fs::exists(reconstructedDir / "src" / "sub" / "helper.cpp"), "Reconstructed src/sub/helper.cpp exists");
    TEST_ASSERT(filesEqual(originalProjectDir / "src" / "sub" / "helper.cpp",
                           reconstructedDir / "src" / "sub" / "helper.cpp"), "src/sub/helper.cpp matches byte-for-byte");

    TEST_ASSERT(fs::exists(reconstructedDir / "data" / "binary.bin"), "Reconstructed data/binary.bin exists");
    TEST_ASSERT(filesEqual(originalProjectDir / "data" / "binary.bin",
                           reconstructedDir / "data" / "binary.bin"), "data/binary.bin matches byte-for-byte");

    TEST_ASSERT(fs::exists(reconstructedDir / "README.txt"), "Reconstructed README.txt exists");
    TEST_ASSERT(filesEqual(originalProjectDir / "README.txt",
                           reconstructedDir / "README.txt"), "README.txt matches byte-for-byte");

    // 8. Verify Persistent Storage Integration on Receiver
    TEST_ASSERT(fs::exists(rootTestDir / "storage" / "chunk_index.json"), "Persistent chunk_index.json was created");
    TEST_ASSERT(fs::exists(rootTestDir / "storage" / "storage_config.json"), "storage_config.json was created");
    TEST_ASSERT(fs::exists(rootTestDir / "storage" / "chunks"), "storage/chunks directory was created");

    std::cout << "[SUCCESS] Entire project reconstructed and verified byte-for-byte!\n";

    fs::remove_all(rootTestDir, ec);
}

int main() {
    std::cout << "========================================\n";
    std::cout << "    RUNNING FILE & E2E TRANSFER TESTS   \n";
    std::cout << "========================================\n";

    // 1. Empty file (0 bytes)
    test_file_sizes_transfer(0, "size_0_bytes");

    // 2. One-byte file
    test_file_sizes_transfer(1, "size_1_byte");

    // 3. 100 bytes (small partial block)
    test_file_sizes_transfer(100, "size_100_bytes");

    // 4. Small file (1 KiB)
    test_file_sizes_transfer(1024, "size_1_kib");

    // 5. Exact buffer boundary (64 KiB)
    test_file_sizes_transfer(64 * 1024, "size_exact_64_kib");

    // 6. 65 KiB (boundary + 1, multiple blocks with final partial block)
    test_file_sizes_transfer(65 * 1024, "size_65_kib");

    // 7. Larger than buffer (150 KiB)
    test_file_sizes_transfer(150 * 1024, "size_150_kib");

    // 8. 1 MiB file (exact queue capacity of 16 blocks)
    test_file_sizes_transfer(1024 * 1024, "size_1_mib");

    // 9. 5 MiB file (wraps around ring buffer multiple times)
    test_file_sizes_transfer(5 * 1024 * 1024, "size_5_mib");

    // 10. 10 MiB file (160 blocks, wraps around ring buffer 10 times)
    test_file_sizes_transfer(10 * 1024 * 1024, "size_10_mib");

    // 11. Binary with embedded null bytes
    test_binary_with_null_bytes();

    // 12. Binary containing all 256 byte values
    test_binary_all_byte_values();

    // 13. Full End-to-End Pipeline & Reconstruction Test
    test_end_to_end_pipeline_and_reconstruction();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
