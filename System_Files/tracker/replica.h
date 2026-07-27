#ifndef P2P_TRACKER_REPLICA_H
#define P2P_TRACKER_REPLICA_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tracker {

// Keeps the other tracker(s) up to date.
//
// Design:
//   * Every state change is turned into a small, *idempotent* op string and
//     appended to one FIFO queue per remote tracker.
//   * A dedicated worker thread per remote tracker drains its queue. An op is
//     only removed once the remote has acknowledged it, so a tracker that is
//     down does not cause updates to be lost - they queue up and are delivered,
//     in order, when it returns. Ordering matters: CREATE_GROUP must not
//     overtake the JOIN_REQUEST that follows it.
//   * Delivery failures back off (1s, doubling, capped at 30s) instead of
//     spinning on a dead endpoint.
//   * On startup - and after the queue has had to be dropped because it grew
//     past its bound - the worker pulls a full snapshot from the remote and
//     merges it. That is the recovery path for a tracker that was offline while
//     the rest of the system moved on.
//
// broadcast() only touches the queue, never the network, so it is safe (and
// intended) to call it while the state mutex is held.
class Replicator {
public:
    using SnapshotHandler = std::function<void(const std::string &)>;

    ~Replicator();

    // endpoints is the full tracker list; the entry at self_index is skipped.
    void start(const std::vector<std::string> &endpoints, int self_index,
               SnapshotHandler on_snapshot);
    void broadcast(const std::string &op);
    void stop();

private:
    struct Remote {
        std::string endpoint;
        std::deque<std::string> queue;
        bool need_snapshot = true;
        std::mutex m;
        std::condition_variable cv;
        std::thread worker;
    };

    void run(Remote *r);
    bool deliver(Remote *r, const std::vector<std::string> &ops);
    bool pull_snapshot(Remote *r);

    std::vector<std::unique_ptr<Remote>> remotes_;
    SnapshotHandler on_snapshot_;
    std::atomic<bool> stopping_{false};
};

}  // namespace tracker

#endif
