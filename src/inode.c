#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "inode.h"
#include "log.h"
#include <string.h>
#include <time.h>

void inode_touch(inode_t *ino, int atime, int mtime, int ctime) {
    if (!ino) return;
    uint32_t now = (uint32_t)time(NULL);
    if (atime) ino->atime = now;
    if (mtime) ino->mtime = now;
    if (ctime) ino->ctime = now;
}

int inode_read(fs_t *fs, uint32_t ino, inode_t *out) {
    if (!fs || !out || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    uint32_t blk = fs->sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);

    uint8_t buf[BLOCK_SIZE];
    int rc = fs_read_block(fs, blk, buf);
    if (rc != 0) return rc;

    memcpy(out, buf + off, sizeof(inode_t));
    return 0;
}

int inode_write(fs_t *fs, uint32_t ino, const inode_t *in) {
    if (!fs || !in || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    uint32_t blk = fs->sb.itable_start + ((ino - 1) / 32);
    size_t off = ((size_t)(ino - 1) % 32) * sizeof(inode_t);

    uint8_t buf[BLOCK_SIZE];
    int rc = fs_read_block(fs, blk, buf);
    if (rc != 0) return rc;

    memcpy(buf + off, in, sizeof(inode_t));
    return fs_write_block(fs, blk, buf);
}

int inode_create(fs_t *fs, uint16_t mode, uid_t uid, gid_t gid) {
    if (!fs) return -EINVAL;

    int res = alloc_inode(fs);
    if (res < 0) return res;

    uint32_t ino = (uint32_t)res;
    inode_t node;
    memset(&node, 0, sizeof(node));
    node.mode = mode;
    node.uid = (uint16_t)uid;
    node.gid = (uint16_t)gid;
    node.nlink = 1;
    inode_touch(&node, 1, 1, 1);

    int rc = inode_write(fs, ino, &node);
    if (rc != 0) {
        free_inode(fs, ino);
        return rc;
    }

    return (int)ino;
}

int bmap(fs_t *fs, inode_t *ino, uint32_t lbn, int alloc, uint32_t *pbn) {
    if (!fs || !ino || !pbn) return -EINVAL;
    if (lbn >= MAX_FILE_BLOCKS) return -EFBIG;

    uint32_t goal = fs->last_alloc_blk;

    /* 1. Direct blocks (0 .. 11) */
    if (lbn < DIRECT_BLOCKS) {
        if (ino->direct[lbn] == 0) {
            if (!alloc) {
                *pbn = 0;
                return 0;
            }
            int b = alloc_block(fs, goal);
            if (b < 0) return b;
            ino->direct[lbn] = (uint32_t)b;
            ino->blocks++;
            *pbn = (uint32_t)b;
            return 0;
        } else {
            *pbn = ino->direct[lbn];
            return 0;
        }
    }

    /* 2. Single indirect blocks (12 .. 1035) */
    if (lbn < DOUBLE_INDIRECT_START) {
        uint32_t idx = lbn - SINGLE_INDIRECT_START;
        if (ino->indirect == 0) {
            if (!alloc) {
                *pbn = 0;
                return 0;
            }
            int ib = alloc_block(fs, goal);
            if (ib < 0) return ib;
            ino->indirect = (uint32_t)ib;
            ino->blocks++;
        }

        uint32_t ptrs[PTRS_PER_BLOCK];
        int rc = fs_read_block(fs, ino->indirect, ptrs);
        if (rc != 0) return rc;

        if (ptrs[idx] == 0) {
            if (!alloc) {
                *pbn = 0;
                return 0;
            }
            uint32_t prev = (idx > 0 && ptrs[idx - 1]) ? ptrs[idx - 1] : ino->indirect;
            int b = alloc_block(fs, prev);
            if (b < 0) return b;
            ptrs[idx] = (uint32_t)b;
            ino->blocks++;
            rc = fs_write_block(fs, ino->indirect, ptrs);
            if (rc != 0) return rc;
            *pbn = (uint32_t)b;
            return 0;
        } else {
            *pbn = ptrs[idx];
            return 0;
        }
    }

    /* 3. Double indirect blocks (1036 .. 1049611) */
    uint32_t d_idx = lbn - DOUBLE_INDIRECT_START;
    uint32_t l1 = d_idx / PTRS_PER_BLOCK;
    uint32_t l2 = d_idx % PTRS_PER_BLOCK;

    if (ino->dindirect == 0) {
        if (!alloc) {
            *pbn = 0;
            return 0;
        }
        int dib = alloc_block(fs, goal);
        if (dib < 0) return dib;
        ino->dindirect = (uint32_t)dib;
        ino->blocks++;
    }

    uint32_t l1_ptrs[PTRS_PER_BLOCK];
    int rc = fs_read_block(fs, ino->dindirect, l1_ptrs);
    if (rc != 0) return rc;

    if (l1_ptrs[l1] == 0) {
        if (!alloc) {
            *pbn = 0;
            return 0;
        }
        uint32_t prev_l1 = (l1 > 0 && l1_ptrs[l1 - 1]) ? l1_ptrs[l1 - 1] : ino->dindirect;
        int l2_blk = alloc_block(fs, prev_l1);
        if (l2_blk < 0) return l2_blk;
        l1_ptrs[l1] = (uint32_t)l2_blk;
        ino->blocks++;
        rc = fs_write_block(fs, ino->dindirect, l1_ptrs);
        if (rc != 0) return rc;
    }

    uint32_t l2_ptrs[PTRS_PER_BLOCK];
    rc = fs_read_block(fs, l1_ptrs[l1], l2_ptrs);
    if (rc != 0) return rc;

    if (l2_ptrs[l2] == 0) {
        if (!alloc) {
            *pbn = 0;
            return 0;
        }
        uint32_t prev_l2 = (l2 > 0 && l2_ptrs[l2 - 1]) ? l2_ptrs[l2 - 1] : l1_ptrs[l1];
        int b = alloc_block(fs, prev_l2);
        if (b < 0) return b;
        l2_ptrs[l2] = (uint32_t)b;
        ino->blocks++;
        rc = fs_write_block(fs, l1_ptrs[l1], l2_ptrs);
        if (rc != 0) return rc;
        *pbn = (uint32_t)b;
        return 0;
    } else {
        *pbn = l2_ptrs[l2];
        return 0;
    }
}

int inode_free_blocks_from(fs_t *fs, inode_t *ino, uint32_t first_lbn) {
    if (!fs || !ino) return -EINVAL;

    /* 1. Direct blocks */
    if (first_lbn < DIRECT_BLOCKS) {
        for (uint32_t i = first_lbn; i < DIRECT_BLOCKS; i++) {
            if (ino->direct[i] != 0) {
                free_block(fs, ino->direct[i]);
                ino->direct[i] = 0;
                if (ino->blocks > 0) ino->blocks--;
            }
        }
    }

    /* 2. Indirect block */
    if (first_lbn < DOUBLE_INDIRECT_START && ino->indirect != 0) {
        uint32_t ptrs[PTRS_PER_BLOCK];
        int rc = fs_read_block(fs, ino->indirect, ptrs);
        if (rc == 0) {
            uint32_t start_idx = (first_lbn >= SINGLE_INDIRECT_START) ? (first_lbn - SINGLE_INDIRECT_START) : 0;
            int any_left = 0;
            for (uint32_t i = 0; i < start_idx; i++) {
                if (ptrs[i] != 0) any_left = 1;
            }
            for (uint32_t i = start_idx; i < PTRS_PER_BLOCK; i++) {
                if (ptrs[i] != 0) {
                    free_block(fs, ptrs[i]);
                    ptrs[i] = 0;
                    if (ino->blocks > 0) ino->blocks--;
                }
            }
            if (!any_left) {
                free_block(fs, ino->indirect);
                ino->indirect = 0;
                if (ino->blocks > 0) ino->blocks--;
            } else {
                fs_write_block(fs, ino->indirect, ptrs);
            }
        }
    }

    /* 3. Double indirect block */
    if (ino->dindirect != 0) {
        uint32_t l1_ptrs[PTRS_PER_BLOCK];
        int rc = fs_read_block(fs, ino->dindirect, l1_ptrs);
        if (rc == 0) {
            uint32_t d_idx = (first_lbn >= DOUBLE_INDIRECT_START) ? (first_lbn - DOUBLE_INDIRECT_START) : 0;
            uint32_t start_l1 = d_idx / PTRS_PER_BLOCK;
            uint32_t start_l2 = d_idx % PTRS_PER_BLOCK;
            int any_l1_left = 0;

            for (uint32_t l1 = 0; l1 < start_l1; l1++) {
                if (l1_ptrs[l1] != 0) any_l1_left = 1;
            }

            for (uint32_t l1 = start_l1; l1 < PTRS_PER_BLOCK; l1++) {
                if (l1_ptrs[l1] != 0) {
                    uint32_t l2_ptrs[PTRS_PER_BLOCK];
                    int r2 = fs_read_block(fs, l1_ptrs[l1], l2_ptrs);
                    if (r2 == 0) {
                        uint32_t cur_start_l2 = (l1 == start_l1) ? start_l2 : 0;
                        int any_l2_left = 0;
                        for (uint32_t l2 = 0; l2 < cur_start_l2; l2++) {
                            if (l2_ptrs[l2] != 0) any_l2_left = 1;
                        }
                        for (uint32_t l2 = cur_start_l2; l2 < PTRS_PER_BLOCK; l2++) {
                            if (l2_ptrs[l2] != 0) {
                                free_block(fs, l2_ptrs[l2]);
                                l2_ptrs[l2] = 0;
                                if (ino->blocks > 0) ino->blocks--;
                            }
                        }
                        if (!any_l2_left) {
                            free_block(fs, l1_ptrs[l1]);
                            l1_ptrs[l1] = 0;
                            if (ino->blocks > 0) ino->blocks--;
                        } else {
                            any_l1_left = 1;
                            fs_write_block(fs, l1_ptrs[l1], l2_ptrs);
                        }
                    }
                }
            }

            if (!any_l1_left) {
                free_block(fs, ino->dindirect);
                ino->dindirect = 0;
                if (ino->blocks > 0) ino->blocks--;
            } else {
                fs_write_block(fs, ino->dindirect, l1_ptrs);
            }
        }
    }

    return 0;
}

int inode_destroy(fs_t *fs, uint32_t ino) {
    if (!fs || ino < 1 || ino >= fs->sb.total_inodes) {
        return -EINVAL;
    }

    inode_t node;
    int rc = inode_read(fs, ino, &node);
    if (rc != 0) return rc;

    rc = inode_free_blocks_from(fs, &node, 0);
    if (rc != 0) return rc;

    memset(&node, 0, sizeof(node));
    rc = inode_write(fs, ino, &node);
    if (rc != 0) return rc;

    return free_inode(fs, ino);
}
