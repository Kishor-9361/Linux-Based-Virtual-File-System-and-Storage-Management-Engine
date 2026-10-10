#ifndef MINIFS_BITMAP_H
#define MINIFS_BITMAP_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"
#include "blk.h"
#include "sb.h"
#include "fs.h"

typedef struct {
    uint64_t alloc_calls;
    uint64_t search_steps;
    uint64_t adjacent_allocs;
} allocator_stats_t;

typedef struct {
    blkdev_t *d;
    fs_t     *fs;
    uint32_t start_blk;
    uint32_t nblks;
    uint32_t nbits;
} bitmap_t;

struct fs {
    blkdev_t *dev;
    superblock_t sb;
    bitmap_t ibitmap;
    bitmap_t bbitmap;
    uint32_t last_alloc_blk;
    allocator_stats_t alloc_stats;
    void     *cache;   /* added in M9 */
    void     *journal; /* added in M10 */
};

/* Bitmap low-level APIs */
int      bm_init(bitmap_t *b, blkdev_t *d, uint32_t start_blk, uint32_t nblks, uint32_t nbits);
int      bm_test(bitmap_t *b, uint32_t bit);
int      bm_set(bitmap_t *b, uint32_t bit);
int      bm_clear(bitmap_t *b, uint32_t bit);
int      bm_find_first_zero(bitmap_t *b, uint32_t from);
int      bm_find_run(bitmap_t *b, uint32_t from, uint32_t len);
uint32_t bm_count_used(bitmap_t *b);

/* Allocator high-level APIs */
int  fs_init_allocators(fs_t *fs, blkdev_t *d);
int  alloc_block(fs_t *fs, uint32_t goal);
int  free_block(fs_t *fs, uint32_t blockno);
int  alloc_inode(fs_t *fs);
int  free_inode(fs_t *fs, uint32_t ino);

#endif /* MINIFS_BITMAP_H */
