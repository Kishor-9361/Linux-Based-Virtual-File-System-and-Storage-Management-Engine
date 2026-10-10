#ifndef MINIFS_FILEIO_H
#define MINIFS_FILEIO_H

#include <stdint.h>
#include <sys/types.h>
#include "common.h"
#include "fs.h"
#include "inode.h"

ssize_t file_read(fs_t *fs, uint32_t ino, uint64_t off, void *buf, size_t len);
ssize_t file_write(fs_t *fs, uint32_t ino, uint64_t off, const void *buf, size_t len);
int     file_truncate(fs_t *fs, uint32_t ino, uint64_t newsize);

#endif /* MINIFS_FILEIO_H */
