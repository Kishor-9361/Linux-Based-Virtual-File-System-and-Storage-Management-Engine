#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "fsck.h"
#include "bitmap.h"
#include "inode.h"
#include "dir.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef struct {
    blkdev_t *dev;
    superblock_t sb;
    const fsck_options_t *opts;
    fsck_stats_t *stats;

    uint8_t *ibitmap;     /* In-memory inode bitmap */
    uint32_t ibitmap_bytes;
    uint8_t *bbitmap;     /* In-memory block bitmap */
    uint32_t bbitmap_bytes;

    uint32_t *owner;      /* owner[blk] = inode claiming this block, 0 = none, UINT32_MAX = metadata */
    uint32_t *ref_count;  /* ref_count[ino] = directory references */
    bool *reachable;      /* reachable[ino] */
    bool *visited_dir;    /* visited_dir[ino] for cycle detection */
    uint32_t *parent_ino; /* parent directory */

    bool bbitmap_dirty;
    bool ibitmap_dirty;
    bool sb_dirty;
} fsck_ctx_t;

/* Helper to read an inode directly from disk */
static int read_inode_raw(fsck_ctx_t *ctx, uint32_t ino, inode_t *node) {
    if (ino == 0 || ino >= ctx->sb.total_inodes) return -EINVAL;
    uint32_t blk = ctx->sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    int rc = blk_read(ctx->dev, blk, buf);
    if (rc != 0) return rc;
    memcpy(node, buf + off, sizeof(inode_t));
    return 0;
}

/* Helper to write an inode directly to disk */
static int write_inode_raw(fsck_ctx_t *ctx, uint32_t ino, const inode_t *node) {
    if (!ctx->opts->repair) return 0;
    if (ino == 0 || ino >= ctx->sb.total_inodes) return -EINVAL;
    uint32_t blk = ctx->sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);
    uint8_t buf[BLOCK_SIZE];
    int rc = blk_read(ctx->dev, blk, buf);
    if (rc != 0) return rc;
    memcpy(buf + off, node, sizeof(inode_t));
    return blk_write(ctx->dev, blk, buf);
}

static inline bool ibitmap_test(fsck_ctx_t *ctx, uint32_t ino) {
    if (ino >= ctx->sb.total_inodes) return false;
    return (ctx->ibitmap[ino / 8] & (1 << (ino % 8))) != 0;
}

static inline void ibitmap_set(fsck_ctx_t *ctx, uint32_t ino) {
    if (ino >= ctx->sb.total_inodes) return;
    ctx->ibitmap[ino / 8] |= (uint8_t)(1 << (ino % 8));
    ctx->ibitmap_dirty = true;
}

static inline void ibitmap_clear(fsck_ctx_t *ctx, uint32_t ino) {
    if (ino >= ctx->sb.total_inodes) return;
    ctx->ibitmap[ino / 8] &= (uint8_t)~(1 << (ino % 8));
    ctx->ibitmap_dirty = true;
}

static inline bool bbitmap_test(fsck_ctx_t *ctx, uint32_t blk) {
    if (blk >= ctx->sb.total_blocks) return false;
    return (ctx->bbitmap[blk / 8] & (1 << (blk % 8))) != 0;
}

static inline void bbitmap_set(fsck_ctx_t *ctx, uint32_t blk) {
    if (blk >= ctx->sb.total_blocks) return;
    ctx->bbitmap[blk / 8] |= (uint8_t)(1 << (blk % 8));
    ctx->bbitmap_dirty = true;
}

static inline void bbitmap_clear(fsck_ctx_t *ctx, uint32_t blk) {
    if (blk >= ctx->sb.total_blocks) return;
    ctx->bbitmap[blk / 8] &= (uint8_t)~(1 << (blk % 8));
    ctx->bbitmap_dirty = true;
}

/* Find a free data block */
static int find_free_block(fsck_ctx_t *ctx) {
    for (uint32_t blk = ctx->sb.data_start; blk < ctx->sb.total_blocks; blk++) {
        if (!bbitmap_test(ctx, blk) && ctx->owner[blk] == 0) {
            return (int)blk;
        }
    }
    return -ENOSPC;
}

