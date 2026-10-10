#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "vfs.h"
#include "perm.h"
#include "cache.h"
#include "journal.h"
#include "log.h"
#include <string.h>
#include <time.h>

static int is_orphan(vfs_t *v, uint32_t ino) {
    for (uint32_t i = 0; i < v->orphans.count; i++) {
        if (v->orphans.inos[i] == ino) return 1;
    }
    return 0;
}

static void add_orphan(vfs_t *v, uint32_t ino) {
    if (is_orphan(v, ino)) return;
    if (v->orphans.count < VFS_MAX_ORPHANS) {
        v->orphans.inos[v->orphans.count++] = ino;
    }
}

static void remove_orphan(vfs_t *v, uint32_t ino) {
    for (uint32_t i = 0; i < v->orphans.count; i++) {
        if (v->orphans.inos[i] == ino) {
            v->orphans.inos[i] = v->orphans.inos[v->orphans.count - 1];
            v->orphans.count--;
            break;
        }
    }
}

static int is_open_by_any(vfs_t *v, uint32_t ino) {
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (v->open_files[i].in_use && v->open_files[i].ino == ino && v->open_files[i].refcount > 0) {
            return 1;
        }
    }
    return 0;
}

/* Path resolver enforcing X_OK on all directories along the path */
static int vfs_resolve_path_perm(vfs_t *v, const char *path, int want_target_perm,
                                 uint32_t *out_ino, uint32_t *out_parent, char *last_name) {
    if (out_ino) *out_ino = 0;
    if (out_parent) *out_parent = 0;
    if (last_name) last_name[0] = '\0';

    if (!v || !path) return -EINVAL;
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len > MAX_PATH_LEN) return -EINVAL;

    uint32_t curr = (path[0] == '/') ? v->fs.sb.root_inode : ((v->cwd_ino != 0) ? v->cwd_ino : v->fs.sb.root_inode);

    int only_slashes = 1;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] != '/') { only_slashes = 0; break; }
    }
    if (only_slashes) {
        if (out_ino) *out_ino = v->fs.sb.root_inode;
        if (out_parent) *out_parent = v->fs.sb.root_inode;
        if (last_name) strcpy(last_name, "/");
        if (want_target_perm != 0) {
            inode_t root_node;
            inode_read(&v->fs, v->fs.sb.root_inode, &root_node);
            return perm_check(&root_node, v->uid, v->gid, want_target_perm);
        }
        return 0;
    }

    char buf[MAX_PATH_LEN + 1];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *components[MAX_DIR_DEPTH + 2];
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "/", &saveptr);
    while (tok != NULL) {
        if (count >= (int)MAX_DIR_DEPTH) return -ENAMETOOLONG;
        components[count++] = tok;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    if (count == 0) {
        if (out_ino) *out_ino = curr;
        if (out_parent) *out_parent = curr;
        if (last_name) strcpy(last_name, ".");
        return 0;
    }

    /* Traverse intermediate components with X_OK check */
    for (int i = 0; i < count - 1; i++) {
        inode_t curr_node;
        int rc = inode_read(&v->fs, curr, &curr_node);
        if (rc != 0) return rc;
        rc = perm_check(&curr_node, v->uid, v->gid, X_OK);
        if (rc != 0) return rc;

        const char *comp = components[i];
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) {
            if (curr == v->fs.sb.root_inode) continue;
            uint32_t p = 0;
            rc = dir_lookup(&v->fs, curr, "..", &p);
            if (rc != 0) return rc;
            curr = p;
        } else {
            uint32_t next = 0;
            rc = dir_lookup(&v->fs, curr, comp, &next);
            if (rc != 0) return rc;

            inode_t next_node;
            rc = inode_read(&v->fs, next, &next_node);
            if (rc != 0) return rc;
            if (!(next_node.mode & S_IFDIR)) return -ENOTDIR;
            curr = next;
        }
    }

    /* Check X_OK on parent directory */
    inode_t parent_node;
    int rc = inode_read(&v->fs, curr, &parent_node);
    if (rc != 0) return rc;
    rc = perm_check(&parent_node, v->uid, v->gid, X_OK);
    if (rc != 0) return rc;

    const char *final_comp = components[count - 1];
    if (out_parent) *out_parent = curr;
    if (last_name) {
        strncpy(last_name, final_comp, 57);
        last_name[57] = '\0';
    }

    uint32_t target = 0;
    if (strcmp(final_comp, ".") == 0) {
        target = curr;
    } else if (strcmp(final_comp, "..") == 0) {
        if (curr == v->fs.sb.root_inode) target = v->fs.sb.root_inode;
        else dir_lookup(&v->fs, curr, "..", &target);
    } else {
        rc = dir_lookup(&v->fs, curr, final_comp, &target);
        if (rc != 0) return rc;
    }

    if (out_ino) *out_ino = target;

    if (want_target_perm != 0 && target != 0) {
        inode_t target_node;
        rc = inode_read(&v->fs, target, &target_node);
        if (rc != 0) return rc;
        rc = perm_check(&target_node, v->uid, v->gid, want_target_perm);
        if (rc != 0) return rc;
    }

    return 0;
}

