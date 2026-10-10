#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "shell.h"
#include "fsck.h"
#include "cache.h"
#include "journal.h"
#include "perm.h"
#include "dir.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define MAX_ARGS 64
#define CHUNK_SIZE 65536

static void perm_mode_str(uint16_t mode, char *str) {
    str[0] = (mode & S_IFDIR) ? 'd' : '-';
    str[1] = (mode & 0400) ? 'r' : '-';
    str[2] = (mode & 0200) ? 'w' : '-';
    str[3] = (mode & 0100) ? 'x' : '-';
    str[4] = (mode & 0040) ? 'r' : '-';
    str[5] = (mode & 0020) ? 'w' : '-';
    str[6] = (mode & 0010) ? 'x' : '-';
    str[7] = (mode & 0004) ? 'r' : '-';
    str[8] = (mode & 0002) ? 'w' : '-';
    str[9] = (mode & 0001) ? 'x' : '-';
    str[10] = '\0';
}

void shell_init(shell_t *sh) {
    if (!sh) return;
    memset(sh, 0, sizeof(*sh));
}

void shell_cleanup(shell_t *sh) {
    if (!sh) return;
    if (sh->is_mounted) {
        vfs_unmount(&sh->v);
        sh->is_mounted = false;
    }
}

/* Helper to parse line into tokens with quote handling */
static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') break; /* Ignore comments */

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p + 1)) {
                    /* Handle escape inside quotes */
                    memmove(p, p + 1, strlen(p));
                }
                p++;
            }
            if (*p == quote) {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) {
                *p++ = '\0';
            }
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* Command implementations */
static int cmd_mount(shell_t *sh, int argc, char **argv) {
    const char *img = (argc > 1) ? argv[1] : sh->img_path;
    if (!img || img[0] == '\0') {
        fprintf(stderr, "error: image path required\n");
        return -EINVAL;
    }
    if (sh->is_mounted) {
        vfs_unmount(&sh->v);
        sh->is_mounted = false;
    }
    int rc = vfs_mount(&sh->v, img);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    strncpy(sh->img_path, img, sizeof(sh->img_path) - 1);
    sh->is_mounted = true;
    return 0;
}

static int cmd_umount(shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    int rc = vfs_unmount(&sh->v);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    sh->is_mounted = false;
    return 0;
}

static int cmd_mkfs(shell_t *sh, int argc, char **argv) {
    const char *img = sh->img_path;
    uint32_t blocks = 8192; /* Default 32 MiB */

    if (argc > 1) {
        char *endptr = NULL;
        unsigned long val = strtoul(argv[1], &endptr, 10);
        if (endptr && (*endptr == 'k' || *endptr == 'K')) val = (val * 1024) / BLOCK_SIZE;
        else if (endptr && (*endptr == 'm' || *endptr == 'M')) val = (val * 1024 * 1024) / BLOCK_SIZE;
        else if (endptr && (*endptr == 'g' || *endptr == 'G')) val = (val * 1024 * 1024 * 1024) / BLOCK_SIZE;
        if (val > 0) blocks = (uint32_t)val;
    }

    if (argc > 2) {
        img = argv[2];
        strncpy(sh->img_path, img, sizeof(sh->img_path) - 1);
    }

    if (!img || img[0] == '\0') {
        fprintf(stderr, "error: image path required\n");
        return -EINVAL;
    }

    if (sh->is_mounted) {
        vfs_unmount(&sh->v);
        sh->is_mounted = false;
    }

    blkdev_t dev;
    int rc = blk_open(&dev, img, 1, blocks);
    if (rc != 0) {
        fprintf(stderr, "error: failed to open disk: %s\n", strerror(-rc));
        return rc;
    }

    rc = sb_format(&dev);
    if (rc != 0) {
        blk_close(&dev);
        fprintf(stderr, "error: formatting failed: %s\n", strerror(-rc));
        return rc;
    }

    fs_t fs;
    rc = fs_init_allocators(&fs, &dev);
    if (rc == 0) {
        dir_bootstrap_root(&fs);
    }
    blk_close(&dev);

    /* Automatically mount */
    return cmd_mount(sh, 1, NULL);
}

