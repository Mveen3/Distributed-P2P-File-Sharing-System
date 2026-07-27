#include "session.h"

#include <unistd.h>

#include <cstdio>

#include "../common/proto.h"

namespace client {
namespace {
const int kTrackerTimeoutSec = 10;
}

void TrackerSession::configure(std::vector<std::string> trackers, const std::string &preferred) {
    std::lock_guard<std::mutex> guard(lock_);
    trackers_ = std::move(trackers);
    current_ = preferred.empty() && !trackers_.empty() ? trackers_.front() : preferred;
}

void TrackerSession::set_identity(const std::string &user, const std::string &password) {
    std::lock_guard<std::mutex> guard(lock_);
    user_ = user;
    password_ = password;
}

void TrackerSession::clear_identity() {
    std::lock_guard<std::mutex> guard(lock_);
    user_.clear();
    password_.clear();
}

void TrackerSession::set_peer_token(const std::string &token) {
    std::lock_guard<std::mutex> guard(lock_);
    peer_token_ = token;
}

std::string TrackerSession::user() const {
    std::lock_guard<std::mutex> guard(lock_);
    return user_;
}

std::string TrackerSession::peer_token() const {
    std::lock_guard<std::mutex> guard(lock_);
    return peer_token_;
}

std::string TrackerSession::current_tracker() const {
    std::lock_guard<std::mutex> guard(lock_);
    return current_;
}

bool TrackerSession::send_once(const std::string &endpoint, const std::string &message,
                               std::string &reply) {
    int fd = proto::dial(endpoint, kTrackerTimeoutSec);
    if (fd < 0) return false;
    bool ok = proto::send_msg(fd, message) && proto::recv_msg(fd, reply);
    ::close(fd);
    return ok;
}

bool TrackerSession::relogin(const std::string &endpoint) {
    if (user_.empty()) return false;
    std::string reply;
    if (!send_once(endpoint, "LOGIN " + user_ + " " + password_ + " " + peer_token_, reply))
        return false;
    return reply.compare(0, 2, "OK") == 0;
}

bool TrackerSession::request(const std::string &message, std::string &reply) {
    std::lock_guard<std::mutex> guard(lock_);

    // Current tracker first, then the rest of the list in order.
    std::vector<std::string> candidates;
    if (!current_.empty()) candidates.push_back(current_);
    for (const auto &t : trackers_)
        if (t != current_) candidates.push_back(t);

    for (const std::string &endpoint : candidates) {
        if (!send_once(endpoint, message, reply)) continue;

        // This tracker restarted, or we reached it before the login replicated.
        // Re-authenticate and replay once; if even that fails, move on to the
        // next tracker rather than reporting a spurious error.
        if (reply == "ERR not_logged_in" && !user_.empty()) {
            if (!relogin(endpoint)) continue;
            if (!send_once(endpoint, message, reply)) continue;
        }

        if (endpoint != current_) {
            std::printf("(switched to tracker %s)\n", endpoint.c_str());
            current_ = endpoint;
        }
        return true;
    }
    return false;
}

}  // namespace client
