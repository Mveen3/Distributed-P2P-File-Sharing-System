#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <cstring>
#include "../common/proto.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace std;

struct User { 
    string pass; 
    bool logged; 
    User(const string &p="") : pass(p), logged(false) {} 
};

struct File { 
    string group, filename, owner, sha; 
    uint64_t size; 
    vector<string> piece_sha; 
    set<string> peers; 
};

static unordered_map<string, User> users;
static unordered_map<string, pair<string, set<string>>> groups; 
static unordered_map<string, vector<string>> requests;
static unordered_map<string, File> files;
static mutex mtx;
static vector<string> trackers;
static int self_idx;
static string data_dir;

bool member(const string& user, const string& group) {
    auto it = groups.find(group);
    return it != groups.end() && it->second.second.count(user);
}

bool owner(const string& user, const string& group) {
    auto it = groups.find(group);
    return it != groups.end() && it->second.first == user;
}

void save() {
    mkdir(data_dir.c_str(), 0755);

    ofstream uf(data_dir + "/users.txt");
    for(auto& p : users) {
        uf << p.first << " " << p.second.pass << "\n";
    }
    uf.close();

    ofstream gf(data_dir + "/groups.txt");
    for(auto& p : groups) {
        gf << p.first << " " << p.second.first;
        for(auto& m : p.second.second) gf << " " << m;
        gf << "\n";
    }
    gf.close();

    ofstream rf(data_dir + "/requests.txt");
    for(auto& p : requests) {
        if(!p.second.empty()) {
            rf << p.first;
            for(auto& u : p.second) rf << " " << u;
            rf << "\n";
        }
    }
    rf.close();

    ofstream ff(data_dir + "/files.txt");
    for(auto& p : files) {
        auto& f = p.second;
        ff << f.group << " " << f.filename << " " << f.size << " " << f.piece_sha.size() << " " << f.sha << " " << f.owner;
        for(size_t i = 0; i < f.piece_sha.size(); i++) ff << (i ? "," : " ") << f.piece_sha[i];
        for(auto& peer : f.peers) ff << " " << peer;
        ff << "\n";
    }
    ff.close();

    cout << "Data saved to " << data_dir << endl;
}

void load() {
    data_dir = "tracker_data_" + to_string(self_idx);
    cout << "Loading data from " << data_dir << endl;

    ifstream uf(data_dir + "/users.txt"), gf(data_dir + "/groups.txt"), rf(data_dir + "/requests.txt"), ff(data_dir + "/files.txt");
    string line, u, p, g, o, m;

    // Load users
    while(getline(uf, line) && !line.empty()) {
        istringstream iss(line);
        if(iss >> u >> p) {
            users[u] = User(p);
            cout << "Loaded user: " << u << endl;
        }
    }

    // Load groups
    while(getline(gf, line) && !line.empty()) {
        istringstream iss(line);
        if(iss >> g >> o) {
            set<string> members;
            while(iss >> m) members.insert(m);
            groups[g] = make_pair(o, members);
            cout << "Loaded group: " << g << " owner: " << o << endl;
        }
    }

    // Load requests
    while(getline(rf, line) && !line.empty()) {
        istringstream iss(line);
        if(iss >> g) {
            vector<string> reqs;
            while(iss >> u) reqs.push_back(u);
            requests[g] = reqs;
            cout << "Loaded requests for group: " << g << endl;
        }
    }

    // Load files
    while(getline(ff, line) && !line.empty()) {
        istringstream iss(line);
        File file;
        string np_str, token;
        if(iss >> file.group >> file.filename >> file.size >> np_str >> file.sha >> file.owner && iss >> token) {
            int np = stoi(np_str);
            size_t pos = 0;
            while(pos < token.size() && (int)file.piece_sha.size() < np) {
                while(pos < token.size() && !isxdigit(token[pos])) pos++;
                if(pos + 40 <= token.size()) {
                    file.piece_sha.push_back(token.substr(pos, 40));
                    pos += 40;
                }
            }
            while(iss >> token) file.peers.insert(token);
            files[file.group + " " + file.filename] = file;
            cout << "Loaded file: " << file.filename << " in group: " << file.group << endl;
        }
    }
}

bool send_to(const string& ep, const string& msg) {
    size_t p = ep.find(':');
    if(p == string::npos) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(stoi(ep.substr(p+1)));
    sa.sin_addr.s_addr = inet_addr(ep.substr(0,p).c_str());

    if(connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }

    bool ok = send_msg(fd, msg);
    if(ok) {
        string rep;
        recv_msg(fd, rep);
    }
    close(fd);
    return ok;
}

