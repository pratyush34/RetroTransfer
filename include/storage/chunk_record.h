#ifndef RETROTRANSFER_STORAGE_CHUNK_RECORD_H
#define RETROTRANSFER_STORAGE_CHUNK_RECORD_H

#include "storage/json_helper.h"

#include <cstdint>
#include <string>

namespace retro::storage {

/**
 * @brief Represents metadata for a chunk stored persistently in the repository.
 */
struct ChunkRecord {
    std::string fingerprint;     // 32-character hexadecimal MurmurHash3 string
    uint64_t size = 0;          // Byte length of chunk payload
    uint32_t crc32 = 0;         // CRC32 checksum for integrity verification
    std::string filename;       // Storage filename relative to chunks/ directory
    std::string status = "available"; // Availability status: "available", "corrupted", "missing"
    uint32_t version = 1;       // Chunk record format version
    uint64_t createdAt = 0;     // Timestamp (epoch seconds) when stored

    bool isValid() const;
    StorageJsonValue toJson() const;
    static bool fromJson(const StorageJsonValue& json, ChunkRecord& outRecord, std::string& error);
};

// Alias ChunkMetadata to ChunkRecord inside namespace retro::storage for OOP requirements
using ChunkMetadata = ChunkRecord;

} // namespace retro::storage

#endif // RETROTRANSFER_STORAGE_CHUNK_RECORD_H