static int cmd_ls(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }

    bool long_format = false;
    const char *path = ".";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_format = true;
        } else {
            path = argv[i];
        }
    }

    int fd = vfs_open(&sh->v, path, O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-fd));
        return fd;
    }

    struct vfs_dirent entry;
    while (vfs_readdir(&sh->v, fd, &entry) == 1) {
        if (!long_format) {
            printf("%s  ", entry.d_name);
        } else {
            char full_path[512];
            if (strcmp(path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "/%s", entry.d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", path, entry.d_name);
            }

            struct vfs_stat st;
            if (vfs_stat(&sh->v, full_path, &st) == 0) {
                char mode_str[11];
                perm_mode_str(st.st_mode, mode_str);

                char time_str[32] = "";
                time_t mt = (time_t)st.st_mtime;
                struct tm tm_buf;
                localtime_r(&mt, &tm_buf);
                strftime(time_str, sizeof(time_str), "%b %e %H:%M", &tm_buf);

                printf("%s %2u %4u %4u %8lu %s %s\n",
                       mode_str, st.st_nlink, st.st_uid, st.st_gid,
                       (unsigned long)st.st_size, time_str, entry.d_name);
            } else {
                printf("?---------  1    0    0        0 Jan  1 00:00 %s\n", entry.d_name);
            }
        }
    }
    if (!long_format) printf("\n");

    vfs_close(&sh->v, fd);
    return 0;
}

static int cmd_cd(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    const char *path = (argc > 1) ? argv[1] : "/";
    int rc = vfs_chdir(&sh->v, path);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_pwd(shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    char buf[512];
    int rc = vfs_getcwd(&sh->v, buf, sizeof(buf));
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    printf("%s\n", buf);
    return 0;
}

static int cmd_mkdir(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }

    bool p_flag = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            p_flag = true;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }

    if (!p_flag) {
        int rc = vfs_mkdir(&sh->v, path, 0755);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", strerror(-rc));
            return rc;
        }
        return 0;
    }

    /* Handle mkdir -p */
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + (tmp[0] == '/' ? 1 : 0); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            vfs_mkdir(&sh->v, tmp, 0755);
            *p = '/';
        }
    }
    int rc = vfs_mkdir(&sh->v, tmp, 0755);
    if (rc != 0 && rc != -EEXIST) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_rmdir(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }
    int rc = vfs_rmdir(&sh->v, argv[1]);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_touch(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }
    int fd = vfs_open(&sh->v, argv[1], O_CREAT | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-fd));
        return fd;
    }
    vfs_close(&sh->v, fd);
    return 0;
}

static int cmd_cat(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }
    int fd = vfs_open(&sh->v, argv[1], O_RDONLY, 0);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-fd));
        return fd;
    }

    char buf[4096];
    ssize_t n;
    while ((n = vfs_read(&sh->v, fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }
    vfs_close(&sh->v, fd);
    return (n < 0) ? (int)n : 0;
}

static int cmd_write(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: path and text required\n");
        return -EINVAL;
    }
    int fd = vfs_open(&sh->v, argv[1], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-fd));
        return fd;
    }
    size_t len = strlen(argv[2]);
    ssize_t wr = vfs_write(&sh->v, fd, argv[2], len);
    vfs_close(&sh->v, fd);
    if (wr < 0) {
        fprintf(stderr, "error: %s\n", strerror((int)-wr));
        return (int)wr;
    }
    return 0;
}

static int cmd_append(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: path and text required\n");
        return -EINVAL;
    }
    int fd = vfs_open(&sh->v, argv[1], O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-fd));
        return fd;
    }
    size_t len = strlen(argv[2]);
    ssize_t wr = vfs_write(&sh->v, fd, argv[2], len);
    vfs_close(&sh->v, fd);
    if (wr < 0) {
        fprintf(stderr, "error: %s\n", strerror((int)-wr));
        return (int)wr;
    }
    return 0;
}

