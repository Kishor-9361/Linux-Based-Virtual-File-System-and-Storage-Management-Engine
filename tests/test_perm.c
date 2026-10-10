#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "perm.h"
#include "vfs.h"
#include "common.h"
#include <unistd.h>

#define TEST_PERM_IMG "/tmp/minifs_test_perm.img"

static int ref_perm_check(uint16_t mode, uid_t file_uid, gid_t file_gid, uid_t u, gid_t g, int want) {
    if (u == 0) {
        if ((want & X_OK) && !(mode & S_IFDIR)) {
            if ((mode & 0111) == 0) return -EACCES;
        }
        return 0;
    }
    int shift = (u == file_uid) ? 6 : ((g == file_gid) ? 3 : 0);
    int bits = (mode >> shift) & 7;
    return ((bits & want) == want) ? 0 : -EACCES;
}

TEST(m8_test1_full_permission_matrix_100_percent) {
    uid_t file_uid = 1000;
    gid_t file_gid = 1000;

    struct {
        const char *desc;
        uid_t u;
        gid_t g;
    } users[] = {
        {"owner", 1000, 1000},
        {"group", 2000, 1000},
        {"other", 3000, 3000},
        {"root",  0,    0}
    };

    int wants[] = {
        R_OK,
        W_OK,
        X_OK,
        R_OK | W_OK,
        R_OK | X_OK,
        W_OK | X_OK,
        R_OK | W_OK | X_OK
    };

    uint16_t types[] = {S_IFREG, S_IFDIR};

    int total_evaluated = 0;

    for (size_t usr = 0; usr < sizeof(users)/sizeof(users[0]); usr++) {
        uid_t u = users[usr].u;
        gid_t g = users[usr].g;

        for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
            uint16_t type = types[t];

            for (size_t w = 0; w < sizeof(wants)/sizeof(wants[0]); w++) {
                int want = wants[w];

                for (uint16_t mode_oct = 0; mode_oct <= 0777; mode_oct++) {
                    inode_t node;
                    memset(&node, 0, sizeof(node));
                    node.mode = type | mode_oct;
                    node.uid = (uint16_t)file_uid;
                    node.gid = (uint16_t)file_gid;

                    int actual = perm_check(&node, u, g, want);
                    int expected = ref_perm_check(node.mode, file_uid, file_gid, u, g, want);

                    ASSERT_EQ(actual, expected);
                    total_evaluated++;
                }
            }
        }
    }

    /* 4 users * 2 types * 7 wants * 512 modes = 28,672 evaluations */
    ASSERT_EQ(total_evaluated, 28672);
}

static void format_vfs_perm(const char *img_path) {
    unlink(img_path);
    blkdev_t dev;
    blk_open(&dev, img_path, 1, 2048);
    sb_format(&dev);
    fs_t fs;
    fs_init_allocators(&fs, &dev);
    dir_bootstrap_root(&fs);
    blk_close(&dev);
}

TEST(m8_test2_traversal_denial_without_x) {
    format_vfs_perm(TEST_PERM_IMG);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_PERM_IMG));

    ASSERT_OK(vfs_mkdir(&v, "/parent", 0755));
    ASSERT_OK(vfs_mkdir(&v, "/parent/child", 0755));
    int fd = vfs_open(&v, "/parent/child/file.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd >= 3);
    ASSERT_OK(vfs_close(&v, fd));

    /* Owner drops execute permission on middle directory: 0755 -> 0644 */
    ASSERT_OK(vfs_chmod(&v, "/parent", 0644));

    /* Non-root user attempts traversal */
    vfs_set_user(&v, 1000, 1000);

    /* Resolving /parent/child/file.txt requires X on /parent */
    struct vfs_stat st;
    ASSERT_ERR(vfs_stat(&v, "/parent/child/file.txt", &st), -EACCES);
    ASSERT_ERR(vfs_open(&v, "/parent/child/file.txt", O_RDONLY, 0), -EACCES);

    /* Root can still traverse */
    vfs_set_user(&v, 0, 0);
    ASSERT_OK(vfs_stat(&v, "/parent/child/file.txt", &st));

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_PERM_IMG);
}

