#include "network/transfer_sender.h"
#include "network/transfer_receiver.h"
#include "storage/storage_manager.h"
#include "storage/chunk_storage.h"
#include "storage/chunk_verifier.h"
#include "scanner.h"
#include "chunk_engine.h"
#include "compression_engine.h"
#include "manifest_writer.h"
#include "reconstruction_engine.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using namespace retro::net;
using namespace retro::storage;

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

static uint16_t getFreePort() {
    TcpServer s;
    s.start(0, "127.0.0.1");
    uint16_t p = s.getPort();
    s.stop();
    return p;
}

static void createTestProjectTree(const fs::path& baseDir) {
    std::error_code ec;
    fs::create_directories(baseDir / "src" / "core" / "include", ec);
    fs::create_directories(baseDir / "data", ec);
    fs::create_directories(baseDir / "assets" / "icons", ec);

    // 1. Text files
    {
        std::ofstream f(baseDir / "README.md");
        f << "# RetroTransfer Test Project\nIncremental project folder transfer tool.\n";
    }
    {
        std::ofstream f(baseDir / "notes.txt");
        for (int i = 0; i < 200; ++i) {
            f << "Project note entry #" << i << ": testing remote deduplication logic.\n";
        }
    }

    // 2. Source code files in nested directories
    {
        std::ofstream f(baseDir / "src" / "core" / "include" / "model.h");
        f << "#ifndef MODEL_H\n#define MODEL_H\nint getModelVersion();\n#endif\n";
    }
    {
        std::ofstream f(baseDir / "src" / "core" / "model.cpp");
        f << "#include \"include/model.h\"\nint getModelVersion() { return 100; }\n";
        for (int i = 0; i < 300; ++i) {
            f << "int computeHash" << i << "() { return " << (i * 3 + 7) << "; }\n";
        }
    }

    // 3. Binary files (arbitrary bytes and null bytes)
    {
        std::ofstream f(baseDir / "data" / "blob.bin", std::ios::binary);
        std::vector<uint8_t> buf(64 * 1024);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
        }
        f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
    {
        std::ofstream f(baseDir / "data" / "nulls.bin", std::ios::binary);
        std::vector<uint8_t> buf(32 * 1024);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = (i % 5 == 0) ? 0 : static_cast<uint8_t>(i & 0xFF);
        }
        f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
    {
        // Binary with all 256 byte values cycled
        std::ofstream f(baseDir / "assets" / "icons" / "app.png", std::ios::binary);
        std::vector<uint8_t> buf(16 * 1024);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<uint8_t>(i % 256);
        }
        f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }

    // 4. Large file (256 KiB)
    {
        std::ofstream f(baseDir / "data" / "large.dat", std::ios::binary);
        std::vector<uint8_t> buf(256 * 1024);
        for (size_t i = 0; i < buf.size(); ++i) {
            buf[i] = static_cast<uint8_t>((i ^ (i >> 8)) & 0xFF);
        }
        f.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }

    // 5. Empty files
    {
        std::ofstream f(baseDir / "empty.txt");
    }
    {
        std::ofstream f(baseDir / "data" / "empty.dat", std::ios::binary);
    }
}

static bool prepareProjectPackage(const fs::path& projectDir,
                                  const fs::path& outManifest,
                                  const fs::path& outPayload,
                                  ChunkStrategy strategy = ChunkStrategy::Adaptive) {
    ProjectScanner scanner(projectDir.string());
    DirectoryNode tree = scanner.scan();

    EngineConfig engineCfg;
    engineCfg.strategy = strategy;
    engineCfg.fixedConfig.chunkSize = 1024;
    engineCfg.cdcConfig = CDCConfig(512, 1024, 2048);
    ChunkEngine engine(engineCfg);
    ProjectChunkResult chunkResult = engine.processScannedProject(tree, projectDir.string());

    CompressionEngine compEngine;
    CompressionResult compResult;
    std::string compErr;
    ProjectChunkResult emptyBaseline;
    if (!compEngine.compressMissingChunks(chunkResult, emptyBaseline, outPayload.string(), compResult, compErr)) {
        std::cerr << "CompressionEngine failed: " << compErr << "\n";
        return false;
    }

    std::ofstream manifestOut(outManifest);
    if (!manifestOut) return false;
    retro::writeChunkManifestJson(manifestOut, chunkResult, engineCfg,
                                  projectDir.string(), "",
                                  outPayload.filename().string(), &compResult);
    return true;
}

