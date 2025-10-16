#include "sha1.h"
#include <cstring>
#include <cstdlib>

static uint32_t rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    uint64_t bitlen = (uint64_t)len * 8ULL;
    size_t padded = ((len + 9) + 63) & ~63;
    uint8_t *buf = (uint8_t*)malloc(padded);
    if(!buf) return;

    memset(buf, 0, padded);
    memcpy(buf, data, len);
    buf[len] = 0x80;

    for(int i = 0; i < 8; i++) {
        buf[padded - 1 - i] = (bitlen >> (8 * i)) & 0xFF;
    }

    for(size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[80];
        for(int i = 0; i < 16; i++) {
            size_t off = chunk + 4 * i;
            w[i] = (buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | (buf[off + 3]);
        }

        for(int i = 16; i < 80; i++) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for(int i = 0; i < 80; i++) {
            uint32_t f, k;
            if(i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if(i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if(i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(buf);
    uint32_t hs[5] = {h0, h1, h2, h3, h4};

    for(int i = 0; i < 5; i++) {
        out[4 * i + 0] = (hs[i] >> 24) & 0xFF;
        out[4 * i + 1] = (hs[i] >> 16) & 0xFF;
        out[4 * i + 2] = (hs[i] >> 8) & 0xFF;
        out[4 * i + 3] = (hs[i] >> 0) & 0xFF;
    }
}

void sha1_hex(const uint8_t *data, size_t len, char outhex[41]) {
    uint8_t out[20];
    sha1(data, len, out);
    static const char hex[] = "0123456789abcdef";

    for(int i = 0; i < 20; i++) {
        outhex[2 * i] = hex[(out[i] >> 4) & 0xF];
        outhex[2 * i + 1] = hex[out[i] & 0xF];
    }
    outhex[40] = '\0';
}