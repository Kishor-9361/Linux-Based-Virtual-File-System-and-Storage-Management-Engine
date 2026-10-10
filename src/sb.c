#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sb.h"
#include "log.h"
#include <string.h>
#include <time.h>

/* IEEE 802.3 CRC32 implementation */
uint32_t crc32(const void *p, size_t n) {
    const uint8_t *data = (const uint8_t *)p;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

int sb_compute_layout(uint32_t total_blocks, superblock_t *out) {
    if (!out) return -EINVAL;
    /* Minimum image size is 256 KB (64 blocks of 4096 bytes) */
    if (total_blocks < 64) return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->magic = MINIFS_MAGIC;
    out->version = MINIFS_VERSION;
    out->block_size = BLOCK_SIZE;
    out->total_blocks = total_blocks;

    /* Journal: ~1.5% of disk, clamp(total_blocks / 64, 64, 4096), scaled for small disks */
    uint32_t j = total_blocks / 64;
    if (j < 64) {
        j = total_blocks / 4;
        if (j > 64) j = 64;
        if (j < 16) j = 16;
    } else if (j > 4096) {
        j = 4096;
    }
    out->journal_start = 1;
    out->journal_blocks = j;

    /* Inodes: total_blocks / 4 (min 128), rounded up to multiple of 32 */
    uint32_t icount = total_blocks / 4;
    if (icount < 128) icount = 128;
    if (icount % 32 != 0) {
        icount += (32 - (icount % 32));
    }
    out->total_inodes = icount;

    out->ibitmap_start = out->journal_start + out->journal_blocks;
    out->ibitmap_blocks = (icount + (BLOCK_SIZE * 8) - 1) / (BLOCK_SIZE * 8);

    out->bbitmap_start = out->ibitmap_start + out->ibitmap_blocks;
    out->bbitmap_blocks = (total_blocks + (BLOCK_SIZE * 8) - 1) / (BLOCK_SIZE * 8);

    out->itable_start = out->bbitmap_start + out->bbitmap_blocks;
    out->itable_blocks = icount / 32;

    out->data_start = out->itable_start + out->itable_blocks;
    if (out->data_start >= total_blocks) {
        return -EINVAL;
    }

    out->free_blocks = total_blocks - out->data_start;
    out->free_inodes = icount - 1; /* Inode 1 is root inode */
    out->root_inode = 1;
    out->state = SB_STATE_CLEAN;
    out->mount_count = 0;
    out->last_mount_time = 0;

    return 0;
}

int sb_store(blkdev_t *d, superblock_t *sb) {
    if (!d || !sb) return -EINVAL;

    sb->crc32 = crc32(sb, offsetof(superblock_t, crc32));

    uint8_t blk[BLOCK_SIZE];
    memset(blk, 0, BLOCK_SIZE);
    memcpy(blk, sb, sizeof(*sb));

    return blk_write(d, 0, blk);
}

int sb_load(blkdev_t *d, superblock_t *out) {
    if (!d || !out) return -EINVAL;

    uint8_t blk[BLOCK_SIZE];
    int rc = blk_read(d, 0, blk);
    if (rc != 0) return rc;

    superblock_t sb;
    memcpy(&sb, blk, sizeof(sb));

    /* 1. Magic */
    if (sb.magic != MINIFS_MAGIC) {
        LOG_ERROR("Invalid magic: 0x%08X (expected 0x%08X)", sb.magic, MINIFS_MAGIC);
        return -EINVAL;
    }

    /* 2. Version */
    if (sb.version != MINIFS_VERSION) {
        LOG_ERROR("Unsupported version: %u (expected %u)", sb.version, MINIFS_VERSION);
        return -EINVAL;
    }

    /* 3. Block size */
    if (sb.block_size != BLOCK_SIZE) {
        LOG_ERROR("Unsupported block size: %u (expected %u)", sb.block_size, BLOCK_SIZE);
        return -EINVAL;
    }

    /* 4. CRC32 check */
    uint32_t expected_crc = crc32(&sb, offsetof(superblock_t, crc32));
    if (sb.crc32 != expected_crc) {
        LOG_ERROR("Superblock CRC32 mismatch: 0x%08X != 0x%08X", sb.crc32, expected_crc);
        return -EUCLEAN;
    }

    /* 5. Geometry sanity */
    if (sb.total_blocks < 64 || sb.total_inodes < 32) return -EINVAL;
    if (sb.journal_start != 1 || sb.journal_blocks == 0) return -EINVAL;
    if (sb.ibitmap_start != sb.journal_start + sb.journal_blocks) return -EINVAL;
    if (sb.bbitmap_start != sb.ibitmap_start + sb.ibitmap_blocks) return -EINVAL;
    if (sb.itable_start != sb.bbitmap_start + sb.bbitmap_blocks) return -EINVAL;
    if (sb.data_start != sb.itable_start + sb.itable_blocks) return -EINVAL;
    if (sb.data_start >= sb.total_blocks) return -EINVAL;
    if (sb.root_inode != 1) return -EINVAL;

    memcpy(out, &sb, sizeof(*out));
    return 0;
}

int sb_format(blkdev_t *d) {
    if (!d || !d->is_open) return -EINVAL;

    superblock_t sb;
    int rc = sb_compute_layout(d->nblocks, &sb);
    if (rc != 0) return rc;

    uint8_t zero_blk[BLOCK_SIZE];
    memset(zero_blk, 0, BLOCK_SIZE);

    /* 1. Zero journal blocks */
    for (uint32_t b = sb.journal_start; b < sb.journal_start + sb.journal_blocks; b++) {
        rc = blk_write(d, b, zero_blk);
        if (rc != 0) return rc;
    }

    /* 2. Format inode bitmap: bit 0 (invalid) and bit 1 (root) used */
    for (uint32_t i = 0; i < sb.ibitmap_blocks; i++) {
        uint8_t ibm[BLOCK_SIZE];
        memset(ibm, 0, BLOCK_SIZE);
        if (i == 0) {
            ibm[0] = 0x03; /* Inode 0 and inode 1 used */
        }
        rc = blk_write(d, sb.ibitmap_start + i, ibm);
        if (rc != 0) return rc;
    }

    /* 3. Format block bitmap: mark 0 .. sb.data_start - 1 as used */
    for (uint32_t i = 0; i < sb.bbitmap_blocks; i++) {
        uint8_t bbm[BLOCK_SIZE];
        memset(bbm, 0, BLOCK_SIZE);
        for (uint32_t byte = 0; byte < BLOCK_SIZE; byte++) {
            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t blkno = (i * BLOCK_SIZE * 8) + (byte * 8) + bit;
                if (blkno < sb.data_start) {
                    bbm[byte] |= (uint8_t)(1U << bit);
                }
            }
        }
        rc = blk_write(d, sb.bbitmap_start + i, bbm);
        if (rc != 0) return rc;
    }

    /* 4. Zero inode table blocks */
    for (uint32_t i = 0; i < sb.itable_blocks; i++) {
        rc = blk_write(d, sb.itable_start + i, zero_blk);
        if (rc != 0) return rc;
    }

    /* 5. Store superblock to block 0 */
    rc = sb_store(d, &sb);
    if (rc != 0) return rc;

    return blk_sync(d);
}
