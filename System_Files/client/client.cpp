#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <sstream>
#include <memory>
#include <cstring>
#include "../common/proto.h"
#include "../common/sha1.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

using namespace std;

const size_t PIECE_SZ = 524288;
const int MAX_CONCURRENT = 8;

static vector<string> trackers;
static string connected_tracker, current_user;
static map<string, string> uploaded_files;
static mutex uploaded_mtx, downloads_mtx;
static int peer_port = 0;

struct DownloadStatus {
    string group, filename, dest;
    int npieces;
    vector<int> have;
    atomic<int> remaining;
    atomic<bool> completed, running;
    mutex m;
    DownloadStatus() : npieces(0), remaining(0), completed(false), running(false) {}
};

static map<string, shared_ptr<DownloadStatus>> downloads;

bool connect_send(const string& addr, const string& msg, string& reply) {
    size_t p = addr.find(':');
    if(p == string::npos) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;

    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(stoi(addr.substr(p+1)));
    sa.sin_addr.s_addr = inet_addr(addr.substr(0,p).c_str());

    if(connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }

    bool ok = send_msg(fd, msg) && recv_msg(fd, reply);
    close(fd);
    return ok;
}

bool failover_send(const string& msg, string& reply) {
    if(connect_send(connected_tracker, msg, reply)) return true;

    for(auto& t : trackers) {
        if(t != connected_tracker && connect_send(t, msg, reply)) {
            connected_tracker = t;
            cout << "Switched to tracker: " << t << endl;
            return true;
        }
    }
    return false;
}

void compute_hashes(const string& path, vector<string>& piece_hex, string& file_hex, uint64_t& size) {
    FILE *f = fopen(path.c_str(), "rb");
    if(!f) return;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t np = (size + PIECE_SZ - 1) / PIECE_SZ;
    piece_hex.clear();
    piece_hex.reserve(np);

    uint8_t *buf = (uint8_t*)malloc(PIECE_SZ);
    if(!buf) { fclose(f); return; }

    for(size_t i = 0; i < np; i++) {
        size_t to_read = (i == np - 1) ? size - i * PIECE_SZ : PIECE_SZ;
        size_t bytes_read = fread(buf, 1, to_read, f);
        if(bytes_read != to_read) break;

        char ph[41];
        sha1_hex(buf, to_read, ph);
        piece_hex.push_back(string(ph));
    }

    string concat;
    for(auto& s : piece_hex) concat += s;
    char fh[41];
    sha1_hex((const uint8_t*)concat.data(), concat.size(), fh);
    file_hex = string(fh);

    free(buf);
    fclose(f);
}

void peer_server_thread(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if(bind(fd, (sockaddr*)&sa, sizeof(sa)) < 0) return;

    listen(fd, 50);

    while(true) {
        int c = accept(fd, nullptr, nullptr);
        if(c < 0) continue;

        thread([c]() {
            string rq;
            if(!recv_msg(c, rq)) {
                close(c);
                return;
            }

            auto parts = split_ws(rq);
            if(parts.size() != 3 || parts[0] != "GETPIECE") {
                send_msg(c, "ERR");
                close(c);
                return;
            }

            string filename = parts[1];
            int idx = stoi(parts[2]);
            string filepath;

            {
                lock_guard<mutex> g(uploaded_mtx);
                auto it = uploaded_files.find(filename);
                if(it != uploaded_files.end()) {
                    filepath = it->second;
                } else {
                    send_msg(c, "ERR");
                    close(c);
                    return;
                }
            }

            FILE *f = fopen(filepath.c_str(), "rb");
            if(!f) {
                send_msg(c, "ERR");
                close(c);
                return;
            }

            fseek(f, 0, SEEK_END);
            size_t fsz = ftell(f);
            size_t np = (fsz + PIECE_SZ - 1) / PIECE_SZ;

            if(idx < 0 || idx >= (int)np) {
                send_msg(c, "ERR");
                fclose(f);
                close(c);
                return;
            }

            size_t off = (size_t)idx * PIECE_SZ;
            size_t to_read = (idx == (int)np - 1) ? fsz - off : PIECE_SZ;

            fseek(f, off, SEEK_SET);
            vector<uint8_t> data(to_read);
            size_t r = fread(data.data(), 1, to_read, f);
            fclose(f);

            if(r != to_read) {
                send_msg(c, "ERR");
            } else {
                send_msg(c, "OK");
                uint32_t n = htonl((uint32_t)to_read);
                send_all(c, &n, 4);
                send_all(c, data.data(), to_read);
            }
            close(c);
        }).detach();
    }
}

