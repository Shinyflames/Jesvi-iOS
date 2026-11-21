/* MIT. Galaxy S20+ LTE (SM-G985F, y2s): where the loader may write, as pure arithmetic.
 *
 * On this phone super is Android 16, userdata Android's data, y2s-root Ubuntu and recovery TWRP; the loader
 * writes into darwin (GPT entry 34) and nowhere else. All quantities are darwin's blocks of 4096 bytes or
 * byte offsets inside darwin. darwin's identity (GUIDs, LBAs) is checked separately (loader.c y2s_darwin_bio).
 *
 *   blocks 0 .. DARWIN_WRITE_FIRST-1          system volume + cryptexes (written from the computer over USB)
 *   blocks DARWIN_WRITE_FIRST .. LOG_BLOCK-1  iOS's data volume (the kernel's writes)
 *   blocks DARWIN_LOG_BLOCK .. DARWIN_BLOCKS-1 the loader's log (16 MiB)
 *
 * Tested on the host by tools/tests/y2s_bounds_test.c (ci/check.sh).
 */
#ifndef XNULOADER_Y2S_BOUNDS_H
#define XNULOADER_Y2S_BOUNDS_H
#include <stdint.h>

#define DARWIN_BLOCKS       8388608ULL    /* 32 GiB */
#define DARWIN_WRITE_FIRST  2042368ULL    /* past the system volume and both cryptexes */
#define DARWIN_LOG_BLOCK    8384512ULL    /* the log slice, to darwin's end */
#define DARWIN_LOG_BLOCKS   (DARWIN_BLOCKS - DARWIN_LOG_BLOCK)   /* 4096 blocks = 16 MiB */
#define DARWIN_BS           4096ULL

/* A USB download of len bytes at byte offset off: darwin only, block-aligned, entirely before the log slice. */
static inline int
y2s_download_ok(int is_darwin, uint64_t off, uint64_t len)
{
    uint64_t limit = DARWIN_LOG_BLOCK * DARWIN_BS, rounded;

    if (!is_darwin || len == 0 || (off % DARWIN_BS) != 0 || off >= limit) return 0;
    if (len > limit) return 0;                              /* (keeps the rounding below from overflowing) */
    rounded = (len + DARWIN_BS - 1) / DARWIN_BS * DARWIN_BS;
    return rounded <= limit - off;
}

/* A write the kernel asks the loader to serve: count blocks from lba, only in the data volume. */
static inline int
y2s_service_write_ok(uint64_t lba, uint64_t count)
{
    return count != 0 && lba >= DARWIN_WRITE_FIRST && lba < DARWIN_LOG_BLOCK && count <= DARWIN_LOG_BLOCK - lba;
}

/* The log: block size bs, written at block lba, len bytes (already a whole number of blocks). */
static inline int
y2s_log_ok(uint64_t bs, uint64_t lba, uint64_t len)
{
    return bs == DARWIN_BS && lba == DARWIN_LOG_BLOCK && len != 0 && len <= DARWIN_LOG_BLOCKS * DARWIN_BS;
}
#endif
