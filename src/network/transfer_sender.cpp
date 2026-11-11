#include "network/transfer_sender.h"
#include "compression_engine.h"
#include "storage/json_helper.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <map>
#include <unordered_set>
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

bool TransferSender::extractChunkListFromManifest(const std::string& path,
                                                  std::vector<ChunkListEntry>& outChunks,
                                                  uint64_t& outTotalBytes,
                                                  std::string& error) {
    outChunks.clear();
    outTotalBytes = 0;

    std::ifstream file(path);
    if (!file) {
        error = "Could not open manifest: " + path;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    retro::storage::StorageJsonParser parser(content);
    retro::storage::StorageJsonValue root;
    if (!parser.parse(root, error)) {
        return false;
    }

    auto filesIt = root.object.find("files");
    if (filesIt == root.object.end() || !filesIt->second.isArray()) {
        return true;
    }

    std::map<std::string, ChunkListEntry> uniqueChunks;
    for (const auto& fVal : filesIt->second.array) {
        if (!fVal.isObject()) continue;
        auto chunksIt = fVal.object.find("chunks");
        if (chunksIt == fVal.object.end() || !chunksIt->second.isArray()) continue;
        for (const auto& cVal : chunksIt->second.array) {
            if (!cVal.isObject()) continue;
            auto fpIt = cVal.object.find("fingerprint");
            auto lenIt = cVal.object.find("length");
            auto crcIt = cVal.object.find("crc32");
            if (fpIt == cVal.object.end() || lenIt == cVal.object.end() || crcIt == cVal.object.end()) {
                continue;
            }
            std::string fp = fpIt->second.asString();
            uint64_t len = lenIt->second.asUint64();
            uint32_t crc = static_cast<uint32_t>(crcIt->second.asUint64());
            outTotalBytes += len;
            if (uniqueChunks.find(fp) == uniqueChunks.end()) {
                uniqueChunks[fp] = ChunkListEntry{fp, len, crc};
            }
        }
    }

    outChunks.reserve(uniqueChunks.size());
    for (auto& pair : uniqueChunks) {
        outChunks.push_back(std::move(pair.second));
    }
    return true;
}

void TransferSender::printDedupSummary(const DedupSummaryMsg& stats, double elapsedSec) {
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
    std::cout << "              REMOTE DEDUPLICATION & TRANSFER               \n";
    std::cout << "============================================================\n";
    std::cout << "Total Project Chunks     : " << stats.totalProjectChunks << "\n";
    std::cout << "Chunks Already on Remote : " << stats.availableChunks << " ("
              << std::fixed << std::setprecision(1) << dedupPercent << "% deduplicated)\n";
    std::cout << "Missing Chunks Requested : " << stats.missingChunks << " ("
              << std::fixed << std::setprecision(1) << missingPercent << "%)\n";
    std::cout << "Chunks Transmitted       : " << stats.transferredChunks << "\n";
    std::cout << "Total Original Data      : " << stats.totalOriginalBytes << " bytes ("
              << (stats.totalOriginalBytes / 1024) << " KB)\n";
    std::cout << "Deduplication Savings    : " << stats.deduplicatedBytes << " bytes ("
              << (stats.deduplicatedBytes / 1024) << " KB saved from transfer)\n";
    std::cout << "Transmitted Payload Wire : " << stats.transmittedPayloadBytes << " bytes ("
              << (stats.transmittedPayloadBytes / 1024) << " KB)\n";
    std::cout << "Compression Savings      : " << stats.compressionSavingsBytes << " bytes\n";
    std::cout << "Total Transfer Time      : " << std::fixed << std::setprecision(2)
              << (elapsedSec * 1000.0) << " ms\n";
    std::cout << "Transfer Throughput      : " << std::fixed << std::setprecision(2)
              << speedMBps << " MB/s\n";
    std::cout << "============================================================\n";
}

static bool writeSelectiveManifestFile(const std::string& inputManifestPath,
                                       const std::string& outputManifestPath,
                                       const std::string& payloadFilename,
                                       const CompressionResult& compResult) {
    std::string content;
    std::ifstream in(inputManifestPath);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    content = ss.str();

    retro::storage::StorageJsonParser parser(content);
    retro::storage::StorageJsonValue manifestVal;
    std::string err;
    if (!parser.parse(manifestVal, err)) return false;

    auto compVal = retro::storage::StorageJsonValue::makeObject();
    compVal.object["baselineRoot"] = retro::storage::StorageJsonValue::makeString("receiver-persistent-storage");
    compVal.object["payloadFile"] = retro::storage::StorageJsonValue::makeString(payloadFilename);
    compVal.object["payloadFormat"] = retro::storage::StorageJsonValue::makeString("RTPK-v1");
    compVal.object["codec"] = retro::storage::StorageJsonValue::makeString("canonical-huffman-per-chunk-with-raw-fallback");
    compVal.object["totalChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.totalChunks);
    compVal.object["reusableChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.reusableChunks);
    compVal.object["missingChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.missingChunks);
    compVal.object["duplicateChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.duplicateChunks);
    compVal.object["huffmanChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.huffmanChunks);
    compVal.object["rawChunks"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.storedRawChunks);
    compVal.object["reusableBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.reusableBytes);
    compVal.object["duplicateBytesAvoided"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.duplicateBytesAvoided);
    compVal.object["totalSourceBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.totalSourceBytes);
    compVal.object["selectedInputBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.selectedInputBytes);
    compVal.object["uniqueInputBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.uniqueInputBytes);
    compVal.object["payloadBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.payloadBytes);
    compVal.object["bundleHeaderBytes"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.bundleHeaderBytes);
    compVal.object["compressionBytesSaved"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.compressionBytesSaved);
    compVal.object["totalBytesAvoided"] = retro::storage::StorageJsonValue::makeNumber(compResult.metrics.totalBytesAvoided);

    auto chunksArr = retro::storage::StorageJsonValue::makeArray();
    for (const auto& c : compResult.payloadChunks) {
        auto cObj = retro::storage::StorageJsonValue::makeObject();
        cObj.object["path"] = retro::storage::StorageJsonValue::makeString(c.metadata.relativePath);
        cObj.object["seq"] = retro::storage::StorageJsonValue::makeNumber(static_cast<uint64_t>(c.metadata.sequenceNumber));
        cObj.object["fingerprint"] = retro::storage::StorageJsonValue::makeString(c.metadata.fingerprint);
        cObj.object["sourceOffset"] = retro::storage::StorageJsonValue::makeNumber(c.metadata.offset);
        cObj.object["originalSize"] = retro::storage::StorageJsonValue::makeNumber(c.metadata.length);
        cObj.object["payloadOffset"] = retro::storage::StorageJsonValue::makeNumber(c.payloadOffset);
        cObj.object["storedSize"] = retro::storage::StorageJsonValue::makeNumber(c.storedSize);
        cObj.object["codec"] = retro::storage::StorageJsonValue::makeString(c.codec);
        cObj.object["crc32"] = retro::storage::StorageJsonValue::makeNumber(static_cast<uint64_t>(c.metadata.crc32));
        chunksArr.array.push_back(std::move(cObj));
    }
    compVal.object["payloadChunks"] = std::move(chunksArr);

    manifestVal.object["selectiveCompression"] = std::move(compVal);

    std::string serialized = retro::storage::StorageJsonWriter::serialize(manifestVal, 2);
    std::ofstream out(outputManifestPath);
    if (!out) return false;
    out << serialized;
    return true;
}

bool TransferSender::send(const SenderConfig& config, std::string& error) {
    stats_ = {};
    auto transferStartTime = std::chrono::steady_clock::now();

    // 1. Validate manifest path & existence
    fs::path manifestFs(config.manifestPath);
    std::error_code ec;
    if (!fs::exists(manifestFs, ec) || ec || !fs::is_regular_file(manifestFs, ec)) {
        error = "Manifest file does not exist or is not a regular file: " + config.manifestPath;
        return false;
    }

    // 2. Validate manifest JSON
    if (!validateJsonFile(config.manifestPath, error)) {
        return false;
    }

    // 3. Filename check
    std::string manifestFilename = manifestFs.filename().string();
    if (!isValidTransferFilename(manifestFilename, ".json")) {
        error = "Manifest filename is invalid or has wrong extension (expected .json): " + manifestFilename;
        return false;
    }

    std::string payloadFilename = "payload.rtpack";
    if (!config.payloadPath.empty()) {
        payloadFilename = fs::path(config.payloadPath).filename().string();
        if (!isValidTransferFilename(payloadFilename, ".rtpack")) {
            error = "Payload filename is invalid or has wrong extension (expected .rtpack): " + payloadFilename;
            return false;
        }
    }

    // 4. Extract chunk list from manifest for remote deduplication
    std::vector<ChunkListEntry> chunkListEntries;
    uint64_t totalOriginalBytes = 0;
    extractChunkListFromManifest(config.manifestPath, chunkListEntries, totalOriginalBytes, error);

    uint64_t initialPayloadSize = 0;
    if (!config.payloadPath.empty() && fs::exists(config.payloadPath, ec) && fs::is_regular_file(config.payloadPath, ec)) {
        initialPayloadSize = fs::file_size(config.payloadPath, ec);
    }

    if (config.verbose) {
        std::cout << "[Sender] Manifest: " << config.manifestPath << " (" << fs::file_size(manifestFs) << " bytes)\n";
        if (!config.payloadPath.empty()) {
            std::cout << "[Sender] Payload : " << config.payloadPath << " (" << initialPayloadSize << " bytes)\n";
        }
        std::cout << "[Sender] Target  : " << config.receiverIp << ":" << config.receiverPort << "\n";
        std::cout << "[Sender] Connecting to receiver...\n";
    }

    // 5. Connect to receiver
    TcpClient client;
    if (!client.connect(config.receiverIp, config.receiverPort, config.connectionTimeoutMs)) {
        error = "Connection failed: " + client.getLastError();
        return false;
    }
    socket_t sock = client.getSocket();

    // 6. Handshake: send HELLO, expect HELLO_ACK
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

    // 7. Remote Deduplication Negotiation (if enabled and chunks present)
    std::string actualManifestPath = config.manifestPath;
    std::string actualPayloadPath = config.payloadPath;
    uint64_t actualPayloadSize = initialPayloadSize;
    bool skipPayloadStreaming = false;
    fs::path tempSelectivePayload;
    fs::path tempSelectiveManifest;

    auto now = std::chrono::system_clock::now();
    auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    if (config.enableRemoteDedup && !chunkListEntries.empty()) {
        if (config.verbose) {
            std::cout << "[Sender] Communicating required chunk information (" 
                      << chunkListEntries.size() << " unique chunks)...\n";
        }
        ChunkListMsg clMsg;
        clMsg.chunks = chunkListEntries;
        std::vector<uint8_t> clBytes;
        serializeChunkList(clMsg, clBytes);
        if (!sendFramedMessage(sock, MessageType::CHUNK_LIST, clBytes)) {
            error = "Failed to send CHUNK_LIST: " + getLastSocketError();
            return false;
        }

        if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
            error = "Failed to receive MISSING_CHUNKS: " + error;
            return false;
        }
        if (ackHdr.type == static_cast<uint16_t>(MessageType::ERROR_MSG)) {
            std::string remoteErr(ackPayload.begin(), ackPayload.end());
            error = "Receiver reported error during chunk negotiation: " + remoteErr;
            return false;
        }
        if (ackHdr.type != static_cast<uint16_t>(MessageType::MISSING_CHUNKS)) {
            error = "Expected MISSING_CHUNKS, got " + messageTypeToString(static_cast<MessageType>(ackHdr.type));
            return false;
        }

        MissingChunksMsg mcMsg;
        if (!deserializeMissingChunks(ackPayload.data(), ackPayload.size(), mcMsg, error)) {
            return false;
        }

        // Acknowledge receipt of missing chunks list
        if (!sendFramedMessage(sock, MessageType::MISSING_CHUNKS_ACK, {})) {
            error = "Failed to send MISSING_CHUNKS_ACK: " + getLastSocketError();
            return false;
        }

        stats_.totalProjectChunks = mcMsg.totalChunks;
        stats_.availableChunks = mcMsg.availableChunks;
        stats_.missingChunks = mcMsg.missingChunks;
        stats_.deduplicatedBytes = mcMsg.availableBytes;
        stats_.totalOriginalBytes = totalOriginalBytes;

        std::unordered_set<std::string> missingSet(mcMsg.missingFingerprints.begin(), mcMsg.missingFingerprints.end());
        skipPayloadStreaming = false;

        if (missingSet.empty()) {
            actualPayloadSize = 0;
            actualPayloadPath = "";
            stats_.transferredChunks = 0;
            stats_.compressionSavingsBytes = 0;
            skipPayloadStreaming = true;
            if (config.verbose) {
                std::cout << "[Sender] All " << mcMsg.availableChunks 
                          << " chunks are already available on receiver! Zero payload bytes required.\n";
            }
            tempSelectiveManifest = fs::temp_directory_path() / ("manifest_dedup_" + std::to_string(epochMs) + ".json");
            CompressionResult emptyComp;
            emptyComp.metrics.totalChunks = mcMsg.totalChunks;
            emptyComp.metrics.reusableChunks = mcMsg.totalChunks;
            emptyComp.metrics.missingChunks = 0;
            writeSelectiveManifestFile(config.manifestPath, tempSelectiveManifest.string(), payloadFilename, emptyComp);
            actualManifestPath = tempSelectiveManifest.string();
        } else if (missingSet.size() == chunkListEntries.size() && !config.payloadPath.empty() && fs::exists(config.payloadPath)) {
            // All chunks missing and full payload already prepared: transmit original manifest and payload directly
            actualManifestPath = config.manifestPath;
            actualPayloadPath = config.payloadPath;
            actualPayloadSize = initialPayloadSize;
            stats_.transferredChunks = chunkListEntries.size();
        } else {
            tempSelectivePayload = fs::temp_directory_path() / ("payload_selective_" + std::to_string(epochMs) + ".rtpack");
            tempSelectiveManifest = fs::temp_directory_path() / ("manifest_selective_" + std::to_string(epochMs) + ".json");

            CompressionEngine compEngine;
            CompressionResult compResult;
            std::string compErr;
            if (!compEngine.createSelectivePayloadFromExisting(config.manifestPath, config.payloadPath, missingSet,
                                                               tempSelectivePayload.string(), compResult, compErr)) {
                error = "Failed to generate selective payload: " + compErr;
                return false;
            }
            writeSelectiveManifestFile(config.manifestPath, tempSelectiveManifest.string(),
                                       tempSelectivePayload.filename().string(), compResult);
            actualManifestPath = tempSelectiveManifest.string();
            actualPayloadPath = tempSelectivePayload.string();
            actualPayloadSize = fs::file_size(tempSelectivePayload);
            stats_.transferredChunks = compResult.payloadChunks.size();
            stats_.compressionSavingsBytes = compResult.metrics.compressionBytesSaved;
            if (config.verbose) {
                std::cout << "[Sender] Selective payload created: " << stats_.transferredChunks 
                          << " missing chunks (" << actualPayloadSize << " bytes)\n";
            }
        }
    } else {
        stats_.totalProjectChunks = chunkListEntries.size();
        stats_.missingChunks = chunkListEntries.size();
        stats_.transferredChunks = chunkListEntries.size();
        stats_.totalOriginalBytes = totalOriginalBytes;
    }

    uint64_t manifestSize = fs::file_size(actualManifestPath, ec);
    if (ec || manifestSize > MAX_MANIFEST_SIZE) {
        error = "Invalid manifest size: " + std::to_string(manifestSize);
        return false;
    }

    // 8. Send TRANSFER_INFO
    if (config.verbose) std::cout << "[Sender] Sending transfer metadata...\n";
    TransferInfo tInfo;
    tInfo.transferId = "transfer_" + std::to_string(epochMs);
    tInfo.manifestFilename = manifestFilename;
    tInfo.manifestSize = manifestSize;
    tInfo.payloadFilename = payloadFilename;
    tInfo.payloadSize = actualPayloadSize;
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
    if (!streamFileWithQueue(sock, actualManifestPath, manifestSize, MessageType::MANIFEST,
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

    // 10. Send Payload (.rtpack) unless skipped by remote deduplication
    if (!skipPayloadStreaming) {
        if (config.verbose) std::cout << "[Sender] Streaming payload: " << payloadFilename << " (using C circular queue)...\n";
        if (!streamFileWithQueue(sock, actualPayloadPath, actualPayloadSize, MessageType::PAYLOAD,
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
    } else if (config.verbose) {
        std::cout << "[Sender] Skipping payload streaming (100% remote deduplication; 0 missing chunks).\n";
    }

    // 11. Send DEDUP_SUMMARY
    stats_.transmittedPayloadBytes = actualPayloadSize;
    stats_.actualTransmittedBytes = manifestSize + actualPayloadSize;
    auto transferEndTime = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(transferEndTime - transferStartTime).count();
    stats_.totalTransferTimeMs = static_cast<uint64_t>(elapsedSec * 1000.0);

    std::vector<uint8_t> sumBytes;
    serializeDedupSummary(stats_, sumBytes);
    if (!sendFramedMessage(sock, MessageType::DEDUP_SUMMARY, sumBytes)) {
        error = "Failed to send DEDUP_SUMMARY: " + getLastSocketError();
        return false;
    }

    if (!receiveFramedMessage(sock, ackHdr, ackPayload, error)) {
        error = "Failed to receive DEDUP_SUMMARY_ACK: " + error;
        return false;
    }

    // 12. Send COMPLETE
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
        printDedupSummary(stats_, elapsedSec);
    }

    client.disconnect();

    if (!tempSelectivePayload.empty()) {
        fs::remove(tempSelectivePayload, ec);
    }
    if (!tempSelectiveManifest.empty()) {
        fs::remove(tempSelectiveManifest, ec);
    }

    return true;
}

} // namespace retro::net
