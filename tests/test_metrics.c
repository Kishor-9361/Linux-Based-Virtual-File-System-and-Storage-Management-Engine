#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "metrics.h"
#include "vfs.h"
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>

#define TEST_METRICS_IMG "/tmp/minifs_test_metrics.img"

TEST(m13_test1_timer_sanity) {
    uint64_t t0 = metrics_now_ns();
    usleep(50000); /* 50 ms */
    uint64_t t1 = metrics_now_ns();

    ASSERT_TRUE(t1 > t0);
    uint64_t diff = t1 - t0;
    /* Allow tolerance: 45 ms to 150 ms (accounting for scheduling jitter) */
    ASSERT_TRUE(diff >= 40000000ULL);
    ASSERT_TRUE(diff <= 200000000ULL);
}

TEST(m13_test2_percentiles_calculation) {
    metrics_reset();

    /* Record exactly 1000 samples for OP_READ: values 1, 2, ..., 1000 */
    for (uint64_t v = 1; v <= 1000; v++) {
        metrics_record_op(OP_READ, v * 1000ULL); /* 1us .. 1000us */
    }

    metrics_percentiles_t p = metrics_calc_percentiles(OP_READ);
    ASSERT_EQ(p.count, 1000);
    ASSERT_EQ(p.min_ns, 1000ULL);
    ASSERT_EQ(p.max_ns, 1000000ULL);

    /* Average should be ~500.5 us (500500 ns) */
    ASSERT_TRUE(p.avg_ns >= 500000.0 && p.avg_ns <= 501000.0);

    /* p50 should be ~500 us (500000 ns) */
    ASSERT_TRUE(p.p50_ns >= 490000ULL && p.p50_ns <= 510000ULL);

    /* p99 should be ~990 us (990000 ns) */
    ASSERT_TRUE(p.p99_ns >= 980000ULL && p.p99_ns <= 1000000ULL);

    metrics_reset();
}

TEST(m13_test3_synthetic_workload_exact_counters) {
    unlink(TEST_METRICS_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, TEST_METRICS_IMG, 1, 4096));
    ASSERT_OK(sb_format(&v.dev));
    ASSERT_OK(fs_init_allocators(&v.fs, &v.dev));
    ASSERT_OK(dir_bootstrap_root(&v.fs));
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, TEST_METRICS_IMG));

    uint32_t initial_free = v.fs.sb.free_blocks;

    /* Write exactly 100 blocks into a file */
    int fd = vfs_open(&v, "/synthetic.dat", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd >= 0);

    char buf[BLOCK_SIZE];
    memset(buf, 0xAB, BLOCK_SIZE);
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(vfs_write(&v, fd, buf, BLOCK_SIZE), (ssize_t)BLOCK_SIZE);
    }
    ASSERT_OK(vfs_close(&v, fd));
    ASSERT_OK(vfs_sync(&v));

    /* 100 data blocks + 1 indirect block (since 100 > 12) = 101 allocated blocks */
    uint32_t allocated = initial_free - v.fs.sb.free_blocks;
    ASSERT_EQ(allocated, 101);

    metrics_storage_health_t h;
    ASSERT_OK(metrics_compute_storage_health(&v.fs, &h));

    /* Hand-calculated expected block utilization: (4096 - free) / 4096 */
    double expected_util = (double)(4096 - v.fs.sb.free_blocks) / 4096.0;
    ASSERT_TRUE(fabs(h.block_utilization - expected_util) < 1e-6);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_METRICS_IMG);
}

TEST(m13_test4_health_vs_independent_computation) {
    unlink(TEST_METRICS_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, TEST_METRICS_IMG, 1, 4096));
    ASSERT_OK(sb_format(&v.dev));
    ASSERT_OK(fs_init_allocators(&v.fs, &v.dev));
    ASSERT_OK(dir_bootstrap_root(&v.fs));
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, TEST_METRICS_IMG));

    /* Create 5 files with various sizes */
    size_t sizes[] = {100, 4096, 7000, 16384, 50000};
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/file_%d.bin", i);
        int fd = vfs_open(&v, name, O_CREAT | O_WRONLY, 0644);
        ASSERT_TRUE(fd >= 0);

        char *buf = (char *)malloc(sizes[i]);
        ASSERT_TRUE(buf != NULL);
        memset(buf, 'Z', sizes[i]);
        ASSERT_EQ(vfs_write(&v, fd, buf, sizes[i]), (ssize_t)sizes[i]);
        free(buf);
        vfs_close(&v, fd);
    }
    vfs_sync(&v);

    /* Independent computation from scratch */
    uint64_t indep_total_file_size = 0;
    uint64_t indep_total_alloc_bytes = 0;
    uint32_t indep_reg_files = 0;

    for (uint32_t ino = 1; ino < v.fs.sb.total_inodes; ino++) {
        if (bm_test(&v.fs.ibitmap, ino) == 1) {
            inode_t node;
            if (inode_read(&v.fs, ino, &node) == 0 && (node.mode & S_IFREG)) {
                indep_reg_files++;
                indep_total_file_size += node.size;
                indep_total_alloc_bytes += (uint64_t)node.blocks * BLOCK_SIZE;
            }
        }
    }

    double indep_internal_frag = (indep_total_alloc_bytes > 0) ?
        ((double)(indep_total_alloc_bytes - indep_total_file_size) / (double)indep_total_alloc_bytes) : 0.0;

    /* Compute largest free run independently */
    uint64_t indep_largest_run = 0;
    uint64_t cur_run = 0;
    for (uint32_t b = v.fs.sb.data_start; b < v.fs.sb.total_blocks; b++) {
        if (bm_test(&v.fs.bbitmap, b) == 0) {
            cur_run++;
            if (cur_run > indep_largest_run) indep_largest_run = cur_run;
        } else {
            cur_run = 0;
        }
    }

    /* Verify against metrics engine */
    metrics_storage_health_t h;
    ASSERT_OK(metrics_compute_storage_health(&v.fs, &h));

    ASSERT_EQ(h.total_files, indep_reg_files);
    ASSERT_TRUE(fabs(h.internal_frag - indep_internal_frag) < 1e-6);
    ASSERT_EQ(h.largest_free_run, indep_largest_run);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_METRICS_IMG);
}

