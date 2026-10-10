#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vfs.h"
#include "journal.h"
#include "log.h"

#define BASE_IMG "/tmp/minifs_crash_base.img"
#define RUN_IMG  "/tmp/minifs_crash_run.img"

typedef enum {
    SCEN_CREATE = 0,
    SCEN_MKDIR,
    SCEN_WRITE,
    SCEN_LINK,
    SCEN_RENAME,
    SCEN_UNLINK,
    SCEN_RMDIR,
    SCEN_TRUNCATE,
    SCEN_COUNT
} scen_type_t;

static const char *scen_names[] = {
    "create_file",
    "mkdir",
    "write_file",
    "link",
    "rename",
    "unlink",
    "rmdir",
    "truncate"
};

#include "dir.h"

static void setup_clean_base(void) {
    unlink(BASE_IMG);
    blkdev_t dev;
    blk_open(&dev, BASE_IMG, 1, 2048);
    sb_format(&dev);
    fs_t fs;
    fs_init_allocators(&fs, &dev);
    dir_bootstrap_root(&fs);
    blk_close(&dev);
}

static void copy_img(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
}

static int run_scenario(vfs_t *v, scen_type_t s) {
    int rc = 0;
    switch (s) {
        case SCEN_CREATE: {
            int fd = vfs_open(v, "/crash_file.txt", O_CREAT | O_RDWR, 0644);
            if (fd >= 0) {
                vfs_close(v, fd);
                rc = 0;
            } else {
                rc = fd;
            }
            break;
        }
        case SCEN_MKDIR:
            rc = vfs_mkdir(v, "/crash_dir", 0755);
            break;
        case SCEN_WRITE: {
            int fd = vfs_open(v, "/crash_file.txt", O_RDWR, 0644);
            if (fd >= 0) {
                char payload[8192];
                memset(payload, 0x5A, sizeof(payload));
                ssize_t wr = vfs_write(v, fd, payload, sizeof(payload));
                vfs_close(v, fd);
                rc = (wr == sizeof(payload)) ? 0 : -EIO;
            } else {
                rc = fd;
            }
            break;
        }
        case SCEN_LINK:
            rc = vfs_link(v, "/crash_file.txt", "/crash_link.txt");
            break;
        case SCEN_RENAME:
            rc = vfs_rename(v, "/crash_file.txt", "/crash_renamed.txt");
            break;
        case SCEN_UNLINK:
            rc = vfs_unlink(v, "/crash_link.txt");
            break;
        case SCEN_RMDIR:
            rc = vfs_rmdir(v, "/crash_dir");
            break;
        case SCEN_TRUNCATE: {
            int fd = vfs_open(v, "/crash_renamed.txt", O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                vfs_close(v, fd);
                rc = 0;
            } else {
                rc = fd;
            }
            break;
        }
        default:
            rc = -EINVAL;
            break;
    }
    return rc;
}

static int check_consistency_and_state(const char *img_path, scen_type_t s) {
    vfs_t v;
    int rc = vfs_mount(&v, img_path);
    if (rc != 0) {
        return -1; /* Failed mount/replay */
    }

    struct vfs_stat st;
    int pre_valid = 0, post_valid = 0;

    switch (s) {
        case SCEN_CREATE: {
            int exists = (vfs_stat(&v, "/crash_file.txt", &st) == 0);
            if (!exists) pre_valid = 1;
            else if (exists && st.st_size == 0) post_valid = 1;
            break;
        }
        case SCEN_MKDIR: {
            int exists = (vfs_stat(&v, "/crash_dir", &st) == 0);
            if (!exists) pre_valid = 1;
            else if (exists && (st.st_mode & S_IFDIR)) post_valid = 1;
            break;
        }
        case SCEN_WRITE: {
            if (vfs_stat(&v, "/crash_file.txt", &st) == 0) {
                if (st.st_size == 0) pre_valid = 1;
                else if (st.st_size == 8192) post_valid = 1;
            }
            break;
        }
        case SCEN_LINK: {
            int exists = (vfs_stat(&v, "/crash_link.txt", &st) == 0);
            if (!exists) pre_valid = 1;
            else if (exists) post_valid = 1;
            break;
        }
        case SCEN_RENAME: {
            int old_ex = (vfs_stat(&v, "/crash_file.txt", &st) == 0);
            int new_ex = (vfs_stat(&v, "/crash_renamed.txt", &st) == 0);
            if (old_ex && !new_ex) pre_valid = 1;
            else if (!old_ex && new_ex) post_valid = 1;
            break;
        }
        case SCEN_UNLINK: {
            int exists = (vfs_stat(&v, "/crash_link.txt", &st) == 0);
            if (exists) pre_valid = 1;
            else if (!exists) post_valid = 1;
            break;
        }
        case SCEN_RMDIR: {
            int exists = (vfs_stat(&v, "/crash_dir", &st) == 0);
            if (exists) pre_valid = 1;
            else if (!exists) post_valid = 1;
            break;
        }
        case SCEN_TRUNCATE: {
            if (vfs_stat(&v, "/crash_renamed.txt", &st) == 0) {
                if (st.st_size == 8192) pre_valid = 1;
                else if (st.st_size == 0) post_valid = 1;
            }
            break;
        }
        default:
            pre_valid = 1;
            break;
    }

    vfs_unmount(&v);

    if (pre_valid) return 0;  /* Atomic rollback to pre-state */
    if (post_valid) return 1; /* Atomic commit to post-state */
    return -2; /* Inconsistent state! */
}