void sync(const string& cmd) {
    thread([cmd]() {
        for(size_t i = 0; i < trackers.size(); ++i) {
            if((int)i != self_idx) {
                if(!send_to(trackers[i], "SYNC " + cmd)) {
                    cout << "Warning: Failed to sync to tracker " << i << ": " << trackers[i] << endl;
                } else {
                    cout << "Synced to tracker " << i << ": " << cmd << endl;
                }
            }
        }
    }).detach();
}

// Handle sync operations with UPLOAD_META prefix handling
void handle_sync(const string& sync_data) {
    istringstream iss(sync_data);
    string cmd;
    if(!(iss >> cmd)) return;

    cout << "Processing sync: " << sync_data << endl;

    if(cmd == "REGISTER") {
        string user, pass;
        if(iss >> user >> pass) {
            users[user] = User(pass);
            cout << "Synced user registration: " << user << endl;
        }
    }
    else if(cmd == "CREATE_GROUP") {
        string user, group;
        if(iss >> user >> group) {
            set<string> members; 
            members.insert(user);
            groups[group] = make_pair(user, members);
            cout << "Synced group creation: " << group << " by " << user << endl;
        }
    }
    else if(cmd == "JOIN_GROUP") {
        string user, group;
        if(iss >> user >> group) {
            auto& v = requests[group];
            if(find(v.begin(), v.end(), user) == v.end()) {
                v.push_back(user);
                cout << "Synced join request: " << user << " -> " << group << endl;
            }
        }
    }
    else if(cmd == "ACCEPT_REQUEST") {
        string group, user;
        if(iss >> group >> user) {
            auto& v = requests[group];
            auto it = find(v.begin(), v.end(), user);
            if(it != v.end()) {
                v.erase(it);
                groups[group].second.insert(user);
                cout << "Synced request acceptance: " << user << " joined " << group << endl;
            }
        }
    }
    else if(cmd == "LEAVE_GROUP") {
        string user, group;
        if(iss >> user >> group) {
            auto git = groups.find(group);
            if(git != groups.end() && git->second.second.count(user)) {
                git->second.second.erase(user);

                // Remove files owned by leaving user
                for(auto it = files.begin(); it != files.end();) {
                    if(it->second.group == group && it->second.owner == user) {
                        it = files.erase(it);
                    } else {
                        ++it;
                    }
                }

                if(git->second.first == user) {
                    if(git->second.second.empty()) {
                        groups.erase(group);
                        requests.erase(group);
                    } else {
                        git->second.first = *git->second.second.begin();
                    }
                }
                cout << "Synced group leave: " << user << " left " << group << endl;
            }
        }
    }
    else if(cmd == "STOP_SHARE") {
        string group, filename, peer;
        if(iss >> group >> filename >> peer) {
            string key = group + " " + filename;
            auto it = files.find(key);
            if(it != files.end()) {
                it->second.peers.erase(peer);
                if(it->second.peers.empty()) {
                    files.erase(it);
                    cout << "Synced file removal: " << filename << " from " << group << endl;
                } else {
                    cout << "Synced peer removal: " << peer << " from " << filename << endl;
                }
            }
        }
    }
    else if(cmd == "ADD_PEER") {
        string group, filename, peer;
        if(iss >> group >> filename >> peer) {
            string key = group + " " + filename;
            auto it = files.find(key);
            if(it != files.end()) {
                it->second.peers.insert(peer);
                cout << "Synced peer addition: " << peer << " to " << filename << endl;
            }
        }
    }
    else if(cmd == "UPLOAD_META") {
        File file;
        string peer, user, np_str;
        if(iss >> file.group >> file.filename >> file.size >> np_str >> file.sha >> peer >> user) {
            int np = stoi(np_str);

            string hash;
            while(iss >> hash && (int)file.piece_sha.size() < np) {
                if(hash.length() == 40) {
                    file.piece_sha.push_back(hash);
                }
            }

            if((int)file.piece_sha.size() == np) {
                file.owner = user;
                file.peers.insert(peer);
                files[file.group + " " + file.filename] = file;
                cout << "Synced file upload: " << file.filename << " in " << file.group << " by " << user << endl;
            }
        }
    }
    else {
        // Handle file upload sync without UPLOAD_META prefix
        // Reset the istringstream to process the entire sync_data as file upload
        istringstream file_iss(sync_data);
        File file;
        string peer, user, np_str;
        if(file_iss >> file.group >> file.filename >> file.size >> np_str >> file.sha >> peer >> user) {
            int np = stoi(np_str);

            string hash;
            while(file_iss >> hash && (int)file.piece_sha.size() < np) {
                if(hash.length() == 40) {
                    file.piece_sha.push_back(hash);
                }
            }

            if((int)file.piece_sha.size() == np) {
                file.owner = user;
                file.peers.insert(peer);
                files[file.group + " " + file.filename] = file;
                cout << "Synced file upload (auto-detected): " << file.filename << " in " << file.group << " by " << user << endl;
            } else {
                cout << "Unknown sync command: " << cmd << endl;
            }
        } else {
            cout << "Unknown sync command: " << cmd << endl;
        }
    }

    // Save immediately after sync processing
    save();
}

