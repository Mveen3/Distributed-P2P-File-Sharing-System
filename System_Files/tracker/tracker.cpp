// Tracker server.
//
// Holds the authoritative metadata for the network - users, groups, join
// requests and the piece maps plus seeder lists of every shared file - and
// replicates every change to the other tracker. Clients never exchange file
// data with it; they only ask it who to talk to.
//
// Concurrency model: one thread per accepted connection, one process-wide mutex
// around the state. Handlers take that mutex once, do their (purely in-memory)
// work, queue any replication ops and release it. No network or disk I/O is ever
// performed while another thread could be blocked on the state.

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../common/proto.h"
#include "../common/sha1.h"
#include "replica.h"
#include "state.h"

namespace {

using tracker::FileMeta;
using tracker::Group;
using tracker::State;

State g_state;
std::mutex g_mutex;  // guards g_state
tracker::Replicator g_replicator;
std::string g_data_dir;
std::vector<std::string> g_trackers;
int g_self_index = 0;

std::atomic<int> g_listen_fd{-1};
std::atomic<bool> g_running{true};

const int kBacklog = 64;

// How often the accept loop wakes up on its own to recheck g_running. Bounds
// worst-case shutdown latency without depending on cross-thread close()/shutdown()
// actually waking a thread parked in accept() - a guarantee that does not hold in
// every environment this may run under.
const int kAcceptPollMs = 200;

// Same idea for the console: it waits for stdin to become readable in bounded
// slices instead of blocking indefinitely inside getline().
const int kConsolePollMs = 200;

// ---------------------------------------------------------------------------
// Small parsing helpers. The protocol is text, and text from the network is
// never trusted: every conversion below reports failure instead of throwing,
// which is what a std::stoi() on a malformed field would do - taking the whole
// tracker down with it.
// ---------------------------------------------------------------------------

bool parse_u64(const std::string &s, uint64_t &out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

// A client identifies its seeding endpoint either fully ("ip:port") or by port
// alone, in which case the address observed on this connection is used. The
// second form is what lets a client work without knowing its own routable IP.
bool resolve_peer(int fd, const std::string &token, std::string &out) {
    if (token.find(':') != std::string::npos) {
        std::string host;
        uint16_t port = 0;
        if (!proto::parse_endpoint(token, host, port)) return false;
        out = token;
        return true;
    }
    uint16_t port = 0;
    if (!proto::parse_port(token, port)) return false;
    std::string ip = proto::peer_address(fd);
    if (ip.empty()) return false;
    out = proto::make_endpoint(ip, port);
    return true;
}

void persist_locked() { tracker::save(g_state, g_data_dir); }

// ---------------------------------------------------------------------------
// Replication ops.
//
// Each op is an absolute, idempotent statement about the desired state ("user X
// has password P", "group G contains member U") rather than a delta. Applying
// one twice, or applying it to a tracker that already agrees, is a no-op, so a
// redelivered or snapshot-overlapping op can never corrupt the replica.
// ---------------------------------------------------------------------------

void apply_op_locked(const std::string &op) {
    std::istringstream iss(op);
    std::string kind;
    if (!(iss >> kind)) return;

    if (kind == "USER") {
        std::string user, pass;
        if (iss >> user >> pass) g_state.users[user].password = pass;

    } else if (kind == "ONLINE") {
        std::string user, flag;
        if (iss >> user >> flag) {
            auto it = g_state.users.find(user);
            if (it != g_state.users.end()) it->second.online = (flag == "1");
        }

    } else if (kind == "GROUP") {
        std::string group, owner;
        if (iss >> group >> owner) {
            Group &g = g_state.groups[group];
            if (g.owner.empty()) g.owner = owner;
            g.members.insert(owner);
        }

    } else if (kind == "MEMBER") {
        std::string group, user;
        if (iss >> group >> user) {
            auto it = g_state.groups.find(group);
            if (it != g_state.groups.end()) {
                it->second.members.insert(user);
                auto &p = it->second.pending;
                p.erase(std::remove(p.begin(), p.end(), user), p.end());
            }
        }

    } else if (kind == "REQUEST" || kind == "UNREQUEST") {
        std::string group, user;
        if (iss >> group >> user) {
            auto it = g_state.groups.find(group);
            if (it != g_state.groups.end()) {
                auto &p = it->second.pending;
                p.erase(std::remove(p.begin(), p.end(), user), p.end());
                if (kind == "REQUEST" && !it->second.members.count(user)) p.push_back(user);
            }
        }

    } else if (kind == "LEAVE") {
        std::string group, user;
        if (iss >> group >> user) {
            auto it = g_state.groups.find(group);
            if (it == g_state.groups.end()) return;
            it->second.members.erase(user);
            auto &p = it->second.pending;
            p.erase(std::remove(p.begin(), p.end(), user), p.end());

            // A member who leaves stops sharing whatever they own in the group.
            for (auto f = g_state.files.begin(); f != g_state.files.end();) {
                if (f->second.group == group && f->second.owner == user)
                    f = g_state.files.erase(f);
                else
                    ++f;
            }
            if (it->second.owner == user) {
                if (it->second.members.empty()) {
                    g_state.groups.erase(it);
                } else {
                    // members is ordered, so both trackers elect the same
                    // successor without having to exchange anything.
                    it->second.owner = *it->second.members.begin();
                }
            }
        }

    } else if (kind == "DELGROUP") {
        std::string group;
        if (iss >> group) g_state.groups.erase(group);

    } else if (kind == "FILE") {
        FileMeta f;
        std::string np_tok, size_tok;
        uint64_t np = 0;
        if (!(iss >> f.group >> f.name >> size_tok >> np_tok >> f.file_sha >> f.owner)) return;
        if (!parse_u64(size_tok, f.size) || !parse_u64(np_tok, np)) return;
        if (!sha1::is_hex(f.file_sha)) return;
        std::string h;
        while (iss >> h && f.piece_sha.size() < np) {
            if (!sha1::is_hex(h)) return;
            f.piece_sha.push_back(h);
        }
        if (f.piece_sha.size() != np) return;

        std::string key = tracker::file_key(f.group, f.name);
        auto it = g_state.files.find(key);
        if (it == g_state.files.end()) {
            g_state.files[key] = f;
        } else {
            // Metadata is replaced, the seeder set is not: peers are local
            // knowledge each tracker accumulates from PEER/UNPEER ops.
            f.peers = it->second.peers;
            it->second = f;
        }

    } else if (kind == "PEER" || kind == "UNPEER") {
        std::string group, name, peer;
        if (iss >> group >> name >> peer) {
            FileMeta *f = g_state.find_file(group, name);
            if (!f) return;
            if (kind == "PEER") {
                f->peers.insert(peer);
            } else {
                f->peers.erase(peer);
                if (f->peers.empty()) g_state.files.erase(tracker::file_key(group, name));
            }
        }

    } else if (kind == "DROPPEER") {
        std::string peer;
        if (iss >> peer) g_state.drop_peer(peer);
    }
}

// Applies an op locally and forwards it. Received SYNC ops are applied with
// apply_op_locked() directly instead, which is what keeps them from echoing back
// and forth between the two trackers forever.
void commit_locked(const std::string &op) {
    apply_op_locked(op);
    g_replicator.broadcast(op);
}

// ---------------------------------------------------------------------------
// Client request handling. Every reply starts with a status line: "OK",
// optionally followed by payload lines, or "ERR <code>".
// ---------------------------------------------------------------------------

std::string err(const char *code) { return std::string("ERR ") + code; }

// Session check. There are no session tokens: a client asserts its identity and
// the tracker verifies that the account exists and is currently logged in. See
// the README's security note.
bool authorised(const std::string &user) {
    auto it = g_state.users.find(user);
    return it != g_state.users.end() && it->second.online;
}

std::string handle_register(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.users.count(a[1])) return err("user_exists");
    commit_locked("USER " + a[1] + " " + a[2]);
    persist_locked();
    return "OK";
}

std::string handle_login(int fd, const std::vector<std::string> &a) {
    if (a.size() != 4) return err("bad_request");
    std::string peer;
    if (!resolve_peer(fd, a[3], peer)) return err("bad_request");

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_state.users.find(a[1]);
    if (it == g_state.users.end()) return err("user_not_found");
    if (it->second.password != a[2]) return err("wrong_password");
    commit_locked("ONLINE " + a[1] + " 1");
    return "OK " + peer;
}

std::string handle_logout(int fd, const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::string peer;
    if (!resolve_peer(fd, a[2], peer)) return err("bad_request");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.users.count(a[1])) return err("user_not_found");
    // Logging out means "stop sharing": the endpoint disappears from every file
    // it was seeding, so no client is ever handed a peer that has gone away.
    commit_locked("DROPPEER " + peer);
    commit_locked("ONLINE " + a[1] + " 0");
    persist_locked();
    return "OK";
}

