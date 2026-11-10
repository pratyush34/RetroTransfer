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

int main() {
    std::cout << "========================================\n";
    std::cout << "        RUNNING PROTOCOL TESTS          \n";
    std::cout << "========================================\n";

    test_header_round_trip();
    test_header_invalid_magic();
    test_header_unsupported_version();
    test_header_truncated();
    test_header_invalid_type();
    test_transfer_info_round_trip();
    test_filename_validation();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
