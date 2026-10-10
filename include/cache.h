#ifndef MINIFS_CACHE_H
#define MINIFS_CACHE_H

#include <stdint.h>
#include <pthread.h>
#include "blk.h"
#include "common.h"

#define DEFAULT_CACHE_SIZE 1024
#define CACHE_WRITE_BACK    0
#define CACHE_WRITE_THROUGH 1

typedef struct buf {
    uint32_t    blockno;
    uint8_t     data[BLOCK_SIZE];
    int         dirty;
    int         refcnt;    /* 0 = unpinned, >0 = pinned */
    int         valid;
    struct buf *lru_prev;
    struct buf *lru_next;
    struct buf *hash_prev;
    struct buf *hash_next;
} buf_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
    uint64_t read_aheads;
} cache_stats_t;

typedef struct cache {
    blkdev_t       *dev;
    uint32_t        num_buffers;
    int             write_policy;
    buf_t          *buffers;
    buf_t          *lru_head;  /* Most Recently Used (MRU) */
    buf_t          *lru_tail;  /* Least Recently Used (LRU) */
    buf_t         **hash_table;
    uint32_t        hash_size;
    pthread_mutex_t lock;
    cache_stats_t   stats;
    int             read_ahead_enabled;
    uint32_t        last_blk;
} cache_t;

/* Cache Lifecycle */
cache_t       *cache_create(blkdev_t *dev, uint32_t num_buffers, int write_policy);
void           cache_destroy(cache_t *c);

/* Buffer Access */
buf_t         *cache_get(cache_t *c, uint32_t blk);
void           cache_put(cache_t *c, buf_t *b, int dirty);

/* Flushing & Invalidation */
int            cache_flush(cache_t *c);
int            cache_flush_block(cache_t *c, uint32_t blk);
int            cache_invalidate_all(cache_t *c);
int            cache_invalidate_block(cache_t *c, uint32_t blk);

/* Introspection & Policy */
cache_stats_t  cache_stats(cache_t *c);
void           cache_reset_stats(cache_t *c);
void           cache_set_write_policy(cache_t *c, int write_policy);
void           cache_set_read_ahead(cache_t *c, int enabled);

#endif /* MINIFS_CACHE_H */