std::string handle_create_group(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    if (g_state.groups.count(a[2])) return err("group_exists");
    commit_locked("GROUP " + a[2] + " " + a[1]);
    persist_locked();
    return "OK";
}

std::string handle_join_group(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    auto it = g_state.groups.find(a[2]);
    if (it == g_state.groups.end()) return err("no_such_group");
    if (it->second.members.count(a[1])) return err("already_member");
    for (const auto &p : it->second.pending)
        if (p == a[1]) return err("already_requested");
    commit_locked("REQUEST " + a[2] + " " + a[1]);
    persist_locked();
    return "OK";
}

std::string handle_leave_group(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    if (!g_state.is_member(a[1], a[2])) return err("not_member");
    commit_locked("LEAVE " + a[2] + " " + a[1]);
    persist_locked();
    return "OK";
}

std::string handle_list_groups(const std::vector<std::string> &a) {
    if (a.size() != 2) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    std::string out = "OK";
    for (const auto &kv : g_state.groups)
        out += "\n" + kv.first + " " + kv.second.owner + " " +
               std::to_string(kv.second.members.size());
    return out;
}

std::string handle_list_requests(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    auto it = g_state.groups.find(a[2]);
    if (it == g_state.groups.end()) return err("no_such_group");
    if (it->second.owner != a[1]) return err("not_owner");
    std::string out = "OK";
    for (const auto &u : it->second.pending) out += "\n" + u;
    return out;
}

