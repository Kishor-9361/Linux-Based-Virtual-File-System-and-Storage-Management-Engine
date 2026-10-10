#ifndef MINIFS_FSCK_H
#define MINIFS_FSCK_H

#include "common.h"
#include "blk.h"
#include "sb.h"
#include "fs.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool repair;        /* If true, fix inconsistencies */
    bool verbose;       /* Verbose logging output */
} fsck_options_t;

typedef struct {
    uint32_t errors_found;
    uint32_t errors_fixed;
    uint32_t leaked_blocks;
    uint32_t missing_alloc_blocks;
    uint32_t bad_link_counts;
    uint32_t orphan_inodes;
    uint32_t duplicate_blocks;
    uint32_t bad_dentries;
    uint32_t bad_pointers;
    uint32_t sb_errors;
} fsck_stats_t;

/* Run complete fsck (Passes 1-5) on the given disk device */
int fsck_check(blkdev_t *dev, const fsck_options_t *opts, fsck_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* MINIFS_FSCK_H */
