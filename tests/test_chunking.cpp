#include "scanner.h"
#include "fingerprint_c.h"
#include "huffman_codec_c.h"
#include "chunk_config.h"
#include "chunk_metadata.h"
#include "metrics.h"
#include "fixed_chunker.h"
#include "cdc_chunker.h"
#include "adaptive_policy.h"
#include "chunk_engine.h"
#include "compression_engine.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string fingerprintHex(const uint8_t* data,
                                  size_t size,
                                  uint32_t seed = 42) {
    char fingerprint[33];
    rt_compute_fingerprint_hex(data, size, seed, fingerprint);
    return fingerprint;
}

static bool huffmanCompress(const std::vector<uint8_t>& input,
                            std::vector<uint8_t>& output,
                            std::string& error) {
    output.clear();
    error.clear();
    uint8_t* encoded = nullptr;
    size_t encodedSize = 0;
    char errorBuffer[512] = {};
    if (!rt_huffman_compress(input.data(), input.size(), &encoded, &encodedSize,
                             errorBuffer, sizeof(errorBuffer))) {
        error = errorBuffer;
        return false;
    }
    if (encodedSize != 0) {
        output.assign(encoded, encoded + encodedSize);
    }
    std::free(encoded);
    return true;
}

static bool huffmanDecompress(const std::vector<uint8_t>& input,
                              uint64_t originalSize,
                              std::vector<uint8_t>& output,
                              std::string& error) {
    output.clear();
    error.clear();
    uint8_t* decoded = nullptr;
    size_t decodedSize = 0;
    char errorBuffer[512] = {};
    if (!rt_huffman_decompress(input.data(), input.size(), originalSize,
                               &decoded, &decodedSize, errorBuffer,
                               sizeof(errorBuffer))) {
        error = errorBuffer;
        return false;
    }
    if (decodedSize != 0) {
        output.assign(decoded, decoded + decodedSize);
    }
    std::free(decoded);
    return true;
}

// Lightweight test framework
static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "  FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            g_tests_failed++; \
            return; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        std::cout << "[TEST] Running " #fn "... "; \
        g_tests_run++; \
        int failed_before = g_tests_failed; \
        fn(); \
        if (g_tests_failed == failed_before) { \
            std::cout << "PASSED\n"; \
        } \
    } while (0)

// Helper to create temporary test files
static const fs::path TEST_DIR = "test_scratch_retro_transfer";

static void setupTestDir() {
    fs::remove_all(TEST_DIR);
    fs::create_directories(TEST_DIR);
}

static void cleanupTestDir() {
    fs::remove_all(TEST_DIR);
}

static fs::path createTestFile(const std::string& name, const std::vector<uint8_t>& data) {
    fs::path p = TEST_DIR / name;
    std::ofstream out(p, std::ios::binary);
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    out.close();
    return p;
}

static fs::path createTestFileText(const std::string& name, const std::string& content) {
    std::vector<uint8_t> data(content.begin(), content.end());
    return createTestFile(name, data);
}

// 1. Empty file
static void test_empty_file() {
    fs::path p = createTestFile("empty.bin", {});

    FixedChunker fixedChunker;
    std::vector<ChunkMetadata> fixedChunks;
    FileChunkMetrics fixedMetrics;
    bool okFixed = fixedChunker.chunkFile(p.string(), "empty.bin", fixedChunks, fixedMetrics);
    TEST_ASSERT(okFixed, "FixedChunker failed on empty file");
    TEST_ASSERT(fixedChunks.empty(), "FixedChunker must produce 0 chunks for empty file");
    TEST_ASSERT(fixedMetrics.fileSize == 0, "Empty file metrics size must be 0");
    TEST_ASSERT(fixedMetrics.chunkCount == 0, "Empty file metrics chunk count must be 0");

    CDCChunker cdcChunker;
    std::vector<ChunkMetadata> cdcChunks;
    FileChunkMetrics cdcMetrics;
    bool okCDC = cdcChunker.chunkFile(p.string(), "empty.bin", cdcChunks, cdcMetrics);
    TEST_ASSERT(okCDC, "CDCChunker failed on empty file");
    TEST_ASSERT(cdcChunks.empty(), "CDCChunker must produce 0 chunks for empty file");
    TEST_ASSERT(cdcMetrics.fileSize == 0, "CDC empty file size must be 0");
}

