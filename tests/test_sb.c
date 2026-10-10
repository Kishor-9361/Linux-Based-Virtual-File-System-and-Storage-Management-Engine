#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>

#define TEST_SB_IMG "/tmp/minifs_test_sb.img"

static void cleanup_test_img(void) {
    unlink(TEST_SB_IMG);
}

TEST(m2_test1_layout_calculation_various_sizes) {
    uint32_t sizes[] = {
        64,             /* 256 KB */
        256,            /* 1 MB */
        4096,           /* 16 MB */
        65536,          /* 256 MB */
        262144          /* 1 GB */
    };

    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        uint32_t total = sizes[i];
        superblock_t sb;
        ASSERT_OK(sb_compute_layout(total, &sb));

        ASSERT_EQ(sb.magic, MINIFS_MAGIC);
        ASSERT_EQ(sb.version, MINIFS_VERSION);
        ASSERT_EQ(sb.block_size, BLOCK_SIZE);
        ASSERT_EQ(sb.total_blocks, total);

        /* Contiguity and non-overlap checks */
        ASSERT_EQ(sb.journal_start, 1);
        ASSERT_EQ(sb.ibitmap_start, sb.journal_start + sb.journal_blocks);
        ASSERT_EQ(sb.bbitmap_start, sb.ibitmap_start + sb.ibitmap_blocks);
        ASSERT_EQ(sb.itable_start, sb.bbitmap_start + sb.bbitmap_blocks);
        ASSERT_EQ(sb.data_start, sb.itable_start + sb.itable_blocks);

        /* All regions inside image */
        ASSERT_TRUE(sb.data_start < total);
        ASSERT_EQ(sb.free_blocks, total - sb.data_start);
        ASSERT_TRUE(sb.free_inodes > 0);
    }

    /* Reject < 256 KB (< 64 blocks) */
    superblock_t bad_sb;
    ASSERT_ERR(sb_compute_layout(63, &bad_sb), -EINVAL);
}

TEST(m2_test2_format_then_load_identical) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_SB_IMG, 1, 1024));

    ASSERT_OK(sb_format(&dev));

    superblock_t loaded;
    ASSERT_OK(sb_load(&dev, &loaded));

    superblock_t expected;
    ASSERT_OK(sb_compute_layout(1024, &expected));
    expected.crc32 = crc32(&expected, offsetof(superblock_t, crc32));

    ASSERT_EQ(loaded.magic, expected.magic);
    ASSERT_EQ(loaded.total_blocks, expected.total_blocks);
    ASSERT_EQ(loaded.data_start, expected.data_start);
    ASSERT_EQ(loaded.free_blocks, expected.free_blocks);
    ASSERT_EQ(loaded.free_inodes, expected.free_inodes);
    ASSERT_EQ(loaded.crc32, expected.crc32);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m2_test3_flip_bytes_crc_failure_detected) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_SB_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    uint8_t orig_block[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, 0, orig_block));

    /* Flip one byte across diverse offsets in superblock (0..251) */
    size_t test_offsets[] = {0, 4, 12, 16, 24, 48, 80, 100, 200, 250};
    setenv("MINIFS_LOG", "NONE", 1);
    for (size_t i = 0; i < sizeof(test_offsets)/sizeof(test_offsets[0]); i++) {
        size_t off = test_offsets[i];
        uint8_t corrupted[BLOCK_SIZE];
        memcpy(corrupted, orig_block, BLOCK_SIZE);
        corrupted[off] ^= 0xFF; /* Corrupt byte */

        ASSERT_OK(blk_write(&dev, 0, corrupted));

        superblock_t out;
        /* Should fail with -EUCLEAN or -EINVAL */
        int rc = sb_load(&dev, &out);
        ASSERT_TRUE(rc == -EUCLEAN || rc == -EINVAL);
    }
    unsetenv("MINIFS_LOG");

    /* Restore original */
    ASSERT_OK(blk_write(&dev, 0, orig_block));
    superblock_t valid_sb;
    ASSERT_OK(sb_load(&dev, &valid_sb));

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m2_test4_wrong_magic_rejected) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_SB_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    uint8_t blk[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, 0, blk));

    /* Set invalid magic */
    uint32_t bad_magic = 0xDEADBEEF;
    memcpy(blk, &bad_magic, sizeof(bad_magic));
    ASSERT_OK(blk_write(&dev, 0, blk));

    setenv("MINIFS_LOG", "NONE", 1);
    superblock_t sb;
    ASSERT_ERR(sb_load(&dev, &sb), -EINVAL);
    unsetenv("MINIFS_LOG");

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m2_test5_bitmap_shows_data_start_blocks_used) {
    cleanup_test_img();
    blkdev_t dev;
    uint32_t total = 512;
    ASSERT_OK(blk_open(&dev, TEST_SB_IMG, 1, total));
    ASSERT_OK(sb_format(&dev));

    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Read block bitmap blocks */
    uint32_t used_count = 0;
    for (uint32_t b = 0; b < sb.bbitmap_blocks; b++) {
        uint8_t bbm[BLOCK_SIZE];
        ASSERT_OK(blk_read(&dev, sb.bbitmap_start + b, bbm));
        for (uint32_t byte = 0; byte < BLOCK_SIZE; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                uint32_t blkno = (b * BLOCK_SIZE * 8) + (byte * 8) + (uint32_t)bit;
                if (blkno < total && (bbm[byte] & (1U << bit))) {
                    used_count++;
                }
            }
        }
    }

    /* Bitmap must show exactly data_start blocks used */
    ASSERT_EQ(used_count, sb.data_start);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m2_test6_reformat_resets_state) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_SB_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Mark state dirty and change mount count */
    sb.state = SB_STATE_DIRTY;
    sb.mount_count = 42;
    ASSERT_OK(sb_store(&dev, &sb));

    superblock_t dirty_sb;
    ASSERT_OK(sb_load(&dev, &dirty_sb));
    ASSERT_EQ(dirty_sb.state, SB_STATE_DIRTY);
    ASSERT_EQ(dirty_sb.mount_count, 42);

    /* Reformat */
    ASSERT_OK(sb_format(&dev));

    superblock_t clean_sb;
    ASSERT_OK(sb_load(&dev, &clean_sb));
    ASSERT_EQ(clean_sb.state, SB_STATE_CLEAN);
    ASSERT_EQ(clean_sb.mount_count, 0);

    blk_close(&dev);
    cleanup_test_img();
}