/* ========================================================================= */
/* Pass 1: Superblock Validation & Geometry                                   */
/* ========================================================================= */
static int pass1_check_superblock(fsck_ctx_t *ctx) {
    uint8_t sb_buf[BLOCK_SIZE];
    int rc = blk_read(ctx->dev, 0, sb_buf);
    if (rc != 0) return rc;

    superblock_t *sb = (superblock_t *)sb_buf;
    bool sb_corrupt = false;

    if (sb->magic != MINIFS_MAGIC) {
        ctx->stats->errors_found++;
        ctx->stats->sb_errors++;
        if (ctx->opts->verbose) printf("[Pass 1] Invalid magic: 0x%08X\n", sb->magic);
        sb_corrupt = true;
    }

    if (sb->version != MINIFS_VERSION) {
        ctx->stats->errors_found++;
        ctx->stats->sb_errors++;
        if (ctx->opts->verbose) printf("[Pass 1] Invalid version: %u\n", sb->version);
        sb_corrupt = true;
    }

    if (sb->block_size != BLOCK_SIZE) {
        ctx->stats->errors_found++;
        ctx->stats->sb_errors++;
        if (ctx->opts->verbose) printf("[Pass 1] Invalid block size: %u\n", sb->block_size);
        sb_corrupt = true;
    }

    uint32_t crc = crc32(sb, offsetof(superblock_t, crc32));
    if (crc != sb->crc32) {
        ctx->stats->errors_found++;
        ctx->stats->sb_errors++;
        if (ctx->opts->verbose) printf("[Pass 1] Superblock CRC mismatch: 0x%08X != 0x%08X\n", crc, sb->crc32);
        sb_corrupt = true;
    }

    if (sb_corrupt) {
        if (ctx->opts->repair) {
            /* Recompute layout from device block count */
            if (sb_compute_layout(ctx->dev->nblocks, &ctx->sb) == 0) {
                ctx->sb.magic = MINIFS_MAGIC;
                ctx->sb.version = MINIFS_VERSION;
                ctx->sb.block_size = BLOCK_SIZE;
                ctx->sb.state = SB_STATE_CLEAN;
                ctx->sb_dirty = true;
                ctx->stats->errors_fixed++;
                if (ctx->opts->verbose) printf("[Pass 1] Superblock geometry repaired\n");
            } else {
                return -EIO;
            }
        } else {
            return -EIO;
        }
    } else {
        memcpy(&ctx->sb, sb, sizeof(superblock_t));
    }

    return 0;
}

/* ========================================================================= */
/* Pass 2 & 3: Inode Tree Scan & Block Ownership Map                         */
/* ========================================================================= */
static int record_block_claim(fsck_ctx_t *ctx, uint32_t ino, uint32_t *p_blk, bool is_meta) {
    uint32_t blk = *p_blk;
    if (blk == 0) return 0;

    /* Range check */
    if (blk < ctx->sb.data_start || blk >= ctx->sb.total_blocks) {
        ctx->stats->errors_found++;
        ctx->stats->bad_pointers++;
        if (ctx->opts->verbose) {
            printf("[Pass 2] Inode %u has out-of-range block pointer %u\n", ino, blk);
        }
        if (ctx->opts->repair) {
            *p_blk = 0;
            ctx->stats->errors_fixed++;
        }
        return 0;
    }

    /* Duplicate check */
    if (ctx->owner[blk] != 0) {
        ctx->stats->errors_found++;
        ctx->stats->duplicate_blocks++;
        if (ctx->opts->verbose) {
            printf("[Pass 3] Duplicate block %u claimed by inode %u (already owned by %u)\n",
                   blk, ino, ctx->owner[blk]);
        }
        if (ctx->opts->repair) {
            int new_blk = find_free_block(ctx);
            if (new_blk > 0) {
                /* Clone block */
                uint8_t copy_buf[BLOCK_SIZE];
                blk_read(ctx->dev, blk, copy_buf);
                blk_write(ctx->dev, (uint32_t)new_blk, copy_buf);
                bbitmap_set(ctx, (uint32_t)new_blk);
                *p_blk = (uint32_t)new_blk;
                ctx->owner[new_blk] = ino;
                ctx->stats->errors_fixed++;
                if (ctx->opts->verbose) {
                    printf("[Pass 3] Cloned duplicate block %u -> %u for inode %u\n", blk, new_blk, ino);
                }
            } else {
                /* ENOSPC: zero pointer */
                *p_blk = 0;
                ctx->stats->errors_fixed++;
            }
        }
        return 0;
    }

    ctx->owner[blk] = (is_meta) ? UINT32_MAX : ino;
    return 0;
}