// 2. One-byte file
static void test_one_byte_file() {
    fs::path p = createTestFile("one_byte.bin", {'X'});

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    bool ok = fixed.chunkFile(p.string(), "one_byte.bin", chunks, metrics);
    TEST_ASSERT(ok, "FixedChunker failed on 1-byte file");
    TEST_ASSERT(chunks.size() == 1, "Expected exactly 1 chunk for 1-byte file");
    TEST_ASSERT(chunks[0].offset == 0, "Offset should be 0");
    TEST_ASSERT(chunks[0].length == 1, "Length should be 1");
    TEST_ASSERT(chunks[0].sequenceNumber == 0, "Sequence should be 0");
    TEST_ASSERT(!chunks[0].fingerprint.empty(), "Fingerprint must not be empty");

    // Test CDC
    CDCChunker cdc;
    std::vector<ChunkMetadata> cdcChunks;
    FileChunkMetrics cdcMetrics;
    bool cdcOk = cdc.chunkFile(p.string(), "one_byte.bin", cdcChunks, cdcMetrics);
    TEST_ASSERT(cdcOk, "CDCChunker failed on 1-byte file");
    TEST_ASSERT(cdcChunks.size() == 1, "CDC expected exactly 1 chunk for 1-byte file");
    TEST_ASSERT(cdcChunks[0].length == 1, "CDC chunk length should be 1");
}

