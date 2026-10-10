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

## [M1] - 2026-10-10
### Added
- Block device abstraction in `include/blk.h` and `src/blk.c`.
- Fixed 4096-byte block read/write operations via `pread`/`pwrite` with short read/write and `EINTR` retry loops.
- Out-of-bounds validation (`blockno >= nblocks` returning `-EINVAL`).
- Deterministic fault injection framework: `FAULT_DROP` (silent write drops simulating power cut) and `FAULT_TORN` (2048-byte torn writes).
- Per-device metrics tracking (`reads`, `writes`, `bytes_read`, `bytes_written`, `syncs`).
- Utility `tools/mkdisk.c` for creating sparse or sized disk images with human-readable suffixes (`1M`, `16M`, `1G`).
- Comprehensive unit test suite `tests/test_blk.c` covering 1 MB pattern verification, 10,000 random operations against a shadow model, bounds checking, persistence, fault injection, and metrics counters.
- Passed gate M1 with report `reports/module_1.md`.
