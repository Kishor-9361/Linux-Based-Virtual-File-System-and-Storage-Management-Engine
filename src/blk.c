#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "blk.h"
#include "log.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

int blk_open(blkdev_t *d, const char *path, int create, uint32_t nblocks) {
    if (!d || !path) return -EINVAL;

    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->fault_mode = FAULT_NONE;
    d->fail_after_n_writes = -1;
    strncpy(d->path, path, sizeof(d->path) - 1);

    if (create) {
        if (nblocks == 0) return -EINVAL;
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -errno;

        off_t total_size = (off_t)nblocks * (off_t)BLOCK_SIZE;
        if (ftruncate(fd, total_size) != 0) {
            int err = -errno;
            close(fd);
            return err;
        }

        d->fd = fd;
        d->nblocks = nblocks;
        d->is_open = 1;
        return 0;
    } else {
        int fd = open(path, O_RDWR);
        if (fd < 0) return -errno;

        struct stat st;
        if (fstat(fd, &st) != 0) {
            int err = -errno;
            close(fd);
            return err;
        }

        uint32_t disk_blocks = (uint32_t)(st.st_size / (off_t)BLOCK_SIZE);
        if (nblocks > 0 && disk_blocks < nblocks) {
            close(fd);
            return -EINVAL;
        }

        d->fd = fd;
        d->nblocks = (nblocks > 0) ? nblocks : disk_blocks;
        d->is_open = 1;
        return 0;
    }
}

int blk_read(blkdev_t *d, uint32_t blockno, void *buf) {
    if (!d || !d->is_open || !buf) return -EINVAL;
    if (blockno >= d->nblocks) return -EINVAL;

    off_t offset = (off_t)blockno * (off_t)BLOCK_SIZE;
    size_t total = 0;

    while (total < BLOCK_SIZE) {
        ssize_t n = pread(d->fd, (char *)buf + total, BLOCK_SIZE - total, offset + (off_t)total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) {
            /* Sparse hole or EOF: zero remaining bytes */
            memset((char *)buf + total, 0, BLOCK_SIZE - total);
            break;
        }
        total += (size_t)n;
    }

    d->metrics.reads++;
    d->metrics.bytes_read += BLOCK_SIZE;
    return 0;
}

int blk_write(blkdev_t *d, uint32_t blockno, const void *buf) {
    if (!d || !d->is_open || !buf) return -EINVAL;
    if (blockno >= d->nblocks) return -EINVAL;

    d->metrics.writes++;

    /* Fault injection logic */
    if (d->fault_mode != FAULT_NONE && d->fail_after_n_writes >= 0) {
        if (d->fail_after_n_writes == 0) {
            if (d->fault_mode == FAULT_TORN) {
                /* Write only first half (2048 bytes), then drop all subsequent writes */
                off_t offset = (off_t)blockno * (off_t)BLOCK_SIZE;
                size_t half = BLOCK_SIZE / 2;
                size_t total = 0;
                while (total < half) {
                    ssize_t n = pwrite(d->fd, (const char *)buf + total, half - total, offset + (off_t)total);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return -errno;
                    }
                    total += (size_t)n;
                }
                d->metrics.bytes_written += half;
                d->fault_mode = FAULT_DROP;
                d->fail_after_n_writes = 0;
                return 0;
            } else if (d->fault_mode == FAULT_DROP) {
                /* Silently drop the write */
                return 0;
            }
        } else {
            d->fail_after_n_writes--;
        }
    }

    off_t offset = (off_t)blockno * (off_t)BLOCK_SIZE;
    size_t total = 0;

    while (total < BLOCK_SIZE) {
        ssize_t n = pwrite(d->fd, (const char *)buf + total, BLOCK_SIZE - total, offset + (off_t)total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        total += (size_t)n;
    }

    d->metrics.bytes_written += BLOCK_SIZE;
    return 0;
}

int blk_sync(blkdev_t *d) {
    if (!d || !d->is_open) return -EINVAL;
    d->metrics.syncs++;
    if (fsync(d->fd) != 0) {
        return -errno;
    }
    return 0;
}

void blk_close(blkdev_t *d) {
    if (!d || !d->is_open) return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    d->is_open = 0;
}

void blk_set_fault(blkdev_t *d, int64_t fail_after_n_writes, int mode) {
    if (!d) return;
    d->fail_after_n_writes = fail_after_n_writes;
    d->fault_mode = mode;
}

blk_metrics_t blk_get_metrics(const blkdev_t *d) {
    if (!d) {
        blk_metrics_t empty = {0, 0, 0, 0, 0};
        return empty;
    }
    return d->metrics;
}
