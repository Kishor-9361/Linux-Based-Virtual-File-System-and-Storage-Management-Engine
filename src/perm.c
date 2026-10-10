#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "perm.h"
#include "log.h"
#include <string.h>
#include <time.h>

void vfs_set_user(vfs_t *v, uid_t uid, gid_t gid) {
    if (!v) return;
    v->uid = uid;
    v->gid = gid;
}

int vfs_umask(vfs_t *v, mode_t mask) {
    if (!v) return 0;
    mode_t old = v->umask;
    v->umask = mask & 0777;
    return (int)old;
}

int perm_check(const inode_t *i, uid_t u, gid_t g, int want) {
    if (!i) return -EINVAL;
    if (want == 0) return 0;

    /* Root bypass */
    if (u == 0) {
        /* If execute requested on a regular file, at least one X bit must be set */
        if ((want & X_OK) && !(i->mode & S_IFDIR)) {
            if ((i->mode & 0111) == 0) {
                return -EACCES;
            }
        }
        return 0;
    }

    int shift;
    if (u == (uid_t)i->uid) {
        shift = 6;
    } else if (g == (gid_t)i->gid) {
        shift = 3;
    } else {
        shift = 0;
    }

    int class_bits = (i->mode >> shift) & 07;
    if ((class_bits & want) == want) {
        return 0;
    }

    return -EACCES;
}

int vfs_chmod(vfs_t *v, const char *path, mode_t mode) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = path_resolve(&v->fs, path, v->cwd_ino, &ino, NULL, NULL);
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

    /* Only owner or root can chmod */
    if (v->uid != 0 && v->uid != (uid_t)node.uid) {
        pthread_mutex_unlock(&v->lock);
        return -EPERM;
    }

    node.mode = (uint16_t)((node.mode & S_IFMT) | (mode & 07777));
    node.ctime = (uint32_t)time(NULL);
    rc = inode_write(&v->fs, ino, &node);

    pthread_mutex_unlock(&v->lock);
    return rc;
}

int vfs_chown(vfs_t *v, const char *path, uid_t uid, gid_t gid) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = path_resolve(&v->fs, path, v->cwd_ino, &ino, NULL, NULL);
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

    /* Changing owner requires root */
    if (uid != (uid_t)-1 && uid != (uid_t)node.uid && v->uid != 0) {
        pthread_mutex_unlock(&v->lock);
        return -EPERM;
    }

    /* Changing group requires root or owner */
    if (gid != (gid_t)-1 && gid != (gid_t)node.gid && v->uid != 0 && v->uid != (uid_t)node.uid) {
        pthread_mutex_unlock(&v->lock);
        return -EPERM;
    }

    if (uid != (uid_t)-1) node.uid = (uint16_t)uid;
    if (gid != (gid_t)-1) node.gid = (uint16_t)gid;
    node.ctime = (uint32_t)time(NULL);
    rc = inode_write(&v->fs, ino, &node);

    pthread_mutex_unlock(&v->lock);
    return rc;
}

int vfs_utimes(vfs_t *v, const char *path, uint32_t atime, uint32_t mtime) {
    if (!v || !v->is_mounted || !path) return -EINVAL;

    pthread_mutex_lock(&v->lock);

    uint32_t ino = 0;
    int rc = path_resolve(&v->fs, path, v->cwd_ino, &ino, NULL, NULL);
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

    if (v->uid != 0 && v->uid != (uid_t)node.uid) {
        pthread_mutex_unlock(&v->lock);
        return -EPERM;
    }

    if (atime) node.atime = atime;
    if (mtime) node.mtime = mtime;
    node.ctime = (uint32_t)time(NULL);
    rc = inode_write(&v->fs, ino, &node);

    pthread_mutex_unlock(&v->lock);
    return rc;
}
