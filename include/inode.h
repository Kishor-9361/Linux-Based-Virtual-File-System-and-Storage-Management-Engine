#ifndef MINIFS_INODE_H
#define MINIFS_INODE_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "common.h"
#include "fs.h"
#include "bitmap.h"

#define DIRECT_BLOCKS   12U
#define PTRS_PER_BLOCK  (BLOCK_SIZE / (uint32_t)sizeof(uint32_t)) /* 1024 */
#define SINGLE_INDIRECT_START DIRECT_BLOCKS
#define DOUBLE_INDIRECT_START (DIRECT_BLOCKS + PTRS_PER_BLOCK) /* 1036 */
#define MAX_FILE_BLOCKS       (DOUBLE_INDIRECT_START + (PTRS_PER_BLOCK * PTRS_PER_BLOCK)) /* 1049612 */

typedef struct __attribute__((packed)) {
    uint16_t mode;              /* S_IFREG, S_IFDIR, and rwxrwxrwx */
    uint16_t uid;
    uint16_t gid;
    uint16_t nlink;
    uint64_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t blocks;            /* Count of 4KB blocks allocated */
    uint32_t direct[DIRECT_BLOCKS];
    uint32_t indirect;          /* Points to block of 1024 pointers */
    uint32_t dindirect;         /* Points to block of 1024 indirect block pointers */
    uint32_t flags;
    uint8_t  pad[36];
} inode_t;

_Static_assert(sizeof(inode_t) == 128, "inode_t must be exactly 128 bytes");

int  inode_read(fs_t *fs, uint32_t ino, inode_t *out);
int  inode_write(fs_t *fs, uint32_t ino, const inode_t *in);
int  inode_create(fs_t *fs, uint16_t mode, uid_t uid, gid_t gid);
int  inode_destroy(fs_t *fs, uint32_t ino);
int  bmap(fs_t *fs, inode_t *ino, uint32_t lbn, int alloc, uint32_t *pbn);
int  inode_free_blocks_from(fs_t *fs, inode_t *ino, uint32_t first_lbn);
void inode_touch(inode_t *ino, int atime, int mtime, int ctime);

#endif /* MINIFS_INODE_H */
