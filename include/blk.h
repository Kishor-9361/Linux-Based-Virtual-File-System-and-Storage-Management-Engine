#ifndef MINIFS_BLK_H
#define MINIFS_BLK_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "common.h"

#define FAULT_NONE 0
#define FAULT_DROP 1
#define FAULT_TORN 2

typedef struct {
    uint64_t reads;
    uint64_t writes;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t syncs;
} blk_metrics_t;

typedef struct {
    int fd;
    uint32_t nblocks;
    int is_open;
    int fault_mode;
    int64_t fail_after_n_writes;
    blk_metrics_t metrics;
    char path[1024];
} blkdev_t;

int  blk_open(blkdev_t *d, const char *path, int create, uint32_t nblocks);
int  blk_read(blkdev_t *d, uint32_t blockno, void *buf);
int  blk_write(blkdev_t *d, uint32_t blockno, const void *buf);
int  blk_sync(blkdev_t *d);
void blk_close(blkdev_t *d);

/* Fault injection */
void blk_set_fault(blkdev_t *d, int64_t fail_after_n_writes, int mode);

/* Metrics getter */
blk_metrics_t blk_get_metrics(const blkdev_t *d);

#endif /* MINIFS_BLK_H */
