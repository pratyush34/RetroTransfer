#ifndef TEST_COMPRESSION_DEDUP_HPP
#define TEST_COMPRESSION_DEDUP_HPP

#include "huffman_codec_c.h"
#include "compression_engine.h"
#include "chunk_engine.h"
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

namespace fs = std::filesystem;

static void test_huffman_empty_data() {
    uint8_t* encoded = nullptr;
    size_t encodedSize = 0;
    char err[512] = {};

    // Compress 0 bytes
    int compOk = rt_huffman_compress(nullptr, 0, &encoded, &encodedSize, err, sizeof(err));
    TEST_ASSERT(compOk == 1, "rt_huffman_compress should handle empty input");
    TEST_ASSERT(encoded != nullptr, "Encoded buffer should not be null");
    TEST_ASSERT(encodedSize == 256, "Encoded empty input should store 256-byte code lengths header");

    // Decompress 0 bytes
    uint8_t* decoded = nullptr;
    size_t decodedSize = 0;
    int decompOk = rt_huffman_decompress(encoded, encodedSize, 0, &decoded, &decodedSize, err, sizeof(err));
    TEST_ASSERT(decompOk == 1, "rt_huffman_decompress should decompress empty input");
    TEST_ASSERT(decodedSize == 0, "Decoded size must be 0");

    std::free(encoded);
    std::free(decoded);
}

static void test_huffman_single_symbol_repeated() {
    const std::vector<uint8_t> input(8192, 'A');
    uint8_t* encoded = nullptr;
    size_t encodedSize = 0;
    char err[512] = {};

    TEST_ASSERT(rt_huffman_compress(input.data(), input.size(), &encoded, &encodedSize, err, sizeof(err)),
                "Huffman compression failed on single repeated symbol");
    // Single symbol should compress extremely well (1 bit per symbol = 1024 bytes + 256 header = ~1280 bytes)
    TEST_ASSERT(encodedSize < input.size() / 4,
                "Huffman compression should achieve >75% reduction on single repeated symbol");

    uint8_t* decoded = nullptr;
    size_t decodedSize = 0;
    TEST_ASSERT(rt_huffman_decompress(encoded, encodedSize, input.size(), &decoded, &decodedSize, err, sizeof(err)),
                "Huffman decompression failed on single repeated symbol");
    TEST_ASSERT(decodedSize == input.size(), "Decoded size mismatch");
    TEST_ASSERT(std::memcmp(decoded, input.data(), input.size()) == 0,
                "Decoded data does not match original single repeated symbol");

    std::free(encoded);
    std::free(decoded);
}

static void test_huffman_uniform_byte_distribution() {
    // 256 distinct bytes repeated 16 times = 4096 bytes
    std::vector<uint8_t> input(4096);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t* encoded = nullptr;
    size_t encodedSize = 0;
    char err[512] = {};

    TEST_ASSERT(rt_huffman_compress(input.data(), input.size(), &encoded, &encodedSize, err, sizeof(err)),
                "Huffman compression failed on uniform distribution");

    uint8_t* decoded = nullptr;
    size_t decodedSize = 0;
    TEST_ASSERT(rt_huffman_decompress(encoded, encodedSize, input.size(), &decoded, &decodedSize, err, sizeof(err)),
                "Huffman decompression failed on uniform distribution");
    TEST_ASSERT(decodedSize == input.size(), "Decoded size mismatch");
    TEST_ASSERT(std::memcmp(decoded, input.data(), input.size()) == 0,
                "Decoded data does not match original uniform bytes");

    std::free(encoded);
    std::free(decoded);
}

