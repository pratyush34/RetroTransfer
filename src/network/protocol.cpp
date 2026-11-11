#include "network/protocol.h"

#include <cstring>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace retro::net {

namespace {

static inline bool isValidHexFingerprint(const std::string& fp) {
    if (fp.size() != 32) return false;
    for (char c : fp) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

static inline uint16_t hostToNet16(uint16_t val) {
    return htons(val);
}

static inline uint16_t netToHost16(uint16_t val) {
    return ntohs(val);
}

static inline uint32_t hostToNet32(uint32_t val) {
    return htonl(val);
}

static inline uint32_t netToHost32(uint32_t val) {
    return ntohl(val);
}

static inline uint64_t hostToNet64(uint64_t val) {
#if defined(__GNUC__) || defined(__clang__)
    #if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return val;
    #else
    return __builtin_bswap64(val);
    #endif
#elif defined(_MSC_VER)
    return _byteswap_uint64(val);
#else
    return (((uint64_t)htonl(static_cast<uint32_t>(val & 0xFFFFFFFFULL))) << 32) |
           htonl(static_cast<uint32_t>(val >> 32));
#endif
}

static inline uint64_t netToHost64(uint64_t val) {
    return hostToNet64(val);
}

void writeUint16(std::vector<uint8_t>& buf, uint16_t val) {
    uint16_t netVal = hostToNet16(val);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&netVal);
    buf.insert(buf.end(), p, p + 2);
}

void writeUint32(std::vector<uint8_t>& buf, uint32_t val) {
    uint32_t netVal = hostToNet32(val);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&netVal);
    buf.insert(buf.end(), p, p + 4);
}

void writeUint64(std::vector<uint8_t>& buf, uint64_t val) {
    uint64_t netVal = hostToNet64(val);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&netVal);
    buf.insert(buf.end(), p, p + 8);
}

bool readUint16(const uint8_t* buf, size_t size, size_t& offset, uint16_t& out) {
    if (offset + 2 > size) return false;
    uint16_t netVal = 0;
    std::memcpy(&netVal, buf + offset, 2);
    out = netToHost16(netVal);
    offset += 2;
    return true;
}

bool readUint32(const uint8_t* buf, size_t size, size_t& offset, uint32_t& out) {
    if (offset + 4 > size) return false;
    uint32_t netVal = 0;
    std::memcpy(&netVal, buf + offset, 4);
    out = netToHost32(netVal);
    offset += 4;
    return true;
}

bool readUint64(const uint8_t* buf, size_t size, size_t& offset, uint64_t& out) {
    if (offset + 8 > size) return false;
    uint64_t netVal = 0;
    std::memcpy(&netVal, buf + offset, 8);
    out = netToHost64(netVal);
    offset += 8;
    return true;
}

void writeString(std::vector<uint8_t>& buf, const std::string& str) {
    writeUint32(buf, static_cast<uint32_t>(str.size()));
    buf.insert(buf.end(), str.begin(), str.end());
}

bool readString(const uint8_t* buf, size_t size, size_t& offset, std::string& out, size_t maxLen) {
    uint32_t len = 0;
    if (!readUint32(buf, size, offset, len)) return false;
    if (len > maxLen || offset + len > size) return false;
    out.assign(reinterpret_cast<const char*>(buf + offset), len);
    offset += len;
    return true;
}

} // anonymous namespace

std::string messageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::HELLO: return "HELLO";
        case MessageType::HELLO_ACK: return "HELLO_ACK";
        case MessageType::TRANSFER_INFO: return "TRANSFER_INFO";
        case MessageType::TRANSFER_INFO_ACK: return "TRANSFER_INFO_ACK";
        case MessageType::MANIFEST: return "MANIFEST";
        case MessageType::MANIFEST_ACK: return "MANIFEST_ACK";
        case MessageType::PAYLOAD: return "PAYLOAD";
        case MessageType::PAYLOAD_ACK: return "PAYLOAD_ACK";
        case MessageType::COMPLETE: return "COMPLETE";
        case MessageType::COMPLETE_ACK: return "COMPLETE_ACK";
        case MessageType::ERROR_MSG: return "ERROR";
        case MessageType::CHUNK_LIST: return "CHUNK_LIST";
        case MessageType::CHUNK_LIST_ACK: return "CHUNK_LIST_ACK";
        case MessageType::MISSING_CHUNKS: return "MISSING_CHUNKS";
        case MessageType::MISSING_CHUNKS_ACK: return "MISSING_CHUNKS_ACK";
        case MessageType::DEDUP_SUMMARY: return "DEDUP_SUMMARY";
        case MessageType::DEDUP_SUMMARY_ACK: return "DEDUP_SUMMARY_ACK";
        default: return "UNKNOWN (" + std::to_string(static_cast<uint16_t>(type)) + ")";
    }
}

