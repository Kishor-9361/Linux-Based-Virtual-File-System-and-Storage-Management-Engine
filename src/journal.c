#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "journal.h"
#include "cache.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static uint32_t crc32_combine(uint32_t prev_crc, const void *p, size_t n) {
    const uint8_t *data = (const uint8_t *)p;
    uint32_t crc = ~prev_crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

journal_t *journal_init(fs_t *fs) {
    if (!fs || !fs->dev) return NULL;
    if (fs->sb.journal_blocks < 2) return NULL;

    journal_t *j = (journal_t *)calloc(1, sizeof(journal_t));
    if (!j) return NULL;

    j->fs = fs;
    j->journal_start = fs->sb.journal_start;
    j->log_start = fs->sb.journal_start + 1;
    j->log_blocks = fs->sb.journal_blocks - 1;

    pthread_mutex_init(&j->lock, NULL);

    /* Read or format journal superblock */
    uint8_t sb_buf[BLOCK_SIZE];
    int rc = blk_read(fs->dev, j->journal_start, sb_buf);
    journal_sb_t *jsb = (journal_sb_t *)sb_buf;

    uint32_t expected_crc = 0;
    if (rc == 0 && jsb->magic == JRNL_MAGIC) {
        expected_crc = crc32(jsb, offsetof(journal_sb_t, crc32));
    }

    if (rc == 0 && jsb->magic == JRNL_MAGIC && jsb->crc32 == expected_crc) {
        j->head_seq = jsb->head_seq;
        j->head_pos = jsb->start_block % j->log_blocks;
        j->tail_pos = j->head_pos;
    } else {
        /* Format new journal */
        memset(sb_buf, 0, BLOCK_SIZE);
        jsb->magic = JRNL_MAGIC;
        jsb->block_size = BLOCK_SIZE;
        jsb->head_seq = 1;
        jsb->start_block = 0;
        jsb->log_blocks = j->log_blocks;
        jsb->crc32 = crc32(jsb, offsetof(journal_sb_t, crc32));

        blk_write(fs->dev, j->journal_start, sb_buf);
        blk_sync(fs->dev);

        j->head_seq = 1;
        j->head_pos = 0;
        j->tail_pos = 0;
    }

    fs->journal = j;
    return j;
}

void journal_destroy(journal_t *j) {
    if (!j) return;
    if (j->fs && j->fs->journal == j) {
        j->fs->journal = NULL;
    }
    pthread_mutex_destroy(&j->lock);
    free(j);
}

tx_t *tx_begin(fs_t *fs) {
    if (!fs || !fs->journal) return NULL;
    journal_t *j = (journal_t *)fs->journal;

    pthread_mutex_lock(&j->lock);

    if (fs->current_tx != NULL) {
        /* Transactions do not nest concurrently on single fs context */
        pthread_mutex_unlock(&j->lock);
        return NULL;
    }

    tx_t *t = (tx_t *)calloc(1, sizeof(tx_t));
    if (!t) {
        pthread_mutex_unlock(&j->lock);
        return NULL;
    }

    t->j = j;
    t->fs = fs;
    t->seq = j->head_seq;
    t->count = 0;
    t->active = 1;

    fs->current_tx = t;
    j->metrics.tx_begun++;

    pthread_mutex_unlock(&j->lock);
    return t;
}

int tx_write(tx_t *t, uint32_t blockno, const void *data) {
    if (!t || !t->active || !data) return -EINVAL;

    /* Check for deduplication: if block was already written in this transaction */
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->target_blockno[i] == blockno) {
            memcpy(t->data[i], data, BLOCK_SIZE);
            return 0;
        }
    }

    if (t->count >= JRNL_MAX_TX_BLOCKS) {
        LOG_ERROR("Transaction block capacity exceeded (%u blocks)", JRNL_MAX_TX_BLOCKS);
        return -ENOSPC;
    }

    t->target_blockno[t->count] = blockno;
    memcpy(t->data[t->count], data, BLOCK_SIZE);
    t->count++;

    return 0;
}

