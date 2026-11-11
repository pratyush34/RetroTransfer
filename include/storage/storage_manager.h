#ifndef RETROTRANSFER_STORAGE_STORAGE_MANAGER_H
#define RETROTRANSFER_STORAGE_STORAGE_MANAGER_H

#include "storage/chunk_index.h"
#include "storage/chunk_storage.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retro::storage {

struct StorageConfig {
    uint32_t formatVersion = 1;
    std::string storageDir = "storage";
    std::string chunksSubdir = "chunks";
    std::string indexFilename = "chunk_index.json";
    uint64_t maxChunkSizeBytes = 1024ULL * 1024ULL * 1024ULL; // 1 GiB limit

    StorageJsonValue toJson() const;
    static bool fromJson(const StorageJsonValue& json, StorageConfig& outConfig, std::string& error);
};

/**
 * @brief High-level coordinator managing persistent chunk storage, persistent indexing,
 *        and repository integrity verification.
 */
class StorageManager {
public:
    explicit StorageManager(const std::string& storageDir = "storage");

    /*
     * Initializes the repository: creates directories, loads/rebuilds index,
     * and loads/saves storage configuration.
     */
    bool initialize(const std::string& storageDir, std::string& error);
    bool initialize(std::string& error) { return initialize(storageDir_, error); }

    /*
     * Stores a chunk persistently: validates data, writes file via ChunkStorage,
     * updates ChunkIndex, and persists index to disk.
     */
    bool storeChunk(const std::string& fingerprint,
                    const uint8_t* data,
                    size_t size,
                    uint32_t crc32,
                    std::string& error);

    /*
     * Retrieves chunk payload bytes from persistent storage.
     */
    bool getChunk(const std::string& fingerprint,
                  std::vector<uint8_t>& outData,
                  std::string& error) const;

    /*
     * Checks if a chunk is present and valid in both index and storage.
     */
    bool hasChunk(const std::string& fingerprint) const;

    /*
     * Validates a stored chunk on disk against expected size and CRC32.
     */
    bool validateChunk(const std::string& fingerprint,
                       uint64_t expectedSize,
                       uint32_t expectedCrc32,
                       std::string& error) const {
        return storage_.validateChunk(fingerprint, expectedSize, expectedCrc32, error);
    }

    /*
     * Deletes a chunk from both storage and index.
     */
    bool deleteChunk(const std::string& fingerprint, std::string& error);

    /*
     * Cross-validates index against physical chunk files on disk.
     */
    bool validateRepository(std::string& error);

    /*
     * Rebuilds index by scanning physical chunk files.
     */
    bool rebuildIndex(std::string& error);

    /*
     * Getters and statistics.
     */
    size_t getStoredChunkCount() const;
    uint64_t getTotalStoredBytes() const;
    const std::string& getStorageDir() const { return storageDir_; }
    const ChunkIndex& getIndex() const { return index_; }
    const ChunkStorage& getStorage() const { return storage_; }
    const StorageConfig& getConfig() const { return config_; }

private:
    std::string storageDir_;
    std::string configPath_;
    std::string indexPath_;
    StorageConfig config_;
    ChunkStorage storage_;
    ChunkIndex index_;

    bool loadOrInitConfig(std::string& error);
};

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_STORAGE_MANAGER_H
