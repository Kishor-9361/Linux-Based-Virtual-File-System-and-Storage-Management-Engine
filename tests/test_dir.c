#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "dir.h"
#include "fileio.h"
#include "inode.h"
#include "bitmap.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>
#include <time.h>

#define TEST_DIR_IMG "/tmp/minifs_test_dir.img"

static void cleanup_test_img(void) {
    unlink(TEST_DIR_IMG);
}

static void test_dir_teardown(fs_t *fs, blkdev_t *dev) {
    if (fs) dir_index_cleanup(fs);
    if (dev) blk_close(dev);
    cleanup_test_img();
}

TEST(m6_test1_deep_tree_25_levels_and_root_dotdot) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 2048));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    /* Check .. from root stays root */
    uint32_t resolved = 0;
    ASSERT_OK(path_resolve(&fs, "/..", 1, &resolved, NULL, NULL));
    ASSERT_EQ(resolved, fs.sb.root_inode);

    ASSERT_OK(path_resolve(&fs, "/../../..", 1, &resolved, NULL, NULL));
    ASSERT_EQ(resolved, fs.sb.root_inode);

    /* Create 25 levels deep */
    uint32_t parent = fs.sb.root_inode;
    char path_buf[1024] = "";

    for (int lvl = 1; lvl <= 25; lvl++) {
        char dirname[16];
        snprintf(dirname, sizeof(dirname), "level%d", lvl);

        int new_ino = inode_create(&fs, S_IFDIR | 0755, 0, 0);
        ASSERT_OK(new_ino);
        ASSERT_OK(dir_init(&fs, (uint32_t)new_ino, parent));
        ASSERT_OK(dir_add(&fs, parent, dirname, (uint32_t)new_ino, MINIFS_FT_DIR));

        strcat(path_buf, "/");
        strcat(path_buf, dirname);
        parent = (uint32_t)new_ino;
    }

    /* Resolve deep path */
    uint32_t deep_ino = 0;
    ASSERT_OK(path_resolve(&fs, path_buf, 1, &deep_ino, NULL, NULL));
    ASSERT_EQ(deep_ino, parent);

    /* Resolve with /.. at end */
    char parent_path[2048];
    snprintf(parent_path, sizeof(parent_path), "%s/..", path_buf);
    uint32_t parent_resolved = 0;
    ASSERT_OK(path_resolve(&fs, parent_path, 1, &parent_resolved, NULL, NULL));

    char expected_parent_path[2048];
    strncpy(expected_parent_path, path_buf, sizeof(expected_parent_path) - 1);
    char *last_slash = strrchr(expected_parent_path, '/');
    *last_slash = '\0';
    uint32_t expected_parent = 0;
    ASSERT_OK(path_resolve(&fs, expected_parent_path, 1, &expected_parent, NULL, NULL));
    ASSERT_EQ(parent_resolved, expected_parent);

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test2_10000_entries_slot_reuse_and_benchmark) {
    cleanup_test_img();
    blkdev_t dev;
    /* 10,000 entries = ~640 KB directory file + inode table. Need ~32 MB disk = 8192 blocks */
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 16384));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    int dir_id = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_id);
    uint32_t dir_ino = (uint32_t)dir_id;
    ASSERT_OK(dir_init(&fs, dir_ino, fs.sb.root_inode));

    /* Create 10,000 entries */
    FILE *csv = fopen("reports/dir_lookup_baseline.csv", "w");
    if (csv) {
        fprintf(csv, "entries,lookup_time_us\n");
    }

    struct timespec t0, t1;

    for (int i = 0; i < 10000; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%05d.dat", i);
        /* Inode number dummy (e.g. (i % 100) + 2) */
        ASSERT_OK(dir_add(&fs, dir_ino, name, (uint32_t)((i % 100) + 2), MINIFS_FT_REG));

        /* Record timing at intervals */
        if (csv && (i == 10 || i == 100 || i == 1000 || i == 5000 || i == 9999)) {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            uint32_t dummy = 0;
            dir_lookup(&fs, dir_ino, name, &dummy);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double us = (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
            fprintf(csv, "%d,%.2f\n", i + 1, us);
        }
    }
    if (csv) fclose(csv);

    inode_t node_before;
    ASSERT_OK(inode_read(&fs, dir_ino, &node_before));
    uint64_t size_at_10k = node_before.size;

    /* Delete every other entry (5000 deletions) */
    for (int i = 0; i < 10000; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "file_%05d.dat", i);
        ASSERT_OK(dir_remove(&fs, dir_ino, name));
    }

    /* Re-add them: slots MUST be reused, so size should NOT grow! */
    for (int i = 0; i < 10000; i += 2) {
        char name[32];
        snprintf(name, sizeof(name), "file_%05d.dat", i);
        ASSERT_OK(dir_add(&fs, dir_ino, name, (uint32_t)((i % 100) + 2), MINIFS_FT_REG));
    }

    inode_t node_after;
    ASSERT_OK(inode_read(&fs, dir_ino, &node_after));
    ASSERT_EQ(node_after.size, size_at_10k); /* Exact slot reuse! */

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test3_duplicate_and_bad_names_rejected) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    /* Duplicate name */
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "hello.txt", 2, MINIFS_FT_REG));
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, "hello.txt", 3, MINIFS_FT_REG), -EEXIST);

    /* Bad names */
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, "", 2, MINIFS_FT_REG), -EINVAL);
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, "path/with/slash", 2, MINIFS_FT_REG), -EINVAL);

    /* 58+ chars (MAX_NAME_LEN is 57) */
    char long_name[70];
    memset(long_name, 'a', 58);
    long_name[58] = '\0';
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, long_name, 2, MINIFS_FT_REG), -EINVAL);

    /* Creation of . and .. rejected */
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, ".", 2, MINIFS_FT_DIR), -EEXIST);
    ASSERT_ERR(dir_add(&fs, fs.sb.root_inode, "..", 2, MINIFS_FT_DIR), -EEXIST);

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test4_multiple_slashes_equivalence) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    int a = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_init(&fs, (uint32_t)a, fs.sb.root_inode));
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "a", (uint32_t)a, MINIFS_FT_DIR));

    int b = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_init(&fs, (uint32_t)b, (uint32_t)a));
    ASSERT_OK(dir_add(&fs, (uint32_t)a, "b", (uint32_t)b, MINIFS_FT_DIR));

    int c = inode_create(&fs, S_IFREG | 0644, 0, 0);
    ASSERT_OK(dir_add(&fs, (uint32_t)b, "c", (uint32_t)c, MINIFS_FT_REG));

    uint32_t ino1 = 0, ino2 = 0;
    ASSERT_OK(path_resolve(&fs, "/a/b/c", 1, &ino1, NULL, NULL));
    ASSERT_OK(path_resolve(&fs, "///a//b///c/", 1, &ino2, NULL, NULL));

    ASSERT_EQ(ino1, (uint32_t)c);
    ASSERT_EQ(ino2, (uint32_t)c);

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test5_path_through_regular_file_enotdir) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    int f = inode_create(&fs, S_IFREG | 0644, 0, 0);
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "file.txt", (uint32_t)f, MINIFS_FT_REG));

    uint32_t ino = 0;
    ASSERT_ERR(path_resolve(&fs, "/file.txt/subpath", 1, &ino, NULL, NULL), -ENOTDIR);

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test6_dir_is_empty_correctness) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    int d = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_init(&fs, (uint32_t)d, fs.sb.root_inode));

    /* Only . and .. -> must be empty */
    ASSERT_EQ(dir_is_empty(&fs, (uint32_t)d), 1);

    /* Add an entry -> not empty */
    ASSERT_OK(dir_add(&fs, (uint32_t)d, "temp.txt", 5, MINIFS_FT_REG));
    ASSERT_EQ(dir_is_empty(&fs, (uint32_t)d), 0);

    /* Remove entry -> empty again */
    ASSERT_OK(dir_remove(&fs, (uint32_t)d, "temp.txt"));
    ASSERT_EQ(dir_is_empty(&fs, (uint32_t)d), 1);

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test7_link_counts_nested_dirs) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    inode_t root_node;
    ASSERT_OK(inode_read(&fs, fs.sb.root_inode, &root_node));
    ASSERT_EQ(root_node.nlink, 2);

    /* Add sub-directory */
    int sub1 = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_init(&fs, (uint32_t)sub1, fs.sb.root_inode));
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "sub1", (uint32_t)sub1, MINIFS_FT_DIR));

    /* Root link count should now be 3 (itself, ".", and "sub1/..") */
    ASSERT_OK(inode_read(&fs, fs.sb.root_inode, &root_node));
    ASSERT_EQ(root_node.nlink, 3);

    /* Add sub2 */
    int sub2 = inode_create(&fs, S_IFDIR | 0755, 0, 0);
    ASSERT_OK(dir_init(&fs, (uint32_t)sub2, fs.sb.root_inode));
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "sub2", (uint32_t)sub2, MINIFS_FT_DIR));

    ASSERT_OK(inode_read(&fs, fs.sb.root_inode, &root_node));
    ASSERT_EQ(root_node.nlink, 4);

    test_dir_teardown(&fs, &dev);
}