int journal_checkpoint(fs_t *fs) {
    if (!fs || !fs->journal) return -EINVAL;
    journal_t *j = (journal_t *)fs->journal;

    /* Step 1: Flush all dirty buffers to home locations */
    if (fs->cache) {
        cache_flush((cache_t *)fs->cache);
    }
    blk_sync(fs->dev);

    /* Step 2: Advance start_block to head_pos */
    j->tail_pos = j->head_pos;

    uint8_t sb_buf[BLOCK_SIZE];
    memset(sb_buf, 0, BLOCK_SIZE);
    journal_sb_t *jsb = (journal_sb_t *)sb_buf;
    jsb->magic = JRNL_MAGIC;
    jsb->block_size = BLOCK_SIZE;
    jsb->head_seq = j->head_seq;
    jsb->start_block = j->tail_pos;
    jsb->log_blocks = j->log_blocks;
    jsb->crc32 = crc32(jsb, offsetof(journal_sb_t, crc32));

    blk_write(fs->dev, j->journal_start, sb_buf);
    blk_sync(fs->dev);

    j->metrics.checkpoints++;
    return 0;
}

int tx_commit(tx_t *t) {
    if (!t || !t->active) return -EINVAL;
    journal_t *j = t->j;
    fs_t *fs = t->fs;

    pthread_mutex_lock(&j->lock);

    /* If transaction did not touch any metadata blocks, commit is a no-op */
    if (t->count == 0) {
        fs->current_tx = NULL;
        j->metrics.tx_committed++;
        free(t);
        pthread_mutex_unlock(&j->lock);
        return 0;
    }

    /* 1. Ordered Data Mode: ensure regular file data blocks are written & synced */
    blk_sync(fs->dev);

    /* Check log space: transaction requires (1 descriptor + count data + 1 commit) */
    uint32_t needed_blocks = t->count + 2;
    if (needed_blocks >= j->log_blocks) {
        pthread_mutex_unlock(&j->lock);
        return -ENOSPC;
    }

    /* 2. Prepare descriptor block */
    uint8_t desc_buf[BLOCK_SIZE];
    memset(desc_buf, 0, BLOCK_SIZE);
    journal_desc_t *desc = (journal_desc_t *)desc_buf;
    desc->magic = JRNL_DESC_MAGIC;
    desc->seq = t->seq;
    desc->count = t->count;
    for (uint32_t i = 0; i < t->count; i++) {
        desc->target_blockno[i] = t->target_blockno[i];
    }
    desc->crc32 = crc32(desc, offsetof(journal_desc_t, crc32));

    /* Compute aggregate data CRC */
    uint32_t running_crc = crc32(desc_buf, BLOCK_SIZE);
    for (uint32_t i = 0; i < t->count; i++) {
        running_crc = crc32_combine(running_crc, t->data[i], BLOCK_SIZE);
    }

    /* 3. Prepare commit block */
    uint8_t comt_buf[BLOCK_SIZE];
    memset(comt_buf, 0, BLOCK_SIZE);
    journal_commit_t *comt = (journal_commit_t *)comt_buf;
    comt->magic = JRNL_COMMIT_MAGIC;
    comt->seq = t->seq;
    comt->count = t->count;
    comt->data_crc32 = running_crc;
    comt->crc32 = crc32(comt, offsetof(journal_commit_t, crc32));

    /* 4. Write descriptor and data blocks into circular log area */
    uint32_t cur = j->head_pos;
    blk_write(fs->dev, j->log_start + cur, desc_buf);
    cur = (cur + 1) % j->log_blocks;

    for (uint32_t i = 0; i < t->count; i++) {
        blk_write(fs->dev, j->log_start + cur, t->data[i]);
        cur = (cur + 1) % j->log_blocks;
    }
    /* Sync descriptor & data blocks */
    blk_sync(fs->dev);

    /* 5. Write commit block into circular log area */
    blk_write(fs->dev, j->log_start + cur, comt_buf);
    cur = (cur + 1) % j->log_blocks;
    /* Sync commit block -> Transaction is now DURABLE */
    blk_sync(fs->dev);

    /* Update head pointers */
    j->head_pos = cur;
    j->head_seq++;
    j->metrics.log_bytes_written += (uint64_t)needed_blocks * BLOCK_SIZE;

    /* 6. Write blocks to home locations */
    for (uint32_t i = 0; i < t->count; i++) {
        uint32_t blk = t->target_blockno[i];
        if (fs->cache) {
            buf_t *b = cache_get((cache_t *)fs->cache, blk);
            if (b) {
                memcpy(b->data, t->data[i], BLOCK_SIZE);
                cache_put((cache_t *)fs->cache, b, 1);
            }
        } else {
            blk_write(fs->dev, blk, t->data[i]);
        }
    }

    /* Flush modified metadata blocks to disk */
    if (fs->cache) {
        cache_flush((cache_t *)fs->cache);
    }
    blk_sync(fs->dev);

    /* 7. Checkpoint: advance journal superblock */
    j->tail_pos = j->head_pos;
    uint8_t sb_buf[BLOCK_SIZE];
    memset(sb_buf, 0, BLOCK_SIZE);
    journal_sb_t *jsb = (journal_sb_t *)sb_buf;
    jsb->magic = JRNL_MAGIC;
    jsb->block_size = BLOCK_SIZE;
    jsb->head_seq = j->head_seq;
    jsb->start_block = j->tail_pos;
    jsb->log_blocks = j->log_blocks;
    jsb->crc32 = crc32(jsb, offsetof(journal_sb_t, crc32));

    blk_write(fs->dev, j->journal_start, sb_buf);
    blk_sync(fs->dev);

    fs->current_tx = NULL;
    j->metrics.tx_committed++;
    free(t);

    pthread_mutex_unlock(&j->lock);
    return 0;
}

