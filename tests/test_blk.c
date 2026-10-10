#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>
#include <fcntl.h>

#define TEST_IMG_PATH "/tmp/minifs_test_blk.img"

static void cleanup_test_image(void) {
    unlink(TEST_IMG_PATH);
}

TEST(m1_test1_create_1mb_distinct_pattern) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 256; /* 1 MB / 4 KB = 256 blocks */
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));
    ASSERT_EQ(dev.nblocks, 256);

    uint8_t write_buf[BLOCK_SIZE];
    uint8_t read_buf[BLOCK_SIZE];

    for (uint32_t b = 0; b < nblocks; b++) {
        memset(write_buf, (int)(b & 0xFF), BLOCK_SIZE);
        ASSERT_OK(blk_write(&dev, b, write_buf));
    }
    ASSERT_OK(blk_sync(&dev));

    for (uint32_t b = 0; b < nblocks; b++) {
        memset(read_buf, 0, BLOCK_SIZE);
        ASSERT_OK(blk_read(&dev, b, read_buf));
        memset(write_buf, (int)(b & 0xFF), BLOCK_SIZE);
        ASSERT_MEM_EQ(read_buf, write_buf, BLOCK_SIZE);
    }

    blk_close(&dev);
    cleanup_test_image();
}

TEST(m1_test2_random_10000_ops_shadow_array) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 64; /* 256 KB test space for high write density */
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));

    /* In-memory shadow array of 64 blocks */
    uint8_t *shadow = (uint8_t *)th_calloc(nblocks, BLOCK_SIZE);
    ASSERT_TRUE(shadow != NULL);

    uint8_t op_buf[BLOCK_SIZE];
    srand(42);

    for (int i = 0; i < 10000; i++) {
        uint32_t blk = (uint32_t)(rand() % (int)nblocks);
        int is_write = rand() % 2;

        if (is_write) {
            uint8_t fill = (uint8_t)(rand() & 0xFF);
            memset(op_buf, fill, BLOCK_SIZE);
            ASSERT_OK(blk_write(&dev, blk, op_buf));
            memcpy(shadow + (blk * BLOCK_SIZE), op_buf, BLOCK_SIZE);
        } else {
            ASSERT_OK(blk_read(&dev, blk, op_buf));
            ASSERT_MEM_EQ(op_buf, shadow + (blk * BLOCK_SIZE), BLOCK_SIZE);
        }
    }

    th_free(shadow);
    blk_close(&dev);
    cleanup_test_image();
}

TEST(m1_test3_out_of_range_einval) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 16;
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));

    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0x11, BLOCK_SIZE);

    ASSERT_ERR(blk_read(&dev, 16, buf), -EINVAL);
    ASSERT_ERR(blk_read(&dev, 100, buf), -EINVAL);
    ASSERT_ERR(blk_write(&dev, 16, buf), -EINVAL);
    ASSERT_ERR(blk_write(&dev, 1000, buf), -EINVAL);

    blk_close(&dev);
    cleanup_test_image();
}

TEST(m1_test4_close_reopen_persists) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 32;
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));

    uint8_t test_data[BLOCK_SIZE];
    memset(test_data, 0x5A, BLOCK_SIZE);
    ASSERT_OK(blk_write(&dev, 7, test_data));
    ASSERT_OK(blk_sync(&dev));
    blk_close(&dev);

    /* Reopen existing */
    blkdev_t dev2;
    ASSERT_OK(blk_open(&dev2, TEST_IMG_PATH, 0, 0));
    ASSERT_EQ(dev2.nblocks, 32);

    uint8_t read_data[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev2, 7, read_data));
    ASSERT_MEM_EQ(test_data, read_data, BLOCK_SIZE);

    blk_close(&dev2);
    cleanup_test_image();
}

TEST(m1_test5_fault_drop_after_5_writes) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 16;
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));

    /* Arm fault drop after 5 writes */
    blk_set_fault(&dev, 5, FAULT_DROP);

    uint8_t buf[BLOCK_SIZE];
    for (uint32_t i = 0; i < 10; i++) {
        memset(buf, (int)(i + 1), BLOCK_SIZE);
        ASSERT_OK(blk_write(&dev, i, buf));
    }
    blk_close(&dev);

    /* Reopen and check: only blocks 0..4 should have written data */
    blkdev_t dev2;
    ASSERT_OK(blk_open(&dev2, TEST_IMG_PATH, 0, 0));

    uint8_t zero_buf[BLOCK_SIZE];
    memset(zero_buf, 0, BLOCK_SIZE);

    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_OK(blk_read(&dev2, i, buf));
        uint8_t expected[BLOCK_SIZE];
        memset(expected, (int)(i + 1), BLOCK_SIZE);
        ASSERT_MEM_EQ(buf, expected, BLOCK_SIZE);
    }

    for (uint32_t i = 5; i < 10; i++) {
        ASSERT_OK(blk_read(&dev2, i, buf));
        ASSERT_MEM_EQ(buf, zero_buf, BLOCK_SIZE);
    }

    blk_close(&dev2);
    cleanup_test_image();
}

TEST(m1_test6_counters_equal_calls) {
    cleanup_test_image();
    blkdev_t dev;
    uint32_t nblocks = 16;
    ASSERT_OK(blk_open(&dev, TEST_IMG_PATH, 1, nblocks));

    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0xEE, BLOCK_SIZE);

    /* 7 writes */
    for (int i = 0; i < 7; i++) {
        ASSERT_OK(blk_write(&dev, (uint32_t)i, buf));
    }
    /* 11 reads */
    for (int i = 0; i < 11; i++) {
        ASSERT_OK(blk_read(&dev, (uint32_t)(i % 7), buf));
    }
    /* 3 syncs */
    ASSERT_OK(blk_sync(&dev));
    ASSERT_OK(blk_sync(&dev));
    ASSERT_OK(blk_sync(&dev));

    blk_metrics_t m = blk_get_metrics(&dev);
    ASSERT_EQ(m.writes, 7);
    ASSERT_EQ(m.reads, 11);
    ASSERT_EQ(m.syncs, 3);
    ASSERT_EQ(m.bytes_written, 7 * BLOCK_SIZE);
    ASSERT_EQ(m.bytes_read, 11 * BLOCK_SIZE);

    blk_close(&dev);
    cleanup_test_image();
}
