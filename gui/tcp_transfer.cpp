#include "tcp_transfer.h"

#include "fingerprint_c.h"
#include "reconstruction_engine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QStorageInfo>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>
#include <limits>

namespace {

constexpr quint64 kMaximumManifestBytes = 256ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaximumPayloadBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr quint32 kBlockBytes = 256U * 1024U;
constexpr int kDigestBytes = 32;
constexpr int kReadIdleTimeoutMs = 30000;
constexpr int kConnectTimeoutMs = 15000;

void appendU32(QByteArray &data, quint32 value)
{
    data.append(static_cast<char>((value >> 24) & 0xff));
    data.append(static_cast<char>((value >> 16) & 0xff));
    data.append(static_cast<char>((value >> 8) & 0xff));
    data.append(static_cast<char>(value & 0xff));
}

void appendU64(QByteArray &data, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        data.append(static_cast<char>((value >> shift) & 0xff));
    }
}

quint32 readU32(const QByteArray &data)
{
    return (static_cast<quint32>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<quint32>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<quint32>(static_cast<unsigned char>(data[3]));
}

quint64 readU64(const QByteArray &data)
{
    quint64 value = 0;
    for (char byte : data) {
        value = (value << 8) | static_cast<unsigned char>(byte);
    }
    return value;
}

bool secureEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (int index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^
                      static_cast<unsigned char>(right[index]);
    }
    return difference == 0;
}

bool readExact(QTcpSocket &socket, qint64 size, QByteArray &data,
               const std::atomic_bool &cancelled, QString &error)
{
    data.clear();
    if (size < 0 || size > std::numeric_limits<int>::max()) {
        error = "TCP frame exceeds the supported size.";
        return false;
    }

    QElapsedTimer idleTimer;
    idleTimer.start();
    while (data.size() < size) {
        if (cancelled.load()) {
            error = "Transfer cancelled.";
            return false;
        }
        if (idleTimer.elapsed() >= kReadIdleTimeoutMs) {
            error = "Timed out waiting for package data.";
            return false;
        }
        qint64 available = socket.bytesAvailable();
        if (available == 0) {
            if (!socket.waitForReadyRead(250) &&
                socket.state() != QAbstractSocket::ConnectedState) {
                error = "Connection closed while receiving data.";
                return false;
            }
            available = socket.bytesAvailable();
            if (available == 0) continue;
        }
        const qint64 amount = std::min<qint64>(available, size - data.size());
        const QByteArray part = socket.read(amount);
        if (part.isEmpty()) {
            if (socket.state() != QAbstractSocket::ConnectedState) {
                error = "Connection closed while receiving data.";
                return false;
            }
            continue;
        }
        data.append(part);
        idleTimer.restart();
    }
    return true;
}

bool writeAll(QTcpSocket &socket, const QByteArray &data,
              const std::atomic_bool &cancelled, QString &error)
{
    qint64 written = 0;
    while (written < data.size()) {
        if (cancelled.load()) {
            error = "Transfer cancelled.";
            return false;
        }
        const qint64 amount = socket.write(data.constData() + written,
                                           data.size() - written);
        if (amount < 0) {
            error = "Could not write to the TCP connection: " + socket.errorString();
            return false;
        }
        if (amount == 0) {
            error = "TCP connection stopped accepting data.";
            return false;
        }
        written += amount;
        while (socket.bytesToWrite() > 0) {
            if (cancelled.load()) {
                error = "Transfer cancelled.";
                return false;
            }
            if (!socket.waitForBytesWritten(250) &&
                socket.state() != QAbstractSocket::ConnectedState) {
                error = "Connection closed while sending data.";
                return false;
            }
        }
    }
    return true;
}

QByteArray randomBytes(int count)
{
    QByteArray result;
    result.reserve(count);
    QRandomGenerator *generator = QRandomGenerator::system();
    for (int index = 0; index < count; ++index) {
        result.append(static_cast<char>(generator->bounded(256)));
    }
    return result;
}

