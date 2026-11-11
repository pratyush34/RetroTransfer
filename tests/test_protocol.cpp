#include "network/protocol.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace retro::net;

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "FAIL: " << __func__ << " (line " << __LINE__ << "): " << msg << "\n"; \
            g_tests_failed++; \
        } else { \
            std::cout << "  [PASS] " << __func__ << ": " << msg << "\n"; \
        } \
    } while (0)

static void test_header_round_trip() {
    PacketHeader original;
    original.magic = PROTOCOL_MAGIC;
    original.version = PROTOCOL_VERSION;
    original.type = static_cast<uint16_t>(MessageType::TRANSFER_INFO);
    original.payloadLength = 123456789ULL;

    std::vector<uint8_t> buffer;
    TEST_ASSERT(serializeHeader(original, buffer), "Header serialize");
    TEST_ASSERT(buffer.size() == HEADER_SIZE, "Header size is 16 bytes");

    PacketHeader deserialized;
    std::string error;
    TEST_ASSERT(deserializeHeader(buffer.data(), buffer.size(), deserialized, error), "Header deserialize");
    TEST_ASSERT(deserialized.magic == PROTOCOL_MAGIC, "Magic matches");
    TEST_ASSERT(deserialized.version == PROTOCOL_VERSION, "Version matches");
    TEST_ASSERT(deserialized.type == static_cast<uint16_t>(MessageType::TRANSFER_INFO), "Type matches");
    TEST_ASSERT(deserialized.payloadLength == 123456789ULL, "Payload length matches");
}

static void test_header_invalid_magic() {
    PacketHeader hdr;
    hdr.magic = 0xDEADBEEF;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(MessageType::HELLO);
    hdr.payloadLength = 0;

    std::vector<uint8_t> buffer;
    serializeHeader(hdr, buffer);

    PacketHeader out;
    std::string error;
    TEST_ASSERT(!deserializeHeader(buffer.data(), buffer.size(), out, error), "Rejects invalid magic");
    TEST_ASSERT(error.find("magic") != std::string::npos, "Error mentions magic");
}

static void test_header_unsupported_version() {
    PacketHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = 99; // Unsupported version
    hdr.type = static_cast<uint16_t>(MessageType::HELLO);
    hdr.payloadLength = 0;

    std::vector<uint8_t> buffer;
    serializeHeader(hdr, buffer);

    PacketHeader out;
    std::string error;
    TEST_ASSERT(!deserializeHeader(buffer.data(), buffer.size(), out, error), "Rejects unsupported version");
    TEST_ASSERT(error.find("version") != std::string::npos, "Error mentions version");
}

static void test_header_truncated() {
    uint8_t buf[10] = {0};
    PacketHeader out;
    std::string error;
    TEST_ASSERT(!deserializeHeader(buf, sizeof(buf), out, error), "Rejects truncated header (< 16 bytes)");
}

static void test_header_invalid_type() {
    PacketHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = 999; // invalid type
    hdr.payloadLength = 0;

    std::vector<uint8_t> buffer;
    serializeHeader(hdr, buffer);

    PacketHeader out;
    std::string error;
    TEST_ASSERT(!deserializeHeader(buffer.data(), buffer.size(), out, error), "Rejects invalid message type");
}

static void test_transfer_info_round_trip() {
    TransferInfo info;
    info.transferId = "transfer_20261110_01";
    info.manifestFilename = "test_chunks.json";
    info.manifestSize = 1048576;
    info.payloadFilename = "test_bundle.rtpack";
    info.payloadSize = 52428800;
    info.protocolVersion = PROTOCOL_VERSION;

    std::vector<uint8_t> buffer;
    TEST_ASSERT(serializeTransferInfo(info, buffer), "Serialize TransferInfo");

    TransferInfo deserialized;
    std::string error;
    TEST_ASSERT(deserializeTransferInfo(buffer.data(), buffer.size(), deserialized, error), "Deserialize TransferInfo");
    TEST_ASSERT(deserialized.transferId == info.transferId, "Transfer ID matches");
    TEST_ASSERT(deserialized.manifestFilename == info.manifestFilename, "Manifest filename matches");
    TEST_ASSERT(deserialized.manifestSize == info.manifestSize, "Manifest size matches");
    TEST_ASSERT(deserialized.payloadFilename == info.payloadFilename, "Payload filename matches");
    TEST_ASSERT(deserialized.payloadSize == info.payloadSize, "Payload size matches");
}

