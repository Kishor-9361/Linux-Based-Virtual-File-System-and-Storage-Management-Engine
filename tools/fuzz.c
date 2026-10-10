#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "vfs.h"
#include "dir.h"
#include "fsck.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define FUZZ_DEFAULT_IMG "/tmp/minifs_fuzz.img"
#define FUZZ_HOST_DIR    "/tmp/minifs_fuzz_host"
#define MAX_NAMES 32

static const char *g_fuzz_names[MAX_NAMES] = {
    "file1.txt", "file2.dat", "file3.bin", "file4.log",
    "alpha.doc", "beta.bin", "gamma.dat", "delta.txt",
    "d1", "d2", "d3", "d1/sub1", "d2/sub2",
    "d1/a.txt", "d1/b.dat", "d2/c.txt", "d2/d.bin",
    "d1/sub1/deep.txt", "d2/sub2/deep.dat",
    "f_link.txt", "f_link2.dat", "shared.bin",
    "temp1.tmp", "temp2.tmp", "log_a.log", "log_b.log",
    "data_x.bin", "data_y.bin", "dir_x", "dir_y",
    "d3/inner.dat", "root_extra.txt"
};

typedef struct {
    unsigned int seed;
    uint32_t total_ops;
    uint32_t stop_at;
    bool crash_injection;
    const char *img_path;
} fuzz_config_t;

static void clean_env(const char *img) {
    unlink(img);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", FUZZ_HOST_DIR);
    int rc = system(cmd);
    (void)rc;
    mkdir(FUZZ_HOST_DIR, 0755);
}

static void init_fs(const char *img) {
    blkdev_t dev;
    blk_open(&dev, img, 1, 16384);
    sb_format(&dev);
    fs_t fs;
    fs_init_allocators(&fs, &dev);
    dir_bootstrap_root(&fs);
    blk_close(&dev);
}