static int pass2_and_3_scan_inodes(fsck_ctx_t *ctx) {
    for (uint32_t ino = 1; ino < ctx->sb.total_inodes; ino++) {
        if (!ibitmap_test(ctx, ino)) continue;

        inode_t node;
        int rc = read_inode_raw(ctx, ino, &node);
        if (rc != 0) continue;

        uint16_t type = (uint16_t)(node.mode & S_IFMT);
        if (type != S_IFREG && type != S_IFDIR) {
            ctx->stats->errors_found++;
            if (ctx->opts->verbose) {
                printf("[Pass 2] Inode %u has invalid mode/type 0x%04X\n", ino, node.mode);
            }
            if (ctx->opts->repair) {
                ibitmap_clear(ctx, ino);
                ctx->stats->errors_fixed++;
            }
            continue;
        }

        bool inode_modified = false;

        /* 1. Direct blocks */
        for (uint32_t i = 0; i < DIRECT_BLOCKS; i++) {
            uint32_t ptr_val = node.direct[i];
            uint32_t prev = ptr_val;
            record_block_claim(ctx, ino, &ptr_val, false);
            if (ptr_val != prev) {
                node.direct[i] = ptr_val;
                inode_modified = true;
            }
        }

        /* 2. Single indirect */
        if (node.indirect != 0) {
            uint32_t ind_val = node.indirect;
            uint32_t prev_ind = ind_val;
            record_block_claim(ctx, ino, &ind_val, true);
            if (ind_val != prev_ind) {
                node.indirect = ind_val;
                inode_modified = true;
            } else if (node.indirect >= ctx->sb.data_start && node.indirect < ctx->sb.total_blocks) {
                uint8_t ind_buf[BLOCK_SIZE];
                if (blk_read(ctx->dev, node.indirect, ind_buf) == 0) {
                    uint32_t *ptrs = (uint32_t *)ind_buf;
                    bool ind_dirty = false;
                    for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++) {
                        uint32_t prev = ptrs[j];
                        record_block_claim(ctx, ino, &ptrs[j], false);
                        if (ptrs[j] != prev) ind_dirty = true;
                    }
                    if (ind_dirty && ctx->opts->repair) {
                        blk_write(ctx->dev, node.indirect, ind_buf);
                    }
                }
            }
        }

        /* 3. Double indirect */
        if (node.dindirect != 0) {
            uint32_t dind_val = node.dindirect;
            uint32_t prev_dind = dind_val;
            record_block_claim(ctx, ino, &dind_val, true);
            if (dind_val != prev_dind) {
                node.dindirect = dind_val;
                inode_modified = true;
            } else if (node.dindirect >= ctx->sb.data_start && node.dindirect < ctx->sb.total_blocks) {
                uint8_t l1_buf[BLOCK_SIZE];
                if (blk_read(ctx->dev, node.dindirect, l1_buf) == 0) {
                    uint32_t *l1_ptrs = (uint32_t *)l1_buf;
                    bool l1_dirty = false;
                    for (uint32_t p1 = 0; p1 < PTRS_PER_BLOCK; p1++) {
                        if (l1_ptrs[p1] == 0) continue;
                        uint32_t prev_l1 = l1_ptrs[p1];
                        record_block_claim(ctx, ino, &l1_ptrs[p1], true);
                        if (l1_ptrs[p1] != prev_l1) {
                            l1_dirty = true;
                        } else if (l1_ptrs[p1] >= ctx->sb.data_start && l1_ptrs[p1] < ctx->sb.total_blocks) {
                            uint8_t l2_buf[BLOCK_SIZE];
                            if (blk_read(ctx->dev, l1_ptrs[p1], l2_buf) == 0) {
                                uint32_t *l2_ptrs = (uint32_t *)l2_buf;
                                bool l2_dirty = false;
                                for (uint32_t p2 = 0; p2 < PTRS_PER_BLOCK; p2++) {
                                    uint32_t prev_l2 = l2_ptrs[p2];
                                    record_block_claim(ctx, ino, &l2_ptrs[p2], false);
                                    if (l2_ptrs[p2] != prev_l2) l2_dirty = true;
                                }
                                if (l2_dirty && ctx->opts->repair) {
                                    blk_write(ctx->dev, l1_ptrs[p1], l2_buf);
                                }
                            }
                        }
                    }
                    if (l1_dirty && ctx->opts->repair) {
                        blk_write(ctx->dev, node.dindirect, l1_buf);
                    }
                }
            }
        }

        if (inode_modified && ctx->opts->repair) {
            write_inode_raw(ctx, ino, &node);
        }
    }
    return 0;
}