static int cmd_truncate(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: path and length required\n");
        return -EINVAL;
    }
    uint64_t len = (uint64_t)strtoull(argv[2], NULL, 10);
    int rc = vfs_truncate(&sh->v, argv[1], len);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

/* Helper for recursive rm -r */
static int recursive_rm(shell_t *sh, const char *path) {
    struct vfs_stat st;
    int rc = vfs_stat(&sh->v, path, &st);
    if (rc != 0) return rc;

    if (!(st.st_mode & S_IFDIR)) {
        return vfs_unlink(&sh->v, path);
    }

    int fd = vfs_open(&sh->v, path, O_RDONLY, 0);
    if (fd < 0) return fd;

    struct vfs_dirent entry;
    while (vfs_readdir(&sh->v, fd, &entry) == 1) {
        if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) continue;
        char sub[512];
        if (strcmp(path, "/") == 0) snprintf(sub, sizeof(sub), "/%s", entry.d_name);
        else snprintf(sub, sizeof(sub), "%s/%s", path, entry.d_name);
        recursive_rm(sh, sub);
    }
    vfs_close(&sh->v, fd);
    return vfs_rmdir(&sh->v, path);
}

static int cmd_rm(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }

    bool r_flag = false;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0) {
            r_flag = true;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }

    int rc = (r_flag) ? recursive_rm(sh, path) : vfs_unlink(&sh->v, path);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_mv(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: source and destination required\n");
        return -EINVAL;
    }
    int rc = vfs_rename(&sh->v, argv[1], argv[2]);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_cp(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: source and destination required\n");
        return -EINVAL;
    }
    int src_fd = vfs_open(&sh->v, argv[1], O_RDONLY, 0);
    if (src_fd < 0) {
        fprintf(stderr, "error: %s\n", strerror(-src_fd));
        return src_fd;
    }
    int dst_fd = vfs_open(&sh->v, argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (dst_fd < 0) {
        vfs_close(&sh->v, src_fd);
        fprintf(stderr, "error: %s\n", strerror(-dst_fd));
        return dst_fd;
    }

    char *buf = (char *)malloc(CHUNK_SIZE);
    if (!buf) {
        vfs_close(&sh->v, src_fd);
        vfs_close(&sh->v, dst_fd);
        return -ENOMEM;
    }

    ssize_t rd;
    int ret = 0;
    while ((rd = vfs_read(&sh->v, src_fd, buf, CHUNK_SIZE)) > 0) {
        ssize_t wr = vfs_write(&sh->v, dst_fd, buf, (size_t)rd);
        if (wr < 0) {
            ret = (int)wr;
            fprintf(stderr, "error: %s\n", strerror(-ret));
            break;
        }
    }
    free(buf);
    vfs_close(&sh->v, src_fd);
    vfs_close(&sh->v, dst_fd);
    return ret;
}

static int cmd_ln(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: source and destination required\n");
        return -EINVAL;
    }
    int rc = vfs_link(&sh->v, argv[1], argv[2]);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_stat(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: path required\n");
        return -EINVAL;
    }
    struct vfs_stat st;
    int rc = vfs_stat(&sh->v, argv[1], &st);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }

    char mode_str[11];
    perm_mode_str(st.st_mode, mode_str);

    printf("  File: %s\n", argv[1]);
    printf("  Size: %-15lu Blocks: %-10u IO Block: 4096  %s\n",
           (unsigned long)st.st_size, st.st_blocks,
           (st.st_mode & S_IFDIR) ? "directory" : "regular file");
    printf(" Inode: %-15u Links: %-10u\n", st.st_ino, st.st_nlink);
    printf("Access: (%04o/%s)  Uid: (%5u)   Gid: (%5u)\n",
           st.st_mode & 07777, mode_str, st.st_uid, st.st_gid);

    time_t at = (time_t)st.st_atime, mt = (time_t)st.st_mtime, ct = (time_t)st.st_ctime;
    char at_s[32], mt_s[32], ct_s[32];
    struct tm tm_buf;
    localtime_r(&at, &tm_buf); strftime(at_s, sizeof(at_s), "%Y-%m-%d %H:%M:%S", &tm_buf);
    localtime_r(&mt, &tm_buf); strftime(mt_s, sizeof(mt_s), "%Y-%m-%d %H:%M:%S", &tm_buf);
    localtime_r(&ct, &tm_buf); strftime(ct_s, sizeof(ct_s), "%Y-%m-%d %H:%M:%S", &tm_buf);

    printf("Access: %s\nModify: %s\nChange: %s\n", at_s, mt_s, ct_s);
    return 0;
}

