#ifndef RETROTRANSFER_SOCKET_COMMON_H
#define RETROTRANSFER_SOCKET_COMMON_H

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
constexpr int SOCKET_CALL_ERROR = SOCKET_ERROR;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int socket_t;
constexpr socket_t INVALID_SOCKET_FD = -1;
constexpr int SOCKET_CALL_ERROR = -1;
#endif

namespace retro::net {

/*
 * Global networking environment initialization and cleanup.
 * Handles WSAStartup on Windows and no-op on POSIX.
 */
bool initializeNetworking();
void cleanupNetworking();

/*
 * Portable socket closure.
 */
void closeSocket(socket_t sock);

/*
 * Configure receive and send timeouts in milliseconds.
 */
bool setSocketTimeout(socket_t sock, int timeoutMs);

/*
 * Reliable send: loops until all requested bytes are sent,
 * or returns false on socket error / broken connection.
 */
bool sendAll(socket_t sock, const void* buffer, size_t length);

/*
 * Reliable receive: loops until all requested bytes are received,
 * or returns false on peer disconnect / socket error.
 */
bool receiveAll(socket_t sock, void* buffer, size_t length);

/*
 * Retrieve last socket error message in human-readable format.
 */
std::string getLastSocketError();

} // namespace retro::net

#endif // RETROTRANSFER_SOCKET_COMMON_H
