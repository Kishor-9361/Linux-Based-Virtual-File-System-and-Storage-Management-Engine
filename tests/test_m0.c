#include "test_harness.h"
#include "common.h"
#include "log.h"

TEST(harness_basic_assertions) {
    ASSERT_TRUE(1 == 1);
    ASSERT_FALSE(1 == 2);
    ASSERT_EQ(42, 42);
    ASSERT_NE(42, 99);
    ASSERT_OK(0);
    ASSERT_OK(100);
    ASSERT_ERR(-ENOENT, -ENOENT);
    ASSERT_ERR(-EINVAL, -EINVAL);

    char buf1[] = "minifs";
    char buf2[] = "minifs";
    ASSERT_MEM_EQ(buf1, buf2, 6);
    ASSERT_STR_EQ("hello", "hello");
}

TEST(constants_sanity) {
    ASSERT_EQ(BLOCK_SIZE, 4096);
    ASSERT_EQ(INODE_SIZE, 128);
    ASSERT_EQ(DIRENT_SIZE, 64);
    ASSERT_EQ(MINIFS_MAGIC, 0x4D494E49);
}

TEST(logging_sanity) {
    LOG_INFO("M0 scaffold test: logging working");
    LOG_DEBUG("Debug message (visible if MINIFS_LOG=DEBUG)");
    ASSERT_TRUE(true);
}

TEST(memory_leak_tracker_clean) {
    void *ptr = th_malloc(128);
    ASSERT_TRUE(ptr != NULL);
    memset(ptr, 0xAA, 128);
    th_free(ptr);
}
