#ifndef SHA1_H
#define SHA1_H

#include <cstdint>
#include <cstddef>

void sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void sha1_hex(const uint8_t *data, size_t len, char outhex[41]);

#endif