std::string handle_accept_request(const std::vector<std::string> &a) {
    if (a.size() != 4) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    auto it = g_state.groups.find(a[2]);
    if (it == g_state.groups.end()) return err("no_such_group");
    if (it->second.owner != a[1]) return err("not_owner");

    bool pending = false;
    for (const auto &u : it->second.pending) pending = pending || u == a[3];
    if (!pending) return err("no_request");

    commit_locked("MEMBER " + a[2] + " " + a[3]);
    persist_locked();
    return "OK";
}

std::string handle_upload_file(int fd, const std::vector<std::string> &a) {
    // UPLOAD_FILE <user> <peer> <group> <name> <size> <npieces> <file_sha> <hash>...
    if (a.size() < 8) return err("bad_request");

    std::string peer;
    if (!resolve_peer(fd, a[2], peer)) return err("bad_request");

    FileMeta f;
    f.group = a[3];
    f.name = a[4];
    f.owner = a[1];
    f.file_sha = a[7];
    uint64_t npieces = 0;
    if (!parse_u64(a[5], f.size) || !parse_u64(a[6], npieces)) return err("bad_request");
    if (!sha1::is_hex(f.file_sha)) return err("bad_request");
    if (a.size() != 8 + npieces) return err("piece_count_mismatch");
    for (size_t i = 8; i < a.size(); i++) {
        if (!sha1::is_hex(a[i])) return err("bad_request");
        f.piece_sha.push_back(a[i]);
    }
    f.peers.insert(peer);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(f.owner)) return err("not_logged_in");
    if (!g_state.is_member(f.owner, f.group)) return err("not_member");

    const FileMeta *existing = g_state.find_file(f.group, f.name);
    if (existing && existing->file_sha != f.file_sha) return err("file_exists");

    std::string op = "FILE " + f.group + " " + f.name + " " + std::to_string(f.size) + " " +
                     std::to_string(f.piece_sha.size()) + " " + f.file_sha + " " + f.owner;
    for (const auto &h : f.piece_sha) op += " " + h;
    commit_locked(op);
    commit_locked("PEER " + f.group + " " + f.name + " " + peer);
    persist_locked();
    return "OK";
}

