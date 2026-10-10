#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "journal.h"
#include "cache.h"
#include "vfs.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>
#include <sys/stat.h>

#define TEST_JRNL_IMG "/tmp/minifs_test_jrnl.img"

static void init_test_fs(blkdev_t *dev, fs_t *fs, uint32_t total_blocks) {
    unlink(TEST_JRNL_IMG);
    int rc = blk_open(dev, TEST_JRNL_IMG, 1, total_blocks);
    ASSERT_EQ(rc, 0);

    rc = sb_format(dev);
    ASSERT_EQ(rc, 0);

    rc = fs_init_allocators(fs, dev);
    ASSERT_EQ(rc, 0);

    fs->cache = cache_create(dev, 64, CACHE_WRITE_BACK);
    ASSERT_TRUE(fs->cache != NULL);

    journal_t *j = journal_init(fs);
    ASSERT_TRUE(j != NULL);
}

static void close_test_fs(blkdev_t *dev, fs_t *fs) {
    if (fs->journal) {
        journal_destroy((journal_t *)fs->journal);
        fs->journal = NULL;
    }
    if (fs->cache) {
        cache_destroy((cache_t *)fs->cache);
        fs->cache = NULL;
    }
    blk_close(dev);
    unlink(TEST_JRNL_IMG);
}

TEST(m10_test1_atomic_commit_and_abort) {
    blkdev_t dev;
    fs_t fs;
    init_test_fs(&dev, &fs, 2048);

    uint32_t target_blk = fs.sb.data_start + 5;
    uint8_t original_buf[BLOCK_SIZE];
    blk_read(&dev, target_blk, original_buf);

    /* 1. Aborted transaction */
    tx_t *t1 = tx_begin(&fs);
    ASSERT_TRUE(t1 != NULL);

    uint8_t mod_buf[BLOCK_SIZE];
    memset(mod_buf, 0xEE, BLOCK_SIZE);
    int rc = tx_write(t1, target_blk, mod_buf);
    ASSERT_EQ(rc, 0);

    tx_abort(t1);

    /* Verify disk block remains unchanged */
    uint8_t check_buf[BLOCK_SIZE];
    blk_read(&dev, target_blk, check_buf);
    ASSERT_EQ(memcmp(original_buf, check_buf, BLOCK_SIZE), 0);

    /* 2. Committed transaction */
    tx_t *t2 = tx_begin(&fs);
    ASSERT_TRUE(t2 != NULL);

    memset(mod_buf, 0xCC, BLOCK_SIZE);
    rc = tx_write(t2, target_blk, mod_buf);
    ASSERT_EQ(rc, 0);

    rc = tx_commit(t2);
    ASSERT_EQ(rc, 0);

    /* Drop cache to verify durability on physical disk */
    cache_invalidate_all((cache_t *)fs.cache);

    blk_read(&dev, target_blk, check_buf);
    ASSERT_EQ(memcmp(mod_buf, check_buf, BLOCK_SIZE), 0);

    close_test_fs(&dev, &fs);
}

TEST(m10_test2_crash_before_commit_discarded) {
    blkdev_t dev;
    fs_t fs;
    init_test_fs(&dev, &fs, 2048);

    journal_t *j = (journal_t *)fs.journal;
    uint32_t target_blk = fs.sb.data_start + 10;

    uint8_t baseline[BLOCK_SIZE] = {0};
    blk_write(&dev, target_blk, baseline);

    /* Write descriptor and data to log manually, omitting commit block */
    uint8_t desc_buf[BLOCK_SIZE] = {0};
    journal_desc_t *desc = (journal_desc_t *)desc_buf;
    desc->magic = JRNL_DESC_MAGIC;
    desc->seq = j->head_seq;
    desc->count = 1;
    desc->target_blockno[0] = target_blk;
    desc->crc32 = crc32(desc, offsetof(journal_desc_t, crc32));

    uint8_t data_copy[BLOCK_SIZE];
    memset(data_copy, 0x77, BLOCK_SIZE);

    blk_write(&dev, j->log_start + j->head_pos, desc_buf);
    blk_write(&dev, j->log_start + ((j->head_pos + 1) % j->log_blocks), data_copy);
    blk_sync(&dev);

    /* Replay must discard this uncommitted transaction */
    int replayed = journal_replay(&fs);
    ASSERT_EQ(replayed, 0);

    /* Target block on disk must still be zero */
    uint8_t check[BLOCK_SIZE];
    blk_read(&dev, target_blk, check);
    ASSERT_EQ(check[0], 0);

    close_test_fs(&dev, &fs);
}

