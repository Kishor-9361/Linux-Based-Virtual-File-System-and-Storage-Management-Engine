#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "blk.h"
#include "sb.h"
#include "common.h"
#include "bitmap.h"
#include "dir.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <image_path>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    blkdev_t dev;
    int rc = blk_open(&dev, path, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "Error: Cannot open '%s': %s (%d)\n", path, strerror(-rc), rc);
        return 1;
    }

    rc = sb_format(&dev);
    if (rc != 0) {
        fprintf(stderr, "Error: Formatting failed: %s (%d)\n", strerror(-rc), rc);
        blk_close(&dev);
        return 1;
    }

    fs_t fs;
    rc = fs_init_allocators(&fs, &dev);
    if (rc != 0) {
        fprintf(stderr, "Error: Allocator init failed: %s (%d)\n", strerror(-rc), rc);
        blk_close(&dev);
        return 1;
    }

    rc = dir_bootstrap_root(&fs);
    if (rc != 0) {
        fprintf(stderr, "Error: Root bootstrap failed: %s (%d)\n", strerror(-rc), rc);
        blk_close(&dev);
        return 1;
    }

    superblock_t sb;
    rc = sb_load(&dev, &sb);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to verify superblock after format: %s (%d)\n", strerror(-rc), rc);
        blk_close(&dev);
        return 1;
    }

    blk_close(&dev);

    printf("Successfully formatted minifs on '%s':\n", path);
    printf("  Total blocks:   %" PRIu32 " (%zu KiB)\n", sb.total_blocks, (size_t)sb.total_blocks * BLOCK_SIZE / 1024);
    printf("  Journal blocks: %" PRIu32 " (start at %" PRIu32 ")\n", sb.journal_blocks, sb.journal_start);
    printf("  Inode count:    %" PRIu32 " (%u blocks)\n", sb.total_inodes, sb.itable_blocks);
    printf("  Data start:     block %" PRIu32 "\n", sb.data_start);
    printf("  Free blocks:    %" PRIu32 "\n", sb.free_blocks);
    printf("  Free inodes:    %" PRIu32 "\n", sb.free_inodes);
    return 0;
}