std::string handle_list_files(const std::vector<std::string> &a) {
    if (a.size() != 3) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    if (!g_state.groups.count(a[2])) return err("no_such_group");
    if (!g_state.is_member(a[1], a[2])) return err("not_member");

    std::string out = "OK";
    for (const auto &kv : g_state.files) {
        const FileMeta &f = kv.second;
        if (f.group != a[2]) continue;
        out += "\n" + f.name + " " + std::to_string(f.size) + " " + std::to_string(f.peers.size());
    }
    return out;
}

std::string handle_get_file(const std::vector<std::string> &a) {
    if (a.size() != 4) return err("bad_request");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    if (!g_state.is_member(a[1], a[2])) return err("not_member");

    const FileMeta *f = g_state.find_file(a[2], a[3]);
    if (!f) return err("no_such_file");
    if (f->peers.empty()) return err("no_peers");

    // OK
    // <size> <npieces> <file_sha>
    // <piece hash> * npieces, one per line
    // <peer endpoint> * n, one per line
    std::string out = "OK\n" + std::to_string(f->size) + " " +
                      std::to_string(f->piece_sha.size()) + " " + f->file_sha + "\n";
    for (const auto &h : f->piece_sha) out += h + "\n";
    out += "PEERS\n";
    for (const auto &p : f->peers) out += p + "\n";
    return out;
}

std::string handle_add_peer(int fd, const std::vector<std::string> &a) {
    if (a.size() != 5) return err("bad_request");
    std::string peer;
    if (!resolve_peer(fd, a[2], peer)) return err("bad_request");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    if (!g_state.is_member(a[1], a[3])) return err("not_member");
    if (!g_state.find_file(a[3], a[4])) return err("no_such_file");
    commit_locked("PEER " + a[3] + " " + a[4] + " " + peer);
    persist_locked();
    return "OK";
}

std::string handle_stop_share(int fd, const std::vector<std::string> &a) {
    if (a.size() != 5) return err("bad_request");
    std::string peer;
    if (!resolve_peer(fd, a[2], peer)) return err("bad_request");

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!authorised(a[1])) return err("not_logged_in");
    const FileMeta *f = g_state.find_file(a[3], a[4]);
    if (!f) return err("no_such_file");
    if (!f->peers.count(peer)) return err("not_sharing");
    commit_locked("UNPEER " + a[3] + " " + a[4] + " " + peer);
    persist_locked();
    return "OK";
}

std::string handle_sync(const std::string &request) {
    // "SYNC " prefix stripped by the caller.
    std::lock_guard<std::mutex> lock(g_mutex);
    apply_op_locked(request);
    persist_locked();
    return "OK";
}

std::string handle_snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return "OK\n" + tracker::encode(g_state);
}