void tx_abort(tx_t *t) {
    if (!t) return;
    journal_t *j = t->j;
    fs_t *fs = t->fs;

    pthread_mutex_lock(&j->lock);

    /* Invalidate any cache buffers that were staged by this transaction */
    if (fs->cache) {
        for (uint32_t i = 0; i < t->count; i++) {
            cache_invalidate_block((cache_t *)fs->cache, t->target_blockno[i]);
        }
    }

    fs->current_tx = NULL;
    j->metrics.tx_aborted++;
    free(t);

    pthread_mutex_unlock(&j->lock);
}

int journal_replay(fs_t *fs) {
    if (!fs || !fs->dev) return -EINVAL;
    if (fs->sb.journal_blocks < 2) return 0;

    uint32_t journal_start = fs->sb.journal_start;
    uint32_t log_start = journal_start + 1;
    uint32_t log_blocks = fs->sb.journal_blocks - 1;

    uint8_t sb_buf[BLOCK_SIZE];
    int rc = blk_read(fs->dev, journal_start, sb_buf);
    if (rc != 0) return rc;

    journal_sb_t *jsb = (journal_sb_t *)sb_buf;
    if (jsb->magic != JRNL_MAGIC) {
        return 0; /* Journal not formatted */
    }

    uint32_t expected_crc = crc32(jsb, offsetof(journal_sb_t, crc32));
    if (jsb->crc32 != expected_crc) {
        LOG_WARN("Journal superblock CRC mismatch during replay");
        return -EIO;
    }

    uint32_t cur = jsb->start_block % log_blocks;
    uint64_t expected_seq = jsb->head_seq;
    int replayed_count = 0;

    while (1) {
        /* 1. Read descriptor candidate */
        uint8_t desc_buf[BLOCK_SIZE];
        rc = blk_read(fs->dev, log_start + cur, desc_buf);
        if (rc != 0) break;

        journal_desc_t *desc = (journal_desc_t *)desc_buf;
        if (desc->magic != JRNL_DESC_MAGIC || desc->seq != expected_seq) {
            break;
        }

        uint32_t desc_crc = crc32(desc, offsetof(journal_desc_t, crc32));
        if (desc->crc32 != desc_crc || desc->count == 0 || desc->count > JRNL_MAX_TX_BLOCKS) {
            break;
        }

        /* 2. Read data block copies candidate */
        uint8_t data_copies[JRNL_MAX_TX_BLOCKS][BLOCK_SIZE];
        uint32_t data_pos = (cur + 1) % log_blocks;
        int read_error = 0;

        for (uint32_t i = 0; i < desc->count; i++) {
            rc = blk_read(fs->dev, log_start + data_pos, data_copies[i]);
            if (rc != 0) {
                read_error = 1;
                break;
            }
            data_pos = (data_pos + 1) % log_blocks;
        }
        if (read_error) break;

        /* 3. Read commit block candidate */
        uint8_t comt_buf[BLOCK_SIZE];
        rc = blk_read(fs->dev, log_start + data_pos, comt_buf);
        if (rc != 0) break;

        journal_commit_t *comt = (journal_commit_t *)comt_buf;
        if (comt->magic != JRNL_COMMIT_MAGIC || comt->seq != desc->seq || comt->count != desc->count) {
            /* Uncommitted transaction: commit block missing or mismatch */
            break;
        }

        uint32_t comt_crc = crc32(comt, offsetof(journal_commit_t, crc32));
        if (comt->crc32 != comt_crc) {
            break;
        }

        /* Verify data CRC */
        uint32_t running_crc = crc32(desc_buf, BLOCK_SIZE);
        for (uint32_t i = 0; i < desc->count; i++) {
            running_crc = crc32_combine(running_crc, data_copies[i], BLOCK_SIZE);
        }
        if (comt->data_crc32 != running_crc) {
            LOG_WARN("Journal transaction %lu data CRC mismatch in log", (unsigned long)desc->seq);
            break;
        }

        /* 4. Transaction is confirmed COMMITTED! Replay blocks to home locations */
        for (uint32_t i = 0; i < desc->count; i++) {
            blk_write(fs->dev, desc->target_blockno[i], data_copies[i]);
        }

        /* Advance log pointer past commit block */
        cur = (data_pos + 1) % log_blocks;
        expected_seq++;
        replayed_count++;
    }

    if (replayed_count > 0) {
        blk_sync(fs->dev);

        /* Invalidate cache so memory doesn't retain stale pre-replay blocks */
        if (fs->cache) {
            cache_invalidate_all((cache_t *)fs->cache);
        }

        /* Reset log checkpoint to current clean position */
        memset(sb_buf, 0, BLOCK_SIZE);
        jsb->magic = JRNL_MAGIC;
        jsb->block_size = BLOCK_SIZE;
        jsb->head_seq = expected_seq;
        jsb->start_block = cur;
        jsb->log_blocks = log_blocks;
        jsb->crc32 = crc32(jsb, offsetof(journal_sb_t, crc32));

        blk_write(fs->dev, journal_start, sb_buf);
        blk_sync(fs->dev);

        if (fs->journal) {
            journal_t *j = (journal_t *)fs->journal;
            j->head_seq = expected_seq;
            j->head_pos = cur;
            j->tail_pos = cur;
            j->metrics.tx_replayed += (uint64_t)replayed_count;
        }

        LOG_INFO("Journal replay completed: %d transactions replayed", replayed_count);
    }

    return replayed_count;
}

journal_metrics_t journal_get_metrics(const journal_t *j) {
    journal_metrics_t m = {0, 0, 0, 0, 0, 0};
    if (j) {
        m = j->metrics;
    }
    return m;
}
