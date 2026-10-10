#ifndef CHUNK_METADATA_H
#define CHUNK_METADATA_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Represents metadata for an individual data chunk.
 * 
 * Notice that ChunkMetadata stores only descriptive and structural info,
 * NOT the raw chunk payload bytes. This allows streaming large files
 * without keeping entire files or chunks resident in memory. Raw chunk bytes
 * can be reread on demand using (relativePath, offset, length).
 */
struct ChunkMetadata {
    std::string relativePath;   // Relative path of the original file
    uint64_t offset = 0;        // Byte offset from start of file
    uint64_t length = 0;        // Actual byte length of this chunk
    std::string fingerprint;    // 128-bit hex content fingerprint (MurmurHash3)
    std::string algorithm;      // Algorithm identifier ("fixed" or "cdc")
    uint32_t sequenceNumber = 0;// 0-indexed sequential chunk index within the file
    uint32_t crc32 = 0;         // CRC32 checksum for transmission integrity verification
};

/**
 * @brief Represents a chunk payload when chunk data is read into memory on demand.
 */
struct ChunkPayload {
    ChunkMetadata metadata;
    std::vector<uint8_t> data;
};

#endif // CHUNK_METADATA_H
