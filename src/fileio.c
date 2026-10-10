#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "fileio.h"
#include "cache.h"
#include "log.h"

static int write_data_block(fs_t *fs, uint32_t pbn, size_t boff, const void *src, size_t n) {
    if (fs->cache) {
        buf_t *b = cache_get((cache_t *)fs->cache, pbn);
        if (!b) return -EIO;
        memcpy(b->data + boff, src, n);
        cache_put((cache_t *)fs->cache, b, 1);
        return 0;
    }
    if (boff == 0 && n == BLOCK_SIZE) {
        return blk_write(fs->dev, pbn, src);
    } else {
        uint8_t blk[BLOCK_SIZE];
        int rc = blk_read(fs->dev, pbn, blk);
        if (rc != 0) return rc;
        memcpy(blk + boff, src, n);
        return blk_write(fs->dev, pbn, blk);
    }
}
#include <string.h>
#include <time.h>

ssize_t file_read(fs_t *fs, uint32_t ino, uint64_t off, void *buf, size_t len) {
    if (!fs || !buf || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, ino, &node);
    if (rc != 0) return rc;

    if (off >= node.size || len == 0) {
        return 0;
    }

    if (off + (uint64_t)len > node.size) {
        len = (size_t)(node.size - off);
    }

    uint64_t pos = off;
    size_t remaining = len;
    size_t total_read = 0;

    while (remaining > 0) {
        uint32_t lbn = (uint32_t)(pos / BLOCK_SIZE);
        size_t boff = (size_t)(pos % BLOCK_SIZE);
        size_t n = (BLOCK_SIZE - boff < remaining) ? (BLOCK_SIZE - boff) : remaining;

        uint32_t pbn = 0;
        rc = bmap(fs, &node, lbn, 0, &pbn);
        if (rc != 0) return rc;

        if (pbn == 0) {
            /* Sparse hole: return zeros */
            memset((char *)buf + total_read, 0, n);
        } else {
            uint8_t blk[BLOCK_SIZE];
            rc = fs_read_block(fs, pbn, blk);
            if (rc != 0) return rc;
            memcpy((char *)buf + total_read, blk + boff, n);
        }

        pos += n;
        remaining -= n;
        total_read += n;
    }

    uint32_t now = (uint32_t)time(NULL);
    if (node.atime != now) {
        node.atime = now;
        inode_write(fs, ino, &node);
    }

    return (ssize_t)total_read;
}

ssize_t file_write(fs_t *fs, uint32_t ino, uint64_t off, const void *buf, size_t len) {
    if (!fs || !buf || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    if (len == 0) return 0;

    inode_t node;
    int rc = inode_read(fs, ino, &node);
    if (rc != 0) return rc;

    if (off + (uint64_t)len > (uint64_t)MAX_FILE_BLOCKS * BLOCK_SIZE) {
        return -EFBIG;
    }

    uint64_t pos = off;
    size_t remaining = len;
    size_t total_written = 0;

    while (remaining > 0) {
        uint32_t lbn = (uint32_t)(pos / BLOCK_SIZE);
        size_t boff = (size_t)(pos % BLOCK_SIZE);
        size_t n = (BLOCK_SIZE - boff < remaining) ? (BLOCK_SIZE - boff) : remaining;

        uint32_t pbn = 0;
        rc = bmap(fs, &node, lbn, 1, &pbn);
        if (rc < 0) {
            if (total_written > 0) {
                break; /* Partial write */
            }
            return rc;
        }

        int is_dir = (node.mode & S_IFDIR);
        if (is_dir) {
            if (boff == 0 && n == BLOCK_SIZE) {
                rc = fs_write_block(fs, pbn, (const char *)buf + total_written);
                if (rc != 0) {
                    if (total_written > 0) break;
                    return rc;
                }
            } else {
                uint8_t blk[BLOCK_SIZE];
                rc = fs_read_block(fs, pbn, blk);
                if (rc != 0) {
                    if (total_written > 0) break;
                    return rc;
                }
                memcpy(blk + boff, (const char *)buf + total_written, n);
                rc = fs_write_block(fs, pbn, blk);
                if (rc != 0) {
                    if (total_written > 0) break;
                    return rc;
                }
            }
        } else {
            rc = write_data_block(fs, pbn, boff, (const char *)buf + total_written, n);
            if (rc != 0) {
                if (total_written > 0) break;
                return rc;
            }
        }

        pos += n;
        remaining -= n;
        total_written += n;
    }

    if (pos > node.size) {
        node.size = pos;
    }

    inode_touch(&node, 0, 1, 1);
    rc = inode_write(fs, ino, &node);
    if (rc != 0 && total_written == 0) return rc;

    return (ssize_t)total_written;
}

int file_truncate(fs_t *fs, uint32_t ino, uint64_t newsize) {
    if (!fs || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, ino, &node);
    if (rc != 0) return rc;

    if (newsize == node.size) {
        return 0;
    }

    if (newsize > node.size) {
        /* Truncate grow: sparse file */
        node.size = newsize;
        inode_touch(&node, 0, 1, 1);
        return inode_write(fs, ino, &node);
    }

    /* Truncate shrink */
    uint32_t last_lbn = (uint32_t)(newsize / BLOCK_SIZE);
    size_t tail_off = (size_t)(newsize % BLOCK_SIZE);
    uint32_t free_start_lbn = last_lbn;

    if (tail_off > 0) {
        /* Zero the remainder of the last partial block */
        uint32_t pbn = 0;
        rc = bmap(fs, &node, last_lbn, 0, &pbn);
        if (rc == 0 && pbn != 0) {
            uint8_t blk[BLOCK_SIZE];
            rc = fs_read_block(fs, pbn, blk);
            if (rc == 0) {
                memset(blk + tail_off, 0, BLOCK_SIZE - tail_off);
                fs_write_block(fs, pbn, blk);
            }
        }
        free_start_lbn = last_lbn + 1;
    }

    rc = inode_free_blocks_from(fs, &node, free_start_lbn);
    if (rc != 0) return rc;

    node.size = newsize;
    inode_touch(&node, 0, 1, 1);
    return inode_write(fs, ino, &node);
}
