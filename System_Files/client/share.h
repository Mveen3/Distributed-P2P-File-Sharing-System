#ifndef P2P_CLIENT_SHARE_H
#define P2P_CLIENT_SHARE_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace client {

// Files are split into fixed 512 KB pieces; only the last piece of a file may be
// shorter.
const size_t PIECE_SIZE = 512 * 1024;

size_t piece_count(uint64_t size);
size_t piece_length(uint64_t size, size_t index);

struct FileInfo {
    std::string path;
    uint64_t size = 0;
    std::string file_sha;                 // SHA1 of the whole file
    std::vector<std::string> piece_sha;   // SHA1 of each piece, in order
};

// Reads a file once, producing the per-piece digests and the whole-file digest
// in the same pass. Peak memory is one piece, so a 1 GB file costs 512 KB.
bool scan_file(const std::string &path, FileInfo &out, std::string &error);

// The set of (group, file) pairs this client is currently seeding, together with
// where each one lives on disk. Read by the peer server threads, written by the
// command loop, hence the lock.
class ShareRegistry {
public:
    void add(const std::string &group, const std::string &name, const std::string &path,
             uint64_t size);
    bool remove(const std::string &group, const std::string &name);
    void clear();
    bool lookup(const std::string &group, const std::string &name, std::string &path,
                uint64_t &size) const;
    std::vector<std::pair<std::string, std::string>> list() const;

private:
    struct Entry {
        std::string path;
        uint64_t size;
    };
    mutable std::mutex m_;
    std::map<std::pair<std::string, std::string>, Entry> entries_;
};

// Serves piece requests on an already-bound listening socket. Takes ownership of
// listen_fd and runs until the process exits.
void run_peer_server(int listen_fd, ShareRegistry *registry);

// Downloader side of the piece protocol: asks peer for one piece, checks its
// length and SHA1, and only then hands it back. A piece that fails either check
// is reported as a failure so the caller can go to a different peer.
bool fetch_piece(const std::string &peer, const std::string &group, const std::string &name,
                 size_t index, size_t expected_len, const std::string &expected_sha,
                 std::vector<char> &out);

}  // namespace client

#endif
