#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "fsck.h"
#include "vfs.h"
#include "dir.h"
#include "inode.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define FSCK_IMG "/tmp/minifs_fsck_test.img"

static void setup_test_image(void) {
    unlink(FSCK_IMG);
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));
    blk_close(&dev);
}

static void populate_vfs_tree(void) {
    setup_test_image();
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, FSCK_IMG));

    ASSERT_OK(vfs_mkdir(&v, "/docs", 0755));
    ASSERT_OK(vfs_mkdir(&v, "/docs/sub", 0755));

    int fd1 = vfs_open(&v, "/docs/file1.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd1 >= 0);
    ASSERT_EQ(vfs_write(&v, fd1, "Hello World from File 1", 23), 23);
    ASSERT_OK(vfs_close(&v, fd1));

    int fd2 = vfs_open(&v, "/docs/sub/file2.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd2 >= 0);
    char buf[8192];
    memset(buf, 'A', sizeof(buf));
    ASSERT_EQ(vfs_write(&v, fd2, buf, sizeof(buf)), (ssize_t)sizeof(buf));
    ASSERT_OK(vfs_close(&v, fd2));

    ASSERT_OK(vfs_link(&v, "/docs/file1.txt", "/docs/file1_link.txt"));

    ASSERT_OK(vfs_unmount(&v));
}

TEST(m11_test1_clean_image_zero_errors) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));

    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    int rc = fsck_check(&dev, &opts, &stats);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test2_bitmap_leak_and_missing_alloc) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));

    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* 1. Corrupt block bitmap: clear an allocated block bit -> missing allocation */
    uint8_t bb[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, sb.bbitmap_start, bb));
    uint32_t allocated_blk = 0;
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++) {
        if (bb[b / 8] & (1 << (b % 8))) {
            allocated_blk = b;
            bb[b / 8] &= (uint8_t)~(1 << (b % 8)); /* Clear it */
            break;
        }
    }
    ASSERT_TRUE(allocated_blk > 0);

    /* 2. Corrupt block bitmap: set a free block bit -> leaked block */
    uint32_t free_blk = 0;
    for (uint32_t b = sb.data_start; b < sb.total_blocks; b++) {
        if (b != allocated_blk && !(bb[b / 8] & (1 << (b % 8)))) {
            free_blk = b;
            bb[b / 8] |= (uint8_t)(1 << (b % 8)); /* Set it */
            break;
        }
    }
    ASSERT_TRUE(free_blk > 0);

    ASSERT_OK(blk_write(&dev, sb.bbitmap_start, bb));
    ASSERT_OK(blk_sync(&dev));

    /* Check in check-only mode */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    int rc = fsck_check(&dev, &opts, &stats);
    ASSERT_EQ(rc, 1);
    ASSERT_TRUE(stats.errors_found >= 2);
    ASSERT_EQ(stats.missing_alloc_blocks, 1);
    ASSERT_EQ(stats.leaked_blocks, 1);

    /* Run in repair mode */
    opts.repair = true;
    rc = fsck_check(&dev, &opts, &stats);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(stats.errors_found, stats.errors_fixed);

    /* Second run must report 0 errors (idempotent) */
    opts.repair = false;
    rc = fsck_check(&dev, &opts, &stats);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test3_bad_link_counts) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Corrupt nlink of an existing file (inode 2) */
    uint32_t ino = 2;
    uint32_t blk = sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, blk, buf));
    inode_t *node = (inode_t *)(buf + off);
    uint16_t orig_nlink = node->nlink;
    node->nlink = (uint16_t)(orig_nlink + 5);
    ASSERT_OK(blk_write(&dev, blk, buf));
    ASSERT_OK(blk_sync(&dev));

    /* Check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);
    ASSERT_TRUE(stats.bad_link_counts > 0);

    /* Repair */
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, stats.errors_fixed);

    /* Idempotent re-check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test4_out_of_range_pointers) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Inject out-of-range pointer into an inode */
    uint32_t ino = 2;
    uint32_t blk = sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, blk, buf));
    inode_t *node = (inode_t *)(buf + off);
    node->direct[5] = sb.total_blocks + 100; /* Beyond total blocks */
    ASSERT_OK(blk_write(&dev, blk, buf));
    ASSERT_OK(blk_sync(&dev));

    /* Check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);
    ASSERT_TRUE(stats.bad_pointers > 0);

    /* Repair */
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);

    /* Idempotent re-check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test5_duplicate_block_claim) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Find block of inode 2 and set it as direct[0] of inode 3 */
    uint32_t blk2 = sb.itable_start + ((2 - 1) / 32);
    size_t off2 = ((size_t)(2 - 1) % 32) * sizeof(inode_t);
    uint8_t buf2[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, blk2, buf2));
    inode_t *node2 = (inode_t *)(buf2 + off2);
    uint32_t shared_blk = node2->direct[0];
    ASSERT_TRUE(shared_blk > 0);

    uint32_t blk3 = sb.itable_start + ((3 - 1) / 32);
    size_t off3 = ((size_t)(3 - 1) % 32) * sizeof(inode_t);
    uint8_t buf3[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, blk3, buf3));
    inode_t *node3 = (inode_t *)(buf3 + off3);
    node3->direct[1] = shared_blk; /* Now both inode 2 and 3 claim shared_blk */
    ASSERT_OK(blk_write(&dev, blk3, buf3));
    ASSERT_OK(blk_sync(&dev));

    /* Check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);
    ASSERT_TRUE(stats.duplicate_blocks > 0);

    /* Repair */
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);

    /* Idempotent re-check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test6_dentry_to_free_inode_and_orphan) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Set unreferenced inode 10 as allocated with size > 0 (orphan with data) */
    uint8_t ib[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, sb.ibitmap_start, ib));
    uint32_t orphan_ino = 10;
    ib[orphan_ino / 8] |= (uint8_t)(1 << (orphan_ino % 8));
    ASSERT_OK(blk_write(&dev, sb.ibitmap_start, ib));

    uint32_t tbl_blk = sb.itable_start + ((orphan_ino - 1) / 32);
    size_t off = ((size_t)(orphan_ino - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, tbl_blk, buf));
    inode_t *node = (inode_t *)(buf + off);
    memset(node, 0, sizeof(inode_t));
    node->mode = S_IFREG | 0644;
    node->size = 100;
    node->nlink = 1;
    ASSERT_OK(blk_write(&dev, tbl_blk, buf));
    ASSERT_OK(blk_sync(&dev));

    /* Check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);
    ASSERT_TRUE(stats.orphan_inodes > 0);

    /* Repair */
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);

    /* Idempotent re-check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test7_bad_dot_dotdot) {
    populate_vfs_tree();

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));

    /* Find /docs directory (inode 2), corrupt its '.' entry */
    uint32_t blk = sb.itable_start + ((2 - 1) / 32);
    size_t off = ((size_t)(2 - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, blk, buf));
    inode_t *node = (inode_t *)(buf + off);
    uint32_t dir_blk = node->direct[0];
    ASSERT_TRUE(dir_blk > 0);

    uint8_t dblk[BLOCK_SIZE];
    ASSERT_OK(blk_read(&dev, dir_blk, dblk));
    dirent_t *de = (dirent_t *)dblk;
    de[0].ino = 999; /* Corrupt '.' pointer */
    ASSERT_OK(blk_write(&dev, dir_blk, dblk));
    ASSERT_OK(blk_sync(&dev));

    /* Check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);

    /* Repair */
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);

    /* Idempotent re-check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}

TEST(m11_test8_superblock_free_counts_and_readonly) {
    populate_vfs_tree();

    /* Compute initial checksum/hash of image */
    int fd = open(FSCK_IMG, O_RDONLY);
    ASSERT_TRUE(fd >= 0);
    char init_data[1024 * 64];
    ssize_t rd = read(fd, init_data, sizeof(init_data));
    ASSERT_TRUE(rd > 0);
    close(fd);

    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));

    /* Corrupt free count in superblock */
    superblock_t sb;
    ASSERT_OK(sb_load(&dev, &sb));
    sb.free_blocks += 50;
    ASSERT_OK(sb_store(&dev, &sb));

    /* Read-only fsck check */
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 1);
    ASSERT_TRUE(stats.sb_errors > 0);

    blk_close(&dev);

    /* Now open and repair */
    ASSERT_OK(blk_open(&dev, FSCK_IMG, 0, 0));
    opts.repair = true;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);

    /* Idempotent check */
    opts.repair = false;
    ASSERT_EQ(fsck_check(&dev, &opts, &stats), 0);
    ASSERT_EQ(stats.errors_found, 0);

    blk_close(&dev);
    unlink(FSCK_IMG);
}
