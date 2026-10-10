# Gate Report: Module 11 (`fsck` Consistency Checker & Repair Tool)

- **Date**: 2026-10-10 11:01:51Z
- **Module ID**: M11
- **Status**: PASSED

## 1. Objective
Implement the comprehensive 5-pass consistency checker and repair engine for `minifs`:
- **Pass 1: Superblock & Geometry Validation**: Magic, version, block size, CRC32 checksum, layout geometry (`itable_start`, `bbitmap_start`, `ibitmap_start`, `data_start`).
- **Pass 2: Inode Tree Scan & Bounds Validation**: Inode mode/type verification (`S_IFREG`, `S_IFDIR`), direct block pointers, single indirect block tables, double indirect block tables (L1 & L2), out-of-range pointer clamping/zeroing.
- **Pass 3: Block Ownership & Duplicate Block Resolution**: Inode-to-block ownership tracking (`owner[blk]`), duplicate block detection, automatic block cloning for duplicate claimants in `--repair` mode.
- **Pass 4: Namespace Walk & Directory Integrity (DFS)**: Directory traversal from root inode (1), verification of `.` and `..` parentage, detection and removal of directory loops/cycles, dangling dentry pruning (dentries pointing to unallocated inodes).
- **Pass 5: Cross-Checks & Free Space Reconciliation**:
  - Leaked blocks (marked allocated in bitmap, unreferenced by any inode) $\to$ clear bit.
  - Missing allocations (claimed by inode, clear in bitmap) $\to$ set bit.
  - Orphan inodes (allocated in inode bitmap, unreachable from namespace) $\to$ free if empty or attach to `/lost+found` as `#<ino>`.
  - Link count verification $\to$ restore `inode.nlink` to counted references.
  - Superblock free counts $\to$ recompute total free blocks and free inodes, commit clean superblock.
- Standalone CLI utility (`tools/fsck_minifs.c`) supporting `--repair` and `--verbose` modes.

## 2. Test Execution Summary
- **Unit & Regression Tests**: 13/13 Test Suites PASSED (73/73 unit & differential tests)
  - `test_bitmap`: 7/7 PASSED
  - `test_blk`: 6/6 PASSED
  - `test_cache`: 7/7 PASSED (7.08x speedup, 141.2x physical I/O reduction)
  - `test_dir`: 8/8 PASSED
  - `test_fileio`: 8/8 PASSED
  - `test_fsck`: 8/8 PASSED
  - `test_inode`: 7/7 PASSED
  - `test_journal`: 7/7 PASSED (including 174/174 WAL crash sweep tests)
  - `test_m0`: 4/4 PASSED
  - `test_perm`: 6/6 PASSED
  - `test_sb`: 6/6 PASSED
  - `test_vfs`: 2/2 PASSED
  - `diff_posix`: 1/1 PASSED (Differential POSIX across all 3 seeds)
- **ASan / UBSan**: Built-in heap tracker active
- **Valgrind / Leak Tracker**: Built-in heap leak tracker: 0 memory leaks across all test suites

## 3. Corruption Matrix & Repair Verification
All corruption scenarios programmatically tested in [`tests/test_fsck.c`](file:///home/kishorveeraragavan/Desktop/VFS&SM/tests/test_fsck.c):

| Test Case | Injected Corruption | Check-Only Detection | Repair Mode Action | Idempotence (2nd Run) |
| :--- | :--- | :---: | :---: | :---: |
| `m11_test1` | Clean populated image | 0 errors detected | No-op | Clean (0 errors) |
| `m11_test2` | Clear allocated bit + set free bit | Leaked & missing alloc detected | Bitmap bits reconciled | Clean (0 errors) |
| `m11_test3` | Corrupt inode `nlink` ($\pm 5$) | Link count mismatch detected | `nlink` set to actual refcount | Clean (0 errors) |
| `m11_test4` | Direct pointer beyond disk bounds | Bad pointer detected | Pointer zeroed | Clean (0 errors) |
| `m11_test5` | Two inodes pointing to same block | Duplicate block detected | Block cloned to new allocation | Clean (0 errors) |
| `m11_test6` | Unreachable allocated inode with data | Orphan inode detected | Rescued to root namespace | Clean (0 errors) |
| `m11_test7` | Corrupted `.` in subdirectory | Missing `.` detected | Reconstructed `.` pointing to self | Clean (0 errors) |
| `m11_test8` | Corrupted `sb.free_blocks` count | Superblock error detected | Free counts recomputed & synced | Clean (0 errors) |

- **Read-Only Invariance**: Verified that in check-only mode (`repair = false`), image content is byte-identical before and after the check.
- **Idempotence**: Every test verified that running `fsck` a second time after `--repair` reports **0 errors**.

## 4. Bugs Found & Resolved
1. **Inode Table Indexing Offset**:
   - Initial `read_inode_raw` and `write_inode_raw` used `(ino * INODE_SIZE) / BLOCK_SIZE` instead of the 1-indexed formula `((ino - 1) / 32)` and `((ino - 1) % 32) * sizeof(inode_t)`.
   - **Resolution**: Aligned inode disk address calculations with `src/inode.c`, eliminating artificial inode mismatches.
2. **Standalone Formatting Root Bootstrap in `mkfs_minifs`**:
   - `tools/mkfs_minifs.c` formatted the superblock but had not bootstrapped the root directory inode (1), causing fresh mkfs images to appear un-initialized.
   - **Resolution**: Updated `mkfs_minifs.c` to call `dir_bootstrap_root(&fs)` upon format, producing fully consistent filesystems out-of-the-box.

