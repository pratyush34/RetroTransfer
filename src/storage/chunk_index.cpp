#include "storage/chunk_index.h"
#include "storage/chunk_verifier.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace retro::storage {

bool ChunkIndex::initialize(const std::string& indexPath, std::string& error) {
    indexPath_ = indexPath;
    std::error_code ec;
    if (fs::exists(indexPath, ec)) {
        return loadIndex(indexPath, error);
    }
    // Create new empty index file
    records_.clear();
    return saveIndex(indexPath, error);
}

bool ChunkIndex::loadIndex(const std::string& indexPath, std::string& error) {
    indexPath_ = indexPath;
    records_.clear();

    StorageJsonValue root;
    if (!readJsonFile(indexPath, root, error)) {
        error = "Failed to load chunk index: " + error;
        return false;
    }

    if (!root.isObject()) {
        error = "Invalid chunk index: root must be a JSON object in " + indexPath;
        return false;
    }

    auto itChunks = root.object.find("chunks");
    if (itChunks == root.object.end() || !itChunks->second.isArray()) {
        error = "Invalid chunk index: missing or invalid 'chunks' array in " + indexPath;
        return false;
    }

    for (size_t i = 0; i < itChunks->second.array.size(); ++i) {
        const auto& item = itChunks->second.array[i];
        ChunkRecord rec;
        std::string recError;
        if (!ChunkRecord::fromJson(item, rec, recError)) {
            error = "Malformed chunk record at index " + std::to_string(i) + ": " + recError;
            records_.clear();
            return false;
        }

        if (records_.find(rec.fingerprint) != records_.end()) {
            error = "Duplicate fingerprint in chunk index: " + rec.fingerprint;
            records_.clear();
            return false;
        }

        records_[rec.fingerprint] = std::move(rec);
    }

    return true;
}

bool ChunkIndex::saveIndex(const std::string& indexPath, std::string& error) const {
    std::string targetPath = indexPath.empty() ? indexPath_ : indexPath;
    if (targetPath.empty()) {
        error = "No index file path specified";
        return false;
    }

    uint64_t nowSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    StorageJsonValue root = StorageJsonValue::makeObject();
    root.object["formatVersion"] = StorageJsonValue::makeNumber(static_cast<uint64_t>(1));
    root.object["description"] = StorageJsonValue::makeString("RetroTransfer persistent chunk index");
    root.object["updatedAt"] = StorageJsonValue::makeNumber(nowSec);
    root.object["chunkCount"] = StorageJsonValue::makeNumber(static_cast<uint64_t>(records_.size()));

    StorageJsonValue chunksArr = StorageJsonValue::makeArray();
    for (const auto& kv : records_) {
        chunksArr.array.push_back(kv.second.toJson());
    }
    root.object["chunks"] = std::move(chunksArr);

    return safeWriteJsonFile(targetPath, root, error);
}

bool ChunkIndex::findChunk(const std::string& fingerprint, ChunkRecord& outRecord) const {
    auto it = records_.find(fingerprint);
    if (it != records_.end()) {
        outRecord = it->second;
        return true;
    }
    return false;
}

bool ChunkIndex::containsChunk(const std::string& fingerprint) const {
    return records_.find(fingerprint) != records_.end();
}

bool ChunkIndex::insertChunk(const ChunkRecord& record, std::string& error) {
    if (!record.isValid()) {
        error = "Cannot insert invalid chunk record: " + record.fingerprint;
        return false;
    }

    auto it = records_.find(record.fingerprint);
    if (it != records_.end()) {
        // If identical metadata already stored, treat as idempotent success
        if (it->second.size == record.size && it->second.crc32 == record.crc32) {
            return true;
        }
        error = "Fingerprint collision detected in index: " + record.fingerprint +
                " already registered with different size or CRC";
        return false;
    }

    records_[record.fingerprint] = record;
    if (!indexPath_.empty()) {
        return saveIndex(indexPath_, error);
    }
    return true;
}

