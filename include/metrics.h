#ifndef MINIFS_METRICS_H
#define MINIFS_METRICS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include <stdio.h>
#include "common.h"
#include "fs.h"
#include "blk.h"
#include "bitmap.h"
#include "inode.h"
#include "cache.h"
#include "journal.h"

#define METRICS_MAX_SAMPLES 10000

typedef enum {
    OP_READ = 0,
    OP_WRITE,
    OP_LOOKUP,
    OP_CREATE,
    OP_UNLINK,
    OP_MKDIR,
    OP_RMDIR,
    OP_RENAME,
    OP_SYNC,
    OP_TRUNCATE,
    OP_COUNT
} metrics_op_t;

typedef struct {
    uint64_t count;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint64_t samples[METRICS_MAX_SAMPLES];
    uint32_t sample_count;
} metrics_latency_t;

typedef struct {
    /* Per-operation latency trackers */
    metrics_latency_t ops[OP_COUNT];

    /* Logical I/O counters */
    uint64_t logical_bytes_read;
    uint64_t logical_bytes_written;
    uint64_t logical_reads;
    uint64_t logical_writes;

    /* Directory counters */
    uint64_t dir_lookups;
    uint64_t dir_entries_scanned;
} metrics_live_t;

typedef struct {
    double block_utilization;      /* (total_blocks - free_blocks) / total_blocks */
    double inode_utilization;      /* (total_inodes - free_inodes) / total_inodes */
    double internal_frag;          /* (allocated_data_bytes - sum(file_size)) / allocated_data_bytes */
    double external_frag;          /* 1 - (largest_free_run / total_free_blocks) */
    double avg_file_frag;          /* Avg discontinuities per fragmented file */
    double pct_files_fragmented;   /* % of multi-block files fragmented */
    double alloc_efficiency;       /* % of allocations adjacent to goal */
    double avg_bits_scanned;       /* avg bitmap bits scanned per alloc */
    double write_amplification;    /* physical_bytes_written / logical_bytes_written */
    double cache_hit_ratio;        /* hits / (hits + misses) */
    uint32_t total_files;
    uint32_t fragmented_files;
    uint32_t multi_block_files;
    uint64_t largest_free_run;
    uint64_t total_free_blocks;
} metrics_storage_health_t;

typedef struct {
    uint64_t count;
    uint64_t min_ns;
    double   avg_ns;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t max_ns;
} metrics_percentiles_t;

/* Core Metrics API */
void metrics_init(void);
void metrics_reset(void);

uint64_t metrics_now_ns(void);
void metrics_record_op(metrics_op_t op, uint64_t latency_ns);
void metrics_record_logical_io(size_t bytes, bool is_write);
void metrics_record_dir_scan(uint32_t entries);

metrics_percentiles_t metrics_calc_percentiles(metrics_op_t op);
metrics_live_t metrics_get_live(void);

int metrics_compute_storage_health(fs_t *fs, metrics_storage_health_t *health);
void metrics_print_report(FILE *out, fs_t *fs);
void metrics_dump_csv(FILE *out, fs_t *fs);

const char *metrics_op_name(metrics_op_t op);

#endif /* MINIFS_METRICS_H */
