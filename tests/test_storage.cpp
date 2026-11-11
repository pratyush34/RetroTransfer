#include "storage/storage_manager.h"
#include "storage/chunk_storage.h"
#include "storage/chunk_index.h"
#include "storage/chunk_verifier.h"
#include "storage/json_helper.h"
#include "reconstruction_engine.h"
#include "manifest_writer.h"
#include "scanner.h"
#include "chunk_engine.h"
#include "compression_engine.h"
#include "network/transfer_receiver.h"
#include "network/transfer_sender.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace retro::storage;
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

static fs::path getTestBaseDir(const std::string& subDir) {
    fs::path base = fs::current_path() / "build" / "test_storage_tmp" / subDir;
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

// =========================================================================
// Category A: ChunkStorage Tests
// =========================================================================

static void test_store_and_retrieve_new_chunk() {
    fs::path testDir = getTestBaseDir("store_retrieve");
    ChunkStorage storage(testDir.string());
    std::string err;
    TEST_ASSERT(storage.initialize(err), "Storage initialized successfully");

    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', ' ', 'C', 'h', 'u', 'n', 'k', '!'};
    std::string fp = ChunkVerifier::computeFingerprint(data.data(), data.size());
    uint32_t crc = ChunkVerifier::computeCrc32(data.data(), data.size());

    TEST_ASSERT(storage.storeChunk(fp, data.data(), data.size(), crc, err), "Stored new chunk");
    TEST_ASSERT(storage.hasChunk(fp), "Storage contains chunk");

    std::vector<uint8_t> retrieved;
    TEST_ASSERT(storage.retrieveChunk(fp, retrieved, err), "Retrieved stored chunk");
    TEST_ASSERT(retrieved == data, "Retrieved data matches original byte-for-byte");
}

static void test_store_duplicate_and_duplicate_detection() {
    fs::path testDir = getTestBaseDir("duplicate_detection");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    std::vector<uint8_t> data = {'D', 'u', 'p', 'l', 'i', 'c', 'a', 't', 'e', 'T', 'e', 's', 't'};
    std::string fp = ChunkVerifier::computeFingerprint(data.data(), data.size());
    uint32_t crc = ChunkVerifier::computeCrc32(data.data(), data.size());

    TEST_ASSERT(storage.storeChunk(fp, data.data(), data.size(), crc, err), "First store succeeds");
    // Store identical chunk twice
    TEST_ASSERT(storage.storeChunk(fp, data.data(), data.size(), crc, err), "Second store of identical chunk succeeds idempotently");

    // Collision detection: different data claiming same fingerprint
    std::vector<uint8_t> fakeData = {'F', 'a', 'k', 'e', 'C', 'o', 'l', 'l', 'i', 's', 'i', 'o', 'n'};
    uint32_t fakeCrc = ChunkVerifier::computeCrc32(fakeData.data(), fakeData.size());
    bool fakeStoreResult = storage.storeChunk(fp, fakeData.data(), fakeData.size(), fakeCrc, err);
    TEST_ASSERT(!fakeStoreResult, "Collision detected and rejected when data differs");
}

static void test_store_empty_chunk() {
    fs::path testDir = getTestBaseDir("empty_chunk");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    std::vector<uint8_t> emptyData;
    std::string fp = ChunkVerifier::computeFingerprint(emptyData.data(), 0);
    uint32_t crc = ChunkVerifier::computeCrc32(emptyData.data(), 0);

    TEST_ASSERT(storage.storeChunk(fp, emptyData.data(), 0, crc, err), "Stored empty chunk (0 bytes)");
    TEST_ASSERT(storage.hasChunk(fp), "Storage reports empty chunk exists");

    std::vector<uint8_t> retrieved;
    TEST_ASSERT(storage.retrieveChunk(fp, retrieved, err), "Retrieved empty chunk");
    TEST_ASSERT(retrieved.empty(), "Retrieved empty chunk has size 0");
}

static void test_store_binary_with_null_bytes() {
    fs::path testDir = getTestBaseDir("binary_null_bytes");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    std::vector<uint8_t> binaryData = {0x00, 0xFF, 0x00, 0x00, 0x42, 0x00, 0x7E, 0x00, 0x01, 0x00};
    std::string fp = ChunkVerifier::computeFingerprint(binaryData.data(), binaryData.size());
    uint32_t crc = ChunkVerifier::computeCrc32(binaryData.data(), binaryData.size());

    TEST_ASSERT(storage.storeChunk(fp, binaryData.data(), binaryData.size(), crc, err), "Stored binary data with null bytes");
    std::vector<uint8_t> retrieved;
    TEST_ASSERT(storage.retrieveChunk(fp, retrieved, err), "Retrieved binary chunk");
    TEST_ASSERT(retrieved == binaryData, "Binary data with null bytes matches perfectly");
}

static void test_store_large_chunk() {
    fs::path testDir = getTestBaseDir("large_chunk");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    // 1 MiB chunk
    size_t largeSize = 1024 * 1024;
    std::vector<uint8_t> largeData(largeSize);
    for (size_t i = 0; i < largeSize; ++i) {
        largeData[i] = static_cast<uint8_t>((i * 37) & 0xFF);
    }
    std::string fp = ChunkVerifier::computeFingerprint(largeData.data(), largeData.size());
    uint32_t crc = ChunkVerifier::computeCrc32(largeData.data(), largeData.size());

    TEST_ASSERT(storage.storeChunk(fp, largeData.data(), largeData.size(), crc, err), "Stored 1 MiB chunk");
    std::vector<uint8_t> retrieved;
    TEST_ASSERT(storage.retrieveChunk(fp, retrieved, err), "Retrieved 1 MiB chunk");
    TEST_ASSERT(retrieved.size() == largeSize, "Retrieved size matches 1 MiB");
    TEST_ASSERT(retrieved == largeData, "1 MiB data content matches byte-for-byte");
}

static void test_retrieve_nonexistent_chunk() {
    fs::path testDir = getTestBaseDir("nonexistent_chunk");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    std::vector<uint8_t> retrieved;
    std::string fakeFp = "0123456789abcdef0123456789abcdef";
    bool ok = storage.retrieveChunk(fakeFp, retrieved, err);
    TEST_ASSERT(!ok, "Retrieving nonexistent chunk returned false");
    TEST_ASSERT(!storage.hasChunk(fakeFp), "hasChunk reports false for nonexistent chunk");
}

static void test_detect_corrupted_data_and_missing_files() {
    fs::path testDir = getTestBaseDir("corrupted_data");
    ChunkStorage storage(testDir.string());
    std::string err;
    storage.initialize(err);

    std::vector<uint8_t> data = {'C', 'o', 'r', 'r', 'u', 'p', 't', 'i', 'o', 'n', 'T', 'e', 's', 't'};
    std::string fp = ChunkVerifier::computeFingerprint(data.data(), data.size());
    uint32_t crc = ChunkVerifier::computeCrc32(data.data(), data.size());
    storage.storeChunk(fp, data.data(), data.size(), crc, err);

    std::string chunkPath = storage.getChunkPath(fp);
    TEST_ASSERT(fs::exists(chunkPath), "Chunk file exists on disk");

    // Corrupt one byte in the chunk file
    {
        std::fstream file(chunkPath, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(0);
        file.put('X');
    }

    std::vector<uint8_t> retrieved;
    bool retrieveOk = storage.retrieveChunk(fp, retrieved, err);
    TEST_ASSERT(!retrieveOk, "Retrieve fails when chunk file data is corrupted");

    bool validateOk = storage.validateChunk(fp, data.size(), crc, err);
    TEST_ASSERT(!validateOk, "Validate detects corrupted chunk file");

    // Remove chunk file -> detect missing file
    std::error_code ec;
    fs::remove(chunkPath, ec);
    TEST_ASSERT(!storage.hasChunk(fp), "hasChunk reports false after file removal");
    bool validateMissing = storage.validateChunk(fp, data.size(), crc, err);
    TEST_ASSERT(!validateMissing, "Validate detects missing storage file");
}

// =========================================================================
// Category B: ChunkIndex Tests
// =========================================================================

static void test_create_save_load_index() {
    fs::path testDir = getTestBaseDir("index_crud");
    std::string indexPath = (testDir / "chunk_index.json").string();

    ChunkIndex index;
    std::string err;
    TEST_ASSERT(index.initialize(indexPath, err), "Index initialized");
    TEST_ASSERT(index.size() == 0, "Index is initially empty");

    ChunkRecord r1;
    r1.fingerprint = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    r1.size = 100;
    r1.crc32 = 12345;
    r1.filename = r1.fingerprint + ".chunk";
    TEST_ASSERT(index.insertChunk(r1, err), "Inserted record 1");

    ChunkRecord r2;
    r2.fingerprint = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    r2.size = 200;
    r2.crc32 = 67890;
    r2.filename = r2.fingerprint + ".chunk";
    TEST_ASSERT(index.insertChunk(r2, err), "Inserted record 2");

    TEST_ASSERT(index.size() == 2, "Index size is 2");

    // Reload index from disk
    ChunkIndex reloaded;
    TEST_ASSERT(reloaded.loadIndex(indexPath, err), "Loaded index from disk");
    TEST_ASSERT(reloaded.size() == 2, "Reloaded index has 2 records");

    ChunkRecord found;
    TEST_ASSERT(reloaded.findChunk(r1.fingerprint, found), "Found record 1 in reloaded index");
    TEST_ASSERT(found.size == 100 && found.crc32 == 12345, "Record 1 attributes match");
    TEST_ASSERT(reloaded.containsChunk(r2.fingerprint), "containsChunk returns true for record 2");

    // Missing fingerprint lookup
    ChunkRecord missing;
    TEST_ASSERT(!reloaded.findChunk("cccccccccccccccccccccccccccccccc", missing), "findChunk returns false for missing");

    // Duplicate insertion: identical succeeds idempotently
    TEST_ASSERT(reloaded.insertChunk(r1, err), "Inserting identical record succeeds idempotently");

    // Duplicate insertion: collision with different size/crc fails
    ChunkRecord collision = r1;
    collision.size = 999;
    TEST_ASSERT(!reloaded.insertChunk(collision, err), "Conflicting duplicate fingerprint rejected");

    // Remove entry
    TEST_ASSERT(reloaded.removeChunk(r1.fingerprint, err), "Removed record 1");
    TEST_ASSERT(reloaded.size() == 1, "Index size after removal is 1");
    TEST_ASSERT(!reloaded.containsChunk(r1.fingerprint), "Record 1 no longer present in index");
}

static void test_malformed_and_missing_json_index() {
    fs::path testDir = getTestBaseDir("malformed_index");
    std::string indexPath = (testDir / "chunk_index.json").string();

    // 1. Missing JSON file
    ChunkIndex missingIndex;
    std::string err;
    TEST_ASSERT(!missingIndex.loadIndex(testDir.string() + "/nonexistent.json", err), "Loading missing JSON returns error");
    TEST_ASSERT(!err.empty(), "Descriptive error message provided for missing JSON");

    // 2. Malformed JSON syntax (broken braces)
    {
        std::ofstream out(indexPath);
        out << "{ \"chunks\": [ { \"fingerprint\": \"abc\" ... broken json";
    }
    ChunkIndex malformedIndex;
    TEST_ASSERT(!malformedIndex.loadIndex(indexPath, err), "Loading malformed JSON syntax fails safely");
    TEST_ASSERT(malformedIndex.size() == 0, "Index contains 0 records after malformed load");

    // 3. Interrupted index update: safeWriteJsonFile uses .tmp and atomic rename
    // Verify that saving creates complete valid JSON and cleans up any temp file
    ChunkIndex safeIndex;
    safeIndex.initialize(indexPath, err);
    ChunkRecord rec;
    rec.fingerprint = "11111111111111111111111111111111";
    rec.size = 50;
    rec.crc32 = 99;
    rec.filename = rec.fingerprint + ".chunk";
    TEST_ASSERT(safeIndex.insertChunk(rec, err), "Safe insert persists atomically");

    std::string tmpFile = indexPath + ".tmp";
    TEST_ASSERT(!fs::exists(tmpFile), "Temporary file is cleaned up after safe atomic save");
    TEST_ASSERT(fs::exists(indexPath), "Final index file exists and is intact");
}

// =========================================================================
// Category C: Persistence Tests (Close and Reopen)
// =========================================================================

static void test_persistence_across_sessions() {
    fs::path testDir = getTestBaseDir("persistence_lifecycle");
    std::string storagePath = testDir.string();

    std::vector<uint8_t> chunkA = {'C', 'h', 'u', 'n', 'k', 'A', '_', 'D', 'a', 't', 'a'};
    std::vector<uint8_t> chunkB = {'C', 'h', 'u', 'n', 'k', 'B', '_', 'D', 'a', 't', 'a'};

    std::string fpA = ChunkVerifier::computeFingerprint(chunkA.data(), chunkA.size());
    uint32_t crcA = ChunkVerifier::computeCrc32(chunkA.data(), chunkA.size());

    std::string fpB = ChunkVerifier::computeFingerprint(chunkB.data(), chunkB.size());
    uint32_t crcB = ChunkVerifier::computeCrc32(chunkB.data(), chunkB.size());

    // Session 1: Store chunks and close StorageManager
    {
        StorageManager mgr(storagePath);
        std::string err;
        TEST_ASSERT(mgr.initialize(err), "Session 1: StorageManager initialized");
        TEST_ASSERT(mgr.storeChunk(fpA, chunkA.data(), chunkA.size(), crcA, err), "Session 1: Stored Chunk A");
        TEST_ASSERT(mgr.storeChunk(fpB, chunkB.data(), chunkB.size(), crcB, err), "Session 1: Stored Chunk B");
        TEST_ASSERT(mgr.getStoredChunkCount() == 2, "Session 1: Chunk count is 2");
    } // StorageManager goes out of scope ("application closes")

    // Session 2: Reopen StorageManager and verify chunks remain available
    {
        StorageManager mgr(storagePath);
        std::string err;
        TEST_ASSERT(mgr.initialize(err), "Session 2: StorageManager reopened");
        TEST_ASSERT(mgr.getStoredChunkCount() == 2, "Session 2: Both chunks present in index");
        TEST_ASSERT(mgr.hasChunk(fpA), "Session 2: Chunk A exists");
        TEST_ASSERT(mgr.hasChunk(fpB), "Session 2: Chunk B exists");

        std::vector<uint8_t> retrievedA;
        TEST_ASSERT(mgr.getChunk(fpA, retrievedA, err), "Session 2: Retrieved Chunk A");
        TEST_ASSERT(retrievedA == chunkA, "Session 2: Chunk A content matches original");

        std::vector<uint8_t> retrievedB;
        TEST_ASSERT(mgr.getChunk(fpB, retrievedB, err), "Session 2: Retrieved Chunk B");
        TEST_ASSERT(retrievedB == chunkB, "Session 2: Chunk B content matches original");

        TEST_ASSERT(mgr.validateRepository(err), "Session 2: Repository consistency validation passes");
    }
}

// =========================================================================
// Category D: Receiver Integration & End-to-End Tests
// =========================================================================

static void test_receiver_stores_chunks_and_reconstructs() {
    fs::path testBase = getTestBaseDir("receiver_storage_e2e");
    fs::path srcProjectDir = testBase / "source_project";
    fs::path transferDir = testBase / "transfer_bundle";
    fs::path rxStorageDir = testBase / "persistent_storage";
    fs::path rxDestDir = testBase / "reconstructed_project";

    // 1. Create a mock source project with text and binary files
    fs::create_directories(srcProjectDir / "src");
    fs::create_directories(srcProjectDir / "data");

    {
        std::ofstream f(srcProjectDir / "src" / "alpha.cpp");
        f << "#include <iostream>\nint main() { std::cout << \"Alpha\"; return 0; }\n";
    }
    {
        std::ofstream f(srcProjectDir / "src" / "beta.cpp");
        f << "void beta() { int x = 42; (void)x; }\n";
    }
    {
        std::ofstream f(srcProjectDir / "data" / "data.bin", std::ios::binary);
        for (int i = 0; i < 2048; ++i) {
            f.put(static_cast<char>((i * 17) & 0xFF));
        }
    }

    // 2. Scan and Chunk
    ProjectScanner scanner(srcProjectDir.string());
    DirectoryNode tree = scanner.scan();

    EngineConfig engineCfg;
    engineCfg.strategy = ChunkStrategy::Fixed;
    engineCfg.fixedConfig.chunkSize = 512;
    ChunkEngine engine(engineCfg);
    ProjectChunkResult chunkResult = engine.processScannedProject(tree, srcProjectDir.string());

    // 3. Selective Compression & Payload Bundle
    fs::create_directories(transferDir);
    fs::path manifestPath = transferDir / "manifest.json";
    fs::path payloadPath = transferDir / "payload.rtpack";

    CompressionEngine compEngine;
    CompressionResult compResult;
    std::string compError;
    ProjectChunkResult emptyBaseline;
    bool compOk = compEngine.compressMissingChunks(chunkResult, emptyBaseline,
                                                   payloadPath.string(), compResult, compError);
    TEST_ASSERT(compOk, "Compression engine generated payload");
    TEST_ASSERT(fs::exists(payloadPath), "Payload file exists on disk");

    {
        std::ofstream manifestOut(manifestPath);
        retro::writeChunkManifestJson(manifestOut, chunkResult, engineCfg,
                                      srcProjectDir.string(), "",
                                      payloadPath.filename().string(), &compResult);
    }
    TEST_ASSERT(fs::exists(manifestPath), "Manifest JSON file created successfully");

    // 4. Test Chunk Extraction from Payload
    std::vector<ExtractedChunk> extracted;
    std::string extractErr;
    TEST_ASSERT(ReconstructionEngine::extractPayloadChunks(manifestPath.string(), payloadPath.string(), extracted, extractErr),
                "Payload chunk extraction succeeds");
    TEST_ASSERT(!extracted.empty(), "Extracted at least one valid chunk from payload");

    // 5. Run Receiver Transfer with persistent chunk storage enabled
    TcpServer probeServer;
    TEST_ASSERT(probeServer.start(0, "127.0.0.1"), "Probe server for ephemeral port");
    uint16_t port = probeServer.getPort();
    probeServer.stop();

    TransferReceiver receiver;
    ReceiverConfig rCfg;
    rCfg.port = port;
    rCfg.baseStorageDir = (testBase / "rx_projects").string();
    rCfg.autoReconstructDir = rxDestDir.string();
    rCfg.chunkStorageDir = rxStorageDir.string();
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

    std::string txErr;
    TEST_ASSERT(sender.send(sCfg, txErr), "Sender sent project payload successfully");
    rxThread.join();
    TEST_ASSERT(rxErr.empty(), "Receiver finished without error");

    // 6. Verify Persistent Storage on Receiver
    StorageManager rxStorage(rxStorageDir.string());
    std::string sErr;
    TEST_ASSERT(rxStorage.initialize(sErr), "Initialized receiver persistent storage manager");
    TEST_ASSERT(rxStorage.getStoredChunkCount() >= extracted.size(), "All extracted chunks are indexed in storage");

    for (const auto& chk : extracted) {
        TEST_ASSERT(rxStorage.hasChunk(chk.fingerprint), "Storage contains chunk: " + chk.fingerprint.substr(0, 8));
        std::vector<uint8_t> storedData;
        TEST_ASSERT(rxStorage.getChunk(chk.fingerprint, storedData, sErr), "Retrieved chunk from persistent storage");
        TEST_ASSERT(storedData == chk.data, "Stored chunk data matches extracted chunk data");
    }

    // 7. Verify Reconstructed Files Match Original
    TEST_ASSERT(fs::exists(rxDestDir / "src" / "alpha.cpp"), "Reconstructed src/alpha.cpp exists");
    TEST_ASSERT(fs::exists(rxDestDir / "src" / "beta.cpp"), "Reconstructed src/beta.cpp exists");
    TEST_ASSERT(fs::exists(rxDestDir / "data" / "data.bin"), "Reconstructed data/data.bin exists");

    std::ifstream origBin(srcProjectDir / "data" / "data.bin", std::ios::binary);
    std::ifstream recoBin(rxDestDir / "data" / "data.bin", std::ios::binary);
    TEST_ASSERT(std::equal(std::istreambuf_iterator<char>(origBin),
                           std::istreambuf_iterator<char>(),
                           std::istreambuf_iterator<char>(recoBin)),
                "Binary file reconstructed byte-for-byte perfectly");

    // 8. Rebuild Index Test on Stored Chunks
    std::string indexJsonPath = (rxStorageDir / "chunk_index.json").string();
    std::error_code ec;
    fs::remove(indexJsonPath, ec);
    TEST_ASSERT(!fs::exists(indexJsonPath), "Removed chunk_index.json to test rebuild");

    TEST_ASSERT(rxStorage.rebuildIndex(sErr), "Rebuilt index from physical chunk files");
    TEST_ASSERT(rxStorage.getStoredChunkCount() >= extracted.size(), "Rebuilt index contains all chunks");
    TEST_ASSERT(rxStorage.validateRepository(sErr), "Rebuilt repository passes full validation");
}

int main() {
    std::cout << "========================================\n";
    std::cout << " RUNNING PERSISTENT CHUNK STORAGE TESTS \n";
    std::cout << "========================================\n";

    std::cout << "\n--- Category A: Storage Tests ---\n";
    test_store_and_retrieve_new_chunk();
    test_store_duplicate_and_duplicate_detection();
    test_store_empty_chunk();
    test_store_binary_with_null_bytes();
    test_store_large_chunk();
    test_retrieve_nonexistent_chunk();
    test_detect_corrupted_data_and_missing_files();

    std::cout << "\n--- Category B: Index Tests ---\n";
    test_create_save_load_index();
    test_malformed_and_missing_json_index();

    std::cout << "\n--- Category C: Persistence Tests ---\n";
    test_persistence_across_sessions();

    std::cout << "\n--- Category D: Receiver Integration Tests ---\n";
    test_receiver_stores_chunks_and_reconstructs();

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
