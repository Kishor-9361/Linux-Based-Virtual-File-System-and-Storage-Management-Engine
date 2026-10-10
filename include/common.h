#ifndef MINIFS_COMMON_H
#define MINIFS_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>

#define BLOCK_SIZE      4096U
#define INODE_SIZE      128U
#define DIRENT_SIZE     64U
#define MINIFS_MAGIC    0x4D494E49U /* "MINI" in ASCII, little-endian */
#define MINIFS_VERSION  1U

#define MAX_PATH_LEN    4096U
#define MAX_NAME_LEN    57U
#define MAX_DIR_DEPTH   40U

/* File types in dirent */
#define MINIFS_FT_UNKNOWN  0
#define MINIFS_FT_REG      1
#define MINIFS_FT_DIR      2

/* Superblock states */
#define SB_STATE_CLEAN   0U
#define SB_STATE_DIRTY   1U

#endif /* MINIFS_COMMON_H */
