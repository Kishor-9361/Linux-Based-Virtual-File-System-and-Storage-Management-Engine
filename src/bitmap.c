#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "bitmap.h"
#include "cache.h"
#include "log.h"
#include <string.h>

int fs_read_block(fs_t *fs, uint32_t blockno, void *buf) {
    if (!fs || !fs->dev) return -EINVAL;
    if (fs->cache) {
        buf_t *b = cache_get((cache_t *)fs->cache, blockno);
        if (!b) return -EIO;
        memcpy(buf, b->data, BLOCK_SIZE);
        cache_put((cache_t *)fs->cache, b, 0);
        return 0;
    }
    return blk_read(fs->dev, blockno, buf);
}

int fs_write_block(fs_t *fs, uint32_t blockno, const void *buf) {
    if (!fs || !fs->dev) return -EINVAL;
    if (fs->cache) {
        buf_t *b = cache_get((cache_t *)fs->cache, blockno);
        if (!b) return -EIO;
        memcpy(b->data, buf, BLOCK_SIZE);
        cache_put((cache_t *)fs->cache, b, 1);
        return 0;
    }
    return blk_write(fs->dev, blockno, buf);
}

static inline int bm_io_read(bitmap_t *b, uint32_t blk, void *buf) {
    if (b->fs) return fs_read_block(b->fs, blk, buf);
    return blk_read(b->d, blk, buf);
}

static inline int bm_io_write(bitmap_t *b, uint32_t blk, const void *buf) {
    if (b->fs) return fs_write_block(b->fs, blk, buf);
    return blk_write(b->d, blk, buf);
}

int bm_init(bitmap_t *b, blkdev_t *d, uint32_t start_blk, uint32_t nblks, uint32_t nbits) {
    if (!b || (!d && !b->fs)) return -EINVAL;
    b->d = d;
    b->start_blk = start_blk;
    b->nblks = nblks;
    b->nbits = nbits;
    return 0;
}

int bm_test(bitmap_t *b, uint32_t bit) {
    if (!b || bit >= b->nbits) return -EINVAL;

    uint32_t blk_offset = bit / (BLOCK_SIZE * 8);
    if (blk_offset >= b->nblks) return -EINVAL;

    uint32_t byte_in_blk = (bit / 8) % BLOCK_SIZE;
    uint32_t bit_in_byte = bit % 8;

    uint8_t buf[BLOCK_SIZE];
    int rc = bm_io_read(b, b->start_blk + blk_offset, buf);
    if (rc != 0) return rc;

    return (buf[byte_in_blk] & (1U << bit_in_byte)) ? 1 : 0;
}

int bm_set(bitmap_t *b, uint32_t bit) {
    if (!b || bit >= b->nbits) return -EINVAL;

    uint32_t blk_offset = bit / (BLOCK_SIZE * 8);
    if (blk_offset >= b->nblks) return -EINVAL;

    uint32_t byte_in_blk = (bit / 8) % BLOCK_SIZE;
    uint32_t bit_in_byte = bit % 8;

    uint8_t buf[BLOCK_SIZE];
    int rc = bm_io_read(b, b->start_blk + blk_offset, buf);
    if (rc != 0) return rc;

    if (buf[byte_in_blk] & (1U << bit_in_byte)) {
        LOG_WARN("Double allocation detected on bit %u", bit);
        return -EEXIST;
    }

    buf[byte_in_blk] |= (uint8_t)(1U << bit_in_byte);
    return bm_io_write(b, b->start_blk + blk_offset, buf);
}

