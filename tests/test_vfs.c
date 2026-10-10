#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "vfs.h"
#include "common.h"
#include <unistd.h>
#include <fcntl.h>

#define TEST_VFS_IMG "/tmp/minifs_test_vfs.img"

static void format_clean_vfs(const char *img_path, uint32_t nblocks) {
    unlink(img_path);
    blkdev_t dev;
    blk_open(&dev, img_path, 1, nblocks);
    sb_format(&dev);
    fs_t fs;
    fs_init_allocators(&fs, &dev);
    dir_bootstrap_root(&fs);
    blk_close(&dev);
}

TEST(m7_test1_posix_semantic_cases) {
    format_clean_vfs(TEST_VFS_IMG, 4096);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_VFS_IMG));

    /* 1. Create / Write / Read */
    int fd = vfs_open(&v, "/test.txt", O_RDWR | O_CREAT, 0644);
    ASSERT_TRUE(fd >= 3);
    char msg[] = "Hello Minifs POSIX!";
    ASSERT_EQ(vfs_write(&v, fd, msg, strlen(msg)), (ssize_t)strlen(msg));

    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    ASSERT_OK(vfs_lseek(&v, fd, 0, SEEK_SET));
    ASSERT_EQ(vfs_read(&v, fd, rbuf, sizeof(rbuf)), (ssize_t)strlen(msg));
    ASSERT_STR_EQ(msg, rbuf);
    ASSERT_OK(vfs_close(&v, fd));

    /* 2. O_EXCL with existing file fails with -EEXIST */
    ASSERT_ERR(vfs_open(&v, "/test.txt", O_CREAT | O_EXCL | O_RDWR, 0644), -EEXIST);

    /* 3. O_TRUNC resets file to 0 size */
    fd = vfs_open(&v, "/test.txt", O_RDWR | O_TRUNC, 0644);
    ASSERT_TRUE(fd >= 3);
    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&v, "/test.txt", &st));
    ASSERT_EQ(st.st_size, 0);
    ASSERT_OK(vfs_close(&v, fd));

    /* 4. O_APPEND automatically appends to end of file */
    fd = vfs_open(&v, "/append.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
    ASSERT_TRUE(fd >= 3);
    ASSERT_EQ(vfs_write(&v, fd, "AAA", 3), 3);
    ASSERT_OK(vfs_lseek(&v, fd, 0, SEEK_SET)); /* Seek to 0, but append must still write to end! */
    ASSERT_EQ(vfs_write(&v, fd, "BBB", 3), 3);
    ASSERT_OK(vfs_close(&v, fd));

    fd = vfs_open(&v, "/append.txt", O_RDONLY, 0);
    char app_buf[16];
    memset(app_buf, 0, sizeof(app_buf));
    ASSERT_EQ(vfs_read(&v, fd, app_buf, sizeof(app_buf)), 6);
    ASSERT_STR_EQ(app_buf, "AAABBB");
    ASSERT_OK(vfs_close(&v, fd));

    /* 5. lseek variants */
    fd = vfs_open(&v, "/append.txt", O_RDONLY, 0);
    ASSERT_EQ(vfs_lseek(&v, fd, 2, SEEK_SET), 2);
    ASSERT_EQ(vfs_lseek(&v, fd, 2, SEEK_CUR), 4);
    ASSERT_EQ(vfs_lseek(&v, fd, -1, SEEK_END), 5);
    ASSERT_ERR(vfs_lseek(&v, fd, -10, SEEK_SET), -EINVAL);
    ASSERT_OK(vfs_close(&v, fd));

    /* 6. Rename over existing file replaces atomically */
    fd = vfs_open(&v, "/src.txt", O_WRONLY | O_CREAT, 0644);
    ASSERT_EQ(vfs_write(&v, fd, "NEW_CONTENT", 11), 11);
    ASSERT_OK(vfs_close(&v, fd));

    ASSERT_OK(vfs_rename(&v, "/src.txt", "/append.txt"));
    ASSERT_ERR(vfs_stat(&v, "/src.txt", &st), -ENOENT);
    ASSERT_OK(vfs_stat(&v, "/append.txt", &st));
    ASSERT_EQ(st.st_size, 11);

    /* 7. Rename directory into its own subtree returns -EINVAL */
    ASSERT_OK(vfs_mkdir(&v, "/dirA", 0755));
    ASSERT_OK(vfs_mkdir(&v, "/dirA/subB", 0755));
    ASSERT_OK(vfs_mkdir(&v, "/dirA/subB/subC", 0755));
    ASSERT_ERR(vfs_rename(&v, "/dirA", "/dirA/subB/subC/nested"), -EINVAL);

    /* 8. Unlink-while-open (orphan semantics) */
    fd = vfs_open(&v, "/unlinked_open.txt", O_RDWR | O_CREAT, 0644);
    ASSERT_TRUE(fd >= 3);
    char secret[] = "STILL_READABLE";
    ASSERT_EQ(vfs_write(&v, fd, secret, strlen(secret)), (ssize_t)strlen(secret));
    ASSERT_OK(vfs_unlink(&v, "/unlinked_open.txt"));

    /* Path lookup fails */
    ASSERT_ERR(vfs_stat(&v, "/unlinked_open.txt", &st), -ENOENT);

    /* But open file is still readable! */
    ASSERT_OK(vfs_lseek(&v, fd, 0, SEEK_SET));
    char secret_read[32];
    memset(secret_read, 0, sizeof(secret_read));
    ASSERT_EQ(vfs_read(&v, fd, secret_read, sizeof(secret_read)), (ssize_t)strlen(secret));
    ASSERT_STR_EQ(secret, secret_read);

    /* Once closed, space is reclaimed */
    ASSERT_OK(vfs_close(&v, fd));

    /* 9. Hard link then unlink one */
    fd = vfs_open(&v, "/original.txt", O_WRONLY | O_CREAT, 0644);
    ASSERT_OK(vfs_close(&v, fd));
    ASSERT_OK(vfs_link(&v, "/original.txt", "/link1.txt"));
    ASSERT_OK(vfs_stat(&v, "/original.txt", &st));
    ASSERT_EQ(st.st_nlink, 2);
    ASSERT_OK(vfs_unlink(&v, "/original.txt"));
    ASSERT_OK(vfs_stat(&v, "/link1.txt", &st));
    ASSERT_EQ(st.st_nlink, 1);
    ASSERT_OK(vfs_unlink(&v, "/link1.txt"));

    /* 10. rmdir non-empty fails with -ENOTEMPTY */
    ASSERT_OK(vfs_mkdir(&v, "/notempty", 0755));
    fd = vfs_open(&v, "/notempty/child.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_OK(vfs_close(&v, fd));
    ASSERT_ERR(vfs_rmdir(&v, "/notempty"), -ENOTEMPTY);
    ASSERT_OK(vfs_unlink(&v, "/notempty/child.txt"));
    ASSERT_OK(vfs_rmdir(&v, "/notempty"));

    /* 11. Bad fd */
    ASSERT_ERR(vfs_read(&v, 99, rbuf, 10), -EBADF);
    ASSERT_ERR(vfs_close(&v, 2), -EBADF);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_VFS_IMG);
}