static void test_huffman_corrupted_stream() {
    const std::vector<uint8_t> input = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd', '!'};
    uint8_t* encoded = nullptr;
    size_t encodedSize = 0;
    char err[512] = {};

    TEST_ASSERT(rt_huffman_compress(input.data(), input.size(), &encoded, &encodedSize, err, sizeof(err)),
                "Compression failed in corrupted stream setup");

    // Case 1: Truncated header (less than 256 bytes)
    uint8_t* decoded = nullptr;
    size_t decodedSize = 0;
    int ok = rt_huffman_decompress(encoded, 100, input.size(), &decoded, &decodedSize, err, sizeof(err));
    TEST_ASSERT(ok == 0, "Decompression must fail on truncated header");

    // Case 2: Corrupted bitstream payload
    if (encodedSize > 260) {
        encoded[258] ^= 0xFF;
        encoded[259] ^= 0xFF;
        uint8_t* badDecoded = nullptr;
        size_t badSize = 0;
        // Should either fail cleanly or output corrupted size/content, but NEVER crash or hang
        rt_huffman_decompress(encoded, encodedSize, input.size(), &badDecoded, &badSize, err, sizeof(err));
        std::free(badDecoded);
    }

    std::free(encoded);
    std::free(decoded);
}

static void test_compression_engine_all_chunks_reusable() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    const std::vector<uint8_t> data(4096, 'K');
    createTestFile("baseline/identical.txt", data);
    createTestFile("current/identical.txt", data);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 4096;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "reusable_all.rtpack";

    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), result, error),
                "compressMissingChunks failed on identical projects");
    TEST_ASSERT(result.metrics.missingChunks == 0, "No chunks should be missing");
    TEST_ASSERT(result.metrics.reusableChunks == 1, "Expected 1 reusable chunk");
    TEST_ASSERT(result.payloadChunks.empty(), "Payload should contain 0 chunks when all are reusable");
    TEST_ASSERT(result.metrics.reusableBytes == 4096, "Reusable bytes should equal 4096");
}

static void test_compression_engine_cross_file_duplicates() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    const std::vector<uint8_t> dupData(2048, 'D');
    createTestFile("current/file1.bin", dupData);
    createTestFile("current/file2.bin", dupData);
    createTestFile("current/file3.bin", dupData);
    createTestFile("current/file4.bin", dupData);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 2048;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "cross_file_dups.rtpack";

    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), result, error),
                "compressMissingChunks failed on cross-file duplicates");
    TEST_ASSERT(result.metrics.totalChunks == 4, "Total chunks should be 4");
    TEST_ASSERT(result.metrics.missingChunks == 4, "All 4 are missing from baseline");
    TEST_ASSERT(result.metrics.duplicateChunks == 3, "3 duplicates should be detected");
    TEST_ASSERT(result.metrics.duplicateBytesAvoided == 3 * 2048, "6144 bytes duplicate avoided");
    TEST_ASSERT(result.payloadChunks.size() == 1, "Payload should store exactly 1 copy");
}

static void test_compression_engine_incompressible_fallback() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    // Generate high-entropy incompressible pseudo-random data
    std::mt19937 rng(1337);
    std::vector<uint8_t> randomData(4096);
    for (auto& b : randomData) {
        b = static_cast<uint8_t>(rng() & 0xFF);
    }
    createTestFile("current/random.bin", randomData);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 4096;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "incompressible.rtpack";

    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), result, error),
                "compressMissingChunks failed on incompressible data");
    TEST_ASSERT(result.payloadChunks.size() == 1, "Payload should have 1 chunk");
    TEST_ASSERT(result.payloadChunks[0].codec == "raw",
                "High entropy data should fall back to raw codec");
    TEST_ASSERT(result.metrics.storedRawChunks == 1, "storedRawChunks metric mismatch");
    TEST_ASSERT(result.metrics.huffmanChunks == 0, "huffmanChunks should be 0");
}

static void test_compression_engine_empty_project() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "current");

    EngineConfig config;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    CompressionEngine compressor;
    CompressionResult result;
    std::string error;
    const fs::path payloadPath = TEST_DIR / "empty.rtpack";

    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), result, error),
                "compressMissingChunks failed on empty projects");
    TEST_ASSERT(result.payloadChunks.empty(), "Payload should have 0 chunks for empty projects");
    TEST_ASSERT(result.metrics.totalChunks == 0, "Total chunks should be 0");
}

#endif // TEST_COMPRESSION_DEDUP_HPP
