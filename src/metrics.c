#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "metrics.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static metrics_live_t g_metrics;

static const char *g_op_names[OP_COUNT] = {
    "READ",
    "WRITE",
    "LOOKUP",
    "CREATE",
    "UNLINK",
    "MKDIR",
    "RMDIR",
    "RENAME",
    "SYNC",
    "TRUNCATE"
};

const char *metrics_op_name(metrics_op_t op) {
    if (op < OP_COUNT) {
        return g_op_names[op];
    }
    return "UNKNOWN";
}

void metrics_init(void) {
    memset(&g_metrics, 0, sizeof(g_metrics));
    for (int i = 0; i < OP_COUNT; i++) {
        g_metrics.ops[i].min_ns = (uint64_t)-1;
    }
}

void metrics_reset(void) {
    metrics_init();
}

uint64_t metrics_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void metrics_record_op(metrics_op_t op, uint64_t latency_ns) {
    if (op >= OP_COUNT) return;
    metrics_latency_t *lat = &g_metrics.ops[op];
    lat->count++;
    lat->total_ns += latency_ns;
    if (latency_ns < lat->min_ns) lat->min_ns = latency_ns;
    if (latency_ns > lat->max_ns) lat->max_ns = latency_ns;

    if (lat->sample_count < METRICS_MAX_SAMPLES) {
        lat->samples[lat->sample_count++] = latency_ns;
    } else {
        /* Reservoir sampling */
        uint64_t idx = (uint64_t)rand() % lat->count;
        if (idx < METRICS_MAX_SAMPLES) {
            lat->samples[idx] = latency_ns;
        }
    }
}

void metrics_record_logical_io(size_t bytes, bool is_write) {
    if (is_write) {
        g_metrics.logical_writes++;
        g_metrics.logical_bytes_written += (uint64_t)bytes;
    } else {
        g_metrics.logical_reads++;
        g_metrics.logical_bytes_read += (uint64_t)bytes;
    }
}

