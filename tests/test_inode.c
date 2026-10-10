#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "inode.h"
#include "bitmap.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>

#define TEST_INO_IMG "/tmp/minifs_test_ino.img"

static void cleanup_test_img(void) {
    unlink(TEST_INO_IMG);
}

TEST(m4_test1_roundtrip_1000_random_inodes_and_boundaries) {
    cleanup_test_img();
    blkdev_t dev;
    /* 4096 blocks = 1024 inodes */
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 4096));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t total_inodes = fs.sb.total_inodes;
    uint32_t boundary_inos[] = {1, 32, 33, total_inodes - 1};

    for (size_t i = 0; i < sizeof(boundary_inos)/sizeof(boundary_inos[0]); i++) {
        uint32_t ino = boundary_inos[i];
        inode_t in, out;
        memset(&in, 0, sizeof(in));
        in.mode = 0644;
        in.uid = (uint16_t)(1000 + i);
        in.gid = (uint16_t)(1000 + i);
        in.size = 12345678ULL + i;
        in.direct[0] = 42 + (uint32_t)i;
        in.indirect = 99 + (uint32_t)i;

        ASSERT_OK(inode_write(&fs, ino, &in));
        ASSERT_OK(inode_read(&fs, ino, &out));
        ASSERT_MEM_EQ(&in, &out, sizeof(inode_t));
    }

    /* 1000 random inodes roundtrip */
    srand(999);
    for (int i = 0; i < 1000; i++) {
        uint32_t ino = 1 + (uint32_t)(rand() % (int)(total_inodes - 1));
        inode_t in, out;
        memset(&in, 0, sizeof(in));
        in.mode = (uint16_t)rand();
        in.uid = (uint16_t)rand();
        in.gid = (uint16_t)rand();
        in.nlink = (uint16_t)rand();
        in.size = (uint64_t)rand();
        in.direct[0] = (uint32_t)rand();
        in.direct[11] = (uint32_t)rand();
        in.indirect = (uint32_t)rand();
        in.dindirect = (uint32_t)rand();

        ASSERT_OK(inode_write(&fs, ino, &in));
        ASSERT_OK(inode_read(&fs, ino, &out));
        ASSERT_MEM_EQ(&in, &out, sizeof(inode_t));
    }

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test2_create_until_enospc_and_destroy_all) {
    cleanup_test_img();
    blkdev_t dev;
    /* Small image with 128 inodes */
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t initial_free_inodes = fs.sb.free_inodes;
    uint32_t inos[256];
    uint32_t count = 0;

    while (1) {
        int res = inode_create(&fs, 0644, 1000, 1000);
        if (res == -ENOSPC) break;
        ASSERT_OK(res);
        inos[count++] = (uint32_t)res;
    }

    ASSERT_EQ(count, initial_free_inodes);
    ASSERT_EQ(fs.sb.free_inodes, 0);

    /* Destroy all created inodes */
    for (uint32_t i = 0; i < count; i++) {
        ASSERT_OK(inode_destroy(&fs, inos[i]));
    }

    ASSERT_EQ(fs.sb.free_inodes, initial_free_inodes);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test3_bmap_boundary_cases) {
    cleanup_test_img();
    blkdev_t dev;
    /* 16 MB image (4096 blocks) */
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 4096));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino_id = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino_id);
    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino_id, &node));

    uint32_t boundaries[] = {
        0,                                                  /* First direct */
        11,                                                 /* Last direct */
        12,                                                 /* First single indirect */
        1035,                                               /* Last single indirect */
        1036,                                               /* First double indirect */
        1036 + 1023,                                        /* End of first l2 block */
        1036 + 1024                                         /* Start of second l2 block */
    };

    for (size_t i = 0; i < sizeof(boundaries)/sizeof(boundaries[0]); i++) {
        uint32_t lbn = boundaries[i];
        uint32_t pbn = 0;
        ASSERT_OK(bmap(&fs, &node, lbn, 1, &pbn));
        ASSERT_TRUE(pbn >= fs.sb.data_start);
        ASSERT_TRUE(pbn < fs.sb.total_blocks);

        /* Read back with alloc=0 */
        uint32_t pbn_read = 0;
        ASSERT_OK(bmap(&fs, &node, lbn, 0, &pbn_read));
        ASSERT_EQ(pbn, pbn_read);
    }

    /* Save updated inode */
    ASSERT_OK(inode_write(&fs, (uint32_t)ino_id, &node));

    /* Test max bounds and EFBIG */
    uint32_t dummy_pbn;
    ASSERT_ERR(bmap(&fs, &node, MAX_FILE_BLOCKS, 0, &dummy_pbn), -EFBIG);
    ASSERT_ERR(bmap(&fs, &node, MAX_FILE_BLOCKS + 1, 1, &dummy_pbn), -EFBIG);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test4_map_5000_consecutive_blocks) {
    cleanup_test_img();
    blkdev_t dev;
    /* 32 MB image = 8192 blocks */
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 8192));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino_id = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino_id);
    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino_id, &node));

    /* Map 5000 consecutive blocks */
    uint32_t *mapped = (uint32_t *)th_malloc(5000 * sizeof(uint32_t));
    ASSERT_TRUE(mapped != NULL);

    for (uint32_t lbn = 0; lbn < 5000; lbn++) {
        ASSERT_OK(bmap(&fs, &node, lbn, 1, &mapped[lbn]));
        ASSERT_TRUE(mapped[lbn] >= fs.sb.data_start);
    }

    /* Read back all 5000 blocks and verify uniqueness */
    for (uint32_t lbn = 0; lbn < 5000; lbn++) {
        uint32_t pbn = 0;
        ASSERT_OK(bmap(&fs, &node, lbn, 0, &pbn));
        ASSERT_EQ(pbn, mapped[lbn]);
    }

    th_free(mapped);
    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test5_holes_alloc_zero_returns_zero) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino_id = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino_id);
    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino_id, &node));

    uint32_t baseline_used = bm_count_used(&fs.bbitmap);

    /* Query unmapped lbn with alloc=0 across direct, indirect, and dindirect */
    uint32_t test_lbns[] = {0, 5, 11, 12, 500, 1035, 1036, 5000};
    for (size_t i = 0; i < sizeof(test_lbns)/sizeof(test_lbns[0]); i++) {
        uint32_t pbn = 9999;
        ASSERT_OK(bmap(&fs, &node, test_lbns[i], 0, &pbn));
        ASSERT_EQ(pbn, 0);
    }

    /* Verify no blocks allocated */
    ASSERT_EQ(bm_count_used(&fs.bbitmap), baseline_used);
    ASSERT_EQ(node.blocks, 0);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test6_free_blocks_from_zero_restores_baseline_no_leak) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 2048));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t baseline_blocks = bm_count_used(&fs.bbitmap);

    int ino_id = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino_id);
    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino_id, &node));

    /* Map across direct, indirect, and double indirect */
    uint32_t pbn;
    ASSERT_OK(bmap(&fs, &node, 5, 1, &pbn));      /* direct */
    ASSERT_OK(bmap(&fs, &node, 100, 1, &pbn));    /* indirect */
    ASSERT_OK(bmap(&fs, &node, 1500, 1, &pbn));   /* double indirect */

    ASSERT_TRUE(bm_count_used(&fs.bbitmap) > baseline_blocks);

    /* Free all blocks */
    ASSERT_OK(inode_free_blocks_from(&fs, &node, 0));
    ASSERT_OK(inode_write(&fs, (uint32_t)ino_id, &node));

    /* Baseline must be fully restored (zero leaks of data or indirect pointer blocks) */
    ASSERT_EQ(bm_count_used(&fs.bbitmap), baseline_blocks);
    ASSERT_EQ(node.blocks, 0);

    blk_close(&dev);
    cleanup_test_img();
}

TEST(m4_test7_invalid_inode_numbers_return_einval) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_INO_IMG, 1, 256));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    inode_t node;
    ASSERT_ERR(inode_read(&fs, 0, &node), -EINVAL);
    ASSERT_ERR(inode_read(&fs, fs.sb.total_inodes, &node), -EINVAL);
    ASSERT_ERR(inode_read(&fs, fs.sb.total_inodes + 10, &node), -EINVAL);

    ASSERT_ERR(inode_write(&fs, 0, &node), -EINVAL);
    ASSERT_ERR(inode_write(&fs, fs.sb.total_inodes, &node), -EINVAL);

    ASSERT_ERR(inode_destroy(&fs, 0), -EINVAL);
    ASSERT_ERR(inode_destroy(&fs, fs.sb.total_inodes), -EINVAL);

    blk_close(&dev);
    cleanup_test_img();
}
