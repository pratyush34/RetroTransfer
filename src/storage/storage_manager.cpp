#include "storage/storage_manager.h"
#include "storage/chunk_verifier.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace retro::storage {

StorageJsonValue StorageConfig::toJson() const {
    StorageJsonValue obj = StorageJsonValue::makeObject();
    obj.object["formatVersion"] = StorageJsonValue::makeNumber(static_cast<uint64_t>(formatVersion));
    obj.object["storageDir"] = StorageJsonValue::makeString(storageDir);
    obj.object["chunksSubdir"] = StorageJsonValue::makeString(chunksSubdir);
    obj.object["indexFilename"] = StorageJsonValue::makeString(indexFilename);
    obj.object["maxChunkSizeBytes"] = StorageJsonValue::makeNumber(maxChunkSizeBytes);
    return obj;
}

bool StorageConfig::fromJson(const StorageJsonValue& json, StorageConfig& outConfig, std::string& error) {
    if (!json.isObject()) {
        error = "Storage config must be a JSON object";
        return false;
    }
    auto itVer = json.object.find("formatVersion");
    if (itVer != json.object.end() && itVer->second.isNumber()) {
        outConfig.formatVersion = static_cast<uint32_t>(itVer->second.asUint64());
    }
    auto itDir = json.object.find("storageDir");
    if (itDir != json.object.end() && itDir->second.isString()) {
        outConfig.storageDir = itDir->second.asString();
    }
    auto itSub = json.object.find("chunksSubdir");
    if (itSub != json.object.end() && itSub->second.isString()) {
        outConfig.chunksSubdir = itSub->second.asString();
    }
    auto itIdx = json.object.find("indexFilename");
    if (itIdx != json.object.end() && itIdx->second.isString()) {
        outConfig.indexFilename = itIdx->second.asString();
    }
    auto itMax = json.object.find("maxChunkSizeBytes");
    if (itMax != json.object.end() && itMax->second.isNumber()) {
        outConfig.maxChunkSizeBytes = itMax->second.asUint64();
    }
    return true;
}

StorageManager::StorageManager(const std::string& storageDir)
    : storageDir_(storageDir), storage_(storageDir) {
    fs::path base(storageDir);
    configPath_ = (base / "storage_config.json").lexically_normal().generic_string();
    indexPath_ = (base / "chunk_index.json").lexically_normal().generic_string();
}

bool StorageManager::loadOrInitConfig(std::string& error) {
    std::error_code ec;
    if (fs::exists(configPath_, ec)) {
        StorageJsonValue json;
        if (!readJsonFile(configPath_, json, error)) {
            return false;
        }
        if (!StorageConfig::fromJson(json, config_, error)) {
            return false;
        }
    } else {
        config_.storageDir = storageDir_;
        config_.chunksSubdir = "chunks";
        config_.indexFilename = "chunk_index.json";
        if (!safeWriteJsonFile(configPath_, config_.toJson(), error)) {
            return false;
        }
    }
    return true;
}

bool StorageManager::initialize(const std::string& storageDir, std::string& error) {
    storageDir_ = fs::path(storageDir).lexically_normal().generic_string();
    fs::path base(storageDir_);
    configPath_ = (base / "storage_config.json").lexically_normal().generic_string();
    indexPath_ = (base / "chunk_index.json").lexically_normal().generic_string();

    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        error = "Failed to create storage root directory (" + storageDir_ + "): " + ec.message();
        return false;
    }

    // Initialize physical storage
    storage_ = ChunkStorage(storageDir_);
    if (!storage_.initialize(error)) {
        return false;
    }

    // Initialize storage config
    if (!loadOrInitConfig(error)) {
        return false;
    }

    // Initialize chunk index
    if (!index_.initialize(indexPath_, error)) {
        // If index loading failed because file is missing, try rebuild
        if (!fs::exists(indexPath_, ec)) {
            if (!rebuildIndex(error)) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

bool StorageManager::storeChunk(const std::string& fingerprint,
                                const uint8_t* data,
                                size_t size,
                                uint32_t crc32,
                                std::string& error) {
    if (size > config_.maxChunkSizeBytes) {
        error = "Chunk size (" + std::to_string(size) + " bytes) exceeds maximum configured chunk size";
        return false;
    }

    // Check if chunk is already registered in index
    ChunkRecord existingRecord;
    if (index_.findChunk(fingerprint, existingRecord)) {
        if (existingRecord.size != size || existingRecord.crc32 != crc32) {
            error = "Collision detected: fingerprint " + fingerprint + " already exists with differing size or checksum";
            return false;
        }
        // Verify physical storage is intact
        if (storage_.hasChunk(fingerprint)) {
            // Confirm byte identical
            std::string cmpErr;
            if (ChunkVerifier::compareDataWithFile(storage_.getChunkPath(fingerprint), data, size, cmpErr)) {
                return true; // Idempotent duplicate: already safely stored
            }
            // If the physical file on disk was corrupted, fall through to overwrite/repair it!
        }
    }

    // Store physical chunk on disk
    if (!storage_.storeChunk(fingerprint, data, size, crc32, error)) {
        return false;
    }

    // Create index entry
    uint64_t nowSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    ChunkRecord rec;
    rec.fingerprint = fingerprint;
    rec.size = static_cast<uint64_t>(size);
    rec.crc32 = crc32;
    rec.filename = fingerprint + ".chunk";
    rec.status = "available";
    rec.version = 1;
    rec.createdAt = nowSec;

    // Update index and persist atomically
    if (!index_.insertChunk(rec, error)) {
        // If index insertion fails, clean up stored chunk
        std::string delErr;
        storage_.deleteChunk(fingerprint, delErr);
        return false;
    }

    return true;
}

bool StorageManager::getChunk(const std::string& fingerprint,
                              std::vector<uint8_t>& outData,
                              std::string& error) const {
    ChunkRecord rec;
    if (!index_.findChunk(fingerprint, rec)) {
        error = "Chunk not found in index: " + fingerprint;
        return false;
    }

    if (rec.status != "available") {
        error = "Chunk status in index is '" + rec.status + "': " + fingerprint;
        return false;
    }

    if (!storage_.retrieveChunk(fingerprint, outData, error)) {
        return false;
    }

    if (outData.size() != rec.size) {
        outData.clear();
        error = "Retrieved chunk size (" + std::to_string(outData.size()) +
                ") does not match indexed size (" + std::to_string(rec.size) + ")";
        return false;
    }

    return true;
}

bool StorageManager::hasChunk(const std::string& fingerprint) const {
    if (!index_.containsChunk(fingerprint)) {
        return false;
    }
    return storage_.hasChunk(fingerprint);
}

bool StorageManager::deleteChunk(const std::string& fingerprint, std::string& error) {
    bool indexOk = index_.removeChunk(fingerprint, error);
    std::string storageErr;
    bool storageOk = storage_.deleteChunk(fingerprint, storageErr);
    if (!indexOk) return false;
    if (!storageOk) {
        error = storageErr;
        return false;
    }
    return true;
}

bool StorageManager::validateRepository(std::string& error) {
    return index_.validateIndex(storage_.getChunksDir(), error);
}

bool StorageManager::rebuildIndex(std::string& error) {
    return index_.rebuildFromChunks(storage_.getChunksDir(), indexPath_, error);
}

size_t StorageManager::getStoredChunkCount() const {
    return index_.size();
}

uint64_t StorageManager::getTotalStoredBytes() const {
    uint64_t total = 0;
    for (const auto& rec : index_.getAllRecords()) {
        total += rec.size;
    }
    return total;
}

} // namespace retro::storage
