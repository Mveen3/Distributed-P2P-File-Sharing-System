#ifndef P2P_CLIENT_DOWNLOAD_H
#define P2P_CLIENT_DOWNLOAD_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "share.h"

namespace client {

// Downloads run in the background: the command loop stays responsive, several
// files can be in flight at once, and show_downloads has something to report.
//
// Within one file, a pool of worker threads pulls piece indices off a shared
// cursor and fetches them from different peers in parallel. Each worker starts
// at a different offset in the peer list, so the load is spread across the swarm
// instead of piling onto whichever peer happens to be first.
class DownloadManager {
public:
    // Called once a download has completed and been verified, so the caller can
    // start seeding the file and register itself with the tracker.
    // Arguments: group, file name, local path, size.
    using CompletionHandler =
        std::function<void(const std::string &, const std::string &, const std::string &, uint64_t)>;

    explicit DownloadManager(CompletionHandler on_complete);

    struct Request {
        std::string group;
        std::string name;
        std::string dest_path;   // final path, already resolved
        std::string file_sha;
        uint64_t size = 0;
        std::vector<std::string> piece_sha;
        std::vector<std::string> peers;
    };

    // Returns false and fills error when the download cannot be started (already
    // running, destination not writable, ...). The transfer itself runs in the
    // background; its outcome is reported through show_status() and a "[C]" line
    // printed on completion.
    bool start(const Request &req, std::string &error);

    void show_status() const;
    bool has_active() const;

private:
    struct Download {
        std::string group, name, dest, file_sha;
        uint64_t size = 0;
        size_t npieces = 0;
        std::vector<std::string> piece_sha;
        std::vector<std::string> peers;

        enum Status { Running, Complete, Failed };
        mutable std::mutex m;
        std::vector<char> have;
        size_t completed = 0;
        Status status = Running;
        std::string error;
    };

    void run(std::shared_ptr<Download> d);
    void worker(std::shared_ptr<Download> d, int out_fd, size_t worker_id,
                const std::vector<size_t> *order, std::atomic<size_t> *cursor);

    CompletionHandler on_complete_;
    mutable std::mutex m_;
    std::map<std::string, std::shared_ptr<Download>> downloads_;
};

}  // namespace client

#endif