/* Helper for visual tree walk */
static void print_tree(shell_t *sh, const char *path, const char *prefix, int *dirs, int *files) {
    int fd = vfs_open(&sh->v, path, O_RDONLY, 0);
    if (fd < 0) return;

    /* Count items */
    struct vfs_dirent entry;
    char names[128][64];
    int count = 0;
    while (vfs_readdir(&sh->v, fd, &entry) == 1 && count < 128) {
        if (strcmp(entry.d_name, ".") == 0 || strcmp(entry.d_name, "..") == 0) continue;
        strncpy(names[count++], entry.d_name, 63);
    }
    vfs_close(&sh->v, fd);

    for (int i = 0; i < count; i++) {
        bool is_last = (i == count - 1);
        char child_path[512];
        if (strcmp(path, "/") == 0) snprintf(child_path, sizeof(child_path), "/%s", names[i]);
        else snprintf(child_path, sizeof(child_path), "%s/%s", path, names[i]);

        struct vfs_stat st;
        vfs_stat(&sh->v, child_path, &st);

        printf("%s%s── %s\n", prefix, is_last ? "└" : "├", names[i]);

        if (st.st_mode & S_IFDIR) {
            (*dirs)++;
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s   ", prefix, is_last ? " " : "│");
            print_tree(sh, child_path, new_prefix, dirs, files);
        } else {
            (*files)++;
        }
    }
}

static int cmd_tree(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    const char *path = (argc > 1) ? argv[1] : ".";
    printf("%s\n", path);
    int dirs = 0, files = 0;
    print_tree(sh, path, "", &dirs, &files);
    printf("\n%d directories, %d files\n", dirs, files);
    return 0;
}

static int cmd_chmod(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: mode and path required\n");
        return -EINVAL;
    }
    mode_t mode = (mode_t)strtoul(argv[1], NULL, 8);
    int rc = vfs_chmod(&sh->v, argv[2], mode);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_chown(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: owner and path required\n");
        return -EINVAL;
    }

    char *colon = strchr(argv[1], ':');
    uid_t uid = (uid_t)strtoul(argv[1], NULL, 10);
    gid_t gid = (gid_t)-1;
    if (colon) {
        gid = (gid_t)strtoul(colon + 1, NULL, 10);
    }

    int rc = vfs_chown(&sh->v, argv[2], uid, gid);
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", strerror(-rc));
        return rc;
    }
    return 0;
}

static int cmd_su(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: uid required\n");
        return -EINVAL;
    }
    sh->v.uid = (uid_t)strtoul(argv[1], NULL, 10);
    if (argc > 2) {
        sh->v.gid = (gid_t)strtoul(argv[2], NULL, 10);
    }
    return 0;
}

static int cmd_umask(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc > 1) {
        mode_t m = (mode_t)strtoul(argv[1], NULL, 8);
        vfs_umask(&sh->v, m);
    }
    printf("%04o\n", sh->v.umask);
    return 0;
}