bool hashFile(const QString &path, QByteArray &digest,
              const std::atomic_bool &cancelled, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = "Could not open package file: " + file.errorString();
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled.load()) {
            error = "Transfer cancelled.";
            return false;
        }
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            error = "Could not read package file: " + file.errorString();
            return false;
        }
        hash.addData(block);
    }
    digest = hash.result();
    return true;
}

QString transferDirectory(const TcpTransferOptions &options,
                         const QByteArray &scanDigest,
                         const QByteArray &manifestDigest,
                         const QByteArray &payloadDigest)
{
    QCryptographicHash id(QCryptographicHash::Sha256);
    id.addData(scanDigest);
    id.addData(manifestDigest);
    id.addData(payloadDigest);
    return QDir(options.incomingDirectory)
        .filePath("incoming/" + QString::fromLatin1(id.result().toHex()));
}

quint64 resumableOffset(QFile &file, quint64 expectedSize, bool &ok,
                        QString &error)
{
    ok = true;
    if (file.size() < 0) {
        ok = false;
        error = "Could not determine the partial transfer file size.";
        return 0;
    }
    const quint64 currentSize = static_cast<quint64>(file.size());
    if (currentSize > expectedSize) {
        if (!file.resize(static_cast<qint64>(expectedSize))) {
            ok = false;
            error = "Could not trim the partial transfer file.";
            return 0;
        }
        return expectedSize;
    }
    if (currentSize == expectedSize || currentSize % kBlockBytes == 0) {
        return currentSize;
    }
    const quint64 safeSize = currentSize - (currentSize % kBlockBytes);
    if (!file.resize(static_cast<qint64>(safeSize))) {
        ok = false;
        error = "Could not discard an incomplete transfer block.";
        return 0;
    }
    return safeSize;
}

bool sendFile(QTcpSocket &socket, quint8 fileType, const QString &path,
              quint64 fileSize, quint64 offset,
              const std::atomic_bool &cancelled,
              const TcpTransferWorker::ProgressCallback &progressCallback,
              QObject *callbackContext, quint64 baseCompleted, quint64 total,
              quint64 &sentBytes, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) ||
        !file.seek(static_cast<qint64>(offset))) {
        error = "Could not open or seek package file: " + file.errorString();
        return false;
    }

    quint64 position = offset;
    while (position < fileSize) {
        const quint32 blockSize = static_cast<quint32>(
            std::min<quint64>(kBlockBytes, fileSize - position));
        const QByteArray block = file.read(blockSize);
        if (block.size() != static_cast<int>(blockSize)) {
            error = "Package file changed or could not be read during transfer.";
            return false;
        }

        QByteArray frame;
        frame.reserve(17 + block.size());
        frame.append(static_cast<char>(fileType));
        appendU64(frame, position);
        appendU32(frame, blockSize);
        appendU32(frame, rt_compute_crc32(
            reinterpret_cast<const uint8_t *>(block.constData()),
            static_cast<size_t>(block.size())));
        frame.append(block);

        bool acknowledged = false;
        for (int attempt = 0; attempt < 3 && !acknowledged; ++attempt) {
            if (!writeAll(socket, frame, cancelled, error)) return false;
            QByteArray ack;
            if (!readExact(socket, 9, ack, cancelled, error)) return false;
            if (static_cast<quint8>(ack[0]) != fileType) {
                error = "Receiver returned an invalid chunk acknowledgement.";
                return false;
            }
            const quint64 nextOffset = readU64(ack.mid(1));
            if (nextOffset == position + blockSize) {
                acknowledged = true;
            } else if (nextOffset != position) {
                error = "Receiver resumed at an unexpected package offset.";
                return false;
            }
        }
        if (!acknowledged) {
            error = "Receiver did not acknowledge a package block.";
            return false;
        }

        position += blockSize;
        sentBytes += blockSize;
        if (callbackContext && progressCallback) {
            QMetaObject::invokeMethod(
                callbackContext,
                [progressCallback, completed = baseCompleted + position,
                 total]() { progressCallback(completed, total); },
                Qt::QueuedConnection);
        }
    }
    return true;
}

