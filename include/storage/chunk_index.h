#ifndef RETROTRANSFER_STORAGE_CHUNK_INDEX_H
#define RETROTRANSFER_STORAGE_CHUNK_INDEX_H

#include "storage/chunk_record.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace retro::storage {

/**
 * @brief Manages the in-memory metadata index of stored chunks and serializes
 *        it persistently to chunk_index.json.
 */
class ChunkIndex {
public:
    ChunkIndex() = default;

    /*
     * Initialize index: sets index file path and loads it if existing.
     * If missing, initializes an empty index and creates the file.
     */
    bool initialize(const std::string& indexPath, std::string& error);

    /*
     * Loads the chunk index from the specified JSON file.
     * Rejects malformed JSON with descriptive errors.
     */
    bool loadIndex(const std::string& indexPath, std::string& error);

    /*
     * Saves the current index atomically to the specified JSON file.
     * Writes to a temporary file first and replaces atomically.
     */
    bool saveIndex(const std::string& indexPath, std::string& error) const;

    /*
     * Finds chunk metadata by fingerprint. Returns true if found.
     */
    bool findChunk(const std::string& fingerprint, ChunkRecord& outRecord) const;

    /*
     * Checks if a chunk fingerprint exists in the index.
     */
    bool containsChunk(const std::string& fingerprint) const;

    /*
     * Inserts a new chunk record. Rejects invalid records or duplicate fingerprints.
     */
    bool insertChunk(const ChunkRecord& record, std::string& error);

    /*
     * Updates an existing chunk record.
     */
    bool updateChunk(const ChunkRecord& record, std::string& error);

    /*
     * Removes a chunk record by fingerprint.
     */
    bool removeChunk(const std::string& fingerprint, std::string& error);

    /*
     * Validates that all index entries point to valid, non-corrupted chunk files on disk.
     * Identifies missing or corrupted chunk files.
     */
    bool validateIndex(const std::string& chunksDir, std::string& error);

    /*
     * Rebuilds the index by scanning all .chunk files in chunksDir, verifying them,
     * and writing the fresh index to indexPath.
     */
    bool rebuildFromChunks(const std::string& chunksDir, const std::string& indexPath, std::string& error);

    /*
     * Returns total number of indexed chunks.
     */
    size_t size() const;

    /*
     * Returns all indexed records.
     */
    std::vector<ChunkRecord> getAllRecords() const;

    /*
     * Clears all in-memory entries.
     */
    void clear();

    const std::string& getIndexPath() const { return indexPath_; }

private:
    std::string indexPath_;
    std::unordered_map<std::string, ChunkRecord> records_;
};

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_CHUNK_INDEX_H