TEST(m8_test3_create_and_unlink_denial_without_w) {
    format_vfs_perm(TEST_PERM_IMG);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_PERM_IMG));

    ASSERT_OK(vfs_mkdir(&v, "/readonly_dir", 0755));
    int fd = vfs_open(&v, "/readonly_dir/existing.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd >= 3);
    ASSERT_OK(vfs_close(&v, fd));

    /* Remove write bit: 0755 -> 0555 */
    ASSERT_OK(vfs_chmod(&v, "/readonly_dir", 0555));

    /* Non-root user */
    vfs_set_user(&v, 1000, 1000);

    /* Cannot create new file in read-only directory */
    ASSERT_ERR(vfs_open(&v, "/readonly_dir/new.txt", O_CREAT | O_WRONLY, 0644), -EACCES);

    /* Cannot unlink existing file from read-only directory */
    ASSERT_ERR(vfs_unlink(&v, "/readonly_dir/existing.txt"), -EACCES);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_PERM_IMG);
}

TEST(m8_test4_chmod_chown_by_non_owner_eperm) {
    format_vfs_perm(TEST_PERM_IMG);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_PERM_IMG));

    /* Root creates file owned by user 1000 */
    int fd = vfs_open(&v, "/user_file.txt", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd >= 3);
    ASSERT_OK(vfs_close(&v, fd));
    ASSERT_OK(vfs_chown(&v, "/user_file.txt", 1000, 1000));

    /* Impersonate attacker (user 2000) */
    vfs_set_user(&v, 2000, 2000);

    /* Non-owner cannot chmod */
    ASSERT_ERR(vfs_chmod(&v, "/user_file.txt", 0777), -EPERM);

    /* Non-owner cannot chown */
    ASSERT_ERR(vfs_chown(&v, "/user_file.txt", 2000, 2000), -EPERM);

    /* File owner (user 1000) CAN chmod */
    vfs_set_user(&v, 1000, 1000);
    ASSERT_OK(vfs_chmod(&v, "/user_file.txt", 0700));

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_PERM_IMG);
}

TEST(m8_test5_umask_applied_to_create) {
    format_vfs_perm(TEST_PERM_IMG);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_PERM_IMG));

    /* Set umask to 0077 */
    ASSERT_EQ(vfs_umask(&v, 0077), 0022);

    int fd = vfs_open(&v, "/secret.txt", O_CREAT | O_WRONLY, 0666);
    ASSERT_TRUE(fd >= 3);
    ASSERT_OK(vfs_close(&v, fd));

    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&v, "/secret.txt", &st));
    /* 0666 & ~0077 = 0600 */
    ASSERT_EQ(st.st_mode & 0777, 0600);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_PERM_IMG);
}

TEST(m8_test6_permissions_survive_remount) {
    format_vfs_perm(TEST_PERM_IMG);
    vfs_t v;
    ASSERT_OK(vfs_mount(&v, TEST_PERM_IMG));

    int fd = vfs_open(&v, "/perm_test.txt", O_CREAT | O_WRONLY, 0640);
    ASSERT_TRUE(fd >= 3);
    ASSERT_OK(vfs_close(&v, fd));
    ASSERT_OK(vfs_chown(&v, "/perm_test.txt", 1234, 5678));

    ASSERT_OK(vfs_unmount(&v));

    /* Remount and verify permissions */
    vfs_t v2;
    ASSERT_OK(vfs_mount(&v2, TEST_PERM_IMG));

    struct vfs_stat st;
    ASSERT_OK(vfs_stat(&v2, "/perm_test.txt", &st));
    ASSERT_EQ(st.st_mode & 0777, 0640);
    ASSERT_EQ(st.st_uid, 1234);
    ASSERT_EQ(st.st_gid, 5678);

    ASSERT_OK(vfs_unmount(&v2));
    unlink(TEST_PERM_IMG);
}
