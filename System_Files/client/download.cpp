#include "download.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

#include "../common/sha1.h"

namespace client {
namespace {

// Parallel piece requests per file. Eight is enough to keep several peers busy
// while bounding both memory (8 * 512 KB) and the file descriptors one download
// can consume.
const size_t kWorkersPerFile = 8;

// Attempts per piece per peer before moving on. One retry absorbs a transient
// hiccup; anything more just delays falling through to a healthier peer.
const int kAttemptsPerPeer = 2;

bool pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    const char *p = static_cast<const char *>(buf);
    size_t written = 0;
    while (written < len) {
        ssize_t w = ::pwrite(fd, p + written, len - written, offset + static_cast<off_t>(written));
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(w);
    }
    return true;
}

std::string key_of(const std::string &group, const std::string &name) { return group + "/" + name; }

}  // namespace

DownloadManager::DownloadManager(CompletionHandler on_complete)
    : on_complete_(std::move(on_complete)) {}

bool DownloadManager::start(const Request &req, std::string &error) {
    if (req.peers.empty()) {
        error = "no peers are sharing this file";
        return false;
    }
    if (req.piece_sha.size() != piece_count(req.size)) {
        error = "tracker metadata is inconsistent (piece count does not match file size)";
        return false;
    }

    std::string key = key_of(req.group, req.name);
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = downloads_.find(key);
        if (it != downloads_.end()) {
            std::lock_guard<std::mutex> dlock(it->second->m);
            if (it->second->status == Download::Running) {
                error = "a download of this file is already in progress";
                return false;
            }
        }
    }

    // Pre-allocate the destination so every piece can be written straight to its
    // final offset; pieces then arrive in any order without any reassembly step.
    int fd = ::open(req.dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = std::string("cannot create '") + req.dest_path + "': " + std::strerror(errno);
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(req.size)) != 0) {
        error = std::string("cannot size '") + req.dest_path + "': " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);

    auto d = std::make_shared<Download>();
    d->group = req.group;
    d->name = req.name;
    d->dest = req.dest_path;
    d->file_sha = req.file_sha;
    d->size = req.size;
    d->piece_sha = req.piece_sha;
    d->peers = req.peers;
    d->npieces = req.piece_sha.size();
    d->have.assign(d->npieces, 0);

    {
        std::lock_guard<std::mutex> lock(m_);
        downloads_[key] = d;
    }

    std::thread(&DownloadManager::run, this, d).detach();
    return true;
}

void DownloadManager::worker(std::shared_ptr<Download> d, int out_fd, size_t worker_id,
                             const std::vector<size_t> *order, std::atomic<size_t> *cursor) {
    std::vector<char> buf;
    const size_t npeers = d->peers.size();

    for (;;) {
        {
            std::lock_guard<std::mutex> lock(d->m);
            if (d->status != Download::Running) return;
        }

        size_t slot = cursor->fetch_add(1);
        if (slot >= order->size()) return;
        size_t index = (*order)[slot];
        size_t len = piece_length(d->size, index);

        bool ok = false;
        std::string last_error;
        for (size_t k = 0; k < npeers && !ok; k++) {
            // Rotating the starting peer per worker and per piece keeps eight
            // concurrent requests from all landing on the same seeder.
            const std::string &peer = d->peers[(worker_id + slot + k) % npeers];
            for (int attempt = 0; attempt < kAttemptsPerPeer && !ok; attempt++) {
                // fetch_piece verifies the SHA1 before returning, so a corrupt
                // piece simply looks like a failed peer and is re-requested from
                // the next one.
                ok = fetch_piece(peer, d->group, d->name, index, len, d->piece_sha[index], buf);
            }
            if (!ok) last_error = "peer " + peer + " could not serve piece";
        }

        if (!ok) {
            std::lock_guard<std::mutex> lock(d->m);
            if (d->status == Download::Running) {
                d->status = Download::Failed;
                d->error = "piece " + std::to_string(index) + " unavailable from any peer";
            }
            return;
        }

        if (!pwrite_all(out_fd, buf.data(), len, static_cast<off_t>(index) * PIECE_SIZE)) {
            std::lock_guard<std::mutex> lock(d->m);
            if (d->status == Download::Running) {
                d->status = Download::Failed;
                d->error = std::string("write failed: ") + std::strerror(errno);
            }
            return;
        }

        std::lock_guard<std::mutex> lock(d->m);
        if (!d->have[index]) {
            d->have[index] = 1;
            d->completed++;
        }
    }
}

