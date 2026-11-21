/* Host test for xnuloader/y2s_bounds.h: every write the SM-G985F loader may make, and the refusals. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../xnuloader/y2s_bounds.h"

int
main(void)
{
    const uint64_t LOG = DARWIN_LOG_BLOCK * DARWIN_BS;   /* byte offset of the log slice */

    /* layout sanity: 32 GiB, 16 MiB log at the end, data volume between */
    assert(DARWIN_BLOCKS * DARWIN_BS == 34359738368ULL);
    assert(DARWIN_LOG_BLOCKS == 4096);
    assert(DARWIN_WRITE_FIRST < DARWIN_LOG_BLOCK);

    /* downloads: darwin only */
    assert(y2s_download_ok(1, 0, 5628755968ULL));        /* the system volume at the start */
    assert(!y2s_download_ok(0, 0, 4096));                 /* recovery, boot, super, up_param, userdata ... */
    assert(!y2s_download_ok(1, 0, 0));                    /* nothing downloaded */
    assert(!y2s_download_ok(1, 100, 4096));               /* unaligned */
    assert(y2s_download_ok(1, LOG - 4096, 4096));         /* the last block before the log */
    assert(!y2s_download_ok(1, LOG - 4096, 4097));        /* rounds up into the log */
    assert(!y2s_download_ok(1, LOG, 4096));               /* into the log */
    assert(!y2s_download_ok(1, LOG - 4096, UINT64_MAX));  /* overflow */
    assert(!y2s_download_ok(1, 0, LOG + 1));

    /* the kernel's writes through the loader: data volume only */
    assert(y2s_service_write_ok(DARWIN_WRITE_FIRST, 1));
    assert(!y2s_service_write_ok(DARWIN_WRITE_FIRST - 1, 1));   /* the system volume / cryptexes */
    assert(!y2s_service_write_ok(0, 1));
    assert(y2s_service_write_ok(DARWIN_LOG_BLOCK - 8, 8));
    assert(!y2s_service_write_ok(DARWIN_LOG_BLOCK - 8, 9));    /* into the log */
    assert(!y2s_service_write_ok(DARWIN_LOG_BLOCK, 1));
    assert(!y2s_service_write_ok(DARWIN_WRITE_FIRST, 0));
    assert(!y2s_service_write_ok(DARWIN_WRITE_FIRST, UINT64_MAX));

    /* the log */
    assert(y2s_log_ok(4096, DARWIN_LOG_BLOCK, (8ULL << 20) + 4096));   /* the staged 8 MiB log + its terminator */
    assert(y2s_log_ok(4096, DARWIN_LOG_BLOCK, DARWIN_LOG_BLOCKS * 4096));
    assert(!y2s_log_ok(4096, DARWIN_LOG_BLOCK, DARWIN_LOG_BLOCKS * 4096 + 4096));
    assert(!y2s_log_ok(512, DARWIN_LOG_BLOCK, 4096));
    assert(!y2s_log_ok(4096, DARWIN_LOG_BLOCK - 1, 4096));
    assert(!y2s_log_ok(4096, DARWIN_LOG_BLOCK, 0));

    puts("y2s bounds: downloads, kernel writes and the log stay inside darwin: passed");
    return 0;
}