int vfs_mount(vfs_t *v, const char *img) {
    if (!v || !img) return -EINVAL;
    memset(v, 0, sizeof(*v));

    int rc = blk_open(&v->dev, img, 0, 0);
    if (rc != 0) return rc;

    rc = fs_init_allocators(&v->fs, &v->dev);
    if (rc != 0) {
        blk_close(&v->dev);
        return rc;
    }

    v->fs.cache = cache_create(&v->dev, DEFAULT_CACHE_SIZE, CACHE_WRITE_BACK);
    if (!v->fs.cache) {
        blk_close(&v->dev);
        return -ENOMEM;
    }

    v->fs.journal = journal_init(&v->fs);
    if (v->fs.sb.state != SB_STATE_CLEAN) {
        journal_replay(&v->fs);
    }

    v->fs.sb.state = SB_STATE_DIRTY;
    v->fs.sb.mount_count++;
    v->fs.sb.last_mount_time = (uint64_t)time(NULL);
    sb_store(&v->dev, &v->fs.sb);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        v->fds[i].ofile_idx = -1;
    }

    v->cwd_ino = v->fs.sb.root_inode;
    strncpy(v->cwd_path, "/", sizeof(v->cwd_path) - 1);
    v->umask = 0022;
    v->uid = 0;
    v->gid = 0;
    pthread_mutex_init(&v->lock, NULL);
    v->is_mounted = 1;

    return 0;
}

int vfs_unmount(vfs_t *v) {
    if (!v || !v->is_mounted) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    for (int i = VFS_FIRST_FD; i < VFS_MAX_FDS; i++) {
        if (v->fds[i].ofile_idx >= 0) {
            v->open_files[v->fds[i].ofile_idx].refcount--;
            if (v->open_files[v->fds[i].ofile_idx].refcount == 0) {
                v->open_files[v->fds[i].ofile_idx].in_use = 0;
            }
            v->fds[i].ofile_idx = -1;
        }
    }

    for (uint32_t i = 0; i < v->orphans.count; i++) {
        inode_destroy(&v->fs, v->orphans.inos[i]);
    }
    v->orphans.count = 0;

    if (v->fs.journal) {
        journal_checkpoint(&v->fs);
        journal_destroy((journal_t *)v->fs.journal);
        v->fs.journal = NULL;
    }

    if (v->fs.cache) {
        cache_flush((cache_t *)v->fs.cache);
        cache_destroy((cache_t *)v->fs.cache);
        v->fs.cache = NULL;
    }

    v->fs.sb.state = SB_STATE_CLEAN;
    sb_store(&v->dev, &v->fs.sb);
    blk_sync(&v->dev);
    blk_close(&v->dev);

    v->is_mounted = 0;
    pthread_mutex_unlock(&v->lock);
    pthread_mutex_destroy(&v->lock);

    return 0;
}

