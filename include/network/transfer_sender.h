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
    std::string projectDir;          // Optional project directory for direct chunk access
    std::string receiverIp;
    std::string benchmarkCSVPath;
    std::string benchmarkJSONPath;
    uint16_t receiverPort = 8080;
    int connectionTimeoutMs = 10000;
    bool verbose = true;
    bool enableRemoteDedup = true;    // Perform remote chunk negotiation (default true)
};

class TransferSender {
public:
    TransferSender() = default;
    ~TransferSender() = default;

    /*
     * Execute full sender workflow with optional remote deduplication:
     * 1. Validate files
     * 2. Connect to receiver
     * 3. Handshake (HELLO -> HELLO_ACK)
     * 4. Remote chunk negotiation (CHUNK_LIST -> MISSING_CHUNKS -> MISSING_CHUNKS_ACK)
     * 5. Selective payload generation (if missing chunks exist)
     * 6. Metadata (TRANSFER_INFO -> TRANSFER_INFO_ACK)
     * 7. Stream manifest (MANIFEST -> MANIFEST_ACK)
     * 8. Stream selective payload with C ring buffer (PAYLOAD -> PAYLOAD_ACK)
     * 9. Deduplication summary (DEDUP_SUMMARY -> DEDUP_SUMMARY_ACK)
     * 10. Finalize (COMPLETE -> COMPLETE_ACK)
     */
    bool send(const SenderConfig& config, std::string& error);

    /*
     * Retrieve transfer performance and deduplication statistics.
     */
    const DedupSummaryMsg& getStatistics() const { return stats_; }

private:
    DedupSummaryMsg stats_{};
    /*
     * Stream a file over socket using bounded 64 KiB buffer and C TransferQueue.
     */
    bool streamFileWithQueue(socket_t sock,
                             const std::string& filePath,
                             uint64_t fileSize,
                             MessageType messageType,
                             const std::string& displayLabel,
                             bool verbose,
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

    /*
     * Extract unique chunk fingerprint list from a manifest file.
     */
    bool extractChunkListFromManifest(const std::string& path,
                                      std::vector<ChunkListEntry>& outChunks,
                                      uint64_t& outTotalBytes,
                                      std::string& error);

    /*
     * Format and print deduplication & transfer summary.
     */
    void printDedupSummary(const DedupSummaryMsg& stats, double elapsedSec);
};

} // namespace retro::net

#endif // RETROTRANSFER_TRANSFER_SENDER_H