/* ========================================================================= */
/* Pass 4: Directory Tree Walk & Namespace Verification                      */
/* ========================================================================= */
static int walk_directory(fsck_ctx_t *ctx, uint32_t dir_ino, uint32_t parent) {
    if (dir_ino >= ctx->sb.total_inodes) return -EINVAL;

    if (ctx->visited_dir[dir_ino]) {
        ctx->stats->errors_found++;
        if (ctx->opts->verbose) {
            printf("[Pass 4] Directory loop detected involving inode %u\n", dir_ino);
        }
        return -ELOOP;
    }

    ctx->visited_dir[dir_ino] = true;
    ctx->reachable[dir_ino] = true;
    ctx->parent_ino[dir_ino] = parent;

    inode_t node;
    int rc = read_inode_raw(ctx, dir_ino, &node);
    if (rc != 0) return rc;

    bool has_dot = false;
    bool has_dotdot = false;

    /* Scan directory blocks */
    uint32_t num_blocks = (uint32_t)((node.size + BLOCK_SIZE - 1) / BLOCK_SIZE);
    if (num_blocks > DIRECT_BLOCKS) num_blocks = DIRECT_BLOCKS;

    for (uint32_t bi = 0; bi < num_blocks; bi++) {
        uint32_t blk = node.direct[bi];
        if (blk == 0) continue;

        uint8_t blk_buf[BLOCK_SIZE];
        if (blk_read(ctx->dev, blk, blk_buf) != 0) continue;

        dirent_t *entries = (dirent_t *)blk_buf;
        uint32_t n_entries = BLOCK_SIZE / sizeof(dirent_t);
        bool blk_dirty = false;

        for (uint32_t ei = 0; ei < n_entries; ei++) {
            dirent_t *de = &entries[ei];
            if (de->ino == 0) continue;

            if (strcmp(de->name, ".") == 0) {
                has_dot = true;
                if (de->ino != dir_ino) {
                    ctx->stats->errors_found++;
                    if (ctx->opts->verbose) printf("[Pass 4] Dir %u has '.' pointing to %u\n", dir_ino, de->ino);
                    if (ctx->opts->repair) {
                        de->ino = dir_ino;
                        blk_dirty = true;
                        ctx->stats->errors_fixed++;
                    }
                }
                ctx->ref_count[dir_ino]++;
                continue;
            }

            if (strcmp(de->name, "..") == 0) {
                has_dotdot = true;
                if (de->ino != parent) {
                    ctx->stats->errors_found++;
                    if (ctx->opts->verbose) printf("[Pass 4] Dir %u has '..' pointing to %u (expected %u)\n", dir_ino, de->ino, parent);
                    if (ctx->opts->repair) {
                        de->ino = parent;
                        blk_dirty = true;
                        ctx->stats->errors_fixed++;
                    }
                }
                ctx->ref_count[parent]++;
                continue;
            }

            /* Entry points to target inode */
            uint32_t target_ino = de->ino;
            if (target_ino >= ctx->sb.total_inodes || !ibitmap_test(ctx, target_ino)) {
                ctx->stats->errors_found++;
                ctx->stats->bad_dentries++;
                if (ctx->opts->verbose) {
                    printf("[Pass 4] Dentry '%s' in dir %u points to invalid/free inode %u\n",
                           de->name, dir_ino, target_ino);
                }
                if (ctx->opts->repair) {
                    memset(de, 0, sizeof(dirent_t));
                    blk_dirty = true;
                    ctx->stats->errors_fixed++;
                }
                continue;
            }

            ctx->ref_count[target_ino]++;
            ctx->reachable[target_ino] = true;

            inode_t child_node;
            if (read_inode_raw(ctx, target_ino, &child_node) == 0) {
                if ((child_node.mode & S_IFMT) == S_IFDIR) {
                    int w_rc = walk_directory(ctx, target_ino, dir_ino);
                    if (w_rc == -ELOOP && ctx->opts->repair) {
                        memset(de, 0, sizeof(dirent_t));
                        blk_dirty = true;
                        ctx->ref_count[target_ino]--;
                        ctx->stats->errors_fixed++;
                    }
                }
            }
        }

        if (blk_dirty && ctx->opts->repair) {
            blk_write(ctx->dev, blk, blk_buf);
        }
    }

    /* Verify presence of '.' and '..' */
    if (!has_dot) {
        ctx->stats->errors_found++;
        if (ctx->opts->verbose) printf("[Pass 4] Dir %u missing '.' entry\n", dir_ino);
        if (ctx->opts->repair && node.direct[0] != 0) {
            uint8_t blk_buf[BLOCK_SIZE];
            if (blk_read(ctx->dev, node.direct[0], blk_buf) == 0) {
                dirent_t *de = (dirent_t *)blk_buf;
                de[0].ino = dir_ino;
                de[0].type = MINIFS_FT_DIR;
                de[0].name_len = 1;
                strcpy(de[0].name, ".");
                blk_write(ctx->dev, node.direct[0], blk_buf);
                ctx->ref_count[dir_ino]++;
                ctx->stats->errors_fixed++;
            }
        }
    }

    if (!has_dotdot) {
        ctx->stats->errors_found++;
        if (ctx->opts->verbose) printf("[Pass 4] Dir %u missing '..' entry\n", dir_ino);
        if (ctx->opts->repair && node.direct[0] != 0) {
            uint8_t blk_buf[BLOCK_SIZE];
            if (blk_read(ctx->dev, node.direct[0], blk_buf) == 0) {
                dirent_t *de = (dirent_t *)blk_buf;
                de[1].ino = parent;
                de[1].type = MINIFS_FT_DIR;
                de[1].name_len = 2;
                strcpy(de[1].name, "..");
                blk_write(ctx->dev, node.direct[0], blk_buf);
                ctx->ref_count[parent]++;
                ctx->stats->errors_fixed++;
            }
        }
    }

    return 0;
}

