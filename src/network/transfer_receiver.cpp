#include "network/transfer_receiver.h"

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

bool TransferReceiver::receiveStreamedFile(socket_t sock,
                                           const std::string& tempPath,
                                           const std::string& finalPath,
                                           uint64_t expectedSize,
                                           const std::string& displayLabel,
                                           std::string& error) {
    // Delete stale temp file if it exists
    std::error_code ec;
    fs::remove(tempPath, ec);

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

    // 2. Transfer Info: Expect TRANSFER_INFO, reply TRANSFER_INFO_ACK
    if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
        error = "Failed to receive TRANSFER_INFO: " + error;
        closeSocket(clientSock);
        return false;
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
                             tInfo.manifestSize, "manifest", error)) {
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

    // 4. Payload: Expect PAYLOAD, stream to temp file, reply PAYLOAD_ACK
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

    fs::path finalPayloadPath = transferDir / tInfo.payloadFilename;
    fs::path tempPayloadPath = transferDir / (tInfo.payloadFilename + ".tmp");

    if (config.verbose) std::cout << "[Receiver] Receiving payload bundle...\n";
    if (!receiveStreamedFile(clientSock, tempPayloadPath.string(), finalPayloadPath.string(),
                             tInfo.payloadSize, "payload", error)) {
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    if (!sendFramedMessage(clientSock, MessageType::PAYLOAD_ACK, {})) {
        error = "Failed to send PAYLOAD_ACK: " + getLastSocketError();
        closeSocket(clientSock);
        return false;
    }

    // 5. Complete: Expect COMPLETE, reply COMPLETE_ACK
    if (!receiveFramedMessage(clientSock, hdr, payload, error)) {
        error = "Failed to receive COMPLETE: " + error;
        closeSocket(clientSock);
        return false;
    }
    if (hdr.type != static_cast<uint16_t>(MessageType::COMPLETE)) {
        error = "Unexpected message in state WAIT_COMPLETE: " + messageTypeToString(static_cast<MessageType>(hdr.type));
        sendErrorMessage(clientSock, error);
        closeSocket(clientSock);
        return false;
    }

    // Final integrity verification of stored files
    if (!fs::exists(finalManifestPath, ec) || !fs::exists(finalPayloadPath, ec)) {
        error = "Received files verification failed on disk";
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
    outFiles.payloadPath = finalPayloadPath.string();
    outFiles.manifestSize = tInfo.manifestSize;
    outFiles.payloadSize = tInfo.payloadSize;

    if (config.verbose) {
        std::cout << "\n============================================================\n";
        std::cout << "Transfer completed successfully!\n";
        std::cout << "Manifest: " << outFiles.manifestPath << "\n";
        std::cout << "Payload : " << outFiles.payloadPath << "\n";
        std::cout << "============================================================\n";
    }

    // 6. Reconstruction Integration
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
        if (!reconstructProject(outFiles.manifestPath, outFiles.payloadPath, destinationDir, recResult, recError)) {
            std::cerr << "Error: Reconstruction failed: " << recError << "\n";
        } else {
            std::cout << "Reconstructed files: " << recResult.filesWritten << "\n";
            std::cout << "Chunks from payload: " << recResult.chunksWritten << "\n";
            std::cout << "Chunks reused      : " << recResult.chunksReused << "\n";
            std::cout << "Reconstructed into : " << fs::absolute(destinationDir) << "\n";
        }
    }

    return true;
}

bool TransferReceiver::reconstructProject(const std::string& manifestPath,
                                          const std::string& payloadPath,
                                          const std::string& destinationDir,
                                          ReconstructionResult& result,
                                          std::string& error) {
    ReconstructionEngine engine;
    return engine.reconstruct(manifestPath, payloadPath, destinationDir, result, error);
}

} // namespace retro::net