static void test_byte_order_wire_format() {
    PacketHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(MessageType::TRANSFER_INFO);
    hdr.payloadLength = 0x0102030405060708ULL;

    std::vector<uint8_t> buf;
    TEST_ASSERT(serializeHeader(hdr, buf), "Serialize header for wire check");
    TEST_ASSERT(buf.size() == 16, "Serialized header size is 16");

    // Check big-endian (network byte order) bytes explicitly:
    // Magic 0x5254504B -> 'R', 'T', 'P', 'K'
    TEST_ASSERT(buf[0] == 0x52 && buf[1] == 0x54 && buf[2] == 0x50 && buf[3] == 0x4B, "Magic is network byte order 'RTPK'");
    // Version 1 -> 0x00, 0x01
    TEST_ASSERT(buf[4] == 0x00 && buf[5] == 0x01, "Version 1 is big-endian 0x00, 0x01");
    // Type TRANSFER_INFO (3) -> 0x00, 0x03
    TEST_ASSERT(buf[6] == 0x00 && buf[7] == 0x03, "Type is big-endian 0x00, 0x03");
    // Length 0x0102030405060708
    TEST_ASSERT(buf[8] == 0x01 && buf[9] == 0x02 && buf[10] == 0x03 && buf[11] == 0x04 &&
                buf[12] == 0x05 && buf[13] == 0x06 && buf[14] == 0x07 && buf[15] == 0x08, "Payload length is big-endian 64-bit");
}