bool serializeHeader(const PacketHeader& header, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(HEADER_SIZE);
    writeUint32(out, header.magic);
    writeUint16(out, header.version);
    writeUint16(out, header.type);
    writeUint64(out, header.payloadLength);
    return true;
}

bool deserializeHeader(const uint8_t* buffer, size_t size, PacketHeader& outHeader, std::string& error) {
    if (size < HEADER_SIZE) {
        error = "Packet header truncated: expected " + std::to_string(HEADER_SIZE) +
                " bytes, got " + std::to_string(size);
        return false;
    }

    size_t offset = 0;
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t type = 0;
    uint64_t length = 0;

    readUint32(buffer, size, offset, magic);
    readUint16(buffer, size, offset, version);
    readUint16(buffer, size, offset, type);
    readUint64(buffer, size, offset, length);

    if (magic != PROTOCOL_MAGIC) {
        error = "Invalid magic number in packet header: 0x" + std::to_string(magic);
        return false;
    }
    if (version != PROTOCOL_VERSION) {
        error = "Unsupported protocol version: " + std::to_string(version) +
                " (expected " + std::to_string(PROTOCOL_VERSION) + ")";
        return false;
    }
    if (type < static_cast<uint16_t>(MessageType::HELLO) ||
        type > static_cast<uint16_t>(MessageType::DEDUP_SUMMARY_ACK)) {
        error = "Invalid or unrecognized message type: " + std::to_string(type);
        return false;
    }
    if (length > MAX_PAYLOAD_SIZE) {
        error = "Payload length exceeds maximum allowed limit: " + std::to_string(length);
        return false;
    }

    outHeader.magic = magic;
    outHeader.version = version;
    outHeader.type = type;
    outHeader.payloadLength = length;
    return true;
}

bool serializeTransferInfo(const TransferInfo& info, std::vector<uint8_t>& out) {
    out.clear();
    writeUint32(out, info.protocolVersion);
    writeUint64(out, info.manifestSize);
    writeUint64(out, info.payloadSize);
    writeString(out, info.transferId);
    writeString(out, info.manifestFilename);
    writeString(out, info.payloadFilename);
    return true;
}

bool deserializeTransferInfo(const uint8_t* buffer, size_t size, TransferInfo& outInfo, std::string& error) {
    size_t offset = 0;
    uint32_t protoVer = 0;
    uint64_t manifestSz = 0;
    uint64_t payloadSz = 0;
    std::string transferId;
    std::string manifestName;
    std::string payloadName;

    if (!readUint32(buffer, size, offset, protoVer) ||
        !readUint64(buffer, size, offset, manifestSz) ||
        !readUint64(buffer, size, offset, payloadSz) ||
        !readString(buffer, size, offset, transferId, 256) ||
        !readString(buffer, size, offset, manifestName, 256) ||
        !readString(buffer, size, offset, payloadName, 256)) {
        error = "Malformed or truncated TRANSFER_INFO payload";
        return false;
    }

    if (protoVer != PROTOCOL_VERSION) {
        error = "TRANSFER_INFO specifies unsupported protocol version: " + std::to_string(protoVer);
        return false;
    }

    if (!isValidTransferFilename(manifestName, ".json")) {
        error = "Invalid manifest filename in TRANSFER_INFO: " + manifestName;
        return false;
    }

    if (!isValidTransferFilename(payloadName, ".rtpack")) {
        error = "Invalid payload filename in TRANSFER_INFO: " + payloadName;
        return false;
    }

    if (manifestSz > MAX_MANIFEST_SIZE) {
        error = "Manifest size in TRANSFER_INFO exceeds 64 MiB limit: " + std::to_string(manifestSz);
        return false;
    }

    outInfo.protocolVersion = protoVer;
    outInfo.manifestSize = manifestSz;
    outInfo.payloadSize = payloadSz;
    outInfo.transferId = transferId;
    outInfo.manifestFilename = manifestName;
    outInfo.payloadFilename = payloadName;
    return true;
}

bool serializeChunkList(const ChunkListMsg& msg, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t count = static_cast<uint32_t>(msg.chunks.size());
    out.reserve(4 + static_cast<size_t>(count) * 44);
    writeUint32(out, count);
    for (const auto& chunk : msg.chunks) {
        if (chunk.fingerprint.size() != 32) return false;
        out.insert(out.end(), chunk.fingerprint.begin(), chunk.fingerprint.end());
        writeUint64(out, chunk.originalSize);
        writeUint32(out, chunk.crc32);
    }
    return true;
}