static int cmd_cp_in(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: hostfile and target path required\n");
        return -EINVAL;
    }

    int host_fd = open(argv[1], O_RDONLY);
    if (host_fd < 0) {
        fprintf(stderr, "error: cannot open host file '%s': %s\n", argv[1], strerror(errno));
        return -errno;
    }

    int vfs_fd = vfs_open(&sh->v, argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (vfs_fd < 0) {
        close(host_fd);
        fprintf(stderr, "error: cannot open target '%s': %s\n", argv[2], strerror(-vfs_fd));
        return vfs_fd;
    }

    char *buf = (char *)malloc(CHUNK_SIZE);
    if (!buf) {
        close(host_fd); vfs_close(&sh->v, vfs_fd);
        return -ENOMEM;
    }

    ssize_t rd;
    int ret = 0;
    while ((rd = read(host_fd, buf, CHUNK_SIZE)) > 0) {
        ssize_t wr = vfs_write(&sh->v, vfs_fd, buf, (size_t)rd);
        if (wr < 0) {
            ret = (int)wr;
            fprintf(stderr, "error: write failed: %s\n", strerror(-ret));
            break;
        }
    }
    if (rd < 0) ret = -errno;

    free(buf);
    close(host_fd);
    vfs_close(&sh->v, vfs_fd);
    return ret;
}

static int cmd_cp_out(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 3) {
        fprintf(stderr, "error: source path and hostfile required\n");
        return -EINVAL;
    }

    int vfs_fd = vfs_open(&sh->v, argv[1], O_RDONLY, 0);
    if (vfs_fd < 0) {
        fprintf(stderr, "error: cannot open source '%s': %s\n", argv[1], strerror(-vfs_fd));
        return vfs_fd;
    }

    int host_fd = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (host_fd < 0) {
        vfs_close(&sh->v, vfs_fd);
        fprintf(stderr, "error: cannot open host file '%s': %s\n", argv[2], strerror(errno));
        return -errno;
    }

    char *buf = (char *)malloc(CHUNK_SIZE);
    if (!buf) {
        vfs_close(&sh->v, vfs_fd); close(host_fd);
        return -ENOMEM;
    }

    ssize_t rd;
    int ret = 0;
    while ((rd = vfs_read(&sh->v, vfs_fd, buf, CHUNK_SIZE)) > 0) {
        ssize_t wr = write(host_fd, buf, (size_t)rd);
        if (wr < 0) {
            ret = -errno;
            fprintf(stderr, "error: host write failed: %s\n", strerror(errno));
            break;
        }
    }
    if (rd < 0) ret = (int)rd;

    free(buf);
    vfs_close(&sh->v, vfs_fd);
    close(host_fd);
    return ret;
}

static int cmd_df(shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    superblock_t *sb = &sh->v.fs.sb;
    uint32_t used_blks = sb->total_blocks - sb->free_blocks;
    uint32_t used_inodes = sb->total_inodes - sb->free_inodes;
    double blk_pct = (sb->total_blocks > 0) ? ((double)used_blks * 100.0 / sb->total_blocks) : 0.0;
    double ino_pct = (sb->total_inodes > 0) ? ((double)used_inodes * 100.0 / sb->total_inodes) : 0.0;

    printf("Filesystem      1K-blocks      Used Available Use%% Inodes IUsed IFree IUse%%\n");
    printf("%-15s %10lu %9lu %9lu %3.0f%% %6u %5u %5u %3.0f%%\n",
           sh->img_path,
           (unsigned long)sb->total_blocks * (BLOCK_SIZE / 1024),
           (unsigned long)used_blks * (BLOCK_SIZE / 1024),
           (unsigned long)sb->free_blocks * (BLOCK_SIZE / 1024),
           blk_pct, sb->total_inodes, used_inodes, sb->free_inodes, ino_pct);
    return 0;
}

static int cmd_fsck(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }

    fsck_options_t opts = { .repair = false, .verbose = true };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0 || strcmp(argv[i], "-r") == 0) {
            opts.repair = true;
        }
    }

    vfs_sync(&sh->v);
    fsck_stats_t stats;
    int rc = fsck_check(&sh->v.dev, &opts, &stats);

    printf("fsck: found %u inconsistencies, fixed %u\n", stats.errors_found, stats.errors_fixed);
    return rc;
}

