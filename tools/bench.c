#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "common.h"
#include "vfs.h"
#include "cache.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#define BENCH_DEFAULT_IMG "/tmp/minifs_bench.img"
#define REPETITIONS 5

static void calc_mean_stddev(const double *vals, size_t n, double *mean, double *stddev) {
    if (n == 0) {
        *mean = 0.0;
        *stddev = 0.0;
        return;
    }
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += vals[i];
    *mean = sum / (double)n;

    double var = 0.0;
    for (size_t i = 0; i < n; i++) {
        double diff = vals[i] - *mean;
        var += diff * diff;
    }
    *stddev = sqrt(var / (double)n);
}

/* 1. Sequential throughput across I/O sizes (512 B to 1 MB) */
static int run_bench_seq(const char *img, FILE *out) {
    fprintf(out, "io_size,write_mb_s,write_stddev,read_mb_s,read_stddev\n");

    size_t sizes[] = {512, 1024, 4096, 16384, 65536, 262144, 1048576};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t total_data = 8 * 1024 * 1024; /* 8 MB test file */

    for (size_t s = 0; s < num_sizes; s++) {
        size_t chunk_sz = sizes[s];
        char *buf = (char *)malloc(chunk_sz);
        if (!buf) return -ENOMEM;
        memset(buf, 'X', chunk_sz);

        double write_mb_s[REPETITIONS];
        double read_mb_s[REPETITIONS];

        for (int r = 0; r < REPETITIONS; r++) {
            unlink(img);
            vfs_t v;
            memset(&v, 0, sizeof(v));
            blk_open(&v.dev, img, 1, 16384);
            sb_format(&v.dev);
            fs_init_allocators(&v.fs, &v.dev);
            dir_bootstrap_root(&v.fs);
            blk_close(&v.dev);

            vfs_mount(&v, img);

            /* Write measurement */
            int fd = vfs_open(&v, "/seq_test.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
            uint64_t t0 = metrics_now_ns();
            size_t written = 0;
            while (written < total_data) {
                ssize_t n = vfs_write(&v, fd, buf, chunk_sz);
                if (n <= 0) break;
                written += (size_t)n;
            }
            vfs_sync(&v);
            uint64_t t1 = metrics_now_ns();
            vfs_close(&v, fd);

            double wr_sec = (double)(t1 - t0) / 1e9;
            write_mb_s[r] = ((double)written / (1024.0 * 1024.0)) / wr_sec;

            /* Read measurement */
            fd = vfs_open(&v, "/seq_test.bin", O_RDONLY, 0);
            t0 = metrics_now_ns();
            size_t read_bytes = 0;
            while (read_bytes < written) {
                ssize_t n = vfs_read(&v, fd, buf, chunk_sz);
                if (n <= 0) break;
                read_bytes += (size_t)n;
            }
            t1 = metrics_now_ns();
            vfs_close(&v, fd);

            double rd_sec = (double)(t1 - t0) / 1e9;
            read_mb_s[r] = ((double)read_bytes / (1024.0 * 1024.0)) / rd_sec;

            vfs_unmount(&v);
        }

        free(buf);
        double w_mean, w_sd, r_mean, r_sd;
        calc_mean_stddev(write_mb_s, REPETITIONS, &w_mean, &w_sd);
        calc_mean_stddev(read_mb_s, REPETITIONS, &r_mean, &r_sd);

        fprintf(out, "%zu,%.2f,%.2f,%.2f,%.2f\n", chunk_sz, w_mean, w_sd, r_mean, r_sd);
        fflush(out);
    }
    return 0;
}

/* 2. 4 KB Random I/O IOPS and latency */
static int run_bench_rand(const char *img, FILE *out) {
    fprintf(out, "op,iops,avg_us,p50_us,p99_us\n");

    unlink(img);
    vfs_t v;
    memset(&v, 0, sizeof(v));
    blk_open(&v.dev, img, 1, 16384);
    sb_format(&v.dev);
    fs_init_allocators(&v.fs, &v.dev);
    dir_bootstrap_root(&v.fs);
    blk_close(&v.dev);

    vfs_mount(&v, img);

    /* Prepare a 16 MiB target file (4096 blocks) */
    int fd = vfs_open(&v, "/rand_test.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    char buf[BLOCK_SIZE];
    memset(buf, 'A', BLOCK_SIZE);
    uint32_t num_blocks = 2048;
    for (uint32_t i = 0; i < num_blocks; i++) {
        vfs_write(&v, fd, buf, BLOCK_SIZE);
    }
    vfs_close(&v, fd);

    /* 4 KB random writes */
    metrics_init();
    fd = vfs_open(&v, "/rand_test.bin", O_RDWR, 0);
    uint32_t ops = 2000;
    uint64_t t0 = metrics_now_ns();
    for (uint32_t i = 0; i < ops; i++) {
        off_t blk_idx = (off_t)(rand() % (int)num_blocks);
        vfs_lseek(&v, fd, blk_idx * BLOCK_SIZE, SEEK_SET);
        uint64_t op_t0 = metrics_now_ns();
        vfs_write(&v, fd, buf, BLOCK_SIZE);
        uint64_t op_t1 = metrics_now_ns();
        metrics_record_op(OP_WRITE, op_t1 - op_t0);
    }
    uint64_t t1 = metrics_now_ns();
    double write_sec = (double)(t1 - t0) / 1e9;
    double write_iops = (double)ops / write_sec;
    metrics_percentiles_t wr_pct = metrics_calc_percentiles(OP_WRITE);

    fprintf(out, "rand_write,%.1f,%.2f,%.2f,%.2f\n",
            write_iops, wr_pct.avg_ns / 1000.0,
            (double)wr_pct.p50_ns / 1000.0, (double)wr_pct.p99_ns / 1000.0);

    /* 4 KB random reads */
    metrics_init();
    t0 = metrics_now_ns();
    for (uint32_t i = 0; i < ops; i++) {
        off_t blk_idx = (off_t)(rand() % (int)num_blocks);
        vfs_lseek(&v, fd, blk_idx * BLOCK_SIZE, SEEK_SET);
        uint64_t op_t0 = metrics_now_ns();
        vfs_read(&v, fd, buf, BLOCK_SIZE);
        uint64_t op_t1 = metrics_now_ns();
        metrics_record_op(OP_READ, op_t1 - op_t0);
    }
    t1 = metrics_now_ns();
    double read_sec = (double)(t1 - t0) / 1e9;
    double read_iops = (double)ops / read_sec;
    metrics_percentiles_t rd_pct = metrics_calc_percentiles(OP_READ);

    fprintf(out, "rand_read,%.1f,%.2f,%.2f,%.2f\n",
            read_iops, rd_pct.avg_ns / 1000.0,
            (double)rd_pct.p50_ns / 1000.0, (double)rd_pct.p99_ns / 1000.0);

    vfs_close(&v, fd);
    vfs_unmount(&v);
    return 0;
}

/* 3. Cache sweep (uncached vs cached 16..1024) */
static int run_bench_cache(const char *img, FILE *out) {
    fprintf(out, "cache_size,throughput_mb_s,hit_ratio_pct\n");

    size_t cache_sizes[] = {0, 16, 64, 256, 1024};
    size_t num_sizes = sizeof(cache_sizes) / sizeof(cache_sizes[0]);

    for (size_t c = 0; c < num_sizes; c++) {
        size_t c_sz = cache_sizes[c];

        unlink(img);
        vfs_t v;
        memset(&v, 0, sizeof(v));
        blk_open(&v.dev, img, 1, 16384);
        sb_format(&v.dev);
        fs_init_allocators(&v.fs, &v.dev);
        dir_bootstrap_root(&v.fs);
        blk_close(&v.dev);

        vfs_mount(&v, img);

        /* Configure cache size */
        if (v.fs.cache) {
            cache_destroy((cache_t *)v.fs.cache);
            v.fs.cache = NULL;
        }
        if (c_sz > 0) {
            v.fs.cache = cache_create(&v.dev, (uint32_t)c_sz, CACHE_WRITE_BACK);
        }

        /* Write working set of 512 blocks (2 MB) */
        int fd = vfs_open(&v, "/cache_workload.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        char buf[BLOCK_SIZE];
        memset(buf, 'C', BLOCK_SIZE);
        uint32_t working_blocks = 512;
        for (uint32_t i = 0; i < working_blocks; i++) {
            vfs_write(&v, fd, buf, BLOCK_SIZE);
        }
        vfs_close(&v, fd);

        /* Repeated reads across working set (10 passes) */
        fd = vfs_open(&v, "/cache_workload.bin", O_RDONLY, 0);
        uint64_t t0 = metrics_now_ns();
        for (int pass = 0; pass < 10; pass++) {
            vfs_lseek(&v, fd, 0, SEEK_SET);
            for (uint32_t i = 0; i < working_blocks; i++) {
                vfs_read(&v, fd, buf, BLOCK_SIZE);
            }
        }
        uint64_t t1 = metrics_now_ns();
        vfs_close(&v, fd);

        double elapsed = (double)(t1 - t0) / 1e9;
        double mb_s = ((double)(working_blocks * 10 * BLOCK_SIZE) / (1024.0 * 1024.0)) / elapsed;

        double hit_ratio = 0.0;
        if (v.fs.cache) {
            cache_stats_t st = cache_stats((cache_t *)v.fs.cache);
            if (st.hits + st.misses > 0) {
                hit_ratio = (double)st.hits * 100.0 / (double)(st.hits + st.misses);
            }
        }

        fprintf(out, "%zu,%.2f,%.2f\n", c_sz, mb_s, hit_ratio);
        fflush(out);

        vfs_unmount(&v);
    }
    return 0;
}

/* 4. Journal metadata test */
static int run_bench_journal(const char *img, FILE *out) {
    fprintf(out, "journal_mode,files_per_sec,elapsed_sec\n");

    for (int mode = 0; mode <= 1; mode++) {
        unlink(img);
        vfs_t v;
        memset(&v, 0, sizeof(v));
        blk_open(&v.dev, img, 1, 16384);
        sb_format(&v.dev);
        fs_init_allocators(&v.fs, &v.dev);
        dir_bootstrap_root(&v.fs);
        blk_close(&v.dev);

        vfs_mount(&v, img);

        if (mode == 0 && v.fs.journal) {
            /* Disable journal */
            journal_destroy((journal_t *)v.fs.journal);
            v.fs.journal = NULL;
        }

        uint32_t nfiles = 1000;
        uint64_t t0 = metrics_now_ns();
        for (uint32_t i = 0; i < nfiles; i++) {
            char name[64];
            snprintf(name, sizeof(name), "/meta_%u.txt", i);
            int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) vfs_close(&v, fd);
        }
        vfs_sync(&v);
        uint64_t t1 = metrics_now_ns();

        double elapsed = (double)(t1 - t0) / 1e9;
        double fps = (double)nfiles / elapsed;

        fprintf(out, "%s,%.1f,%.3f\n", (mode == 1 ? "journal_on" : "journal_off"), fps, elapsed);
        fflush(out);

        vfs_unmount(&v);
    }
    return 0;
}

/* 5. Directory lookup vs size sweep */
static int run_bench_dir(const char *img, FILE *out) {
    fprintf(out, "entries,lookup_time_us\n");

    uint32_t target_counts[] = {10, 100, 1000, 5000, 10000};
    size_t num_counts = sizeof(target_counts) / sizeof(target_counts[0]);

    unlink(img);
    vfs_t v;
    memset(&v, 0, sizeof(v));
    blk_open(&v.dev, img, 1, 16384);
    sb_format(&v.dev);
    fs_init_allocators(&v.fs, &v.dev);
    dir_bootstrap_root(&v.fs);
    blk_close(&v.dev);

    vfs_mount(&v, img);

    uint32_t current_entries = 0;
    for (size_t c = 0; c < num_counts; c++) {
        uint32_t target = target_counts[c];
        while (current_entries < target) {
            char fname[64];
            snprintf(fname, sizeof(fname), "/file_%u.dat", current_entries);
            int fd = vfs_open(&v, fname, O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) vfs_close(&v, fd);
            current_entries++;
        }

        /* Measure 1000 lookups for random existing files */
        uint32_t lookups = 500;
        uint64_t t0 = metrics_now_ns();
        for (uint32_t i = 0; i < lookups; i++) {
            uint32_t query_idx = (uint32_t)(rand() % (int)current_entries);
            char qname[64];
            snprintf(qname, sizeof(qname), "/file_%u.dat", query_idx);
            struct vfs_stat st;
            vfs_stat(&v, qname, &st);
        }
        uint64_t t1 = metrics_now_ns();

        double avg_lookup_us = ((double)(t1 - t0) / 1000.0) / (double)lookups;
        fprintf(out, "%u,%.2f\n", target, avg_lookup_us);
        fflush(out);
    }

    vfs_unmount(&v);
    return 0;
}

/* 6. Fragmentation vs fill level */
static int run_bench_frag(const char *img, FILE *out) {
    fprintf(out, "fill_pct,block_util,external_frag,pct_files_frag,avg_file_frag\n");

    int fill_levels[] = {30, 60, 90};
    for (int l = 0; l < 3; l++) {
        int fill = fill_levels[l];

        unlink(img);
        vfs_t v;
        memset(&v, 0, sizeof(v));
        blk_open(&v.dev, img, 1, 8192);
        sb_format(&v.dev);
        fs_init_allocators(&v.fs, &v.dev);
        dir_bootstrap_root(&v.fs);
        blk_close(&v.dev);

        vfs_mount(&v, img);

        /* Fill FS with a mix of files to target utilization */
        uint32_t target_blocks = (uint32_t)((8192 * fill) / 100);
        uint32_t created = 0;
        char buf[BLOCK_SIZE];
        memset(buf, 'F', sizeof(buf));

        while ((8192 - v.fs.sb.free_blocks) < target_blocks && created < 500) {
            char name[64];
            snprintf(name, sizeof(name), "/f_%u.dat", created);
            int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
            if (fd < 0) break;
            /* Random size 1 .. 8 blocks */
            int blks = 1 + (rand() % 8);
            for (int b = 0; b < blks; b++) {
                if (vfs_write(&v, fd, buf, BLOCK_SIZE) <= 0) break;
            }
            vfs_close(&v, fd);
            created++;
        }

        /* Delete every 3rd file to induce fragmentation */
        for (uint32_t i = 0; i < created; i += 3) {
            char name[64];
            snprintf(name, sizeof(name), "/f_%u.dat", i);
            vfs_unlink(&v, name);
        }

        /* Fill back holes with varying sizes */
        for (uint32_t i = 1000; i < 1100 && (8192 - v.fs.sb.free_blocks) < target_blocks; i++) {
            char name[64];
            snprintf(name, sizeof(name), "/refill_%u.dat", i);
            int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
            if (fd >= 0) {
                int blks = 1 + (rand() % 4);
                for (int b = 0; b < blks; b++) {
                    if (vfs_write(&v, fd, buf, BLOCK_SIZE) <= 0) break;
                }
                vfs_close(&v, fd);
            }
        }

        metrics_storage_health_t h;
        metrics_compute_storage_health(&v.fs, &h);

        fprintf(out, "%d,%.2f,%.2f,%.2f,%.2f\n",
                fill, h.block_utilization * 100.0,
                h.external_frag * 100.0,
                h.pct_files_fragmented,
                h.avg_file_frag);
        fflush(out);

        vfs_unmount(&v);
    }
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <seq|rand|cache|journal|dir|frag|all> [image_path] [csv_output]\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *sub = argv[1];
    const char *img = (argc > 2) ? argv[2] : BENCH_DEFAULT_IMG;
    const char *csv_path = (argc > 3) ? argv[3] : NULL;

    FILE *out = stdout;
    if (csv_path) {
        out = fopen(csv_path, "w");
        if (!out) {
            perror("fopen");
            return 1;
        }
    }

    int rc = 0;
    if (strcmp(sub, "seq") == 0) {
        rc = run_bench_seq(img, out);
    } else if (strcmp(sub, "rand") == 0) {
        rc = run_bench_rand(img, out);
    } else if (strcmp(sub, "cache") == 0) {
        rc = run_bench_cache(img, out);
    } else if (strcmp(sub, "journal") == 0) {
        rc = run_bench_journal(img, out);
    } else if (strcmp(sub, "dir") == 0) {
        rc = run_bench_dir(img, out);
    } else if (strcmp(sub, "frag") == 0) {
        rc = run_bench_frag(img, out);
    } else if (strcmp(sub, "all") == 0) {
        run_bench_seq(img, out);
        run_bench_rand(img, out);
        run_bench_cache(img, out);
        run_bench_journal(img, out);
        run_bench_dir(img, out);
        run_bench_frag(img, out);
    } else {
        print_usage(argv[0]);
        rc = 1;
    }

    if (csv_path) fclose(out);
    unlink(img);
    return rc;
}