// =========================================================================
// TEST 1: First Transfer
// =========================================================================
static void test1_first_transfer(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 1: First Transfer ---\n";
    fs::path srcDir = rootTestDir / "src_v1";
    fs::path collDir = rootTestDir / "coll_v1";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v1";
    fs::path rxProjects = rootTestDir / "received_projects";

    createTestProjectTree(srcDir);
    fs::create_directories(collDir);

    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";
    TEST_ASSERT(prepareProjectPackage(srcDir, manifestPath, payloadPath), "Packaged project v1");

    uint16_t port = getFreePort();
    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = rxProjects.string();
    rCfg.chunkStorageDir = storageDir.string();
    rCfg.autoReconstructDir = rxDir.string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxErr;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxErr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = port;
    sCfg.verbose = false;
    sCfg.enableRemoteDedup = true;

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender completed first transfer");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    const DedupSummaryMsg& stats = sender.getStatistics();
    TEST_ASSERT(stats.totalProjectChunks > 0, "Project chunks generated > 0");
    TEST_ASSERT(stats.availableChunks == 0, "No chunks initially available on receiver");
    TEST_ASSERT(stats.missingChunks == stats.totalProjectChunks, "All chunks requested as missing");
    TEST_ASSERT(stats.transferredChunks == stats.totalProjectChunks, "All chunks transferred");

    // Verify storage and indexing on receiver
    StorageManager mgr(storageDir.string());
    std::string sErr;
    TEST_ASSERT(mgr.initialize(sErr), "Opened receiver persistent storage");
    TEST_ASSERT(mgr.getStoredChunkCount() == stats.totalProjectChunks, "All chunks indexed in storage");
    TEST_ASSERT(mgr.validateRepository(sErr), "Persistent repository is consistent");

    // Verify reconstruction
    TEST_ASSERT(filesEqual(srcDir / "README.md", rxDir / "README.md"), "README.md matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "notes.txt", rxDir / "notes.txt"), "notes.txt matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "blob.bin", rxDir / "data" / "blob.bin"), "data/blob.bin matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "nulls.bin", rxDir / "data" / "nulls.bin"), "data/nulls.bin matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"), "data/large.dat matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "empty.txt", rxDir / "empty.txt"), "empty.txt matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "empty.dat", rxDir / "data" / "empty.dat"), "data/empty.dat matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "include" / "model.h", rxDir / "src" / "core" / "include" / "model.h"), "model.h matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "model.cpp", rxDir / "src" / "core" / "model.cpp"), "model.cpp matches byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "assets" / "icons" / "app.png", rxDir / "assets" / "icons" / "app.png"), "app.png matches byte-for-byte");
}

// =========================================================================
// TEST 2: Identical Second Transfer
// =========================================================================
static void test2_identical_second_transfer(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 2: Identical Second Transfer ---\n";
    fs::path srcDir = rootTestDir / "src_v1";
    fs::path collDir = rootTestDir / "coll_v1";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v2";
    fs::path rxProjects = rootTestDir / "received_projects";

    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";

    uint16_t port = getFreePort();
    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = rxProjects.string();
    rCfg.chunkStorageDir = storageDir.string();
    rCfg.autoReconstructDir = rxDir.string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxErr;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxErr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = port;
    sCfg.verbose = false;
    sCfg.enableRemoteDedup = true;

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender completed second transfer");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    const DedupSummaryMsg& stats = sender.getStatistics();
    TEST_ASSERT(stats.availableChunks == stats.totalProjectChunks, "All chunks already available on receiver");
    TEST_ASSERT(stats.missingChunks == 0, "0 chunks missing");
    TEST_ASSERT(stats.transferredChunks == 0, "0 chunks transferred");
    TEST_ASSERT(stats.transmittedPayloadBytes == 0, "NO payload bytes transmitted (100% remote dedup)");
    TEST_ASSERT(stats.deduplicatedBytes > 0, "Deduplication savings recorded for all available chunks");

    // Verify reconstruction from persistent storage succeeded byte-for-byte!
    TEST_ASSERT(filesEqual(srcDir / "README.md", rxDir / "README.md"), "README.md reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "notes.txt", rxDir / "notes.txt"), "notes.txt reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "blob.bin", rxDir / "data" / "blob.bin"), "blob.bin reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"), "large.dat reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "model.cpp", rxDir / "src" / "core" / "model.cpp"), "model.cpp reconstructed byte-for-byte");
}

