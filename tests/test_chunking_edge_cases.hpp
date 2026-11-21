#ifndef TEST_CHUNKING_EDGE_CASES_HPP
#define TEST_CHUNKING_EDGE_CASES_HPP

#include "cdc_chunker.h"
#include "fixed_chunker.h"
#include "chunk_engine.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <numeric>

namespace fs = std::filesystem;

static void test_cdc_empty_file_edge() {
    const fs::path p = createTestFile("cdc_empty.dat", {});
    CDCConfig config;
    config.minChunkSize = 1024;
    config.targetChunkSize = 4096;
    config.maxChunkSize = 16384;
    CDCChunker chunker(config);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    TEST_ASSERT(chunker.chunkFile(p.string(), "cdc_empty.dat", chunks, metrics),
                "CDC chunkFile failed on empty file");
    TEST_ASSERT(chunks.empty(), "Empty file should produce 0 chunks in CDC");
    TEST_ASSERT(metrics.chunkCount == 0, "Chunk count should be 0");
    TEST_ASSERT(metrics.fileSize == 0, "File size should be 0");
}

static void test_cdc_one_byte_file_edge() {
    const fs::path p = createTestFile("cdc_one_byte.dat", {0x42});
    CDCConfig config;
    config.minChunkSize = 1024;
    config.targetChunkSize = 4096;
    config.maxChunkSize = 16384;
    CDCChunker chunker(config);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    TEST_ASSERT(chunker.chunkFile(p.string(), "cdc_one_byte.dat", chunks, metrics),
                "CDC chunkFile failed on 1-byte file");
    TEST_ASSERT(chunks.size() == 1, "1-byte file should produce exactly 1 chunk in CDC");
    TEST_ASSERT(chunks[0].length == 1, "Chunk length must be 1");
    TEST_ASSERT(chunks[0].offset == 0, "Chunk offset must be 0");
    TEST_ASSERT(chunks[0].crc32 != 0, "Chunk CRC32 must be computed");
}

static void test_cdc_smaller_than_min_chunk_edge() {
    CDCConfig config;
    config.minChunkSize = 2048;
    config.targetChunkSize = 4096;
    config.maxChunkSize = 8192;
    CDCChunker chunker(config);

    const std::vector<uint8_t> data(1500, 0x7E);
    const fs::path p = createTestFile("cdc_small.dat", data);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    TEST_ASSERT(chunker.chunkFile(p.string(), "cdc_small.dat", chunks, metrics),
                "CDC chunkFile failed on small file");
    TEST_ASSERT(chunks.size() == 1, "File smaller than minChunkSize should produce 1 chunk");
    TEST_ASSERT(chunks[0].length == 1500, "Chunk length must equal file size");
    TEST_ASSERT(chunks[0].offset == 0, "Chunk offset must be 0");
}

static void test_cdc_repetitive_pattern_edge() {
    CDCConfig config;
    config.minChunkSize = 2048;
    config.targetChunkSize = 4096;
    config.maxChunkSize = 8192;
    CDCChunker chunker(config);

    const std::vector<uint8_t> data(65536, 0xAA);
    const fs::path p = createTestFile("cdc_repeat.dat", data);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    TEST_ASSERT(chunker.chunkFile(p.string(), "cdc_repeat.dat", chunks, metrics),
                "CDC chunkFile failed on repetitive file");
    TEST_ASSERT(!chunks.empty(), "Repetitive file must produce chunks");

    uint64_t totalBytes = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        TEST_ASSERT(chunks[i].length >= config.minChunkSize || i == chunks.size() - 1,
                    "Intermediate chunk length must be >= minChunkSize");
        TEST_ASSERT(chunks[i].length <= config.maxChunkSize,
                    "Chunk length must not exceed maxChunkSize");
        TEST_ASSERT(chunks[i].offset == totalBytes,
                    "Chunk offset must be contiguous");
        totalBytes += chunks[i].length;
    }
    TEST_ASSERT(totalBytes == data.size(), "Total chunk bytes must match file size");
}

