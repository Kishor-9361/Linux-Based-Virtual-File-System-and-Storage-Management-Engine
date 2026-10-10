#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "fsck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [--repair|-r] [--verbose|-v] <image_path>\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *img_path = NULL;
    fsck_options_t opts = { .repair = false, .verbose = false };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repair") == 0 || strcmp(argv[i], "-r") == 0) {
            opts.repair = true;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            opts.verbose = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            img_path = argv[i];
        }
    }

    if (!img_path) {
        fprintf(stderr, "Error: Image path required\n");
        print_usage(argv[0]);
        return 1;
    }

    blkdev_t dev;
    int rc = blk_open(&dev, img_path, 0, 0);
    if (rc != 0) {
        fprintf(stderr, "Error: Failed to open image '%s' (rc=%d)\n", img_path, rc);
        return 1;
    }

    printf("========================================================\n");
    printf("           minifs Consistency Checker (fsck)            \n");
    printf("========================================================\n");
    printf("Target Image: %s (%u blocks, %lu bytes)\n", img_path, dev.nblocks, (unsigned long)dev.nblocks * BLOCK_SIZE);
    printf("Mode:         %s\n", opts.repair ? "REPAIR (modifications enabled)" : "CHECK ONLY (read-only)");
    printf("--------------------------------------------------------\n");

    fsck_stats_t stats;
    rc = fsck_check(&dev, &opts, &stats);

    printf("==================== fsck Summary ======================\n");
    printf("Total Inconsistencies Found: %u\n", stats.errors_found);
    if (opts.repair) {
        printf("Total Inconsistencies Fixed: %u\n", stats.errors_fixed);
    }
    printf(" - Leaked Blocks:            %u\n", stats.leaked_blocks);
    printf(" - Missing Allocations:      %u\n", stats.missing_alloc_blocks);
    printf(" - Duplicate Blocks:         %u\n", stats.duplicate_blocks);
    printf(" - Bad Link Counts:          %u\n", stats.bad_link_counts);
    printf(" - Orphan Inodes:            %u\n", stats.orphan_inodes);
    printf(" - Bad Dentries:             %u\n", stats.bad_dentries);
    printf(" - Bad Block Pointers:       %u\n", stats.bad_pointers);
    printf(" - Superblock Errors:        %u\n", stats.sb_errors);
    printf("--------------------------------------------------------\n");

    blk_close(&dev);

    if (stats.errors_found == 0) {
        printf("RESULT: CLEAN (Filesystem is fully consistent)\n");
        return 0;
    }

    if (opts.repair) {
        if (stats.errors_found == stats.errors_fixed) {
            printf("RESULT: REPAIRED (All inconsistencies resolved)\n");
            return 0;
        } else {
            printf("RESULT: PARTIALLY REPAIRED (%u unresolved)\n", stats.errors_found - stats.errors_fixed);
            return 1;
        }
    } else {
        printf("RESULT: CORRUPTED (Inconsistencies detected; run with --repair)\n");
        return 1;
    }
}