// 3. File smaller than chunk size
static void test_smaller_than_chunk_size() {
    std::vector<uint8_t> data(300, 0x42);
    fs::path p = createTestFile("small.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    fixed.chunkFile(p.string(), "small.bin", chunks, metrics);
    TEST_ASSERT(chunks.size() == 1, "Expected 1 chunk for file smaller than chunk size");
    TEST_ASSERT(chunks[0].length == 300, "Chunk length must equal file size");
    TEST_ASSERT(chunks[0].offset == 0, "Chunk offset must be 0");
}

// 4. File exactly equal to chunk size
static void test_exact_chunk_size() {
    std::vector<uint8_t> data(1024, 0x33);
    fs::path p = createTestFile("exact.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    fixed.chunkFile(p.string(), "exact.bin", chunks, metrics);
    TEST_ASSERT(chunks.size() == 1, "Expected exactly 1 chunk for exact chunk size");
    TEST_ASSERT(chunks[0].offset == 0, "Offset must be 0");
    TEST_ASSERT(chunks[0].length == 1024, "Length must be 1024");
}

// 5. File exactly divisible by chunk size
static void test_exactly_divisible_by_chunk_size() {
    std::vector<uint8_t> data(2048, 0x77);
    fs::path p = createTestFile("divisible.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    fixed.chunkFile(p.string(), "divisible.bin", chunks, metrics);
    TEST_ASSERT(chunks.size() == 2, "Expected exactly 2 chunks for 2048 bytes with 1024 chunk size");
    TEST_ASSERT(chunks[0].offset == 0 && chunks[0].length == 1024, "Chunk 0 bounds mismatch");
    TEST_ASSERT(chunks[1].offset == 1024 && chunks[1].length == 1024, "Chunk 1 bounds mismatch");
    TEST_ASSERT(chunks[0].sequenceNumber == 0 && chunks[1].sequenceNumber == 1, "Sequence numbers mismatch");
}

// 6. File not divisible by chunk size
static void test_not_divisible_by_chunk_size() {
    std::vector<uint8_t> data(2500, 0x55);
    fs::path p = createTestFile("non_divisible.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    fixed.chunkFile(p.string(), "non_divisible.bin", chunks, metrics);
    TEST_ASSERT(chunks.size() == 3, "Expected 3 chunks for 2500 bytes with 1024 chunk size");
    TEST_ASSERT(chunks[0].offset == 0 && chunks[0].length == 1024, "Chunk 0 bounds mismatch");
    TEST_ASSERT(chunks[1].offset == 1024 && chunks[1].length == 1024, "Chunk 1 bounds mismatch");
    TEST_ASSERT(chunks[2].offset == 2048 && chunks[2].length == 452, "Chunk 2 (partial) bounds mismatch");
}

// 7. Binary data with embedded null bytes
static void test_binary_data_embedded_nulls() {
    std::vector<uint8_t> binData = {0x00, 0x11, 0x00, 0x00, 0x22, 0x00, 0x33, 0x00, 0x00, 0x44};
    std::vector<uint8_t> binData2 = {0x00, 0x11, 0x00, 0x00, 0x22, 0x00, 0x33, 0x00, 0x00, 0x55}; // differs at end

    std::string fp1 = fingerprintHex(binData.data(), binData.size());
    std::string fp2 = fingerprintHex(binData2.data(), binData2.size());

    TEST_ASSERT(!fp1.empty(), "Fingerprint 1 must not be empty");
    TEST_ASSERT(fp1.length() == 32, "Fingerprint must be 32 hex chars");
    TEST_ASSERT(fp1 != fp2, "Binary inputs with different null positions or bytes must produce different fingerprints");

    // Test payload reread
    fs::path p = createTestFile("nulls.bin", binData);
    std::vector<uint8_t> reread;
    bool ok = ChunkEngine::readChunkPayload(p.string(), 0, binData.size(), reread);
    TEST_ASSERT(ok, "readChunkPayload failed on binary file with nulls");
    TEST_ASSERT(reread == binData, "Reread data must exactly match binary original including nulls");
}

// CRC32 must match the standard IEEE 802.3 known-answer vector.
static void test_crc32_standard_vector() {
    const std::string sample = "123456789";
    const uint32_t crc = rt_compute_crc32(
        reinterpret_cast<const uint8_t*>(sample.data()), sample.size());
    TEST_ASSERT(crc == 0xCBF43926U, "CRC32 known-answer vector mismatch");
}

static void test_c_murmurhash_hex_matches_binary_result() {
    const std::vector<uint8_t> input = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x42
    };
    constexpr uint32_t seed = 12345;
    const RtFingerprint128 cHash =
        rt_murmurhash3_128(input.data(), input.size(), seed);

    char cHex[33] = {};
    rt_compute_fingerprint_hex(input.data(), input.size(), seed, cHex);
    char expected[33];
    std::snprintf(expected, sizeof(expected), "%016llx%016llx",
                  static_cast<unsigned long long>(cHash.high),
                  static_cast<unsigned long long>(cHash.low));
    TEST_ASSERT(std::string(cHex) == expected,
                "C MurmurHash3 hex does not match its binary result");
}

// 8. Identical files producing identical fingerprints
static void test_identical_files_produce_identical_fingerprints() {
    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    fs::path p1 = createTestFile("fileA.bin", data);
    fs::path p2 = createTestFile("fileB.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 1024;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks1, chunks2;
    FileChunkMetrics m1, m2;
    fixed.chunkFile(p1.string(), "fileA.bin", chunks1, m1);
    fixed.chunkFile(p2.string(), "fileB.bin", chunks2, m2);

    TEST_ASSERT(chunks1.size() == chunks2.size(), "Identical files must have same chunk count");
    for (size_t i = 0; i < chunks1.size(); ++i) {
        TEST_ASSERT(chunks1[i].fingerprint == chunks2[i].fingerprint, "Fingerprint mismatch on identical chunk");
        TEST_ASSERT(chunks1[i].crc32 == chunks2[i].crc32, "CRC32 mismatch on identical chunk");
    }
}

// 9. Changed contents produce different fingerprints
static void test_changed_contents_produce_different_fingerprints() {
    std::vector<uint8_t> data1 = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> data2 = {1, 2, 3, 4, 5, 6, 7, 9}; // 1 bit diff

    std::string fp1 = fingerprintHex(data1.data(), data1.size());
    std::string fp2 = fingerprintHex(data2.data(), data2.size());
    TEST_ASSERT(fp1 != fp2, "Single bit change must yield completely different fingerprint");
}

// 10. Correct fixed-size offsets and lengths
static void test_correct_offsets_and_lengths() {
    std::vector<uint8_t> data(5500);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>((i * 7) & 0xFF);

    fs::path p = createTestFile("offsets.bin", data);
    FixedChunkConfig cfg;
    cfg.chunkSize = 1500;
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    fixed.chunkFile(p.string(), "offsets.bin", chunks, m);

    TEST_ASSERT(chunks.size() == 4, "Expected 4 chunks for 5500 bytes with 1500 chunk size");
    uint64_t totalLength = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        TEST_ASSERT(chunks[i].offset == i * 1500, "Chunk offset incorrect");
        TEST_ASSERT(chunks[i].sequenceNumber == i, "Sequence number incorrect");
        totalLength += chunks[i].length;

        // Verify each chunk can be reread from disk and matches its fingerprint
        std::vector<uint8_t> payload;
        bool readOk = ChunkEngine::readChunkPayload(p.string(), chunks[i].offset, chunks[i].length, payload);
        TEST_ASSERT(readOk, "readChunkPayload failed");
        std::string fpReread = fingerprintHex(payload.data(), payload.size());
        TEST_ASSERT(fpReread == chunks[i].fingerprint, "Reread chunk fingerprint does not match metadata");
    }
    TEST_ASSERT(totalLength == 5500, "Sum of chunk lengths must equal file size");
}

// 11. CDC minimum chunk-size behavior
static void test_cdc_min_chunk_size_behavior() {
    // Generate pseudo-random data
    std::vector<uint8_t> data(50000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
    }
    fs::path p = createTestFile("cdc_min.bin", data);

    CDCConfig cfg(512, 1024, 4096);
    CDCChunker cdc(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    cdc.chunkFile(p.string(), "cdc_min.bin", chunks, m);

    TEST_ASSERT(!chunks.empty(), "CDC must produce chunks");
    for (size_t i = 0; i + 1 < chunks.size(); ++i) {
        TEST_ASSERT(chunks[i].length >= 512, 
                    "All CDC chunks (except possibly last) must be >= minChunkSize");
    }
}

// 12. CDC maximum chunk-size behavior
static void test_cdc_max_chunk_size_behavior() {
    // Repeated pattern where hash boundary might never trigger
    std::vector<uint8_t> data(30000, 0x00);
    fs::path p = createTestFile("cdc_max.bin", data);

    CDCConfig cfg(256, 1024, 2048);
    CDCChunker cdc(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    cdc.chunkFile(p.string(), "cdc_max.bin", chunks, m);

    TEST_ASSERT(!chunks.empty(), "CDC must produce chunks");
    for (const auto& chunk : chunks) {
        TEST_ASSERT(chunk.length <= 2048, "No CDC chunk must exceed maxChunkSize");
    }
}

// 13. Deterministic CDC output
static void test_cdc_deterministic_output() {
    std::vector<uint8_t> data(40000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>((i * 101) & 0xFF);

    CDCConfig cfg(512, 2048, 8192);
    CDCChunker cdc(cfg);

    std::vector<ChunkMetadata> c1, c2;
    FileChunkMetrics m1, m2;

    cdc.chunkBuffer(data.data(), data.size(), "test.bin", c1, m1);
    cdc.chunkBuffer(data.data(), data.size(), "test.bin", c2, m2);

    TEST_ASSERT(c1.size() == c2.size(), "CDC runs must produce identical chunk count");
    for (size_t i = 0; i < c1.size(); ++i) {
        TEST_ASSERT(c1[i].offset == c2[i].offset, "CDC offset mismatch");
        TEST_ASSERT(c1[i].length == c2[i].length, "CDC length mismatch");
        TEST_ASSERT(c1[i].fingerprint == c2[i].fingerprint, "CDC fingerprint mismatch");
    }
}

// 14. CDC must actually use content boundaries instead of always cutting at max size.
static void test_cdc_content_boundaries_trigger() {
    std::mt19937 rng(123456U);
    std::vector<uint8_t> data(1024 * 1024);
    for (auto& byte : data) {
        byte = static_cast<uint8_t>(rng() & 0xFFU);
    }

    CDCConfig cfg(1024, 4096, 16384);
    CDCChunker cdc(cfg);
    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    const bool ok = cdc.chunkBuffer(data.data(), data.size(), "random.bin", chunks, metrics);
    TEST_ASSERT(ok, "CDC failed to chunk deterministic pseudo-random data");
    TEST_ASSERT(chunks.size() > 1, "CDC should split a large input into multiple chunks");

    bool foundContentBoundary = false;
    uint64_t expectedOffset = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        TEST_ASSERT(chunks[i].offset == expectedOffset, "CDC chunk offsets must be contiguous");
        TEST_ASSERT(chunks[i].length > 0 && chunks[i].length <= cfg.maxChunkSize,
                    "CDC chunk size must be positive and no larger than maxChunkSize");
        if (i + 1 < chunks.size() && chunks[i].length < cfg.maxChunkSize) {
            foundContentBoundary = true;
        }
        expectedOffset += chunks[i].length;
    }
    TEST_ASSERT(expectedOffset == data.size(), "CDC chunks must cover all input bytes");
    TEST_ASSERT(foundContentBoundary,
                "CDC should find content-defined boundaries before hitting maxChunkSize");

    const fs::path filePath = createTestFile("cdc_random_compare.bin", data);
    std::vector<ChunkMetadata> fileChunks;
    FileChunkMetrics fileMetrics;
    CDCChunker tinyReadBufferCdc(cfg, 7);
    const bool fileOk = tinyReadBufferCdc.chunkFile(filePath.string(), "random.bin", fileChunks, fileMetrics);
    TEST_ASSERT(fileOk, "CDC file path failed on the same input with a tiny I/O buffer");
    TEST_ASSERT(tinyReadBufferCdc.getIoBufferSize() == 7, "CDC I/O buffer size setting was not retained");
    TEST_ASSERT(fileChunks.size() == chunks.size(), "CDC file and buffer paths must agree on chunk count");
    for (size_t i = 0; i < chunks.size(); ++i) {
        TEST_ASSERT(fileChunks[i].offset == chunks[i].offset &&
                    fileChunks[i].length == chunks[i].length &&
                    fileChunks[i].fingerprint == chunks[i].fingerprint,
                    "CDC file and buffer paths must produce matching metadata");
    }
}

// 15. Scanner skips generated output and does not follow directory symlinks.
static void test_scanner_skips_generated_dirs_and_symlinks() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "collection");
    fs::create_directories(TEST_DIR / "build-scratch");
    createTestFileText("visible.txt", "scan me");
    createTestFileText("collection/generated.json", "generated output");
    createTestFileText("build-scratch/generated.cpp", "generated build output");

    std::error_code linkError;
    const fs::path absoluteRoot = fs::absolute(TEST_DIR);
    fs::create_directory_symlink(absoluteRoot, TEST_DIR / "cycle", linkError);

    ProjectScanner scanner(TEST_DIR.string());
    DirectoryNode root = scanner.scan();
    TEST_ASSERT(root.files.size() == 1, "Scanner should include the visible file only");
    TEST_ASSERT(root.files[0].fileName == "visible.txt", "Visible file missing from scan");
    TEST_ASSERT(root.directories.empty(), "Generated collection and symlink directories should be skipped");
}

// 16. Invalid chunk sizes and configuration
static void test_invalid_configurations() {
    FixedChunkConfig badFixed;
    badFixed.chunkSize = 0;
    TEST_ASSERT(!badFixed.isValid(), "Fixed chunkSize 0 must be invalid");
    FixedChunker fixed(badFixed);
    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    TEST_ASSERT(!fixed.chunkBuffer(nullptr, 0, "test", chunks, m), "Chunking with invalid config must fail");

    FixedChunker validFixed;
    TEST_ASSERT(!validFixed.chunkBuffer(nullptr, 1, "test", chunks, m),
                "Fixed chunker must reject null input for non-empty data");
    CDCChunker validCDC;
    TEST_ASSERT(!validCDC.chunkBuffer(nullptr, 1, "test", chunks, m),
                "CDC chunker must reject null input for non-empty data");

    CDCConfig badCDC1(0, 1024, 2048);
    TEST_ASSERT(!badCDC1.isValid(), "CDC min 0 must be invalid");

    CDCConfig badCDC2(2048, 1024, 4096);
    TEST_ASSERT(!badCDC2.isValid(), "CDC min > target must be invalid");

    CDCConfig badCDC3(512, 4096, 2048);
    TEST_ASSERT(!badCDC3.isValid(), "CDC target > max must be invalid");

    CDCConfig modifiedAfterConstruction;
    modifiedAfterConstruction.targetChunkSize = 1024;
    CDCChunker recomputesMask(modifiedAfterConstruction);
    TEST_ASSERT(recomputesMask.getConfig().mask == 1023,
                "CDC chunker should recompute a stale mask from targetChunkSize");

    FileChunkMetrics reusedMetrics;
    reusedMetrics.fingerprintThroughputMBps = 999.0;
    reusedMetrics.totalThroughputMBps = 999.0;
    std::vector<ChunkMetadata> emptyChunks;
    TEST_ASSERT(validFixed.chunkBuffer(nullptr, 0, "empty", emptyChunks, reusedMetrics),
                "Fixed chunker should accept an empty null buffer");
    TEST_ASSERT(reusedMetrics.fingerprintThroughputMBps == 0.0 &&
                reusedMetrics.totalThroughputMBps == 0.0,
                "Empty-input metrics must reset throughput instead of retaining stale values");
}

// 15. Missing or unreadable files
static void test_missing_unreadable_file() {
    FixedChunker fixed;
    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    bool ok = fixed.chunkFile("C:/non_existent_folder_xyz/no_file.dat", "no_file.dat", chunks, m);
    TEST_ASSERT(!ok, "FixedChunker must return false for missing file");

    CDCChunker cdc;
    bool cdcOk = cdc.chunkFile("C:/non_existent_folder_xyz/no_file.dat", "no_file.dat", chunks, m);
    TEST_ASSERT(!cdcOk, "CDCChunker must return false for missing file");
}

// 16. Large files processed incrementally
static void test_large_file_incremental() {
    const size_t fileSize = 1024 * 1024; // 1 MB
    std::vector<uint8_t> data(fileSize);
    for (size_t i = 0; i < fileSize; ++i) data[i] = static_cast<uint8_t>((i ^ 0xAA) & 0xFF);

    fs::path p = createTestFile("large.bin", data);

    FixedChunkConfig cfg;
    cfg.chunkSize = 64 * 1024; // 64 KiB
    FixedChunker fixed(cfg);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics m;
    bool ok = fixed.chunkFile(p.string(), "large.bin", chunks, m);
    TEST_ASSERT(ok, "Large file chunking failed");
    TEST_ASSERT(chunks.size() == 16, "1 MB / 64 KB must produce exactly 16 chunks");
    TEST_ASSERT(m.totalChunkBytes == fileSize, "Total chunk bytes must equal 1 MB");
}

// 17. Adaptive mode heuristics
static void test_adaptive_mode_heuristics() {
    AdaptivePolicy policy;
    FixedChunkConfig fixedCfg;
    CDCConfig cdcCfg(16384, 65536, 262144);

    // Sub-chunk size file (< 16 KiB)
    FileInfo smallFile;
    smallFile.fileName = "small.txt";
    smallFile.filePath = "/path/small.txt";
    smallFile.fileSize = 4096;
    smallFile.fileType = ".txt";
    auto decSmall = policy.decide(smallFile, ChunkStrategy::Adaptive, fixedCfg, cdcCfg);
    TEST_ASSERT(decSmall.selectedStrategy == ChunkStrategy::Fixed, "Small file must select Fixed strategy");
    TEST_ASSERT(!decSmall.reason.empty(), "Reason must be recorded");

    // Pre-compressed / high entropy archive
    FileInfo zipFile;
    zipFile.fileName = "bundle.zip";
    zipFile.filePath = "/path/bundle.zip";
    zipFile.fileSize = 1000000;
    zipFile.fileType = ".zip";
    auto decZip = policy.decide(zipFile, ChunkStrategy::Adaptive, fixedCfg, cdcCfg);
    TEST_ASSERT(decZip.selectedStrategy == ChunkStrategy::Fixed, "Zip file must select Fixed strategy");

    // Source code file
    FileInfo cppFile;
    cppFile.fileName = "main.cpp";
    cppFile.filePath = "/path/main.cpp";
    cppFile.fileSize = 50000;
    cppFile.fileType = ".cpp";
    auto decCpp = policy.decide(cppFile, ChunkStrategy::Adaptive, fixedCfg, cdcCfg);
    TEST_ASSERT(decCpp.selectedStrategy == ChunkStrategy::CDC, "C++ source file must select CDC strategy");
}

// 18. Explicit mode overrides
static void test_explicit_mode_overrides() {
    AdaptivePolicy policy;
    FixedChunkConfig fixedCfg;
    CDCConfig cdcCfg;

    FileInfo cppFile;
    cppFile.fileName = "main.cpp";
    cppFile.filePath = "/path/main.cpp";
    cppFile.fileSize = 50000;
    cppFile.fileType = ".cpp";

    // Override with Fixed
    auto decOverrideFixed = policy.decide(cppFile, ChunkStrategy::Fixed, fixedCfg, cdcCfg);
    TEST_ASSERT(decOverrideFixed.selectedStrategy == ChunkStrategy::Fixed, "Override with Fixed failed");
    TEST_ASSERT(decOverrideFixed.wasOverridden, "wasOverridden flag should be true");

    FileInfo zipFile;
    zipFile.fileName = "archive.zip";
    zipFile.filePath = "/path/archive.zip";
    zipFile.fileSize = 1000000;
    zipFile.fileType = ".zip";

    // Override with CDC
    auto decOverrideCDC = policy.decide(zipFile, ChunkStrategy::CDC, fixedCfg, cdcCfg);
    TEST_ASSERT(decOverrideCDC.selectedStrategy == ChunkStrategy::CDC, "Override with CDC failed");
    TEST_ASSERT(decOverrideCDC.wasOverridden, "wasOverridden flag should be true");
}

// 21. Payload reread and direct byte comparison
static void test_payload_reread_and_byte_comparison() {
    std::vector<uint8_t> dataA = {10, 20, 30, 40};
    std::vector<uint8_t> dataB = {10, 20, 30, 40};
    fs::path pA = createTestFile("verifA.bin", dataA);
    fs::path pB = createTestFile("verifB.bin", dataB);

    std::string fpA = fingerprintHex(dataA.data(), dataA.size());
    std::string fpB = fingerprintHex(dataB.data(), dataB.size());
    TEST_ASSERT(fpA == fpB, "Fingerprints match");

    // Candidate match verification: reread payloads and compare byte-for-byte
    std::vector<uint8_t> readA, readB;
    bool okA = ChunkEngine::readChunkPayload(pA.string(), 0, dataA.size(), readA);
    bool okB = ChunkEngine::readChunkPayload(pB.string(), 0, dataB.size(), readB);
    TEST_ASSERT(okA && okB, "readChunkPayload failed");
    TEST_ASSERT(readA == readB, "Byte-by-byte content verification succeeded");
}

// 22. Version comparison metrics
static void test_version_comparison_metrics() {
    std::vector<ChunkMetadata> v1Chunks = {
        {"file.cpp", 0, 1000, "fp1", "cdc", 0, 0},
        {"file.cpp", 1000, 1000, "fp2", "cdc", 1, 0},
        {"file.cpp", 2000, 1000, "fp3", "cdc", 2, 0}
    };

    // v2 has fp1, a modified block fp_new, and fp3
    std::vector<ChunkMetadata> v2Chunks = {
        {"file.cpp", 0, 1000, "fp1", "cdc", 0, 0},
        {"file.cpp", 1000, 1000, "fp_modified", "cdc", 1, 0},
        {"file.cpp", 2000, 1000, "fp3", "cdc", 2, 0}
    };

    VersionComparisonMetrics comp = ChunkEngine::compareVersions(v1Chunks, v2Chunks);
    TEST_ASSERT(comp.matchingChunks == 2, "Expected 2 matching chunks");
    TEST_ASSERT(comp.newChunks == 1, "Expected 1 new chunk");
    TEST_ASSERT(comp.reusableBytes == 2000, "Expected 2000 reusable bytes");
    TEST_ASSERT(comp.transferredBytesNeeded == 1000, "Expected 1000 new bytes transferred");
    TEST_ASSERT(comp.reuseRatio > 0.66 && comp.reuseRatio < 0.67, "Reuse ratio should be approx 66.7%");
}

static void test_huffman_round_trip() {
    std::vector<uint8_t> input;
    input.reserve(12000);
    for (size_t i = 0; i < 12000; ++i) {
        input.push_back(static_cast<uint8_t>((i * 37 + i / 11) & 0xff));
    }

    std::vector<uint8_t> encoded;
    std::vector<uint8_t> decoded;
    std::string error;
    TEST_ASSERT(huffmanCompress(input, encoded, error),
                "Huffman compressor rejected valid binary data");
    TEST_ASSERT(huffmanDecompress(encoded, input.size(), decoded, error),
                "Huffman decompressor rejected its own output");
    TEST_ASSERT(decoded == input, "Huffman round-trip changed input bytes");

    const std::vector<uint8_t> empty;
    TEST_ASSERT(huffmanCompress(empty, encoded, error),
                "Huffman compressor rejected empty input");
    TEST_ASSERT(huffmanDecompress(encoded, 0, decoded, error) && decoded.empty(),
                "Empty Huffman stream did not round-trip");
}

static void test_selective_huffman_compression() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    std::vector<uint8_t> shared(4096, 'A');
    std::vector<uint8_t> added(4096, 'B');
    std::vector<uint8_t> noisy(4096);
    for (size_t i = 0; i < noisy.size(); ++i) {
        noisy[i] = static_cast<uint8_t>(i & 0xff);
    }
    createTestFile("baseline/shared.txt", shared);
    createTestFile("current/shared.txt", shared);
    createTestFile("current/added.txt", added);
    createTestFile("current/noise.bin", noisy);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = shared.size();
    ChunkEngine engine(config);

    ProjectScanner baselineScanner((TEST_DIR / "baseline").string());
    ProjectScanner currentScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(
        baselineScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(
        currentScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "selected.rtpack";
    TEST_ASSERT(compressor.compressMissingChunks(current, baseline,
                                                 payloadPath.string(), result, error),
                "Selective compression failed");
    TEST_ASSERT(result.metrics.totalChunks == 3, "Unexpected current chunk count");
    TEST_ASSERT(result.metrics.reusableChunks == 1,
                "Identical baseline chunk should be marked reusable");
    TEST_ASSERT(result.metrics.missingChunks == 2,
                "Only the absent chunks should be written");
    TEST_ASSERT(result.metrics.huffmanChunks == 1,
                "Compressible missing chunk should use Huffman");
    TEST_ASSERT(result.metrics.storedRawChunks == 1,
                "Incompressible missing chunk should use raw storage");
    TEST_ASSERT(result.payloadChunks.size() == 2,
                "Payload bundle should contain only missing chunks");

    std::ifstream payload(payloadPath, std::ios::binary);
    char header[8] = {};
    payload.read(header, sizeof(header));
    TEST_ASSERT(payload && std::string(header, header + 4) == "RTPK",
                "Payload bundle header is invalid");
    for (const CompressedChunkInfo& chunk : result.payloadChunks) {
        payload.clear();
        payload.seekg(static_cast<std::streamoff>(chunk.payloadOffset));
        std::vector<uint8_t> stored(static_cast<size_t>(chunk.storedSize));
        payload.read(reinterpret_cast<char*>(stored.data()),
                     static_cast<std::streamsize>(stored.size()));
        TEST_ASSERT(payload.gcount() == static_cast<std::streamsize>(stored.size()),
                    "Could not read selected chunk from payload bundle");

        std::vector<uint8_t> decoded;
        if (chunk.codec == "huffman") {
            TEST_ASSERT(huffmanDecompress(stored, chunk.metadata.length,
                                          decoded, error),
                        "Could not decode selected Huffman chunk");
        } else {
            TEST_ASSERT(chunk.codec == "raw", "Unknown selected chunk codec");
            decoded = stored;
        }
        const std::vector<uint8_t>& expected =
            chunk.metadata.relativePath == "added.txt" ? added : noisy;
        TEST_ASSERT(decoded == expected,
                    "Decoded selected chunk differs from its source");
    }
}

static void test_selective_compression_deduplicates_current_chunks() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    const std::vector<uint8_t> repeated(4096, 'R');
    createTestFile("current/first.bin", repeated);
    createTestFile("current/second.bin", repeated);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = repeated.size();
    ChunkEngine engine(config);

    ProjectScanner baselineScanner((TEST_DIR / "baseline").string());
    ProjectScanner currentScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(
        baselineScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(
        currentScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "duplicates.rtpack";
    TEST_ASSERT(compressor.compressMissingChunks(current, baseline,
                                                 payloadPath.string(), result, error),
                "Selective compression with duplicate chunks failed");
    TEST_ASSERT(result.metrics.totalChunks == 2,
                "Both current chunks should be counted");
    TEST_ASSERT(result.metrics.missingChunks == 2,
                "Both chunks are absent from the baseline");
    TEST_ASSERT(result.metrics.duplicateChunks == 1,
                "One repeated missing chunk should be deduplicated");
    TEST_ASSERT(result.metrics.duplicateBytesAvoided == repeated.size(),
                "Repeated chunk bytes were not counted as avoided");
    TEST_ASSERT(result.metrics.uniqueInputBytes == repeated.size(),
                "Only one copy should be compressed");
    TEST_ASSERT(result.payloadChunks.size() == 1,
                "Payload should contain one copy of repeated chunk data");
    TEST_ASSERT(result.metrics.totalSourceBytes == repeated.size() * 2,
                "Source byte total should include both chunks");
    TEST_ASSERT(result.metrics.totalBytesAvoided ==
                    result.metrics.totalSourceBytes -
                        result.metrics.payloadBytes -
                        result.metrics.bundleHeaderBytes,
                "Total bytes avoided should account for bundle contents");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "   RETROTRANSFER CHUNKING ENGINE TESTS  \n";
    std::cout << "========================================\n";

    setupTestDir();

    RUN_TEST(test_empty_file);
    RUN_TEST(test_one_byte_file);
    RUN_TEST(test_smaller_than_chunk_size);
    RUN_TEST(test_exact_chunk_size);
    RUN_TEST(test_exactly_divisible_by_chunk_size);
    RUN_TEST(test_not_divisible_by_chunk_size);
    RUN_TEST(test_binary_data_embedded_nulls);
    RUN_TEST(test_crc32_standard_vector);
    RUN_TEST(test_c_murmurhash_hex_matches_binary_result);
    RUN_TEST(test_identical_files_produce_identical_fingerprints);
    RUN_TEST(test_changed_contents_produce_different_fingerprints);
    RUN_TEST(test_correct_offsets_and_lengths);
    RUN_TEST(test_cdc_min_chunk_size_behavior);
    RUN_TEST(test_cdc_max_chunk_size_behavior);
    RUN_TEST(test_cdc_deterministic_output);
    RUN_TEST(test_cdc_content_boundaries_trigger);
    RUN_TEST(test_scanner_skips_generated_dirs_and_symlinks);
    RUN_TEST(test_invalid_configurations);
    RUN_TEST(test_missing_unreadable_file);
    RUN_TEST(test_large_file_incremental);
    RUN_TEST(test_adaptive_mode_heuristics);
    RUN_TEST(test_explicit_mode_overrides);
    RUN_TEST(test_payload_reread_and_byte_comparison);
    RUN_TEST(test_version_comparison_metrics);
    RUN_TEST(test_huffman_round_trip);
    RUN_TEST(test_selective_huffman_compression);
    RUN_TEST(test_selective_compression_deduplicates_current_chunks);

    cleanupTestDir();

    std::cout << "========================================\n";
    std::cout << "Tests run: " << g_tests_run << " | Failures: " << g_tests_failed << "\n";
    if (g_tests_failed == 0) {
        std::cout << "ALL TESTS PASSED SUCCESSFULLY!\n";
    } else {
        std::cout << "SOME TESTS FAILED!\n";
    }
    std::cout << "========================================\n";

    return g_tests_failed == 0 ? 0 : 1;
}
