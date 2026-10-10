#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "dir.h"
#include "fileio.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#define DIRENTS_PER_BLOCK (BLOCK_SIZE / sizeof(dirent_t))

int dir_init(fs_t *fs, uint32_t new_dir, uint32_t parent) {
    if (!fs || new_dir < 1 || new_dir >= fs->sb.total_inodes ||
        parent < 1 || parent >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    dirent_t entries[2];
    memset(entries, 0, sizeof(entries));

    /* Entry 0: "." */
    entries[0].ino = new_dir;
    entries[0].type = MINIFS_FT_DIR;
    entries[0].name_len = 1;
    strncpy(entries[0].name, ".", sizeof(entries[0].name) - 1);

    /* Entry 1: ".." */
    entries[1].ino = parent;
    entries[1].type = MINIFS_FT_DIR;
    entries[1].name_len = 2;
    strncpy(entries[1].name, "..", sizeof(entries[1].name) - 1);

    ssize_t written = file_write(fs, new_dir, 0, entries, sizeof(entries));
    if (written < (ssize_t)sizeof(entries)) {
        return (written < 0) ? (int)written : -EIO;
    }

    /* Update new_dir link count to 2 (itself and ".") */
    inode_t node;
    int rc = inode_read(fs, new_dir, &node);
    if (rc != 0) return rc;
    node.mode |= S_IFDIR;
    node.nlink = 2;
    rc = inode_write(fs, new_dir, &node);
    if (rc != 0) return rc;

    /* Increment parent's link count for ".." (if parent is not itself) */
    if (parent != new_dir) {
        inode_t parent_node;
        rc = inode_read(fs, parent, &parent_node);
        if (rc != 0) return rc;
        parent_node.nlink++;
        rc = inode_write(fs, parent, &parent_node);
        if (rc != 0) return rc;
    }

    dir_index_invalidate(fs, new_dir);
    if (parent != new_dir) {
        dir_index_invalidate(fs, parent);
    }

    return 0;
}

int dir_bootstrap_root(fs_t *fs) {
    if (!fs) return -EINVAL;
    inode_t root;
    memset(&root, 0, sizeof(root));
    root.mode = S_IFDIR | 0755;
    root.uid = 0;
    root.gid = 0;
    root.nlink = 2;
    inode_touch(&root, 1, 1, 1);
    int rc = inode_write(fs, fs->sb.root_inode, &root);
    if (rc != 0) return rc;

    return dir_init(fs, fs->sb.root_inode, fs->sb.root_inode);
}

/* =========================================================================
 * In-Memory Directory Hash Index Implementation
 * ========================================================================= */
#define DIR_HASH_BUCKETS 128
#define DIR_TABLE_BUCKETS 64

typedef struct dir_hash_node {
    uint32_t ino;
    uint8_t  type;
    uint8_t  name_len;
    char     name[60];
    struct dir_hash_node *next;
} dir_hash_node_t;

typedef struct dir_hash_table {
    uint32_t dir_ino;
    bool is_populated;
    dir_hash_node_t *buckets[DIR_HASH_BUCKETS];
    struct dir_hash_table *next;
} dir_hash_table_t;

typedef struct {
    dir_hash_table_t *tables[DIR_TABLE_BUCKETS];
} dir_index_cache_t;

static inline uint32_t dir_hash_djb2(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + (uint32_t)c;
    }
    return hash;
}

static dir_index_cache_t *get_or_create_cache(fs_t *fs) {
    if (!fs) return NULL;
    if (!fs->dir_index) {
        fs->dir_index = calloc(1, sizeof(dir_index_cache_t));
    }
    return (dir_index_cache_t *)fs->dir_index;
}

static dir_hash_table_t *get_dir_table(fs_t *fs, uint32_t dir_ino, bool create) {
    dir_index_cache_t *cache = get_or_create_cache(fs);
    if (!cache) return NULL;
    uint32_t slot = dir_ino % DIR_TABLE_BUCKETS;
    dir_hash_table_t *curr = cache->tables[slot];
    while (curr) {
        if (curr->dir_ino == dir_ino) return curr;
        curr = curr->next;
    }
    if (!create) return NULL;
    dir_hash_table_t *tbl = calloc(1, sizeof(dir_hash_table_t));
    if (!tbl) return NULL;
    tbl->dir_ino = dir_ino;
    tbl->next = cache->tables[slot];
    cache->tables[slot] = tbl;
    return tbl;
}

