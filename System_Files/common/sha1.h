#ifndef P2P_SHA1_H
#define P2P_SHA1_H

#include <cstddef>
#include <string>

// SHA1 helpers backed by OpenSSL's EVP interface.
//
// Digests are handled everywhere as 40 character lowercase hex strings: that is
// what travels over the wire and what the tracker writes to disk, so keeping a
// single representation removes a whole class of conversion bugs.
namespace sha1 {

const size_t HEX_LEN = 40;

// Digest of a buffer that is already in memory. Used for individual pieces,
// which are at most PIECE_SIZE (512 KB) bytes.
std::string hex(const void *data, size_t len);

// Incremental hasher, so whole-file digests never require holding the file in
// memory. digest() finalises the context; the object is single use.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    void update(const void *data, size_t len);
    std::string digest();

private:
    void *ctx_;  // EVP_MD_CTX*, kept opaque so OpenSSL stays out of the headers
    bool ok_;
};

// Streams an entire file through Stream(). Returns false if the file cannot be
// opened or read to the end.
bool hex_file(const std::string &path, std::string &out);

// True when s is exactly HEX_LEN hex characters.
bool is_hex(const std::string &s);

}  // namespace sha1

#endif
