#include "adaptive/socket.hpp"

#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace adaptive {
namespace {

void setError(std::string *error, const std::string &message) {
    if (error) *error = message;
}

void configureSocket(int fd) {
    const int enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    timeval sendTimeout{2, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
}

} // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket &&other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool Socket::connectTo(const std::string &host, std::uint16_t port, std::string *error) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        setError(error, ::gai_strerror(lookup));
        return false;
    }
    for (addrinfo *address = addresses; address; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype,
                                       address->ai_protocol);
        if (candidate < 0) continue;
        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            fd_ = candidate;
            configureSocket(fd_);
            break;
        }
        ::close(candidate);
    }
    ::freeaddrinfo(addresses);
    if (fd_ < 0) setError(error, std::strerror(errno));
    return fd_ >= 0;
}

bool Socket::listenOn(std::uint16_t port, std::string *error) {
    close();
    fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd_ < 0) {
        setError(error, std::strerror(errno));
        return false;
    }
    const int enabled = 1;
    const int disabled = 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    ::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &disabled, sizeof(disabled));
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        ::listen(fd_, 4) < 0) {
        setError(error, std::strerror(errno));
        close();
        return false;
    }
    return true;
}

Socket Socket::acceptOne(std::string *error) const {
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) {
        setError(error, std::strerror(errno));
        return Socket();
    }
    configureSocket(client);
    return Socket(client);
}

bool Socket::sendAll(const std::uint8_t *data, std::size_t size, std::string *error) const {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t sent = ::send(fd_, data + offset, size - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        setError(error, sent == 0 ? "peer closed" : std::strerror(errno));
        return false;
    }
    return true;
}

long Socket::receive(std::uint8_t *data, std::size_t size, std::string *error) const {
    while (true) {
        const ssize_t received = ::recv(fd_, data, size, 0);
        if (received >= 0) return static_cast<long>(received);
        if (errno == EINTR) continue;
        setError(error, std::strerror(errno));
        return -1;
    }
}

bool Socket::waitReadable(int timeoutMs) const {
    if (fd_ < 0) return false;
    pollfd descriptor{fd_, POLLIN, 0};
    int result;
    do result = ::poll(&descriptor, 1, timeoutMs); while (result < 0 && errno == EINTR);
    return result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR));
}

bool Socket::tcpMetrics(double &estimatedCapacityBps, double &rttMs) const {
    tcp_info info{};
    socklen_t size = sizeof(info);
    if (fd_ < 0 || ::getsockopt(fd_, IPPROTO_TCP, TCP_INFO, &info, &size) < 0 ||
        info.tcpi_rtt == 0)
        return false;
    rttMs = info.tcpi_rtt / 1000.0;
    estimatedCapacityBps = static_cast<double>(info.tcpi_snd_cwnd) *
                           static_cast<double>(info.tcpi_snd_mss) * 8.0 * 1000000.0 /
                           static_cast<double>(info.tcpi_rtt);
    return true;
}

void Socket::close() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace adaptive
