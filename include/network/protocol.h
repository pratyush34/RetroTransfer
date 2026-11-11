#ifndef RETROTRANSFER_PROTOCOL_H
#define RETROTRANSFER_PROTOCOL_H

#include "network/socket_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace retro::net {

constexpr uint32_t PROTOCOL_MAGIC = 0x5254504B; /* ASCII for 'R', 'T', 'P', 'K' */
constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr size_t HEADER_SIZE = 16;
constexpr size_t MAX_MANIFEST_SIZE = 64ULL * 1024ULL * 1024ULL;       /* 64 MiB limit */
constexpr uint64_t MAX_PAYLOAD_SIZE = 100ULL * 1024ULL * 1024ULL * 1024ULL; /* 100 GiB limit */

enum class MessageType : uint16_t {
    HELLO             = 1,
    HELLO_ACK         = 2,
    TRANSFER_INFO     = 3,
    TRANSFER_INFO_ACK = 4,
    MANIFEST          = 5,
    MANIFEST_ACK      = 6,
    PAYLOAD           = 7,
    PAYLOAD_ACK       = 8,
    COMPLETE          = 9,
    COMPLETE_ACK      = 10,
    ERROR_MSG         = 11,
    CHUNK_LIST        = 12,
    CHUNK_LIST_ACK    = 13,
    MISSING_CHUNKS    = 14,
    MISSING_CHUNKS_ACK= 15,
    DEDUP_SUMMARY     = 16,
    DEDUP_SUMMARY_ACK = 17
};

std::string messageTypeToString(MessageType type);

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic = PROTOCOL_MAGIC;
    uint16_t version = PROTOCOL_VERSION;
    uint16_t type = static_cast<uint16_t>(MessageType::HELLO);
    uint64_t payloadLength = 0;
};
#pragma pack(pop)

struct TransferInfo {
    std::string transferId;
    std::string manifestFilename;
    uint64_t manifestSize = 0;
    std::string payloadFilename;
    uint64_t payloadSize = 0;
    uint32_t protocolVersion = PROTOCOL_VERSION;
};

/*
 * Remote deduplication message structures.
 */
struct ChunkListEntry {
    std::string fingerprint; // 32 hex characters
    uint64_t originalSize = 0;
    uint32_t crc32 = 0;
};

struct ChunkListMsg {
    std::vector<ChunkListEntry> chunks;
};

struct MissingChunksMsg {
    uint32_t totalChunks = 0;
    uint32_t availableChunks = 0;
    uint32_t missingChunks = 0;
    uint64_t availableBytes = 0;
    uint64_t missingBytes = 0;
    std::vector<std::string> missingFingerprints;
};

struct DedupSummaryMsg {
    uint32_t totalProjectChunks = 0;
    uint32_t availableChunks = 0;
    uint32_t missingChunks = 0;
    uint32_t transferredChunks = 0;
    uint64_t totalOriginalBytes = 0;
    uint64_t deduplicatedBytes = 0;
    uint64_t transmittedPayloadBytes = 0;
    uint64_t compressionSavingsBytes = 0;
    uint64_t actualTransmittedBytes = 0;
    uint64_t totalTransferTimeMs = 0;
};

/*
 * Header serialization & deserialization.
 * Enforces network byte order (big endian) and rigorous validation.
 */
bool serializeHeader(const PacketHeader& header, std::vector<uint8_t>& out);
bool deserializeHeader(const uint8_t* buffer, size_t size, PacketHeader& outHeader, std::string& error);

/*
 * Transfer metadata serialization & deserialization.
 */
bool serializeTransferInfo(const TransferInfo& info, std::vector<uint8_t>& out);
bool deserializeTransferInfo(const uint8_t* buffer, size_t size, TransferInfo& outInfo, std::string& error);

/*
 * Remote deduplication message serialization & deserialization.
 */
bool serializeChunkList(const ChunkListMsg& msg, std::vector<uint8_t>& out);
bool deserializeChunkList(const uint8_t* buffer, size_t size, ChunkListMsg& outMsg, std::string& error);

bool serializeMissingChunks(const MissingChunksMsg& msg, std::vector<uint8_t>& out);
bool deserializeMissingChunks(const uint8_t* buffer, size_t size, MissingChunksMsg& outMsg, std::string& error);

bool serializeDedupSummary(const DedupSummaryMsg& msg, std::vector<uint8_t>& out);
bool deserializeDedupSummary(const uint8_t* buffer, size_t size, DedupSummaryMsg& outMsg, std::string& error);

/*
 * Safe filename validation to protect against directory traversal and malicious paths.
 */
bool isValidTransferFilename(const std::string& filename, const std::string& expectedExtension);

/*
 * Socket-level framing helpers.
 */
bool sendPacketHeader(socket_t sock, const PacketHeader& header);
bool receivePacketHeader(socket_t sock, PacketHeader& header, std::string& error);

bool sendFramedMessage(socket_t sock, MessageType type, const std::vector<uint8_t>& payload);
bool receiveFramedMessage(socket_t sock, PacketHeader& header, std::vector<uint8_t>& payload, std::string& error);

/*
 * Send error message over socket.
 */
bool sendErrorMessage(socket_t sock, const std::string& errorMessage);

} // namespace retro::net

#endif // RETROTRANSFER_PROTOCOL_H
