#include "network/transfer_receiver.h"
#include "storage/storage_manager.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>

namespace fs = std::filesystem;

namespace retro::net {

constexpr size_t RECV_BUFFER_SIZE = 64 * 1024; // 64 KiB buffer

TransferReceiver::~TransferReceiver() {
    stop();
}

void TransferReceiver::stop() {
    server_.stop();
}

uint16_t TransferReceiver::getBoundPort() const {
    return server_.getPort();
}

bool TransferReceiver::validateReceivedJson(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "Could not open received manifest for validation: " + path;
        return false;
    }

    char firstChar = 0;
    char lastChar = 0;
    char ch;
    int braceBalance = 0;
    bool inString = false;
    bool escape = false;

    while (file.get(ch)) {
        if (!firstChar && !std::isspace(static_cast<unsigned char>(ch))) {
            firstChar = ch;
        }
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            lastChar = ch;
        }

        if (inString) {
            if (escape) {
                escape = false;
            } else if (ch == '\\') {
                escape = true;
            } else if (ch == '"') {
                inString = false;
            }
        } else {
            if (ch == '"') {
                inString = true;
            } else if (ch == '{') {
                braceBalance++;
            } else if (ch == '}') {
                braceBalance--;
                if (braceBalance < 0) {
                    error = "Manifest JSON contains unbalanced closing brace '}'";
                    return false;
                }
            }
        }
    }

    if (firstChar != '{' || lastChar != '}' || braceBalance != 0) {
        error = "Received manifest is not a valid JSON object";
        return false;
    }

    return true;
}

void TransferReceiver::printProgress(const std::string& label,
                                     const std::string& filename,
                                     uint64_t bytesTransferred,
                                     uint64_t totalBytes,
                                     const std::chrono::steady_clock::time_point& startTime) {
    auto now = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(now - startTime).count();
    double speedMBps = (elapsedSec > 0.001)
        ? ((bytesTransferred / (1024.0 * 1024.0)) / elapsedSec)
        : 0.0;

    double percent = (totalBytes > 0)
        ? (static_cast<double>(bytesTransferred) / static_cast<double>(totalBytes)) * 100.0
        : 100.0;
    percent = std::min(100.0, std::max(0.0, percent));

    int barWidth = 24;
    int filled = static_cast<int>(barWidth * (percent / 100.0));
    filled = std::min(barWidth, std::max(0, filled));

    std::cout << "\r[Receiver] " << label << " " << filename << " [" 
              << std::string(filled, '=')
              << std::string(barWidth - filled, ' ') << "] "
              << std::fixed << std::setprecision(1) << percent << "% ("
              << (bytesTransferred / 1024) << " KB / " << (totalBytes / 1024) << " KB) "
              << std::setprecision(2) << speedMBps << " MB/s" << std::flush;

    if (bytesTransferred >= totalBytes) {
        std::cout << "\n";
    }
}

