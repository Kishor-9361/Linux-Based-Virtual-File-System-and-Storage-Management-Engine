#ifndef MINIFS_PERM_H
#define MINIFS_PERM_H

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include "common.h"
#include "inode.h"
#include "vfs.h"

#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

void vfs_set_user(vfs_t *v, uid_t uid, gid_t gid);
int  perm_check(const inode_t *i, uid_t u, gid_t g, int want);
int  vfs_chmod(vfs_t *v, const char *path, mode_t mode);
int  vfs_chown(vfs_t *v, const char *path, uid_t uid, gid_t gid);
int  vfs_utimes(vfs_t *v, const char *path, uint32_t atime, uint32_t mtime);
int  vfs_umask(vfs_t *v, mode_t mask);

#endif /* MINIFS_PERM_H */
