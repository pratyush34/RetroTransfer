#include "network/tcp_client.h"
#include "network/tcp_server.h"
#include "network/protocol.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
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

static void test_handshake_and_info() {
    TcpServer server;
    // Bind to port 0 to get an ephemeral free port
    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server started on ephemeral port");
    uint16_t port = server.getPort();
    TEST_ASSERT(port > 0, "Server assigned non-zero port");

    std::thread serverThread([&]() {
        std::string clientIp;
        uint16_t clientPort = 0;
        socket_t clientSock = server.acceptClient(clientIp, clientPort, 5000);
        TEST_ASSERT(clientSock != INVALID_SOCKET_FD, "Server accepted client");

        // 1. Receive HELLO
        PacketHeader hdr;
        std::vector<uint8_t> payload;
        std::string error;
        TEST_ASSERT(receiveFramedMessage(clientSock, hdr, payload, error), "Server received HELLO message");
        TEST_ASSERT(hdr.type == static_cast<uint16_t>(MessageType::HELLO), "Message is HELLO");

        // Send HELLO_ACK
        TEST_ASSERT(sendFramedMessage(clientSock, MessageType::HELLO_ACK, {}), "Server sent HELLO_ACK");

        // 2. Receive TRANSFER_INFO
        TEST_ASSERT(receiveFramedMessage(clientSock, hdr, payload, error), "Server received TRANSFER_INFO");
        TEST_ASSERT(hdr.type == static_cast<uint16_t>(MessageType::TRANSFER_INFO), "Message is TRANSFER_INFO");
        TransferInfo info;
        TEST_ASSERT(deserializeTransferInfo(payload.data(), payload.size(), info, error), "Deserialized info");
        TEST_ASSERT(info.manifestFilename == "test_chunks.json", "Info manifest filename matches");

        // Send TRANSFER_INFO_ACK
        TEST_ASSERT(sendFramedMessage(clientSock, MessageType::TRANSFER_INFO_ACK, {}), "Server sent TRANSFER_INFO_ACK");

        closeSocket(clientSock);
    });

    TcpClient client;
    TEST_ASSERT(client.connect("127.0.0.1", port, 3000), "Client connected to server");

    // Send HELLO
    TEST_ASSERT(sendFramedMessage(client.getSocket(), MessageType::HELLO, {}), "Client sent HELLO");

    // Receive HELLO_ACK
    PacketHeader respHdr;
    std::vector<uint8_t> respPayload;
    std::string err;
    TEST_ASSERT(receiveFramedMessage(client.getSocket(), respHdr, respPayload, err), "Client received response");
    TEST_ASSERT(respHdr.type == static_cast<uint16_t>(MessageType::HELLO_ACK), "Response is HELLO_ACK");

    // Send TRANSFER_INFO
    TransferInfo tInfo;
    tInfo.transferId = "xfer_101";
    tInfo.manifestFilename = "test_chunks.json";
    tInfo.manifestSize = 100;
    tInfo.payloadFilename = "test.rtpack";
    tInfo.payloadSize = 500;
    std::vector<uint8_t> infoBytes;
    serializeTransferInfo(tInfo, infoBytes);
    TEST_ASSERT(sendFramedMessage(client.getSocket(), MessageType::TRANSFER_INFO, infoBytes), "Client sent TRANSFER_INFO");

    // Receive TRANSFER_INFO_ACK
    TEST_ASSERT(receiveFramedMessage(client.getSocket(), respHdr, respPayload, err), "Client received TRANSFER_INFO_ACK");
    TEST_ASSERT(respHdr.type == static_cast<uint16_t>(MessageType::TRANSFER_INFO_ACK), "Response is TRANSFER_INFO_ACK");

    client.disconnect();
    serverThread.join();
    server.stop();
}

static void test_connection_failure() {
    TcpClient client;
    // Connecting to closed port should fail cleanly without crashing
    bool connected = client.connect("127.0.0.1", 54321, 500);
    TEST_ASSERT(!connected, "Connect to non-listening port fails gracefully");
    TEST_ASSERT(!client.getLastError().empty(), "Error message is recorded");
}

static void test_disconnect_detection() {
    TcpServer server;
    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server started");
    uint16_t port = server.getPort();

    std::thread serverThread([&]() {
        std::string ip;
        uint16_t clientPort = 0;
        socket_t clientSock = server.acceptClient(ip, clientPort, 5000);
        TEST_ASSERT(clientSock != INVALID_SOCKET_FD, "Accepted connection");

        // Expect client to abruptly disconnect
        PacketHeader hdr;
        std::string err;
        bool res = receivePacketHeader(clientSock, hdr, err);
        TEST_ASSERT(!res, "Detected client disconnect cleanly (receivePacketHeader returned false)");

        closeSocket(clientSock);
    });

    TcpClient client;
    TEST_ASSERT(client.connect("127.0.0.1", port, 2000), "Client connected");
    // Client disconnects immediately without sending header
    client.disconnect();

    serverThread.join();
    server.stop();
}

