#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "vfs.h"
#include "fsck.h"
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define CONCURRENCY_IMG "/tmp/minifs_test_concurrency.img"
#define NUM_THREADS 4
#define OPS_PER_THREAD 500

typedef struct {
    int thread_id;
    vfs_t *v;
    pthread_barrier_t *barrier;
    int error_count;
} worker_arg_t;

static void *worker_func(void *arg) {
    worker_arg_t *w = (worker_arg_t *)arg;
    vfs_t *v = w->v;

    /* Wait for all threads to reach start line */
    pthread_barrier_wait(w->barrier);

    char dir_name[64];
    snprintf(dir_name, sizeof(dir_name), "/thread_%d", w->thread_id);
    vfs_mkdir(v, dir_name, 0755);

    char buf[BLOCK_SIZE];
    memset(buf, (int)('A' + w->thread_id), sizeof(buf));

    for (int i = 0; i < OPS_PER_THREAD; i++) {
        char file_name[128];
        snprintf(file_name, sizeof(file_name), "%s/file_%d.dat", dir_name, i % 20);

        int action = i % 4;
        if (action == 0) {
            /* Create and write private file */
            int fd = vfs_open(v, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd >= 0) {
                vfs_write(v, fd, buf, 512);
                vfs_close(v, fd);
            } else {
                w->error_count++;
            }
        } else if (action == 1) {
            /* Read private file */
            int fd = vfs_open(v, file_name, O_RDONLY, 0);
            if (fd >= 0) {
                char r_buf[512];
                vfs_read(v, fd, r_buf, sizeof(r_buf));
                vfs_close(v, fd);
            }
        } else if (action == 2) {
            /* Stat private file */
            struct vfs_stat st;
            vfs_stat(v, file_name, &st);
        } else {
            /* Append to shared file */
            int fd = vfs_open(v, "/shared.log", O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd >= 0) {
                char entry[64];
                snprintf(entry, sizeof(entry), "T%d-step%d\n", w->thread_id, i);
                vfs_write(v, fd, entry, strlen(entry));
                vfs_close(v, fd);
            }
        }
    }

    return NULL;
}

TEST(m14_test1_multithreaded_concurrency) {
    unlink(CONCURRENCY_IMG);

    vfs_t v;
    memset(&v, 0, sizeof(v));
    ASSERT_OK(blk_open(&v.dev, CONCURRENCY_IMG, 1, 8192));
    ASSERT_OK(sb_format(&v.dev));
    ASSERT_OK(fs_init_allocators(&v.fs, &v.dev));
    ASSERT_OK(dir_bootstrap_root(&v.fs));
    blk_close(&v.dev);

    ASSERT_OK(vfs_mount(&v, CONCURRENCY_IMG));

    pthread_t threads[NUM_THREADS];
    worker_arg_t args[NUM_THREADS];
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NUM_THREADS);

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].v = &v;
        args[i].barrier = &barrier;
        args[i].error_count = 0;
        ASSERT_EQ(pthread_create(&threads[i], NULL, worker_func, &args[i]), 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        ASSERT_EQ(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ(args[i].error_count, 0);
    }

    pthread_barrier_destroy(&barrier);

    ASSERT_OK(vfs_sync(&v));
    ASSERT_OK(vfs_unmount(&v));

    /* Check consistency with fsck */
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, CONCURRENCY_IMG, 0, 0));
    fsck_options_t opts = { .repair = false, .verbose = false };
    fsck_stats_t stats;
    ASSERT_OK(fsck_check(&dev, &opts, &stats));
    ASSERT_EQ(stats.errors_found, 0);
    blk_close(&dev);

    unlink(CONCURRENCY_IMG);
}
