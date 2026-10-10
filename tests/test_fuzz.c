#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "test_harness.h"
#include <stdlib.h>
#include <unistd.h>

#define FUZZ_BIN "build/bin/fuzz"

TEST(m14_test4_model_differential_fuzzing_5_seeds) {
    unsigned int test_seeds[] = {111, 222, 333, 444, 555};
    size_t num_seeds = sizeof(test_seeds) / sizeof(test_seeds[0]);

    for (size_t s = 0; s < num_seeds; s++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "%s --seed %u --ops 2000 >/dev/null 2>&1",
                 FUZZ_BIN, test_seeds[s]);
        int rc = system(cmd);
        ASSERT_EQ(rc, 0);
    }
}

TEST(m14_test5_fuzz_replay_until_checkpoint) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s --seed 999 --ops 1000 --until 500 >/dev/null 2>&1",
             FUZZ_BIN);
    int rc = system(cmd);
    ASSERT_EQ(rc, 0);
}
