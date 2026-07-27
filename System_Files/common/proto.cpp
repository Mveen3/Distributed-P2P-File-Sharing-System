#include "proto.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace proto {

bool send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;  // signal, not a failure: retry
            return false;
        }
        if (r == 0) return false;  // peer closed
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t r = ::recv(fd, p + got, len - got, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        // r == 0 is a clean EOF. It must be distinguished from EINTR: testing
        // errno after a zero return reads a stale value and can spin forever.
        if (r == 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool send_msg(int fd, const std::string &msg) {
    if (msg.size() > MAX_MSG) return false;
    uint32_t n = htonl(static_cast<uint32_t>(msg.size()));
    if (!send_all(fd, &n, sizeof(n))) return false;
    if (msg.empty()) return true;
    return send_all(fd, msg.data(), msg.size());
}

bool recv_msg(int fd, std::string &out) {
    uint32_t n = 0;
    if (!recv_all(fd, &n, sizeof(n))) return false;
    n = ntohl(n);
    if (n > MAX_MSG) return false;
    out.assign(n, '\0');
    if (n == 0) return true;
    return recv_all(fd, &out[0], n);
}

std::vector<std::string> split_ws(const std::string &s) {
    std::istringstream iss(s);
    std::vector<std::string> parts;
    std::string tok;
    while (iss >> tok) parts.push_back(tok);
    return parts;
}

bool parse_port(const std::string &s, uint16_t &port) {
    if (s.empty() || s.size() > 5) return false;
    unsigned long value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    if (value == 0 || value > 65535) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

bool parse_endpoint(const std::string &ep, std::string &host, uint16_t &port) {
    size_t colon = ep.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    if (!parse_port(ep.substr(colon + 1), port)) return false;
    host = ep.substr(0, colon);

    // Only IPv4 literals are accepted; validating here keeps every caller from
    // having to second-guess inet_pton's return value.
    struct in_addr addr;
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) return false;
    return true;
}

std::string make_endpoint(const std::string &host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

bool set_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return false;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return false;
    return true;
}

int dial(const std::string &endpoint, int timeout_sec) {
    std::string host;
    uint16_t port = 0;
    if (!parse_endpoint(endpoint, host, port)) return -1;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &sa.sin_addr);  // validated above

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr;
        do {
            pr = ::poll(&pfd, 1, timeout_sec * 1000);
        } while (pr < 0 && errno == EINTR);
        if (pr <= 0) {  // timed out or poll failed
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }

    if (::fcntl(fd, F_SETFL, flags) < 0 || !set_timeout(fd, timeout_sec)) {
        ::close(fd);
        return -1;
    }

    // Piece requests are small and latency sensitive; batching them behind
    // Nagle's algorithm only adds delay.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

int listen_on(const std::string &host, uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        std::fprintf(stderr, "listen_on: invalid address '%s'\n", host.c_str());
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0) {
        std::perror("bind");
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        std::perror("listen");
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string peer_address(int fd) {
    sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&sa), &len) < 0) return "";
    char buf[INET_ADDRSTRLEN];
    if (!::inet_ntop(AF_INET, &sa.sin_addr, buf, sizeof(buf))) return "";
    return std::string(buf);
}

uint16_t local_port(int fd) {
    sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&sa), &len) < 0) return 0;
    return ntohs(sa.sin_port);
}

void ignore_sigpipe() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &sa, nullptr);
}

}  // namespace proto