TEST(m13_test5_file_fragmentation_detection) {
    unlink(TEST_METRICS_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, TEST_METRICS_IMG, 1, 2048));
    ASSERT_OK(sb_format(&v.dev));
    fs_init_allocators(&v.fs, &v.dev);
    dir_bootstrap_root(&v.fs);
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, TEST_METRICS_IMG));

    /* Create interleaved files: a1, b1, a2, b2 */
    int fd_a = vfs_open(&v, "/file_a.dat", O_CREAT | O_WRONLY, 0644);
    int fd_b = vfs_open(&v, "/file_b.dat", O_CREAT | O_WRONLY, 0644);
    ASSERT_TRUE(fd_a >= 0 && fd_b >= 0);

    char buf[BLOCK_SIZE];
    memset(buf, 'X', BLOCK_SIZE);

    /* Alternate writes between fd_a and fd_b to create physical discontinuities */
    for (int i = 0; i < 4; i++) {
        vfs_write(&v, fd_a, buf, BLOCK_SIZE);
        vfs_write(&v, fd_b, buf, BLOCK_SIZE);
    }
    vfs_close(&v, fd_a);
    vfs_close(&v, fd_b);
    vfs_sync(&v);

    metrics_storage_health_t h;
    ASSERT_OK(metrics_compute_storage_health(&v.fs, &h));

    /* Both file_a and file_b are multi-block and should have discontinuities */
    ASSERT_TRUE(h.multi_block_files >= 2);
    ASSERT_TRUE(h.fragmented_files >= 1);
    ASSERT_TRUE(h.pct_files_fragmented > 0.0);
    ASSERT_TRUE(h.avg_file_frag > 0.0);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_METRICS_IMG);
}

TEST(m13_test6_csv_and_report_output) {
    unlink(TEST_METRICS_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, TEST_METRICS_IMG, 1, 1024));
    ASSERT_OK(sb_format(&v.dev));
    fs_init_allocators(&v.fs, &v.dev);
    dir_bootstrap_root(&v.fs);
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, TEST_METRICS_IMG));

    const char *csv_file = "/tmp/test_metrics_out.csv";
    FILE *f = fopen(csv_file, "w");
    ASSERT_TRUE(f != NULL);
    metrics_dump_csv(f, &v.fs);
    fclose(f);

    /* Verify CSV file has content and starts with header */
    f = fopen(csv_file, "r");
    ASSERT_TRUE(f != NULL);
    char line[256];
    ASSERT_TRUE(fgets(line, sizeof(line), f) != NULL);
    ASSERT_STR_EQ(line, "metric,value\n");

    int line_count = 1;
    while (fgets(line, sizeof(line), f)) {
        line_count++;
        ASSERT_TRUE(strchr(line, ',') != NULL);
    }
    fclose(f);
    unlink(csv_file);

    ASSERT_TRUE(line_count > 10);

    ASSERT_OK(vfs_unmount(&v));
    unlink(TEST_METRICS_IMG);
}

TEST(m13_test7_live_counters_and_reset) {
    metrics_reset();

    metrics_record_logical_io(4096, true);
    metrics_record_logical_io(8192, false);
    metrics_record_dir_scan(42);

    metrics_live_t live = metrics_get_live();
    ASSERT_EQ(live.logical_bytes_written, 4096);
    ASSERT_EQ(live.logical_writes, 1);
    ASSERT_EQ(live.logical_bytes_read, 8192);
    ASSERT_EQ(live.logical_reads, 1);
    ASSERT_EQ(live.dir_lookups, 1);
    ASSERT_EQ(live.dir_entries_scanned, 42);

    metrics_reset();
    live = metrics_get_live();
    ASSERT_EQ(live.logical_bytes_written, 0);
    ASSERT_EQ(live.logical_writes, 0);
    ASSERT_EQ(live.dir_lookups, 0);
}
