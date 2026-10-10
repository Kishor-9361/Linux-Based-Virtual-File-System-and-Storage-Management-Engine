#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include "fileio.h"
#include "inode.h"
#include "bitmap.h"
#include "sb.h"
#include "blk.h"
#include "common.h"
#include <unistd.h>
#include <fcntl.h>

#define TEST_FIO_IMG "/tmp/minifs_test_fio.img"
#define HOST_EXTRACT "/tmp/minifs_test_extracted.bin"
#define HOST_ORIGINAL "/tmp/minifs_test_original.bin"

static void cleanup_test_files(void) {
    unlink(TEST_FIO_IMG);
    unlink(HOST_EXTRACT);
    unlink(HOST_ORIGINAL);
}

TEST(m5_test1_write_read_various_sizes) {
    cleanup_test_files();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    size_t sizes[] = {1, 4095, 4096, 4097, 8192};
    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); s++) {
        size_t sz = sizes[s];
        uint8_t *wbuf = (uint8_t *)th_malloc(sz);
        uint8_t *rbuf = (uint8_t *)th_malloc(sz);
        ASSERT_TRUE(wbuf && rbuf);

        for (size_t i = 0; i < sz; i++) {
            wbuf[i] = (uint8_t)((i + s) & 0xFF);
        }

        /* Truncate to 0 before each size test */
        ASSERT_OK(file_truncate(&fs, (uint32_t)ino, 0));
        ASSERT_EQ(file_write(&fs, (uint32_t)ino, 0, wbuf, sz), (ssize_t)sz);

        memset(rbuf, 0, sz);
        ASSERT_EQ(file_read(&fs, (uint32_t)ino, 0, rbuf, sz), (ssize_t)sz);
        ASSERT_MEM_EQ(wbuf, rbuf, sz);

        th_free(wbuf);
        th_free(rbuf);
    }

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test2_unaligned_and_indirection_crossings) {
    cleanup_test_files();
    blkdev_t dev;
    /* 16 MB image (4096 blocks) */
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, 4096));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* 1. Unaligned write at offsets 1, 4095, 4096, 4097 */
    uint64_t offsets[] = {1, 4095, 4096, 4097};
    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        uint64_t off = offsets[i];
        char msg[] = "UnalignedPayloadCheck";
        size_t len = strlen(msg);
        ASSERT_EQ(file_write(&fs, (uint32_t)ino, off, msg, len), (ssize_t)len);

        char readback[64];
        memset(readback, 0, sizeof(readback));
        ASSERT_EQ(file_read(&fs, (uint32_t)ino, off, readback, len), (ssize_t)len);
        ASSERT_STR_EQ(msg, readback);
    }

    /* 2. Direct -> Indirect boundary crossing (~48 KB = 12 * 4096 = 49152) */
    uint64_t direct_cross_off = (12 * BLOCK_SIZE) - 500;
    uint8_t cross_buf[1000];
    memset(cross_buf, 0xAB, sizeof(cross_buf));
    ASSERT_EQ(file_write(&fs, (uint32_t)ino, direct_cross_off, cross_buf, sizeof(cross_buf)), (ssize_t)sizeof(cross_buf));

    uint8_t cross_read[1000];
    ASSERT_EQ(file_read(&fs, (uint32_t)ino, direct_cross_off, cross_read, sizeof(cross_read)), (ssize_t)sizeof(cross_read));
    ASSERT_MEM_EQ(cross_buf, cross_read, sizeof(cross_buf));

    /* 3. Indirect -> Double-indirect crossing (offset = 1036 * 4096 = 4243456) */
    uint64_t dind_cross_off = ((uint64_t)DOUBLE_INDIRECT_START * BLOCK_SIZE) - 500;
    memset(cross_buf, 0xCD, sizeof(cross_buf));
    ASSERT_EQ(file_write(&fs, (uint32_t)ino, dind_cross_off, cross_buf, sizeof(cross_buf)), (ssize_t)sizeof(cross_buf));

    ASSERT_EQ(file_read(&fs, (uint32_t)ino, dind_cross_off, cross_read, sizeof(cross_read)), (ssize_t)sizeof(cross_read));
    ASSERT_MEM_EQ(cross_buf, cross_read, sizeof(cross_buf));

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test3_write_100mb_and_sha256_verify) {
    cleanup_test_files();
    blkdev_t dev;
    /* 100 MB data = 25600 blocks + 1000 blocks metadata = 27000 blocks */
    uint32_t total_blocks = 27000;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, total_blocks));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* Create original file on host disk and simultaneously write to minifs */
    int host_fd = open(HOST_ORIGINAL, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(host_fd >= 0);

    size_t chunk_sz = 64 * 1024; /* 64 KB chunks */
    size_t total_bytes = 100 * 1024 * 1024; /* 100 MB */
    uint8_t *chunk = (uint8_t *)th_malloc(chunk_sz);
    ASSERT_TRUE(chunk != NULL);

    uint64_t written = 0;
    uint32_t prng_state = 0x12345678;

    while (written < total_bytes) {
        size_t to_write = (total_bytes - written < chunk_sz) ? (total_bytes - written) : chunk_sz;
        for (size_t i = 0; i < to_write; i++) {
            prng_state = prng_state * 1664525U + 1013904223U;
            chunk[i] = (uint8_t)(prng_state >> 24);
        }

        /* Write to host */
        ssize_t hw = write(host_fd, chunk, to_write);
        ASSERT_EQ(hw, (ssize_t)to_write);

        /* Write to minifs */
        ssize_t fw = file_write(&fs, (uint32_t)ino, written, chunk, to_write);
        ASSERT_EQ(fw, (ssize_t)to_write);

        written += to_write;
    }
    close(host_fd);

    /* Read back from minifs and write to HOST_EXTRACT */
    int extract_fd = open(HOST_EXTRACT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(extract_fd >= 0);

    uint64_t read_bytes = 0;
    while (read_bytes < total_bytes) {
        size_t to_read = (total_bytes - read_bytes < chunk_sz) ? (total_bytes - read_bytes) : chunk_sz;
        ssize_t fr = file_read(&fs, (uint32_t)ino, read_bytes, chunk, to_read);
        ASSERT_EQ(fr, (ssize_t)to_read);

        ssize_t ew = write(extract_fd, chunk, to_read);
        ASSERT_EQ(ew, (ssize_t)to_read);

        read_bytes += to_read;
    }
    close(extract_fd);
    th_free(chunk);

    /* Compare SHA-256 using sha256sum */
    int diff = system("sha256sum " HOST_ORIGINAL " " HOST_EXTRACT " | awk '{print $1}' | uniq | wc -l | grep -q '^1$'");
    ASSERT_EQ(diff, 0);

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test4_sparse_write_at_1gb) {
    cleanup_test_files();
    blkdev_t dev;
    /* Create a disk that has at least enough blocks for dindirect metadata and 1 data block */
    uint32_t total = 4096;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, total));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* Write 4 bytes at offset 1 GB (1073741824 bytes = block 262144 in double indirect) */
    uint64_t off_1gb = 1073741824ULL;
    char payload[] = "SPARSE_AT_1GB";
    size_t len = strlen(payload);

    ASSERT_EQ(file_write(&fs, (uint32_t)ino, off_1gb, payload, len), (ssize_t)len);

    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino, &node));
    ASSERT_EQ(node.size, off_1gb + len);
    /* blocks allocated should be small (1 top dindirect + 1 second-level + 1 data block = 3 blocks) */
    ASSERT_TRUE(node.blocks <= 4);

    /* Reading hole before 1 GB returns zeros */
    char hole_buf[100];
    ASSERT_EQ(file_read(&fs, (uint32_t)ino, 500000, hole_buf, sizeof(hole_buf)), (ssize_t)sizeof(hole_buf));
    for (size_t i = 0; i < sizeof(hole_buf); i++) {
        ASSERT_EQ(hole_buf[i], 0);
    }

    /* Reading payload at 1 GB matches */
    char read_payload[64];
    memset(read_payload, 0, sizeof(read_payload));
    ASSERT_EQ(file_read(&fs, (uint32_t)ino, off_1gb, read_payload, len), (ssize_t)len);
    ASSERT_STR_EQ(payload, read_payload);

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test5_shrink_then_grow_no_data_resurrection) {
    cleanup_test_files();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* Write 8192 bytes of 0xFF */
    uint8_t orig[8192];
    memset(orig, 0xFF, sizeof(orig));
    ASSERT_EQ(file_write(&fs, (uint32_t)ino, 0, orig, sizeof(orig)), (ssize_t)sizeof(orig));

    /* Shrink to 2000 bytes (tail of block 0 should be zeroed, block 1 freed) */
    ASSERT_OK(file_truncate(&fs, (uint32_t)ino, 2000));

    /* Grow back to 8192 bytes */
    ASSERT_OK(file_truncate(&fs, (uint32_t)ino, 8192));

    /* Read all 8192 bytes: bytes 0..1999 are 0xFF, bytes 2000..8191 MUST be zeros! */
    uint8_t readback[8192];
    ASSERT_EQ(file_read(&fs, (uint32_t)ino, 0, readback, sizeof(readback)), (ssize_t)sizeof(readback));

    for (size_t i = 0; i < 2000; i++) {
        ASSERT_EQ(readback[i], 0xFF);
    }
    for (size_t i = 2000; i < 8192; i++) {
        ASSERT_EQ(readback[i], 0);
    }

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test6_truncate_to_zero_frees_all_blocks) {
    cleanup_test_files();
    blkdev_t dev;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, 1024));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    uint32_t baseline_used = bm_count_used(&fs.bbitmap);

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* Write 100 KB across direct and indirect */
    uint8_t buf[4096];
    memset(buf, 0x55, sizeof(buf));
    for (int i = 0; i < 25; i++) {
        ASSERT_EQ(file_write(&fs, (uint32_t)ino, (uint64_t)i * 4096, buf, sizeof(buf)), (ssize_t)sizeof(buf));
    }

    ASSERT_TRUE(bm_count_used(&fs.bbitmap) > baseline_used);

    /* Truncate to 0 */
    ASSERT_OK(file_truncate(&fs, (uint32_t)ino, 0));

    inode_t node;
    ASSERT_OK(inode_read(&fs, (uint32_t)ino, &node));
    ASSERT_EQ(node.size, 0);
    ASSERT_EQ(node.blocks, 0);
    ASSERT_EQ(bm_count_used(&fs.bbitmap), baseline_used);

    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test7_fill_disk_partial_write_then_enospc) {
    cleanup_test_files();
    blkdev_t dev;
    /* Very small disk with only 64 data blocks */
    uint32_t total = 128;
    ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, total));
    ASSERT_OK(sb_format(&dev));

    fs_t fs;
    ASSERT_OK(fs_init_allocators(&fs, &dev));

    int ino = inode_create(&fs, 0644, 0, 0);
    ASSERT_OK(ino);

    /* Try to write 1 MB (256 blocks) on a disk with fewer blocks */
    size_t big_sz = 256 * BLOCK_SIZE;
    uint8_t *big_buf = (uint8_t *)th_calloc(1, big_sz);
    ASSERT_TRUE(big_buf != NULL);

    ssize_t written = file_write(&fs, (uint32_t)ino, 0, big_buf, big_sz);
    /* Should return partial bytes written */
    ASSERT_TRUE(written > 0);
    ASSERT_TRUE(written < (ssize_t)big_sz);

    /* Next write should return -ENOSPC */
    ASSERT_ERR(file_write(&fs, (uint32_t)ino, (uint64_t)written, big_buf, 4096), -ENOSPC);

    /* Check consistency: popcount matches total - free */
    uint32_t used = bm_count_used(&fs.bbitmap);
    ASSERT_EQ(fs.sb.free_blocks, fs.sb.total_blocks - used);

    th_free(big_buf);
    blk_close(&dev);
    cleanup_test_files();
}

