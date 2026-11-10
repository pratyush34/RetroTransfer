#include "network/socket_common.h"

#include <algorithm>
#include <iostream>

namespace retro::net {

static bool g_networkingInitialized = false;

bool initializeNetworking() {
#ifdef _WIN32
    if (!g_networkingInitialized) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            std::cerr << "WSAStartup failed with error: " << result << "\n";
            return false;
        }
        g_networkingInitialized = true;
    }
    return true;
#else
    g_networkingInitialized = true;
    return true;
#endif
}

void cleanupNetworking() {
#ifdef _WIN32
    if (g_networkingInitialized) {
        WSACleanup();
        g_networkingInitialized = false;
    }
#endif
}

void closeSocket(socket_t sock) {
    if (sock != INVALID_SOCKET_FD) {
#ifdef _WIN32
        shutdown(sock, SD_BOTH);
        closesocket(sock);
#else
        shutdown(sock, SHUT_RDWR);
        close(sock);
#endif
    }
}

bool setSocketTimeout(socket_t sock, int timeoutMs) {
    if (sock == INVALID_SOCKET_FD) {
        return false;
    }

#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    int res1 = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                          reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    int res2 = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                          reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return (res1 == 0 && res2 == 0);
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int res1 = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int res2 = setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return (res1 == 0 && res2 == 0);
#endif
}

bool sendAll(socket_t sock, const void* buffer, size_t length) {
    if (sock == INVALID_SOCKET_FD || (buffer == nullptr && length > 0)) {
        return false;
    }

    const char* ptr = static_cast<const char*>(buffer);
    size_t totalSent = 0;

    while (totalSent < length) {
        size_t chunk = std::min(length - totalSent, static_cast<size_t>(65536));
#ifdef _WIN32
        int sent = send(sock, ptr + totalSent, static_cast<int>(chunk), 0);
#else
        ssize_t sent = send(sock, ptr + totalSent, chunk, 0);
#endif
        if (sent <= 0) {
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }

    return true;
}

bool receiveAll(socket_t sock, void* buffer, size_t length) {
    if (sock == INVALID_SOCKET_FD || (buffer == nullptr && length > 0)) {
        return false;
    }

    char* ptr = static_cast<char*>(buffer);
    size_t totalReceived = 0;

    while (totalReceived < length) {
        size_t chunk = std::min(length - totalReceived, static_cast<size_t>(65536));
#ifdef _WIN32
        int received = recv(sock, ptr + totalReceived, static_cast<int>(chunk), 0);
#else
        ssize_t received = recv(sock, ptr + totalReceived, chunk, 0);
#endif
        if (received <= 0) {
            return false;
        }
        totalReceived += static_cast<size_t>(received);
    }

    return true;
}

std::string getLastSocketError() {
#ifdef _WIN32
    int err = WSAGetLastError();
    char msgBuf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   msgBuf, sizeof(msgBuf), nullptr);
    return std::string("Winsock error ") + std::to_string(err) + ": " + msgBuf;
#else
    return std::string("Socket error ") + std::to_string(errno) + ": " + strerror(errno);
#endif
}

} // namespace retro::net