bool deserializeChunkList(const uint8_t* buffer, size_t size, ChunkListMsg& outMsg, std::string& error) {
    outMsg.chunks.clear();
    if (size < 4) {
        error = "CHUNK_LIST payload truncated (minimum 4 bytes required)";
        return false;
    }
    size_t offset = 0;
    uint32_t count = 0;
    if (!readUint32(buffer, size, offset, count)) {
        error = "Failed to read chunk count in CHUNK_LIST";
        return false;
    }
    constexpr uint32_t MAX_CHUNKS = 1000000;
    if (count > MAX_CHUNKS) {
        error = "CHUNK_LIST chunk count exceeds maximum allowed limit (1,000,000): " + std::to_string(count);
        return false;
    }
    if (size != 4 + static_cast<size_t>(count) * 44) {
        error = "CHUNK_LIST payload size mismatch: expected " +
                std::to_string(4 + static_cast<size_t>(count) * 44) + " bytes, got " + std::to_string(size);
        return false;
    }
    outMsg.chunks.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string fp(reinterpret_cast<const char*>(buffer + offset), 32);
        offset += 32;
        if (!isValidHexFingerprint(fp)) {
            error = "Invalid fingerprint format at index " + std::to_string(i) + " in CHUNK_LIST: " + fp;
            return false;
        }
        uint64_t origSize = 0;
        uint32_t crc = 0;
        readUint64(buffer, size, offset, origSize);
        readUint32(buffer, size, offset, crc);
        ChunkListEntry entry;
        entry.fingerprint = std::move(fp);
        entry.originalSize = origSize;
        entry.crc32 = crc;
        outMsg.chunks.push_back(std::move(entry));
    }
    return true;
}

bool serializeMissingChunks(const MissingChunksMsg& msg, std::vector<uint8_t>& out) {
    out.clear();
    uint32_t missingCount = static_cast<uint32_t>(msg.missingFingerprints.size());
    out.reserve(28 + static_cast<size_t>(missingCount) * 32);
    writeUint32(out, msg.totalChunks);
    writeUint32(out, msg.availableChunks);
    writeUint32(out, missingCount);
    writeUint64(out, msg.availableBytes);
    writeUint64(out, msg.missingBytes);
    for (const auto& fp : msg.missingFingerprints) {
        if (fp.size() != 32) return false;
        out.insert(out.end(), fp.begin(), fp.end());
    }
    return true;
}

bool deserializeMissingChunks(const uint8_t* buffer, size_t size, MissingChunksMsg& outMsg, std::string& error) {
    outMsg = {};
    if (size < 28) {
        error = "MISSING_CHUNKS payload truncated (minimum 28 bytes required)";
        return false;
    }
    size_t offset = 0;
    uint32_t total = 0;
    uint32_t avail = 0;
    uint32_t missing = 0;
    uint64_t availBytes = 0;
    uint64_t missingBytes = 0;
    readUint32(buffer, size, offset, total);
    readUint32(buffer, size, offset, avail);
    readUint32(buffer, size, offset, missing);
    readUint64(buffer, size, offset, availBytes);
    readUint64(buffer, size, offset, missingBytes);

    if (total != avail + missing) {
        error = "MISSING_CHUNKS inconsistent counts: total=" + std::to_string(total) +
                " != available(" + std::to_string(avail) + ") + missing(" + std::to_string(missing) + ")";
        return false;
    }
    if (size != 28 + static_cast<size_t>(missing) * 32) {
        error = "MISSING_CHUNKS payload size mismatch: expected " +
                std::to_string(28 + static_cast<size_t>(missing) * 32) + " bytes, got " + std::to_string(size);
        return false;
    }
    outMsg.totalChunks = total;
    outMsg.availableChunks = avail;
    outMsg.missingChunks = missing;
    outMsg.availableBytes = availBytes;
    outMsg.missingBytes = missingBytes;
    outMsg.missingFingerprints.reserve(missing);
    for (uint32_t i = 0; i < missing; ++i) {
        std::string fp(reinterpret_cast<const char*>(buffer + offset), 32);
        offset += 32;
        if (!isValidHexFingerprint(fp)) {
            error = "Invalid fingerprint in MISSING_CHUNKS: " + fp;
            return false;
        }
        outMsg.missingFingerprints.push_back(std::move(fp));
    }
    return true;
}

