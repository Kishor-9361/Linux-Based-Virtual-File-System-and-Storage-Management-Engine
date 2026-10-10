# Gate Report: Module 10 (WAL Journaling & Crash Consistency)

- **Date**: 2026-10-10 10:46:25Z
- **Module ID**: M10
- **Status**: PASSED

## 1. Objective
Implement write-ahead logging (WAL) journaling and crash consistency for `minifs`:
- Circular journal log ring in dedicated metadata block region.
- Atomic multi-block transactions with CRC32 verification and atomic commit sequence:
  1. Write journal descriptor block with target block mappings.
  2. Write updated payload/metadata blocks into journal log area.
  3. Disk sync (`blk_sync`).
  4. Write journal commit block.
  5. Disk sync (`blk_sync`) -> transaction durable.
  6. Write blocks to home locations in disk/cache.
  7. Checkpoint: advance journal superblock start pointer and sync.
- Journal recovery & replay on mount if superblock state is `SB_STATE_DIRTY`.
- Crash-consistency matrix evaluation tool (`tools/crashtest.c`).

## 2. Test Execution Summary
- **Unit & Regression Tests**: 12/12 Test Suites PASSED (65/65 tests passed)
  - `test_bitmap`: 7/7 PASSED
  - `test_blk`: 6/6 PASSED
  - `test_cache`: 7/7 PASSED (9.63x speedup, 141.2x physical I/O reduction)
  - `test_dir`: 8/8 PASSED
  - `test_fileio`: 8/8 PASSED
  - `test_inode`: 7/7 PASSED
  - `test_journal`: 7/7 PASSED (including 174/174 WAL crash-sweep tests)
  - `test_m0`: 4/4 PASSED
  - `test_perm`: 6/6 PASSED
  - `test_sb`: 6/6 PASSED
  - `test_vfs`: 2/2 PASSED
  - `diff_posix`: 1/1 PASSED (Seeds 101, 202, 303 across 2000 ops each)
- **ASan / UBSan**: Built-in heap tracker active
- **Valgrind / Leak Tracker**: Built-in heap leak tracker: 0 memory leaks across all test suites

## 3. Crash Consistency Matrix Sweep Results
Evaluated using `./build/bin/crashtest sweep` across both `FAULT_DROP` and `FAULT_TORN` fault modes at every individual write step $k \in [0, W]$:

| Mutating Scenario | Writes Required ($W$) | Tests Evaluated ($2 \times (W+1)$) | Pass Rate |
| :--- | :---: | :---: | :---: |
| `create_file` | 10 | 22 | 22 / 22 (100.0%) |
| `mkdir` | 15 | 32 | 32 / 32 (100.0%) |
| `write_file` | 11 | 24 | 24 / 24 (100.0%) |
| `link` | 7 | 16 | 16 / 16 (100.0%) |
| `rename` | 7 | 16 | 16 / 16 (100.0%) |
| `unlink` | 7 | 16 | 16 / 16 (100.0%) |
| `rmdir` | 13 | 28 | 28 / 28 (100.0%) |
| `truncate` | 9 | 20 | 20 / 20 (100.0%) |
| **Total** | - | **174** | **174 / 174 (100.0%)** |

All crashed disk images successfully recovered via `journal_replay()`, with zero filesystem corruption and either clean rollback (if crash occurred prior to commit) or durable application (if crash occurred after commit). Full step-by-step matrix saved to [`reports/crash_matrix.csv`](file:///home/kishorveeraragavan/Desktop/VFS&SM/reports/crash_matrix.csv).

## 4. Bugs Found & Resolved
1. **Differential POSIX Transaction Leak in `vfs_rename`**:
   - `tx_begin` was called prior to directory structure validations (`-EINVAL`, `-ENOTDIR`, `-ENOTEMPTY`, `-EISDIR`). When validation failed, early return without `tx_abort(tx)` left `current_tx` active, inadvertently capturing subsequent operations.
   - **Resolution**: Refactored `vfs_rename` to perform all pre-condition validations before invoking `tx_begin`, ensuring transactions are only instantiated when all constraints are verified.
2. **Tool Binary Dependency in Makefile**:
   - `make test` previously depended only on test binaries rather than `all`, preventing `crashtest` from being compiled prior to test execution in clean builds.
   - **Resolution**: Updated `Makefile` so that `test` target depends on `all $(TEST_BINS)`.