void handle(const vector<string>& parts, int fd) {
    string cmd = parts[0];

    if(cmd == "REGISTER" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(users.count(parts[1])) {
            send_msg(fd, "ERR user_exists");
        } else {
            users[parts[1]] = User(parts[2]);
            save(); // Save immediately
            send_msg(fd, "OK");
            sync("REGISTER " + parts[1] + " " + parts[2]);
        }
    }
    else if(cmd == "LOGIN" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        auto it = users.find(parts[1]);
        if(it == users.end()) {
            send_msg(fd, "ERR user_not_found");
        } else if(it->second.pass != parts[2]) {
            send_msg(fd, "ERR wrong_password");
        } else {
            it->second.logged = true;
            save(); // Save login state
            send_msg(fd, "OK");
        }
    }
    else if(cmd == "CREATE_GROUP" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(groups.count(parts[2])) {
            send_msg(fd, "ERR grp_exists");
        } else {
            set<string> members; 
            members.insert(parts[1]); 
            groups[parts[2]] = make_pair(parts[1], members);
            save(); // Save immediately
            send_msg(fd, "OK");
            sync("CREATE_GROUP " + parts[1] + " " + parts[2]);
        }
    }
    else if(cmd == "JOIN_GROUP" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(!groups.count(parts[2])) {
            send_msg(fd, "ERR no_group");
        } else if(member(parts[1], parts[2])) {
            send_msg(fd, "ERR already_member");
        } else {
            auto& v = requests[parts[2]]; 
            if(find(v.begin(), v.end(), parts[1]) == v.end()) v.push_back(parts[1]);
            save(); // Save immediately
            send_msg(fd, "OK");
            sync("JOIN_GROUP " + parts[1] + " " + parts[2]);
        }
    }
    else if(cmd == "LIST_GROUPS") {
        lock_guard<mutex> g(mtx);
        string out;
        for(auto& p : groups) {
            out += p.first + "\n";
        }
        send_msg(fd, out);
    }
    else if(cmd == "LIST_REQUESTS" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(!owner(parts[2], parts[1])) {
            send_msg(fd, "ERR not_owner");
        } else {
            string out;
            for(auto& u : requests[parts[1]]) out += u + "\n";
            send_msg(fd, out);
        }
    }
    else if(cmd == "ACCEPT_REQUEST" && parts.size() == 4) {
        lock_guard<mutex> g(mtx);
        if(!owner(parts[3], parts[1])) {
            send_msg(fd, "ERR not_owner");
        } else {
            auto& v = requests[parts[1]];
            auto it = find(v.begin(), v.end(), parts[2]);
            if(it == v.end()) {
                send_msg(fd, "ERR no_request");
            } else {
                v.erase(it);
                groups[parts[1]].second.insert(parts[2]);
                save(); // Save immediately
                send_msg(fd, "OK");
                sync("ACCEPT_REQUEST " + parts[1] + " " + parts[2]);
            }
        }
    }
    else if(cmd == "LEAVE_GROUP" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(!member(parts[1], parts[2])) {
            send_msg(fd, "ERR not_member");
        } else {
            auto& gi = groups[parts[2]];
            gi.second.erase(parts[1]);

            for(auto it = files.begin(); it != files.end();) {
                if(it->second.group == parts[2] && it->second.owner == parts[1]) {
                    it = files.erase(it);
                } else {
                    ++it;
                }
            }

            if(gi.first == parts[1]) {
                if(gi.second.empty()) {
                    groups.erase(parts[2]);
                    requests.erase(parts[2]);
                } else {
                    gi.first = *gi.second.begin();
                }
            }
            save(); // Save immediately
            send_msg(fd, "OK");
            sync("LEAVE_GROUP " + parts[1] + " " + parts[2]);
        }
    }
    else if(cmd == "LIST_FILES" && parts.size() == 3) {
        lock_guard<mutex> g(mtx);
        if(!member(parts[2], parts[1])) {
            send_msg(fd, "ERR not_member");
        } else {
            string out;
            for(auto& p : files) {
                if(p.second.group == parts[1]) out += p.second.filename + "\n";
            }
            send_msg(fd, out);
        }
    }
    else if(cmd == "GET_FILE_PEERS" && parts.size() == 4) {
        lock_guard<mutex> g(mtx);
        string key = parts[1] + " " + parts[2];
        if(!member(parts[3], parts[1])) {
            send_msg(fd, "ERR not_member");
        } else {
            auto it = files.find(key);
            if(it == files.end()) {
                send_msg(fd, "ERR no_file");
            } else if(it->second.peers.empty()) {
                send_msg(fd, "ERR no_peers_available");
            } else {
                auto& f = it->second;
                string out = to_string(f.size) + " " + to_string(f.piece_sha.size()) + "\n" + f.sha + "\n";
                for(size_t i = 0; i < f.piece_sha.size(); i++) out += (i ? "," : "") + f.piece_sha[i];
                out += "\nPEERS\n";
                for(auto& p : f.peers) out += p + "\n";
                send_msg(fd, out);
            }
        }
    }
    else if(cmd == "STOP_SHARE" && parts.size() == 4) {
        lock_guard<mutex> g(mtx);
        string key = parts[1] + " " + parts[2];
        auto it = files.find(key);
        if(it != files.end()) {
            it->second.peers.erase(parts[3]);
            if(it->second.peers.empty()) files.erase(it);
        }
        save(); // Save immediately
        send_msg(fd, "OK");
        sync("STOP_SHARE " + parts[1] + " " + parts[2] + " " + parts[3]);
    }
    else if(cmd == "ADD_PEER" && parts.size() == 4) {
        lock_guard<mutex> g(mtx);
        string key = parts[1] + " " + parts[2];
        auto it = files.find(key);
        if(it != files.end()) it->second.peers.insert(parts[3]);
        save(); // Save immediately
        send_msg(fd, "OK");
        sync("ADD_PEER " + parts[1] + " " + parts[2] + " " + parts[3]);
    }
    else if(cmd.substr(0, 11) == "UPLOAD_META") {
        string full = cmd;
        for(size_t i = 1; i < parts.size(); i++) full += " " + parts[i];

        istringstream iss(full.substr(12));
        File file;
        string peer, user, np_str;
        iss >> file.group >> file.filename >> file.size >> np_str >> file.sha >> peer >> user;
        int np = stoi(np_str);

        string hash;
        while(iss >> hash && (int)file.piece_sha.size() < np) {
            if(hash.length() == 40) {
                file.piece_sha.push_back(hash);
            }
        }

        lock_guard<mutex> g(mtx);
        if(!member(user, file.group)) {
            send_msg(fd, "ERR not_member");
        } else if((int)file.piece_sha.size() != np) {
            send_msg(fd, "ERR piece_count_mismatch");
        } else {
            file.owner = user;
            file.peers.insert(peer);
            files[file.group + " " + file.filename] = file;
            save(); // Save immediately
            send_msg(fd, "OK");
            // Send with UPLOAD_META prefix for sync handling
            sync("UPLOAD_META " + full.substr(12));
        }
    }
    else if(cmd == "SYNC" && parts.size() >= 2) {
        string sync_data;
        for(size_t i = 1; i < parts.size(); i++) {
            if(i > 1) sync_data += " ";
            sync_data += parts[i];
        }

        if(!sync_data.empty()) {
            lock_guard<mutex> g(mtx);
            handle_sync(sync_data);
        }
        send_msg(fd, "OK");
    }
    else {
        send_msg(fd, "ERR unknown_cmd");
    }
}