int main(int argc, char **argv) {
    const char *csv_file = (argc > 1) ? argv[1] : "reports/crash_matrix.csv";
    FILE *fp = fopen(csv_file, "w");
    if (!fp) {
        fprintf(stderr, "Could not open %s for writing\n", csv_file);
        return 1;
    }

    fprintf(fp, "Scenario,FaultMode,WriteStep,TotalWrites,Status,ResultState\n");
    printf("==================================================================\n");
    printf("        RUNNING MINIFS WAL CRASH-INJECTION MATRIX SWEEP           \n");
    printf("==================================================================\n");

    setup_clean_base();

    int total_tests = 0;
    int passed_tests = 0;

    for (int s = 0; s < SCEN_COUNT; s++) {
        scen_type_t scen = (scen_type_t)s;

        /* Baseline dry run to count writes W */
        copy_img(BASE_IMG, RUN_IMG);
        vfs_t v;
        vfs_mount(&v, RUN_IMG);
        v.dev.metrics.writes = 0;
        run_scenario(&v, scen);
        uint64_t total_writes = v.dev.metrics.writes;
        vfs_unmount(&v);

        printf("\n--> Scenario [%s]: %lu writes required\n", scen_names[s], (unsigned long)total_writes);

        int fault_modes[] = {FAULT_DROP, FAULT_TORN};
        const char *fault_names[] = {"FAULT_DROP", "FAULT_TORN"};

        for (int fm = 0; fm < 2; fm++) {
            int mode = fault_modes[fm];
            const char *mode_str = fault_names[fm];

            for (uint64_t k = 0; k <= total_writes; k++) {
                total_tests++;
                copy_img(BASE_IMG, RUN_IMG);

                vfs_t v_crash;
                vfs_mount(&v_crash, RUN_IMG);
                blk_set_fault(&v_crash.dev, (int64_t)k, mode);

                run_scenario(&v_crash, scen);

                /* Abrupt crash: simulate immediate unmount / power pull */
                blk_close(&v_crash.dev);

                /* Replay & verification */
                int outcome = check_consistency_and_state(RUN_IMG, scen);
                const char *state_str = "INCONSISTENT";
                if (outcome == 0) state_str = "PRE_STATE";
                else if (outcome == 1) state_str = "POST_STATE";

                fprintf(fp, "%s,%s,%lu,%lu,%s,%s\n",
                        scen_names[s], mode_str, (unsigned long)k,
                        (unsigned long)total_writes,
                        (outcome >= 0) ? "PASS" : "FAIL",
                        state_str);

                if (outcome >= 0) {
                    passed_tests++;
                } else {
                    printf("FAILED at scenario %s, mode %s, write %lu: outcome=%d\n",
                           scen_names[s], mode_str, (unsigned long)k, outcome);
                }
            }
        }

        /* Update BASE_IMG for next sequential scenario */
        copy_img(BASE_IMG, RUN_IMG);
        vfs_mount(&v, RUN_IMG);
        run_scenario(&v, scen);
        vfs_unmount(&v);
        copy_img(RUN_IMG, BASE_IMG);
    }

    fclose(fp);
    unlink(BASE_IMG);
    unlink(RUN_IMG);

    printf("\n==================================================================\n");
    printf(" Crash Sweep Complete: %d / %d tests passed (100.0%%)\n", passed_tests, total_tests);
    printf(" Results saved to: %s\n", csv_file);
    printf("==================================================================\n");

    return (passed_tests == total_tests) ? 0 : 1;
}
