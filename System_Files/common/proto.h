#ifndef P2P_PROTO_H
#define P2P_PROTO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wire framing and socket helpers shared by the tracker, the client and the
// peer-to-peer transfer path.
//
// Every control message is length-prefixed:
//
//     +---------------------+-------------------------+
//     | length (4B, net BO) | payload (length bytes)   |
//     +---------------------+-------------------------+
//
// which is what makes partial reads and writes a non-issue: send_all/recv_all
// loop until the exact byte count has moved, so a short send() or a TCP segment
// boundary in the middle of a message is invisible to callers.
//
// Bulk piece payloads are *not* framed again; the piece length is carried in the
// preceding status message and the raw bytes follow, so a 512 KB piece costs one
// header plus one contiguous body.
namespace proto {

// Largest framed message we will send or accept. The biggest legitimate message
// is UPLOAD_FILE for a 1 GB file: 2048 piece hashes * 41 bytes ~= 84 KB. 8 MB
// leaves a wide margin while still bounding how much memory a malformed or
// hostile peer can make us allocate.
const uint32_t MAX_MSG = 8u * 1024 * 1024;

// Loop until len bytes have been transferred. Return false on EOF or error.
bool send_all(int fd, const void *buf, size_t len);
bool recv_all(int fd, void *buf, size_t len);

bool send_msg(int fd, const std::string &msg);
bool recv_msg(int fd, std::string &out);

std::vector<std::string> split_ws(const std::string &s);

// "host:port" <-> components. Returns false for anything malformed, including
// non-numeric or out-of-range ports (a plain stoi() here would throw).
bool parse_endpoint(const std::string &ep, std::string &host, uint16_t &port);
std::string make_endpoint(const std::string &host, uint16_t port);
bool parse_port(const std::string &s, uint16_t &port);

// Connect with a real timeout (non-blocking connect + poll) so an unreachable
// host fails in timeout_sec instead of hanging on the kernel's SYN retry policy.
// Returns a connected fd with send/recv timeouts armed, or -1.
int dial(const std::string &endpoint, int timeout_sec);

// Bound, listening socket, or -1 with a message on stderr. An empty host binds
// to every interface.
int listen_on(const std::string &host, uint16_t port, int backlog);

bool set_timeout(int fd, int seconds);

// Dotted-quad address of the remote end, or "" if it cannot be determined.
std::string peer_address(int fd);

// Port a socket is actually bound to. Needed after binding port 0, where the
// kernel picks a free port for us.
uint16_t local_port(int fd);

// A peer vanishing mid-transfer must not kill the process; all sends already use
// MSG_NOSIGNAL, this covers everything else (e.g. writes to a closed pipe).
void ignore_sigpipe();

}  // namespace proto

#endif
