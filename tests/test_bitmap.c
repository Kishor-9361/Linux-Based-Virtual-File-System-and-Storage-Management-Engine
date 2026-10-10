#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "bitmap.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>

#define TEST_BM_IMG "/tmp/minifs_test_bm.img"

static void cleanup_test_img(void) {
    unlink(TEST_BM_IMG);
}

TEST(m3_test1_alloc_all_blocks_enospc_and_free_all) {
    cleanup_test_img();
    blkdev_t dev;
    uint32_t total = 128; /* Small disk for exhaustive testing */
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, total));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t initial_free = fs.sb.free_blocks;
    uint32_t allocated_count = 0;
    int *allocated = (int *)th_malloc(initial_free * sizeof(int));
    ASSERT_TRUE(allocated != NULL);

    /* Allocate until ENOSPC */
    while (1) {
        int blk = alloc_block(&fs, 0);
        if (blk == -ENOSPC) break;
        ASSERT_OK(blk);
        allocated[allocated_count++] = blk;
    }

    ASSERT_EQ(allocated_count, initial_free);
    ASSERT_EQ(fs.sb.free_blocks, 0);
    ASSERT_ERR(alloc_block(&fs, 0), -ENOSPC);

    /* Free all allocated blocks */
    for (uint32_t i = 0; i < allocated_count; i++) {
        ASSERT_OK(free_block(&fs, (uint32_t)allocated[i]));
    }

    ASSERT_EQ(fs.sb.free_blocks, initial_free);

    th_free(allocated);
    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test2_double_free_double_set_detected) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int blk = alloc_block(&fs, 0);
    ASSERT_OK(blk);

    /* Double set on the raw bitmap */
    ASSERT_ERR(bm_set(&fs.bbitmap, (uint32_t)blk), -EEXIST);

    /* Free it */
    ASSERT_OK(free_block(&fs, (uint32_t)blk));

    /* Double free */
    ASSERT_ERR(free_block(&fs, (uint32_t)blk), -EINVAL);
    ASSERT_ERR(bm_clear(&fs.bbitmap, (uint32_t)blk), -EINVAL);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test3_100k_random_alloc_free_shadow_model) {
    cleanup_test_img();
    blkdev_t dev;
    uint32_t total = 512;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, total));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    bool *shadow = (bool *)th_calloc(total, sizeof(bool));
    ASSERT_TRUE(shadow != NULL);

    /* Mark metadata blocks as used in shadow */
    for (uint32_t b = 0; b < fs.sb.data_start; b++) {
        shadow[b] = true;
    }

    srand(12345);
    int active_allocs = 0;

    for (int op = 0; op < 100000; op++) {
        int action = rand() % 2;
        if (action == 0 || active_allocs == 0) {
            /* Alloc */
            uint32_t goal = (uint32_t)(rand() % (int)total);
            int blk = alloc_block(&fs, goal);
            if (blk >= 0) {
                ASSERT_TRUE((uint32_t)blk >= fs.sb.data_start);
                ASSERT_TRUE((uint32_t)blk < total);
                ASSERT_FALSE(shadow[blk]); /* Must not have been allocated */
                shadow[blk] = true;
                active_allocs++;
            } else {
                ASSERT_EQ(blk, -ENOSPC);
            }
        } else {
            /* Free a random block */
            uint32_t target = fs.sb.data_start + (uint32_t)(rand() % (int)(total - fs.sb.data_start));
            if (shadow[target]) {
                ASSERT_OK(free_block(&fs, target));
                shadow[target] = false;
                active_allocs--;
            } else {
                ASSERT_ERR(free_block(&fs, target), -EINVAL);
            }
        }
    }

    /* Verify shadow vs bitmap */
    for (uint32_t b = 0; b < total; b++) {
        int is_set = bm_test(&fs.bbitmap, b);
        ASSERT_EQ(is_set, shadow[b] ? 1 : 0);
    }

    th_free(shadow);
    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test4_free_blocks_equals_total_minus_popcount) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    /* Test across multiple allocations and frees */
    for (int i = 0; i < 20; i++) {
        int blk = alloc_block(&fs, 0);
        ASSERT_OK(blk);
        uint32_t used = bm_count_used(&fs.bbitmap);
        ASSERT_EQ(fs.sb.free_blocks, fs.sb.total_blocks - used);
    }

    for (int i = 0; i < 10; i++) {
        uint32_t blk = fs.sb.data_start + (uint32_t)i;
        ASSERT_OK(free_block(&fs, blk));
        uint32_t used = bm_count_used(&fs.bbitmap);
        ASSERT_EQ(fs.sb.free_blocks, fs.sb.total_blocks - used);
    }

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test5_find_run_crafted_bitmaps) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    /* Data start is around block 69 */
    /* Check run of 4 blocks */
    int run = bm_find_run(&fs.bbitmap, fs.sb.data_start, 4);
    ASSERT_OK(run);
    ASSERT_EQ(run, (int)fs.sb.data_start);

    /* Allocate 2 blocks */
    ASSERT_OK(bm_set(&fs.bbitmap, fs.sb.data_start));
    ASSERT_OK(bm_set(&fs.bbitmap, fs.sb.data_start + 1));

    /* Now run of 4 should start at data_start + 2 */
    run = bm_find_run(&fs.bbitmap, fs.sb.data_start, 4);
    ASSERT_OK(run);
    ASSERT_EQ(run, (int)(fs.sb.data_start + 2));

    /* Impossible run larger than disk */
    ASSERT_ERR(bm_find_run(&fs.bbitmap, 0, 1000), -ENOSPC);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test6_goal_based_allocation_contiguity) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int b1 = alloc_block(&fs, fs.sb.data_start);
    ASSERT_OK(b1);

    /* Requesting goal b1 should return b1 + 1 */
    int b2 = alloc_block(&fs, (uint32_t)b1);
    ASSERT_OK(b2);
    ASSERT_EQ(b2, b1 + 1);

    int b3 = alloc_block(&fs, (uint32_t)b2);
    ASSERT_OK(b3);
    ASSERT_EQ(b3, b2 + 1);

    ASSERT_TRUE(fs.alloc_stats.adjacent_allocs >= 2);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m3_test7_boundary_bits_and_persistence) {
    cleanup_test_img();
    blkdev_t dev;
    /* Create 32768+64 block image (~128 MB) to test 32768 bit boundary */
    uint32_t nblocks = 32832;
    ASSERT_OK(blk_open(&dev, TEST_BM_IMG, 1, nblocks));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t boundaries[] = {7, 8, 63, 64, 32767, 32768, nblocks - 1};
    for (size_t i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        uint32_t b = boundaries[i];
        if (b >= fs.sb.data_start) {
            ASSERT_EQ(bm_test(&fs.bbitmap, b), 0);
            ASSERT_OK(bm_set(&fs.bbitmap, b));
            ASSERT_EQ(bm_test(&fs.bbitmap, b), 1);
        }
    }

    blk_close(&dev);

    /* Persistence check: Reopen and verify bits survived */
    blkdev_t dev2;
    ASSERT_OK(blk_open(&dev2, TEST_BM_IMG, 0, 0));
    fs_t fs2;
    ASSERT_OK(fs_init_allocators(&fs2, &dev2));

    for (size_t i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        uint32_t b = boundaries[i];
        if (b >= fs.sb.data_start) {
            ASSERT_EQ(bm_test(&fs2.bbitmap, b), 1);
            ASSERT_OK(bm_clear(&fs2.bbitmap, b));
            ASSERT_EQ(bm_test(&fs2.bbitmap, b), 0);
        }
    }

    blk_close(&dev2);
    cleanup_test_img();
}