TEST(m10_test3_committed_replay_applied) {
    blkdev_t dev;
    fs_t fs;
    init_test_fs(&dev, &fs, 2048);

    journal_t *j = (journal_t *)fs.journal;
    uint32_t target_blk = fs.sb.data_start + 20;

    uint8_t baseline[BLOCK_SIZE] = {0};
    blk_write(&dev, target_blk, baseline);

    /* Form a valid transaction in log (descriptor + data + commit) */
    uint8_t desc_buf[BLOCK_SIZE] = {0};
    journal_desc_t *desc = (journal_desc_t *)desc_buf;
    desc->magic = JRNL_DESC_MAGIC;
    desc->seq = j->head_seq;
    desc->count = 1;
    desc->target_blockno[0] = target_blk;
    desc->crc32 = crc32(desc, offsetof(journal_desc_t, crc32));

    uint8_t data_copy[BLOCK_SIZE];
    memset(data_copy, 0x88, BLOCK_SIZE);

    uint8_t comt_buf[BLOCK_SIZE] = {0};
    journal_commit_t *comt = (journal_commit_t *)comt_buf;
    comt->magic = JRNL_COMMIT_MAGIC;
    comt->seq = j->head_seq;
    comt->count = 1;

    /* Compute aggregate data CRC */
    uint32_t run_crc = crc32(desc_buf, BLOCK_SIZE);
    const uint8_t *dptr = data_copy;
    uint32_t crc_tmp = ~run_crc;
    for (size_t i = 0; i < BLOCK_SIZE; i++) {
        crc_tmp ^= dptr[i];
        for (int b = 0; b < 8; b++) {
            crc_tmp = (crc_tmp >> 1) ^ (0xEDB88320U & (uint32_t)(-(int32_t)(crc_tmp & 1U)));
        }
    }
    comt->data_crc32 = ~crc_tmp;
    comt->crc32 = crc32(comt, offsetof(journal_commit_t, crc32));

    uint32_t p0 = j->head_pos;
    uint32_t p1 = (p0 + 1) % j->log_blocks;
    uint32_t p2 = (p1 + 1) % j->log_blocks;

    blk_write(&dev, j->log_start + p0, desc_buf);
    blk_write(&dev, j->log_start + p1, data_copy);
    blk_write(&dev, j->log_start + p2, comt_buf);
    blk_sync(&dev);

    /* Target block on disk is STILL 0 before replay */
    uint8_t check[BLOCK_SIZE];
    blk_read(&dev, target_blk, check);
    ASSERT_EQ(check[0], 0);

    /* Run replay */
    int replayed = journal_replay(&fs);
    ASSERT_EQ(replayed, 1);

    /* Target block on disk MUST now contain 0x88 */
    blk_read(&dev, target_blk, check);
    ASSERT_EQ(check[0], 0x88);
    ASSERT_EQ(check[BLOCK_SIZE - 1], 0x88);

    close_test_fs(&dev, &fs);
}

TEST(m10_test4_corrupt_log_block_detected) {
    blkdev_t dev;
    fs_t fs;
    init_test_fs(&dev, &fs, 2048);

    journal_t *j = (journal_t *)fs.journal;
    uint32_t target_blk = fs.sb.data_start + 30;

    /* Form transaction with intentional corrupt byte */
    uint8_t desc_buf[BLOCK_SIZE] = {0};
    journal_desc_t *desc = (journal_desc_t *)desc_buf;
    desc->magic = JRNL_DESC_MAGIC;
    desc->seq = j->head_seq;
    desc->count = 1;
    desc->target_blockno[0] = target_blk;
    desc->crc32 = crc32(desc, offsetof(journal_desc_t, crc32));

    /* Flip byte */
    desc_buf[10] ^= 0xFF;

    blk_write(&dev, j->log_start + j->head_pos, desc_buf);
    blk_sync(&dev);

    /* Replay must detect CRC corrupt descriptor and skip it */
    int replayed = journal_replay(&fs);
    ASSERT_EQ(replayed, 0);

    close_test_fs(&dev, &fs);
}

TEST(m10_test5_idempotent_replay) {
    blkdev_t dev;
    fs_t fs;
    init_test_fs(&dev, &fs, 2048);

    uint32_t target_blk = fs.sb.data_start + 40;
    tx_t *t = tx_begin(&fs);
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0x42, BLOCK_SIZE);
    tx_write(t, target_blk, buf);
    tx_commit(t);

    /* Replay on already-checkpointed journal returns 0 */
    int r1 = journal_replay(&fs);
    ASSERT_EQ(r1, 0);

    uint8_t check1[BLOCK_SIZE];
    blk_read(&dev, target_blk, check1);
    ASSERT_EQ(check1[0], 0x42);

    /* Second replay */
    int r2 = journal_replay(&fs);
    ASSERT_EQ(r2, 0);

    uint8_t check2[BLOCK_SIZE];
    blk_read(&dev, target_blk, check2);
    ASSERT_EQ(memcmp(check1, check2, BLOCK_SIZE), 0);

    close_test_fs(&dev, &fs);
}

TEST(m10_test6_log_wraparound) {
    blkdev_t dev;
    fs_t fs;
    /* Small 512 block disk -> 16 log blocks */
    init_test_fs(&dev, &fs, 512);

    journal_t *j = (journal_t *)fs.journal;
    uint32_t log_blocks = j->log_blocks;

    /* Execute 3x log_blocks transactions to wrap around multiple times */
    for (uint32_t i = 0; i < log_blocks * 3; i++) {
        tx_t *t = tx_begin(&fs);
        ASSERT_TRUE(t != NULL);

        uint8_t buf[BLOCK_SIZE];
        memset(buf, (int)(i & 0xFF), BLOCK_SIZE);
        uint32_t blk = fs.sb.data_start + (i % 10);
        tx_write(t, blk, buf);

        int rc = tx_commit(t);
        ASSERT_EQ(rc, 0);
    }

    journal_metrics_t m = journal_get_metrics(j);
    ASSERT_EQ(m.tx_committed, (uint64_t)(log_blocks * 3));

    close_test_fs(&dev, &fs);
}

TEST(m10_test7_crashtest_sweep_suite) {
    /* Invoke crashtest tool binary to generate crash_matrix.csv */
    int rc = system("./build/bin/crashtest reports/crash_matrix.csv");
    ASSERT_EQ(rc, 0);

    /* Verify reports/crash_matrix.csv exists and is populated */
    struct stat st;
    rc = stat("reports/crash_matrix.csv", &st);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(st.st_size > 100);
}
