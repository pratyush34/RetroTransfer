#ifndef RETROTRANSFER_TCP_CLIENT_H
#define RETROTRANSFER_TCP_CLIENT_H

#include "network/socket_common.h"

#include <string>

namespace retro::net {

class TcpClient {
public:
    TcpClient();
    ~TcpClient();

    // Prevent copying, allow moving
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&& other) noexcept;
    TcpClient& operator=(TcpClient&& other) noexcept;

    /*
     * Connect to receiver at given host and port.
     * timeoutMs specifies the connection timeout.
     */
    bool connect(const std::string& host, uint16_t port, int timeoutMs = 10000);

    /*
     * Disconnect and release socket resources.
     */
    void disconnect();

    bool isConnected() const;
    socket_t getSocket() const;
    const std::string& getLastError() const;

private:
    socket_t socketFd_ = INVALID_SOCKET_FD;
    bool connected_ = false;
    std::string lastError_;
};

} // namespace retro::net

#endif // RETROTRANSFER_TCP_CLIENT_H