// =========================================================================
// TEST 3: Modified Project
// =========================================================================
static void test3_modified_project(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 3: Modified Project ---\n";
    fs::path srcDir = rootTestDir / "src_v3";
    fs::path collDir = rootTestDir / "coll_v3";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v3";
    fs::path rxProjects = rootTestDir / "received_projects";

    // Copy src_v1 to src_v3
    std::error_code ec;
    fs::copy(rootTestDir / "src_v1", srcDir, fs::copy_options::recursive, ec);

    // Modify a small portion in model.cpp
    {
        std::ofstream f(srcDir / "src" / "core" / "model.cpp", std::ios::app);
        f << "\n// MODIFIED CODE LINE FOR TEST 3\nint newIncrementalHelper() { return 9999; }\n";
    }

    fs::create_directories(collDir);
    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";
    TEST_ASSERT(prepareProjectPackage(srcDir, manifestPath, payloadPath), "Packaged modified project v3");

    uint16_t port = getFreePort();
    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = rxProjects.string();
    rCfg.chunkStorageDir = storageDir.string();
    rCfg.autoReconstructDir = rxDir.string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxErr;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxErr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = port;
    sCfg.verbose = false;
    sCfg.enableRemoteDedup = true;

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender completed modified project transfer");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    const DedupSummaryMsg& stats = sender.getStatistics();
    TEST_ASSERT(stats.availableChunks > 0, "Unmodified chunks reused from persistent storage");
    TEST_ASSERT(stats.missingChunks > 0, "Modified chunks identified as missing");
    TEST_ASSERT(stats.missingChunks < stats.totalProjectChunks, "Only modified chunks transferred (selective transfer)");
    TEST_ASSERT(stats.transferredChunks == stats.missingChunks, "Transferred exactly missing chunk count");

    // Verify reconstructed files match modified project
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "model.cpp", rxDir / "src" / "core" / "model.cpp"),
                "Modified model.cpp reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"),
                "Unmodified large.dat reconstructed byte-for-byte");
}

// =========================================================================
// TEST 4: New File
// =========================================================================
static void test4_new_file(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 4: New File Added ---\n";
    fs::path srcDir = rootTestDir / "src_v4";
    fs::path collDir = rootTestDir / "coll_v4";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v4";
    fs::path rxProjects = rootTestDir / "received_projects";

    std::error_code ec;
    fs::copy(rootTestDir / "src_v3", srcDir, fs::copy_options::recursive, ec);

    // Add a brand new file
    {
        std::ofstream f(srcDir / "src" / "core" / "extra_feature.cpp");
        f << "// Brand new feature added in Test 4\n";
        for (int i = 0; i < 200; ++i) {
            f << "int feature" << i << "() { return " << (i * 101) << "; }\n";
        }
    }
    // Add a new empty file
    {
        std::ofstream f(srcDir / "data" / "new_empty.txt");
    }

    fs::create_directories(collDir);
    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";
    TEST_ASSERT(prepareProjectPackage(srcDir, manifestPath, payloadPath), "Packaged project v4 with new file");

    uint16_t port = getFreePort();
    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = rxProjects.string();
    rCfg.chunkStorageDir = storageDir.string();
    rCfg.autoReconstructDir = rxDir.string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxErr;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxErr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = port;
    sCfg.verbose = false;
    sCfg.enableRemoteDedup = true;

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender completed transfer with new file");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    const DedupSummaryMsg& stats = sender.getStatistics();
    TEST_ASSERT(stats.availableChunks > 0, "Prior existing chunks reused");
    TEST_ASSERT(stats.missingChunks > 0 && stats.missingChunks < stats.totalProjectChunks,
                "Only newly added chunks transferred");

    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "extra_feature.cpp", rxDir / "src" / "core" / "extra_feature.cpp"),
                "New extra_feature.cpp reconstructed byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "data" / "new_empty.txt", rxDir / "data" / "new_empty.txt"),
                "New empty file reconstructed byte-for-byte");
}

