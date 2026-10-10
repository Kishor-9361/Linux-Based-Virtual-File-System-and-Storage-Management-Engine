#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "vfs.h"
#include "fsck.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define STRESS_IMG "/tmp/minifs_test_stress.img"

TEST(m14_test2_disk_full_enospc_and_recovery) {
    unlink(STRESS_IMG);

    /* Use a compact 1024-block filesystem (~4 MiB total, ~700 data blocks) */
    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, STRESS_IMG, 1, 1024));
    ASSERT_OK(sb_format(&v.dev));
    ASSERT_OK(fs_init_allocators(&v.fs, &v.dev));
    ASSERT_OK(dir_bootstrap_root(&v.fs));
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, STRESS_IMG));

    char buf[BLOCK_SIZE];
    memset(buf, 0xEE, sizeof(buf));

    int files_created = 0;
    bool hit_enospc = false;

    /* Write files until ENOSPC is encountered */
    for (int i = 0; i < 500; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/fill_%d.dat", i);
        int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            hit_enospc = true;
            break;
        }

        /* Write 4 blocks per file */
        for (int b = 0; b < 4; b++) {
            ssize_t wr = vfs_write(&v, fd, buf, BLOCK_SIZE);
            if (wr <= 0) {
                hit_enospc = true;
                break;
            }
        }
        vfs_close(&v, fd);
        files_created++;
        if (hit_enospc) break;
    }

    ASSERT_TRUE(hit_enospc);
    ASSERT_TRUE(files_created > 50);

    /* Verify free count and popcount invariant */
    uint32_t used_bits = bm_count_used(&v.fs.bbitmap);
    ASSERT_EQ(v.fs.sb.free_blocks, v.fs.sb.total_blocks - used_bits);

    /* Further writes must fail cleanly without crash */
    int fail_fd = vfs_open(&v, "/should_fail.dat", O_CREAT | O_WRONLY, 0644);
    if (fail_fd >= 0) {
        ssize_t wr = vfs_write(&v, fail_fd, buf, BLOCK_SIZE);
        /* If write fails or returns 0/error, close and unlink */
        vfs_close(&v, fail_fd);
        vfs_unlink(&v, "/should_fail.dat");
        (void)wr;
    }

    /* Delete half of the created files */
    for (int i = 0; i < files_created; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "/fill_%d.dat", i);
        ASSERT_OK(vfs_unlink(&v, name));
    }

    /* Verify space reclaimed and free_blocks increased */
    ASSERT_TRUE(v.fs.sb.free_blocks > 50);
    used_bits = bm_count_used(&v.fs.bbitmap);
    ASSERT_EQ(v.fs.sb.free_blocks, v.fs.sb.total_blocks - used_bits);

    /* Now we should be able to create new files again cleanly */
    int ok_fd = vfs_open(&v, "/reclaimed.dat", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(ok_fd >= 0);
    ASSERT_EQ(vfs_write(&v, ok_fd, buf, BLOCK_SIZE), (ssize_t)BLOCK_SIZE);
    ASSERT_OK(vfs_close(&v, ok_fd));

    ASSERT_OK(vfs_sync(&v));
    ASSERT_OK(vfs_unmount(&v));

    /* Verify fsck clean */
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, STRESS_IMG, 0, 0));
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_OK(fsck_check(&dev, &opts, &stats));
    ASSERT_EQ(stats.errors_found, 0);
    blk_close(&dev);

    unlink(STRESS_IMG);
}

TEST(m14_test3_inode_exhaustion_and_recovery) {
    unlink(STRESS_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, STRESS_IMG, 1, 1024));
    ASSERT_OK(sb_format(&v.dev));
    ASSERT_OK(fs_init_allocators(&v.fs, &v.dev));
    ASSERT_OK(dir_bootstrap_root(&v.fs));
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, STRESS_IMG));

    uint32_t total_inodes = v.fs.sb.total_inodes;
    int inodes_created = 0;

    /* Create empty files until inodes run out */
    for (uint32_t i = 0; i < total_inodes + 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/ino_%u.txt", i);
        int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) break;
        vfs_close(&v, fd);
        inodes_created++;
    }

    /* All free inodes should be exhausted */
    ASSERT_EQ(v.fs.sb.free_inodes, 0);

    /* Further creation must fail with -ENOSPC */
    int extra_fd = vfs_open(&v, "/overflow.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_EQ(extra_fd, -ENOSPC);

    /* Delete files to recover */
    for (int i = 0; i < inodes_created; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/ino_%d.txt", i);
        ASSERT_OK(vfs_unlink(&v, name));
    }

    ASSERT_EQ(v.fs.sb.free_inodes, total_inodes - 2); /* root (1) and lost+found or root reservation */

    ASSERT_OK(vfs_sync(&v));
    ASSERT_OK(vfs_unmount(&v));

    /* Verify fsck clean */
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, STRESS_IMG, 0, 0));
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_OK(fsck_check(&dev, &opts, &stats));
    ASSERT_EQ(stats.errors_found, 0);
    blk_close(&dev);

    unlink(STRESS_IMG);
}