void DownloadManager::run(std::shared_ptr<Download> d) {
    // Randomising the piece order spreads concurrent downloaders of the same
    // file across different pieces, which in turn spreads them across seeders.
    std::vector<size_t> order(d->npieces);
    for (size_t i = 0; i < d->npieces; i++) order[i] = i;
    std::mt19937 rng(std::random_device{}());
    std::shuffle(order.begin(), order.end(), rng);

    int out_fd = ::open(d->dest.c_str(), O_WRONLY);
    if (out_fd < 0) {
        std::lock_guard<std::mutex> lock(d->m);
        d->status = Download::Failed;
        d->error = std::string("cannot open destination: ") + std::strerror(errno);
        return;
    }

    std::atomic<size_t> cursor{0};
    size_t nworkers = std::min(kWorkersPerFile, std::max<size_t>(1, d->npieces));
    std::vector<std::thread> workers;
    workers.reserve(nworkers);
    for (size_t i = 0; i < nworkers; i++)
        workers.emplace_back(&DownloadManager::worker, this, d, out_fd, i, &order, &cursor);
    for (auto &t : workers) t.join();

    ::fsync(out_fd);
    ::close(out_fd);

    {
        std::lock_guard<std::mutex> lock(d->m);
        if (d->status == Download::Failed) {
            std::printf("[X] [%s] %s - %s\n", d->group.c_str(), d->name.c_str(), d->error.c_str());
            std::fflush(stdout);
            return;
        }
        if (d->completed != d->npieces) {
            d->status = Download::Failed;
            d->error = "incomplete transfer";
            std::printf("[X] [%s] %s - %s\n", d->group.c_str(), d->name.c_str(), d->error.c_str());
            std::fflush(stdout);
            return;
        }
    }

    // Final gate: every piece already matched its own hash, this confirms the
    // assembled file as a whole is byte-identical to what the uploader shared.
    std::string actual;
    if (!sha1::hex_file(d->dest, actual) || actual != d->file_sha) {
        std::lock_guard<std::mutex> lock(d->m);
        d->status = Download::Failed;
        d->error = "whole-file SHA1 mismatch";
        std::printf("[X] [%s] %s - %s\n", d->group.c_str(), d->name.c_str(), d->error.c_str());
        std::fflush(stdout);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(d->m);
        d->status = Download::Complete;
    }

    // A completed download becomes a new source for everyone else.
    if (on_complete_) on_complete_(d->group, d->name, d->dest, d->size);

    std::printf("[C] [%s] %s\n", d->group.c_str(), d->name.c_str());
    std::fflush(stdout);
}

void DownloadManager::show_status() const {
    std::lock_guard<std::mutex> lock(m_);
    if (downloads_.empty()) {
        std::printf("no downloads yet\n");
        return;
    }
    for (const auto &kv : downloads_) {
        const Download &d = *kv.second;
        std::lock_guard<std::mutex> dlock(d.m);
        switch (d.status) {
            case Download::Complete:
                std::printf("[C] [%s] %s\n", d.group.c_str(), d.name.c_str());
                break;
            case Download::Running:
                std::printf("[D] [%s] %s - %zu/%zu pieces\n", d.group.c_str(), d.name.c_str(),
                            d.completed, d.npieces);
                break;
            case Download::Failed:
                std::printf("[X] [%s] %s - %s\n", d.group.c_str(), d.name.c_str(),
                            d.error.c_str());
                break;
        }
    }
}

bool DownloadManager::has_active() const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto &kv : downloads_) {
        std::lock_guard<std::mutex> dlock(kv.second->m);
        if (kv.second->status == Download::Running) return true;
    }
    return false;
}

}  // namespace client