std::string dispatch(int fd, const std::string &request) {
    std::vector<std::string> a = proto::split_ws(request);
    if (a.empty()) return err("bad_request");
    const std::string &cmd = a[0];

    if (cmd == "REGISTER") return handle_register(a);
    if (cmd == "LOGIN") return handle_login(fd, a);
    if (cmd == "LOGOUT") return handle_logout(fd, a);
    if (cmd == "CREATE_GROUP") return handle_create_group(a);
    if (cmd == "JOIN_GROUP") return handle_join_group(a);
    if (cmd == "LEAVE_GROUP") return handle_leave_group(a);
    if (cmd == "LIST_GROUPS") return handle_list_groups(a);
    if (cmd == "LIST_REQUESTS") return handle_list_requests(a);
    if (cmd == "ACCEPT_REQUEST") return handle_accept_request(a);
    if (cmd == "UPLOAD_FILE") return handle_upload_file(fd, a);
    if (cmd == "LIST_FILES") return handle_list_files(a);
    if (cmd == "GET_FILE") return handle_get_file(a);
    if (cmd == "ADD_PEER") return handle_add_peer(fd, a);
    if (cmd == "STOP_SHARE") return handle_stop_share(fd, a);
    if (cmd == "SNAPSHOT") return handle_snapshot();
    if (cmd == "SYNC") {
        size_t sp = request.find(' ');
        if (sp == std::string::npos) return err("bad_request");
        return handle_sync(request.substr(sp + 1));
    }
    return err("unknown_command");
}

// One connection, many requests: clients keep the socket open for the duration
// of a command and trackers reuse it for a whole batch of sync ops.
void serve_connection(int fd) {
    std::string request;
    while (proto::recv_msg(fd, request)) {
        std::string reply;
        try {
            reply = dispatch(fd, request);
        } catch (const std::exception &e) {
            // A failure while serving one request must not take down the
            // tracker; report it and keep the other connections alive.
            std::fprintf(stderr, "[tracker] request failed: %s\n", e.what());
            reply = err("internal_error");
        }
        if (!proto::send_msg(fd, reply)) break;
    }
    ::close(fd);
}

// Signals g_running false and closes the listening socket exactly once. Closing
// (not shutdown()) is what actually matters here: shutdown() only has a defined
// effect on a *connected* socket, so calling it on a listening fd does not wake a
// different thread blocked in accept() on it - the tracker would then sit there
// until a real connection happened to arrive. close() from another thread does
// reliably unblock a concurrent accept() with EBADF on Linux, which is what both
// the console's 'quit' and a caught signal need. The exchange() makes this safe
// to call from both places (and from main's own cleanup) without double-closing.
void request_shutdown() {
    g_running.store(false);
    int fd = g_listen_fd.exchange(-1);
    if (fd >= 0) ::close(fd);
}

void console_thread() {
    std::string line;
    while (g_running.load()) {
        // Wait for stdin to become readable rather than parking inside getline().
        // Two reasons: a shutdown requested elsewhere (a signal) is noticed within
        // one poll interval, and - more importantly - the thread is not sitting in
        // a blocking read at process-exit time. exit() walks every open FILE stream
        // to flush it and takes each one's lock; a reader blocked on stdin holds
        // stdin's lock for the whole read, so exit() would deadlock there until the
        // read happened to complete (i.e. until the user pressed Ctrl+D).
        pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, kConsolePollMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (pr == 0) continue;  // nothing typed yet; recheck g_running

        // Readable also covers EOF, where getline() fails immediately: no console
        // is attached (stdin redirected or closed). That is not a reason to stop
        // serving; only an explicit 'quit' or a signal shuts the tracker down.
        if (!std::getline(std::cin, line)) return;

        std::vector<std::string> a = proto::split_ws(line);
        if (a.empty()) continue;

        if (a[0] == "quit") {
            break;
        } else if (a[0] == "save") {
            std::lock_guard<std::mutex> lock(g_mutex);
            persist_locked();
            std::cout << "state written to " << g_data_dir << "/state.txt" << std::endl;
        } else if (a[0] == "status") {
            std::lock_guard<std::mutex> lock(g_mutex);
            size_t online = 0;
            for (const auto &kv : g_state.users) online += kv.second.online ? 1 : 0;
            std::cout << "users=" << g_state.users.size() << " (online " << online << ")"
                      << " groups=" << g_state.groups.size() << " files=" << g_state.files.size()
                      << std::endl;
        } else if (a[0] == "files") {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &kv : g_state.files)
                std::cout << kv.first << " size=" << kv.second.size
                          << " pieces=" << kv.second.piece_sha.size()
                          << " seeders=" << kv.second.peers.size() << std::endl;
        } else if (a[0] == "groups") {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &kv : g_state.groups)
                std::cout << kv.first << " owner=" << kv.second.owner
                          << " members=" << kv.second.members.size()
                          << " pending=" << kv.second.pending.size() << std::endl;
        } else if (a[0] == "help") {
            std::cout << "commands: status | groups | files | save | quit" << std::endl;
        } else {
            std::cout << "unknown command (try 'help')" << std::endl;
        }
    }

    request_shutdown();
}

