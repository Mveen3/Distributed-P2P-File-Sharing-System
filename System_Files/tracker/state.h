#ifndef P2P_TRACKER_STATE_H
#define P2P_TRACKER_STATE_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tracker {

struct User {
    std::string password;
    bool online = false;  // runtime only, never persisted
};

struct Group {
    std::string owner;
    std::set<std::string> members;
    std::vector<std::string> pending;  // join requests, kept in arrival order
};

struct FileMeta {
    std::string group;
    std::string name;
    std::string owner;
    std::string file_sha;                 // SHA1 of the complete file contents
    uint64_t size = 0;
    std::vector<std::string> piece_sha;   // SHA1 per 512 KB piece, in order
    std::set<std::string> peers;          // "ip:port" of clients seeding this file
};

// The complete tracker data set.
//
// This struct carries no lock of its own: tracker.cpp owns a single mutex that
// guards it for the whole process, and every handler takes that mutex for the
// duration of one request. One coarse lock is the right trade-off here because
// requests are short, purely in-memory operations, and it makes the "check
// permission, mutate, queue replication op" sequence atomic without any lock
// ordering rules to get wrong.
struct State {
    std::map<std::string, User> users;
    std::map<std::string, Group> groups;
    std::map<std::string, FileMeta> files;  // keyed by file_key(group, name)

    // Convenience lookups. All of them assume the caller holds the state mutex.
    bool is_member(const std::string &user, const std::string &group) const;
    bool is_owner(const std::string &user, const std::string &group) const;
    const FileMeta *find_file(const std::string &group, const std::string &name) const;
    FileMeta *find_file(const std::string &group, const std::string &name);

    // Removes peer from every file it seeds. Returns the keys of files that
    // lost their last seeder and were therefore dropped.
    std::vector<std::string> drop_peer(const std::string &peer);
};

// Files are keyed by group and name together: the same name may legitimately be
// shared in two different groups and those are distinct files.
std::string file_key(const std::string &group, const std::string &name);

// Persistence. The whole data set lives in one file (dir/state.txt) that save()
// writes to a temporary path and rename()s into place, so a crash mid-write can
// never leave a truncated or half-updated snapshot behind.
bool save(const State &s, const std::string &dir);
bool load(State &s, const std::string &dir);

// Text encoding shared by the on-disk format and the replication snapshot, so
// there is exactly one parser to keep correct.
std::string encode(const State &s);
bool decode_into(State &s, const std::string &text, bool merge_only);

}  // namespace tracker

#endif
