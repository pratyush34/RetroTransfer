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

    // Put socket in non-blocking mode to implement genuine connection timeout
#ifdef _WIN32
    u_long nonBlocking = 1;
    ioctlsocket(socketFd_, FIONBIO, &nonBlocking);
#else
    int origFlags = fcntl(socketFd_, F_GETFL, 0);
    fcntl(socketFd_, F_SETFL, origFlags | O_NONBLOCK);
#endif

    int connRes = ::connect(socketFd_, addrResult->ai_addr, static_cast<int>(addrResult->ai_addrlen));
    bool connectSuccess = false;

    if (connRes == 0) {
        connectSuccess = true;
    } else {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
#else
        if (errno == EINPROGRESS) {
#endif
            fd_set writeFds, exceptFds;
            FD_ZERO(&writeFds);
            FD_ZERO(&exceptFds);
            FD_SET(socketFd_, &writeFds);
            FD_SET(socketFd_, &exceptFds);

            struct timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            int sel = select(static_cast<int>(socketFd_ + 1), nullptr, &writeFds, &exceptFds, (timeoutMs > 0 ? &tv : nullptr));
            if (sel == 0) {
                lastError_ = "Connection to " + host + ":" + portStr + " timed out after " + std::to_string(timeoutMs) + " ms";
            } else if (sel < 0) {
                lastError_ = "Select failed during connect: " + getLastSocketError();
            } else {
                int soError = 0;
                socklen_t soLen = sizeof(soError);
                if (getsockopt(socketFd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen) < 0 || soError != 0) {
                    lastError_ = "Failed to connect to " + host + ":" + portStr + " (socket error: " + std::to_string(soError) + ")";
                } else {
                    connectSuccess = true;
                }
            }
        } else {
            lastError_ = "Failed to initiate connect to " + host + ":" + portStr + ": " + getLastSocketError();
        }
    }

    // Restore socket to blocking mode
#ifdef _WIN32
    u_long blocking = 0;
    ioctlsocket(socketFd_, FIONBIO, &blocking);
#else
    fcntl(socketFd_, F_SETFL, origFlags);
#endif

    if (!connectSuccess) {
        closeSocket(socketFd_);
        socketFd_ = INVALID_SOCKET_FD;
        freeaddrinfo(addrResult);
        return false;
    }

    // Set receive and send timeouts for data phase
    setSocketTimeout(socketFd_, timeoutMs);

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