int run_fuzzer(const fuzz_config_t *cfg) {
    printf("Starting Fuzzer: Seed=%u, Ops=%u, StopAt=%u, Img=%s\n",
           cfg->seed, cfg->total_ops, cfg->stop_at, cfg->img_path);

    clean_env(cfg->img_path);
    init_fs(cfg->img_path);

    vfs_t v;
    int rc = vfs_mount(&v, cfg->img_path);
    if (rc != 0) {
        fprintf(stderr, "Failed to mount fuzz image: %s\n", strerror(-rc));
        return 1;
    }

    srand(cfg->seed);

    char host_path[512];
    char vfs_path[512];
    char buf[BLOCK_SIZE];

    for (uint32_t op = 1; op <= cfg->total_ops; op++) {
        if (cfg->stop_at > 0 && op > cfg->stop_at) {
            printf("Reached target op %u. Stopping replay.\n", cfg->stop_at);
            break;
        }

        int action = rand() % 10;
        const char *name = g_fuzz_names[rand() % MAX_NAMES];
        snprintf(host_path, sizeof(host_path), "%s/%s", FUZZ_HOST_DIR, name);
        snprintf(vfs_path, sizeof(vfs_path), "/%s", name);

        switch (action) {
            case 0: { /* Create & Write */
                int host_fd = open(host_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                int host_err = (host_fd >= 0) ? 0 : -errno;

                int vfs_fd = vfs_open(&v, vfs_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                int vfs_err = (vfs_fd >= 0) ? 0 : vfs_fd;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (create %s): host_err=%d, vfs_err=%d\n",
                            op, name, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }

                if (host_fd >= 0 && vfs_fd >= 0) {
                    size_t sz = (size_t)(1 + (rand() % 8192));
                    memset(buf, (int)((op + sz) & 0xFF), sizeof(buf));
                    size_t written = 0;
                    while (written < sz) {
                        size_t to_wr = (sz - written < sizeof(buf)) ? (sz - written) : sizeof(buf);
                        write(host_fd, buf, to_wr);
                        vfs_write(&v, vfs_fd, buf, to_wr);
                        written += to_wr;
                    }
                    close(host_fd);
                    vfs_close(&v, vfs_fd);
                }
                break;
            }
            case 1: { /* Append / random offset write */
                int host_fd = open(host_path, O_WRONLY);
                int vfs_fd = vfs_open(&v, vfs_path, O_WRONLY, 0);

                if ((host_fd >= 0) != (vfs_fd >= 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (open-write %s): host=%d, vfs=%d\n",
                            op, name, host_fd, vfs_fd);
                    if (host_fd >= 0) close(host_fd);
                    if (vfs_fd >= 0) vfs_close(&v, vfs_fd);
                    vfs_unmount(&v);
                    return 2;
                }

                if (host_fd >= 0 && vfs_fd >= 0) {
                    off_t offset = (off_t)(rand() % 16384);
                    lseek(host_fd, offset, SEEK_SET);
                    vfs_lseek(&v, vfs_fd, offset, SEEK_SET);

                    size_t sz = (size_t)(1 + (rand() % 2048));
                    memset(buf, (int)((op ^ offset) & 0xFF), sizeof(buf));
                    write(host_fd, buf, sz);
                    vfs_write(&v, vfs_fd, buf, sz);

                    close(host_fd);
                    vfs_close(&v, vfs_fd);
                }
                break;
            }
            case 2: { /* Read verification */
                int host_fd = open(host_path, O_RDONLY);
                int vfs_fd = vfs_open(&v, vfs_path, O_RDONLY, 0);

                if ((host_fd >= 0) != (vfs_fd >= 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (open-read %s): host=%d, vfs=%d\n",
                            op, name, host_fd, vfs_fd);
                    if (host_fd >= 0) close(host_fd);
                    if (vfs_fd >= 0) vfs_close(&v, vfs_fd);
                    vfs_unmount(&v);
                    return 2;
                }

                if (host_fd >= 0 && vfs_fd >= 0) {
                    char h_buf[512], v_buf[512];
                    ssize_t hr = read(host_fd, h_buf, sizeof(h_buf));
                    int herr = (hr >= 0) ? 0 : -errno;
                    ssize_t vr = vfs_read(&v, vfs_fd, v_buf, sizeof(v_buf));
                    int verr = (vr >= 0) ? 0 : (int)vr;

                    if ((herr == 0) != (verr == 0)) {
                        fprintf(stderr, "READ DIVERGENCE at op %u (%s): herr=%d, verr=%d\n",
                                op, name, herr, verr);
                        close(host_fd);
                        vfs_close(&v, vfs_fd);
                        vfs_unmount(&v);
                        return 3;
                    }

                    if (hr >= 0 && vr >= 0) {
                        if (hr != vr || (hr > 0 && memcmp(h_buf, v_buf, (size_t)hr) != 0)) {
                            fprintf(stderr, "CONTENT MISMATCH at op %u (%s): hr=%zd, vr=%zd\n",
                                    op, name, hr, vr);
                            close(host_fd);
                            vfs_close(&v, vfs_fd);
                            vfs_unmount(&v);
                            return 3;
                        }
                    }
                    close(host_fd);
                    vfs_close(&v, vfs_fd);
                }
                break;
            }
            case 3: { /* Mkdir */
                int hr = mkdir(host_path, 0755);
                int host_err = (hr == 0) ? 0 : -errno;

                int vr = vfs_mkdir(&v, vfs_path, 0755);
                int vfs_err = vr;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (mkdir %s): host_err=%d, vfs_err=%d\n",
                            op, name, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }
                break;
            }
            case 4: { /* Truncate */
                uint64_t trunc_sz = (uint64_t)(rand() % 4096);
                int hr = truncate(host_path, (off_t)trunc_sz);
                int host_err = (hr == 0) ? 0 : -errno;

                int vr = vfs_truncate(&v, vfs_path, trunc_sz);
                int vfs_err = vr;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (truncate %s to %lu): host=%d, vfs=%d\n",
                            op, name, (unsigned long)trunc_sz, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }
                break;
            }
            case 5: { /* Unlink */
                int hr = unlink(host_path);
                int host_err = (hr == 0) ? 0 : -errno;

                int vr = vfs_unlink(&v, vfs_path);
                int vfs_err = vr;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (unlink %s): host=%d, vfs=%d\n",
                            op, name, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }
                break;
            }
            case 6: { /* Rmdir */
                int hr = rmdir(host_path);
                int host_err = (hr == 0) ? 0 : -errno;

                int vr = vfs_rmdir(&v, vfs_path);
                int vfs_err = vr;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (rmdir %s): host=%d, vfs=%d\n",
                            op, name, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }
                break;
            }
            case 7: { /* Rename */
                const char *dst_name = g_fuzz_names[rand() % MAX_NAMES];
                char host_dst[512], vfs_dst[512];
                snprintf(host_dst, sizeof(host_dst), "%s/%s", FUZZ_HOST_DIR, dst_name);
                snprintf(vfs_dst, sizeof(vfs_dst), "/%s", dst_name);

                int hr = rename(host_path, host_dst);
                int host_err = (hr == 0) ? 0 : -errno;

                int vr = vfs_rename(&v, vfs_path, vfs_dst);
                int vfs_err = vr;

                if ((host_err == 0) != (vfs_err == 0)) {
                    fprintf(stderr, "DIVERGENCE at op %u (rename %s -> %s): host=%d, vfs=%d\n",
                            op, name, dst_name, host_err, vfs_err);
                    vfs_unmount(&v);
                    return 2;
                }
                break;
            }
            case 8: { /* Sync */
                vfs_sync(&v);
                break;
            }
            case 9: { /* Remount */
                vfs_unmount(&v);
                rc = vfs_mount(&v, cfg->img_path);
                if (rc != 0) {
                    fprintf(stderr, "Remount failed at op %u: %s\n", op, strerror(-rc));
                    return 4;
                }
                break;
            }
        }

        /* Continuous verification: Directory hash index matches linear scan */
        const char *chk_name = g_fuzz_names[rand() % MAX_NAMES];
        uint32_t i_hash = 0, i_lin = 0;
        int r_hash = dir_lookup(&v.fs, v.fs.sb.root_inode, chk_name, &i_hash);
        int r_lin = dir_linear_lookup(&v.fs, v.fs.sb.root_inode, chk_name, &i_lin);
        if (r_hash != r_lin || (r_hash == 0 && i_hash != i_lin)) {
            fprintf(stderr, "DIVERGENCE at op %u: dir hash index (%d,%u) != linear scan (%d,%u) for %s\n",
                    op, r_hash, i_hash, r_lin, i_lin, chk_name);
            vfs_unmount(&v);
            return 7;
        }

        /* Periodic consistency check every 1,000 ops */
        if (op % 1000 == 0) {
            vfs_sync(&v);
            fsck_options_t fopts = { .repair = false, .verbose = false };
            fsck_stats_t fstats;
            int frc = fsck_check(&v.dev, &fopts, &fstats);
            if (frc != 0 || fstats.errors_found > 0) {
                fprintf(stderr, "FSCK ERROR detected at op %u: %u inconsistencies found!\n",
                        op, fstats.errors_found);
                vfs_unmount(&v);
                return 5;
            }
            printf("  [Checkpoint op %u / %u] - fsck CLEAN\n", op, cfg->total_ops);
        }
    }

    vfs_sync(&v);
    vfs_unmount(&v);

    /* Final FSCK check */
    blkdev_t check_dev;
    blk_open(&check_dev, cfg->img_path, 0, 0);
    fsck_options_t fopts = { .repair = false, .verbose = false };
    fsck_stats_t fstats;
    int frc = fsck_check(&check_dev, &fopts, &fstats);
    blk_close(&check_dev);

    if (frc != 0 || fstats.errors_found > 0) {
        fprintf(stderr, "FINAL FSCK FAILED: %u errors found!\n", fstats.errors_found);
        clean_env(cfg->img_path);
        return 6;
    }

    printf("Fuzz run SUCCESS: %u ops executed, zero divergences, fsck clean!\n", cfg->total_ops);
    clean_env(cfg->img_path);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--seed N] [--ops K] [--until K] [image_path]\n", prog);
}

int main(int argc, char **argv) {
    fuzz_config_t cfg;
    cfg.seed = 42;
    cfg.total_ops = 10000;
    cfg.stop_at = 0;
    cfg.crash_injection = false;
    cfg.img_path = FUZZ_DEFAULT_IMG;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.seed = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            cfg.total_ops = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            cfg.stop_at = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            cfg.img_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    return run_fuzzer(&cfg);
}