void metrics_record_dir_scan(uint32_t entries) {
    g_metrics.dir_lookups++;
    g_metrics.dir_entries_scanned += entries;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

metrics_percentiles_t metrics_calc_percentiles(metrics_op_t op) {
    metrics_percentiles_t res = {0, 0, 0.0, 0, 0, 0};
    if (op >= OP_COUNT) return res;
    metrics_latency_t *lat = &g_metrics.ops[op];
    if (lat->count == 0 || lat->sample_count == 0) return res;

    res.count = lat->count;
    res.min_ns = lat->min_ns;
    res.max_ns = lat->max_ns;
    res.avg_ns = (double)lat->total_ns / (double)lat->count;

    uint64_t *sorted = (uint64_t *)malloc(lat->sample_count * sizeof(uint64_t));
    if (!sorted) return res;
    memcpy(sorted, lat->samples, lat->sample_count * sizeof(uint64_t));
    qsort(sorted, lat->sample_count, sizeof(uint64_t), compare_u64);

    size_t idx50 = (size_t)(lat->sample_count * 50 / 100);
    size_t idx99 = (size_t)(lat->sample_count * 99 / 100);
    if (idx99 >= lat->sample_count) idx99 = lat->sample_count - 1;

    res.p50_ns = sorted[idx50];
    res.p99_ns = sorted[idx99];

    free(sorted);
    return res;
}

metrics_live_t metrics_get_live(void) {
    return g_metrics;
}

int metrics_compute_storage_health(fs_t *fs, metrics_storage_health_t *health) {
    if (!fs || !health) return -EINVAL;
    memset(health, 0, sizeof(*health));

    /* 1. Basic utilization */
    if (fs->sb.total_blocks > 0) {
        health->block_utilization = (double)(fs->sb.total_blocks - fs->sb.free_blocks) /
                                    (double)fs->sb.total_blocks;
    }
    if (fs->sb.total_inodes > 0) {
        health->inode_utilization = (double)(fs->sb.total_inodes - fs->sb.free_inodes) /
                                    (double)fs->sb.total_inodes;
    }

    /* 2. External fragmentation: find largest run of free blocks in data area */
    uint64_t largest_run = 0;
    uint64_t current_run = 0;
    for (uint32_t b = fs->sb.data_start; b < fs->sb.total_blocks; b++) {
        int used = bm_test(&fs->bbitmap, b);
        if (used == 0) {
            current_run++;
            if (current_run > largest_run) {
                largest_run = current_run;
            }
        } else {
            current_run = 0;
        }
    }
    health->largest_free_run = largest_run;
    health->total_free_blocks = fs->sb.free_blocks;
    if (fs->sb.free_blocks > 0) {
        health->external_frag = 1.0 - ((double)largest_run / (double)fs->sb.free_blocks);
    } else {
        health->external_frag = 0.0;
    }

    /* 3. Internal fragmentation and File fragmentation */
    uint64_t total_alloc_bytes = 0;
    uint64_t total_file_size = 0;
    uint64_t total_discontinuities = 0;

    for (uint32_t ino = 1; ino < fs->sb.total_inodes; ino++) {
        int used = bm_test(&fs->ibitmap, ino);
        if (used != 1) continue;

        inode_t node;
        if (inode_read(fs, ino, &node) != 0) continue;

        if (node.mode & S_IFREG) {
            health->total_files++;
            uint64_t alloc_bytes = (uint64_t)node.blocks * BLOCK_SIZE;
            total_alloc_bytes += alloc_bytes;
            total_file_size += node.size;

            if (node.blocks > 1) {
                health->multi_block_files++;

                /* Check discontinuities in physical block allocation */
                uint32_t nblocks_in_file = (uint32_t)((node.size + BLOCK_SIZE - 1) / BLOCK_SIZE);
                if (nblocks_in_file > 0) {
                    uint32_t prev_pbn = 0;
                    uint32_t discontinuities = 0;
                    for (uint32_t lbn = 0; lbn < nblocks_in_file; lbn++) {
                        uint32_t pbn = 0;
                        if (bmap(fs, &node, lbn, 0, &pbn) == 0 && pbn != 0) {
                            if (prev_pbn != 0 && pbn != prev_pbn + 1) {
                                discontinuities++;
                            }
                            prev_pbn = pbn;
                        }
                    }
                    if (discontinuities > 0) {
                        health->fragmented_files++;
                        total_discontinuities += discontinuities;
                    }
                }
            }
        }
    }

    if (total_alloc_bytes > 0) {
        health->internal_frag = (double)(total_alloc_bytes - total_file_size) / (double)total_alloc_bytes;
    }
    if (health->fragmented_files > 0) {
        health->avg_file_frag = (double)total_discontinuities / (double)health->fragmented_files;
    }
    if (health->multi_block_files > 0) {
        health->pct_files_fragmented = (double)health->fragmented_files * 100.0 / (double)health->multi_block_files;
    }

    /* 4. Allocator efficiency */
    if (fs->alloc_stats.alloc_calls > 0) {
        health->alloc_efficiency = (double)fs->alloc_stats.adjacent_allocs * 100.0 /
                                   (double)fs->alloc_stats.alloc_calls;
        health->avg_bits_scanned = (double)fs->alloc_stats.search_steps /
                                   (double)fs->alloc_stats.alloc_calls;
    }

    /* 5. Write amplification */
    if (g_metrics.logical_bytes_written > 0 && fs->dev) {
        health->write_amplification = (double)fs->dev->metrics.bytes_written /
                                      (double)g_metrics.logical_bytes_written;
    } else {
        health->write_amplification = 1.0;
    }

    /* 6. Cache hit ratio */
    if (fs->cache) {
        cache_stats_t cst = cache_stats((cache_t *)fs->cache);
        if (cst.hits + cst.misses > 0) {
            health->cache_hit_ratio = (double)cst.hits / (double)(cst.hits + cst.misses);
        }
    }

    return 0;
}

void metrics_print_report(FILE *out, fs_t *fs) {
    metrics_storage_health_t h;
    metrics_compute_storage_health(fs, &h);

    fprintf(out, "==================================================================\n");
    fprintf(out, "                   MINIFS SYSTEM METRICS & HEALTH                 \n");
    fprintf(out, "==================================================================\n\n");

    fprintf(out, "1. Capacity & Utilization:\n");
    fprintf(out, "   Block Utilization:      %6.2f%% (%u / %u blocks used)\n",
            h.block_utilization * 100.0,
            fs->sb.total_blocks - fs->sb.free_blocks, fs->sb.total_blocks);
    fprintf(out, "   Inode Utilization:      %6.2f%% (%u / %u inodes used)\n",
            h.inode_utilization * 100.0,
            fs->sb.total_inodes - fs->sb.free_inodes, fs->sb.total_inodes);
    fprintf(out, "\n");

    fprintf(out, "2. Storage Fragmentation:\n");
    fprintf(out, "   Internal Fragmentation: %6.2f%%\n", h.internal_frag * 100.0);
    fprintf(out, "   External Fragmentation: %6.2f%% (Largest free run: %" PRIu64 " blks)\n",
            h.external_frag * 100.0, h.largest_free_run);
    fprintf(out, "   File Fragmentation:     %6.2f%% (%u / %u multi-block files)\n",
            h.pct_files_fragmented, h.fragmented_files, h.multi_block_files);
    fprintf(out, "   Avg File Discont:       %6.2f per fragmented file\n", h.avg_file_frag);
    fprintf(out, "\n");

    fprintf(out, "3. Allocation Performance:\n");
    fprintf(out, "   Alloc Efficiency:       %6.2f%% adjacent to goal\n", h.alloc_efficiency);
    fprintf(out, "   Avg Scanned Bits:       %6.2f bits per alloc\n", h.avg_bits_scanned);
    fprintf(out, "\n");

    fprintf(out, "4. I/O Subsystem & Cache:\n");
    fprintf(out, "   Write Amplification:    %6.2fx\n", h.write_amplification);
    fprintf(out, "   Cache Hit Ratio:        %6.2f%%\n", h.cache_hit_ratio * 100.0);
    if (fs->dev) {
        fprintf(out, "   Physical Reads/Writes:  %" PRIu64 " / %" PRIu64 " (Bytes: %" PRIu64 " / %" PRIu64 ")\n",
                fs->dev->metrics.reads, fs->dev->metrics.writes,
                fs->dev->metrics.bytes_read, fs->dev->metrics.bytes_written);
    }
    fprintf(out, "   Logical Reads/Writes:   %" PRIu64 " / %" PRIu64 " (Bytes: %" PRIu64 " / %" PRIu64 ")\n",
            g_metrics.logical_reads, g_metrics.logical_writes,
            g_metrics.logical_bytes_read, g_metrics.logical_bytes_written);
    fprintf(out, "\n");

    fprintf(out, "5. Operation Latencies:\n");
    fprintf(out, "   %-10s %8s %10s %10s %10s %10s %10s\n",
            "Op", "Count", "Min (us)", "Avg (us)", "p50 (us)", "p99 (us)", "Max (us)");
    fprintf(out, "   ----------------------------------------------------------------------\n");

    for (int i = 0; i < OP_COUNT; i++) {
        metrics_percentiles_t p = metrics_calc_percentiles((metrics_op_t)i);
        if (p.count > 0) {
            fprintf(out, "   %-10s %8" PRIu64 " %10.2f %10.2f %10.2f %10.2f %10.2f\n",
                    g_op_names[i], p.count,
                    (double)p.min_ns / 1000.0,
                    p.avg_ns / 1000.0,
                    (double)p.p50_ns / 1000.0,
                    (double)p.p99_ns / 1000.0,
                    (double)p.max_ns / 1000.0);
        }
    }
    fprintf(out, "==================================================================\n");
}

void metrics_dump_csv(FILE *out, fs_t *fs) {
    metrics_storage_health_t h;
    metrics_compute_storage_health(fs, &h);

    fprintf(out, "metric,value\n");
    fprintf(out, "block_utilization,%.4f\n", h.block_utilization);
    fprintf(out, "inode_utilization,%.4f\n", h.inode_utilization);
    fprintf(out, "internal_frag,%.4f\n", h.internal_frag);
    fprintf(out, "external_frag,%.4f\n", h.external_frag);
    fprintf(out, "pct_files_fragmented,%.4f\n", h.pct_files_fragmented);
    fprintf(out, "avg_file_frag,%.4f\n", h.avg_file_frag);
    fprintf(out, "alloc_efficiency,%.4f\n", h.alloc_efficiency);
    fprintf(out, "avg_bits_scanned,%.4f\n", h.avg_bits_scanned);
    fprintf(out, "write_amplification,%.4f\n", h.write_amplification);
    fprintf(out, "cache_hit_ratio,%.4f\n", h.cache_hit_ratio);
    fprintf(out, "largest_free_run,%" PRIu64 "\n", h.largest_free_run);
    fprintf(out, "total_free_blocks,%" PRIu64 "\n", h.total_free_blocks);
    fprintf(out, "total_files,%u\n", h.total_files);
    fprintf(out, "fragmented_files,%u\n", h.fragmented_files);

    for (int i = 0; i < OP_COUNT; i++) {
        metrics_percentiles_t p = metrics_calc_percentiles((metrics_op_t)i);
        if (p.count > 0) {
            fprintf(out, "%s_count,%" PRIu64 "\n", g_op_names[i], p.count);
            fprintf(out, "%s_min_us,%.2f\n", g_op_names[i], (double)p.min_ns / 1000.0);
            fprintf(out, "%s_avg_us,%.2f\n", g_op_names[i], p.avg_ns / 1000.0);
            fprintf(out, "%s_p50_us,%.2f\n", g_op_names[i], (double)p.p50_ns / 1000.0);
            fprintf(out, "%s_p99_us,%.2f\n", g_op_names[i], (double)p.p99_ns / 1000.0);
            fprintf(out, "%s_max_us,%.2f\n", g_op_names[i], (double)p.max_ns / 1000.0);
        }
    }
}
