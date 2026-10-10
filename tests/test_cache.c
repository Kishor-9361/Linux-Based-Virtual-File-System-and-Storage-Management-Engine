#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "cache.h"
#include "vfs.h"
#include "fileio.h"
#include "common.h"
#include <unistd.h>
#include <time.h>

#define TEST_CACHE_IMG "/tmp/minifs_test_cache.img"

static void setup_blank_dev(blkdev_t *dev, uint32_t blocks) {
    unlink(TEST_CACHE_IMG);
    int rc = blk_open(dev, TEST_CACHE_IMG, 1, blocks);
    ASSERT_EQ(rc, 0);
}

TEST(m9_test1_scripted_hit_miss_counters) {
    blkdev_t dev;
    setup_blank_dev(&dev, 128);

    cache_t *c = cache_create(&dev, 4, CACHE_WRITE_BACK);
    ASSERT_TRUE(c != NULL);

    /* 1. Access blocks 0, 1, 2, 3 -> 4 misses, 0 hits */
    for (uint32_t i = 0; i < 4; i++) {
        buf_t *b = cache_get(c, i);
        ASSERT_TRUE(b != NULL);
        cache_put(c, b, 0);
    }
    cache_stats_t s = cache_stats(c);
    ASSERT_EQ(s.misses, 4);
    ASSERT_EQ(s.hits, 0);
    ASSERT_EQ(s.evictions, 0);

    /* 2. Access blocks 0, 1, 2, 3 again -> 4 hits, 0 new misses */
    for (uint32_t i = 0; i < 4; i++) {
        buf_t *b = cache_get(c, i);
        ASSERT_TRUE(b != NULL);
        cache_put(c, b, 0);
    }
    s = cache_stats(c);
    ASSERT_EQ(s.misses, 4);
    ASSERT_EQ(s.hits, 4);
    ASSERT_EQ(s.evictions, 0);

    /* 3. Access blocks 4, 5, 6, 7 -> 4 misses, 4 evictions */
    for (uint32_t i = 4; i < 8; i++) {
        buf_t *b = cache_get(c, i);
        ASSERT_TRUE(b != NULL);
        cache_put(c, b, 0);
    }
    s = cache_stats(c);
    ASSERT_EQ(s.misses, 8);
    ASSERT_EQ(s.hits, 4);
    ASSERT_EQ(s.evictions, 4);

    /* 4. Access blocks 0, 1, 2, 3 -> all were evicted, so 4 new misses */
    for (uint32_t i = 0; i < 4; i++) {
        buf_t *b = cache_get(c, i);
        ASSERT_TRUE(b != NULL);
        cache_put(c, b, 0);
    }
    s = cache_stats(c);
    ASSERT_EQ(s.misses, 12);
    ASSERT_EQ(s.hits, 4);
    ASSERT_EQ(s.evictions, 8);

    cache_destroy(c);
    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}

TEST(m9_test2_lru_order_eviction) {
    blkdev_t dev;
    setup_blank_dev(&dev, 64);

    /* Cache size 3 */
    cache_t *c = cache_create(&dev, 3, CACHE_WRITE_BACK);
    ASSERT_TRUE(c != NULL);

    /* Insert 10, 20, 30 */
    buf_t *b = cache_get(c, 10); cache_put(c, b, 0);
    b = cache_get(c, 20); cache_put(c, b, 0);
    b = cache_get(c, 30); cache_put(c, b, 0);
    /* LRU order: MRU [30, 20, 10] LRU */

    /* Touch 10 -> becomes MRU */
    b = cache_get(c, 10); cache_put(c, b, 0);
    /* LRU order: MRU [10, 30, 20] LRU -> tail is 20! */

    /* Insert 40 -> must evict 20! */
    b = cache_get(c, 40); cache_put(c, b, 0);

    /* Verify 10, 30, 40 are hits, and 20 is a miss */
    cache_reset_stats(c);

    b = cache_get(c, 10); cache_put(c, b, 0); /* Hit */
    b = cache_get(c, 30); cache_put(c, b, 0); /* Hit */
    b = cache_get(c, 40); cache_put(c, b, 0); /* Hit */
    cache_stats_t s = cache_stats(c);
    ASSERT_EQ(s.hits, 3);
    ASSERT_EQ(s.misses, 0);

    /* Access 20 -> Miss! */
    b = cache_get(c, 20); cache_put(c, b, 0);
    s = cache_stats(c);
    ASSERT_EQ(s.misses, 1);

    cache_destroy(c);
    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}