typedef struct {
    int count;
    char names[10][64];
} iter_context_t;

static int list_cb(const dirent_t *de, void *ctx) {
    iter_context_t *c = (iter_context_t *)ctx;
    if (c->count < 10) {
        strncpy(c->names[c->count], de->name, 63);
        c->count++;
    }
    return 0;
}

TEST(m6_test8_listing_returns_live_entries) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "alpha", 10, MINIFS_FT_REG));
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "beta", 11, MINIFS_FT_REG));
    ASSERT_OK(dir_add(&fs, fs.sb.root_inode, "gamma", 12, MINIFS_FT_REG));
    ASSERT_OK(dir_remove(&fs, fs.sb.root_inode, "beta"));

    iter_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_OK(dir_list(&fs, fs.sb.root_inode, list_cb, &ctx));

    /* Entries should be ".", "..", "alpha", "gamma" (4 total, no "beta") */
    ASSERT_EQ(ctx.count, 4);
    ASSERT_STR_EQ(ctx.names[0], ".");
    ASSERT_STR_EQ(ctx.names[1], "..");
    ASSERT_STR_EQ(ctx.names[2], "alpha");
    ASSERT_STR_EQ(ctx.names[3], "gamma");

    test_dir_teardown(&fs, &dev);
}

TEST(m6_test9_hash_index_equivalence_with_linear_lookup) {
    cleanup_test_img();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_DIR_IMG, 1, 2048));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));
    ASSERT_OK(dir_bootstrap_root(&fs));

    /* Add 150 entries */
    for (int i = 0; i < 150; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%d.txt", i);
        ASSERT_OK(dir_add(&fs, fs.sb.root_inode, name, (uint32_t)(100 + i), MINIFS_FT_REG));
    }

    /* Remove 30 entries */
    for (int i = 10; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%d.txt", i);
        ASSERT_OK(dir_remove(&fs, fs.sb.root_inode, name));
    }

    /* Verify 100% equivalence between hash lookup and linear lookup across all 150 + 50 non-existent */
    for (int i = 0; i < 200; i++) {
        char name[32];
        snprintf(name, sizeof(name), "file_%d.txt", i);

        uint32_t ino_hash = 0, ino_lin = 0;
        int rc_hash = dir_lookup(&fs, fs.sb.root_inode, name, &ino_hash);
        int rc_lin = dir_linear_lookup(&fs, fs.sb.root_inode, name, &ino_lin);

        ASSERT_EQ(rc_hash, rc_lin);
        if (rc_hash == 0) {
            ASSERT_EQ(ino_hash, ino_lin);
        }
    }

    test_dir_teardown(&fs, &dev);
}
