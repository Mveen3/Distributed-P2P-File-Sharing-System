#include "sha1.h"

#include <openssl/evp.h>

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <stdexcept>
#include <vector>

namespace sha1 {
namespace {

const char kHexDigits[] = "0123456789abcdef";

std::string to_hex(const unsigned char *digest, unsigned int len) {
    std::string out(static_cast<size_t>(len) * 2, '\0');
    for (unsigned int i = 0; i < len; i++) {
        out[2 * i] = kHexDigits[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = kHexDigits[digest[i] & 0xF];
    }
    return out;
}

}  // namespace

std::string hex(const void *data, size_t len) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(data, len, digest, &digest_len, EVP_sha1(), nullptr) != 1)
        throw std::runtime_error("EVP_Digest(SHA1) failed");
    return to_hex(digest, digest_len);
}

Stream::Stream() : ctx_(nullptr), ok_(false) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestInit_ex(SHA1) failed");
    }
    ctx_ = ctx;
    ok_ = true;
}

Stream::~Stream() {
    if (ctx_) EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_));
}

void Stream::update(const void *data, size_t len) {
    if (!ok_ || len == 0) return;
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(ctx_), data, len) != 1) ok_ = false;
}

std::string Stream::digest() {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!ok_ || EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(ctx_), d, &n) != 1)
        throw std::runtime_error("EVP_DigestFinal_ex(SHA1) failed");
    return to_hex(d, n);
}

bool hex_file(const std::string &path, std::string &out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    Stream stream;
    std::vector<unsigned char> buf(1 << 20);
    for (;;) {
        ssize_t r = ::read(fd, buf.data(), buf.size());
        if (r < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (r == 0) break;
        stream.update(buf.data(), static_cast<size_t>(r));
    }
    ::close(fd);

    out = stream.digest();
    return true;
}

bool is_hex(const std::string &s) {
    if (s.size() != HEX_LEN) return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c)) return false;
    return true;
}

}  // namespace sha1