static int cmd_journal(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc > 1 && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "stats") == 0)) {
        journal_t *j = (journal_t *)sh->v.fs.journal;
        if (!j) {
            printf("Journal: not initialized\n");
            return 0;
        }
        printf("Journal Status:\n");
        printf("  Log Start Block: %u\n", j->log_start);
        printf("  Log Blocks:      %u\n", j->log_blocks);
        printf("  Head Position:   %u\n", j->head_pos);
        printf("  Tail Position:   %u\n", j->tail_pos);
        printf("  Head Sequence:   %" PRIu64 "\n", j->head_seq);
        printf("  Tx Committed:    %" PRIu64 "\n", j->metrics.tx_committed);
        printf("  Tx Aborted:      %" PRIu64 "\n", j->metrics.tx_aborted);
        printf("  Log Bytes Comm:  %" PRIu64 "\n", j->metrics.log_bytes_written);
    } else {
        printf("Usage: journal status\n");
    }
    return 0;
}

static int cmd_cache(shell_t *sh, int argc, char **argv) {
    (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc > 1 && (strcmp(argv[1], "stats") == 0 || strcmp(argv[1], "status") == 0)) {
        cache_t *c = (cache_t *)sh->v.fs.cache;
        if (!c) {
            printf("Cache: disabled\n");
            return 0;
        }
        cache_stats_t st = cache_stats(c);
        double hit_ratio = (st.hits + st.misses > 0) ?
            ((double)st.hits * 100.0 / (double)(st.hits + st.misses)) : 0.0;
        printf("Cache Stats:\n");
        printf("  Hits:        %" PRIu64 "\n", st.hits);
        printf("  Misses:      %" PRIu64 "\n", st.misses);
        printf("  Hit Ratio:   %.2f%%\n", hit_ratio);
        printf("  Evictions:   %" PRIu64 "\n", st.evictions);
        printf("  Writebacks:  %" PRIu64 "\n", st.writebacks);
        printf("  Read-aheads: %" PRIu64 "\n", st.read_aheads);
    } else {
        printf("Usage: cache stats\n");
    }
    return 0;
}

static int cmd_crash(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc < 2) {
        fprintf(stderr, "error: fail_after count required\n");
        return -EINVAL;
    }
    int count = atoi(argv[1]);
    blk_set_fault(&sh->v.dev, (int64_t)count, FAULT_DROP);
    printf("Armed fault injection: FAULT_DROP after %d writes\n", count);
    return 0;
}

static int cmd_sync(shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    return vfs_sync(&sh->v);
}

static int cmd_metrics(shell_t *sh, int argc, char **argv) {
    if (!sh->is_mounted) {
        fprintf(stderr, "error: not mounted\n");
        return -EINVAL;
    }
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        metrics_reset();
        printf("Metrics reset.\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "csv") == 0) {
        metrics_dump_csv(stdout, &sh->v.fs);
        return 0;
    }
    metrics_print_report(stdout, &sh->v.fs);
    return 0;
}

static int cmd_help(shell_t *sh, int argc, char **argv) {
    (void)sh; (void)argc; (void)argv;
    printf("minifs shell commands:\n");
    printf("  mount <img_path>       Mount minifs image\n");
    printf("  umount                 Unmount current filesystem\n");
    printf("  mkfs [size] [img_path] Format new filesystem\n");
    printf("  ls [-l] [path]         List directory contents\n");
    printf("  cd [path]              Change working directory\n");
    printf("  pwd                    Print current working directory\n");
    printf("  mkdir [-p] <path>      Create directory\n");
    printf("  rmdir <path>           Remove empty directory\n");
    printf("  touch <path>           Create empty file\n");
    printf("  cat <path>             Display file contents\n");
    printf("  write <path> <text>    Write text to file (truncate)\n");
    printf("  append <path> <text>   Append text to file\n");
    printf("  truncate <path> <len>  Truncate file\n");
    printf("  rm [-r] <path>         Remove file or directory\n");
    printf("  mv <src> <dst>         Rename / move file\n");
    printf("  cp <src> <dst>         Copy file\n");
    printf("  ln <src> <dst>         Create hard link\n");
    printf("  stat <path>            Show file/directory metadata\n");
    printf("  tree [path]            Display visual directory tree\n");
    printf("  chmod <mode> <path>    Change file permissions (octal)\n");
    printf("  chown <uid[:gid]> <p>  Change file owner and group\n");
    printf("  su <uid> [gid]         Switch shell credentials\n");
    printf("  umask [mask]           Display or set umask\n");
    printf("  cp-in <host> <target>  Import file from host system\n");
    printf("  cp-out <target> <host> Export file to host system\n");
    printf("  df                     Show disk usage\n");
    printf("  fsck [--repair]        Check / repair filesystem\n");
    printf("  journal status         Show journal metrics and state\n");
    printf("  cache stats            Show LRU cache statistics\n");
    printf("  sync                   Flush dirty buffers to disk\n");
    printf("  crash <n>              Arm fault injection after n writes\n");
    printf("  help                   Display this help message\n");
    printf("  exit / quit            Exit shell\n");
    return 0;
}

