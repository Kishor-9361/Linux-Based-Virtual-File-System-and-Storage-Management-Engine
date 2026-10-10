#ifndef MINIFS_JOURNAL_H
#define MINIFS_JOURNAL_H

#include <stdint.h>
#include <pthread.h>
#include "blk.h"
#include "sb.h"
#include "bitmap.h"
#include "common.h"

#define JRNL_MAGIC          0x4A524E4CU /* "JRNL" in ASCII */
#define JRNL_DESC_MAGIC     0x44455343U /* "DESC" in ASCII */
#define JRNL_COMMIT_MAGIC   0x434F4D54U /* "COMT" in ASCII */
#define JRNL_MAX_TX_BLOCKS  64U

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* JRNL_MAGIC */
    uint32_t block_size;    /* BLOCK_SIZE (4096) */
    uint64_t head_seq;      /* Next transaction sequence number */
    uint32_t start_block;   /* Oldest active log block relative to log area */
    uint32_t log_blocks;    /* Total circular log blocks */
    uint8_t  reserved[228];
    uint32_t crc32;         /* CRC32 over preceding 252 bytes */
} journal_sb_t;
_Static_assert(sizeof(journal_sb_t) == 256, "journal_sb_t must be 256 bytes");

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* JRNL_DESC_MAGIC */
    uint64_t seq;
    uint32_t count;
    uint32_t target_blockno[JRNL_MAX_TX_BLOCKS];
    uint8_t  reserved[BLOCK_SIZE - 16 - (JRNL_MAX_TX_BLOCKS * 4) - 4];
    uint32_t crc32;
} journal_desc_t;
_Static_assert(sizeof(journal_desc_t) == BLOCK_SIZE, "journal_desc_t must be BLOCK_SIZE");

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* JRNL_COMMIT_MAGIC */
    uint64_t seq;
    uint32_t count;
    uint32_t data_crc32;
    uint8_t  reserved[BLOCK_SIZE - 20 - 4];
    uint32_t crc32;
} journal_commit_t;
_Static_assert(sizeof(journal_commit_t) == BLOCK_SIZE, "journal_commit_t must be BLOCK_SIZE");

typedef struct {
    uint64_t tx_begun;
    uint64_t tx_committed;
    uint64_t tx_aborted;
    uint64_t tx_replayed;
    uint64_t checkpoints;
    uint64_t log_bytes_written;
} journal_metrics_t;

typedef struct journal {
    fs_t             *fs;
    uint32_t          journal_start; /* Absolute block of journal superblock */
    uint32_t          log_start;     /* Absolute block of circular log area */
    uint32_t          log_blocks;    /* Total blocks in circular log */
    uint64_t          head_seq;
    uint32_t          head_pos;      /* Relative write position in log */
    uint32_t          tail_pos;      /* Relative start position of oldest uncheckpointed tx */
    pthread_mutex_t   lock;
    journal_metrics_t metrics;
} journal_t;

typedef struct tx {
    journal_t *j;
    fs_t      *fs;
    uint64_t   seq;
    uint32_t   count;
    uint32_t   target_blockno[JRNL_MAX_TX_BLOCKS];
    uint8_t    data[JRNL_MAX_TX_BLOCKS][BLOCK_SIZE];
    int        active;
} tx_t;

/* Journal lifecycle */
journal_t *journal_init(fs_t *fs);
void       journal_destroy(journal_t *j);

/* Transaction APIs */
tx_t      *tx_begin(fs_t *fs);
int        tx_write(tx_t *t, uint32_t blockno, const void *data);
int        tx_commit(tx_t *t);
void       tx_abort(tx_t *t);

/* Recovery & Checkpointing */
int        journal_replay(fs_t *fs);
int        journal_checkpoint(fs_t *fs);

/* Metrics */
journal_metrics_t journal_get_metrics(const journal_t *j);

#endif /* MINIFS_JOURNAL_H */