int vfs_open(vfs_t *v, const char *path, int flags, mode_t mode) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t target_ino = 0;
    uint32_t parent_ino = 0;
    char last_name[64] = "";

    int rc = vfs_resolve_path_perm(v, path, 0, &target_ino, &parent_ino, last_name);

    if (rc == -ENOENT) {
        if (!(flags & O_CREAT) || parent_ino == 0) {
            pthread_mutex_unlock(&v->lock);
            return -ENOENT;
        }

        inode_t parent_node;
        rc = inode_read(&v->fs, parent_ino, &parent_node);
        if (rc != 0 || !(parent_node.mode & S_IFDIR)) {
            pthread_mutex_unlock(&v->lock);
            return (rc != 0) ? rc : -ENOTDIR;
        }

        /* Create requires W_OK | X_OK on parent directory */
        rc = perm_check(&parent_node, v->uid, v->gid, W_OK | X_OK);
        if (rc != 0) {
            pthread_mutex_unlock(&v->lock);
            return rc;
        }

        tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
        mode_t eff_mode = (mode & ~v->umask) | S_IFREG;
        int new_ino = inode_create(&v->fs, (uint16_t)eff_mode, v->uid, v->gid);
        if (new_ino < 0) {
            if (tx) tx_abort(tx);
            pthread_mutex_unlock(&v->lock);
            return new_ino;
        }

        rc = dir_add(&v->fs, parent_ino, last_name, (uint32_t)new_ino, MINIFS_FT_REG);
        if (rc != 0) {
            inode_destroy(&v->fs, (uint32_t)new_ino);
            if (tx) tx_abort(tx);
            pthread_mutex_unlock(&v->lock);
            return rc;
        }
        if (tx) tx_commit(tx);
        target_ino = (uint32_t)new_ino;
    } else if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    } else {
        /* File exists */
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            pthread_mutex_unlock(&v->lock);
            return -EEXIST;
        }

        inode_t node;
        rc = inode_read(&v->fs, target_ino, &node);
        if (rc != 0) {
            pthread_mutex_unlock(&v->lock);
            return rc;
        }

        if (node.mode & S_IFDIR) {
            if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC)) {
                pthread_mutex_unlock(&v->lock);
                return -EISDIR;
            }
        }

        /* Check permissions on file */
        int want = 0;
        if ((flags & O_ACCMODE) == O_RDONLY) want = R_OK;
        else if ((flags & O_ACCMODE) == O_WRONLY) want = W_OK;
        else if ((flags & O_ACCMODE) == O_RDWR) want = R_OK | W_OK;
        if (flags & O_TRUNC) want |= W_OK;

        rc = perm_check(&node, v->uid, v->gid, want);
        if (rc != 0) {
            pthread_mutex_unlock(&v->lock);
            return rc;
        }

        if (flags & O_TRUNC && !(node.mode & S_IFDIR)) {
            tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
            file_truncate(&v->fs, target_ino, 0);
            if (tx) tx_commit(tx);
        }
    }

    int fd = -1;
    for (int i = VFS_FIRST_FD; i < VFS_MAX_FDS; i++) {
        if (v->fds[i].ofile_idx < 0) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EMFILE;
    }

    int ofile_idx = -1;
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!v->open_files[i].in_use) {
            ofile_idx = i;
            break;
        }
    }
    if (ofile_idx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -ENFILE;
    }

    inode_t cur_node;
    inode_read(&v->fs, target_ino, &cur_node);

    vfs_open_file_t *of = &v->open_files[ofile_idx];
    of->ino = target_ino;
    of->flags = flags;
    of->offset = (flags & O_APPEND) ? cur_node.size : 0;
    of->refcount = 1;
    of->in_use = 1;

    v->fds[fd].ofile_idx = ofile_idx;

    pthread_mutex_unlock(&v->lock);
    return fd;
}