// =========================================================================
// TEST 5: Corrupted Stored Chunk
// =========================================================================
static void test5_corrupted_stored_chunk(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 5: Corrupted Stored Chunk Recovery ---\n";
    fs::path srcDir = rootTestDir / "src_v1";
    fs::path collDir = rootTestDir / "coll_v1";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v5";
    fs::path rxProjects = rootTestDir / "received_projects";

    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";

    // Deliberately corrupt one stored chunk in receiver's storage/chunks/
    fs::path chunksDir = storageDir / "chunks";
    bool corruptedOne = false;
    std::string corruptedFp;
    for (const auto& entry : fs::directory_iterator(chunksDir)) {
        if (entry.path().extension() == ".chunk" && entry.file_size() > 16) {
            corruptedFp = entry.path().stem().string();
            // Corrupt the chunk file bytes
            std::fstream chunkFile(entry.path(), std::ios::in | std::ios::out | std::ios::binary);
            std::vector<char> corruptBytes(16, 0x55);
            chunkFile.write(corruptBytes.data(), corruptBytes.size());
            chunkFile.close();
            corruptedOne = true;
            std::cout << "  Deliberately corrupted chunk file: " << corruptedFp.substr(0, 8) << "...\n";
            break;
        }
    }
    TEST_ASSERT(corruptedOne, "Successfully corrupted one chunk file on receiver");

    uint16_t port = getFreePort();
    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = rxProjects.string();
    rCfg.chunkStorageDir = storageDir.string();
    rCfg.autoReconstructDir = rxDir.string();
    rCfg.enableChunkStorage = true;
    rCfg.interactive = false;
    rCfg.verbose = false;
    rCfg.acceptTimeoutMs = 8000;

    ReceivedFiles rxFiles;
    std::string rxErr;
    std::thread rxThread([&]() {
        receiver.receive(rCfg, rxFiles, rxErr);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    TransferSender sender;
    SenderConfig sCfg;
    sCfg.manifestPath = manifestPath.string();
    sCfg.payloadPath = payloadPath.string();
    sCfg.receiverIp = "127.0.0.1";
    sCfg.receiverPort = port;
    sCfg.verbose = false;
    sCfg.enableRemoteDedup = true;

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender retransmitted corrupted chunk");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    const DedupSummaryMsg& stats = sender.getStatistics();
    TEST_ASSERT(stats.missingChunks >= 1, "Corrupted chunk was detected as missing by receiver");
    TEST_ASSERT(stats.transferredChunks >= 1, "Corrupted chunk was retransmitted and repaired");

    // Verify repository consistency after repair
    StorageManager mgr(storageDir.string());
    std::string sErr;
    TEST_ASSERT(mgr.initialize(sErr), "Reopened persistent storage");
    TEST_ASSERT(mgr.validateRepository(sErr), "Repaired repository passes full integrity verification");

    // Verify reconstruction succeeds byte-for-byte
    TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"),
                "Reconstructed large.dat matches original byte-for-byte after chunk repair");
}

// =========================================================================
// TEST 6: Receiver Restart
// =========================================================================
static void test6_receiver_restart(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 6: Receiver Restart and Persistence ---\n";
    fs::path srcDir = rootTestDir / "src_v1";
    fs::path collDir = rootTestDir / "coll_v1";
    fs::path storageDir = rootTestDir / "receiver_storage";
    fs::path rxDir = rootTestDir / "reconstructed_v6";
    fs::path rxProjects = rootTestDir / "received_projects";

    fs::path manifestPath = collDir / "manifest.json";
    fs::path payloadPath = collDir / "payload.rtpack";

    // Simulate complete receiver restart with a fresh receiver instance
    uint16_t port = getFreePort();
    {
        TransferReceiver restartedReceiver; // Brand new receiver instance
        ReceiverConfig rCfg;
        rCfg.port = port;
        rCfg.baseStorageDir = rxProjects.string();
        rCfg.chunkStorageDir = storageDir.string();
        rCfg.autoReconstructDir = rxDir.string();
        rCfg.enableChunkStorage = true;
        rCfg.interactive = false;
        rCfg.verbose = false;
        rCfg.acceptTimeoutMs = 8000;

        ReceivedFiles rxFiles;
        std::string rxErr;
        std::thread rxThread([&]() {
            restartedReceiver.receive(rCfg, rxFiles, rxErr);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        TransferSender sender;
        SenderConfig sCfg;
        sCfg.manifestPath = manifestPath.string();
        sCfg.payloadPath = payloadPath.string();
        sCfg.receiverIp = "127.0.0.1";
        sCfg.receiverPort = port;
        sCfg.verbose = false;
        sCfg.enableRemoteDedup = true;

        std::string txErr;
        TEST_ASSERT(sender.send(sCfg, txErr), "Sender completed transfer to restarted receiver");
        rxThread.join();
        TEST_ASSERT(rxErr.empty(), "Restarted receiver completed successfully");

        const DedupSummaryMsg& stats = sender.getStatistics();
        TEST_ASSERT(stats.availableChunks == stats.totalProjectChunks,
                    "Restarted receiver correctly loaded persistent index and recognized all chunks");
        TEST_ASSERT(stats.missingChunks == 0, "Zero missing chunks on restarted receiver");
        TEST_ASSERT(stats.transmittedPayloadBytes == 0, "No payload bytes transmitted to restarted receiver");

        // Verify reconstruction
        TEST_ASSERT(filesEqual(srcDir / "README.md", rxDir / "README.md"), "README.md matches after restart");
        TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"), "large.dat matches after restart");
    }
}

// =========================================================================
// TEST 7: Reconstruction Verification (Text, Binary, Empty, Large, Nulls, Nested)
// =========================================================================
static void test7_reconstruction_verification(const fs::path& rootTestDir) {
    std::cout << "\n--- TEST 7: Comprehensive Reconstruction Verification ---\n";
    fs::path srcDir = rootTestDir / "src_v1";
    fs::path rxDir = rootTestDir / "reconstructed_v1";

    // 1. Text files
    TEST_ASSERT(filesEqual(srcDir / "README.md", rxDir / "README.md"), "Text file (README.md) verified byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "notes.txt", rxDir / "notes.txt"), "Multi-line text (notes.txt) verified byte-for-byte");

    // 2. Binary files
    TEST_ASSERT(filesEqual(srcDir / "data" / "blob.bin", rxDir / "data" / "blob.bin"), "Binary file (blob.bin) verified byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "assets" / "icons" / "app.png", rxDir / "assets" / "icons" / "app.png"),
                "Binary file (app.png, all 256 byte values) verified byte-for-byte");

    // 3. Embedded null bytes
    TEST_ASSERT(filesEqual(srcDir / "data" / "nulls.bin", rxDir / "data" / "nulls.bin"),
                "Binary with null bytes (nulls.bin) verified byte-for-byte");

    // 4. Large file
    TEST_ASSERT(filesEqual(srcDir / "data" / "large.dat", rxDir / "data" / "large.dat"),
                "Large file (large.dat, 256 KiB) verified byte-for-byte");

    // 5. Empty files
    TEST_ASSERT(filesEqual(srcDir / "empty.txt", rxDir / "empty.txt"), "Empty file (empty.txt, 0 bytes) verified");
    TEST_ASSERT(filesEqual(srcDir / "data" / "empty.dat", rxDir / "data" / "empty.dat"), "Empty binary file (0 bytes) verified");

    // 6. Nested directory structure
    TEST_ASSERT(fs::exists(rxDir / "src" / "core" / "include" / "model.h"), "Nested dir 3 levels deep exists");
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "include" / "model.h", rxDir / "src" / "core" / "include" / "model.h"),
                "Nested header verified byte-for-byte");
    TEST_ASSERT(filesEqual(srcDir / "src" / "core" / "model.cpp", rxDir / "src" / "core" / "model.cpp"),
                "Nested source code verified byte-for-byte");

    std::cout << "[SUCCESS] All 7 test scenarios verified with byte-for-byte equality!\n";
}

int main() {
    std::cout << "============================================================\n";
    std::cout << "        REMOTE DEDUPLICATION INTEGRATION TESTS              \n";
    std::cout << "============================================================\n";

    fs::path testBaseDir = fs::current_path() / "build" / "test_dedup_tmp";
    std::error_code ec;
    fs::remove_all(testBaseDir, ec);
    fs::create_directories(testBaseDir, ec);

    test1_first_transfer(testBaseDir);
    test2_identical_second_transfer(testBaseDir);
    test3_modified_project(testBaseDir);
    test4_new_file(testBaseDir);
    test5_corrupted_stored_chunk(testBaseDir);
    test6_receiver_restart(testBaseDir);
    test7_reconstruction_verification(testBaseDir);

    fs::remove_all(testBaseDir, ec);

    std::cout << "\n============================================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "============================================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
