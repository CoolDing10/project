#ifndef ADAPTIVE_SOCKET_HPP
#define ADAPTIVE_SOCKET_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace adaptive {

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket();
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;

    bool connectTo(const std::string &host, std::uint16_t port, std::string *error = nullptr);
    bool listenOn(std::uint16_t port, std::string *error = nullptr);
    Socket acceptOne(std::string *error = nullptr) const;
    bool sendAll(const std::uint8_t *data, std::size_t size, std::string *error = nullptr) const;
    long receive(std::uint8_t *data, std::size_t size, std::string *error = nullptr) const;
    bool waitReadable(int timeoutMs) const;
    bool tcpMetrics(double &estimatedCapacityBps, double &rttMs) const;
    void close();
    bool valid() const { return fd_ >= 0; }
    int nativeHandle() const { return fd_; }

private:
    int fd_ = -1;
};

} // namespace adaptive

#endif