int vfs_close(vfs_t *v, int fd) {
    if (!v || !v->is_mounted) return -EINVAL;
    if (fd < VFS_FIRST_FD || fd >= VFS_MAX_FDS) return -EBADF;

    pthread_mutex_lock(&v->lock);

    int oidx = v->fds[fd].ofile_idx;
    if (oidx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    v->fds[fd].ofile_idx = -1;
    vfs_open_file_t *of = &v->open_files[oidx];
    of->refcount--;

    if (of->refcount == 0) {
        uint32_t ino = of->ino;
        of->in_use = 0;

        if (is_orphan(v, ino)) {
            if (!is_open_by_any(v, ino)) {
                inode_destroy(&v->fs, ino);
                remove_orphan(v, ino);
            }
        }
    }

    pthread_mutex_unlock(&v->lock);
    return 0;
}

ssize_t vfs_read(vfs_t *v, int fd, void *buf, size_t n) {
    if (!v || !v->is_mounted || !buf) return -EINVAL;
    if (fd < VFS_FIRST_FD || fd >= VFS_MAX_FDS) return -EBADF;

    pthread_mutex_lock(&v->lock);

    int oidx = v->fds[fd].ofile_idx;
    if (oidx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    vfs_open_file_t *of = &v->open_files[oidx];
    if ((of->flags & O_ACCMODE) == O_WRONLY) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    inode_t node;
    int rc = inode_read(&v->fs, of->ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }
    if (node.mode & S_IFDIR) {
        pthread_mutex_unlock(&v->lock);
        return -EISDIR;
    }

    ssize_t rd = file_read(&v->fs, of->ino, of->offset, buf, n);
    if (rd > 0) {
        of->offset += (uint64_t)rd;
    }

    pthread_mutex_unlock(&v->lock);
    return rd;
}

ssize_t vfs_write(vfs_t *v, int fd, const void *buf, size_t n) {
    if (!v || !v->is_mounted || !buf) return -EINVAL;
    if (fd < VFS_FIRST_FD || fd >= VFS_MAX_FDS) return -EBADF;

    pthread_mutex_lock(&v->lock);

    int oidx = v->fds[fd].ofile_idx;
    if (oidx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    vfs_open_file_t *of = &v->open_files[oidx];
    if ((of->flags & O_ACCMODE) == O_RDONLY) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    inode_t node;
    int rc = inode_read(&v->fs, of->ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }
    if (node.mode & S_IFDIR) {
        pthread_mutex_unlock(&v->lock);
        return -EISDIR;
    }

    if (of->flags & O_APPEND) {
        of->offset = node.size;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    ssize_t wr = file_write(&v->fs, of->ino, of->offset, buf, n);
    if (wr > 0) {
        of->offset += (uint64_t)wr;
    }
    if (tx) {
        if (wr >= 0) tx_commit(tx);
        else tx_abort(tx);
    }

    pthread_mutex_unlock(&v->lock);
    return wr;
}

off_t vfs_lseek(vfs_t *v, int fd, off_t off, int whence) {
    if (!v || !v->is_mounted) return -EINVAL;
    if (fd < VFS_FIRST_FD || fd >= VFS_MAX_FDS) return -EBADF;

    pthread_mutex_lock(&v->lock);

    int oidx = v->fds[fd].ofile_idx;
    if (oidx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    vfs_open_file_t *of = &v->open_files[oidx];
    inode_t node;
    int rc = inode_read(&v->fs, of->ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    off_t new_off = 0;
    switch (whence) {
        case SEEK_SET:
            new_off = off;
            break;
        case SEEK_CUR:
            new_off = (off_t)of->offset + off;
            break;
        case SEEK_END:
            new_off = (off_t)node.size + off;
            break;
        default:
            pthread_mutex_unlock(&v->lock);
            return -EINVAL;
    }

    if (new_off < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EINVAL;
    }

    of->offset = (uint64_t)new_off;
    pthread_mutex_unlock(&v->lock);
    return new_off;
}

int vfs_mkdir(vfs_t *v, const char *path, mode_t mode) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t target_ino = 0;
    uint32_t parent_ino = 0;
    char last_name[64] = "";

    int rc = vfs_resolve_path_perm(v, path, 0, &target_ino, &parent_ino, last_name);
    if (rc == 0) {
        pthread_mutex_unlock(&v->lock);
        return -EEXIST;
    }
    if (rc != -ENOENT || parent_ino == 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t parent_node;
    rc = inode_read(&v->fs, parent_ino, &parent_node);
    if (rc != 0 || !(parent_node.mode & S_IFDIR)) {
        pthread_mutex_unlock(&v->lock);
        return (rc != 0) ? rc : -ENOTDIR;
    }

    /* Create requires W_OK | X_OK on parent directory */
    rc = perm_check(&parent_node, v->uid, v->gid, W_OK | X_OK);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    mode_t eff = (mode & ~v->umask) | S_IFDIR;
    int ino = inode_create(&v->fs, (uint16_t)eff, v->uid, v->gid);
    if (ino < 0) {
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return ino;
    }

    rc = dir_init(&v->fs, (uint32_t)ino, parent_ino);
    if (rc != 0) {
        inode_destroy(&v->fs, (uint32_t)ino);
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    rc = dir_add(&v->fs, parent_ino, last_name, (uint32_t)ino, MINIFS_FT_DIR);
    if (rc != 0) {
        inode_destroy(&v->fs, (uint32_t)ino);
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (tx) tx_commit(tx);

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_rmdir(vfs_t *v, const char *path) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t target_ino = 0;
    uint32_t parent_ino = 0;
    char last_name[64] = "";

    int rc = vfs_resolve_path_perm(v, path, 0, &target_ino, &parent_ino, last_name);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (target_ino == v->fs.sb.root_inode) {
        pthread_mutex_unlock(&v->lock);
        return -EBUSY;
    }

    if (strcmp(last_name, ".") == 0 || strcmp(last_name, "..") == 0) {
        pthread_mutex_unlock(&v->lock);
        return -EINVAL;
    }

    inode_t parent_node;
    rc = inode_read(&v->fs, parent_ino, &parent_node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    /* Rmdir requires W_OK | X_OK on parent directory */
    rc = perm_check(&parent_node, v->uid, v->gid, W_OK | X_OK);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t target_node;
    rc = inode_read(&v->fs, target_ino, &target_node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (!(target_node.mode & S_IFDIR)) {
        pthread_mutex_unlock(&v->lock);
        return -ENOTDIR;
    }

    if (!dir_is_empty(&v->fs, target_ino)) {
        pthread_mutex_unlock(&v->lock);
        return -ENOTEMPTY;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    rc = dir_remove(&v->fs, parent_ino, last_name);
    if (rc != 0) {
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (parent_node.nlink > 0) parent_node.nlink--;
    inode_write(&v->fs, parent_ino, &parent_node);

    inode_destroy(&v->fs, target_ino);
    if (tx) tx_commit(tx);

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_unlink(vfs_t *v, const char *path) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t target_ino = 0;
    uint32_t parent_ino = 0;
    char last_name[64] = "";

    int rc = vfs_resolve_path_perm(v, path, 0, &target_ino, &parent_ino, last_name);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t parent_node;
    rc = inode_read(&v->fs, parent_ino, &parent_node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    /* Unlink requires W_OK | X_OK on parent directory */
    rc = perm_check(&parent_node, v->uid, v->gid, W_OK | X_OK);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t node;
    rc = inode_read(&v->fs, target_ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (node.mode & S_IFDIR) {
        pthread_mutex_unlock(&v->lock);
        return -EISDIR;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    rc = dir_remove(&v->fs, parent_ino, last_name);
    if (rc != 0) {
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (node.nlink > 0) node.nlink--;

    if (node.nlink == 0) {
        if (is_open_by_any(v, target_ino)) {
            inode_write(&v->fs, target_ino, &node);
            add_orphan(v, target_ino);
        } else {
            inode_destroy(&v->fs, target_ino);
        }
    } else {
        inode_write(&v->fs, target_ino, &node);
    }
    if (tx) tx_commit(tx);

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_rename(vfs_t *v, const char *oldpath, const char *newpath) {
    if (!v || !v->is_mounted || !oldpath || !newpath) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t old_ino = 0, old_parent = 0;
    char old_name[64] = "";
    int rc = vfs_resolve_path_perm(v, oldpath, 0, &old_ino, &old_parent, old_name);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    uint32_t new_ino = 0, new_parent = 0;
    char new_name[64] = "";
    rc = vfs_resolve_path_perm(v, newpath, 0, &new_ino, &new_parent, new_name);
    if (rc != 0) {
        if (rc != -ENOENT || new_parent == 0) {
            pthread_mutex_unlock(&v->lock);
            return rc;
        }
    }

    /* Perm check on both parent directories */
    inode_t op_node, np_node;
    inode_read(&v->fs, old_parent, &op_node);
    if (perm_check(&op_node, v->uid, v->gid, W_OK | X_OK) != 0) {
        pthread_mutex_unlock(&v->lock);
        return -EACCES;
    }
    inode_read(&v->fs, new_parent, &np_node);
    if (perm_check(&np_node, v->uid, v->gid, W_OK | X_OK) != 0) {
        pthread_mutex_unlock(&v->lock);
        return -EACCES;
    }

    if (old_ino == new_ino && old_ino != 0) {
        pthread_mutex_unlock(&v->lock);
        return 0;
    }

    inode_t old_node;
    rc = inode_read(&v->fs, old_ino, &old_node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (old_node.mode & S_IFDIR) {
        uint32_t curr = new_parent;
        while (curr != v->fs.sb.root_inode && curr != 0) {
            if (curr == old_ino) {
                pthread_mutex_unlock(&v->lock);
                return -EINVAL;
            }
            uint32_t up = 0;
            if (dir_lookup(&v->fs, curr, "..", &up) != 0) break;
            curr = up;
        }

        if (new_ino != 0) {
            inode_t new_node;
            inode_read(&v->fs, new_ino, &new_node);
            if (!(new_node.mode & S_IFDIR)) {
                pthread_mutex_unlock(&v->lock);
                return -ENOTDIR;
            }
            if (!dir_is_empty(&v->fs, new_ino)) {
                pthread_mutex_unlock(&v->lock);
                return -ENOTEMPTY;
            }
        }
    } else {
        if (new_ino != 0) {
            inode_t new_node;
            inode_read(&v->fs, new_ino, &new_node);
            if (new_node.mode & S_IFDIR) {
                pthread_mutex_unlock(&v->lock);
                return -EISDIR;
            }
        }
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;

    if (old_node.mode & S_IFDIR) {
        if (new_ino != 0) {
            inode_t new_node;
            inode_read(&v->fs, new_ino, &new_node);
            dir_remove(&v->fs, new_parent, new_name);
            inode_destroy(&v->fs, new_ino);

            if (inode_read(&v->fs, new_parent, &np_node) == 0) {
                if (np_node.nlink > 0) np_node.nlink--;
                inode_write(&v->fs, new_parent, &np_node);
            }
        }

        dir_add(&v->fs, new_parent, new_name, old_ino, MINIFS_FT_DIR);
        dir_remove(&v->fs, old_parent, old_name);

        if (old_parent != new_parent) {
            dirent_t de;
            de.ino = new_parent;
            de.type = MINIFS_FT_DIR;
            de.name_len = 2;
            strcpy(de.name, "..");
            file_write(&v->fs, old_ino, sizeof(dirent_t), &de, sizeof(de));

            inode_read(&v->fs, old_parent, &op_node);
            if (op_node.nlink > 0) op_node.nlink--;
            inode_write(&v->fs, old_parent, &op_node);

            inode_read(&v->fs, new_parent, &np_node);
            np_node.nlink++;
            inode_write(&v->fs, new_parent, &np_node);
        }
    } else {
        if (new_ino != 0) {
            inode_t new_node;
            inode_read(&v->fs, new_ino, &new_node);
            dir_remove(&v->fs, new_parent, new_name);
            if (new_node.nlink > 0) new_node.nlink--;
            if (new_node.nlink == 0) {
                if (is_open_by_any(v, new_ino)) {
                    inode_write(&v->fs, new_ino, &new_node);
                    add_orphan(v, new_ino);
                } else {
                    inode_destroy(&v->fs, new_ino);
                }
            } else {
                inode_write(&v->fs, new_ino, &new_node);
            }
        }

        dir_add(&v->fs, new_parent, new_name, old_ino, MINIFS_FT_REG);
        dir_remove(&v->fs, old_parent, old_name);
    }

    if (tx) tx_commit(tx);

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_link(vfs_t *v, const char *oldpath, const char *newpath) {
    if (!v || !v->is_mounted || !oldpath || !newpath) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t old_ino = 0;
    int rc = vfs_resolve_path_perm(v, oldpath, 0, &old_ino, NULL, NULL);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t node;
    rc = inode_read(&v->fs, old_ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (node.mode & S_IFDIR) {
        pthread_mutex_unlock(&v->lock);
        return -EPERM;
    }

    if (node.nlink >= 65535) {
        pthread_mutex_unlock(&v->lock);
        return -EMLINK;
    }

    uint32_t new_ino = 0, new_parent = 0;
    char new_name[64] = "";
    rc = vfs_resolve_path_perm(v, newpath, 0, &new_ino, &new_parent, new_name);
    if (rc == 0) {
        pthread_mutex_unlock(&v->lock);
        return -EEXIST;
    }
    if (rc != -ENOENT || new_parent == 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t np_node;
    inode_read(&v->fs, new_parent, &np_node);
    if (perm_check(&np_node, v->uid, v->gid, W_OK | X_OK) != 0) {
        pthread_mutex_unlock(&v->lock);
        return -EACCES;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    rc = dir_add(&v->fs, new_parent, new_name, old_ino, MINIFS_FT_REG);
    if (rc != 0) {
        if (tx) tx_abort(tx);
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    node.nlink++;
    inode_touch(&node, 0, 0, 1);
    rc = inode_write(&v->fs, old_ino, &node);
    if (tx) {
        if (rc == 0) tx_commit(tx);
        else tx_abort(tx);
    }

    pthread_mutex_unlock(&v->lock);
    return rc;
}

int vfs_stat(vfs_t *v, const char *path, struct vfs_stat *st) {
    if (!v || !v->is_mounted || !path || !st) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = vfs_resolve_path_perm(v, path, 0, &ino, NULL, NULL);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t node;
    rc = inode_read(&v->fs, ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    st->st_ino = ino;
    st->st_mode = node.mode;
    st->st_nlink = node.nlink;
    st->st_uid = node.uid;
    st->st_gid = node.gid;
    st->st_size = node.size;
    st->st_blocks = node.blocks;
    st->st_atime = node.atime;
    st->st_mtime = node.mtime;
    st->st_ctime = node.ctime;

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_truncate(vfs_t *v, const char *path, uint64_t length) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = vfs_resolve_path_perm(v, path, W_OK, &ino, NULL, NULL);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t node;
    rc = inode_read(&v->fs, ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (node.mode & S_IFDIR) {
        pthread_mutex_unlock(&v->lock);
        return -EISDIR;
    }

    rc = perm_check(&node, v->uid, v->gid, W_OK);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    tx_t *tx = (v->fs.journal) ? tx_begin(&v->fs) : NULL;
    rc = file_truncate(&v->fs, ino, length);
    if (tx) {
        if (rc == 0) tx_commit(tx);
        else tx_abort(tx);
    }

    pthread_mutex_unlock(&v->lock);
    return rc;
}

int vfs_readdir(vfs_t *v, int fd, struct vfs_dirent *de) {
    if (!v || !v->is_mounted || !de) return -EINVAL;
    if (fd < VFS_FIRST_FD || fd >= VFS_MAX_FDS) return -EBADF;

    pthread_mutex_lock(&v->lock);

    int oidx = v->fds[fd].ofile_idx;
    if (oidx < 0) {
        pthread_mutex_unlock(&v->lock);
        return -EBADF;
    }

    vfs_open_file_t *of = &v->open_files[oidx];
    inode_t node;
    int rc = inode_read(&v->fs, of->ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (!(node.mode & S_IFDIR)) {
        pthread_mutex_unlock(&v->lock);
        return -ENOTDIR;
    }

    dirent_t raw_de;
    while (of->offset < node.size) {
        ssize_t rd = file_read(&v->fs, of->ino, of->offset, &raw_de, sizeof(raw_de));
        if (rd < (ssize_t)sizeof(raw_de)) {
            pthread_mutex_unlock(&v->lock);
            return 0;
        }

        of->offset += sizeof(raw_de);
        if (raw_de.ino != 0) {
            de->d_ino = raw_de.ino;
            de->d_type = raw_de.type;
            strncpy(de->d_name, raw_de.name, sizeof(de->d_name) - 1);
            de->d_name[sizeof(de->d_name) - 1] = '\0';
            pthread_mutex_unlock(&v->lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_chdir(vfs_t *v, const char *path) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = vfs_resolve_path_perm(v, path, X_OK, &ino, NULL, NULL);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    inode_t node;
    rc = inode_read(&v->fs, ino, &node);
    if (rc != 0) {
        pthread_mutex_unlock(&v->lock);
        return rc;
    }

    if (!(node.mode & S_IFDIR)) {
        pthread_mutex_unlock(&v->lock);
        return -ENOTDIR;
    }

    v->cwd_ino = ino;
    if (path[0] == '/') {
        strncpy(v->cwd_path, path, sizeof(v->cwd_path) - 1);
    }

    pthread_mutex_unlock(&v->lock);
    return 0;
}

int vfs_getcwd(vfs_t *v, char *buf, size_t size) {
    if (!v || !v->is_mounted || !buf || size == 0) return -EINVAL;

    pthread_mutex_lock(&v->lock);
    strncpy(buf, v->cwd_path, size - 1);
    buf[size - 1] = '\0';
    pthread_mutex_unlock(&v->lock);

    return 0;
}

int vfs_sync(vfs_t *v) {
    if (!v || !v->is_mounted) return -EINVAL;

    pthread_mutex_lock(&v->lock);
    if (v->fs.journal) {
        journal_checkpoint(&v->fs);
    }
    if (v->fs.cache) {
        cache_flush((cache_t *)v->fs.cache);
    }
    sb_store(&v->dev, &v->fs.sb);
    int rc = blk_sync(&v->dev);
    pthread_mutex_unlock(&v->lock);
    return rc;
}

