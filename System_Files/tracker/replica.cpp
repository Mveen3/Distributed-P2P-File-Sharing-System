#include "replica.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "../common/proto.h"

namespace tracker {
namespace {

// An unreachable peer must not make us grow without bound. Past this many
// pending ops we throw the queue away and fall back to a full snapshot resync,
// which is cheaper than replaying a huge log anyway.
const size_t kMaxQueue = 100000;

// Ops delivered per connection. Batching keeps the common case (a burst of
// changes) down to one TCP handshake.
const size_t kBatch = 64;

const int kDialTimeoutSec = 5;
const int kMinBackoffMs = 1000;
const int kMaxBackoffMs = 30000;

}  // namespace

Replicator::~Replicator() { stop(); }

void Replicator::start(const std::vector<std::string> &endpoints, int self_index,
                       SnapshotHandler on_snapshot) {
    on_snapshot_ = std::move(on_snapshot);

    for (size_t i = 0; i < endpoints.size(); i++) {
        if (static_cast<int>(i) == self_index) continue;
        std::unique_ptr<Remote> r(new Remote());
        r->endpoint = endpoints[i];
        remotes_.push_back(std::move(r));
    }
    for (auto &r : remotes_) {
        Remote *raw = r.get();
        raw->worker = std::thread(&Replicator::run, this, raw);
    }
}

void Replicator::stop() {
    if (stopping_.exchange(true)) return;
    for (auto &r : remotes_) {
        {
            std::lock_guard<std::mutex> lock(r->m);
            r->cv.notify_all();
        }
        if (r->worker.joinable()) r->worker.join();
    }
    remotes_.clear();
}

void Replicator::broadcast(const std::string &op) {
    for (auto &r : remotes_) {
        std::lock_guard<std::mutex> lock(r->m);
        if (r->queue.size() >= kMaxQueue) {
            r->queue.clear();
            r->need_snapshot = true;
            std::fprintf(stderr, "[replica] %s: backlog overflow, will resync by snapshot\n",
                         r->endpoint.c_str());
        }
        r->queue.push_back(op);
        r->cv.notify_one();
    }
}

bool Replicator::deliver(Remote *r, const std::vector<std::string> &ops) {
    int fd = proto::dial(r->endpoint, kDialTimeoutSec);
    if (fd < 0) return false;

    bool ok = true;
    for (const std::string &op : ops) {
        std::string reply;
        if (!proto::send_msg(fd, "SYNC " + op) || !proto::recv_msg(fd, reply) ||
            reply.compare(0, 2, "OK") != 0) {
            ok = false;
            break;
        }
    }
    ::close(fd);
    return ok;
}

bool Replicator::pull_snapshot(Remote *r) {
    int fd = proto::dial(r->endpoint, kDialTimeoutSec);
    if (fd < 0) return false;

    std::string reply;
    bool ok = proto::send_msg(fd, "SNAPSHOT") && proto::recv_msg(fd, reply) &&
              reply.compare(0, 3, "OK\n") == 0;
    ::close(fd);
    if (!ok) return false;

    if (on_snapshot_) on_snapshot_(reply.substr(3));
    std::fprintf(stderr, "[replica] %s: snapshot merged\n", r->endpoint.c_str());
    return true;
}

void Replicator::run(Remote *r) {
    int backoff_ms = kMinBackoffMs;

    while (!stopping_.load()) {
        // Recovery first: a tracker that has just come up (or that dropped its
        // backlog) reconciles with the peer before streaming new ops.
        bool need_snapshot;
        {
            std::lock_guard<std::mutex> lock(r->m);
            need_snapshot = r->need_snapshot;
        }
        if (need_snapshot) {
            if (pull_snapshot(r)) {
                std::lock_guard<std::mutex> lock(r->m);
                r->need_snapshot = false;
                backoff_ms = kMinBackoffMs;
            } else {
                std::unique_lock<std::mutex> lock(r->m);
                r->cv.wait_for(lock, std::chrono::milliseconds(backoff_ms),
                               [&] { return stopping_.load(); });
                backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
                continue;
            }
        }

        // Take a batch without removing it: entries stay queued until the remote
        // has acknowledged them, which is what makes an offline peer lossless.
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(r->m);
            r->cv.wait(lock, [&] { return stopping_.load() || !r->queue.empty(); });
            if (stopping_.load()) return;
            size_t n = std::min(kBatch, r->queue.size());
            batch.assign(r->queue.begin(), r->queue.begin() + static_cast<long>(n));
        }

        if (deliver(r, batch)) {
            std::lock_guard<std::mutex> lock(r->m);
            for (size_t i = 0; i < batch.size() && !r->queue.empty(); i++) r->queue.pop_front();
            backoff_ms = kMinBackoffMs;
        } else {
            std::unique_lock<std::mutex> lock(r->m);
            std::fprintf(stderr, "[replica] %s unreachable, %zu op(s) queued; retrying in %dms\n",
                         r->endpoint.c_str(), r->queue.size(), backoff_ms);
            r->cv.wait_for(lock, std::chrono::milliseconds(backoff_ms),
                           [&] { return stopping_.load(); });
            backoff_ms = std::min(backoff_ms * 2, kMaxBackoffMs);
        }
    }
}

}  // namespace tracker
