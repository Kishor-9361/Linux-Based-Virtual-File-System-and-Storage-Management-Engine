#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <errno.h>

#define MAX_TESTS 256

typedef void (*test_fn_t)(void);

typedef struct {
    const char *name;
    test_fn_t fn;
} test_case_t;

static test_case_t g_tests[MAX_TESTS];
static int g_test_count = 0;
static int g_current_test_failed = 0;
static int g_failed_tests = 0;
static const char *g_current_test_name = NULL;

/* Leak tracking */
static size_t g_allocated_bytes = 0;
static size_t g_allocated_blocks = 0;

static inline void *th_malloc(size_t sz) {
    if (sz == 0) return NULL;
    size_t *p = (size_t *)malloc(sz + sizeof(size_t));
    if (!p) return NULL;
    *p = sz;
    g_allocated_bytes += sz;
    g_allocated_blocks++;
    return (void *)(p + 1);
}

static inline void *th_calloc(size_t nmemb, size_t sz) {
    size_t total = nmemb * sz;
    void *ptr = th_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

static inline void *th_realloc(void *ptr, size_t sz) {
    if (!ptr) return th_malloc(sz);
    if (sz == 0) {
        /* free */
        size_t *orig = ((size_t *)ptr) - 1;
        g_allocated_bytes -= *orig;
        g_allocated_blocks--;
        free(orig);
        return NULL;
    }
    size_t *orig = ((size_t *)ptr) - 1;
    size_t old_sz = *orig;
    size_t *p = (size_t *)realloc(orig, sz + sizeof(size_t));
    if (!p) return NULL;
    *p = sz;
    g_allocated_bytes = g_allocated_bytes - old_sz + sz;
    return (void *)(p + 1);
}

static inline void th_free(void *ptr) {
    if (!ptr) return;
    size_t *p = ((size_t *)ptr) - 1;
    g_allocated_bytes -= *p;
    g_allocated_blocks--;
    free(p);
}

static inline void test_register(const char *name, test_fn_t fn) {
    if (g_test_count < MAX_TESTS) {
        g_tests[g_test_count].name = name;
        g_tests[g_test_count].fn = fn;
        g_test_count++;
    } else {
        fprintf(stderr, "Exceeded MAX_TESTS\n");
        exit(1);
    }
}

#define TEST(name) \
    static void test_func_##name(void); \
    __attribute__((constructor)) static void test_reg_##name(void) { \
        test_register(#name, test_func_##name); \
    } \
    static void test_func_##name(void)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    int64_t _va = (int64_t)(a); \
    int64_t _vb = (int64_t)(b); \
    if (_va != _vb) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s (%" PRId64 ") != %s (%" PRId64 ")\n", \
                __FILE__, __LINE__, #a, (int64_t)_va, #b, (int64_t)_vb); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_NE(a, b) do { \
    int64_t _va = (int64_t)(a); \
    int64_t _vb = (int64_t)(b); \
    if (_va == _vb) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s (%" PRId64 ") == %s (%" PRId64 ")\n", \
                __FILE__, __LINE__, #a, (int64_t)_va, #b, (int64_t)_vb); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_OK(x) do { \
    int64_t _res = (int64_t)(x); \
    if (_res < 0) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s returned %" PRId64 " (expected >= 0, errno %s)\n", \
                __FILE__, __LINE__, #x, _res, strerror((int)-_res)); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_ERR(x, expected_neg_errno) do { \
    int64_t _res = (int64_t)(x); \
    int64_t _exp = (int64_t)(expected_neg_errno); \
    if (_res != _exp) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s returned %" PRId64 " (expected %" PRId64 ")\n", \
                __FILE__, __LINE__, #x, _res, _exp); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_MEM_EQ(a, b, size) do { \
    size_t _sz = (size_t)(size); \
    if (memcmp(a, b, _sz) != 0) { \
        fprintf(stderr, "  [FAIL] %s:%d: Memory mismatch between %s and %s (%zu bytes)\n", \
                __FILE__, __LINE__, #a, #b, _sz); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char *_sa = (const char *)(a); \
    const char *_sb = (const char *)(b); \
    if ((_sa == NULL && _sb != NULL) || (_sa != NULL && _sb == NULL) || \
        (_sa && _sb && strcmp(_sa, _sb) != 0)) { \
        fprintf(stderr, "  [FAIL] %s:%d: String mismatch: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, _sa ? _sa : "(null)", _sb ? _sb : "(null)"); \
        g_current_test_failed = 1; \
        return; \
    } \
} while(0)

static inline int test_harness_run_all(void) {
    printf("================ Running %d tests ================\n", g_test_count);
    g_failed_tests = 0;
    int passed = 0;

    for (int i = 0; i < g_test_count; i++) {
        g_current_test_name = g_tests[i].name;
        g_current_test_failed = 0;
        size_t initial_bytes = g_allocated_bytes;
        size_t initial_blocks = g_allocated_blocks;

        g_tests[i].fn();

        if (g_allocated_bytes != initial_bytes || g_allocated_blocks != initial_blocks) {
            fprintf(stderr, "  [LEAK] Memory leak detected in test %s: %zu unfreed bytes (%zu blocks)\n",
                    g_current_test_name, g_allocated_bytes - initial_bytes, g_allocated_blocks - initial_blocks);
            g_current_test_failed = 1;
        }

        if (g_current_test_failed) {
            printf("[FAIL] %s\n", g_tests[i].name);
            g_failed_tests++;
        } else {
            printf("[PASS] %s\n", g_tests[i].name);
            passed++;
        }
    }

    printf("================ Summary: %d passed, %d failed ================\n", passed, g_failed_tests);
    return (g_failed_tests == 0) ? 0 : 1;
}

#ifndef NO_TEST_MAIN
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return test_harness_run_all();
}
#endif

#endif /* TEST_HARNESS_H */
