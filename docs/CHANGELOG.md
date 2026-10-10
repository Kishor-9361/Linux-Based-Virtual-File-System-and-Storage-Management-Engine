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

## [M2] - 2026-10-10
### Added
- Superblock definitions in `include/sb.h` with packed 256-byte layout and static size assertion.
- Hardware-compliant IEEE 802.3 CRC32 checksum engine for on-disk superblock verification.
- Layout computation (`sb_compute_layout`) calculating contiguous, non-overlapping regions for journal, inode bitmap, block bitmap, inode table, and data blocks.
- On-disk formatting engine (`sb_format`) that clears metadata areas, initializes root inode reservation, marks metadata blocks as allocated in block bitmap, and commits superblock.
- Superblock loading and validation (`sb_load`) checking magic (`0x4D494E49`), version, block size, CRC32, and geometry bounds.
- CLI formatting utility `tools/mkfs_minifs.c`.
- Comprehensive test suite `tests/test_sb.c` testing geometry from 256 KB to 1 GB, round-trip serialization, bit-flip CRC corruption detection, invalid magic handling, bitmap allocation tracking, and reformatting idempotence.
- Gate report `reports/module_2.md` with block 0 superblock hexdump.
- Passed gate M2.

## [M3] - 2026-10-10
### Added
- Unified block I/O routing wrappers `fs_read_block()` and `fs_write_block()` in `src/bitmap.c` / `include/fs.h`.
- Low-level bitmap engine in `include/bitmap.h` and `src/bitmap.c` supporting test, set, clear, run detection (`bm_find_run`), and popcount scanning (`bm_count_used`).
- Fast 64-bit word scanning with `__builtin_ctzll` and `__builtin_popcountll` for high-throughput space management.
- Double-allocation detection returning `-EEXIST` and double-free detection returning `-EINVAL`.
- Next-fit block allocator (`alloc_block`) with spatial locality starting at `goal`, wrap-around searching, search step counters, adjacent placement tracking, and zero-on-allocate semantics.
- Inode allocator (`alloc_inode`) and deallocator (`free_inode`) updating superblock free counts atomically.
- Comprehensive test suite `tests/test_bitmap.c` covering full disk exhaustion to `-ENOSPC`, double free/set detection, 100,000-op randomized differential shadow model, popcount synchronization, contiguous run finding, goal-based contiguity, boundary bit handling (7, 8, 63, 64, 32767, 32768), and cross-session persistence.
- Passed gate M3 with report `reports/module_3.md`.
