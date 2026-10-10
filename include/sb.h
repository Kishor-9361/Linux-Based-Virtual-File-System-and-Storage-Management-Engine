#ifndef MINIFS_SB_H
#define MINIFS_SB_H

#include <stdint.h>
#include <stddef.h>
#include "common.h"
#include "blk.h"

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* 0x4D494E49 */
    uint32_t version;           /* 1 */
    uint32_t block_size;        /* 4096 */
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t ibitmap_start;
    uint32_t ibitmap_blocks;
    uint32_t bbitmap_start;
    uint32_t bbitmap_blocks;
    uint32_t itable_start;
    uint32_t itable_blocks;
    uint32_t data_start;
    uint32_t root_inode;        /* 1 */
    uint32_t state;             /* 0 = clean, 1 = mounted/dirty */
    uint32_t mount_count;
    uint64_t last_mount_time;
    uint8_t  reserved[168];     /* Pad to 256 bytes */
    uint32_t crc32;             /* Over all preceding 252 bytes */
} superblock_t;

_Static_assert(sizeof(superblock_t) == 256, "superblock_t must be exactly 256 bytes");

/* CRC32 calculation */
uint32_t crc32(const void *p, size_t n);

/* Superblock APIs */
int sb_compute_layout(uint32_t total_blocks, superblock_t *out);
int sb_format(blkdev_t *d);
int sb_load(blkdev_t *d, superblock_t *out);
int sb_store(blkdev_t *d, superblock_t *sb);

#endif /* MINIFS_SB_H */
