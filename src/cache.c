#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "cache.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static inline uint32_t hash_func(uint32_t blk, uint32_t hash_size) {
    return (blk * 2654435761u) & (hash_size - 1);
}

static void lru_remove(cache_t *c, buf_t *b) {
    if (b->lru_prev) {
        b->lru_prev->lru_next = b->lru_next;
    } else {
        c->lru_head = b->lru_next;
    }
    if (b->lru_next) {
        b->lru_next->lru_prev = b->lru_prev;
    } else {
        c->lru_tail = b->lru_prev;
    }
    b->lru_prev = NULL;
    b->lru_next = NULL;
}

static void lru_push_head(cache_t *c, buf_t *b) {
    b->lru_prev = NULL;
    b->lru_next = c->lru_head;
    if (c->lru_head) {
        c->lru_head->lru_prev = b;
    }
    c->lru_head = b;
    if (!c->lru_tail) {
        c->lru_tail = b;
    }
}

static void lru_touch(cache_t *c, buf_t *b) {
    if (c->lru_head == b) return;
    lru_remove(c, b);
    lru_push_head(c, b);
}

static void hash_insert(cache_t *c, buf_t *b) {
    uint32_t bucket = hash_func(b->blockno, c->hash_size);
    b->hash_next = c->hash_table[bucket];
    b->hash_prev = NULL;
    if (c->hash_table[bucket]) {
        c->hash_table[bucket]->hash_prev = b;
    }
    c->hash_table[bucket] = b;
}

static void hash_remove(cache_t *c, buf_t *b) {
    uint32_t bucket = hash_func(b->blockno, c->hash_size);
    if (b->hash_prev) {
        b->hash_prev->hash_next = b->hash_next;
    } else {
        c->hash_table[bucket] = b->hash_next;
    }
    if (b->hash_next) {
        b->hash_next->hash_prev = b->hash_prev;
    }
    b->hash_prev = NULL;
    b->hash_next = NULL;
}

static buf_t *hash_lookup(cache_t *c, uint32_t blk) {
    uint32_t bucket = hash_func(blk, c->hash_size);
    buf_t *curr = c->hash_table[bucket];
    while (curr) {
        if (curr->valid && curr->blockno == blk) {
            return curr;
        }
        curr = curr->hash_next;
    }
    return NULL;
}

static int compare_buf_ptrs(const void *a, const void *b) {
    const buf_t *b1 = *(const buf_t * const *)a;
    const buf_t *b2 = *(const buf_t * const *)b;
    if (b1->blockno < b2->blockno) return -1;
    if (b1->blockno > b2->blockno) return 1;
    return 0;
}

cache_t *cache_create(blkdev_t *dev, uint32_t num_buffers, int write_policy) {
    if (!dev || num_buffers == 0) return NULL;

    cache_t *c = (cache_t *)calloc(1, sizeof(cache_t));
    if (!c) return NULL;

    c->dev = dev;
    c->num_buffers = num_buffers;
    c->write_policy = write_policy;

    /* Compute hash table size as power of 2 >= 2 * num_buffers */
    uint32_t hsize = 16;
    while (hsize < num_buffers * 2) {
        hsize <<= 1;
    }
    c->hash_size = hsize;

    c->hash_table = (buf_t **)calloc(c->hash_size, sizeof(buf_t *));
    if (!c->hash_table) {
        free(c);
        return NULL;
    }

    c->buffers = (buf_t *)calloc(c->num_buffers, sizeof(buf_t));
    if (!c->buffers) {
        free(c->hash_table);
        free(c);
        return NULL;
    }

    /* Initialize LRU list */
    for (uint32_t i = 0; i < c->num_buffers; i++) {
        buf_t *b = &c->buffers[i];
        b->valid = 0;
        b->dirty = 0;
        b->refcnt = 0;
        if (i == 0) {
            c->lru_head = b;
        } else {
            b->lru_prev = &c->buffers[i - 1];
            c->buffers[i - 1].lru_next = b;
        }
        if (i == c->num_buffers - 1) {
            c->lru_tail = b;
        }
    }

    pthread_mutex_init(&c->lock, NULL);
    return c;
}

