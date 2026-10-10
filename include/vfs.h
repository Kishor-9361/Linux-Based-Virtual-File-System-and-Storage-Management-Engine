#ifndef MINIFS_VFS_H
#define MINIFS_VFS_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "common.h"
#include "blk.h"
#include "sb.h"
#include "fs.h"
#include "bitmap.h"
#include "inode.h"
#include "fileio.h"
#include "dir.h"

#ifdef st_atime
#undef st_atime
#endif
#ifdef st_mtime
#undef st_mtime
#endif
#ifdef st_ctime
#undef st_ctime
#endif

#define VFS_MAX_OPEN_FILES 256
#define VFS_MAX_FDS        256
#define VFS_FIRST_FD       3
#define VFS_MAX_ORPHANS    256

struct vfs_stat {
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint64_t st_size;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

struct vfs_dirent {
    uint32_t d_ino;
    uint8_t  d_type;
    char     d_name[64];
};

typedef struct {
    uint32_t ino;
    uint64_t offset;
    int      flags;
    uint32_t refcount;
    int      in_use;
} vfs_open_file_t;

typedef struct {
    int ofile_idx; /* Index into open_files, or -1 if closed */
} vfs_fd_entry_t;

typedef struct {
    uint32_t inos[VFS_MAX_ORPHANS];
    uint32_t count;
} vfs_orphan_list_t;

typedef struct vfs {
    blkdev_t dev;
    fs_t     fs;
    int      is_mounted;
    pthread_mutex_t lock;
    uint32_t cwd_ino;
    char     cwd_path[MAX_PATH_LEN];
    mode_t   umask;
    uid_t    uid;
    gid_t    gid;
    vfs_open_file_t open_files[VFS_MAX_OPEN_FILES];
    vfs_fd_entry_t  fds[VFS_MAX_FDS];
    vfs_orphan_list_t orphans;
} vfs_t;

/* Public VFS APIs */
int     vfs_mount(vfs_t *v, const char *img);
int     vfs_unmount(vfs_t *v);
int     vfs_open(vfs_t *v, const char *path, int flags, mode_t mode);
int     vfs_close(vfs_t *v, int fd);
ssize_t vfs_read(vfs_t *v, int fd, void *buf, size_t n);
ssize_t vfs_write(vfs_t *v, int fd, const void *buf, size_t n);
off_t   vfs_lseek(vfs_t *v, int fd, off_t off, int whence);
int     vfs_mkdir(vfs_t *v, const char *path, mode_t mode);
int     vfs_rmdir(vfs_t *v, const char *path);
int     vfs_unlink(vfs_t *v, const char *path);
int     vfs_rename(vfs_t *v, const char *oldpath, const char *newpath);
int     vfs_link(vfs_t *v, const char *oldpath, const char *newpath);
int     vfs_stat(vfs_t *v, const char *path, struct vfs_stat *st);
int     vfs_readdir(vfs_t *v, int fd, struct vfs_dirent *de);
int     vfs_chdir(vfs_t *v, const char *path);
int     vfs_getcwd(vfs_t *v, char *buf, size_t size);
int     vfs_sync(vfs_t *v);

#endif /* MINIFS_VFS_H */
