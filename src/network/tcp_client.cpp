#include "network/tcp_client.h"

#include <cstring>
#include <iostream>

namespace retro::net {

TcpClient::TcpClient() {
    initializeNetworking();
}

TcpClient::~TcpClient() {
    disconnect();
}

TcpClient::TcpClient(TcpClient&& other) noexcept
    : socketFd_(other.socketFd_),
      connected_(other.connected_),
      lastError_(std::move(other.lastError_)) {
    other.socketFd_ = INVALID_SOCKET_FD;
    other.connected_ = false;
}

TcpClient& TcpClient::operator=(TcpClient&& other) noexcept {
    if (this != &other) {
        disconnect();
        socketFd_ = other.socketFd_;
        connected_ = other.connected_;
        lastError_ = std::move(other.lastError_);
        other.socketFd_ = INVALID_SOCKET_FD;
        other.connected_ = false;
    }
    return *this;
}

bool TcpClient::connect(const std::string& host, uint16_t port, int timeoutMs) {
    disconnect();
    lastError_.clear();

    if (!initializeNetworking()) {
        lastError_ = "Failed to initialize networking subsystem";
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string portStr = std::to_string(port);
    struct addrinfo* addrResult = nullptr;

    int res = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &addrResult);
    if (res != 0 || !addrResult) {
        lastError_ = "Could not resolve host '" + host + "': " +
                     (addrResult ? gai_strerror(res) : "No address records found");
        return false;
    }

    socketFd_ = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
    if (socketFd_ == INVALID_SOCKET_FD) {
        lastError_ = "Failed to create socket: " + getLastSocketError();
        freeaddrinfo(addrResult);
        return false;
    }

    // Enable TCP_NODELAY to avoid Nagle's algorithm delay
    int flag = 1;
    setsockopt(socketFd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    // Set connection/socket timeout (15 seconds default)
    setSocketTimeout(socketFd_, timeoutMs);

    if (::connect(socketFd_, addrResult->ai_addr, static_cast<int>(addrResult->ai_addrlen)) == SOCKET_CALL_ERROR) {
        lastError_ = "Failed to connect to " + host + ":" + portStr + ": " + getLastSocketError();
        closeSocket(socketFd_);
        socketFd_ = INVALID_SOCKET_FD;
        freeaddrinfo(addrResult);
        return false;
    }

    freeaddrinfo(addrResult);
    connected_ = true;
    return true;
}

void TcpClient::disconnect() {
    if (socketFd_ != INVALID_SOCKET_FD) {
        closeSocket(socketFd_);
        socketFd_ = INVALID_SOCKET_FD;
    }
    connected_ = false;
}

bool TcpClient::isConnected() const {
    return connected_ && (socketFd_ != INVALID_SOCKET_FD);
}

socket_t TcpClient::getSocket() const {
    return socketFd_;
}

const std::string& TcpClient::getLastError() const {
    return lastError_;
}

} // namespace retro::net