TEST(m7_test2_remount_persistence) {
    format_clean_vfs(TEST_VFS_IMG, 2048);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_VFS_IMG));

    ASSERT_OK(vfs_mkdir(&v, "/folder", 0755));
    int fd = vfs_open(&v, "/folder/persistent.txt", O_WRONLY | O_CREAT, 0644);
    ASSERT_TRUE(fd >= 3);
    char data[] = "PersistedAcrossMounts";
    ASSERT_EQ(vfs_write(&v, fd, data, strlen(data)), (ssize_t)strlen(data));
    ASSERT_OK(vfs_close(&v, fd));

    ASSERT_OK(vfs_unmount(&v));

    /* Remount */
    vfs_t v2;
    ASSERT_OK(vfs_mount(&v2, TEST_VFS_IMG));

    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&v2, "/folder/persistent.txt", &st));
    ASSERT_EQ(st.st_size, strlen(data));

    fd = vfs_open(&v2, "/folder/persistent.txt", O_RDONLY, 0);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(vfs_read(&v2, fd, buf, sizeof(buf)), (ssize_t)strlen(data));
    ASSERT_STR_EQ(data, buf);
    ASSERT_OK(vfs_close(&v2, fd));

    ASSERT_OK(vfs_unmount(&v2));
    unlink(TEST_VFS_IMG);
}