bool ChunkIndex::updateChunk(const ChunkRecord& record, std::string& error) {
    if (!record.isValid()) {
        error = "Cannot update with invalid chunk record: " + record.fingerprint;
        return false;
    }

    auto it = records_.find(record.fingerprint);
    if (it == records_.end()) {
        error = "Cannot update nonexistent chunk in index: " + record.fingerprint;
        return false;
    }

    it->second = record;
    if (!indexPath_.empty()) {
        return saveIndex(indexPath_, error);
    }
    return true;
}

bool ChunkIndex::removeChunk(const std::string& fingerprint, std::string& error) {
    auto it = records_.find(fingerprint);
    if (it == records_.end()) {
        return true; // Already absent
    }

    records_.erase(it);
    if (!indexPath_.empty()) {
        return saveIndex(indexPath_, error);
    }
    return true;
}

bool ChunkIndex::validateIndex(const std::string& chunksDir, std::string& error) {
    bool allValid = true;
    std::string accumulatedErrors;

    for (auto& kv : records_) {
        ChunkRecord& rec = kv.second;
        fs::path chunkFilePath = fs::path(chunksDir) / rec.filename;
        std::string chunkPathStr = chunkFilePath.lexically_normal().generic_string();

        std::string verifyError;
        if (!ChunkVerifier::verifyFile(chunkPathStr, rec.fingerprint, rec.size, rec.crc32, verifyError)) {
            allValid = false;
            std::error_code ec;
            if (!fs::exists(chunkPathStr, ec)) {
                rec.status = "missing";
                accumulatedErrors += "Missing chunk: " + rec.fingerprint + " at " + chunkPathStr + "\n";
            } else {
                rec.status = "corrupted";
                accumulatedErrors += "Corrupted chunk: " + rec.fingerprint + " (" + verifyError + ")\n";
            }
        } else {
            rec.status = "available";
        }
    }

    if (!allValid) {
        error = accumulatedErrors;
        return false;
    }

    return true;
}

bool ChunkIndex::rebuildFromChunks(const std::string& chunksDir, const std::string& indexPath, std::string& error) {
    std::error_code ec;
    if (!fs::exists(chunksDir, ec) || !fs::is_directory(chunksDir, ec)) {
        error = "Chunks directory does not exist: " + chunksDir;
        return false;
    }

    records_.clear();
    uint64_t nowSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& entry : fs::directory_iterator(chunksDir, ec)) {
        if (!entry.is_regular_file(ec)) continue;

        std::string filename = entry.path().filename().string();
        if (filename.size() != 38 || filename.substr(32) != ".chunk") {
            continue; // Not a chunk file
        }

        std::string fp = filename.substr(0, 32);
        if (!ChunkVerifier::isValidFingerprintFormat(fp)) {
            continue;
        }

        uintmax_t fileSize = entry.file_size(ec);
        if (ec) continue;

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file.is_open()) continue;

        std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
        if (fileSize > 0) {
            file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
            if (file.gcount() != static_cast<std::streamsize>(fileSize)) continue;
        }

        std::string computedFp = ChunkVerifier::computeFingerprint(buffer.data(), buffer.size());
        if (computedFp != fp) {
            // Corrupted chunk, do not index
            continue;
        }

        uint32_t computedCrc = ChunkVerifier::computeCrc32(buffer.data(), buffer.size());

        ChunkRecord rec;
        rec.fingerprint = fp;
        rec.size = static_cast<uint64_t>(fileSize);
        rec.crc32 = computedCrc;
        rec.filename = filename;
        rec.status = "available";
        rec.version = 1;
        rec.createdAt = nowSec;

        records_[fp] = rec;
    }

    indexPath_ = indexPath;
    return saveIndex(indexPath, error);
}

size_t ChunkIndex::size() const {
    return records_.size();
}

std::vector<ChunkRecord> ChunkIndex::getAllRecords() const {
    std::vector<ChunkRecord> results;
    results.reserve(records_.size());
    for (const auto& kv : records_) {
        results.push_back(kv.second);
    }
    return results;
}

void ChunkIndex::clear() {
    records_.clear();
}

} // namespace retro::storage