bool receiveFile(QTcpSocket &socket, quint8 expectedType, QFile &file,
                 quint64 expectedSize, quint64 &offset,
                 const std::atomic_bool &cancelled,
                 const TcpTransferWorker::ProgressCallback &progressCallback,
                 QObject *callbackContext, quint64 baseCompleted,
                 quint64 total, QString &error)
{
    while (offset < expectedSize) {
        QByteArray header;
        if (!readExact(socket, 17, header, cancelled, error)) return false;
        const quint8 fileType = static_cast<quint8>(header[0]);
        const quint64 blockOffset = readU64(header.mid(1, 8));
        const quint32 blockSize = readU32(header.mid(9, 4));
        const quint32 expectedCrc = readU32(header.mid(13, 4));

        if (fileType != expectedType || blockOffset != offset ||
            blockSize == 0 || blockSize > kBlockBytes ||
            blockSize > expectedSize - offset) {
            error = "Received an invalid or out-of-order package block.";
            return false;
        }

        QByteArray block;
        if (!readExact(socket, blockSize, block, cancelled, error)) return false;
        const uint32_t actualCrc = rt_compute_crc32(
            reinterpret_cast<const uint8_t *>(block.constData()),
            static_cast<size_t>(block.size()));
        if (actualCrc != expectedCrc) {
            QByteArray ack(1, static_cast<char>(expectedType));
            appendU64(ack, offset);
            if (!writeAll(socket, ack, cancelled, error)) return false;
            continue;
        }
        if (!file.seek(static_cast<qint64>(offset)) ||
            file.write(block) != block.size() || !file.flush()) {
            error = "Could not persist a received package block: " +
                    file.errorString();
            return false;
        }

        offset += blockSize;
        QByteArray ack(1, static_cast<char>(expectedType));
        appendU64(ack, offset);
        if (!writeAll(socket, ack, cancelled, error)) return false;

        if (callbackContext && progressCallback) {
            QMetaObject::invokeMethod(
                callbackContext,
                [progressCallback, completed = baseCompleted + offset,
                 total]() { progressCallback(completed, total); },
                Qt::QueuedConnection);
        }
    }
    return true;
}

bool sendStatus(QTcpSocket &socket, bool success, const QString &message,
                const std::atomic_bool &cancelled, QString &error)
{
    const QByteArray text = message.toUtf8();
    QByteArray response(1, success ? '\x01' : '\x00');
    appendU32(response, static_cast<quint32>(text.size()));
    response.append(text);
    return writeAll(socket, response, cancelled, error);
}

bool receiveStatus(QTcpSocket &socket, const std::atomic_bool &cancelled,
                   QString &error)
{
    QByteArray header;
    if (!readExact(socket, 5, header, cancelled, error)) return false;
    const bool success = static_cast<quint8>(header[0]) == 1;
    const quint32 messageSize = readU32(header.mid(1));
    if (messageSize > 1024 * 1024) {
        error = "Receiver returned an oversized status message.";
        return false;
    }
    QByteArray message;
    if (!readExact(socket, messageSize, message, cancelled, error)) return false;
    if (!success) {
        error = QString::fromUtf8(message);
        return false;
    }
    return true;
}

