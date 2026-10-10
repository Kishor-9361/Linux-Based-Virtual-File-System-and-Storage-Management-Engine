#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "vfs.h"
#include "common.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

#define DIFF_IMG "/tmp/minifs_diff_posix.img"
#define HOST_DIR "/tmp/minifs_host_diff"

static void cleanup_diff_env(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", DIFF_IMG, HOST_DIR);
    int rc = system(cmd);
    (void)rc;
}

static void init_diff_env(void) {
    cleanup_diff_env();
    mkdir(HOST_DIR, 0755);

    blkdev_t dev;
    blk_open(&dev, DIFF_IMG, 1, 8192);
    sb_format(&dev);
    fs_t fs;
    fs_init_allocators(&fs, &dev);
    dir_bootstrap_root(&fs);
    blk_close(&dev);
}

static int map_host_errno(int res) {
    if (res >= 0) return 0;
    return -errno;
}

TEST(m7_differential_posix_3_seeds) {
    unsigned int seeds[] = {101, 202, 303};

    for (size_t s = 0; s < sizeof(seeds)/sizeof(seeds[0]); s++) {
        init_diff_env();

        vfs_t v;
        ASSERT_OK(vfs_mount(&v, DIFF_IMG));

        srand(seeds[s]);

        /* Track a pool of names */
        const char *names[] = {
            "file_a.txt", "file_b.txt", "file_c.txt", "file_d.txt",
            "dir_1", "dir_2", "dir_1/nested.txt", "dir_2/sub.dat"
        };
        size_t n_names = sizeof(names)/sizeof(names[0]);

        for (int op = 0; op < 5000; op++) {
            int action = rand() % 8;
            const char *name = names[rand() % (int)n_names];

            char host_path[512];
            snprintf(host_path, sizeof(host_path), "%s/%s", HOST_DIR, name);

            char vfs_path[512];
            snprintf(vfs_path, sizeof(vfs_path), "/%s", name);

            if (action == 0) {
                /* Create and write */
                int host_fd = open(host_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                int host_err = (host_fd >= 0) ? 0 : -errno;

                int vfs_fd = vfs_open(&v, vfs_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                int vfs_err = (vfs_fd >= 0) ? 0 : vfs_fd;

                /* Both must succeed or both fail with matching error */
                ASSERT_EQ((host_err == 0), (vfs_err == 0));

                if (host_fd >= 0 && vfs_fd >= 0) {
                    char data[64];
                    snprintf(data, sizeof(data), "RandPayload_%d_%d", op, rand());
                    size_t len = strlen(data);

                    ssize_t hw = write(host_fd, data, len);
                    ssize_t vw = vfs_write(&v, vfs_fd, data, len);
                    ASSERT_EQ(hw, vw);

                    close(host_fd);
                    ASSERT_OK(vfs_close(&v, vfs_fd));
                }
            } else if (action == 1) {
                /* Read */
                int host_fd = open(host_path, O_RDONLY);
                int host_err = (host_fd >= 0) ? 0 : -errno;

                int vfs_fd = vfs_open(&v, vfs_path, O_RDONLY, 0);
                int vfs_err = (vfs_fd >= 0) ? 0 : vfs_fd;

                ASSERT_EQ((host_err == 0), (vfs_err == 0));

                if (host_fd >= 0 && vfs_fd >= 0) {
                    char hbuf[128], vbuf[128];
                    ssize_t hr = read(host_fd, hbuf, sizeof(hbuf));
                    int herr_rd = (hr >= 0) ? 0 : -errno;
                    ssize_t vr = vfs_read(&v, vfs_fd, vbuf, sizeof(vbuf));
                    int verr_rd = (vr >= 0) ? 0 : (int)vr;

                    ASSERT_EQ(herr_rd, verr_rd);
                    if (hr >= 0 && vr >= 0) {
                        ASSERT_EQ(hr, vr);
                        if (hr > 0) {
                            ASSERT_MEM_EQ(hbuf, vbuf, (size_t)hr);
                        }
                    }
                    close(host_fd);
                    ASSERT_OK(vfs_close(&v, vfs_fd));
                }
            } else if (action == 2) {
                /* Mkdir */
                int hres = mkdir(host_path, 0755);
                int herr = map_host_errno(hres);

                int vres = vfs_mkdir(&v, vfs_path, 0755);
                ASSERT_EQ((herr == 0), (vres == 0));
            } else if (action == 3) {
                /* Unlink */
                int hres = unlink(host_path);
                int herr = map_host_errno(hres);

                int vres = vfs_unlink(&v, vfs_path);
                ASSERT_EQ((herr == 0), (vres == 0));
            } else if (action == 4) {
                /* Rmdir */
                int hres = rmdir(host_path);
                int herr = map_host_errno(hres);

                int vres = vfs_rmdir(&v, vfs_path);
                ASSERT_EQ((herr == 0), (vres == 0));
            } else if (action == 5) {
                /* Rename */
                const char *dest_name = names[rand() % (int)n_names];
                char host_dest[512], vfs_dest[512];
                snprintf(host_dest, sizeof(host_dest), "%s/%s", HOST_DIR, dest_name);
                snprintf(vfs_dest, sizeof(vfs_dest), "/%s", dest_name);

                int hres = rename(host_path, host_dest);
                int herr = map_host_errno(hres);

                int vres = vfs_rename(&v, vfs_path, vfs_dest);
                if ((herr == 0) != (vres == 0)) {
                    printf("Divergence in rename: old='%s', new='%s', herr=%d, vres=%d\n",
                           vfs_path, vfs_dest, herr, vres);
                }
                ASSERT_EQ((herr == 0), (vres == 0));
            } else if (action == 6) {
                /* Hard link */
                const char *dest_name = names[rand() % (int)n_names];
                char host_dest[512], vfs_dest[512];
                snprintf(host_dest, sizeof(host_dest), "%s/%s", HOST_DIR, dest_name);
                snprintf(vfs_dest, sizeof(vfs_dest), "/%s", dest_name);

                int hres = link(host_path, host_dest);
                int herr = map_host_errno(hres);

                int vres = vfs_link(&v, vfs_path, vfs_dest);
                ASSERT_EQ((herr == 0), (vres == 0));
            } else if (action == 7) {
                /* Stat verification */
                struct stat hst;
                int hres = stat(host_path, &hst);
                int herr = map_host_errno(hres);

                struct vfs_stat vst;
                int vres = vfs_stat(&v, vfs_path, &vst);

                ASSERT_EQ((herr == 0), (vres == 0));
                if (herr == 0 && vres == 0) {
                    if (!S_ISDIR(hst.st_mode)) {
                        ASSERT_EQ((uint64_t)hst.st_size, vst.st_size);
                    }
                    ASSERT_EQ((uint16_t)hst.st_nlink, vst.st_nlink);
                    ASSERT_EQ(S_ISDIR(hst.st_mode) ? 1 : 0, (vst.st_mode & S_IFDIR) ? 1 : 0);
                }
            }
        }

        ASSERT_OK(vfs_unmount(&v));
        cleanup_diff_env();
    }
}
