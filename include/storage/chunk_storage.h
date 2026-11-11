#ifndef RETROTRANSFER_STORAGE_CHUNK_STORAGE_H
#define RETROTRANSFER_STORAGE_CHUNK_STORAGE_H

#include "storage/chunk_verifier.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace retro::storage {

/**
 * @brief Manages the physical persistence of chunk files on disk.
 *
 * Chunks are stored in <storageDir>/chunks/<fingerprint>.chunk.
 * Writes are atomic and collision-safe using temporary files.
 */
class ChunkStorage {
public:
    explicit ChunkStorage(const std::string& storageDir = "storage");

    /*
     * Ensures storage directories exist on disk.
     */
    bool initialize(std::string& error);

    /*
     * Persistently stores a chunk into <chunksDir>/<fingerprint>.chunk.
     * Checks for collisions and duplicate fingerprints before writing.
     * Uses atomic temporary file writes.
     */
    bool storeChunk(const std::string& fingerprint,
                    const uint8_t* data,
                    size_t size,
                    uint32_t crc32,
                    std::string& error);

    /*
     * Retrieves chunk payload bytes from disk, verifying its integrity.
     */
    bool retrieveChunk(const std::string& fingerprint,
                       std::vector<uint8_t>& outData,
                       std::string& error) const;

    /*
     * Checks if a chunk file exists on disk.
     */
    bool hasChunk(const std::string& fingerprint) const;

    /*
     * Verifies stored chunk file matches expected size, fingerprint and CRC32.
     */
    bool validateChunk(const std::string& fingerprint,
                       uint64_t expectedSize,
                       uint32_t expectedCrc32,
                       std::string& error) const;

    /*
     * Deletes a stored chunk file from disk.
     */
    bool deleteChunk(const std::string& fingerprint, std::string& error);

    /*
     * Returns the full file path for a chunk with the given fingerprint.
     */
    std::string getChunkPath(const std::string& fingerprint) const;

    /*
     * Scans the chunks directory and returns all stored chunk fingerprints.
     */
    std::vector<std::string> listStoredChunkFingerprints() const;

    /*
     * Returns byte size of a stored chunk, or 0 if not found.
     */
    uint64_t getStoredChunkSize(const std::string& fingerprint) const;

    const std::string& getStorageDir() const { return storageDir_; }
    const std::string& getChunksDir() const { return chunksDir_; }

private:
    std::string storageDir_;
    std::string chunksDir_;
};

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_CHUNK_STORAGE_H
