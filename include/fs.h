#ifndef MINIFS_FS_H
#define MINIFS_FS_H

#include <stdint.h>
#include "common.h"
#include "blk.h"
#include "sb.h"

/* Forward declarations */
typedef struct fs fs_t;

int fs_read_block(fs_t *fs, uint32_t blockno, void *buf);
int fs_write_block(fs_t *fs, uint32_t blockno, const void *buf);

#endif /* MINIFS_FS_H */
