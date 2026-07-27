#include "state.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "../common/sha1.h"

namespace tracker {
namespace {

// Every identifier in the system arrives as a whitespace-delimited protocol
// token, so a space-separated record format is unambiguous as long as variable
// length lists are preceded by an explicit count.
bool read_count(std::istringstream &iss, size_t &out) {
    long long v = 0;
    if (!(iss >> v) || v < 0) return false;
    out = static_cast<size_t>(v);
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    size_t written = 0;
    while (written < data.size()) {
        ssize_t w = ::write(fd, data.data() + written, data.size() - written);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        written += static_cast<size_t>(w);
    }
    // Flush before the rename so the snapshot is durable, not just visible.
    ::fsync(fd);
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool read_whole_file(const std::string &path, std::string &out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.clear();
    char buf[65536];
    for (;;) {
        ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (r == 0) break;
        out.append(buf, static_cast<size_t>(r));
    }
    ::close(fd);
    return true;
}

}  // namespace

std::string file_key(const std::string &group, const std::string &name) {
    return group + "/" + name;
}

bool State::is_member(const std::string &user, const std::string &group) const {
    auto it = groups.find(group);
    return it != groups.end() && it->second.members.count(user) > 0;
}

bool State::is_owner(const std::string &user, const std::string &group) const {
    auto it = groups.find(group);
    return it != groups.end() && it->second.owner == user;
}

const FileMeta *State::find_file(const std::string &group, const std::string &name) const {
    auto it = files.find(file_key(group, name));
    return it == files.end() ? nullptr : &it->second;
}

FileMeta *State::find_file(const std::string &group, const std::string &name) {
    auto it = files.find(file_key(group, name));
    return it == files.end() ? nullptr : &it->second;
}

std::vector<std::string> State::drop_peer(const std::string &peer) {
    std::vector<std::string> orphaned;
    for (auto it = files.begin(); it != files.end();) {
        it->second.peers.erase(peer);
        if (it->second.peers.empty()) {
            orphaned.push_back(it->first);
            it = files.erase(it);
        } else {
            ++it;
        }
    }
    return orphaned;
}

std::string encode(const State &s) {
    std::ostringstream out;

    for (const auto &kv : s.users) out << "USER " << kv.first << " " << kv.second.password << "\n";

    for (const auto &kv : s.groups) {
        const Group &g = kv.second;
        out << "GROUP " << kv.first << " " << g.owner << " " << g.members.size();
        for (const auto &m : g.members) out << " " << m;
        out << " " << g.pending.size();
        for (const auto &p : g.pending) out << " " << p;
        out << "\n";
    }

    for (const auto &kv : s.files) {
        const FileMeta &f = kv.second;
        out << "FILE " << f.group << " " << f.name << " " << f.size << " " << f.piece_sha.size()
            << " " << f.file_sha << " " << f.owner << " " << f.peers.size();
        for (const auto &p : f.peers) out << " " << p;
        for (const auto &h : f.piece_sha) out << " " << h;
        out << "\n";
    }

    return out.str();
}

bool decode_into(State &s, const std::string &text, bool merge_only) {
    if (!merge_only) {
        s.users.clear();
        s.groups.clear();
        s.files.clear();
    }

    std::istringstream lines(text);
    std::string line;
    bool clean = true;

    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string kind;
        if (!(iss >> kind)) continue;

        if (kind == "USER") {
            std::string name, password;
            if (!(iss >> name >> password)) {
                clean = false;
                continue;
            }
            // Merging must never clobber a live record (in particular the
            // runtime online flag) with a remote copy of the same user.
            auto it = s.users.find(name);
            if (it == s.users.end())
                s.users[name].password = password;
            else if (!merge_only)
                it->second.password = password;

        } else if (kind == "GROUP") {
            std::string name, owner;
            size_t nmembers = 0, npending = 0;
            if (!(iss >> name >> owner) || !read_count(iss, nmembers)) {
                clean = false;
                continue;
            }
            Group g;
            g.owner = owner;
            bool ok = true;
            for (size_t i = 0; i < nmembers; i++) {
                std::string m;
                if (!(iss >> m)) { ok = false; break; }
                g.members.insert(m);
            }
            if (!ok || !read_count(iss, npending)) {
                clean = false;
                continue;
            }
            for (size_t i = 0; i < npending; i++) {
                std::string p;
                if (!(iss >> p)) { ok = false; break; }
                g.pending.push_back(p);
            }
            if (!ok) {
                clean = false;
                continue;
            }

            auto it = s.groups.find(name);
            if (it == s.groups.end()) {
                s.groups[name] = g;
            } else if (merge_only) {
                it->second.members.insert(g.members.begin(), g.members.end());
                for (const auto &p : g.pending) {
                    bool known = it->second.members.count(p) > 0;
                    for (const auto &q : it->second.pending) known = known || q == p;
                    if (!known) it->second.pending.push_back(p);
                }
            } else {
                it->second = g;
            }

        } else if (kind == "FILE") {
            FileMeta f;
            size_t npieces = 0, npeers = 0;
            if (!(iss >> f.group >> f.name >> f.size) || !read_count(iss, npieces) ||
                !(iss >> f.file_sha >> f.owner) || !read_count(iss, npeers)) {
                clean = false;
                continue;
            }
            bool ok = sha1::is_hex(f.file_sha);
            for (size_t i = 0; i < npeers && ok; i++) {
                std::string p;
                if (!(iss >> p)) ok = false;
                else f.peers.insert(p);
            }
            f.piece_sha.reserve(npieces);
            for (size_t i = 0; i < npieces && ok; i++) {
                std::string h;
                if (!(iss >> h) || !sha1::is_hex(h)) ok = false;
                else f.piece_sha.push_back(h);
            }
            if (!ok || f.piece_sha.size() != npieces) {
                clean = false;
                continue;
            }

            std::string key = file_key(f.group, f.name);
            auto it = s.files.find(key);
            if (it == s.files.end()) {
                s.files[key] = f;
            } else if (merge_only) {
                it->second.peers.insert(f.peers.begin(), f.peers.end());
            } else {
                it->second = f;
            }

        } else {
            clean = false;
        }
    }

    return clean;
}

bool save(const State &s, const std::string &dir) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        std::perror("mkdir");
        return false;
    }
    if (!write_file_atomic(dir + "/state.txt", encode(s))) {
        std::perror("save");
        return false;
    }
    return true;
}

bool load(State &s, const std::string &dir) {
    std::string text;
    if (!read_whole_file(dir + "/state.txt", text)) return false;  // first run
    return decode_into(s, text, /*merge_only=*/false);
}

}  // namespace tracker