void on_signal(int) {
    // close() is on the POSIX async-signal-safe list, so this is safe to run
    // directly from a signal handler.
    request_shutdown();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <tracker_info.txt> <tracker_no>\n", argv[0]);
        return 1;
    }

    proto::ignore_sigpipe();

    // Read the tracker directory first: the index has to be validated before it
    // is used to pick a data directory.
    {
        FILE *f = std::fopen(argv[1], "r");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) g_trackers.push_back(line);
        }
        std::fclose(f);
    }

    char *end = nullptr;
    long idx = std::strtol(argv[2], &end, 10);
    if (!end || *end != '\0' || idx < 0 || idx >= static_cast<long>(g_trackers.size())) {
        std::fprintf(stderr, "tracker_no must be between 0 and %zu\n",
                     g_trackers.empty() ? 0 : g_trackers.size() - 1);
        return 1;
    }
    g_self_index = static_cast<int>(idx);

    std::string host;
    uint16_t port = 0;
    if (!proto::parse_endpoint(g_trackers[g_self_index], host, port)) {
        std::fprintf(stderr, "malformed tracker endpoint: %s\n",
                     g_trackers[g_self_index].c_str());
        return 1;
    }

    g_data_dir = "tracker_data_" + std::to_string(g_self_index);
    if (tracker::load(g_state, g_data_dir))
        std::printf("restored %zu user(s), %zu group(s), %zu file(s) from %s\n",
                    g_state.users.size(), g_state.groups.size(), g_state.files.size(),
                    g_data_dir.c_str());

    int lfd = proto::listen_on(host, port, kBacklog);
    if (lfd < 0) return 1;
    g_listen_fd.store(lfd);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Merging a peer's snapshot is the recovery path for a tracker that was
    // offline; it must not overwrite anything we already know, hence merge_only.
    g_replicator.start(g_trackers, g_self_index, [](const std::string &snapshot) {
        std::lock_guard<std::mutex> lock(g_mutex);
        tracker::decode_into(g_state, snapshot, /*merge_only=*/true);
        persist_locked();
    });

    std::thread console(console_thread);

    std::printf("tracker %d listening on %s (type 'help' for console commands)\n", g_self_index,
                g_trackers[g_self_index].c_str());
    std::fflush(stdout);

    while (g_running.load()) {
        pollfd pfd;
        pfd.fd = lfd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, kAcceptPollMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;  // genuine error on the listening socket: stop accepting
        }
        if (pr == 0) continue;  // timed out; loop back and recheck g_running

        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        proto::set_timeout(cfd, 60);
        std::thread(serve_connection, cfd).detach();
    }

    request_shutdown();  // idempotent: no-op if a signal or 'quit' already closed lfd

    g_replicator.stop();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        persist_locked();
    }
    std::printf("tracker %d stopped\n", g_self_index);
    std::fflush(stdout);
    std::fflush(stderr);

    // Terminate directly instead of returning, which would call exit().
    //
    // exit() flushes and locks every open FILE stream and runs the destructors of
    // globals such as g_replicator and g_state. Detached threads are still alive
    // at this point - connection handlers parked in recv(), and the console thread
    // somewhere around stdin - so both of those steps can block on a lock a
    // detached thread happens to hold, or race a global being destroyed out from
    // under it. Nothing here needs that cleanup: the state file has already been
    // written, and it is updated via rename() so it is never left half-written no
    // matter when the process dies.
    console.detach();
    std::_Exit(0);
}