bool TransferReceiver::receiveStreamedFile(socket_t sock,
                                           const std::string& tempPath,
                                           const std::string& finalPath,
                                           uint64_t expectedSize,
                                           const std::string& displayLabel,
                                           bool verbose,
                                           std::string& error) {
    // Delete stale temp file if it exists
    std::error_code ec;
    fs::remove(tempPath, ec);

    std::string filename = fs::path(finalPath).filename().string();
    auto startTime = std::chrono::steady_clock::now();

    if (expectedSize == 0) {
        std::ofstream outFile(tempPath, std::ios::binary);
        outFile.close();
        fs::remove(finalPath, ec);
        fs::rename(tempPath, finalPath, ec);
        if (verbose) {
            printProgress(displayLabel, filename, 0, 0, startTime);
        }
        return true;
    }

    std::ofstream outFile(tempPath, std::ios::binary);
    if (!outFile) {
        error = "Failed to create temporary file: " + tempPath;
        return false;
    }

    std::vector<uint8_t> buffer(RECV_BUFFER_SIZE);
    uint64_t bytesReceived = 0;

    while (bytesReceived < expectedSize) {
        size_t toRead = static_cast<size_t>(
            std::min(static_cast<uint64_t>(RECV_BUFFER_SIZE), expectedSize - bytesReceived));

        if (!receiveAll(sock, buffer.data(), toRead)) {
            error = "Connection lost while receiving " + displayLabel + " (" +
                    std::to_string(bytesReceived) + " of " + std::to_string(expectedSize) + " bytes)";
            outFile.close();
            fs::remove(tempPath, ec);
            return false;
        }

        outFile.write(reinterpret_cast<const char*>(buffer.data()), toRead);
        if (!outFile) {
            error = "Disk write failure while writing to " + tempPath;
            outFile.close();
            fs::remove(tempPath, ec);
            return false;
        }

        bytesReceived += toRead;
        if (verbose) {
            printProgress(displayLabel, filename, bytesReceived, expectedSize, startTime);
        }
    }

    outFile.close();

    // Verify written file size matches expected size
    uint64_t actualSize = fs::file_size(tempPath, ec);
    if (ec || actualSize != expectedSize) {
        error = "Size mismatch for received " + displayLabel + ": expected " +
                std::to_string(expectedSize) + ", got " + std::to_string(actualSize);
        fs::remove(tempPath, ec);
        return false;
    }

    // Rename temp file to final destination file
    fs::remove(finalPath, ec); // Remove destination if it already exists
    fs::rename(tempPath, finalPath, ec);
    if (ec) {
        error = "Failed to rename temp file " + tempPath + " to " + finalPath + ": " + ec.message();
        return false;
    }

    return true;
}

