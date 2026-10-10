#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "blk.h"
#include "common.h"

static uint64_t parse_size(const char *str) {
    char *end = NULL;
    uint64_t val = (uint64_t)strtoull(str, &end, 10);
    if (!end || *end == '\0') {
        /* Assume blocks if small, bytes if multiple of 4096 */
        return val;
    }
    if (*end == 'K' || *end == 'k') return val * 1024ULL;
    if (*end == 'M' || *end == 'm') return val * 1024ULL * 1024ULL;
    if (*end == 'G' || *end == 'g') return val * 1024ULL * 1024ULL * 1024ULL;
    return val;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image_path> <size (e.g. 1M, 16M, 1G or num_blocks)>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    uint64_t parsed = parse_size(argv[2]);
    uint32_t nblocks = 0;

    if (parsed % BLOCK_SIZE == 0 && parsed >= BLOCK_SIZE) {
        nblocks = (uint32_t)(parsed / BLOCK_SIZE);
    } else {
        /* User specified raw block count */
        nblocks = (uint32_t)parsed;
    }

    if (nblocks == 0) {
        fprintf(stderr, "Error: Invalid block count (must be > 0)\n");
        return 1;
    }

    blkdev_t dev;
    int rc = blk_open(&dev, path, 1, nblocks);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to create disk image '%s': %s (%d)\n", path, strerror(-rc), rc);
        return 1;
    }

    blk_close(&dev);
    printf("Successfully created disk image '%s' with %" PRIu32 " blocks (%" PRIu64 " bytes)\n",
           path, nblocks, (uint64_t)nblocks * BLOCK_SIZE);
    return 0;
}