TEST(m9_test3_dirty_eviction_writes_to_disk) {
    blkdev_t dev;
    setup_blank_dev(&dev, 64);

    uint8_t zero_blk[BLOCK_SIZE] = {0};
    blk_write(&dev, 5, zero_blk);

    cache_t *c = cache_create(&dev, 2, CACHE_WRITE_BACK);
    ASSERT_TRUE(c != NULL);

    /* Get block 5, write distinct pattern 0xAA, mark dirty */
    buf_t *b = cache_get(c, 5);
    ASSERT_TRUE(b != NULL);
    memset(b->data, 0xAA, BLOCK_SIZE);
    cache_put(c, b, 1); /* Dirty */

    /* Disk must still be zero prior to flush or eviction */
    uint8_t check_buf[BLOCK_SIZE];
    blk_read(&dev, 5, check_buf);
    ASSERT_EQ(check_buf[0], 0);

    /* Exceed cache size (access blocks 6 and 7) -> causes eviction of block 5 */
    b = cache_get(c, 6); cache_put(c, b, 0);
    b = cache_get(c, 7); cache_put(c, b, 0);

    /* Disk MUST now contain 0xAA because block 5 was written back upon eviction */
    blk_read(&dev, 5, check_buf);
    ASSERT_EQ(check_buf[0], 0xAA);
    ASSERT_EQ(check_buf[BLOCK_SIZE - 1], 0xAA);

    cache_destroy(c);
    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}

TEST(m9_test4_pinned_buffers_never_evicted) {
    blkdev_t dev;
    setup_blank_dev(&dev, 64);

    cache_t *c = cache_create(&dev, 2, CACHE_WRITE_BACK);
    ASSERT_TRUE(c != NULL);

    /* Pin both buffers */
    buf_t *b1 = cache_get(c, 10);
    ASSERT_TRUE(b1 != NULL);
    buf_t *b2 = cache_get(c, 20);
    ASSERT_TRUE(b2 != NULL);

    /* Cache is fully pinned. Third get must fail gracefully */
    buf_t *b3 = cache_get(c, 30);
    ASSERT_TRUE(b3 == NULL);

    /* Unpin b1 */
    cache_put(c, b1, 0);

    /* Now getting block 30 must succeed by evicting unpinned b1 */
    b3 = cache_get(c, 30);
    ASSERT_TRUE(b3 != NULL);

    cache_put(c, b2, 0);
    cache_put(c, b3, 0);

    cache_destroy(c);
    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}