static int pass4_check_namespace(fsck_ctx_t *ctx) {
    uint32_t root_ino = ctx->sb.root_inode;
    if (root_ino == 0 || root_ino >= ctx->sb.total_inodes || !ibitmap_test(ctx, root_ino)) {
        ctx->stats->errors_found++;
        if (ctx->opts->verbose) printf("[Pass 4] Root inode %u is not valid!\n", root_ino);
        return -EINVAL;
    }

    return walk_directory(ctx, root_ino, root_ino);
}

/* ========================================================================= */
/* Pass 5: Cross-Checks & Free Space Reconciliation                          */
/* ========================================================================= */
static int pass5_cross_checks(fsck_ctx_t *ctx) {
    /* 1. Block bitmap reconciliation */
    for (uint32_t blk = ctx->sb.data_start; blk < ctx->sb.total_blocks; blk++) {
        bool bit_set = bbitmap_test(ctx, blk);
        bool is_owned = (ctx->owner[blk] != 0);

        if (bit_set && !is_owned) {
            /* Leaked block */
            ctx->stats->errors_found++;
            ctx->stats->leaked_blocks++;
            if (ctx->opts->verbose) {
                printf("[Pass 5] Leaked block %u (bit set, but block unreferenced)\n", blk);
            }
            if (ctx->opts->repair) {
                bbitmap_clear(ctx, blk);
                ctx->stats->errors_fixed++;
            }
        } else if (!bit_set && is_owned) {
            /* Missing allocation */
            ctx->stats->errors_found++;
            ctx->stats->missing_alloc_blocks++;
            if (ctx->opts->verbose) {
                printf("[Pass 5] Block %u claimed by inode %u but bitmap bit is clear\n",
                       blk, ctx->owner[blk]);
            }
            if (ctx->opts->repair) {
                bbitmap_set(ctx, blk);
                ctx->stats->errors_fixed++;
            }
        }
    }

    /* 2. Inode bitmap & orphan reconciliation */
    for (uint32_t ino = 1; ino < ctx->sb.total_inodes; ino++) {
        if (!ibitmap_test(ctx, ino)) continue;

        if (!ctx->reachable[ino]) {
            /* Orphan inode */
            ctx->stats->errors_found++;
            ctx->stats->orphan_inodes++;
            if (ctx->opts->verbose) {
                printf("[Pass 5] Orphan inode %u found\n", ino);
            }

            if (ctx->opts->repair) {
                inode_t node;
                read_inode_raw(ctx, ino, &node);

                if (node.nlink == 0 || node.size == 0) {
                    /* Free orphan */
                    ibitmap_clear(ctx, ino);
                    memset(&node, 0, sizeof(inode_t));
                    write_inode_raw(ctx, ino, &node);
                    ctx->stats->errors_fixed++;
                } else {
                    /* Attach to root directory */
                    inode_t root_node;
                    read_inode_raw(ctx, ctx->sb.root_inode, &root_node);
                    if (root_node.direct[0] != 0) {
                        uint8_t blk_buf[BLOCK_SIZE];
                        if (blk_read(ctx->dev, root_node.direct[0], blk_buf) == 0) {
                            dirent_t *de = (dirent_t *)blk_buf;
                            uint32_t max_e = BLOCK_SIZE / sizeof(dirent_t);
                            for (uint32_t e = 2; e < max_e; e++) {
                                if (de[e].ino == 0) {
                                    de[e].ino = ino;
                                    de[e].type = ((node.mode & S_IFMT) == S_IFDIR) ? MINIFS_FT_DIR : MINIFS_FT_REG;
                                    snprintf(de[e].name, sizeof(de[e].name), "#%u", ino);
                                    de[e].name_len = (uint8_t)strlen(de[e].name);
                                    blk_write(ctx->dev, root_node.direct[0], blk_buf);
                                    ctx->ref_count[ino]++;
                                    ctx->reachable[ino] = true;
                                    ctx->stats->errors_fixed++;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* 3. Link count reconciliation */
    for (uint32_t ino = 1; ino < ctx->sb.total_inodes; ino++) {
        if (!ibitmap_test(ctx, ino)) continue;

        inode_t node;
        read_inode_raw(ctx, ino, &node);

        if (node.nlink != ctx->ref_count[ino]) {
            ctx->stats->errors_found++;
            ctx->stats->bad_link_counts++;
            if (ctx->opts->verbose) {
                printf("[Pass 5] Inode %u link count mismatch: on-disk %u != counted %u\n",
                       ino, node.nlink, ctx->ref_count[ino]);
            }
            if (ctx->opts->repair) {
                node.nlink = (uint16_t)ctx->ref_count[ino];
                write_inode_raw(ctx, ino, &node);
                ctx->stats->errors_fixed++;
            }
        }
    }

    /* 4. Superblock free counts */
    uint32_t free_inodes = 0;
    for (uint32_t ino = 1; ino < ctx->sb.total_inodes; ino++) {
        if (!ibitmap_test(ctx, ino)) free_inodes++;
    }

    uint32_t free_blocks = 0;
    for (uint32_t blk = ctx->sb.data_start; blk < ctx->sb.total_blocks; blk++) {
        if (!bbitmap_test(ctx, blk)) free_blocks++;
    }

    if (ctx->sb.free_inodes != free_inodes || ctx->sb.free_blocks != free_blocks) {
        ctx->stats->errors_found++;
        ctx->stats->sb_errors++;
        if (ctx->opts->verbose) {
            printf("[Pass 5] Superblock free count mismatch: free_inodes (%u vs %u), free_blocks (%u vs %u)\n",
                   ctx->sb.free_inodes, free_inodes, ctx->sb.free_blocks, free_blocks);
        }
        if (ctx->opts->repair) {
            ctx->sb.free_inodes = free_inodes;
            ctx->sb.free_blocks = free_blocks;
            ctx->sb.state = SB_STATE_CLEAN;
            ctx->sb_dirty = true;
            ctx->stats->errors_fixed++;
        }
    }

    return 0;
}

/* Write out dirty bitmaps and superblock if in repair mode */
static int flush_repairs(fsck_ctx_t *ctx) {
    if (!ctx->opts->repair) return 0;

    if (ctx->ibitmap_dirty) {
        uint32_t blks = ctx->sb.ibitmap_blocks;
        for (uint32_t i = 0; i < blks; i++) {
            blk_write(ctx->dev, ctx->sb.ibitmap_start + i, ctx->ibitmap + i * BLOCK_SIZE);
        }
        ctx->ibitmap_dirty = false;
    }

    if (ctx->bbitmap_dirty) {
        uint32_t blks = ctx->sb.bbitmap_blocks;
        for (uint32_t i = 0; i < blks; i++) {
            blk_write(ctx->dev, ctx->sb.bbitmap_start + i, ctx->bbitmap + i * BLOCK_SIZE);
        }
        ctx->bbitmap_dirty = false;
    }

    if (ctx->sb_dirty) {
        ctx->sb.crc32 = crc32(&ctx->sb, offsetof(superblock_t, crc32));
        uint8_t sb_buf[BLOCK_SIZE];
        memset(sb_buf, 0, BLOCK_SIZE);
        memcpy(sb_buf, &ctx->sb, sizeof(superblock_t));
        blk_write(ctx->dev, 0, sb_buf);
        ctx->sb_dirty = false;
    }

    return blk_sync(ctx->dev);
}

/* ========================================================================= */
/* Main Entry Point: fsck_check                                              */
/* ========================================================================= */
int fsck_check(blkdev_t *dev, const fsck_options_t *opts, fsck_stats_t *stats) {
    if (!dev || !opts || !stats) return -EINVAL;
    memset(stats, 0, sizeof(*stats));

    fsck_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dev = dev;
    ctx.opts = opts;
    ctx.stats = stats;

    /* Pass 1: Superblock check */
    int rc = pass1_check_superblock(&ctx);
    if (rc != 0) {
        return rc;
    }

    /* Allocate working tracking tables */
    ctx.ibitmap_bytes = ctx.sb.ibitmap_blocks * BLOCK_SIZE;
    ctx.ibitmap = (uint8_t *)malloc(ctx.ibitmap_bytes);
    if (!ctx.ibitmap) return -ENOMEM;

    for (uint32_t i = 0; i < ctx.sb.ibitmap_blocks; i++) {
        blk_read(dev, ctx.sb.ibitmap_start + i, ctx.ibitmap + i * BLOCK_SIZE);
    }

    ctx.bbitmap_bytes = ctx.sb.bbitmap_blocks * BLOCK_SIZE;
    ctx.bbitmap = (uint8_t *)malloc(ctx.bbitmap_bytes);
    if (!ctx.bbitmap) {
        free(ctx.ibitmap);
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < ctx.sb.bbitmap_blocks; i++) {
        blk_read(dev, ctx.sb.bbitmap_start + i, ctx.bbitmap + i * BLOCK_SIZE);
    }

    ctx.owner = (uint32_t *)calloc(ctx.sb.total_blocks, sizeof(uint32_t));
    ctx.ref_count = (uint32_t *)calloc(ctx.sb.total_inodes, sizeof(uint32_t));
    ctx.reachable = (bool *)calloc(ctx.sb.total_inodes, sizeof(bool));
    ctx.visited_dir = (bool *)calloc(ctx.sb.total_inodes, sizeof(bool));
    ctx.parent_ino = (uint32_t *)calloc(ctx.sb.total_inodes, sizeof(uint32_t));

    if (!ctx.owner || !ctx.ref_count || !ctx.reachable || !ctx.visited_dir || !ctx.parent_ino) {
        free(ctx.ibitmap); free(ctx.bbitmap);
        free(ctx.owner); free(ctx.ref_count); free(ctx.reachable); free(ctx.visited_dir); free(ctx.parent_ino);
        return -ENOMEM;
    }

    /* Mark metadata blocks as reserved in owner map */
    for (uint32_t b = 0; b < ctx.sb.data_start; b++) {
        ctx.owner[b] = UINT32_MAX;
    }

    /* Pass 2 & 3: Inode scan & block ownership */
    pass2_and_3_scan_inodes(&ctx);

    /* Pass 4: Directory tree DFS */
    pass4_check_namespace(&ctx);

    /* Pass 5: Cross-checks & reconciliations */
    pass5_cross_checks(&ctx);

    /* Commit repairs to disk if enabled */
    if (opts->repair) {
        flush_repairs(&ctx);
    }

    free(ctx.ibitmap);
    free(ctx.bbitmap);
    free(ctx.owner);
    free(ctx.ref_count);
    free(ctx.reachable);
    free(ctx.visited_dir);
    free(ctx.parent_ino);

    return (stats->errors_found > 0 && !opts->repair) ? 1 : 0;
}
