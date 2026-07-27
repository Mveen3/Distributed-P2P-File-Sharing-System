#include "share.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../common/proto.h"
#include "../common/sha1.h"

namespace client {
namespace {

const int kPeerTimeoutSec = 20;

// read()/pread() are allowed to return fewer bytes than asked for; every bulk
// read in this file goes through these loops so a short read is never mistaken
// for a truncated file.
bool read_exact(int fd, void *buf, size_t len, size_t &got) {
    char *p = static_cast<char *>(buf);
    got = 0;
    while (got < len) {
        ssize_t r = ::read(fd, p + got, len - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) break;  // end of file
        got += static_cast<size_t>(r);
    }
    return true;
}

bool pread_exact(int fd, void *buf, size_t len, off_t offset) {
    char *p = static_cast<char *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t r = ::pread(fd, p + got, len - got, offset + static_cast<off_t>(got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

void serve_piece_request(int fd, ShareRegistry *registry) {
    std::string request;
    if (!proto::recv_msg(fd, request)) {
        ::close(fd);
        return;
    }

    std::vector<std::string> a = proto::split_ws(request);
    if (a.size() != 4 || a[0] != "GETPIECE") {
        proto::send_msg(fd, "ERR bad_request");
        ::close(fd);
        return;
    }

    // The file is keyed by group as well as name: the same file name in two
    // different groups is two different files.
    std::string path;
    uint64_t size = 0;
    if (!registry->lookup(a[1], a[2], path, size)) {
        proto::send_msg(fd, "ERR not_shared");
        ::close(fd);
        return;
    }

    char *end = nullptr;
    long long idx = std::strtoll(a[3].c_str(), &end, 10);
    size_t npieces = piece_count(size);
    if (!end || *end != '\0' || idx < 0 || static_cast<size_t>(idx) >= npieces) {
        proto::send_msg(fd, "ERR bad_index");
        ::close(fd);
        return;
    }

    size_t len = piece_length(size, static_cast<size_t>(idx));
    off_t offset = static_cast<off_t>(idx) * static_cast<off_t>(PIECE_SIZE);

    int file_fd = ::open(path.c_str(), O_RDONLY);
    if (file_fd < 0) {
        proto::send_msg(fd, "ERR unavailable");
        ::close(fd);
        return;
    }

    std::vector<char> buf(len);
    bool ok = pread_exact(file_fd, buf.data(), len, offset);
    ::close(file_fd);

    if (!ok) {
        proto::send_msg(fd, "ERR read_failed");
    } else if (proto::send_msg(fd, "OK " + std::to_string(len))) {
        // The payload follows the status message unframed: its length is already
        // known, so a second header would just add a round of copying.
        proto::send_all(fd, buf.data(), len);
    }
    ::close(fd);
}

}  // namespace

size_t piece_count(uint64_t size) {
    return static_cast<size_t>((size + PIECE_SIZE - 1) / PIECE_SIZE);
}

size_t piece_length(uint64_t size, size_t index) {
    uint64_t offset = static_cast<uint64_t>(index) * PIECE_SIZE;
    if (offset >= size) return 0;
    uint64_t remaining = size - offset;
    return remaining < PIECE_SIZE ? static_cast<size_t>(remaining) : PIECE_SIZE;
}

bool scan_file(const std::string &path, FileInfo &out, std::string &error) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error = std::string("cannot open '") + path + "': " + std::strerror(errno);
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "'" + path + "' is not a regular file";
        ::close(fd);
        return false;
    }

    out.path = path;
    out.size = static_cast<uint64_t>(st.st_size);
    out.piece_sha.clear();
    out.piece_sha.reserve(piece_count(out.size));

    sha1::Stream whole;
    std::vector<char> buf(PIECE_SIZE);
    uint64_t total = 0;
    for (;;) {
        size_t got = 0;
        if (!read_exact(fd, buf.data(), PIECE_SIZE, got)) {
            error = std::string("read error on '") + path + "': " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        if (got == 0) break;
        out.piece_sha.push_back(sha1::hex(buf.data(), got));
        whole.update(buf.data(), got);
        total += got;
        if (got < PIECE_SIZE) break;
    }
    ::close(fd);

    if (total != out.size) {
        error = "'" + path + "' changed while it was being read";
        return false;
    }
    out.file_sha = whole.digest();
    return true;
}

void ShareRegistry::add(const std::string &group, const std::string &name, const std::string &path,
                        uint64_t size) {
    std::lock_guard<std::mutex> lock(m_);
    entries_[{group, name}] = Entry{path, size};
}

bool ShareRegistry::remove(const std::string &group, const std::string &name) {
    std::lock_guard<std::mutex> lock(m_);
    return entries_.erase({group, name}) > 0;
}

void ShareRegistry::clear() {
    std::lock_guard<std::mutex> lock(m_);
    entries_.clear();
}

bool ShareRegistry::lookup(const std::string &group, const std::string &name, std::string &path,
                           uint64_t &size) const {
    std::lock_guard<std::mutex> lock(m_);
    auto it = entries_.find({group, name});
    if (it == entries_.end()) return false;
    path = it->second.path;
    size = it->second.size;
    return true;
}

std::vector<std::pair<std::string, std::string>> ShareRegistry::list() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(entries_.size());
    for (const auto &kv : entries_) out.push_back(kv.first);
    return out;
}

void run_peer_server(int listen_fd, ShareRegistry *registry) {
    for (;;) {
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;  // listener closed or unrecoverable error
        }
        proto::set_timeout(fd, kPeerTimeoutSec);
        std::thread(serve_piece_request, fd, registry).detach();
    }
    ::close(listen_fd);
}

bool fetch_piece(const std::string &peer, const std::string &group, const std::string &name,
                 size_t index, size_t expected_len, const std::string &expected_sha,
                 std::vector<char> &out) {
    int fd = proto::dial(peer, kPeerTimeoutSec);
    if (fd < 0) return false;

    std::string request = "GETPIECE " + group + " " + name + " " + std::to_string(index);
    std::string reply;
    if (!proto::send_msg(fd, request) || !proto::recv_msg(fd, reply)) {
        ::close(fd);
        return false;
    }

    std::vector<std::string> a = proto::split_ws(reply);
    if (a.size() != 2 || a[0] != "OK") {
        ::close(fd);
        return false;
    }

    char *end = nullptr;
    long long len = std::strtoll(a[1].c_str(), &end, 10);
    if (!end || *end != '\0' || len < 0 || static_cast<size_t>(len) != expected_len) {
        ::close(fd);  // peer disagrees about the file: treat it as a bad source
        return false;
    }

    out.resize(expected_len);
    bool ok = expected_len == 0 || proto::recv_all(fd, out.data(), expected_len);
    ::close(fd);
    if (!ok) return false;

    // Integrity check happens before the piece is allowed anywhere near the
    // destination file, so a corrupt or malicious peer can never write to it.
    return sha1::hex(out.data(), out.size()) == expected_sha;
}

}  // namespace client
