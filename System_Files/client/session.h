#ifndef P2P_CLIENT_SESSION_H
#define P2P_CLIENT_SESSION_H

#include <mutex>
#include <string>
#include <vector>

namespace client {

// The client's link to the tracker network.
//
// A request is sent to the tracker we are currently bound to; if that tracker is
// unreachable the session walks the rest of the list and sticks to whichever one
// answers. Because the trackers replicate everything, any of them can serve any
// request.
//
// It also repairs the session transparently. A tracker that restarted, or one we
// have just failed over to before the login replicated, will answer
// "ERR not_logged_in"; the session re-authenticates with the credentials from
// this session and replays the request once. Callers never see it.
class TrackerSession {
public:
    void configure(std::vector<std::string> trackers, const std::string &preferred);

    void set_identity(const std::string &user, const std::string &password);
    void clear_identity();
    void set_peer_token(const std::string &token);

    std::string user() const;
    std::string peer_token() const;
    std::string current_tracker() const;

    // false means no tracker could be reached at all. A tracker that answered
    // with "ERR ..." is still a success at this level; reply holds its answer.
    bool request(const std::string &message, std::string &reply);

private:
    bool send_once(const std::string &endpoint, const std::string &message, std::string &reply);
    bool relogin(const std::string &endpoint);  // requires lock_ held

    mutable std::mutex lock_;
    std::vector<std::string> trackers_;
    std::string current_;
    std::string user_;
    std::string password_;
    std::string peer_token_;
};

}  // namespace client

#endif