void cache_destroy(cache_t *c) {
    if (!c) return;
    cache_flush(c);

    pthread_mutex_destroy(&c->lock);
    free(c->buffers);
    free(c->hash_table);
    free(c);
}

buf_t *cache_get(cache_t *c, uint32_t blk) {
    if (!c) return NULL;

    pthread_mutex_lock(&c->lock);

    /* 1. Check if already present */
    buf_t *b = hash_lookup(c, blk);
    if (b) {
        c->stats.hits++;
        b->refcnt++;
        lru_touch(c, b);

        /* Optional read-ahead check */
        int should_read_ahead = c->read_ahead_enabled && (blk == c->last_blk + 1);
        c->last_blk = blk;
        pthread_mutex_unlock(&c->lock);

        if (should_read_ahead && blk + 1 < c->dev->nblocks) {
            /* Prefetch blk + 1 */
            buf_t *ra = cache_get(c, blk + 1);
            if (ra) {
                cache_put(c, ra, 0);
                pthread_mutex_lock(&c->lock);
                c->stats.read_aheads++;
                pthread_mutex_unlock(&c->lock);
            }
        }
        return b;
    }

    /* 2. Cache miss: find unpinned victim from LRU tail */
    c->stats.misses++;
    buf_t *victim = NULL;
    for (buf_t *curr = c->lru_tail; curr != NULL; curr = curr->lru_prev) {
        if (curr->refcnt == 0) {
            victim = curr;
            break;
        }
    }

    if (!victim) {
        /* All buffers are pinned */
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    /* 3. Evict existing content if valid */
    if (victim->valid) {
        if (victim->dirty) {
            int rc = blk_write(c->dev, victim->blockno, victim->data);
            if (rc != 0) {
                LOG_ERROR("Failed to writeback dirty block %u on eviction: %d", victim->blockno, rc);
            }
            victim->dirty = 0;
            c->stats.writebacks++;
        }
        hash_remove(c, victim);
        c->stats.evictions++;
    }

    /* 4. Read target block from disk */
    int rc = blk_read(c->dev, blk, victim->data);
    if (rc != 0) {
        LOG_ERROR("Failed to read block %u: %d", blk, rc);
        victim->valid = 0;
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }

    /* 5. Initialize buffer */
    victim->blockno = blk;
    victim->valid = 1;
    victim->dirty = 0;
    victim->refcnt = 1;

    hash_insert(c, victim);
    lru_touch(c, victim);

    int should_read_ahead = c->read_ahead_enabled && (blk == c->last_blk + 1);
    c->last_blk = blk;
    pthread_mutex_unlock(&c->lock);

    if (should_read_ahead && blk + 1 < c->dev->nblocks) {
        buf_t *ra = cache_get(c, blk + 1);
        if (ra) {
            cache_put(c, ra, 0);
            pthread_mutex_lock(&c->lock);
            c->stats.read_aheads++;
            pthread_mutex_unlock(&c->lock);
        }
    }

    return victim;
}

void cache_put(cache_t *c, buf_t *b, int dirty) {
    if (!c || !b) return;

    pthread_mutex_lock(&c->lock);

    if (dirty) {
        b->dirty = 1;
        if (c->write_policy == CACHE_WRITE_THROUGH) {
            int rc = blk_write(c->dev, b->blockno, b->data);
            if (rc != 0) {
                LOG_ERROR("Failed write-through for block %u: %d", b->blockno, rc);
            }
            b->dirty = 0;
            c->stats.writebacks++;
        }
    }

    if (b->refcnt > 0) {
        b->refcnt--;
    }

    pthread_mutex_unlock(&c->lock);
}

int cache_flush(cache_t *c) {
    if (!c) return -EINVAL;

    pthread_mutex_lock(&c->lock);

    /* Collect all dirty buffers */
    uint32_t dirty_count = 0;
    for (uint32_t i = 0; i < c->num_buffers; i++) {
        if (c->buffers[i].valid && c->buffers[i].dirty) {
            dirty_count++;
        }
    }

    if (dirty_count > 0) {
        buf_t **dirty_array = (buf_t **)malloc(dirty_count * sizeof(buf_t *));
        if (dirty_array) {
            uint32_t idx = 0;
            for (uint32_t i = 0; i < c->num_buffers; i++) {
                if (c->buffers[i].valid && c->buffers[i].dirty) {
                    dirty_array[idx++] = &c->buffers[i];
                }
            }

            /* Sort by blockno for disk locality / elevator ordering */
            qsort(dirty_array, dirty_count, sizeof(buf_t *), compare_buf_ptrs);

            for (uint32_t i = 0; i < dirty_count; i++) {
                buf_t *b = dirty_array[i];
                int rc = blk_write(c->dev, b->blockno, b->data);
                if (rc != 0) {
                    LOG_ERROR("cache_flush: failed writing block %u: %d", b->blockno, rc);
                }
                b->dirty = 0;
                c->stats.writebacks++;
            }
            free(dirty_array);
        } else {
            /* Fallback if allocation failed: write unsorted */
            for (uint32_t i = 0; i < c->num_buffers; i++) {
                buf_t *b = &c->buffers[i];
                if (b->valid && b->dirty) {
                    blk_write(c->dev, b->blockno, b->data);
                    b->dirty = 0;
                    c->stats.writebacks++;
                }
            }
        }
    }

    int rc = blk_sync(c->dev);
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int cache_flush_block(cache_t *c, uint32_t blk) {
    if (!c) return -EINVAL;

    pthread_mutex_lock(&c->lock);
    buf_t *b = hash_lookup(c, blk);
    int rc = 0;
    if (b && b->valid && b->dirty) {
        rc = blk_write(c->dev, b->blockno, b->data);
        b->dirty = 0;
        c->stats.writebacks++;
    }
    if (rc == 0) {
        rc = blk_sync(c->dev);
    }
    pthread_mutex_unlock(&c->lock);
    return rc;
}

int cache_invalidate_all(cache_t *c) {
    if (!c) return -EINVAL;

    pthread_mutex_lock(&c->lock);

    memset(c->hash_table, 0, c->hash_size * sizeof(buf_t *));

    for (uint32_t i = 0; i < c->num_buffers; i++) {
        buf_t *b = &c->buffers[i];
        b->valid = 0;
        b->dirty = 0;
        b->refcnt = 0;
        b->hash_prev = NULL;
        b->hash_next = NULL;
        if (i == 0) {
            c->lru_head = b;
            b->lru_prev = NULL;
        } else {
            b->lru_prev = &c->buffers[i - 1];
            c->buffers[i - 1].lru_next = b;
        }
        if (i == c->num_buffers - 1) {
            c->lru_tail = b;
            b->lru_next = NULL;
        }
    }

    pthread_mutex_unlock(&c->lock);
    return 0;
}

int cache_invalidate_block(cache_t *c, uint32_t blk) {
    if (!c) return -EINVAL;

    pthread_mutex_lock(&c->lock);
    buf_t *b = hash_lookup(c, blk);
    if (b) {
        hash_remove(c, b);
        b->valid = 0;
        b->dirty = 0;
        b->refcnt = 0;
        /* Move to LRU tail for early reuse */
        lru_remove(c, b);
        if (c->lru_tail) {
            c->lru_tail->lru_next = b;
            b->lru_prev = c->lru_tail;
            b->lru_next = NULL;
            c->lru_tail = b;
        } else {
            c->lru_head = b;
            c->lru_tail = b;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return 0;
}

cache_stats_t cache_stats(cache_t *c) {
    cache_stats_t s = {0, 0, 0, 0, 0};
    if (!c) return s;

    pthread_mutex_lock(&c->lock);
    s = c->stats;
    pthread_mutex_unlock(&c->lock);
    return s;
}

void cache_reset_stats(cache_t *c) {
    if (!c) return;

    pthread_mutex_lock(&c->lock);
    memset(&c->stats, 0, sizeof(c->stats));
    pthread_mutex_unlock(&c->lock);
}

void cache_set_write_policy(cache_t *c, int write_policy) {
    if (!c) return;

    pthread_mutex_lock(&c->lock);
    c->write_policy = write_policy;
    pthread_mutex_unlock(&c->lock);
}

void cache_set_read_ahead(cache_t *c, int enabled) {
    if (!c) return;

    pthread_mutex_lock(&c->lock);
    c->read_ahead_enabled = enabled;
    pthread_mutex_unlock(&c->lock);
}
