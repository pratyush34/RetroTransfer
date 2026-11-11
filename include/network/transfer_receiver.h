#ifndef RETROTRANSFER_TRANSFER_RECEIVER_H
#define RETROTRANSFER_TRANSFER_RECEIVER_H

#include "network/tcp_server.h"
#include "network/protocol.h"
#include "reconstruction_engine.h"

#include <chrono>
#include <string>

namespace retro::net {

struct ReceiverConfig {
    uint16_t port = 8080;
    std::string baseStorageDir = "received_projects";
    std::string autoReconstructDir; // If non-empty, automatically reconstructs to this dir
    bool interactive = true;         // If true, prompts user before reconstruction
    bool verbose = true;
    int acceptTimeoutMs = 0;        // 0 = block indefinitely until sender connects
    std::string chunkStorageDir = "storage"; // Directory for persistent chunk storage
    bool enableChunkStorage = true;          // Enable persisting and indexing received chunks
};

struct ReceivedFiles {
    std::string transferId;
    std::string manifestPath;
    std::string payloadPath;
    uint64_t manifestSize = 0;
    uint64_t payloadSize = 0;
};

class TransferReceiver {
public:
    TransferReceiver() = default;
    ~TransferReceiver();

    /*
     * Start the receiver server and process a single transfer session.
     */
    bool receive(const ReceiverConfig& config, ReceivedFiles& outFiles, std::string& error);

    /*
     * Run the existing reconstruction engine on the received files.
     */
    bool reconstructProject(const std::string& manifestPath,
                            const std::string& payloadPath,
                            const std::string& destinationDir,
                            ReconstructionResult& result,
                            std::string& error,
                            const std::string& chunkStorageDir = "");

    /*
     * Stop the server if listening.
     */
    void stop();

    uint16_t getBoundPort() const;

private:
    TcpServer server_;

    /*
     * Receive a streamed file from socket into temporary file, then rename to final path.
     */
    bool receiveStreamedFile(socket_t sock,
                             const std::string& tempPath,
                             const std::string& finalPath,
                             uint64_t expectedSize,
                             const std::string& displayLabel,
                             bool verbose,
                             std::string& error);

    /*
     * Display progress bar and transfer speed on receiver.
     */
    void printProgress(const std::string& label,
                       const std::string& filename,
                       uint64_t bytesTransferred,
                       uint64_t totalBytes,
                       const std::chrono::steady_clock::time_point& startTime);

    /*
     * Validate JSON format of the received manifest file.
     */
    bool validateReceivedJson(const std::string& path, std::string& error);
};

} // namespace retro::net

#endif // RETROTRANSFER_TRANSFER_RECEIVER_H