static void test_fixed_chunker_odd_sizes() {
    FixedChunkConfig config;
    config.chunkSize = 3;
    FixedChunker chunker(config);

    const std::vector<uint8_t> data = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
        11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25
    };
    const fs::path p = createTestFile("fixed_odd.dat", data);

    std::vector<ChunkMetadata> chunks;
    FileChunkMetrics metrics;
    TEST_ASSERT(chunker.chunkFile(p.string(), "fixed_odd.dat", chunks, metrics),
                "FixedChunker failed on odd size");
    TEST_ASSERT(chunks.size() == 9, "Expected 9 chunks (8 of size 3, 1 of size 1)");
    for (size_t i = 0; i < 8; ++i) {
        TEST_ASSERT(chunks[i].length == 3, "First 8 chunks must have size 3");
        TEST_ASSERT(chunks[i].offset == i * 3, "Offset must be i * 3");
    }
    TEST_ASSERT(chunks[8].length == 1, "Final chunk must have size 1");
    TEST_ASSERT(chunks[8].offset == 24, "Final chunk offset must be 24");
}

static void test_chunk_engine_process_file_direct() {
    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 1024;
    ChunkEngine engine(config);

    const std::vector<uint8_t> data(2500, 'X');
    const fs::path p = createTestFile("direct_file.dat", data);

    FileInfo info;
    info.fileName = "direct_file.dat";
    info.filePath = p.string();
    info.fileSize = data.size();
    info.fileType = ".dat";
    info.modificationTime = 1000;

    FileChunkResult result;
    TEST_ASSERT(engine.processFile(info, "direct_file.dat", result),
                "ChunkEngine::processFile failed on valid file");
    TEST_ASSERT(result.chunks.size() == 3, "Expected 3 chunks for 2500 bytes with 1024 chunkSize");
    TEST_ASSERT(result.metrics.fileSize == 2500, "File size metric mismatch");

    FileInfo invalidInfo;
    invalidInfo.fileName = "nonexistent.dat";
    invalidInfo.filePath = "test_scratch_retro_transfer/nonexistent.dat";
    FileChunkResult failResult;
    TEST_ASSERT(!engine.processFile(invalidInfo, "nonexistent.dat", failResult),
                "ChunkEngine::processFile should return false for nonexistent file");
}

static void test_read_chunk_payload_bounds() {
    const std::vector<uint8_t> content = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'};
    const fs::path p = createTestFile("bounds_test.dat", content);

    std::vector<uint8_t> readData;
    // Valid read: 4 bytes from offset 2
    TEST_ASSERT(ChunkEngine::readChunkPayload(p.string(), 2, 4, readData),
                "readChunkPayload failed on valid read");
    const std::vector<uint8_t> expected = {'C', 'D', 'E', 'F'};
    TEST_ASSERT(readData == expected, "readChunkPayload data mismatch");

    // Zero-length read
    std::vector<uint8_t> emptyRead;
    TEST_ASSERT(ChunkEngine::readChunkPayload(p.string(), 0, 0, emptyRead),
                "readChunkPayload should handle 0 length");
    TEST_ASSERT(emptyRead.empty(), "Zero-length read should produce empty buffer");

    // Out-of-bounds read: offset 8, length 5 (file has 10 bytes)
    std::vector<uint8_t> oobRead;
    TEST_ASSERT(!ChunkEngine::readChunkPayload(p.string(), 8, 5, oobRead),
                "readChunkPayload must fail when reading past end of file");

    // Non-existent file read
    std::vector<uint8_t> noFileRead;
    TEST_ASSERT(!ChunkEngine::readChunkPayload("test_scratch_retro_transfer/missing_file.dat", 0, 10, noFileRead),
                "readChunkPayload must fail on non-existent file");
}

#endif // TEST_CHUNKING_EDGE_CASES_HPP
