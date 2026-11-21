#ifndef TEST_RECONSTRUCTION_COMPREHENSIVE_HPP
#define TEST_RECONSTRUCTION_COMPREHENSIVE_HPP

#include "reconstruction_engine.h"
#include "compression_engine.h"
#include "chunk_engine.h"
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static void test_reconstruction_corrupted_payload() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "current");
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "dest");

    const std::vector<uint8_t> data(4096, 'M');
    createTestFile("current/test.txt", data);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 4096;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    const fs::path payloadPath = TEST_DIR / "corrupt.rtpack";
    CompressionResult compression;
    std::string error;
    CompressionEngine compressor;
    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), compression, error),
                "Compression setup failed");

    // Build valid manifest
    const fs::path manifestPath = TEST_DIR / "corrupt_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"corrupt.rtpack\",\"payloadChunks\":[";
        for (size_t i = 0; i < compression.payloadChunks.size(); ++i) {
            const auto& chunk = compression.payloadChunks[i];
            if (i != 0) manifest << ",";
            manifest << "{\"fingerprint\":\"" << chunk.metadata.fingerprint
                     << "\",\"originalSize\":" << chunk.metadata.length
                     << ",\"payloadOffset\":" << chunk.payloadOffset
                     << ",\"storedSize\":" << chunk.storedSize
                     << ",\"codec\":\"" << chunk.codec
                     << "\",\"crc32\":" << chunk.metadata.crc32 << "}";
        }
        manifest << "]},\"files\":[{\"path\":\"test.txt\",\"size\":4096,\"chunks\":[";
        manifest << "{\"seq\":0,\"offset\":0,\"length\":4096,\"fingerprint\":\""
                 << current.allChunks[0].fingerprint << "\",\"crc32\":" << current.allChunks[0].crc32 << "}]}]}";
    }

    // Now intentionally corrupt the payload file magic header bytes
    {
        std::ofstream payload(payloadPath, std::ios::binary | std::ios::trunc);
        payload.write("BADMAGIC", 8);
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    bool ok = reconstruction.reconstruct(manifestPath.string(), payloadPath.string(),
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(!ok, "Reconstruction must fail when payload data is corrupted");
    TEST_ASSERT(!error.empty(), "Error message must be populated on corrupted payload");
}

static void test_reconstruction_missing_payload() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "dest");

    const fs::path manifestPath = TEST_DIR / "missing_payload_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"nonexistent.rtpack\",\"payloadChunks\":[]},"
                 << "\"files\":[]}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    // Missing payload file must fail because RTPK bundle is required to exist
    bool ok = reconstruction.reconstruct(manifestPath.string(),
                                         (TEST_DIR / "nonexistent.rtpack").string(),
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(!ok, "Reconstruction must fail when payload bundle does not exist");
    TEST_ASSERT(!error.empty(), "Error message must report missing payload bundle");
}

static void test_reconstruction_invalid_manifest_json() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "dest");

    const fs::path manifestPath = TEST_DIR / "bad_json.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{ this is not valid json! }";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    bool ok = reconstruction.reconstruct(manifestPath.string(), "dummy.rtpack",
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(!ok, "Reconstruction must fail on malformed JSON");
    TEST_ASSERT(error.find("JSON") != std::string::npos || !error.empty(),
                "Error message should mention parsing or JSON");
}

static void test_reconstruction_missing_manifest_fields() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "dest");

    // Missing 'files' field
    const fs::path manifestPath = TEST_DIR / "missing_fields.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\"}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    bool ok = reconstruction.reconstruct(manifestPath.string(), "dummy.rtpack",
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(!ok, "Reconstruction must fail when required fields are missing");
}

static void test_reconstruction_path_traversal_rejected() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "dest");

    const fs::path manifestPath = TEST_DIR / "traversal_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"bundle.rtpack\",\"payloadChunks\":[]},"
                 << "\"files\":[{\"path\":\"../outside.txt\",\"size\":0,\"chunks\":[]}]}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    bool ok = reconstruction.reconstruct(manifestPath.string(), "bundle.rtpack",
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(!ok, "Reconstruction must reject directory traversal (../)");
    TEST_ASSERT(error.find("unsafe") != std::string::npos || error.find("path") != std::string::npos,
                "Error should flag path as unsafe");
}

static void test_reconstruction_empty_file() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "dest");

    const fs::path bundlePath = TEST_DIR / "empty_bundle.rtpack";
    {
        std::ofstream bundle(bundlePath, std::ios::binary);
        const char header[8] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
        bundle.write(header, 8);
    }

    const fs::path manifestPath = TEST_DIR / "empty_file_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"empty_bundle.rtpack\",\"payloadChunks\":[]},"
                 << "\"files\":[{\"path\":\"empty_created.txt\",\"size\":0,\"chunks\":[]}]}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    bool ok = reconstruction.reconstruct(manifestPath.string(), bundlePath.string(),
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(ok, "Reconstruction should succeed for empty file: " + error);
    TEST_ASSERT(result.filesWritten == 1, "Expected 1 file written");

    const fs::path outFilePath = TEST_DIR / "dest/empty_created.txt";
    TEST_ASSERT(fs::exists(outFilePath), "Output empty file must exist");
    TEST_ASSERT(fs::file_size(outFilePath) == 0, "Output file must be 0 bytes");
}

