#ifndef RETROTRANSFER_TCP_SERVER_H
#define RETROTRANSFER_TCP_SERVER_H

#include "network/socket_common.h"

#include <string>

namespace retro::net {

class TcpServer {
public:
    TcpServer();
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    /*
     * Start listening on the specified port.
     * If port is 0, the OS assigns an ephemeral port (useful for dynamic testing).
     */
    bool start(uint16_t port, const std::string& bindIp = "0.0.0.0");

    /*
     * Accept an incoming client connection.
     * timeoutMs specifies max wait time (0 = block until client arrives).
     * Returns socket_t for client, or INVALID_SOCKET_FD on timeout / failure.
     */
    socket_t acceptClient(std::string& outClientIp, uint16_t& outClientPort, int timeoutMs = 0);

    /*
     * Stop server and close listening socket.
     */
    void stop();

    bool isListening() const;
    uint16_t getPort() const;
    const std::string& getLastError() const;

private:
    socket_t listenFd_ = INVALID_SOCKET_FD;
    uint16_t port_ = 0;
    std::string lastError_;
};

} // namespace retro::net

#endif // RETROTRANSFER_TCP_SERVER_H
