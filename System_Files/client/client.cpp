// Client application.
//
// Every client is both a downloader and a seeder: it runs a peer server that
// serves 512 KB pieces of the files it shares, and a download manager that pulls
// pieces of other files from several peers at once. The tracker is only ever
// consulted for metadata - accounts, groups, piece hashes and who to ask.
//
// Usage: ./client <IP>:<PORT> tracker_info.txt
//
// <IP>:<PORT> is the address this client listens on for peer traffic. If the
// address given happens to be one of the tracker endpoints from
// tracker_info.txt, it is instead taken as "connect to this tracker first" and a
// free port is chosen automatically - both documented invocations work.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/proto.h"
#include "download.h"
#include "session.h"
#include "share.h"

namespace {

using client::DownloadManager;
using client::FileInfo;
using client::ShareRegistry;
using client::TrackerSession;

TrackerSession g_session;
ShareRegistry g_shares;

// ---------------------------------------------------------------------------
// Reply helpers. Tracker replies are a status line ("OK" or "ERR <code>")
// followed by zero or more payload lines.
// ---------------------------------------------------------------------------

std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool reply_ok(const std::string &reply) { return reply.compare(0, 2, "OK") == 0; }

void print_error(const std::string &reply) {
    std::vector<std::string> lines = split_lines(reply);
    std::printf("error: %s\n", lines.empty() ? "empty reply" : lines[0].c_str());
}

// Sends a request and reports the outcome. Returns false if the command failed
// for any reason (unreachable trackers or a tracker-side error).
bool call(const std::string &message, std::string &reply) {
    if (!g_session.request(message, reply)) {
        std::printf("error: no tracker is reachable\n");
        return false;
    }
    if (!reply_ok(reply)) {
        print_error(reply);
        return false;
    }
    return true;
}

bool require_login() {
    if (g_session.user().empty()) {
        std::printf("error: login required\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void cmd_create_user(const std::vector<std::string> &a) {
    std::string reply;
    if (call("REGISTER " + a[1] + " " + a[2], reply)) std::printf("user '%s' created\n", a[1].c_str());
}

void cmd_login(const std::vector<std::string> &a) {
    if (!g_session.user().empty()) {
        std::printf("error: already logged in as '%s'\n", g_session.user().c_str());
        return;
    }
    std::string reply;
    if (!g_session.request("LOGIN " + a[1] + " " + a[2] + " " + g_session.peer_token(), reply)) {
        std::printf("error: no tracker is reachable\n");
        return;
    }
    if (!reply_ok(reply)) {
        print_error(reply);
        return;
    }
    g_session.set_identity(a[1], a[2]);

    // The tracker echoes the endpoint it will advertise for us, which is the
    // address other peers will dial.
    std::vector<std::string> parts = proto::split_ws(reply);
    std::printf("logged in as '%s', seeding from %s\n", a[1].c_str(),
                parts.size() > 1 ? parts[1].c_str() : "this host");
}

void cmd_logout() {
    if (!require_login()) return;
    std::string reply;
    // Logging out stops sharing: the tracker drops this endpoint from every file
    // it was seeding so no one is handed a dead peer.
    g_session.request("LOGOUT " + g_session.user() + " " + g_session.peer_token(), reply);
    g_session.clear_identity();
    g_shares.clear();
    std::printf("logged out\n");
}

void cmd_create_group(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (call("CREATE_GROUP " + g_session.user() + " " + a[1], reply))
        std::printf("group '%s' created\n", a[1].c_str());
}

void cmd_join_group(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (call("JOIN_GROUP " + g_session.user() + " " + a[1], reply))
        std::printf("join request sent to the owner of '%s'\n", a[1].c_str());
}

void cmd_leave_group(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (call("LEAVE_GROUP " + g_session.user() + " " + a[1], reply)) {
        // Anything shared into that group is no longer ours to serve.
        for (const auto &entry : g_shares.list())
            if (entry.first == a[1]) g_shares.remove(entry.first, entry.second);
        std::printf("left group '%s'\n", a[1].c_str());
    }
}

void cmd_list_groups() {
    if (!require_login()) return;
    std::string reply;
    if (!call("LIST_GROUPS " + g_session.user(), reply)) return;
    std::vector<std::string> lines = split_lines(reply);
    if (lines.size() <= 1) {
        std::printf("no groups exist yet\n");
        return;
    }
    std::printf("%-24s %-16s %s\n", "GROUP", "OWNER", "MEMBERS");
    for (size_t i = 1; i < lines.size(); i++) {
        std::vector<std::string> f = proto::split_ws(lines[i]);
        if (f.size() == 3) std::printf("%-24s %-16s %s\n", f[0].c_str(), f[1].c_str(), f[2].c_str());
    }
}

void cmd_list_requests(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (!call("LIST_REQUESTS " + g_session.user() + " " + a[1], reply)) return;
    std::vector<std::string> lines = split_lines(reply);
    if (lines.size() <= 1) {
        std::printf("no pending requests for '%s'\n", a[1].c_str());
        return;
    }
    for (size_t i = 1; i < lines.size(); i++) std::printf("%s\n", lines[i].c_str());
}

void cmd_accept_request(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (call("ACCEPT_REQUEST " + g_session.user() + " " + a[1] + " " + a[2], reply))
        std::printf("'%s' added to group '%s'\n", a[2].c_str(), a[1].c_str());
}

void cmd_upload_file(const std::vector<std::string> &a) {
    if (!require_login()) return;
    const std::string &group = a[1];
    const std::string &path = a[2];

    FileInfo info;
    std::string error;
    if (!client::scan_file(path, info, error)) {
        std::printf("error: %s\n", error.c_str());
        return;
    }

    // Basename: the group sees the file by name, not by the uploader's path.
    size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
        std::printf("error: '%s' has no file name\n", path.c_str());
        return;
    }

    std::string message = "UPLOAD_FILE " + g_session.user() + " " + g_session.peer_token() + " " +
                          group + " " + name + " " + std::to_string(info.size) + " " +
                          std::to_string(info.piece_sha.size()) + " " + info.file_sha;
    for (const auto &h : info.piece_sha) message += " " + h;

    // Register locally first so a peer that hears about the file from the
    // tracker can never arrive before we are ready to serve it.
    g_shares.add(group, name, path, info.size);

    std::string reply;
    if (!call(message, reply)) {
        g_shares.remove(group, name);
        return;
    }
    std::printf("sharing '%s' in '%s' (%llu bytes, %zu piece(s))\n", name.c_str(), group.c_str(),
                static_cast<unsigned long long>(info.size), info.piece_sha.size());
}

void cmd_list_files(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (!call("LIST_FILES " + g_session.user() + " " + a[1], reply)) return;
    std::vector<std::string> lines = split_lines(reply);
    if (lines.size() <= 1) {
        std::printf("no files shared in '%s'\n", a[1].c_str());
        return;
    }
    std::printf("%-32s %14s %s\n", "FILE", "SIZE", "SEEDERS");
    for (size_t i = 1; i < lines.size(); i++) {
        std::vector<std::string> f = proto::split_ws(lines[i]);
        if (f.size() == 3)
            std::printf("%-32s %14s %s\n", f[0].c_str(), f[1].c_str(), f[2].c_str());
    }
}

void cmd_download_file(DownloadManager &downloads, const std::vector<std::string> &a) {
    if (!require_login()) return;
    const std::string &group = a[1];
    const std::string &name = a[2];
    const std::string &dest = a[3];

    std::string reply;
    if (!call("GET_FILE " + g_session.user() + " " + group + " " + name, reply)) return;

    // OK / "<size> <npieces> <file_sha>" / piece hashes / PEERS / peer endpoints
    std::vector<std::string> lines = split_lines(reply);
    if (lines.size() < 2) {
        std::printf("error: malformed tracker reply\n");
        return;
    }

    std::vector<std::string> header = proto::split_ws(lines[1]);
    if (header.size() != 3) {
        std::printf("error: malformed tracker reply\n");
        return;
    }

    DownloadManager::Request req;
    req.group = group;
    req.name = name;
    req.size = std::strtoull(header[0].c_str(), nullptr, 10);
    size_t npieces = static_cast<size_t>(std::strtoull(header[1].c_str(), nullptr, 10));
    req.file_sha = header[2];

    size_t i = 2;
    for (; i < lines.size() && req.piece_sha.size() < npieces; i++) req.piece_sha.push_back(lines[i]);
    if (i >= lines.size() || lines[i] != "PEERS") {
        std::printf("error: malformed tracker reply\n");
        return;
    }
    for (i++; i < lines.size(); i++)
        if (!lines[i].empty()) req.peers.push_back(lines[i]);

    // A directory destination means "put the file in here under its own name".
    struct stat st;
    req.dest_path = dest;
    if (::stat(dest.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        std::string dir = dest;
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        req.dest_path = dir + "/" + name;
    }

    std::string error;
    if (!downloads.start(req, error)) {
        std::printf("error: %s\n", error.c_str());
        return;
    }
    std::printf("downloading '%s' from %zu peer(s) into %s\n", name.c_str(), req.peers.size(),
                req.dest_path.c_str());
}

void cmd_stop_share(const std::vector<std::string> &a) {
    if (!require_login()) return;
    std::string reply;
    if (call("STOP_SHARE " + g_session.user() + " " + g_session.peer_token() + " " + a[1] + " " +
                 a[2],
             reply)) {
        g_shares.remove(a[1], a[2]);
        std::printf("stopped sharing '%s' in '%s'\n", a[2].c_str(), a[1].c_str());
    }
}

void print_help() {
    std::printf(
        "user     create_user <user> <password> | login <user> <password> | logout\n"
        "group    create_group <group> | join_group <group> | leave_group <group>\n"
        "         list_groups | list_requests <group> | accept_request <group> <user>\n"
        "file     upload_file <group> <file_path> | list_files <group>\n"
        "         download_file <group> <file_name> <destination_path>\n"
        "         show_downloads | stop_share <group> <file_name>\n"
        "session  help | quit\n");
}

// True when the command has exactly the expected number of arguments.
bool arity(const std::vector<std::string> &a, size_t expected, const char *usage) {
    if (a.size() == expected) return true;
    std::printf("usage: %s\n", usage);
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <IP>:<PORT> <tracker_info.txt>\n", argv[0]);
        return 1;
    }

    proto::ignore_sigpipe();

    std::vector<std::string> trackers;
    {
        FILE *f = std::fopen(argv[2], "r");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", argv[2]);
            return 1;
        }
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) trackers.push_back(line);
        }
        std::fclose(f);
    }
    if (trackers.empty()) {
        std::fprintf(stderr, "%s lists no trackers\n", argv[2]);
        return 1;
    }

    std::string self = argv[1];
    std::string host;
    uint16_t port = 0;
    if (!proto::parse_endpoint(self, host, port)) {
        std::fprintf(stderr, "'%s' is not a valid <IP>:<PORT>\n", argv[1]);
        return 1;
    }

    // If the address given is a tracker's, it names the tracker to prefer rather
    // than an address to listen on, and the kernel picks our peer port.
    std::string preferred_tracker = trackers.front();
    bool self_is_tracker = false;
    for (const auto &t : trackers) self_is_tracker = self_is_tracker || t == self;
    if (self_is_tracker) {
        preferred_tracker = self;
        host = "0.0.0.0";
        port = 0;
    }

    int listen_fd = proto::listen_on(host, port, 64);
    if (listen_fd < 0) {
        std::fprintf(stderr, "cannot listen on %s\n", self.c_str());
        return 1;
    }
    uint16_t bound_port = proto::local_port(listen_fd);

    g_session.configure(trackers, preferred_tracker);
    // When the client was told its own address, advertise it verbatim; otherwise
    // send only the port and let the tracker fill in the address it sees us on.
    g_session.set_peer_token(self_is_tracker ? std::to_string(bound_port)
                                             : proto::make_endpoint(host, bound_port));

    std::thread(client::run_peer_server, listen_fd, &g_shares).detach();

    DownloadManager downloads([](const std::string &group, const std::string &name,
                                 const std::string &path, uint64_t size) {
        // A verified download turns this client into another seeder for the file.
        g_shares.add(group, name, path, size);
        std::string user = g_session.user();
        if (user.empty()) return;  // logged out while the transfer was running
        std::string reply;
        g_session.request(
            "ADD_PEER " + user + " " + g_session.peer_token() + " " + group + " " + name, reply);
    });

    std::printf("peer server listening on port %u; tracker %s\n", bound_port,
                preferred_tracker.c_str());
    std::printf("type 'help' for the command list\n");

    std::string line;
    for (;;) {
        std::printf("> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) break;

        std::vector<std::string> a = proto::split_ws(line);
        // A trailing '&' used to mean "download in the background"; downloads are
        // always asynchronous now, so it is accepted and ignored.
        if (!a.empty() && a.back() == "&") a.pop_back();
        if (a.empty()) continue;

        const std::string &cmd = a[0];
        if (cmd == "create_user") {
            if (arity(a, 3, "create_user <user_id> <password>")) cmd_create_user(a);
        } else if (cmd == "login") {
            if (arity(a, 3, "login <user_id> <password>")) cmd_login(a);
        } else if (cmd == "logout") {
            cmd_logout();
        } else if (cmd == "create_group") {
            if (arity(a, 2, "create_group <group_id>")) cmd_create_group(a);
        } else if (cmd == "join_group") {
            if (arity(a, 2, "join_group <group_id>")) cmd_join_group(a);
        } else if (cmd == "leave_group") {
            if (arity(a, 2, "leave_group <group_id>")) cmd_leave_group(a);
        } else if (cmd == "list_groups") {
            if (arity(a, 1, "list_groups")) cmd_list_groups();
        } else if (cmd == "list_requests") {
            if (arity(a, 2, "list_requests <group_id>")) cmd_list_requests(a);
        } else if (cmd == "accept_request") {
            if (arity(a, 3, "accept_request <group_id> <user_id>")) cmd_accept_request(a);
        } else if (cmd == "upload_file") {
            if (arity(a, 3, "upload_file <group_id> <file_path>")) cmd_upload_file(a);
        } else if (cmd == "list_files") {
            if (arity(a, 2, "list_files <group_id>")) cmd_list_files(a);
        } else if (cmd == "download_file") {
            if (arity(a, 4, "download_file <group_id> <file_name> <destination_path>"))
                cmd_download_file(downloads, a);
        } else if (cmd == "show_downloads") {
            downloads.show_status();
        } else if (cmd == "stop_share") {
            if (arity(a, 3, "stop_share <group_id> <file_name>")) cmd_stop_share(a);
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "quit" || cmd == "exit") {
            break;
        } else {
            std::printf("unknown command '%s' (try 'help')\n", cmd.c_str());
        }
    }

    if (downloads.has_active()) std::printf("note: abandoning downloads still in progress\n");
    if (!g_session.user().empty()) {
        std::string reply;
        g_session.request("LOGOUT " + g_session.user() + " " + g_session.peer_token(), reply);
    }
    return 0;
}