static void test_reconstruction_deeply_nested_directory() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "current");
    fs::create_directories(TEST_DIR / "baseline");
    fs::create_directories(TEST_DIR / "dest");

    const std::vector<uint8_t> data = {'N', 'e', 's', 't', 'e', 'd', '!'};
    createTestFile("current/nested_sub.txt", data);

    EngineConfig config;
    config.strategy = ChunkStrategy::Fixed;
    config.fixedConfig.chunkSize = 4096;
    ChunkEngine engine(config);

    ProjectScanner baseScanner((TEST_DIR / "baseline").string());
    ProjectScanner currScanner((TEST_DIR / "current").string());
    const ProjectChunkResult baseline = engine.processScannedProject(baseScanner.scan(), (TEST_DIR / "baseline").string());
    const ProjectChunkResult current = engine.processScannedProject(currScanner.scan(), (TEST_DIR / "current").string());

    const fs::path payloadPath = TEST_DIR / "nested.rtpack";
    CompressionResult compression;
    std::string error;
    CompressionEngine compressor;
    TEST_ASSERT(compressor.compressMissingChunks(current, baseline, payloadPath.string(), compression, error),
                "Compression failed");

    // Write manifest specifying deep path 'sub1/sub2/sub3/output.txt'
    const fs::path manifestPath = TEST_DIR / "nested_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"nested.rtpack\",\"payloadChunks\":[";
        manifest << "{\"fingerprint\":\"" << compression.payloadChunks[0].metadata.fingerprint
                 << "\",\"originalSize\":" << compression.payloadChunks[0].metadata.length
                 << ",\"payloadOffset\":" << compression.payloadChunks[0].payloadOffset
                 << ",\"storedSize\":" << compression.payloadChunks[0].storedSize
                 << ",\"codec\":\"" << compression.payloadChunks[0].codec
                 << "\",\"crc32\":" << compression.payloadChunks[0].metadata.crc32 << "}]},\"files\":[";
        manifest << "{\"path\":\"sub1/sub2/sub3/output.txt\",\"size\":" << data.size() << ",\"chunks\":[";
        manifest << "{\"seq\":0,\"offset\":0,\"length\":" << data.size()
                 << ",\"fingerprint\":\"" << current.allChunks[0].fingerprint
                 << "\",\"crc32\":" << current.allChunks[0].crc32 << "}]}]}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    bool ok = reconstruction.reconstruct(manifestPath.string(), payloadPath.string(),
                                         (TEST_DIR / "dest").string(), result, error);
    TEST_ASSERT(ok, "Reconstruction failed for deeply nested directory: " + error);
    TEST_ASSERT(result.filesWritten == 1, "Expected 1 file written");

    const fs::path destFile = TEST_DIR / "dest/sub1/sub2/sub3/output.txt";
    TEST_ASSERT(fs::exists(destFile), "Deeply nested output file must exist");
    std::ifstream in(destFile, std::ios::binary);
    std::vector<uint8_t> readBack((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
    TEST_ASSERT(readBack == data, "Deeply nested file contents mismatch");
}

static void test_reconstruction_chunk_store_integration() {
    setupTestDir();
    fs::create_directories(TEST_DIR / "chunk_store");
    fs::create_directories(TEST_DIR / "dest");

    const std::vector<uint8_t> chunkData = {'C', 'h', 'u', 'n', 'k', 'S', 't', 'o', 'r', 'e'};
    char fpHex[33] = {};
    rt_compute_fingerprint_hex(chunkData.data(), chunkData.size(), 42, fpHex);
    const uint32_t crc = rt_compute_crc32(chunkData.data(), chunkData.size());

    // Create .chunk file in chunk_store: <fingerprint>-<length>-<crc32>.chunk
    const std::string chunkFileName = std::string(fpHex) + "-" + std::to_string(chunkData.size()) + "-" + std::to_string(crc) + ".chunk";
    {
        std::ofstream chunkFile(TEST_DIR / "chunk_store" / chunkFileName, std::ios::binary);
        chunkFile.write(reinterpret_cast<const char*>(chunkData.data()), chunkData.size());
    }

    const fs::path bundlePath = TEST_DIR / "store_bundle.rtpack";
    {
        std::ofstream bundle(bundlePath, std::ios::binary);
        const char header[8] = {'R', 'T', 'P', 'K', 1, 0, 0, 0};
        bundle.write(header, 8);
    }

    // Manifest referring to this chunk, but payloadChunks is empty (chunk should come from chunk_store)
    const fs::path manifestPath = TEST_DIR / "store_manifest.json";
    {
        std::ofstream manifest(manifestPath);
        manifest << "{\"selectedStrategy\":\"fixed\",\"chunking\":{\"fixedChunkSize\":4096,\"cdcMinChunkSize\":16384,\"cdcTargetChunkSize\":65536,\"cdcMaxChunkSize\":262144},"
                 << "\"selectiveCompression\":{\"payloadFile\":\"store_bundle.rtpack\",\"payloadChunks\":[]},"
                 << "\"files\":[{\"path\":\"from_store.txt\",\"size\":" << chunkData.size() << ",\"chunks\":[";
        manifest << "{\"seq\":0,\"offset\":0,\"length\":" << chunkData.size()
                 << ",\"fingerprint\":\"" << fpHex
                 << "\",\"crc32\":" << crc << "}]}]}";
    }

    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string error;
    bool ok = reconstruction.reconstruct(manifestPath.string(), bundlePath.string(),
                                         (TEST_DIR / "dest").string(), result, error,
                                         (TEST_DIR / "chunk_store").string());
    TEST_ASSERT(ok, "Reconstruction with chunk store failed: " + error);
    TEST_ASSERT(result.filesWritten == 1, "Expected 1 file written");
    TEST_ASSERT(result.chunksReused == 1, "Expected 1 chunk reused from chunk store");

    const fs::path destFile = TEST_DIR / "dest/from_store.txt";
    TEST_ASSERT(fs::exists(destFile), "Dest file from chunk store must exist");
}

#endif // TEST_RECONSTRUCTION_COMPREHENSIVE_HPP