int bm_clear(bitmap_t *b, uint32_t bit) {
    if (!b || bit >= b->nbits) return -EINVAL;

    uint32_t blk_offset = bit / (BLOCK_SIZE * 8);
    if (blk_offset >= b->nblks) return -EINVAL;

    uint32_t byte_in_blk = (bit / 8) % BLOCK_SIZE;
    uint32_t bit_in_byte = bit % 8;

    uint8_t buf[BLOCK_SIZE];
    int rc = bm_io_read(b, b->start_blk + blk_offset, buf);
    if (rc != 0) return rc;

    if (!(buf[byte_in_blk] & (1U << bit_in_byte))) {
        LOG_WARN("Double free detected on bit %u", bit);
        return -EINVAL;
    }

    buf[byte_in_blk] &= (uint8_t)~(1U << bit_in_byte);
    return bm_io_write(b, b->start_blk + blk_offset, buf);
}

int bm_find_first_zero(bitmap_t *b, uint32_t from) {
    if (!b || from >= b->nbits) return -ENOSPC;

    uint32_t start_blk_idx = from / (BLOCK_SIZE * 8);
    uint32_t start_bit_in_blk = from % (BLOCK_SIZE * 8);

    for (uint32_t bi = start_blk_idx; bi < b->nblks; bi++) {
        uint8_t blk[BLOCK_SIZE];
        int rc = bm_io_read(b, b->start_blk + bi, blk);
        if (rc != 0) return rc;

        uint64_t *words = (uint64_t *)blk;
        size_t nwords = BLOCK_SIZE / sizeof(uint64_t);
        size_t start_w = (bi == start_blk_idx) ? (start_bit_in_blk / 64) : 0;

        for (size_t w = start_w; w < nwords; w++) {
            uint64_t val = words[w];
            if (val == 0xFFFFFFFFFFFFFFFFULL) {
                continue;
            }

            /* Non-full 64-bit word */
            uint32_t bit_base = (bi * BLOCK_SIZE * 8) + (uint32_t)(w * 64);
            for (int bidx = 0; bidx < 64; bidx++) {
                uint32_t global_bit = bit_base + (uint32_t)bidx;
                if (global_bit < from) continue;
                if (global_bit >= b->nbits) return -ENOSPC;

                if ((val & (1ULL << bidx)) == 0) {
                    return (int)global_bit;
                }
            }
        }
    }

    return -ENOSPC;
}

int bm_find_run(bitmap_t *b, uint32_t from, uint32_t len) {
    if (!b || len == 0 || from >= b->nbits) return -ENOSPC;

    uint32_t run_start = 0;
    uint32_t run_len = 0;

    for (uint32_t bit = from; bit < b->nbits; bit++) {
        int is_set = bm_test(b, bit);
        if (is_set < 0) return is_set;

        if (is_set == 0) {
            if (run_len == 0) {
                run_start = bit;
            }
            run_len++;
            if (run_len == len) {
                return (int)run_start;
            }
        } else {
            run_len = 0;
        }
    }

    return -ENOSPC;
}

uint32_t bm_count_used(bitmap_t *b) {
    if (!b) return 0;
    uint32_t count = 0;

    for (uint32_t bi = 0; bi < b->nblks; bi++) {
        uint8_t blk[BLOCK_SIZE];
        if (bm_io_read(b, b->start_blk + bi, blk) != 0) return 0;

        uint64_t *words = (uint64_t *)blk;
        size_t nwords = BLOCK_SIZE / sizeof(uint64_t);

        for (size_t w = 0; w < nwords; w++) {
            uint32_t bit_base = (bi * BLOCK_SIZE * 8) + (uint32_t)(w * 64);
            if (bit_base >= b->nbits) break;

            if (bit_base + 64 <= b->nbits) {
                count += (uint32_t)__builtin_popcountll(words[w]);
            } else {
                /* Tail partial word */
                uint32_t remaining = b->nbits - bit_base;
                uint64_t mask = (remaining == 64) ? ~0ULL : ((1ULL << remaining) - 1ULL);
                count += (uint32_t)__builtin_popcountll(words[w] & mask);
            }
        }
    }

    return count;
}