static void free_dir_table_nodes(dir_hash_table_t *tbl) {
    if (!tbl) return;
    for (int i = 0; i < DIR_HASH_BUCKETS; i++) {
        dir_hash_node_t *curr = tbl->buckets[i];
        while (curr) {
            dir_hash_node_t *next = curr->next;
            free(curr);
            curr = next;
        }
        tbl->buckets[i] = NULL;
    }
    tbl->is_populated = false;
}

void dir_index_invalidate(fs_t *fs, uint32_t dir_ino) {
    if (!fs || !fs->dir_index) return;
    dir_index_cache_t *cache = (dir_index_cache_t *)fs->dir_index;
    uint32_t slot = dir_ino % DIR_TABLE_BUCKETS;
    dir_hash_table_t **pprev = &cache->tables[slot];
    while (*pprev) {
        dir_hash_table_t *curr = *pprev;
        if (curr->dir_ino == dir_ino) {
            *pprev = curr->next;
            free_dir_table_nodes(curr);
            free(curr);
            return;
        }
        pprev = &curr->next;
    }
}

void dir_index_cleanup(fs_t *fs) {
    if (!fs || !fs->dir_index) return;
    dir_index_cache_t *cache = (dir_index_cache_t *)fs->dir_index;
    for (int s = 0; s < DIR_TABLE_BUCKETS; s++) {
        dir_hash_table_t *curr = cache->tables[s];
        while (curr) {
            dir_hash_table_t *next = curr->next;
            free_dir_table_nodes(curr);
            free(curr);
            curr = next;
        }
    }
    free(cache);
    fs->dir_index = NULL;
}

static int dir_index_populate(fs_t *fs, dir_hash_table_t *tbl, inode_t *dir_node) {
    if (!fs || !tbl || !dir_node) return -EINVAL;
    free_dir_table_nodes(tbl);

    uint64_t offset = 0;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    while (offset < dir_node->size) {
        size_t to_read = (dir_node->size - offset < BLOCK_SIZE) ? (size_t)(dir_node->size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, tbl->dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0) {
                dir_hash_node_t *node = malloc(sizeof(dir_hash_node_t));
                if (!node) return -ENOMEM;
                node->ino = blk_entries[i].ino;
                node->type = blk_entries[i].type;
                node->name_len = blk_entries[i].name_len;
                strncpy(node->name, blk_entries[i].name, sizeof(node->name) - 1);
                node->name[sizeof(node->name) - 1] = '\0';

                uint32_t b = dir_hash_djb2(node->name) % DIR_HASH_BUCKETS;
                node->next = tbl->buckets[b];
                tbl->buckets[b] = node;
            }
        }
        offset += (uint64_t)rd;
    }
    tbl->is_populated = true;
    return 0;
}

int dir_linear_lookup(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    if (!fs || !name || dir_ino < 1 || dir_ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, dir_ino, &node);
    if (rc != 0) return rc;

    if (!(node.mode & S_IFDIR)) {
        return -ENOTDIR;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > MAX_NAME_LEN) {
        return -EINVAL;
    }

    uint64_t offset = 0;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    while (offset < node.size) {
        size_t to_read = (node.size - offset < BLOCK_SIZE) ? (size_t)(node.size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0 && blk_entries[i].name_len == name_len &&
                strcmp(blk_entries[i].name, name) == 0) {
                if (out_ino) *out_ino = blk_entries[i].ino;
                return 0;
            }
        }
        offset += (uint64_t)rd;
    }

    return -ENOENT;
}

int dir_lookup(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    if (!fs || !name || dir_ino < 1 || dir_ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > MAX_NAME_LEN) {
        return -EINVAL;
    }

    dir_hash_table_t *tbl = get_dir_table(fs, dir_ino, true);
    if (!tbl) {
        return dir_linear_lookup(fs, dir_ino, name, out_ino);
    }

    if (!tbl->is_populated) {
        inode_t node;
        int rc = inode_read(fs, dir_ino, &node);
        if (rc != 0) return rc;
        if (!(node.mode & S_IFDIR)) return -ENOTDIR;
        rc = dir_index_populate(fs, tbl, &node);
        if (rc != 0) return dir_linear_lookup(fs, dir_ino, name, out_ino);
    }

    /* O(1) hash table lookup */
    uint32_t b = dir_hash_djb2(name) % DIR_HASH_BUCKETS;
    dir_hash_node_t *curr = tbl->buckets[b];
    while (curr) {
        if (curr->name_len == name_len && strcmp(curr->name, name) == 0) {
            if (out_ino) *out_ino = curr->ino;
            return 0;
        }
        curr = curr->next;
    }

    return -ENOENT;
}

