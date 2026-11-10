#include "network/tcp_server.h"

#include <cstring>
#include <iostream>

namespace retro::net {

TcpServer::TcpServer() {
    initializeNetworking();
}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(uint16_t port, const std::string& bindIp) {
    stop();
    lastError_.clear();

    if (!initializeNetworking()) {
        lastError_ = "Failed to initialize networking subsystem";
        return false;
    }

    listenFd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenFd_ == INVALID_SOCKET_FD) {
        lastError_ = "Failed to create listening socket: " + getLastSocketError();
        return false;
    }

    // Set SO_REUSEADDR
    int reuse = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in serverAddr;
    std::memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (bindIp.empty() || bindIp == "0.0.0.0") {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, bindIp.c_str(), &serverAddr.sin_addr) <= 0) {
            lastError_ = "Invalid bind IP address: " + bindIp;
            stop();
            return false;
        }
    }

    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_CALL_ERROR) {
        lastError_ = "Failed to bind socket to port " + std::to_string(port) + ": " + getLastSocketError();
        stop();
        return false;
    }

    if (listen(listenFd_, 5) == SOCKET_CALL_ERROR) {
        lastError_ = "Failed to listen on socket: " + getLastSocketError();
        stop();
        return false;
    }

    // Retrieve actual assigned port if 0 was passed
    if (port == 0) {
        struct sockaddr_in boundAddr;
        socklen_t addrLen = sizeof(boundAddr);
        if (getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&boundAddr), &addrLen) == 0) {
            port_ = ntohs(boundAddr.sin_port);
        } else {
            port_ = port;
        }
    } else {
        port_ = port;
    }

    return true;
}

socket_t TcpServer::acceptClient(std::string& outClientIp, uint16_t& outClientPort, int timeoutMs) {
    if (listenFd_ == INVALID_SOCKET_FD) {
        lastError_ = "Server is not listening";
        return INVALID_SOCKET_FD;
    }

    if (timeoutMs > 0) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(listenFd_, &readFds);

        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int selectRes = select(static_cast<int>(listenFd_ + 1), &readFds, nullptr, nullptr, &tv);
        if (selectRes <= 0) {
            lastError_ = (selectRes == 0) ? "Accept timed out" : "Select failed: " + getLastSocketError();
            return INVALID_SOCKET_FD;
        }
    }

    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    socket_t clientFd = accept(listenFd_, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

    if (clientFd == INVALID_SOCKET_FD) {
        lastError_ = "Failed to accept client connection: " + getLastSocketError();
        return INVALID_SOCKET_FD;
    }

    // Enable TCP_NODELAY on accepted connection
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    char ipStr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
    outClientIp = ipStr;
    outClientPort = ntohs(clientAddr.sin_port);

    return clientFd;
}

void TcpServer::stop() {
    if (listenFd_ != INVALID_SOCKET_FD) {
        closeSocket(listenFd_);
        listenFd_ = INVALID_SOCKET_FD;
    }
    port_ = 0;
}

bool TcpServer::isListening() const {
    return listenFd_ != INVALID_SOCKET_FD;
}

uint16_t TcpServer::getPort() const {
    return port_;
}

const std::string& TcpServer::getLastError() const {
    return lastError_;
}

} // namespace retro::net