void client_loop(int fd) {
    string msg;
    while(recv_msg(fd, msg) && !msg.empty()) {
        auto parts = split_ws(msg);
        if(!parts.empty()) handle(parts, fd);
    }
    close(fd);
}

int main(int argc, char **argv) {
    if(argc < 3) {
        cerr << "Usage: tracker tracker_info.txt <idx>\n";
        return 1;
    }

    self_idx = atoi(argv[2]);
    load();

    ifstream ifs(argv[1]);
    string line;
    while(getline(ifs, line) && !line.empty()) trackers.push_back(line);
    if(self_idx < 0 || self_idx >= (int)trackers.size()) {
        cerr << "bad idx\n";
        return 1;
    }

    string my = trackers[self_idx];
    size_t p = my.find(':');
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(stoi(my.substr(p+1)));
    sa.sin_addr.s_addr = INADDR_ANY;

    bind(fd, (sockaddr*)&sa, sizeof(sa));
    listen(fd, 20);

    printf("Tracker %d listening on %s\n", self_idx, my.c_str());

    thread([&]() {
        string cmd;
        while(getline(cin, cmd) && cmd != "quit") {
            if(cmd == "save") {
                lock_guard<mutex> g(mtx);
                save();
            } else if(cmd == "status") {
                lock_guard<mutex> g(mtx);
                cout << "Users: " << users.size() << ", Groups: " << groups.size() 
                     << ", Files: " << files.size() << endl;
            }
        }
        lock_guard<mutex> g(mtx);
        save();
        exit(0);
    }).detach();

    while(true) {
        int cfd = accept(fd, nullptr, nullptr);
        if(cfd >= 0) thread(client_loop, cfd).detach();
    }
}