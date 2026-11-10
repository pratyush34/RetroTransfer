#include "network/transfer_sender.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace retro::net {

constexpr size_t STREAM_BUFFER_SIZE = 64 * 1024; // 64 KiB buffer
constexpr size_t QUEUE_CAPACITY = 16;            // Ring buffer capacity (16 descriptors)

bool TransferSender::validateJsonFile(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "Could not open manifest file for reading: " + path;
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
                    error = "Manifest JSON has unbalanced closing brace '}'";
                    return false;
                }
            }
        }
    }

    if (firstChar != '{' || lastChar != '}' || braceBalance != 0) {
        error = "Manifest is not a valid JSON object";
        return false;
    }

    return true;
}

void TransferSender::printProgress(const std::string& label,
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

    std::cout << "\r" << label << " [" 
              << std::string(filled, '=')
              << std::string(barWidth - filled, ' ') << "] "
              << std::fixed << std::setprecision(1) << percent << "% ("
              << (bytesTransferred / 1024) << " KB / " << (totalBytes / 1024) << " KB) "
              << std::setprecision(2) << speedMBps << " MB/s" << std::flush;

    if (bytesTransferred >= totalBytes) {
        std::cout << "\n";
    }
}

bool TransferSender::streamFileWithQueue(socket_t sock,
                                         const std::string& filePath,
                                         uint64_t fileSize,
                                         MessageType messageType,
                                         const std::string& displayLabel,
                                         bool verbose,
                                         std::string& error) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        error = "Failed to open file for streaming: " + filePath;
        return false;
    }

    // 1. Send packet header for the file transfer
    PacketHeader header;
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = static_cast<uint16_t>(messageType);
    header.payloadLength = fileSize;

    if (!sendPacketHeader(sock, header)) {
        error = "Failed to send packet header for " + messageTypeToString(messageType);
        return false;
    }

    if (fileSize == 0) {
        if (verbose) {
            printProgress(displayLabel, 0, 0, std::chrono::steady_clock::now());
        }
        return true;
    }

    // 2. Initialize bounded circular queue in C
    TransferQueue queue;
    if (!transfer_queue_init(&queue, QUEUE_CAPACITY)) {
        error = "Failed to initialize C transfer queue (ring buffer)";
        return false;
    }

    std::vector<uint8_t> buffer(STREAM_BUFFER_SIZE);
    uint64_t bytesPlanned = 0;
    uint64_t bytesTransferred = 0;
    uint32_t blockIndex = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (bytesTransferred < fileSize) {
        // Enqueue transfer descriptors into C ring buffer up to capacity
        while (!transfer_queue_is_full(&queue) && bytesPlanned < fileSize) {
            uint32_t curSize = static_cast<uint32_t>(
                std::min(static_cast<uint64_t>(STREAM_BUFFER_SIZE), fileSize - bytesPlanned));

            TransferBlockDesc desc;
            desc.file_offset = bytesPlanned;
            desc.block_size = curSize;
            desc.block_index = blockIndex++;
            desc.is_last = (bytesPlanned + curSize >= fileSize) ? 1 : 0;

            if (!transfer_queue_enqueue(&queue, &desc)) {
                break; // Queue full
            }
            bytesPlanned += curSize;
        }

        // Dequeue transfer descriptors and stream data blocks over socket
        while (!transfer_queue_is_empty(&queue)) {
            TransferBlockDesc desc;
            if (!transfer_queue_dequeue(&queue, &desc)) {
                break;
            }

            file.seekg(static_cast<std::streamoff>(desc.file_offset), std::ios::beg);
            file.read(reinterpret_cast<char*>(buffer.data()), desc.block_size);
            if (file.bad() || static_cast<size_t>(file.gcount()) != desc.block_size) {
                error = "Failed reading block " + std::to_string(desc.block_index) +
                        " from " + filePath;
                transfer_queue_destroy(&queue);
                return false;
            }

            if (!sendAll(sock, buffer.data(), desc.block_size)) {
                error = "Network transmission failed while streaming " + displayLabel;
                transfer_queue_destroy(&queue);
                return false;
            }

            bytesTransferred += desc.block_size;
            if (verbose) {
                printProgress(displayLabel, bytesTransferred, fileSize, startTime);
            }
        }
    }

    transfer_queue_destroy(&queue);
    return true;
}

