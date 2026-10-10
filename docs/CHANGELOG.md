# Changelog: minifs

## [M0] - 2026-10-10
### Added
- Project scaffold, directory layout (`include/`, `src/`, `tools/`, `tests/`, `scripts/`, `reports/`, `docs/`).
- Header `include/common.h` with core constants: `BLOCK_SIZE=4096`, `INODE_SIZE=128`, `DIRENT_SIZE=64`, `MINIFS_MAGIC=0x4D494E49`.
- Logging subsystem in `include/log.h` with `MINIFS_LOG` dynamic log levels.
- Test harness `tests/test_harness.h` with assertions (`ASSERT_EQ`, `ASSERT_OK`, `ASSERT_ERR`, `ASSERT_MEM_EQ`, `ASSERT_STR_EQ`), test auto-registration, and built-in heap leak tracking.
- Test suite `tests/test_m0.c` validating harness, constants, logging, and leak tracking.
- Makefile supporting `all`, `test`, `test-asan`, `test-valgrind`, `gate`, `bench`, `coverage`, and `clean`.
- Gate runner `scripts/run_gate.sh` with deliberate failure verification, automated markdown report generation, and exit status validation.
- Passed gate M0 with report `reports/module_0.md`.
