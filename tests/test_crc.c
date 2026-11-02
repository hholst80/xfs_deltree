/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
extern uint32_t crc32c_le(uint32_t, const unsigned char *, size_t);
extern uint32_t __real_crc32c_le(uint32_t, const unsigned char *, size_t);
int main(void)
{
    unsigned char data[16384];
    uint32_t random = 12345;
    size_t i, offset, length;
    for (i = 0; i < sizeof data; ++i) {
        random = random * 1664525U + 1013904223U;
        data[i] = (unsigned char)(random >> 24);
    }
    if (~crc32c_le(UINT32_MAX, (const unsigned char *)"123456789", 9) != 0xe3069283U)
        return 1;
    for (offset = 0; offset < 16; ++offset)
        for (length = 0; length < 8192; ++length) {
            uint32_t seed = (uint32_t)(offset * 10000 + length);
            if (crc32c_le(seed, data + offset, length) !=
                __real_crc32c_le(seed, data + offset, length)) {
                fprintf(stderr, "CRC mismatch: offset=%zu length=%zu\n", offset, length);
                return 1;
            }
        }
    puts("CRC32C matches portable libfrog for 131072 seed/alignment/length cases");
    return 0;
}