static void test_sequential_connections() {
    TcpServer server;
    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server started on ephemeral port for sequential test");
    uint16_t port = server.getPort();

    std::thread serverThread([&]() {
        // Handle Client 1
        std::string ip1;
        uint16_t port1 = 0;
        socket_t s1 = server.acceptClient(ip1, port1, 5000);
        TEST_ASSERT(s1 != INVALID_SOCKET_FD, "Server accepted sequential client 1");
        PacketHeader h1;
        std::vector<uint8_t> p1;
        std::string err;
        TEST_ASSERT(receiveFramedMessage(s1, h1, p1, err), "Server received msg from client 1");
        TEST_ASSERT(sendFramedMessage(s1, MessageType::HELLO_ACK, {}), "Server acked client 1");
        closeSocket(s1);

        // Handle Client 2
        std::string ip2;
        uint16_t port2 = 0;
        socket_t s2 = server.acceptClient(ip2, port2, 5000);
        TEST_ASSERT(s2 != INVALID_SOCKET_FD, "Server accepted sequential client 2");
        PacketHeader h2;
        std::vector<uint8_t> p2;
        TEST_ASSERT(receiveFramedMessage(s2, h2, p2, err), "Server received msg from client 2");
        TEST_ASSERT(sendFramedMessage(s2, MessageType::HELLO_ACK, {}), "Server acked client 2");
        closeSocket(s2);
    });

    // Client 1
    {
        TcpClient client1;
        TEST_ASSERT(client1.connect("127.0.0.1", port, 2000), "Client 1 connected");
        TEST_ASSERT(sendFramedMessage(client1.getSocket(), MessageType::HELLO, {}), "Client 1 sent HELLO");
        PacketHeader ack;
        std::vector<uint8_t> p;
        std::string err;
        TEST_ASSERT(receiveFramedMessage(client1.getSocket(), ack, p, err), "Client 1 received ack");
        client1.disconnect();
        TEST_ASSERT(!client1.isConnected(), "Client 1 disconnected cleanly");
    }

    // Client 2
    {
        TcpClient client2;
        TEST_ASSERT(client2.connect("127.0.0.1", port, 2000), "Client 2 connected");
        TEST_ASSERT(sendFramedMessage(client2.getSocket(), MessageType::HELLO, {}), "Client 2 sent HELLO");
        PacketHeader ack;
        std::vector<uint8_t> p;
        std::string err;
        TEST_ASSERT(receiveFramedMessage(client2.getSocket(), ack, p, err), "Client 2 received ack");
        client2.disconnect();
        TEST_ASSERT(!client2.isConnected(), "Client 2 disconnected cleanly");
    }

    serverThread.join();
    server.stop();
    TEST_ASSERT(!server.isListening(), "Server stopped after sequential test");
}

static void test_client_wrong_ip() {
    TcpClient client;
    bool ok = client.connect("invalid.nonexistent.domain.test", 8080, 500);
    TEST_ASSERT(!ok, "Connecting to non-existent domain fails cleanly");
    TEST_ASSERT(!client.getLastError().empty(), "Meaningful error message recorded for non-existent domain");
}

static void test_client_timeout() {
    TcpClient client;
    auto t0 = std::chrono::steady_clock::now();
    // Non-routable TEST-NET-1 IP address (RFC 5737)
    bool ok = client.connect("192.0.2.1", 8080, 500);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    TEST_ASSERT(!ok, "Connecting to unreachable host timed out / failed cleanly");
    TEST_ASSERT(ms >= 400 && ms < 4000, "Connection timeout respected configured duration (< 4s)");
    TEST_ASSERT(!client.getLastError().empty(), "Error message recorded on timeout");
}

static void test_server_binding_and_cleanup() {
    TcpServer server;
    TEST_ASSERT(!server.isListening(), "Initially not listening");
    TEST_ASSERT(server.getPort() == 0, "Initial port is 0");

    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server started");
    TEST_ASSERT(server.isListening(), "Server is listening");
    uint16_t assignedPort = server.getPort();
    TEST_ASSERT(assignedPort > 0, "Assigned valid ephemeral port");

    server.stop();
    TEST_ASSERT(!server.isListening(), "Server stopped");
    TEST_ASSERT(server.getPort() == 0, "Port reset to 0 after stop");

    // Restart server on another port
    TEST_ASSERT(server.start(0, "127.0.0.1"), "Server restarted on new ephemeral port");
    TEST_ASSERT(server.isListening(), "Server is listening after restart");
    server.stop();
}

static void test_networking_init_cleanup() {
    TEST_ASSERT(initializeNetworking(), "initializeNetworking succeeds");
    cleanupNetworking();
    // Re-initialize to verify idempotency
    TEST_ASSERT(initializeNetworking(), "Re-initialize networking succeeds");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "        RUNNING LOCAL NETWORK TESTS     \n";
    std::cout << "========================================\n";

    test_networking_init_cleanup();
    test_server_binding_and_cleanup();
    test_handshake_and_info();
    test_sequential_connections();
    test_connection_failure();
    test_client_wrong_ip();
    test_client_timeout();
    test_disconnect_detection();

    std::cout << "========================================\n";
    std::cout << "Results: " << g_tests_run << " run, " << g_tests_failed << " failed\n";
    std::cout << "========================================\n";

    return (g_tests_failed == 0) ? 0 : 1;
}