static void test_all_message_types() {
    TEST_ASSERT(messageTypeToString(MessageType::HELLO) == "HELLO", "Type HELLO string");
    TEST_ASSERT(messageTypeToString(MessageType::HELLO_ACK) == "HELLO_ACK", "Type HELLO_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::TRANSFER_INFO) == "TRANSFER_INFO", "Type TRANSFER_INFO string");
    TEST_ASSERT(messageTypeToString(MessageType::TRANSFER_INFO_ACK) == "TRANSFER_INFO_ACK", "Type TRANSFER_INFO_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::MANIFEST) == "MANIFEST", "Type MANIFEST string");
    TEST_ASSERT(messageTypeToString(MessageType::MANIFEST_ACK) == "MANIFEST_ACK", "Type MANIFEST_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::PAYLOAD) == "PAYLOAD", "Type PAYLOAD string");
    TEST_ASSERT(messageTypeToString(MessageType::PAYLOAD_ACK) == "PAYLOAD_ACK", "Type PAYLOAD_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::COMPLETE) == "COMPLETE", "Type COMPLETE string");
    TEST_ASSERT(messageTypeToString(MessageType::COMPLETE_ACK) == "COMPLETE_ACK", "Type COMPLETE_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::ERROR_MSG) == "ERROR", "Type ERROR string");
    TEST_ASSERT(messageTypeToString(MessageType::CHUNK_LIST) == "CHUNK_LIST", "Type CHUNK_LIST string");
    TEST_ASSERT(messageTypeToString(MessageType::CHUNK_LIST_ACK) == "CHUNK_LIST_ACK", "Type CHUNK_LIST_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::MISSING_CHUNKS) == "MISSING_CHUNKS", "Type MISSING_CHUNKS string");
    TEST_ASSERT(messageTypeToString(MessageType::MISSING_CHUNKS_ACK) == "MISSING_CHUNKS_ACK", "Type MISSING_CHUNKS_ACK string");
    TEST_ASSERT(messageTypeToString(MessageType::DEDUP_SUMMARY) == "DEDUP_SUMMARY", "Type DEDUP_SUMMARY string");
    TEST_ASSERT(messageTypeToString(MessageType::DEDUP_SUMMARY_ACK) == "DEDUP_SUMMARY_ACK", "Type DEDUP_SUMMARY_ACK string");
}

static void test_zero_length_messages() {
    PacketHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(MessageType::HELLO);
    hdr.payloadLength = 0;

    std::vector<uint8_t> buf;
    serializeHeader(hdr, buf);

    PacketHeader out;
    std::string err;
    TEST_ASSERT(deserializeHeader(buf.data(), buf.size(), out, err), "Zero-length HELLO deserialized");
    TEST_ASSERT(out.payloadLength == 0, "Payload length is 0");

    hdr.type = static_cast<uint16_t>(MessageType::COMPLETE);
    serializeHeader(hdr, buf);
    TEST_ASSERT(deserializeHeader(buf.data(), buf.size(), out, err), "Zero-length COMPLETE deserialized");
    TEST_ASSERT(out.payloadLength == 0, "Payload length is 0");
}

static void test_extremely_large_declared_size() {
    PacketHeader hdr;
    hdr.magic = PROTOCOL_MAGIC;
    hdr.version = PROTOCOL_VERSION;
    hdr.type = static_cast<uint16_t>(MessageType::PAYLOAD);
    hdr.payloadLength = 200ULL * 1024ULL * 1024ULL * 1024ULL; // 200 GiB (> 100 GiB limit)

    std::vector<uint8_t> buf;
    serializeHeader(hdr, buf);

    PacketHeader out;
    std::string err;
    TEST_ASSERT(!deserializeHeader(buf.data(), buf.size(), out, err), "Rejects declared size exceeding 100 GiB limit");
    TEST_ASSERT(err.find("exceeds maximum allowed limit") != std::string::npos, "Error mentions limit exceeded");
}

static void test_malformed_transfer_info() {
    // 1. Truncated transfer info
    std::vector<uint8_t> shortBuf = {0x00, 0x01};
    TransferInfo out;
    std::string err;
    TEST_ASSERT(!deserializeTransferInfo(shortBuf.data(), shortBuf.size(), out, err), "Rejects truncated TRANSFER_INFO");

    // 2. Unsupported protocol version
    TransferInfo badVer;
    badVer.protocolVersion = 99;
    badVer.manifestFilename = "valid.json";
    badVer.payloadFilename = "valid.rtpack";
    std::vector<uint8_t> buf;
    serializeTransferInfo(badVer, buf);
    TEST_ASSERT(!deserializeTransferInfo(buf.data(), buf.size(), out, err), "Rejects unsupported version in TRANSFER_INFO");

    // 3. Traversal in manifest filename
    TransferInfo travManifest;
    travManifest.manifestFilename = "../traversal.json";
    travManifest.payloadFilename = "valid.rtpack";
    serializeTransferInfo(travManifest, buf);
    TEST_ASSERT(!deserializeTransferInfo(buf.data(), buf.size(), out, err), "Rejects path traversal in manifest filename");

    // 4. Traversal in payload filename
    TransferInfo travPayload;
    travPayload.manifestFilename = "valid.json";
    travPayload.payloadFilename = "..\\traversal.rtpack";
    serializeTransferInfo(travPayload, buf);
    TEST_ASSERT(!deserializeTransferInfo(buf.data(), buf.size(), out, err), "Rejects path traversal in payload filename");

    // 5. Manifest size exceeding 64 MiB
    TransferInfo hugeManifest;
    hugeManifest.manifestFilename = "valid.json";
    hugeManifest.payloadFilename = "valid.rtpack";
    hugeManifest.manifestSize = 70ULL * 1024ULL * 1024ULL;
    serializeTransferInfo(hugeManifest, buf);
    TEST_ASSERT(!deserializeTransferInfo(buf.data(), buf.size(), out, err), "Rejects manifest size > 64 MiB in TRANSFER_INFO");
}

static void test_filename_validation() {
    TEST_ASSERT(isValidTransferFilename("manifest.json", ".json"), "Valid json name");
    TEST_ASSERT(isValidTransferFilename("bundle.rtpack", ".rtpack"), "Valid rtpack name");
    TEST_ASSERT(isValidTransferFilename("my_project_v1.rtpack", ".rtpack"), "Valid rtpack with underscores");

    // Path traversal attacks
    TEST_ASSERT(!isValidTransferFilename("../manifest.json", ".json"), "Rejects ../");
    TEST_ASSERT(!isValidTransferFilename("..\\manifest.json", ".json"), "Rejects ..\\");
    TEST_ASSERT(!isValidTransferFilename("dir/manifest.json", ".json"), "Rejects forward slash");
    TEST_ASSERT(!isValidTransferFilename("dir\\manifest.json", ".json"), "Rejects backslash");
    TEST_ASSERT(!isValidTransferFilename(".", ".json"), "Rejects dot");
    TEST_ASSERT(!isValidTransferFilename("..", ".json"), "Rejects double dot");
    TEST_ASSERT(!isValidTransferFilename("", ".json"), "Rejects empty");

    // Wrong extensions
    TEST_ASSERT(!isValidTransferFilename("file.exe", ".json"), "Rejects wrong extension .exe");
    TEST_ASSERT(!isValidTransferFilename("manifest.json", ".rtpack"), "Rejects mismatched extension");
}

static void test_chunk_list_serialization() {
    ChunkListMsg msg;
    ChunkListEntry e1;
    e1.fingerprint = "0123456789abcdef0123456789abcdef";
    e1.originalSize = 4096;
    e1.crc32 = 0x12345678;
    msg.chunks.push_back(e1);

    ChunkListEntry e2;
    e2.fingerprint = "abcdef0123456789abcdef0123456789";
    e2.originalSize = 8192;
    e2.crc32 = 0x87654321;
    msg.chunks.push_back(e2);

    std::vector<uint8_t> buf;
    TEST_ASSERT(serializeChunkList(msg, buf), "Serialize ChunkListMsg");
    TEST_ASSERT(buf.size() == 4 + 2 * 44, "ChunkList wire size is 4 + 2*44");

    ChunkListMsg deserialized;
    std::string err;
    TEST_ASSERT(deserializeChunkList(buf.data(), buf.size(), deserialized, err), "Deserialize ChunkListMsg");
    TEST_ASSERT(deserialized.chunks.size() == 2, "Chunk count matches 2");
    TEST_ASSERT(deserialized.chunks[0].fingerprint == e1.fingerprint, "Entry 1 fingerprint matches");
    TEST_ASSERT(deserialized.chunks[0].originalSize == 4096, "Entry 1 size matches");
    TEST_ASSERT(deserialized.chunks[0].crc32 == 0x12345678, "Entry 1 crc32 matches");
    TEST_ASSERT(deserialized.chunks[1].fingerprint == e2.fingerprint, "Entry 2 fingerprint matches");

    // Test rejection of invalid payload size
    buf.pop_back();
    TEST_ASSERT(!deserializeChunkList(buf.data(), buf.size(), deserialized, err), "Rejects truncated CHUNK_LIST");
}

static void test_missing_chunks_serialization() {
    MissingChunksMsg msg;
    msg.totalChunks = 10;
    msg.availableChunks = 8;
    msg.missingChunks = 2;
    msg.availableBytes = 32768;
    msg.missingBytes = 8192;
    msg.missingFingerprints = {
        "1111222233334444aaaabbbbccccdddd",
        "5555666677778888eeeeffff00001111"
    };

    std::vector<uint8_t> buf;
    TEST_ASSERT(serializeMissingChunks(msg, buf), "Serialize MissingChunksMsg");
    TEST_ASSERT(buf.size() == 28 + 2 * 32, "MissingChunks wire size is 28 + 2*32");

    MissingChunksMsg deserialized;
    std::string err;
    TEST_ASSERT(deserializeMissingChunks(buf.data(), buf.size(), deserialized, err), "Deserialize MissingChunksMsg");
    TEST_ASSERT(deserialized.totalChunks == 10, "Total chunks matches 10");
    TEST_ASSERT(deserialized.availableChunks == 8, "Available chunks matches 8");
    TEST_ASSERT(deserialized.missingChunks == 2, "Missing chunks matches 2");
    TEST_ASSERT(deserialized.availableBytes == 32768, "Available bytes matches 32768");
    TEST_ASSERT(deserialized.missingBytes == 8192, "Missing bytes matches 8192");
    TEST_ASSERT(deserialized.missingFingerprints.size() == 2, "Missing fingerprints count matches 2");
    TEST_ASSERT(deserialized.missingFingerprints[0] == msg.missingFingerprints[0], "Missing fp 0 matches");
    TEST_ASSERT(deserialized.missingFingerprints[1] == msg.missingFingerprints[1], "Missing fp 1 matches");

    // Inconsistent counts rejection
    msg.availableChunks = 7; // total(10) != avail(7) + missing(2)
    serializeMissingChunks(msg, buf);
    TEST_ASSERT(!deserializeMissingChunks(buf.data(), buf.size(), deserialized, err), "Rejects inconsistent total != avail + missing");
}

static void test_dedup_summary_serialization() {
    DedupSummaryMsg msg;
    msg.totalProjectChunks = 50;
    msg.availableChunks = 40;
    msg.missingChunks = 10;
    msg.transferredChunks = 10;
    msg.totalOriginalBytes = 200000;
    msg.deduplicatedBytes = 160000;
    msg.transmittedPayloadBytes = 18000;
    msg.compressionSavingsBytes = 22000;
    msg.actualTransmittedBytes = 25000;
    msg.totalTransferTimeMs = 150;

    std::vector<uint8_t> buf;
    TEST_ASSERT(serializeDedupSummary(msg, buf), "Serialize DedupSummaryMsg");
    TEST_ASSERT(buf.size() == 64, "DedupSummary wire size is 64 bytes");

    DedupSummaryMsg deserialized;
    std::string err;
    TEST_ASSERT(deserializeDedupSummary(buf.data(), buf.size(), deserialized, err), "Deserialize DedupSummaryMsg");
    TEST_ASSERT(deserialized.totalProjectChunks == 50, "Total project chunks 50");
    TEST_ASSERT(deserialized.availableChunks == 40, "Available chunks 40");
    TEST_ASSERT(deserialized.missingChunks == 10, "Missing chunks 10");
    TEST_ASSERT(deserialized.transferredChunks == 10, "Transferred chunks 10");
    TEST_ASSERT(deserialized.totalOriginalBytes == 200000, "Total original bytes 200000");
    TEST_ASSERT(deserialized.deduplicatedBytes == 160000, "Deduplicated bytes 160000");
    TEST_ASSERT(deserialized.transmittedPayloadBytes == 18000, "Transmitted payload bytes 18000");
    TEST_ASSERT(deserialized.compressionSavingsBytes == 22000, "Compression savings bytes 22000");
    TEST_ASSERT(deserialized.actualTransmittedBytes == 25000, "Actual transmitted bytes 25000");
    TEST_ASSERT(deserialized.totalTransferTimeMs == 150, "Total transfer time 150 ms");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "        RUNNING PROTOCOL TESTS          \n";
    std::cout << "========================================\n";

    test_byte_order_wire_format();
    test_all_message_types();
    test_header_round_trip();
    test_header_invalid_magic();
    test_header_unsupported_version();
    test_header_truncated();
    test_header_invalid_type();
    test_zero_length_messages();
    test_extremely_large_declared_size();
    test_transfer_info_round_trip();
    test_malformed_transfer_info();
    test_filename_validation();
    test_chunk_list_serialization();
    test_missing_chunks_serialization();
    test_dedup_summary_serialization();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
