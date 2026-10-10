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

## [M4] - 2026-10-10
### Added
- 128-byte packed inode structure `inode_t` with compile-time `_Static_assert` in `include/inode.h`.
- Inode table serialization (`inode_read`, `inode_write`) with block clustering (32 inodes per 4 KB block).
- Inode lifecycle management (`inode_create`, `inode_destroy`) initializing mode, uid, gid, nlink, and timestamps.
- Multi-tier logical-to-physical block translator `bmap`:
  - 12 direct block pointers (0 .. 11)
  - Single indirect block with 1024 pointers (12 .. 1035)
  - Double indirect block with $1024 \times 1024$ pointers (1036 .. 1049611) supporting files up to ~4 GiB.
  - Zero-on-allocate semantics and sparse hole query support.
  - Out-of-range protection returning `-EFBIG`.
- Block tree deallocation engine `inode_free_blocks_from` clearing data and indirect blocks while reclaiming empty pointer tables.
- Comprehensive unit test suite `tests/test_inode.c` covering 1000 random inode roundtrips, boundary inodes (1, 32, 33, last), inode exhaustion and restoration, boundary `bmap` tests, 5000 consecutive block mapping with uniqueness verification, sparse holes, and block tree leak freedom.
- Passed gate M4 with report `reports/module_4.md`.

## [M5] - 2026-10-10
### Added
- Arbitrary byte-offset file I/O operations in `include/fileio.h` and `src/fileio.c`: `file_read`, `file_write`, and `file_truncate`.
- Partial block read-modify-write optimization skipping reads during full 4096-byte aligned writes.
- Partial write semantics on `-ENOSPC` returning bytes written so far.
- Sparse file write and read handling (unmapped blocks read back as zeroes without allocating physical storage).
- Truncate shrink with tail zeroing of the last partial block preventing old data exposure on subsequent expansions.
- Comprehensive unit test suite `tests/test_fileio.c` covering write/read across boundary sizes (1, 4095, 4096, 4097, 8192 bytes), unaligned offsets crossing direct and indirect tiers, 100 MB write verified against host `sha256sum`, sparse write at 1 GB offset, shrink/grow zeroing, disk full partial write resilience, and 50,000-op randomized differential model test across 3 separate PRNG seeds (42, 1337, 2026).
- Passed gate M5 with report `reports/module_5.md`.

## [M6] - 2026-10-10
### Added
- 64-byte packed directory record `dirent_t` with compile-time `_Static_assert` in `include/dir.h`.
- Hierarchical directory subsystem in `src/dir.c`: `dir_init`, `dir_bootstrap_root`, `dir_lookup`, `dir_add`, `dir_remove`, `dir_list`, `dir_is_empty`, and `path_resolve`.
- Block-buffered dirent scanning (64 dirents per read) providing high performance across large namespaces.
- Slot recycling mechanism: freed slots (`ino == 0`) are reused by subsequent `dir_add` calls without unbounded directory file growth.
- Hard link maintenance: directory creation maintains `nlink = 2` for self and increments parent `nlink` for `..`.
- Full path resolver (`path_resolve`) handling absolute and relative paths, collapsing duplicate slashes (`//`), resolving `.` and `..` up to 40 levels deep, with root's `..` anchored at root.
- Comprehensive unit test suite `tests/test_dir.c` covering 25-level deep directory nesting, 10,000 entries stress test with 5,000 deletions and slot-reusing re-adds, duplicate rejection, slash collapsing, regular file path blockage (`-ENOTDIR`), emptiness validation, link count verification, and directory iteration.
- Baseline lookup latency scaling benchmark saved to `reports/dir_lookup_baseline.csv`.
- Passed gate M6 with report `reports/module_6.md`.

## [M7] - 2026-10-10
### Added
- Complete POSIX Virtual File System (VFS) interface in `include/vfs.h` and `src/vfs.c`:
  - System-wide open-file table (256 slots) and per-process file-descriptor table (fd >= 3).
  - `vfs_mount` and `vfs_unmount` maintaining superblock mount counts and clean/dirty states.
  - `vfs_open`, `vfs_close`, `vfs_read`, `vfs_write`, `vfs_lseek` supporting `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`.
  - Directory management: `vfs_mkdir`, `vfs_rmdir` enforcing non-empty validation (`-ENOTEMPTY`).
  - File operations: `vfs_unlink` with open-file orphan deferral (data persists while open and reclaims on final close).
  - Atomic rename: `vfs_rename` with ancestor-cycle prevention (`-EINVAL`) and directory link count tracking.
  - Hard links (`vfs_link`) with link count limits up to 65535 (`-EMLINK`).
  - Metadata inspection: `vfs_stat`, `vfs_readdir`, `vfs_chdir`, and `vfs_getcwd`.
- Comprehensive unit test suite `tests/test_vfs.c` validating POSIX semantic cases, orphan retention, rename over existing files, ancestor loops, and cross-mount persistence.
- Differential test harness `tests/diff_posix.c` testing 5,000 randomized operations against host Linux ext4/tmpfs across 3 separate seeds (101, 202, 303) with 0 divergences.
- Passed gate M7 with report `reports/module_7.md`.

## [0.9.0] - Phase C: Module 8 - Permissions, DAC, Ownership & Access Enforcement
- Implemented Discretionary Access Control (DAC) in `include/perm.h` and `src/perm.c`:
  - `perm_check()` validating standard POSIX permission precedence (root bypass, owner match, group match, other).
  - Mode bit operators (`perm_mode_str`, `perm_apply_umask`).
  - Strict ownership rules for `vfs_chmod` and `vfs_chown` (root or owner for chmod; root only for chown).
- Integrated traversal (`X_OK`) and modification (`W_OK | X_OK`) permission checks across all VFS path-based entry points (`vfs_open`, `vfs_mkdir`, `vfs_rmdir`, `vfs_unlink`, `vfs_rename`, `vfs_link`, `vfs_stat`, `vfs_chmod`, `vfs_chown`, `vfs_opendir`).
- Validated all 28,672 permutations in `tests/test_perm.c` across all user classes, requested rights, and mode combinations ($2 \times 2 \times 2 \times 8 \times 8 \times 8 \times 8$).
- Verified directory traversal denial without `X_OK`, creation/deletion denial without `W_OK`, umask masking on inode creation, and permission persistence across mount cycles.
- Passed gate M8 with report `reports/module_8.md`.