TEST(m5_test8_randomized_differential_model_3_seeds) {
    unsigned int seeds[] = {42, 1337, 2026};

    for (size_t s_idx = 0; s_idx < sizeof(seeds)/sizeof(seeds[0]); s_idx++) {
        cleanup_test_files();
        blkdev_t dev;
        ASSERT_OK(blk_open(&dev, TEST_FIO_IMG, 1, 2048));
        ASSERT_OK(sb_format(&dev));

        fs_t fs;
        ASSERT_OK(fs_init_allocators(&fs, &dev));

        int ino = inode_create(&fs, 0644, 0, 0);
        ASSERT_OK(ino);

        /* In-memory byte array model of up to 512 KB */
        size_t max_model_sz = 512 * 1024;
        uint8_t *model = (uint8_t *)th_calloc(1, max_model_sz);
        size_t model_size = 0;
        ASSERT_TRUE(model != NULL);

        srand(seeds[s_idx]);

        for (int op = 0; op < 50000; op++) {
            int action = rand() % 3;

            if (action == 0) {
                /* Write */
                size_t off = (size_t)(rand() % (int)(max_model_sz - 4096));
                size_t len = 1 + (size_t)(rand() % 4096);
                uint8_t pattern = (uint8_t)(rand() & 0xFF);

                uint8_t buf[4096];
                memset(buf, pattern, len);

                ssize_t res = file_write(&fs, (uint32_t)ino, off, buf, len);
                ASSERT_EQ(res, (ssize_t)len);

                /* Update model */
                if (off > model_size) {
                    /* Zero gap */
                    memset(model + model_size, 0, off - model_size);
                }
                memcpy(model + off, buf, len);
                if (off + len > model_size) {
                    model_size = off + len;
                }
            } else if (action == 1) {
                /* Read */
                if (model_size == 0) continue;
                size_t off = (size_t)(rand() % (int)model_size);
                size_t len = 1 + (size_t)(rand() % 4096);

                uint8_t actual[4096];
                ssize_t read_bytes = file_read(&fs, (uint32_t)ino, off, actual, len);

                size_t expected_len = (off + len > model_size) ? (model_size - off) : len;
                ASSERT_EQ(read_bytes, (ssize_t)expected_len);
                ASSERT_MEM_EQ(actual, model + off, expected_len);
            } else {
                /* Truncate */
                size_t new_sz = (size_t)(rand() % (int)(max_model_sz / 2));
                ASSERT_OK(file_truncate(&fs, (uint32_t)ino, new_sz));

                if (new_sz < model_size) {
                    memset(model + new_sz, 0, model_size - new_sz);
                }
                model_size = new_sz;
            }
        }

        /* Final full read comparison */
        uint8_t *final_read = (uint8_t *)th_malloc(model_size);
        ASSERT_TRUE(final_read != NULL);
        ASSERT_EQ(file_read(&fs, (uint32_t)ino, 0, final_read, model_size), (ssize_t)model_size);
        ASSERT_MEM_EQ(final_read, model, model_size);

        th_free(final_read);
        th_free(model);
        blk_close(&dev);
        cleanup_test_files();
    }
}