bool TransferSender::send(const SenderConfig& config, std::string& error) {
    // 1. Validate manifest path & existence
    fs::path manifestFs(config.manifestPath);
    std::error_code ec;
    if (!fs::exists(manifestFs, ec) || ec || !fs::is_regular_file(manifestFs, ec)) {
        error = "Manifest file does not exist or is not a regular file: " + config.manifestPath;
        return false;
    }

    // 2. Validate payload path & existence
    fs::path payloadFs(config.payloadPath);
    if (!fs::exists(payloadFs, ec) || ec || !fs::is_regular_file(payloadFs, ec)) {
        error = "Payload file does not exist or is not a regular file: " + config.payloadPath;
        return false;
    }

    // 3. Filename check
    std::string manifestFilename = manifestFs.filename().string();
    std::string payloadFilename = payloadFs.filename().string();
    if (!isValidTransferFilename(manifestFilename, ".json")) {
        error = "Manifest filename is invalid or has wrong extension (expected .json): " + manifestFilename;
        return false;
    }
    if (!isValidTransferFilename(payloadFilename, ".rtpack")) {
        error = "Payload filename is invalid or has wrong extension (expected .rtpack): " + payloadFilename;
        return false;
    }

    // 4. Calculate file sizes
    uint64_t manifestSize = fs::file_size(manifestFs, ec);
    if (ec) {
        error = "Could not determine manifest file size: " + ec.message();
        return false;
    }
    if (manifestSize > MAX_MANIFEST_SIZE) {
        error = "Manifest file exceeds maximum allowed size (64 MiB): " + std::to_string(manifestSize);
        return false;
    }

    uint64_t payloadSize = fs::file_size(payloadFs, ec);
    if (ec) {
        error = "Could not determine payload file size: " + ec.message();
        return false;
    }

    // 5. Validate manifest JSON
    if (!validateJsonFile(config.manifestPath, error)) {
        return false;
    }

    if (config.verbose) {
        std::cout << "[Sender] Manifest: " << config.manifestPath << " (" << manifestSize << " bytes)\n";
        std::cout << "[Sender] Payload : " << config.payloadPath << " (" << payloadSize << " bytes)\n";
        std::cout << "[Sender] Target  : " << config.receiverIp << ":" << config.receiverPort << "\n";
        std::cout << "[Sender] Connecting to receiver...\n";
    }

    // 6. Connect to receiver
    TcpClient client;
    if (!client.connect(config.receiverIp, config.receiverPort, config.connectionTimeoutMs)) {
        error = "Connection failed: " + client.getLastError();
        return false;
    }
    socket_t sock = client.getSocket();

    // 7. Handshake: send HELLO, expect HELLO_ACK
    if (config.verbose) std::cout << "[Sender] Performing handshake (HELLO)...\n";
    if (!sendFramedMessage(sock, MessageType::HELLO, {})) {
        error = "Failed to send HELLO handshake: " + getLastSocketError();
        return false;
    }

    PacketHeader ackHdr;
    std::vector<uint8_t> ackPayload;
    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Handshake failed: " + error;
        return false;
    }
    if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
        std::string remoteErr(ackPayload.begin(), ackPayload.end());
        error = "Receiver rejected handshake: " + (remoteErr.empty() ? "Remote error" : remoteErr);
        return false;
    }
    if (ackHdr.type != static_cast<uint16_t>(MessageType::HELLO_ACK)) {
        error = "Expected HELLO_ACK, received " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
        return false;
    }

    // 8. Send TRANSFER_INFO
    if (config.verbose) std::cout << "[Sender] Sending transfer metadata...\n";
    TransferInfo tInfo;
    auto now = std::chrono::system_clock::now();
    auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    tInfo.transferId = "transfer_" + std::to_string(epochMs);
    tInfo.manifestFilename = manifestFilename;
    tInfo.manifestSize = manifestSize;
    tInfo.payloadFilename = payloadFilename;
    tInfo.payloadSize = payloadSize;
    tInfo.protocolVersion = PROTOCOL_VERSION;

    std::vector<uint8_t> infoBytes;
    serializeTransferInfo(tInfo, infoBytes);
    if (!sendFramedMessage(sock, MessageType::TRANSFER_INFO, infoBytes)) {
        error = "Failed to send TRANSFER_INFO: " + getLastSocketError();
        return false;
    }

    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Failed to receive TRANSFER_INFO_ACK: " + error;
        return false;
    }
    if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
        std::string remoteErr(ackPayload.begin(), ackPayload.end());
        error = "Receiver rejected transfer info: " + (remoteErr.empty() ? "Remote error" : remoteErr);
        return false;
    }
    if (ackHdr.type != static_cast<uint16_t>(MessageType::TRANSFER_INFO_ACK)) {
        error = "Expected TRANSFER_INFO_ACK, received " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
        return false;
    }

    // 9. Send Manifest
    if (config.verbose) std::cout << "[Sender] Streaming manifest: " << manifestFilename << "...\n";
    if (!streamFileWithQueue(sock, config.manifestPath, manifestSize, MessageType::MANIFEST,
                             "Manifest " + manifestFilename, config.verbose, error)) {
        return false;
    }

    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Failed to receive MANIFEST_ACK: " + error;
        return false;
    }
    if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
        std::string remoteErr(ackPayload.begin(), ackPayload.end());
        error = "Receiver rejected manifest: " + (remoteErr.empty() ? "Remote error" : remoteErr);
        return false;
    }
    if (ackHdr.type != static_cast<uint16_t>(MessageType::MANIFEST_ACK)) {
        error = "Expected MANIFEST_ACK, received " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
        return false;
    }

    // 10. Send Payload (.rtpack)
    if (config.verbose) std::cout << "[Sender] Streaming payload: " << payloadFilename << " (using C circular queue)...\n";
    if (!streamFileWithQueue(sock, config.payloadPath, payloadSize, MessageType::PAYLOAD,
                             "Payload  " + payloadFilename, config.verbose, error)) {
        return false;
    }

    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Failed to receive PAYLOAD_ACK: " + error;
        return false;
    }
    if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
        std::string remoteErr(ackPayload.begin(), ackPayload.end());
        error = "Receiver rejected payload: " + (remoteErr.empty() ? "Remote error" : remoteErr);
        return false;
    }
    if (ackHdr.type != static_cast<uint16_t>(MessageType::PAYLOAD_ACK)) {
        error = "Expected PAYLOAD_ACK, received " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
        return false;
    }

    // 11. Send COMPLETE
    if (config.verbose) std::cout << "[Sender] Finalizing transfer (COMPLETE)...\n";
    if (!sendFramedMessage(sock, MessageType::COMPLETE, {})) {
        error = "Failed to send COMPLETE: " + getLastSocketError();
        return false;
    }

    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Failed to receive COMPLETE_ACK: " + error;
        return false;
    }
    if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
        std::string remoteErr(ackPayload.begin(), ackPayload.end());
        error = "Receiver failed transfer finalization: " + (remoteErr.empty() ? "Remote error" : remoteErr);
        return false;
    }
    if (ackHdr.type != static_cast<uint16_t>(MessageType::COMPLETE_ACK)) {
        error = "Expected COMPLETE_ACK, received " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
        return false;
    }

    if (config.verbose) {
        std::cout << "[Sender] Transfer completed successfully!\n";
    }

    client.disconnect();
    return true;
}

} // namespace retro::net
