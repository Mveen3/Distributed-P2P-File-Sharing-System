#include "proto.h"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <cstring>
#include <sstream>

ssize_t send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t sent = 0;
    while(sent < len) {
        ssize_t r = send(fd, p + sent, len - sent, 0);
        if(r <= 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        sent += r;
    }
    return sent;
}

ssize_t recv_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t*)buf;
    size_t got = 0;
    while(got < len) {
        ssize_t r = recv(fd, p + got, len - got, 0);
        if(r <= 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        got += r;
    }
    return got;
}

bool send_msg(int fd, const std::string &s) {
    uint32_t n = htonl((uint32_t)s.size());
    if(send_all(fd, &n, 4) != 4) return false;
    if(s.size() == 0) return true;
    return send_all(fd, s.data(), s.size()) == (ssize_t)s.size();
}

bool recv_msg(int fd, std::string &out) {
    uint32_t n;
    if(recv_all(fd, &n, 4) != 4) return false;
    n = ntohl(n);
    if(n == 0) {
        out.clear();
        return true;
    }
    if(n > 1024*1024) return false;
    out.resize(n);
    if(recv_all(fd, &out[0], n) != (ssize_t)n) return false;
    return true;
}

std::vector<std::string> split_ws(const std::string &s) {
    std::istringstream ss(s);
    std::vector<std::string> v;
    std::string t;
    while(ss >> t) v.push_back(t);
    return v;
}