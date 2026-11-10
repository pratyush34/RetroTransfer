#ifndef RETROTRANSFER_TRANSFER_SENDER_H
#define RETROTRANSFER_TRANSFER_SENDER_H

#include "network/tcp_client.h"
#include "network/protocol.h"
#include "network/transfer_queue.h"

#include <string>
#include <chrono>

namespace retro::net {

struct SenderConfig {
    std::string manifestPath;
    std::string payloadPath;
    std::string receiverIp;
    uint16_t receiverPort = 8080;
    int connectionTimeoutMs = 10000;
    bool verbose = true;
};

class TransferSender {
public:
    TransferSender() = default;
    ~TransferSender() = default;

    /*
     * Execute full sender workflow:
     * 1. Validate files
     * 2. Connect to receiver
     * 3. Handshake (HELLO -> HELLO_ACK)
     * 4. Metadata (TRANSFER_INFO -> TRANSFER_INFO_ACK)
     * 5. Stream manifest (MANIFEST -> MANIFEST_ACK)
     * 6. Stream payload with C ring buffer (PAYLOAD -> PAYLOAD_ACK)
     * 7. Finalize (COMPLETE -> COMPLETE_ACK)
     */
    bool send(const SenderConfig& config, std::string& error);

private:
    /*
     * Stream a file over socket using bounded 64 KiB buffer and C TransferQueue.
     */
    bool streamFileWithQueue(socket_t sock,
                             const std::string& filePath,
                             uint64_t fileSize,
                             MessageType messageType,
                             const std::string& displayLabel,
                             std::string& error);

    /*
     * Display progress bar and transfer speed.
     */
    void printProgress(const std::string& label,
                       uint64_t bytesTransferred,
                       uint64_t totalBytes,
                       const std::chrono::steady_clock::time_point& startTime);

    /*
     * Simple JSON syntax validator.
     */
    bool validateJsonFile(const std::string& path, std::string& error);
};

} // namespace retro::net

#endif // RETROTRANSFER_TRANSFER_SENDER_H