bool receiveOnePackage(QTcpSocket &socket,
                       const TcpTransferOptions &options,
                       const std::atomic_bool &cancelled,
                       const TcpTransferWorker::LogCallback &logCallback,
                       const TcpTransferWorker::ProgressCallback &progressCallback,
                       QObject *callbackContext,
                       QString &summary, QString &error)
{
    QByteArray hello;
    if (!readExact(socket, 156, hello, cancelled, error)) return false;
    if (hello.left(4) != "RTF2") {
        error = "Sender uses an unsupported transfer protocol.";
        return false;
    }
    const QByteArray clientNonce = hello.mid(4, 32);
    const QByteArray descriptor = hello.mid(36, 120);
    const quint64 scanSize = readU64(descriptor.mid(0, 8));
    const quint64 manifestSize = readU64(descriptor.mid(8, 8));
    const quint64 payloadSize = readU64(descriptor.mid(16, 8));
    const QByteArray scanDigest = descriptor.mid(24, 32);
    const QByteArray manifestDigest = descriptor.mid(56, 32);
    const QByteArray payloadDigest = descriptor.mid(88, 32);
    const QByteArray serverNonce = randomBytes(32);
    const QByteArray serverProof = QMessageAuthenticationCode::hash(
        QByteArray("server") + clientNonce + serverNonce + descriptor,
        options.pairingCode.toUtf8(), QCryptographicHash::Sha256);
    if (!writeAll(socket, serverNonce + serverProof, cancelled, error)) return false;

    QByteArray response;
    if (!readExact(socket, kDigestBytes, response, cancelled, error)) return false;
    const QByteArray expected = QMessageAuthenticationCode::hash(
        QByteArray("client") + clientNonce + serverNonce + descriptor,
        options.pairingCode.toUtf8(),
        QCryptographicHash::Sha256);
    if (!secureEqual(response, expected)) {
        error = "Sender pairing code is invalid.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    if (scanSize == 0 || scanSize > kMaximumManifestBytes ||
        manifestSize == 0 || manifestSize > kMaximumManifestBytes ||
        payloadSize < 8 || payloadSize > kMaximumPayloadBytes) {
        error = "Incoming package exceeds configured size limits.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }

    const QString directory = transferDirectory(
        options, scanDigest, manifestDigest, payloadDigest);
    if (!QDir().mkpath(directory)) {
        error = "Could not create persistent incoming-transfer directory.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }

    const QString scanPath = QDir(directory).filePath("scan.json.part");
    const QString manifestPath = QDir(directory).filePath("manifest.json.part");
    const QString payloadPath = QDir(directory).filePath("payload.rtpack.part");
    QFile scanFile(scanPath);
    QFile manifestFile(manifestPath);
    QFile payloadFile(payloadPath);
    if (!scanFile.open(QIODevice::ReadWrite) ||
        !manifestFile.open(QIODevice::ReadWrite) ||
        !payloadFile.open(QIODevice::ReadWrite)) {
        error = "Could not open persistent incoming-transfer files.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }

    bool scanOffsetOk = false;
    bool manifestOffsetOk = false;
    bool payloadOffsetOk = false;
    quint64 scanOffset = resumableOffset(
        scanFile, scanSize, scanOffsetOk, error);
    if (!scanOffsetOk) {
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    quint64 manifestOffset = resumableOffset(
        manifestFile, manifestSize, manifestOffsetOk, error);
    if (!manifestOffsetOk) {
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    quint64 payloadOffset = resumableOffset(
        payloadFile, payloadSize, payloadOffsetOk, error);
    if (!payloadOffsetOk) {
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    QStorageInfo storage(directory);
    if (!storage.isValid() || !storage.isReady() || storage.isReadOnly()) {
        error = "Incoming-transfer storage is not available or writable.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    const qint64 availableSpace = storage.bytesAvailable();
    if (availableSpace < 0) {
        error = "Could not determine free space for the incoming package.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    const quint64 remainingBytes =
        (scanSize - scanOffset) + (manifestSize - manifestOffset) +
        (payloadSize - payloadOffset);
    if (static_cast<quint64>(availableSpace) < remainingBytes) {
        error = "Not enough free space for the incoming package.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }

    if (!sendStatus(socket, true, QString(), cancelled, error)) return false;
    QByteArray offsets;
    appendU64(offsets, scanOffset);
    appendU64(offsets, manifestOffset);
    appendU64(offsets, payloadOffset);
    if (!writeAll(socket, offsets, cancelled, error)) return false;

    const quint64 total = scanSize + manifestSize + payloadSize;
    if (callbackContext && progressCallback) {
        QMetaObject::invokeMethod(
            callbackContext,
            [progressCallback,
             completed = scanOffset + manifestOffset + payloadOffset,
             total]() { progressCallback(completed, total); },
            Qt::QueuedConnection);
    }

    if (!receiveFile(socket, 1, scanFile, scanSize, scanOffset,
                     cancelled, progressCallback, callbackContext, 0,
                     total, error) ||
        !receiveFile(socket, 2, manifestFile, manifestSize, manifestOffset,
                     cancelled, progressCallback, callbackContext, scanSize,
                     total, error) ||
        !receiveFile(socket, 3, payloadFile, payloadSize, payloadOffset,
                     cancelled, progressCallback, callbackContext,
                     scanSize + manifestSize,
                     total, error)) {
        return false;
    }

    QByteArray endMarker;
    if (!readExact(socket, 1, endMarker, cancelled, error) ||
        static_cast<quint8>(endMarker[0]) != 0xff) {
        if (error.isEmpty()) error = "Sender did not finish the package transfer.";
        return false;
    }
    scanFile.close();
    manifestFile.close();
    payloadFile.close();

    QByteArray actualScanDigest;
    QByteArray actualManifestDigest;
    QByteArray actualPayloadDigest;
    if (!hashFile(scanPath, actualScanDigest, cancelled, error) ||
        !hashFile(manifestPath, actualManifestDigest, cancelled, error) ||
        !hashFile(payloadPath, actualPayloadDigest, cancelled, error)) {
        return false;
    }
    if (actualScanDigest != scanDigest ||
        actualManifestDigest != manifestDigest ||
        actualPayloadDigest != payloadDigest) {
        error = "Final scan/manifest/payload SHA-256 verification failed.";
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }

    if (callbackContext && logCallback) {
        QMetaObject::invokeMethod(
            callbackContext,
            [logCallback]() {
                logCallback("Package received and SHA-256 verified; reconstructing project.");
            },
            Qt::QueuedConnection);
    }
    ReconstructionEngine reconstruction;
    ReconstructionResult result;
    std::string reconstructionError;
    if (!reconstruction.reconstruct(
            manifestPath.toStdString(),
            payloadPath.toStdString(),
            options.destinationDirectory.toStdString(), result,
            reconstructionError,
            QDir(options.incomingDirectory).filePath("chunks").toStdString())) {
        error = QString::fromStdString(reconstructionError);
        sendStatus(socket, false, error, cancelled, error);
        return false;
    }
    if (!sendStatus(socket, true, QString(), cancelled, error)) return false;

    summary = QString("Reconstructed %1 files; %2 chunks from payload, %3 reused.")
                  .arg(static_cast<qulonglong>(result.filesWritten))
                  .arg(static_cast<qulonglong>(result.chunksWritten))
                  .arg(static_cast<qulonglong>(result.chunksReused));
    summary += "\nScan, manifest, and payload saved under " + directory;
    return true;
}

bool runSender(const TcpTransferOptions &options,
               const std::atomic_bool &cancelled,
               const TcpTransferWorker::LogCallback &logCallback,
               const TcpTransferWorker::ProgressCallback &progressCallback,
               QObject *callbackContext,
               QString &summary, QString &error)
{
    QFile scan(options.scanPath);
    QFile manifest(options.manifestPath);
    QFile payload(options.payloadPath);
    if (!scan.open(QIODevice::ReadOnly) ||
        !manifest.open(QIODevice::ReadOnly) ||
        !payload.open(QIODevice::ReadOnly)) {
        error = "Could not open the selected scan, manifest, or payload file.";
        return false;
    }

    const quint64 scanSize = static_cast<quint64>(scan.size());
    const quint64 manifestSize = static_cast<quint64>(manifest.size());
    const quint64 payloadSize = static_cast<quint64>(payload.size());
    if (scanSize == 0 || scanSize > kMaximumManifestBytes ||
        manifestSize == 0 || manifestSize > kMaximumManifestBytes ||
        payloadSize < 8 || payloadSize > kMaximumPayloadBytes) {
        error = "Selected package exceeds configured size limits.";
        return false;
    }

    QByteArray scanDigest;
    QByteArray manifestDigest;
    QByteArray payloadDigest;
    if (!hashFile(options.scanPath, scanDigest, cancelled, error) ||
        !hashFile(options.manifestPath, manifestDigest, cancelled, error) ||
        !hashFile(options.payloadPath, payloadDigest, cancelled, error)) {
        return false;
    }

    QTcpSocket socket;
    socket.connectToHost(options.host, options.port);
    QElapsedTimer connectTimer;
    connectTimer.start();
    while (socket.state() == QAbstractSocket::ConnectingState) {
        if (cancelled.load()) {
            error = "Transfer cancelled.";
            socket.abort();
            return false;
        }
        if (connectTimer.elapsed() >= kConnectTimeoutMs) {
            socket.abort();
            error = "Timed out connecting to the receiver.";
            return false;
        }
        socket.waitForConnected(250);
    }
    if (socket.state() != QAbstractSocket::ConnectedState) {
        error = "Could not connect to receiver: " + socket.errorString();
        return false;
    }

    QByteArray descriptor;
    appendU64(descriptor, scanSize);
    appendU64(descriptor, manifestSize);
    appendU64(descriptor, payloadSize);
    descriptor.append(scanDigest);
    descriptor.append(manifestDigest);
    descriptor.append(payloadDigest);
    const QByteArray clientNonce = randomBytes(32);
    if (!writeAll(socket, QByteArray("RTF2", 4) + clientNonce + descriptor,
                  cancelled, error)) {
        return false;
    }

    QByteArray challenge;
    if (!readExact(socket, 64, challenge, cancelled, error)) return false;
    const QByteArray serverNonce = challenge.left(32);
    const QByteArray serverProof = challenge.mid(32);
    const QByteArray expectedServerProof = QMessageAuthenticationCode::hash(
        QByteArray("server") + clientNonce + serverNonce + descriptor,
        options.pairingCode.toUtf8(), QCryptographicHash::Sha256);
    const bool receiverAuthenticated =
        secureEqual(serverProof, expectedServerProof);
    const QByteArray clientProof = QMessageAuthenticationCode::hash(
        QByteArray("client") + clientNonce + serverNonce + descriptor,
        options.pairingCode.toUtf8(), QCryptographicHash::Sha256);
    if (!writeAll(socket, clientProof, cancelled, error) ||
        !receiveStatus(socket, cancelled, error)) {
        return false;
    }
    if (!receiverAuthenticated) {
        error = "Receiver pairing authentication failed.";
        return false;
    }

    QByteArray offsets;
    if (!readExact(socket, 24, offsets, cancelled, error)) return false;
    const quint64 scanOffset = readU64(offsets.mid(0, 8));
    const quint64 manifestOffset = readU64(offsets.mid(8, 8));
    const quint64 payloadOffset = readU64(offsets.mid(16, 8));
    if (scanOffset > scanSize || manifestOffset > manifestSize ||
        payloadOffset > payloadSize) {
        error = "Receiver returned invalid resume offsets.";
        return false;
    }

    if (callbackContext && logCallback) {
        const QString message =
            QString("Connected. Resuming at %1 / %2 scan, %3 / %4 manifest, and %5 / %6 payload bytes.")
                .arg(static_cast<qulonglong>(scanOffset))
                .arg(static_cast<qulonglong>(scanSize))
                .arg(static_cast<qulonglong>(manifestOffset))
                .arg(static_cast<qulonglong>(manifestSize))
                .arg(static_cast<qulonglong>(payloadOffset))
                .arg(static_cast<qulonglong>(payloadSize));
        QMetaObject::invokeMethod(
            callbackContext,
            [logCallback, message]() { logCallback(message); },
            Qt::QueuedConnection);
    }

    quint64 sentBytes = 0;
    const quint64 total = scanSize + manifestSize + payloadSize;
    if (callbackContext && progressCallback) {
        QMetaObject::invokeMethod(
            callbackContext,
            [progressCallback,
             completed = scanOffset + manifestOffset + payloadOffset,
             total]() { progressCallback(completed, total); },
            Qt::QueuedConnection);
    }
    if (!sendFile(socket, 1, options.scanPath, scanSize,
                  scanOffset, cancelled, progressCallback,
                  callbackContext, 0, total, sentBytes, error) ||
        !sendFile(socket, 2, options.manifestPath, manifestSize,
                  manifestOffset, cancelled, progressCallback,
                  callbackContext, scanSize, total, sentBytes, error) ||
        !sendFile(socket, 3, options.payloadPath, payloadSize,
                  payloadOffset, cancelled, progressCallback,
                  callbackContext, scanSize + manifestSize, total,
                  sentBytes, error)) {
        return false;
    }

    if (!writeAll(socket, QByteArray(1, static_cast<char>(0xff)),
                  cancelled, error) ||
        !receiveStatus(socket, cancelled, error)) {
        return false;
    }

    const quint64 resumedBytes = scanOffset + manifestOffset + payloadOffset;

    const double resumePercentage = total > 0
        ? (static_cast<double>(resumedBytes) / total) * 100.0
        : 0.0;

    summary = QString(
        "Transfer completed successfully!\n"
        "New bytes transmitted: %1\n"
        "Bytes resumed: %2\n"
        "Resume efficiency: %3%"
    )
        .arg(static_cast<qulonglong>(sentBytes))
        .arg(static_cast<qulonglong>(resumedBytes))
        .arg(resumePercentage, 0, 'f', 2);

    return true;

}}

TcpTransferWorker::TcpTransferWorker(
    const TcpTransferOptions &options, QObject *callbackContext,
    LogCallback logCallback, ProgressCallback progressCallback, QObject *parent)
    : QThread(parent),
      options_(options),
      callbackContext_(callbackContext),
      logCallback_(std::move(logCallback)),
      progressCallback_(std::move(progressCallback))
{
}

TcpTransferWorker::~TcpTransferWorker()
{
    cancel();
    wait();
}

void TcpTransferWorker::cancel()
{
    cancelled_.store(true);
}

bool TcpTransferWorker::succeeded() const
{
    return succeeded_;
}

QString TcpTransferWorker::errorMessage() const
{
    return errorMessage_;
}

QString TcpTransferWorker::summary() const
{
    return summary_;
}

void TcpTransferWorker::reportLog(const QString &message)
{
    if (!callbackContext_ || !logCallback_) return;
    QMetaObject::invokeMethod(
        callbackContext_,
        [callback = logCallback_, message]() { callback(message); },
        Qt::QueuedConnection);
}

void TcpTransferWorker::run()
{
    succeeded_ = false;
    errorMessage_.clear();
    summary_.clear();

    if (options_.mode == TcpTransferOptions::Mode::SendPackage) {
        succeeded_ = runSender(options_, cancelled_, logCallback_,
                               progressCallback_, callbackContext_,
                               summary_, errorMessage_);
        return;
    }

    QTcpServer server;
    if (!server.listen(QHostAddress::AnyIPv4, options_.port)) {
        errorMessage_ = "Could not listen on the selected TCP port: " +
                        server.errorString();
        return;
    }
    reportLog(QString("Listening on TCP port %1 for a paired sender.")
                  .arg(options_.port));

    int invalidPairings = 0;
    while (!cancelled_.load()) {
        if (!server.waitForNewConnection(250)) {
            if (server.serverError() != QAbstractSocket::UnknownSocketError &&
                !server.isListening()) {
                errorMessage_ = "Receiver socket stopped: " + server.errorString();
                return;
            }
            continue;
        }
        QTcpSocket *socket = server.nextPendingConnection();
        if (!socket) continue;

        QString error;
        QString summary;
        if (receiveOnePackage(*socket, options_, cancelled_, logCallback_,
                              progressCallback_, callbackContext_,
                              summary, error)) {
            socket->disconnectFromHost();
            delete socket;
            succeeded_ = true;
            summary_ = summary;
            return;
        }
        socket->disconnectFromHost();
        socket->waitForDisconnected(250);
        delete socket;

        if (cancelled_.load()) {
            errorMessage_ = "Transfer cancelled.";
            return;
        }
        if (error == "Sender pairing code is invalid." &&
            ++invalidPairings >= 5) {
            errorMessage_ =
                "Too many invalid pairing attempts. Restart the receiver to create a new code.";
            return;
        }
        reportLog("Connection rejected or interrupted: " + error);
    }
    errorMessage_ = "Receiver stopped before a package completed.";
}
