#ifndef MINIFS_DIR_H
#define MINIFS_DIR_H

#include <stdint.h>
#include "common.h"
#include "fs.h"
#include "inode.h"

typedef struct __attribute__((packed)) {
    uint32_t ino;           /* 0 = free slot */
    uint8_t  name_len;
    uint8_t  type;          /* MINIFS_FT_REG, MINIFS_FT_DIR, etc. */
    char     name[58];      /* null-terminated if name_len < 58 */
} dirent_t;

_Static_assert(sizeof(dirent_t) == 64, "dirent_t must be exactly 64 bytes");

typedef int (*dir_iter_cb)(const dirent_t *de, void *ctx);

int dir_init(fs_t *fs, uint32_t new_dir, uint32_t parent);
int dir_lookup(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino);
int dir_add(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type);
int dir_remove(fs_t *fs, uint32_t dir_ino, const char *name);
int dir_list(fs_t *fs, uint32_t dir_ino, dir_iter_cb cb, void *ctx);
int dir_is_empty(fs_t *fs, uint32_t dir_ino);
int path_resolve(fs_t *fs, const char *path, uint32_t cwd, uint32_t *out_ino, uint32_t *out_parent, char *last_name);

/* Helper to bootstrap root directory on formatted disk */
int dir_bootstrap_root(fs_t *fs);

/* Directory hash indexing APIs */
int dir_linear_lookup(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino);
void dir_index_invalidate(fs_t *fs, uint32_t dir_ino);
void dir_index_cleanup(fs_t *fs);

#endif /* MINIFS_DIR_H */