int dir_add(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type) {
    if (!fs || !name || dir_ino < 1 || dir_ino >= fs->sb.total_inodes ||
        ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > MAX_NAME_LEN) return -EINVAL;
    if (strchr(name, '/') != NULL) return -EINVAL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return -EEXIST;

    inode_t node;
    int rc = inode_read(fs, dir_ino, &node);
    if (rc != 0) return rc;

    if (!(node.mode & S_IFDIR)) return -ENOTDIR;

    uint64_t offset = 0;
    uint64_t free_slot_off = (uint64_t)-1;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    /* Scan in blocks to check duplicates and find first free slot */
    while (offset < node.size) {
        size_t to_read = (node.size - offset < BLOCK_SIZE) ? (size_t)(node.size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0) {
                if (blk_entries[i].name_len == name_len && strcmp(blk_entries[i].name, name) == 0) {
                    return -EEXIST;
                }
            } else {
                if (free_slot_off == (uint64_t)-1) {
                    free_slot_off = offset + (uint64_t)(i * sizeof(dirent_t));
                }
            }
        }
        offset += (uint64_t)rd;
    }

    /* Target slot */
    uint64_t target_off = (free_slot_off != (uint64_t)-1) ? free_slot_off : node.size;

    dirent_t new_entry;
    memset(&new_entry, 0, sizeof(new_entry));
    new_entry.ino = ino;
    new_entry.name_len = (uint8_t)name_len;
    new_entry.type = type;
    strncpy(new_entry.name, name, sizeof(new_entry.name) - 1);

    ssize_t wr = file_write(fs, dir_ino, target_off, &new_entry, sizeof(new_entry));
    if (wr < (ssize_t)sizeof(new_entry)) {
        return (wr < 0) ? (int)wr : -ENOSPC;
    }

    /* Update in-memory hash table if populated */
    dir_hash_table_t *tbl = get_dir_table(fs, dir_ino, false);
    if (tbl && tbl->is_populated) {
        dir_hash_node_t *hn = malloc(sizeof(dir_hash_node_t));
        if (hn) {
            hn->ino = ino;
            hn->type = type;
            hn->name_len = (uint8_t)name_len;
            strncpy(hn->name, name, sizeof(hn->name) - 1);
            hn->name[sizeof(hn->name) - 1] = '\0';
            uint32_t b = dir_hash_djb2(name) % DIR_HASH_BUCKETS;
            hn->next = tbl->buckets[b];
            tbl->buckets[b] = hn;
        }
    }

    return 0;
}

int dir_remove(fs_t *fs, uint32_t dir_ino, const char *name) {
    if (!fs || !name || dir_ino < 1 || dir_ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, dir_ino, &node);
    if (rc != 0) return rc;

    if (!(node.mode & S_IFDIR)) return -ENOTDIR;

    size_t name_len = strlen(name);
    uint64_t offset = 0;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    while (offset < node.size) {
        size_t to_read = (node.size - offset < BLOCK_SIZE) ? (size_t)(node.size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0 && blk_entries[i].name_len == name_len &&
                strcmp(blk_entries[i].name, name) == 0) {
                uint64_t entry_off = offset + (uint64_t)(i * sizeof(dirent_t));
                dirent_t zero_de;
                memset(&zero_de, 0, sizeof(zero_de));
                ssize_t wr = file_write(fs, dir_ino, entry_off, &zero_de, sizeof(zero_de));
                if (wr < (ssize_t)sizeof(zero_de)) {
                    return (wr < 0) ? (int)wr : -EIO;
                }

                /* Remove from in-memory hash table if populated */
                dir_hash_table_t *tbl = get_dir_table(fs, dir_ino, false);
                if (tbl && tbl->is_populated) {
                    uint32_t b = dir_hash_djb2(name) % DIR_HASH_BUCKETS;
                    dir_hash_node_t **pprev = &tbl->buckets[b];
                    while (*pprev) {
                        dir_hash_node_t *c = *pprev;
                        if (c->name_len == name_len && strcmp(c->name, name) == 0) {
                            *pprev = c->next;
                            free(c);
                            break;
                        }
                        pprev = &c->next;
                    }
                }

                return 0;
            }
        }
        offset += (uint64_t)rd;
    }

    return -ENOENT;
}

int dir_list(fs_t *fs, uint32_t dir_ino, dir_iter_cb cb, void *ctx) {
    if (!fs || !cb || dir_ino < 1 || dir_ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, dir_ino, &node);
    if (rc != 0) return rc;

    if (!(node.mode & S_IFDIR)) return -ENOTDIR;

    uint64_t offset = 0;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    while (offset < node.size) {
        size_t to_read = (node.size - offset < BLOCK_SIZE) ? (size_t)(node.size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0) {
                int stop = cb(&blk_entries[i], ctx);
                if (stop != 0) return 0;
            }
        }
        offset += (uint64_t)rd;
    }

    return 0;
}