TEST(m9_test5_equivalence_across_cache_sizes) {
    uint32_t sizes[] = {1, 16, 1024};
    uint8_t reference_buf[4096];
    for (int i = 0; i < 4096; i++) {
        reference_buf[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }

    for (int idx = 0; idx < 3; idx++) {
        uint32_t csize = sizes[idx];
        unlink(TEST_CACHE_IMG);
        blkdev_t dev;
        int rc = blk_open(&dev, TEST_CACHE_IMG, 1, 2048);
        ASSERT_EQ(rc, 0);

        rc = sb_format(&dev);
        ASSERT_EQ(rc, 0);

        fs_t fs;
        rc = fs_init_allocators(&fs, &dev);
        ASSERT_EQ(rc, 0);

        cache_t *c = cache_create(&dev, csize, CACHE_WRITE_BACK);
        fs.cache = c;

        /* Create file and write 4096 bytes */
        int ino = inode_create(&fs, S_IFREG | 0644, 0, 0);
        ASSERT_TRUE(ino > 0);

        ssize_t written = file_write(&fs, (uint32_t)ino, 0, reference_buf, sizeof(reference_buf));
        ASSERT_EQ(written, (ssize_t)sizeof(reference_buf));

        /* Read back and verify */
        uint8_t read_buf[4096];
        memset(read_buf, 0, sizeof(read_buf));
        ssize_t read_bytes = file_read(&fs, (uint32_t)ino, 0, read_buf, sizeof(read_buf));
        ASSERT_EQ(read_bytes, (ssize_t)sizeof(reference_buf));
        ASSERT_EQ(memcmp(reference_buf, read_buf, sizeof(reference_buf)), 0);

        cache_flush(c);
        cache_destroy(c);
        blk_close(&dev);
        unlink(TEST_CACHE_IMG);
    }
}

TEST(m9_test6_flush_vs_invalidate_crash_behavior) {
    unlink(TEST_CACHE_IMG);
    blkdev_t dev;
    int rc = blk_open(&dev, TEST_CACHE_IMG, 1, 2048);
    ASSERT_EQ(rc, 0);

    rc = sb_format(&dev);
    ASSERT_EQ(rc, 0);

    fs_t fs;
    rc = fs_init_allocators(&fs, &dev);
    ASSERT_EQ(rc, 0);

    cache_t *c = cache_create(&dev, 16, CACHE_WRITE_BACK);
    fs.cache = c;

    int ino = inode_create(&fs, S_IFREG | 0644, 0, 0);
    ASSERT_TRUE(ino > 0);

    char msg1[] = "Flushed committed data!";
    file_write(&fs, (uint32_t)ino, 0, msg1, strlen(msg1));
    cache_flush(c);

    char msg2[] = "Unflushed dirty data!";
    file_write(&fs, (uint32_t)ino, 100, msg2, strlen(msg2));

    /* Simulate sudden power cut / crash by dropping cache contents without flush */
    cache_invalidate_all(c);
    cache_destroy(c);
    blk_close(&dev);

    /* Reopen disk directly: msg1 must be on disk, but msg2 must have been lost */
    rc = blk_open(&dev, TEST_CACHE_IMG, 0, 0);
    ASSERT_EQ(rc, 0);
    rc = fs_init_allocators(&fs, &dev);
    ASSERT_EQ(rc, 0);

    char verify_buf[200] = {0};
    ssize_t r = file_read(&fs, (uint32_t)ino, 0, verify_buf, sizeof(verify_buf));
    ASSERT_TRUE(r >= (ssize_t)strlen(msg1));
    ASSERT_EQ(memcmp(verify_buf, msg1, strlen(msg1)), 0);
    /* Offset 100 must NOT contain msg2 (remains 0 because it was lost) */
    ASSERT_EQ(verify_buf[100], 0);

    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}

TEST(m9_test7_benchmark_speedup) {
    unlink(TEST_CACHE_IMG);
    blkdev_t dev;
    uint32_t total_blocks = 2048; /* 8 MB disk */
    int rc = blk_open(&dev, TEST_CACHE_IMG, 1, total_blocks);
    ASSERT_EQ(rc, 0);

    rc = sb_format(&dev);
    ASSERT_EQ(rc, 0);

    fs_t fs;
    rc = fs_init_allocators(&fs, &dev);
    ASSERT_EQ(rc, 0);

    /* Write 100 blocks of data */
    int ino = inode_create(&fs, S_IFREG | 0644, 0, 0);
    ASSERT_TRUE(ino > 0);

    uint8_t payload[BLOCK_SIZE];
    memset(payload, 0x42, sizeof(payload));
    for (uint32_t i = 0; i < 100; i++) {
        file_write(&fs, (uint32_t)ino, (uint64_t)i * BLOCK_SIZE, payload, BLOCK_SIZE);
    }

    /* Part A: 5000 reads with Cache OFF */
    dev.metrics.reads = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < 5000; i++) {
        uint64_t offset = (uint64_t)(i % 100) * BLOCK_SIZE;
        uint8_t buf[BLOCK_SIZE];
        file_read(&fs, (uint32_t)ino, offset, buf, BLOCK_SIZE);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double uncached_sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    uint64_t uncached_io = dev.metrics.reads;

    /* Part B: 5000 reads with Cache ON (128 buffers) */
    cache_t *c = cache_create(&dev, 128, CACHE_WRITE_BACK);
    fs.cache = c;
    dev.metrics.reads = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint32_t i = 0; i < 5000; i++) {
        uint64_t offset = (uint64_t)(i % 100) * BLOCK_SIZE;
        uint8_t buf[BLOCK_SIZE];
        file_read(&fs, (uint32_t)ino, offset, buf, BLOCK_SIZE);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double cached_sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
    uint64_t cached_io = dev.metrics.reads;

    cache_stats_t s = cache_stats(c);

    printf("\n--- Cache Benchmark Results ---\n");
    printf("Uncached: 5000 reads took %.4f ms (Physical Block Reads: %lu)\n", uncached_sec * 1000.0, (unsigned long)uncached_io);
    printf("Cached:   5000 reads took %.4f ms (Physical Block Reads: %lu)\n", cached_sec * 1000.0, (unsigned long)cached_io);
    printf("Cache stats: Hits=%lu, Misses=%lu, Hit Ratio=%.2f%%\n",
           (unsigned long)s.hits, (unsigned long)s.misses,
           (double)s.hits / (double)(s.hits + s.misses) * 100.0);
    if (cached_sec > 0.0) {
        printf("Speedup: %.2fx (Physical I/O reduction: %.1fx)\n",
               uncached_sec / cached_sec,
               (double)uncached_io / (double)(cached_io > 0 ? cached_io : 1));
    }
    printf("-------------------------------\n");

    /* Physical I/O must be drastically reduced with cache */
    ASSERT_TRUE(cached_io < uncached_io / 5);

    cache_destroy(c);
    blk_close(&dev);
    unlink(TEST_CACHE_IMG);
}