int fs_init_allocators(fs_t *fs, blkdev_t *d) {
    if (!fs || !d) return -EINVAL;
    memset(fs, 0, sizeof(*fs));
    fs->dev = d;

    int rc = sb_load(d, &fs->sb);
    if (rc != 0) return rc;

    rc = bm_init(&fs->ibitmap, d, fs->sb.ibitmap_start, fs->sb.ibitmap_blocks, fs->sb.total_inodes);
    if (rc != 0) return rc;
    fs->ibitmap.fs = fs;

    rc = bm_init(&fs->bbitmap, d, fs->sb.bbitmap_start, fs->sb.bbitmap_blocks, fs->sb.total_blocks);
    if (rc != 0) return rc;
    fs->bbitmap.fs = fs;

    fs->last_alloc_blk = fs->sb.data_start;
    return 0;
}

int alloc_block(fs_t *fs, uint32_t goal) {
    if (!fs) return -EINVAL;

    fs->alloc_stats.alloc_calls++;

    if (goal < fs->sb.data_start || goal >= fs->sb.total_blocks) {
        goal = (fs->last_alloc_blk >= fs->sb.data_start && fs->last_alloc_blk < fs->sb.total_blocks)
               ? fs->last_alloc_blk : fs->sb.data_start;
    }

    /* 1. Search from goal to end */
    int res = bm_find_first_zero(&fs->bbitmap, goal);

    /* 2. Wrap-around search from data_start to goal if needed */
    if (res < 0 && goal > fs->sb.data_start) {
        res = bm_find_first_zero(&fs->bbitmap, fs->sb.data_start);
        if (res >= (int)goal) {
            res = -ENOSPC;
        }
    }

    if (res < 0) return -ENOSPC;

    uint32_t blkno = (uint32_t)res;
    int rc = bm_set(&fs->bbitmap, blkno);
    if (rc != 0) return rc;

    if (fs->sb.free_blocks > 0) {
        fs->sb.free_blocks--;
        sb_store(fs->dev, &fs->sb);
    }

    /* Zero allocated block */
    uint8_t zero_blk[BLOCK_SIZE];
    memset(zero_blk, 0, BLOCK_SIZE);
    rc = fs_write_block(fs, blkno, zero_blk);
    if (rc != 0) {
        bm_clear(&fs->bbitmap, blkno);
        fs->sb.free_blocks++;
        sb_store(fs->dev, &fs->sb);
        return rc;
    }

    if (blkno == goal + 1) {
        fs->alloc_stats.adjacent_allocs++;
    }
    fs->alloc_stats.search_steps += (blkno >= goal) ? (blkno - goal) : (fs->sb.total_blocks - goal + blkno);
    fs->last_alloc_blk = blkno;

    return (int)blkno;
}

int free_block(fs_t *fs, uint32_t blockno) {
    if (!fs) return -EINVAL;
    if (blockno < fs->sb.data_start || blockno >= fs->sb.total_blocks) {
        return -EINVAL;
    }

    int rc = bm_clear(&fs->bbitmap, blockno);
    if (rc != 0) return rc;

    fs->sb.free_blocks++;
    return sb_store(fs->dev, &fs->sb);
}

int alloc_inode(fs_t *fs) {
    if (!fs) return -EINVAL;

    /* Inodes start at 1; inode 0 is reserved/invalid */
    int res = bm_find_first_zero(&fs->ibitmap, 1);
    if (res < 0 || (uint32_t)res >= fs->sb.total_inodes) {
        return -ENOSPC;
    }

    uint32_t ino = (uint32_t)res;
    int rc = bm_set(&fs->ibitmap, ino);
    if (rc != 0) return rc;

    if (fs->sb.free_inodes > 0) {
        fs->sb.free_inodes--;
        sb_store(fs->dev, &fs->sb);
    }

    return (int)ino;
}

int free_inode(fs_t *fs, uint32_t ino) {
    if (!fs) return -EINVAL;
    if (ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    int rc = bm_clear(&fs->ibitmap, ino);
    if (rc != 0) return rc;

    fs->sb.free_inodes++;
    return sb_store(fs->dev, &fs->sb);
}