bool TransferReceiver::receive(const ReceiverConfig& config, ReceivedFiles& outFiles, std::string& error) {
    if (!server_.isListening()) {
        if (!server_.start(config.port, "0.0.0.0")) {
            error = "Failed to start TCP server on port " + std::to_string(config.port) + ": " + server_.getLastError();
            return false;
        }
    }

    if (config.verbose) {
        std::cout << "[Receiver] Listening on port " << server_.getPort() << "...\n";
        std::cout << "[Receiver] Waiting for sender connection...\n";
    }

    std::string clientIp;
    uint16_t clientPort = 0;
    socket_t clientSock = server_.acceptClient(clientIp, clientPort, config.acceptTimeoutMs);
    if (clientSock == INVALID_SOCKET_FD) {
        error = "Accept failed: " + server_.getLastError();
        return false;
    }

    if (config.verbose) {
        std::cout << "[Receiver] Accepted connection from " << clientIp << ":" << clientPort << "\n";
    }

    // 1. Handshake: Expect HELLO, reply HELLO_ACK
    PacketHeader hdr;
    std::vector<uint8_t> payload;
    if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
        error = "Handshake failed: " + error;
        closeSocket(clientSock);
        return false;
    }
    if (hdr.type != static_cast<uint16_t>(MessageType::HELLO)) {
        error = "Unexpected message in state WAIT_HELLO: " + messageTypeToString(static_cast<MessageType>(hdr.type));
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    if (!sendFramedMessage(clientSock, MessageType::HELLO_ACK, {})) {
        error = "Failed to send HELLO_ACK: " + getLastSocketError();
        closeSocket(clientSock);
        return false;
    }

    bool skipPayloadStreaming = false;

    // 2. Expect either CHUNK_LIST (remote deduplication) or TRANSFER_INFO (direct)
    if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
        error = "Failed to receive message after handshake: " + error;
        closeSocket(clientSock);
        return false;
    }

    if (hdr.type == static_cast<uint16_t>(MessageType::CHUNK_LIST)) {
        ChunkListMsg clMsg;
        if (!deserializeChunkList(payload.data(), payload.size(), clMsg, error)) {
            sendErrorMessage(clientSock, error);
            closeSocket(clientSock);
            return false;
        }

        if (config.verbose) {
            std::cout << "[Receiver] Received CHUNK_LIST query with " << clMsg.chunks.size() << " chunks\n";
            std::cout << "[Receiver] Inspecting persistent chunk storage (" << config.chunkStorageDir << ")...\n";
        }

        retro::storage::StorageManager storageMgr(config.chunkStorageDir);
        std::string sErr;
        storageMgr.initialize(sErr);

        uint32_t availableChunks = 0;
        uint32_t missingChunks = 0;
        uint64_t availableBytes = 0;
        uint64_t missingBytes = 0;
        std::vector<std::string> missingFps;

        for (const auto& chk : clMsg.chunks) {
            std::string valErr;
            // The chunk must exist on disk and pass size, CRC32, and MurmurHash3 checks
            if (storageMgr.hasChunk(chk.fingerprint) &&
                storageMgr.validateChunk(chk.fingerprint, chk.originalSize, chk.crc32, valErr)) {
                availableChunks++;
                availableBytes += chk.originalSize;
            } else {
                missingChunks++;
                missingBytes += chk.originalSize;
                missingFps.push_back(chk.fingerprint);
            }
        }

        if (config.verbose) {
            std::cout << "[Receiver] Chunk inventory: " << availableChunks << " available ("
                      << (availableBytes / 1024) << " KB), " << missingChunks << " missing ("
                      << (missingBytes / 1024) << " KB)\n";
        }

        MissingChunksMsg mcMsg;
        mcMsg.totalChunks = static_cast<uint32_t>(clMsg.chunks.size());
        mcMsg.availableChunks = availableChunks;
        mcMsg.missingChunks = missingChunks;
        mcMsg.availableBytes = availableBytes;
        mcMsg.missingBytes = missingBytes;
        mcMsg.missingFingerprints = missingFps;
        skipPayloadStreaming = (missingChunks == 0);

        std::vector<uint8_t> mcBytes;
        serializeMissingChunks(mcMsg, mcBytes);
        if (!sendFramedMessage(clientSock, MessageType::MISSING_CHUNKS, mcBytes)) {
            error = "Failed to send MISSING_CHUNKS: " + getLastSocketError();
            closeSocket(clientSock);
            return false;
        }

        // Wait for MISSING_CHUNKS_ACK
        if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
            error = "Failed to receive MISSING_CHUNKS_ACK: " + error;
            closeSocket(clientSock);
            return false;
        }

        // Now receive TRANSFER_INFO
        if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
            error = "Failed to receive TRANSFER_INFO after negotiation: " + error;
            closeSocket(clientSock);
            return false;
        }
    }

    if (hdr.type != static_cast<uint16_t>(MessageType::TRANSFER_INFO)) {
        error = "Unexpected message in state WAIT_TRANSFER_INFO: " + messageTypeToString(static_cast<MessageType>(hdr.type));
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    TransferInfo tInfo;
    if (!deserializeTransferInfo(payload.data(), payload.size(), tInfo, error)) {
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    // Create unique storage directory
    fs::path transferDir = fs::path(config.baseStorageDir) / tInfo.transferId;
    std::error_code ec;
    fs::create_directories(transferDir, ec);
    if (ec) {
        error = "Failed to create directory " + transferDir.string() + ": " + ec.message();
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    // Verify sufficient disk space for incoming transfer
    std::error_code spaceEc;
    fs::space_info spaceInfo = fs::space(transferDir, spaceEc);
    if (!spaceEc) {
        uint64_t requiredBytes = tInfo.manifestSize + tInfo.payloadSize + (1024ULL * 1024ULL); // with 1MB margin
        if (spaceInfo.available < requiredBytes) {
            error = "Insufficient disk space: " + std::to_string(spaceInfo.available) +
                    " bytes available, " + std::to_string(requiredBytes) + " bytes required";
            sendErrorMessage(clientSock, error);
            closeSocket(clientSock);
            return false;
        }
    }

    if (config.verbose) {
        std::cout << "[Receiver] Transfer ID : " << tInfo.transferId << "\n";
        std::cout << "[Receiver] Destination : " << transferDir.string() << "\n";
        std::cout << "[Receiver] Manifest    : " << tInfo.manifestFilename << " (" << tInfo.manifestSize << " bytes)\n";
        std::cout << "[Receiver] Payload     : " << tInfo.payloadFilename << " (" << tInfo.payloadSize << " bytes)\n";
    }

    if (!sendFramedMessage(clientSock, MessageType::TRANSFER_INFO_ACK, {})) {
        error = "Failed to send TRANSFER_INFO_ACK: " + getLastSocketError();
        closeSocket(clientSock);
        return false;
    }

    // 3. Manifest: Expect MANIFEST, stream to temp file, validate JSON, reply MANIFEST_ACK
    if (!receivePacketHeader(clientSock, hdr, error)) {
        error = "Failed to receive MANIFEST header: " + error;
        closeSocket(clientSock);
        return false;
    }
    if (hdr.type != static_cast<uint16_t>(MessageType::MANIFEST)) {
        error = "Unexpected message in state WAIT_MANIFEST: " + messageTypeToString(static_cast<MessageType>(hdr.type));
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }
    if (hdr.payloadLength != tInfo.manifestSize) {
        error = "MANIFEST header length (" + std::to_string(hdr.payloadLength) +
                ") does not match TRANSFER_INFO size (" + std::to_string(tInfo.manifestSize) + ")";
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    fs::path finalManifestPath = transferDir / tInfo.manifestFilename;
    fs::path tempManifestPath = transferDir / (tInfo.manifestFilename + ".tmp");

    if (config.verbose) std::cout << "[Receiver] Receiving manifest...\n";
    if (!receiveStreamedFile(clientSock, tempManifestPath.string(), finalManifestPath.string(),
                             tInfo.manifestSize, "manifest", config.verbose, error)) {
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    // Validate received manifest JSON
    if (!validateReceivedJson(finalManifestPath.string(), error)) {
        sendErrorMessage(clientSock, error);
        fs::remove(finalManifestPath, ec);
        closeSocket(clientSock);
        return false;
    }

    if (!sendFramedMessage(clientSock, MessageType::MANIFEST_ACK, {})) {
        error = "Failed to send MANIFEST_ACK: " + getLastSocketError();
        closeSocket(clientSock);
        return false;
    }

    // 4. Payload: Expect PAYLOAD unless skipped by remote deduplication
    fs::path finalPayloadPath;
    if (!skipPayloadStreaming) {
        finalPayloadPath = transferDir / tInfo.payloadFilename;
        fs::path tempPayloadPath = transferDir / (tInfo.payloadFilename + ".tmp");

        if (!receivePacketHeader(clientSock, hdr, error)) {
            error = "Failed to receive PAYLOAD header: " + error;
            closeSocket(clientSock);
            return false;
        }
        if (hdr.type != static_cast<uint16_t>(MessageType::PAYLOAD)) {
            error = "Unexpected message in state WAIT_PAYLOAD: " + messageTypeToString(static_cast<MessageType>(hdr.type));
            sendErrorMessage(clientSock, error);
            closeSocket(clientSock);
            return false;
        }
        if (hdr.payloadLength != tInfo.payloadSize) {
            error = "PAYLOAD header length (" + std::to_string(hdr.payloadLength) +
                    ") does not match TRANSFER_INFO size (" + std::to_string(tInfo.payloadSize) + ")";
            sendErrorMessage(clientSock, error);
            closeSocket(clientSock);
            return false;
        }

        if (config.verbose) std::cout << "[Receiver] Receiving payload bundle...\n";
        if (!receiveStreamedFile(clientSock, tempPayloadPath.string(), finalPayloadPath.string(),
                                 tInfo.payloadSize, "payload", config.verbose, error)) {
            sendErrorMessage(clientSock, error);
            closeSocket(clientSock);
            return false;
        }

        if (!sendFramedMessage(clientSock, MessageType::PAYLOAD_ACK, {})) {
            error = "Failed to send PAYLOAD_ACK: " + getLastSocketError();
            closeSocket(clientSock);
            return false;
        }
    } else if (config.verbose) {
        std::cout << "[Receiver] Zero payload chunks required; payload streaming skipped.\n";
    }

    // 5. Complete / Dedup Summary: Expect DEDUP_SUMMARY or COMPLETE
    if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
        error = "Failed to receive message after payload: " + error;
        closeSocket(clientSock);
        return false;
    }

    if (hdr.type == static_cast<uint16_t>(MessageType::DEDUP_SUMMARY)) {
        deserializeDedupSummary(payload.data(), payload.size(), stats_, error);
        sendFramedMessage(clientSock, MessageType::DEDUP_SUMMARY_ACK, {});

        if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
            error = "Failed to receive COMPLETE after DEDUP_SUMMARY: " + error;
            closeSocket(clientSock);
            return false;
        }
    }

    if (hdr.type != static_cast<uint16_t>(MessageType::COMPLETE)) {
        error = "Unexpected message in state WAIT_COMPLETE: " + messageTypeToString(static_cast<MessageType>(hdr.type));
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    // Final integrity verification of stored files
    if (!fs::exists(finalManifestPath, ec)) {
        error = "Received manifest verification failed on disk";
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }
    if (!skipPayloadStreaming && !fs::exists(finalPayloadPath, ec)) {
        error = "Received payload verification failed on disk";
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    if (!sendFramedMessage(clientSock, MessageType::COMPLETE_ACK, {})) {
        error = "Failed to send COMPLETE_ACK: " + getLastSocketError();
        closeSocket(clientSock);
        return false;
    }

    closeSocket(clientSock);

    outFiles.transferId = tInfo.transferId;
    outFiles.manifestPath = finalManifestPath.string();
    outFiles.payloadPath = (!skipPayloadStreaming) ? finalPayloadPath.string() : "";
    outFiles.manifestSize = tInfo.manifestSize;
    outFiles.payloadSize = tInfo.payloadSize;

    if (config.verbose) {
        std::cout << "\n============================================================\n";
        std::cout << "Transfer completed successfully!\n";
        std::cout << "Manifest: " << outFiles.manifestPath << "\n";
        std::cout << "Payload : " << outFiles.payloadPath << "\n";
        std::cout << "============================================================\n";
    }

    // 6. Persistent Chunk Storage and Indexing Integration
    if (config.enableChunkStorage) {
        if (config.verbose) {
            std::cout << "[Receiver] Persisting received chunks into storage (" << config.chunkStorageDir << ")...\n";
        }
        retro::storage::StorageManager storageMgr(config.chunkStorageDir);
        std::string sErr;
        if (!storageMgr.initialize(sErr)) {
            if (config.verbose) {
                std::cerr << "[Receiver] Warning: StorageManager initialization failed: " << sErr << "\n";
            }
        } else {
            std::vector<ExtractedChunk> extractedChunks;
            std::string extractErr;
            if (ReconstructionEngine::extractPayloadChunks(
                    outFiles.manifestPath, outFiles.payloadPath, extractedChunks, extractErr)) {
                size_t storedCount = 0;
                size_t existingCount = 0;
                for (const auto& chk : extractedChunks) {
                    if (storageMgr.hasChunk(chk.fingerprint)) {
                        existingCount++;
                    }
                    std::string storeErr;
                    if (storageMgr.storeChunk(chk.fingerprint, chk.data.data(), chk.data.size(), chk.crc32, storeErr)) {
                        storedCount++;
                    } else if (config.verbose) {
                        std::cerr << "[Receiver] Warning: Failed to store chunk " << chk.fingerprint << ": " << storeErr << "\n";
                    }
                }
                if (config.verbose && !extractedChunks.empty()) {
                    std::cout << "[Receiver] Persistent Storage: " << extractedChunks.size() << " chunks processed ("
                              << existingCount << " existing, " << (storedCount - existingCount) << " newly added). Total indexed: "
                              << storageMgr.getStoredChunkCount() << " chunks.\n";
                }
            }
        }
    }

    // 7. Reconstruction Integration
    std::string destinationDir = config.autoReconstructDir;

    if (destinationDir.empty() && config.interactive) {
        std::cout << "\nWould you like to reconstruct the project now? (y/n): ";
        std::string answer;
        if (std::cin >> answer && (answer == "y" || answer == "Y" || answer == "yes")) {
            std::cout << "Enter destination directory for reconstructed project: ";
            std::cin >> destinationDir;
        }
    }

    if (!destinationDir.empty()) {
        std::cout << "\n[Reconstruction] Starting project reconstruction into: " << destinationDir << "...\n";
        ReconstructionResult recResult;
        std::string recError;
        if (!reconstructProject(outFiles.manifestPath, outFiles.payloadPath, destinationDir, recResult, recError,
                                config.enableChunkStorage ? config.chunkStorageDir : "")) {
            std::cerr << "Error: Reconstruction failed: " << recError << "\n";
        } else {
            std::cout << "Reconstructed files: " << recResult.filesWritten << "\n";
            std::cout << "Chunks from payload: " << recResult.chunksWritten << "\n";
            std::cout << "Chunks reused      : " << recResult.chunksReused << "\n";
            std::cout << "Reconstructed into : " << fs::absolute(destinationDir) << "\n";
        }
    }

    if (config.verbose && stats_.totalProjectChunks > 0) {
        printDedupSummary(stats_, 0.0);
    }

    return true;
}

void TransferReceiver::printDedupSummary(const DedupSummaryMsg& stats, double elapsedSec) {
    double speedMBps = (elapsedSec > 0.001)
        ? ((stats.transmittedPayloadBytes / (1024.0 * 1024.0)) / elapsedSec)
        : 0.0;
    double dedupPercent = (stats.totalProjectChunks > 0)
        ? (100.0 * stats.availableChunks / stats.totalProjectChunks)
        : 0.0;
    double missingPercent = (stats.totalProjectChunks > 0)
        ? (100.0 * stats.missingChunks / stats.totalProjectChunks)
        : 0.0;

    std::cout << "\n============================================================\n";
    std::cout << "        RECEIVER DEDUPLICATION & RECONSTRUCTION             \n";
    std::cout << "============================================================\n";
    std::cout << "Total Project Chunks     : " << stats.totalProjectChunks << "\n";
    std::cout << "Chunks Available Locally : " << stats.availableChunks << " ("
              << std::fixed << std::setprecision(1) << dedupPercent << "% deduplicated)\n";
    std::cout << "Missing Chunks Transferred: " << stats.missingChunks << " ("
              << std::fixed << std::setprecision(1) << missingPercent << "%)\n";
    std::cout << "Chunks Received          : " << stats.transferredChunks << "\n";
    std::cout << "Total Original Data      : " << stats.totalOriginalBytes << " bytes ("
              << (stats.totalOriginalBytes / 1024) << " KB)\n";
    std::cout << "Deduplication Savings    : " << stats.deduplicatedBytes << " bytes ("
              << (stats.deduplicatedBytes / 1024) << " KB saved from wire)\n";
    std::cout << "Transmitted Payload Wire : " << stats.transmittedPayloadBytes << " bytes ("
              << (stats.transmittedPayloadBytes / 1024) << " KB)\n";
    std::cout << "Compression Savings      : " << stats.compressionSavingsBytes << " bytes\n";
    if (elapsedSec > 0.0) {
        std::cout << "Total Transfer Time      : " << std::fixed << std::setprecision(2)
                  << (elapsedSec * 1000.0) << " ms\n";
        std::cout << "Transfer Speed           : " << std::fixed << std::setprecision(2)
                  << speedMBps << " MB/s\n";
    }
    std::cout << "============================================================\n";
}

bool TransferReceiver::reconstructProject(const std::string& manifestPath,
                                          const std::string& payloadPath,
                                          const std::string& destinationDir,
                                          ReconstructionResult& result,
                                          std::string& error,
                                          const std::string& chunkStorageDir) {
    ReconstructionEngine engine;
    return engine.reconstruct(manifestPath, payloadPath, destinationDir, result, error, chunkStorageDir);
}

} // namespace retro::net