bool serializeDedupSummary(const DedupSummaryMsg& msg, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(64);
    writeUint32(out, msg.totalProjectChunks);
    writeUint32(out, msg.availableChunks);
    writeUint32(out, msg.missingChunks);
    writeUint32(out, msg.transferredChunks);
    writeUint64(out, msg.totalOriginalBytes);
    writeUint64(out, msg.deduplicatedBytes);
    writeUint64(out, msg.transmittedPayloadBytes);
    writeUint64(out, msg.compressionSavingsBytes);
    writeUint64(out, msg.actualTransmittedBytes);
    writeUint64(out, msg.totalTransferTimeMs);
    return true;
}

bool deserializeDedupSummary(const uint8_t* buffer, size_t size, DedupSummaryMsg& outMsg, std::string& error) {
    if (size < 64) {
        error = "DEDUP_SUMMARY payload truncated: expected 64 bytes, got " + std::to_string(size);
        return false;
    }
    size_t offset = 0;
    readUint32(buffer, size, offset, outMsg.totalProjectChunks);
    readUint32(buffer, size, offset, outMsg.availableChunks);
    readUint32(buffer, size, offset, outMsg.missingChunks);
    readUint32(buffer, size, offset, outMsg.transferredChunks);
    readUint64(buffer, size, offset, outMsg.totalOriginalBytes);
    readUint64(buffer, size, offset, outMsg.deduplicatedBytes);
    readUint64(buffer, size, offset, outMsg.transmittedPayloadBytes);
    readUint64(buffer, size, offset, outMsg.compressionSavingsBytes);
    readUint64(buffer, size, offset, outMsg.actualTransmittedBytes);
    readUint64(buffer, size, offset, outMsg.totalTransferTimeMs);
    return true;
}

bool isValidTransferFilename(const std::string& filename, const std::string& expectedExtension) {
    if (filename.empty() || filename.size() > 255) return false;

    // Reject path traversal and directory separators
    if (filename.find('/') != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos ||
        filename == "." || filename == "..") {
        return false;
    }

    // Verify extension
    if (!expectedExtension.empty()) {
        std::filesystem::path p(filename);
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string exp = expectedExtension;
        for (char& c : exp) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != exp) return false;
    }

    // Reject illegal control or reserved filename characters
    for (char c : filename) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == ':' || uc == '*' || uc == '?' ||
            uc == '"' || uc == '<' || uc == '>' || uc == '|') {
            return false;
        }
    }

    return true;
}

bool sendPacketHeader(socket_t sock, const PacketHeader& header) {
    std::vector<uint8_t> bytes;
    serializeHeader(header, bytes);
    return sendAll(sock, bytes.data(), bytes.size());
}

bool receivePacketHeader(socket_t sock, PacketHeader& header, std::string& error) {
    uint8_t buf[HEADER_SIZE];
    if (!receiveAll(sock, buf, HEADER_SIZE)) {
        error = "Failed to receive packet header (connection closed or timed out)";
        return false;
    }
    return deserializeHeader(buf, HEADER_SIZE, header, error);
}

bool sendFramedMessage(socket_t sock, MessageType type, const std::vector<uint8_t>& payload) {
    PacketHeader header;
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint16_t>(type);
    header.payloadLength = payload.size();

    if (!sendPacketHeader(sock, header)) {
        return false;
    }

    if (!payload.empty()) {
        if (!sendAll(sock, payload.data(), payload.size())) {
            return false;
        }
    }
    return true;
}

bool receiveFramedMessage(socket_t sock, PacketHeader& header, std::vector<uint8_t>& payload, std::string& error) {
    if (!receivePacketHeader(sock, header, error)) {
        return false;
    }

    payload.clear();
    if (header.payloadLength > 0) {
        if (header.payloadLength > MAX_MANIFEST_SIZE &&
            header.type != static_cast<uint16_t>(MessageType::PAYLOAD)) {
            error = "Message payload too large for in-memory message: " + std::to_string(header.payloadLength);
            return false;
        }
        payload.resize(static_cast<size_t>(header.payloadLength));
        if (!receiveAll(sock, payload.data(), payload.size())) {
            error = "Failed to receive complete message body";
            return false;
        }
    }

    return true;
}

bool sendErrorMessage(socket_t sock, const std::string& errorMessage) {
    std::vector<uint8_t> payload(errorMessage.begin(), errorMessage.end());
    return sendFramedMessage(sock, MessageType::ERROR_MSG, payload);
}

} // namespace retro::net
