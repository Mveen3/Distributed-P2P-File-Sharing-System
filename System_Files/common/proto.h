#ifndef PROTO_H
#define PROTO_H

#include <string>
#include <vector>
#include <cstdint>
#include <sys/types.h>

ssize_t send_all(int fd, const void *buf, size_t len);
ssize_t recv_all(int fd, void *buf, size_t len);
bool send_msg(int fd, const std::string &s);
bool recv_msg(int fd, std::string &out);
std::vector<std::string> split_ws(const std::string &s);

#endif