static int cmd_exit(shell_t *sh, int argc, char **argv) {
    (void)argc; (void)argv;
    sh->exit_requested = true;
    return 0;
}

typedef struct {
    const char *name;
    int (*handler)(shell_t *sh, int argc, char **argv);
} command_entry_t;

static const command_entry_t cmd_table[] = {
    {"mount", cmd_mount},
    {"umount", cmd_umount},
    {"unmount", cmd_umount},
    {"mkfs", cmd_mkfs},
    {"ls", cmd_ls},
    {"cd", cmd_cd},
    {"pwd", cmd_pwd},
    {"mkdir", cmd_mkdir},
    {"rmdir", cmd_rmdir},
    {"touch", cmd_touch},
    {"cat", cmd_cat},
    {"write", cmd_write},
    {"append", cmd_append},
    {"truncate", cmd_truncate},
    {"rm", cmd_rm},
    {"mv", cmd_mv},
    {"cp", cmd_cp},
    {"ln", cmd_ln},
    {"stat", cmd_stat},
    {"tree", cmd_tree},
    {"chmod", cmd_chmod},
    {"chown", cmd_chown},
    {"su", cmd_su},
    {"umask", cmd_umask},
    {"cp-in", cmd_cp_in},
    {"cp-out", cmd_cp_out},
    {"df", cmd_df},
    {"fsck", cmd_fsck},
    {"journal", cmd_journal},
    {"cache", cmd_cache},
    {"crash", cmd_crash},
    {"sync", cmd_sync},
    {"metrics", cmd_metrics},
    {"help", cmd_help},
    {"exit", cmd_exit},
    {"quit", cmd_exit},
    {NULL, NULL}
};

int shell_execute_line(shell_t *sh, const char *line) {
    if (!sh || !line) return -EINVAL;

    char buf[1024];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing newline */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    char *argv[MAX_ARGS];
    int argc = tokenize(buf, argv, MAX_ARGS);
    if (argc == 0) return 0; /* Empty line */

    for (const command_entry_t *cmd = cmd_table; cmd->name; cmd++) {
        if (strcmp(cmd->name, argv[0]) == 0) {
            int rc = cmd->handler(sh, argc, argv);
            sh->last_status = rc;
            return rc;
        }
    }

    fprintf(stderr, "error: unknown command '%s'\n", argv[0]);
    sh->last_status = -ENOENT;
    return -ENOENT;
}

int shell_run_script(shell_t *sh, const char *script_path) {
    if (!sh || !script_path) return -EINVAL;
    FILE *fp = fopen(script_path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open script '%s': %s\n", script_path, strerror(errno));
        return -errno;
    }

    char line[1024];
    int last_rc = 0;
    while (!sh->exit_requested && fgets(line, sizeof(line), fp)) {
        int rc = shell_execute_line(sh, line);
        if (rc != 0) last_rc = rc;
    }
    fclose(fp);
    return last_rc;
}

int shell_run_repl(shell_t *sh) {
    if (!sh) return -EINVAL;
    char line[1024];

    while (!sh->exit_requested) {
        char cwd[256] = "/";
        if (sh->is_mounted) {
            vfs_getcwd(&sh->v, cwd, sizeof(cwd));
        }
        printf("minifs:%s$ ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }

        shell_execute_line(sh, line);
    }
    return 0;
}
