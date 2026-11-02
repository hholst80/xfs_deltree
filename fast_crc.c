/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) && defined(__GNUC__)
#include <nmmintrin.h>
#endif

extern uint32_t __real_crc32c_le(uint32_t, const unsigned char *, size_t);

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("sse4.2")))
static uint32_t hardware_crc(uint32_t crc, const unsigned char *p, size_t n)
{
    uint64_t running = crc;
    while (n >= 8) {
        uint64_t word;
        memcpy(&word, p, sizeof word);
        running = _mm_crc32_u64(running, word);
        p += 8; n -= 8;
    }
    crc = (uint32_t)running;
    while (n--) crc = _mm_crc32_u8(crc, *p++);
    return crc;
}
#endif

/* Same polynomial, seed and result as libfrog, including its non-complemented
 * API. Runtime dispatch retains the portable library path on other CPUs. */
uint32_t __wrap_crc32c_le(uint32_t crc, const unsigned char *p, size_t n)
{
#if defined(__x86_64__) && defined(__GNUC__)
    static int supported = -1;
    if (supported < 0) supported = !!__builtin_cpu_supports("sse4.2");
    if (supported) return hardware_crc(crc, p, n);
#endif
    return __real_crc32c_le(crc, p, n);
}