void start_peer_server() {
    srand(time(NULL) + getpid());
    int port = 20000 + rand() % 15000;

    for(int tries = 0; tries < 40; tries++, port++) {
        int test_fd = socket(AF_INET, SOCK_STREAM, 0);
        if(test_fd < 0) continue;

        int opt = 1;
        setsockopt(test_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = INADDR_ANY;

        if(bind(test_fd, (sockaddr*)&sa, sizeof(sa)) == 0) {
            close(test_fd);
            thread(peer_server_thread, port).detach();
            peer_port = port;
            return;
        }
        close(test_fd);
    }
    peer_port = port;
}

bool download_piece(const string& peer, const string& fname, int idx, const string& dest, const string& hash) {
    size_t p = peer.find(':');
    if(p == string::npos) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return false;

    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(stoi(peer.substr(p+1)));
    sa.sin_addr.s_addr = inet_addr(peer.substr(0,p).c_str());

    if(connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }

    string req = "GETPIECE " + fname + " " + to_string(idx);
    if(!send_msg(fd, req)) {
        close(fd);
        return false;
    }

    string rep;
    if(!recv_msg(fd, rep) || rep != "OK") {
        close(fd);
        return false;
    }

    uint32_t n;
    if(recv_all(fd, &n, 4) != 4) {
        close(fd);
        return false;
    }
    n = ntohl(n);
    if(n > PIECE_SZ) {
        close(fd);
        return false;
    }

    vector<char> buf(n);
    if(recv_all(fd, buf.data(), n) != (ssize_t)n) {
        close(fd);
        return false;
    }

    char computed[41];
    sha1_hex((const uint8_t*)buf.data(), n, computed);
    if(string(computed) != hash) {
        close(fd);
        return false;
    }

    int out_fd = open(dest.c_str(), O_WRONLY);
    if(out_fd < 0) {
        close(fd);
        return false;
    }

    off_t off = (off_t)idx * (off_t)PIECE_SZ;
    if(lseek(out_fd, off, SEEK_SET) < 0) {
        close(out_fd);
        close(fd);
        return false;
    }

    ssize_t w = write(out_fd, buf.data(), n);
    close(out_fd);
    close(fd);

    return w == (ssize_t)n;
}

void download_file(string g, string fname, string dest, vector<string> hashes, vector<string> peers, uint64_t fsz, string fsha) {
    auto ds = make_shared<DownloadStatus>();
    ds->group = g; ds->filename = fname; ds->dest = dest; ds->npieces = hashes.size();
    ds->have.assign(hashes.size(), 0); ds->remaining = hashes.size();
    ds->completed = false; ds->running = true;

    {
        lock_guard<mutex> g_dl(downloads_mtx);
        downloads[g + ":" + fname] = ds;
    }

    int batch = min(MAX_CONCURRENT, (int)hashes.size());
    for(int start = 0; start < (int)hashes.size(); start += batch) {
        int end = min(start + batch, (int)hashes.size());
        vector<thread> threads;

        for(int idx = start; idx < end; idx++) {
            threads.push_back(thread([idx, fname, dest, ds, &hashes, &peers]() {
                string hash = hashes[idx];
                bool success = false;

                for(const auto& peer : peers) {
                    for(int retry = 0; retry < 2; retry++) {
                        if(download_piece(peer, fname, idx, dest, hash)) {
                            {
                                lock_guard<mutex> lg(ds->m);
                                ds->have[idx] = 1;
                            }
                            ds->remaining--;
                            success = true;
                            break;
                        }
                    }
                    if(success) break;
                }
            }));
        }

        for(auto& t : threads) {
            if(t.joinable()) t.join();
        }
    }

    ds->running = false;
    if(ds->remaining == 0) {
        ds->completed = true;
        cout << "[C] " << g << " " << fname << endl;

        vector<string> temp_pieces;
        string temp_hash;
        uint64_t temp_size;
        compute_hashes(dest, temp_pieces, temp_hash, temp_size);

        if(temp_hash == fsha && temp_size == fsz) {
            string peer_addr = "127.0.0.1:" + to_string(peer_port);
            string rep;
            failover_send("ADD_PEER " + g + " " + fname + " " + peer_addr, rep);

            lock_guard<mutex> g_uf(uploaded_mtx);
            uploaded_files[fname] = dest;
        }
    }
}

void show_downloads() {
    lock_guard<mutex> g(downloads_mtx);
    if(downloads.empty()) {
        cout << "No active downloads" << endl;
        return;
    }

    for(auto& kv : downloads) {
        auto ds = kv.second;
        if(!ds) continue;

        lock_guard<mutex> g_ds(ds->m);
        int have = 0;
        for(int v : ds->have) if(v) have++;

        if(ds->completed) {
            printf("[C] %s %s\n", ds->group.c_str(), ds->filename.c_str());
        } else if(ds->running) {
            printf("[D] %s %s - %d/%d\n", ds->group.c_str(), ds->filename.c_str(), have, ds->npieces);
        } else if(have > 0) {
            printf("[P] %s %s - %d/%d\n", ds->group.c_str(), ds->filename.c_str(), have, ds->npieces);
        }
    }
}

vector<string> parse_hashes(const string& line) {
    vector<string> hashes;
    size_t pos = 0;

    while(pos < line.length()) {
        while(pos < line.length() && !isxdigit(line[pos])) pos++;
        if(pos + 40 <= line.length()) {
            string candidate = line.substr(pos, 40);
            bool valid = true;
            for(char c : candidate) {
                if(!isxdigit(c)) { valid = false; break; }
            }
            if(valid) {
                hashes.push_back(candidate);
                pos += 40;
                if(pos < line.length() && line[pos] == ',') pos++;
            } else {
                pos++;
            }
        } else {
            break;
        }
    }
    return hashes;
}

bool parse_download_cmd(const string& line, string& group, string& filename, string& dest) {
    string cleaned = line;
    size_t amp = cleaned.find_last_of('&');
    if(amp != string::npos) cleaned = cleaned.substr(0, amp);

    auto tokens = split_ws(cleaned);
    if(tokens.size() != 4 || tokens[0] != "download_file") return false;

    group = tokens[1]; filename = tokens[2]; dest = tokens[3];
    return true;
}

int main(int argc, char **argv) {
    if(argc < 3) {
        cerr << "Usage: client <tracker_ip:port> tracker_info.txt\n";
        return 1;
    }

    connected_tracker = argv[1];

    ifstream ifs(argv[2]);
    string l;
    while(getline(ifs, l) && !l.empty()) trackers.push_back(l);

    start_peer_server();
    printf("Peer server listening on port %d\n", peer_port);

    string line;
    while(true) {
        cout << "> ";
        if(!getline(cin, line)) break;

        auto tokens = split_ws(line);
        if(tokens.empty()) continue;

        string cmd = tokens[0], rep;

        if(cmd == "create_user" && tokens.size() == 3) {
            if(failover_send("REGISTER " + tokens[1] + " " + tokens[2], rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "login" && tokens.size() == 3) {
            if(failover_send("LOGIN " + tokens[1] + " " + tokens[2], rep)) {
                if(rep == "OK") {
                    current_user = tokens[1];
                }
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "create_group" && tokens.size() == 2) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("CREATE_GROUP " + current_user + " " + tokens[1], rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "join_group" && tokens.size() == 2) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("JOIN_GROUP " + current_user + " " + tokens[1], rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "leave_group" && tokens.size() == 2) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("LEAVE_GROUP " + current_user + " " + tokens[1], rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "list_groups") {
            if(failover_send("LIST_GROUPS", rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "list_requests" && tokens.size() == 2) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("LIST_REQUESTS " + tokens[1] + " " + current_user, rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "accept_request" && tokens.size() == 3) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("ACCEPT_REQUEST " + tokens[1] + " " + tokens[2] + " " + current_user, rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "upload_file" && tokens.size() == 3) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }

            string g = tokens[1], path = tokens[2];
            vector<string> piece_hash;
            string file_hash;
            uint64_t fsz;

            compute_hashes(path, piece_hash, file_hash, fsz);
            if(piece_hash.empty()) { cout << "file read error" << endl; continue; }

            string fname = path.substr(path.find_last_of("/\\") + 1);
            string peer = "127.0.0.1:" + to_string(peer_port);

            {
                lock_guard<mutex> g_uf(uploaded_mtx);
                uploaded_files[fname] = path;
            }

            string msg = "UPLOAD_META " + g + " " + fname + " " + to_string(fsz) + " " + to_string(piece_hash.size()) + " " + file_hash + " " + peer + " " + current_user;
            for(auto& ph : piece_hash) msg += " " + ph;

            if(failover_send(msg, rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "list_files" && tokens.size() == 2) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }
            if(failover_send("LIST_FILES " + tokens[1] + " " + current_user, rep)) {
                cout << rep << endl;
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "download_file") {
            if(current_user.empty()) { cout << "login required" << endl; continue; }

            string g, fname, dest;
            if(!parse_download_cmd(line, g, fname, dest)) {
                cout << "Usage: download_file <group> <filename> <destination>" << endl;
                continue;
            }

            if(!failover_send("GET_FILE_PEERS " + g + " " + fname + " " + current_user, rep)) {
                cout << "All trackers unreachable" << endl;
                continue;
            }

            if(rep.rfind("ERR", 0) == 0) { cout << rep << endl; continue; }

            istringstream iss(rep);
            uint64_t fsz;
            int np;
            iss >> fsz >> np;

            string tmp;
            getline(iss, tmp);
            string file_sha;
            getline(iss, file_sha);
            string piece_line;
            getline(iss, piece_line);

            vector<string> hashes = parse_hashes(piece_line);
            if((int)hashes.size() != np) {
                cout << "Error: hash count mismatch" << endl;
                continue;
            }

            string pline;
            while(getline(iss, pline) && pline != "PEERS");

            vector<string> peers;
            while(getline(iss, pline) && !pline.empty()) {
                peers.push_back(pline);
            }

            if(peers.empty()) { cout << "No peers available" << endl; continue; }

            struct stat st;
            string outpath = dest;
            if(stat(dest.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                outpath = dest + "/" + fname;
            }

            int out_fd = open(outpath.c_str(), O_CREAT | O_RDWR, 0644);
            if(out_fd < 0) { cout << "cannot create " << dest << endl; continue; }

            if(ftruncate(out_fd, fsz) != 0) {
                cout << "cannot set file size" << endl;
                close(out_fd);
                continue;
            }
            close(out_fd);

            bool background = line.find('&') != string::npos;

            if(background) {
                thread(download_file, g, fname, outpath, hashes, peers, fsz, file_sha).detach();
            } else {
                download_file(g, fname, outpath, hashes, peers, fsz, file_sha);
            }
        }
        else if(cmd == "show_downloads") {
            show_downloads();
        }
        else if(cmd == "stop_share" && tokens.size() == 3) {
            if(current_user.empty()) { cout << "login required" << endl; continue; }

            string peer = "127.0.0.1:" + to_string(peer_port);
            if(failover_send("STOP_SHARE " + tokens[1] + " " + tokens[2] + " " + peer, rep)) {
                cout << rep << endl;
                lock_guard<mutex> g_uf(uploaded_mtx);
                uploaded_files.erase(tokens[2]);
            } else {
                cout << "All trackers unreachable" << endl;
            }
        }
        else if(cmd == "logout") {
            current_user.clear();
            lock_guard<mutex> g_uf(uploaded_mtx);
            uploaded_files.clear();
            cout << "OK" << endl;
        }
        else if(cmd == "quit") {
            break;
        }
        else {
            cout << "Unknown command" << endl;
        }
    }

    return 0;
}