int dir_is_empty(fs_t *fs, uint32_t dir_ino) {
    if (!fs || dir_ino < 1 || dir_ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, dir_ino, &node);
    if (rc != 0) return rc;

    if (!(node.mode & S_IFDIR)) return -ENOTDIR;

    uint64_t offset = 0;
    dirent_t blk_entries[DIRENTS_PER_BLOCK];

    while (offset < node.size) {
        size_t to_read = (node.size - offset < BLOCK_SIZE) ? (size_t)(node.size - offset) : BLOCK_SIZE;
        ssize_t rd = file_read(fs, dir_ino, offset, blk_entries, to_read);
        if (rd <= 0) break;

        size_t count = (size_t)rd / sizeof(dirent_t);
        for (size_t i = 0; i < count; i++) {
            if (blk_entries[i].ino != 0) {
                if (strcmp(blk_entries[i].name, ".") != 0 && strcmp(blk_entries[i].name, "..") != 0) {
                    return 0; /* Has entry, not empty */
                }
            }
        }
        offset += (uint64_t)rd;
    }

    return 1; /* Empty */
}

int path_resolve(fs_t *fs, const char *path, uint32_t cwd, uint32_t *out_ino, uint32_t *out_parent, char *last_name) {
    if (out_ino) *out_ino = 0;
    if (out_parent) *out_parent = 0;
    if (last_name) last_name[0] = '\0';

    if (!fs || !path) return -EINVAL;

    size_t path_len = strlen(path);
    if (path_len == 0 || path_len > MAX_PATH_LEN) return -EINVAL;

    uint32_t curr = (path[0] == '/') ? fs->sb.root_inode : ((cwd != 0) ? cwd : fs->sb.root_inode);

    /* Handle root path "/" */
    int only_slashes = 1;
    for (size_t i = 0; i < path_len; i++) {
        if (path[i] != '/') {
            only_slashes = 0;
            break;
        }
    }

    if (only_slashes) {
        if (out_ino) *out_ino = fs->sb.root_inode;
        if (out_parent) *out_parent = fs->sb.root_inode;
        if (last_name) strcpy(last_name, "/");
        return 0;
    }

    /* Duplicate path for tokenization */
    char buf[MAX_PATH_LEN + 1];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Parse components */
    char *components[MAX_DIR_DEPTH + 2];
    int count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "/", &saveptr);

    while (tok != NULL) {
        if (count >= (int)MAX_DIR_DEPTH) {
            return -ENAMETOOLONG;
        }
        components[count++] = tok;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    if (count == 0) {
        if (out_ino) *out_ino = curr;
        if (out_parent) *out_parent = curr;
        if (last_name) strcpy(last_name, ".");
        return 0;
    }

    /* Traverse up to the last component */
    for (int i = 0; i < count - 1; i++) {
        const char *comp = components[i];
        if (strcmp(comp, ".") == 0) {
            continue;
        } else if (strcmp(comp, "..") == 0) {
            if (curr == fs->sb.root_inode) {
                /* Root's .. remains root */
                continue;
            }
            uint32_t parent = 0;
            int rc = dir_lookup(fs, curr, "..", &parent);
            if (rc != 0) return rc;
            curr = parent;
        } else {
            uint32_t next = 0;
            int rc = dir_lookup(fs, curr, comp, &next);
            if (rc != 0) return rc;

            inode_t next_node;
            rc = inode_read(fs, next, &next_node);
            if (rc != 0) return rc;
            if (!(next_node.mode & S_IFDIR)) {
                return -ENOTDIR;
            }
            curr = next;
        }
    }

    /* Last component */
    const char *final_comp = components[count - 1];
    if (out_parent) *out_parent = curr;
    if (last_name) {
        strncpy(last_name, final_comp, 57);
        last_name[57] = '\0';
    }

    if (out_ino) {
        if (strcmp(final_comp, ".") == 0) {
            *out_ino = curr;
            return 0;
        } else if (strcmp(final_comp, "..") == 0) {
            if (curr == fs->sb.root_inode) {
                *out_ino = fs->sb.root_inode;
                return 0;
            }
            return dir_lookup(fs, curr, "..", out_ino);
        } else {
            return dir_lookup(fs, curr, final_comp, out_ino);
        }
    }

    return 0;
}
