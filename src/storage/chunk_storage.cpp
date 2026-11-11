#include "storage/chunk_storage.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace retro::storage {

static std::atomic<uint64_t> g_tempFileCounter{0};

ChunkStorage::ChunkStorage(const std::string& storageDir)
    : storageDir_(storageDir) {
    fs::path base(storageDir);
    chunksDir_ = (base / "chunks").lexically_normal().generic_string();
    storageDir_ = base.lexically_normal().generic_string();
}

bool ChunkStorage::initialize(std::string& error) {
    std::error_code ec;
    fs::create_directories(chunksDir_, ec);
    if (ec) {
        error = "Failed to create chunks storage directory (" + chunksDir_ + "): " + ec.message();
        return false;
    }
    return true;
}

std::string ChunkStorage::getChunkPath(const std::string& fingerprint) const {
    fs::path p = fs::path(chunksDir_) / (fingerprint + ".chunk");
    return p.lexically_normal().generic_string();
}

bool ChunkStorage::hasChunk(const std::string& fingerprint) const {
    if (!ChunkVerifier::isValidFingerprintFormat(fingerprint)) return false;
    std::error_code ec;
    return fs::is_regular_file(getChunkPath(fingerprint), ec);
}

uint64_t ChunkStorage::getStoredChunkSize(const std::string& fingerprint) const {
    if (!hasChunk(fingerprint)) return 0;
    std::error_code ec;
    uintmax_t sz = fs::file_size(getChunkPath(fingerprint), ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

bool ChunkStorage::storeChunk(const std::string& fingerprint,
                              const uint8_t* data,
                              size_t size,
                              uint32_t crc32,
                              std::string& error) {
    if (!ChunkVerifier::isValidFingerprintFormat(fingerprint)) {
        error = "Invalid fingerprint format for storage: " + fingerprint;
        return false;
    }

    // Verify incoming data integrity
    if (!ChunkVerifier::verifyData(data, size, fingerprint, crc32, error)) {
        return false;
    }

    std::string targetPath = getChunkPath(fingerprint);
    std::error_code ec;

    // Check if chunk is already stored
    if (fs::exists(targetPath, ec)) {
        std::string vErr;
        if (ChunkVerifier::verifyFile(targetPath, fingerprint, size, crc32, vErr)) {
            // Identical valid chunk already safely stored
            return true;
        }
        // If file on disk fails verification (corrupted or truncated), proceed to safely overwrite/repair it
    }

    // Ensure directory exists
    fs::create_directories(chunksDir_, ec);
    if (ec) {
        error = "Cannot create chunks directory: " + ec.message();
        return false;
    }

    // Generate safe temporary file path
    uint64_t count = ++g_tempFileCounter;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
    std::string tempPath = (fs::path(chunksDir_) /
                            (fingerprint + "." + std::to_string(nowMs) + "." + std::to_string(count) + ".tmp"))
                               .lexically_normal().generic_string();

    // Write chunk data safely
    {
        std::ofstream outFile(tempPath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            error = "Could not open temporary chunk file: " + tempPath;
            return false;
        }

        if (size > 0 && data != nullptr) {
            outFile.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        outFile.flush();
        if (!outFile.good()) {
            outFile.close();
            fs::remove(tempPath, ec);
            error = "I/O error while writing temporary chunk file: " + tempPath;
            return false;
        }
    }

    // Verify written temporary chunk file before making permanent
    if (!ChunkVerifier::verifyFile(tempPath, fingerprint, size, crc32, error)) {
        fs::remove(tempPath, ec);
        error = "Validation of written temporary chunk failed: " + error;
        return false;
    }

    // Atomically rename temporary file to target path
    fs::remove(targetPath, ec); // Remove destination if partially created
    ec.clear();
    fs::rename(tempPath, targetPath, ec);
    if (ec) {
        error = "Failed to finalize chunk storage from " + tempPath + " to " + targetPath + ": " + ec.message();
        fs::remove(tempPath, ec);
        return false;
    }

    return true;
}

bool ChunkStorage::retrieveChunk(const std::string& fingerprint,
                                 std::vector<uint8_t>& outData,
                                 std::string& error) const {
    if (!ChunkVerifier::isValidFingerprintFormat(fingerprint)) {
        error = "Invalid fingerprint format for retrieval: " + fingerprint;
        return false;
    }

    std::string path = getChunkPath(fingerprint);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        error = "Chunk file not found: " + path;
        return false;
    }

    uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        error = "Could not read chunk size: " + ec.message();
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "Could not open chunk file: " + path;
        return false;
    }

    outData.resize(static_cast<size_t>(size));
    if (size > 0) {
        file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(size));
        if (file.gcount() != static_cast<std::streamsize>(size)) {
            outData.clear();
            error = "Incomplete read of chunk file: " + path;
            return false;
        }
    }

    // Verify retrieved chunk fingerprint
    std::string actualFp = ChunkVerifier::computeFingerprint(outData.data(), outData.size());
    if (actualFp != fingerprint) {
        outData.clear();
        error = "Retrieved chunk is corrupted (fingerprint mismatch: expected " +
                fingerprint + ", got " + actualFp + ")";
        return false;
    }

    return true;
}

bool ChunkStorage::validateChunk(const std::string& fingerprint,
                                 uint64_t expectedSize,
                                 uint32_t expectedCrc32,
                                 std::string& error) const {
    return ChunkVerifier::verifyFile(getChunkPath(fingerprint), fingerprint, expectedSize, expectedCrc32, error);
}

bool ChunkStorage::deleteChunk(const std::string& fingerprint, std::string& error) {
    if (!ChunkVerifier::isValidFingerprintFormat(fingerprint)) {
        error = "Invalid fingerprint: " + fingerprint;
        return false;
    }
    std::string path = getChunkPath(fingerprint);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (!fs::remove(path, ec) && ec) {
            error = "Could not remove chunk file: " + ec.message();
            return false;
        }
    }
    return true;
}

std::vector<std::string> ChunkStorage::listStoredChunkFingerprints() const {
    std::vector<std::string> results;
    std::error_code ec;
    if (!fs::exists(chunksDir_, ec) || !fs::is_directory(chunksDir_, ec)) {
        return results;
    }

    for (const auto& entry : fs::directory_iterator(chunksDir_, ec)) {
        if (entry.is_regular_file(ec)) {
            std::string filename = entry.path().filename().string();
            if (filename.size() == 38 && filename.substr(32) == ".chunk") { // 32 chars + .chunk
                std::string fp = filename.substr(0, 32);
                if (ChunkVerifier::isValidFingerprintFormat(fp)) {
                    results.push_back(fp);
                }
            }
        }
    }

    return results;
}

} // namespace retro::storage
