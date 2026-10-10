# Execution Plan: Linux-Based Virtual File System & Storage Management Engine (C)

Project name used below: **`minifs`**. Language: C11 on Linux. Storage: a regular file used as a disk image.

---

## 0. How to use this document

- This is the single source of truth for the implementing agent.
- Work **strictly in order**: M0 → M14. A module is "done" only when its **gate** passes.
- Every module has: goal, files, data structures, API, core logic, edge cases, tests, gate, and report output.
- Never weaken or skip a test to pass a gate. Fix the code.

---

## 1. Global conventions (apply to all modules)

### 1.1 Build and quality rules
| Rule | Detail |
|---|---|
| Compiler flags | `-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -g -O0` (tests), `-O2` (benchmarks) |
| Sanitizers | Every test run under `-fsanitize=address,undefined` **and** Valgrind (`--leak-check=full --error-exitcode=1`) |
| Error handling | Functions return `0`/positive on success, **negative errno** on failure (`-ENOENT`, `-ENOSPC`, `-EEXIST`, `-EINVAL`, `-EIO`, `-EACCES`, `-ENOTDIR`, `-EISDIR`, `-ENOTEMPTY`, `-EBADF`). No `exit()`/`abort()` in library code. |
| On-disk structs | `__attribute__((packed))`, fixed-width ints (`uint32_t`), little-endian only (assert at compile time with `_Static_assert` on struct sizes) |
| Constants | `BLOCK_SIZE=4096`, `INODE_SIZE=128`, `DIRENT_SIZE=64`, `MINIFS_MAGIC=0x4D494E49` |
| Git | One commit per passed gate; tag `m<N>-pass` |
| Reports | After each gate: `reports/module_<N>.md` (objective, design, tests pass/fail, bugs found, metrics) |
| Changelog | Append to `docs/CHANGELOG.md` |

### 1.2 Directory layout
```
minifs/
├── Makefile
├── include/        # public headers: blk.h sb.h bitmap.h inode.h fileio.h dir.h vfs.h perm.h cache.h journal.h fsck.h metrics.h
├── src/            # implementations (one .c per header) + main.c (CLI)
├── tools/          # mkdisk.c, mkfs_minifs.c, crashtest.c, fuzz.c
├── tests/          # test_<module>.c + test_harness.h
├── scripts/        # run_gate.sh, bench.sh, e2e.sh, coverage.sh
├── reports/        # module_N.md, final_report.md, bench CSVs, charts
└── docs/           # CHANGELOG.md, DESIGN.md, ondisk_layout.md
```

### 1.3 Test harness (built in M0)
- `TEST(name)` macro registers a test; `ASSERT_EQ(a,b)`, `ASSERT_OK(x)` (x >= 0), `ASSERT_ERR(x, -EXXX)`, `ASSERT_MEM_EQ`.
- Runner prints `PASS/FAIL name`, returns non-zero on any failure.
- `scripts/run_gate.sh N` does: build → run `test_<mod>` → ASan/UBSan run → Valgrind run → write `reports/module_N.md` → exit non-zero if anything fails.

### 1.4 On-disk layout (fixed design)
```
Block 0              : Superblock
Block 1 .. J         : Journal  (1 journal-superblock + JOURNAL_BLOCKS)
Next B_i blocks      : Inode bitmap
Next B_b blocks      : Block bitmap
Next I_t blocks      : Inode table  (32 inodes per 4 KB block)
Remaining            : Data blocks
```
**Layout calculation (mkfs):**
```
total_blocks   = image_size / 4096
journal_blocks = clamp(total_blocks / 64, 64, 4096)         // ~1.5%
inode_count    = total_blocks / 4  (min 128), rounded up to multiple of 32
inode_tbl_blks = inode_count / 32
ibm_blks       = ceil(inode_count / (4096*8))
bbm_blks       = ceil(total_blocks / (4096*8))
data_start     = 1 + journal_blocks + ibm_blks + bbm_blks + inode_tbl_blks
```
All region start offsets are stored in the superblock. Block numbers everywhere are **absolute** (0 = invalid/null pointer, since block 0 is the superblock and never data).

### 1.5 Core data structures

```c
typedef struct __attribute__((packed)) {
    uint32_t magic, version, block_size;
    uint32_t total_blocks, total_inodes;
    uint32_t free_blocks, free_inodes;
    uint32_t journal_start, journal_blocks;
    uint32_t ibitmap_start, ibitmap_blocks;
    uint32_t bbitmap_start, bbitmap_blocks;
    uint32_t itable_start, itable_blocks;
    uint32_t data_start, root_inode;      // root_inode = 1
    uint32_t state;                       // 0 = clean, 1 = mounted/dirty
    uint32_t mount_count;
    uint64_t last_mount_time;
    uint8_t  reserved[...];               // pad to 256 bytes
    uint32_t crc32;                       // over all preceding bytes
} superblock_t;

typedef struct __attribute__((packed)) {  // 128 bytes
    uint16_t mode;      // type bits (S_IFREG/S_IFDIR) | permission bits
    uint16_t uid, gid, nlink;
    uint64_t size;
    uint32_t atime, mtime, ctime;
    uint32_t blocks;    // count of data+indirect blocks owned
    uint32_t direct[12];
    uint32_t indirect;      // 1024 pointers
    uint32_t dindirect;     // 1024 x 1024 pointers
    uint32_t flags;
    uint8_t  pad[36];
} inode_t;

typedef struct __attribute__((packed)) { // 64 bytes, 64 per block
    uint32_t ino;       // 0 = free slot
    uint8_t  name_len;
    uint8_t  type;
    char     name[58];
} dirent_t;
```
Max file size = (12 + 1024 + 1024²) × 4 KB ≈ 4 GiB. Inode numbers start at 1; inode 0 = invalid; inode 1 = root.

---

## 2. Execution phases overview

| Phase | Modules | Outcome |
|---|---|---|
| A. Foundation | M0–M3 | Fake disk, formatted image, free-space tracking |
| B. File system core | M4–M6 | Inodes, file data, directories |
| C. Public interface | M7–M8 | POSIX-like API, permissions |
| D. Performance & safety | M9–M11 | Cache, journal + crash recovery, fsck |
| E. Tooling & evaluation | M12–M13 | CLI shell, metrics engine |
| F. Validation | M14 | Fuzz/oracle/stress/soak, coverage, final report |

---

## 3. Module-by-module plan

### M0. Project scaffold
**Goal:** reproducible build/test infrastructure.
**Work:** directory tree, Makefile (targets: `all`, `test`, `test-asan`, `test-valgrind`, `gate N`, `bench`, `coverage`, `clean`), `test_harness.h`, `log.h` (levels via env var `MINIFS_LOG`), `scripts/run_gate.sh`.
**Tests:** a dummy test that passes and one deliberately failing test (to verify the runner returns non-zero; then remove it).
**Gate:** `make test` runs cleanly; `scripts/run_gate.sh 0` produces `reports/module_0.md`.

---

### M1. Block device layer (`blk.h/.c`)
**Goal:** treat the image file as an array of 4 KB blocks.

**API**
```c
int  blk_open(blkdev_t *d, const char *path, int create, uint32_t nblocks);
int  blk_read(blkdev_t *d, uint32_t blockno, void *buf);    // exactly 4096 bytes
int  blk_write(blkdev_t *d, uint32_t blockno, const void *buf);
int  blk_sync(blkdev_t *d);                                   // fsync
void blk_close(blkdev_t *d);
// fault injection (used by M10):
void blk_set_fault(blkdev_t *d, int64_t fail_after_n_writes, int mode);
// counters for metrics: reads, writes, bytes_read, bytes_written, syncs
```
**Logic**
- `pread(fd, buf, 4096, (off_t)blockno * 4096)`; loop until the full block is transferred (handle short reads/`EINTR`).
- Reject `blockno >= nblocks` with `-EINVAL`.
- Create mode: `ftruncate` to `nblocks*4096` (sparse).
- **Fault injection modes:** `FAULT_DROP` (after N writes, silently ignore all further writes, simulating power loss) and `FAULT_TORN` (the Nth write persists only the first half of the block). Counter is decremented inside `blk_write`.
- Per-device counters for the metrics engine.

**Tests**
1. Create 1 MB image, write distinct pattern to every block, read all back, compare.
2. Random 10,000 write/read ops vs. an in-memory shadow array.
3. Out-of-range read/write returns `-EINVAL`.
4. Close, reopen, data persists.
5. `FAULT_DROP` after 5 writes: only first 5 persist.
6. Counters equal the actual number of calls.

**Gate:** all pass, ASan/Valgrind clean. Tool `tools/mkdisk` creates an image of a given size.

---

### M2. Superblock and mkfs (`sb.h/.c`, `tools/mkfs_minifs.c`)
**Goal:** compute layout, format an image, validate on mount.

**API**
```c
int sb_compute_layout(uint32_t total_blocks, superblock_t *out);
int sb_format(blkdev_t *d);          // writes superblock, zeroes journal hdr/bitmaps/inode table, creates root dir
int sb_load(blkdev_t *d, superblock_t *out);   // checks magic, version, crc32, geometry
int sb_store(blkdev_t *d, const superblock_t *sb); // recomputes crc
uint32_t crc32(const void *p, size_t n);
```
**Logic**
- `sb_compute_layout` per §1.4; assert regions are contiguous, non-overlapping, and `data_start < total_blocks`; reject images < 256 KB with `-EINVAL`.
- `sb_format`: write superblock; zero journal super + bitmaps + inode table; **mark metadata blocks (0..data_start-1) as used in the block bitmap**; free counts = total − data_start. Root inode creation is completed in M4/M6 (a hook `mkfs_create_root()` is called there).
- `sb_load` validation order: magic → version → block_size → crc → geometry sanity (all starts < total, ordered). Return `-EINVAL`/`-EUCLEAN`.
- Mount sets `state=1` and stores; clean unmount sets `state=0`. A `state==1` on mount means "unclean shutdown" → triggers journal replay (M10) and optional fsck.

**Tests**
1. Layout for sizes 256 KB, 1 MB, 16 MB, 256 MB, 1 GB: no overlap, all regions inside image, counts consistent.
2. Format then load gives identical struct.
3. Flip one byte in superblock (each field) → CRC failure detected.
4. Wrong magic → rejected; non-minifs file → rejected.
5. Bitmap shows exactly `data_start` blocks used after format.
6. Re-format an existing image fully resets state.

**Gate:** all pass. Hexdump of block 0 included in report.

---

### M3. Bitmap allocators (`bitmap.h/.c`)
**Goal:** free-space management for inodes and blocks.

**API**
```c
int  bm_init(bitmap_t *b, blkdev_t *d, uint32_t start_blk, uint32_t nblks, uint32_t nbits);
int  bm_test(bitmap_t *b, uint32_t bit);
int  bm_set(bitmap_t *b, uint32_t bit);    // -EEXIST-like error if already set (double alloc)
int  bm_clear(bitmap_t *b, uint32_t bit);  // -EINVAL if already clear (double free)
int  bm_find_first_zero(bitmap_t *b, uint32_t from);
int  bm_find_run(bitmap_t *b, uint32_t from, uint32_t len);   // contiguous zeros
uint32_t bm_count_used(bitmap_t *b);
int  alloc_block(fs_t *fs, uint32_t goal);  // returns blockno or -ENOSPC
int  free_block(fs_t *fs, uint32_t blockno);
int  alloc_inode(fs_t *fs);                 // returns ino or -ENOSPC
int  free_inode(fs_t *fs, uint32_t ino);
```
**Logic**
- Bitmaps are accessed through block I/O (cache added in M9; design API so it routes through `fs_read_block/fs_write_block` wrappers, not `blk_*` directly).
- `find_first_zero`: scan bytes; skip `0xFF` bytes fast; use `__builtin_ctz(~byte)` inside first non-full byte; scan 64 bits at a time for speed.
- `alloc_block(goal)`: **next-fit with locality** – search from `goal` (the last block of the file being extended; 0 means a rotating "last allocated" hint), wrap around once. Updates superblock `free_blocks`, increments allocator stats (search steps, whether result was adjacent to goal).
- `bm_find_run` supports `alloc_blocks_contig(n)` used for preallocation and the benchmarks.
- Freed data blocks are **not zeroed** on free but are zeroed on allocation (prevents info leaks and stale pointers).
- Double-free and out-of-range are detected and return errors (and log).

**Tests**
1. Alloc all blocks → `-ENOSPC`; free all → bitmap identical to initial.
2. Double free / double set detected.
3. Allocations are unique (no block returned twice) over 100k random alloc/free ops against a shadow `bool[]` model.
4. `free_blocks` in superblock always equals `total − popcount(bitmap)`.
5. `find_run(len)` returns correct run in crafted bitmaps (start, middle, end, none).
6. Goal-based allocation returns `goal+1` when free (contiguity test).
7. Boundary bits (first, last, byte/word/block boundaries 7/8/63/64/32767/32768).

**Gate:** all pass; persistence test (alloc, close, reopen, state intact).

---

### M4. Inode layer (`inode.h/.c`)
**Goal:** persistent metadata and file-offset → block mapping.

**API**
```c
int inode_read(fs_t *fs, uint32_t ino, inode_t *out);
int inode_write(fs_t *fs, uint32_t ino, const inode_t *in);
int inode_create(fs_t *fs, uint16_t mode, uid_t uid, gid_t gid);  // alloc + init, returns ino
int inode_destroy(fs_t *fs, uint32_t ino);                         // frees all blocks + inode
int bmap(fs_t *fs, inode_t *ino, uint32_t lbn, int alloc, uint32_t *pbn); // logical → physical block
int inode_free_blocks_from(fs_t *fs, inode_t *ino, uint32_t first_lbn);   // for truncate
```
**Logic**
- Inode location: `blk = itable_start + (ino-1)/32`, `off = ((ino-1)%32)*128`. Read-modify-write the containing block.
- **`bmap(lbn)`**:
  - `lbn < 12` → `direct[lbn]`.
  - `lbn < 12+1024` → read `indirect` block, index `lbn-12`.
  - else → `idx = lbn-1036`; `l1 = idx/1024`, `l2 = idx%1024`; read `dindirect` block → entry `l1` → second-level block → entry `l2`.
  - If pointer is 0 and `alloc==1`: allocate (goal = previous block in file), zero the new block (pointer blocks must be zeroed), store pointer, update `ino->blocks`. If `alloc==0` return `*pbn=0` (hole).
  - `lbn >= 1036 + 1024²` → `-EFBIG`.
- `inode_free_blocks_from`: walks pointer tree from `first_lbn`; frees data blocks; frees an indirect block only when **all** its entries are cleared; updates `blocks` count.
- Timestamps set on create; helper `inode_touch(atime/mtime/ctime)`.

**Tests**
1. Round-trip all fields for 1000 random inodes, including boundary inodes (1, 32, 33, last).
2. Create inodes until `-ENOSPC`; destroy all → free_inodes restored.
3. `bmap` boundary cases: lbn = 0, 11, 12, 1035, 1036, 1036+1023, 1036+1024, max, max+1 (`-EFBIG`).
4. Map 5,000 consecutive blocks with alloc, read back mapping, all unique and valid.
5. Holes: `bmap(alloc=0)` of unmapped lbn returns 0 and does not allocate.
6. After `inode_free_blocks_from(0)`: bitmap used-count returns to baseline (no leaked indirect blocks).
7. Invalid inode numbers (0, > total) return `-EINVAL`.

**Gate:** all pass.

---

### M5. File data I/O (`fileio.h/.c`)
**Goal:** read/write/truncate at arbitrary byte offsets.

**API**
```c
ssize_t file_read(fs_t *fs, uint32_t ino, uint64_t off, void *buf, size_t len);
ssize_t file_write(fs_t *fs, uint32_t ino, uint64_t off, const void *buf, size_t len);
int     file_truncate(fs_t *fs, uint32_t ino, uint64_t newsize);
```
**Logic**
- **Read:** clamp `len` to `size - off` (return 0 at/after EOF). Loop: `lbn = pos/4096`, `boff = pos%4096`, `n = min(4096-boff, remaining)`; `bmap(alloc=0)`; if hole → fill zeros; else read block, copy slice.
- **Write:** loop same way with `bmap(alloc=1)`. If the write covers the whole block, skip the read (no read-modify-write). Otherwise read-modify-write. On `-ENOSPC` mid-write: return bytes written so far (partial write semantics) or the error if zero bytes written. After loop: `size = max(size, off+written)`, update `mtime/ctime`, write inode.
- **Write past EOF (sparse):** gap blocks are left unallocated (holes), reads return zeros.
- **Truncate shrink:** free blocks from `ceil(newsize/4096)`; zero the tail of the last partial block (so growing again later does not expose old data). **Truncate grow:** just set size (sparse).
- `blocks` count and bitmap must stay consistent after every op.

**Tests**
1. Write 1 byte, 4095, 4096, 4097, 8192 bytes at offset 0; read back.
2. Unaligned offsets (1, 4095, 4096, 4097) and crossing direct→indirect (offset ≈ 48 KB) and indirect→double-indirect (≈ 4.2 MB) boundaries.
3. Write 100 MB file with random content, SHA-256 compare vs host `sha256sum` (extract using test helper).
4. Sparse: write at offset 1 GB; `blocks` small; read hole returns zeros; `size` correct.
5. Shrink then grow: old data not resurrected (zeros).
6. Truncate to 0 frees all blocks; used-count back to baseline.
7. Fill disk: write returns partial count then `-ENOSPC`; no bitmap/inode corruption.
8. Randomized differential test against an in-memory byte-array model (50k ops of write/read/truncate with random offsets/lengths).

**Gate:** all pass; model-based test with 3 different seeds.

---

### M6. Directories and path resolution (`dir.h/.c`)
**Goal:** hierarchical namespace.

**API**
```c
int dir_lookup(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino);
int dir_add(fs_t *fs, uint32_t dir_ino, const char *name, uint32_t ino, uint8_t type);
int dir_remove(fs_t *fs, uint32_t dir_ino, const char *name);
int dir_list(fs_t *fs, uint32_t dir_ino, dir_iter_cb cb, void *ctx);
int dir_is_empty(fs_t *fs, uint32_t dir_ino);
int path_resolve(fs_t *fs, const char *path, uint32_t cwd, uint32_t *ino, uint32_t *parent, char *last_name);
int dir_init(fs_t *fs, uint32_t new_dir, uint32_t parent);   // writes "." and ".."
```
**Logic**
- Directory = regular inode with `S_IFDIR`, content = array of 64-byte `dirent_t`. Slot with `ino==0` is free.
- `dir_add`: reject name length 0 or > 57, names containing `/` or NUL, `.`/`..` creation, duplicates (`-EEXIST`). Find first free slot (scan); if none, extend the directory file by one block (via `file_write`). Update directory `size`, `mtime`.
- `dir_remove`: set slot `ino=0` (and name zeroed). Do **not** shrink the file (slots reused).
- `path_resolve`: split on `/`; start at root (absolute) or `cwd` (relative); for each component: `.` stay, `..` go to parent (root's `..` = root), else `dir_lookup`. Intermediate component must be a directory (`-ENOTDIR`). Collapse repeated slashes and trailing slash. Return parent inode and final name (for create/unlink). Enforce **max 4096 chars path**, **max 40 symlink-free depth**.
- `dir_init` creates `.` (self) and `..` (parent); parent `nlink++`.
- Link counts: new dir has `nlink=2` (self + "."), parent gets +1 for "..".
- **Advanced (after core passes):** per-directory hash index (see §6 A1).

**Tests**
1. mkdir tree 25 levels deep, resolve deep path; `..` from root stays root.
2. Create 10,000 entries in one directory; look up each; delete every other; re-add (slots reused, directory size does not grow beyond bound).
3. Duplicate name rejected; bad names rejected (empty, 58+ chars, contains `/`).
4. `a//b///c/` equals `a/b/c`.
5. Path through a regular file → `-ENOTDIR`.
6. `dir_is_empty` correct with only `.`/`..`.
7. Link counts of directories correct after create/remove nested dirs.
8. Listing returns exactly the live entries (no duplicates, no missing).

**Gate:** all pass; lookup-time-vs-entry-count CSV recorded as baseline (O(n)) for later comparison.

---

### M7. VFS API and file-descriptor table (`vfs.h/.c`)
**Goal:** POSIX-like user-facing operations.

**API**
```c
int  vfs_mount(vfs_t *v, const char *img);   int vfs_unmount(vfs_t *v);
int  vfs_open(vfs_t *v, const char *path, int flags, mode_t mode);   // O_RDONLY/WRONLY/RDWR/CREAT/EXCL/TRUNC/APPEND
int  vfs_close(vfs_t *v, int fd);
ssize_t vfs_read(vfs_t *v, int fd, void *buf, size_t n);
ssize_t vfs_write(vfs_t *v, int fd, const void *buf, size_t n);
off_t   vfs_lseek(vfs_t *v, int fd, off_t off, int whence);
int  vfs_mkdir(...); int vfs_rmdir(...); int vfs_unlink(...);
int  vfs_rename(...); int vfs_link(...); int vfs_stat(vfs_t*, const char*, struct vfs_stat*);
int  vfs_readdir(...); int vfs_chdir(...); int vfs_getcwd(...);
```
**Logic**
- **Open-file table** (system-wide, 256 entries): `{ino, offset, flags, refcount}`. **Per-process fd table** (single process in this project): `fd → open-file entry`. `fd` 0–2 reserved or not used; start at 3.
- `open`: resolve path; if missing and `O_CREAT` → create inode (mode & ~umask), `dir_add`; `O_EXCL` with existing → `-EEXIST`; `O_TRUNC` → truncate; directory opened for write → `-EISDIR`.
- `unlink`: `dir_remove`, `nlink--`; **if `nlink==0` and no open fds**, destroy inode; if still open, defer destruction until last `close` (orphan semantics). Keep an in-memory orphan list; on mount, fsck/journal recovery frees persistent orphans.
- `rmdir`: must be empty, not `.`/`..`/root; parent `nlink--`.
- `rename(old,new)`: resolve both; if target exists and is a file → replace atomically (unlink target); rename dir into its own subtree → `-EINVAL` (walk new parent's `..` chain up to root to check); updates `..` entry and parent link counts when moving directories across parents. Done inside a single transaction once M10 exists.
- `link`: hard link to files only (not dirs); `nlink++`; `-EMLINK` at 65535.
- `lseek`: `SEEK_SET/CUR/END`; negative result → `-EINVAL`; seeking beyond EOF allowed.
- `APPEND`: each write sets offset = size first.
- Every call takes a `fs->lock` (pthread mutex, trivial at this point; used in M14).

**Tests**
1. Table of semantic cases (each compared to real Linux behavior on a tmp directory on ext4/tmpfs): create/write/read; `O_EXCL`; `O_TRUNC`; `O_APPEND`; lseek variants; rename over existing; rename dir into itself (`-EINVAL`); unlink-while-open (data still readable until close, then space reclaimed); hard link then unlink one; rmdir non-empty (`-ENOTEMPTY`); bad fd (`-EBADF`); fd exhaustion.
2. **Differential harness** `tests/diff_posix.c`: random sequence of ~5,000 ops applied to minifs and to a host tmp dir; compare return codes (mapped to errno) and `stat` size/nlink, and directory listings at checkpoints.
3. Remount between operations; state identical.

**Gate:** all pass; zero divergences in 3 differential seeds.

---

### M8. Permissions and ownership (`perm.h/.c`)
**Goal:** enforce UNIX-style access control.

**API**
```c
void vfs_set_user(vfs_t *v, uid_t uid, gid_t gid);   // simulated current user (root = uid 0)
int  perm_check(const inode_t *i, uid_t u, gid_t g, int want);  // want = R_OK|W_OK|X_OK
int  vfs_chmod(...); int vfs_chown(...); int vfs_utimes(...); int vfs_umask(...);
```
**Logic**
- `perm_check`: uid 0 → allowed (except X on a file with no x bit at all); else pick class: owner if `uid==i->uid`, else group if `gid==i->gid`, else other; allowed iff `(mode>>shift) & want == want`.
- Enforce in VFS: path traversal requires **X on every directory** along the path; `open` needs R/W per flags; create/unlink/rename need **W+X on the parent directory**; `readdir` needs R on the directory; `chmod` only owner/root; `chown` only root (uid) or owner (gid to a group, simplified).
- `chmod` updates `ctime`; permissions persist in inode.
- Sticky bit (optional, advanced): only owner of file/dir or root may unlink in a sticky directory.

**Tests**
1. Full matrix: {owner, group, other, root} × {r, w, x} × {file, dir} × mode patterns 000–777 (generated programmatically; expected result computed by an independent reference function in the test).
2. Traversal denial: remove x on middle directory of a path → `-EACCES`.
3. Create in a directory without w → `-EACCES`; unlink in dir without w → `-EACCES`.
4. chmod/chown by non-owner → `-EPERM`.
5. umask applied to create mode.
6. Permissions survive remount.

**Gate:** matrix passes 100%.

---

### M9. Block cache (`cache.h/.c`)
**Goal:** reduce physical I/O; central point for all block access.

**Design**
- Fixed pool of N buffers (default 1024 = 4 MB; configurable). Each: `{blockno, data[4096], dirty, pinned/refcnt, valid, lru_prev/next, hash_next}`.
- **Hash table** (blockno → buffer) with chaining; **doubly linked LRU list**.
- Write policy: **write-back** (dirty flag), flushed on eviction, `cache_flush()`, and unmount. `write-through` mode selectable by flag for comparison and for journal home-writes.

**API**
```c
buf_t *cache_get(cache_t *c, uint32_t blk);      // read if absent, pin, move to MRU
void   cache_put(cache_t *c, buf_t *b, int dirty); // unpin, set dirty
int    cache_flush(cache_t *c);                  // write all dirty in blockno order (better disk locality), then blk_sync
int    cache_invalidate_all(cache_t *c);         // used by tests simulating restart
cache_stats_t cache_stats(cache_t *c);           // hits, misses, evictions, writebacks
```
**Logic**
- Miss: pick victim = LRU tail among **unpinned** buffers; if dirty, write back first; if all pinned → `-ENOMEM` (never evicts pinned).
- Optionally **read-ahead**: on sequential access pattern (blk N then N+1) prefetch N+2..N+4 (advanced).
- All upper layers (bitmaps, inode table, pointer blocks, data, directory) call `fs_read_block/fs_write_block` → cache → `blk_*`. After this module, **no module calls `blk_*` directly except the cache and the journal**.

**Tests**
1. Hit/miss counters exact for scripted access patterns (e.g., sequential scan > cache size = all misses; repeated access to working set < cache = all hits after warm-up).
2. LRU order correct (access A,B,C, touch A, insert D, evict B first).
3. Dirty eviction writes data to disk (verify with a second raw `blk_read`).
4. Pinned buffers never evicted; exhaustion returns error.
5. **Equivalence test:** rerun M5 and M7 random model tests with cache sizes 1 (degenerate), 16, 1024, and cache-off. Results identical.
6. After `cache_flush` + reopen, all data present; without flush, dirty data lost (expected; documents why journaling is needed).
7. Benchmark: sequential and random read of 64 MB, cache on vs off (report speedup).

**Gate:** all pass; speedup measured and recorded.

---

### M10. Journaling and crash recovery (`journal.h/.c`)
**Goal:** atomic multi-block metadata updates; crash consistency.

**Mode:** **metadata-only, redo (write-ahead) journal, ordered data** (ext3 `data=ordered` style).

**Journal layout:** `[journal superblock][ circular log area ]`
- Journal superblock: `{magic, head_seq, start_block (log position of oldest un-checkpointed txn), crc}`.
- Transaction in log: `[descriptor block: {magic, seq, count, target_blockno[count], crc}] [count data blocks (copies of new content)] [commit block: {magic, seq, crc over descriptor+data}]`.

**API**
```c
tx_t *tx_begin(fs_t *fs);
int   tx_write(tx_t *t, uint32_t blockno, const void *data);   // records the new image of a metadata block (dedup by blockno)
int   tx_commit(tx_t *t);
void  tx_abort(tx_t *t);
int   journal_replay(fs_t *fs);      // called at mount if state != clean
int   journal_checkpoint(fs_t *fs);
```
**Commit protocol (order is the whole point):**
1. Data blocks of regular files are written to their final location first (ordered mode) and synced if the transaction allocated them.
2. Write descriptor + data copies to the log. `blk_sync`.
3. Write commit block. `blk_sync`. **→ transaction is now durable.**
4. Write the same blocks to their home locations (through cache, then flush those blocks). `blk_sync`.
5. Advance journal superblock (`start_block`, `head_seq`) to free log space. `blk_sync`. (This is the checkpoint.)

**Recovery (`journal_replay`):** read journal superblock; from `start_block`, repeatedly: read descriptor (magic/seq/crc valid?) → read `count` data blocks → read commit (magic, seq, crc matches?). If commit missing/invalid → stop (that transaction is discarded, because it was never committed). If valid → write each data block to its target; continue with `seq+1`. At end: sync, reset log, mark superblock clean.

**Logic for which operations are transactions:** each VFS mutating call (`create`, `mkdir`, `unlink`, `rmdir`, `rename`, `link`, `write`-that-allocates, `truncate`, `chmod/chown`) wraps its metadata changes (bitmaps, inodes, directory blocks, pointer blocks, superblock free counts) in one `tx`. Internally, metadata writers call `tx_write` instead of writing directly when a tx is active. Large operations (e.g., truncate of 1 GB) split into bounded sub-transactions of ≤ N blocks, each keeping fs consistent (free the tail progressively and track it with an orphan record).
- Log full → force checkpoint, then retry; a single transaction larger than the log → `-ENOSPC` (prevented by splitting).

**Tests (the most important gate)**
1. Unit: commit, then simulate reboot (drop cache) → data present. Uncommitted tx (abort) → no effect.
2. **Crash-injection sweep** (`tools/crashtest.c`): for each scenario S in {create file, write 1 MB, mkdir, unlink, rename, rmdir, truncate, link}:
   - run S once with counting to learn the total number of block writes `W`;
   - for `k = 0..W`: fresh copy of the baseline image → set `FAULT_DROP` after `k` writes → run S → "power loss" → remount (journal replay) → **assert** (a) fsck clean, (b) state is **exactly the pre-state or exactly the post-state** (compare tree dump + file hashes), never in between.
   - Repeat with `FAULT_TORN` at each write.
3. Replay is **idempotent**: crash during replay itself (inject fault in replay), replay again → same result.
4. Corrupt a committed log block (flip a byte) → crc detects, transaction skipped or error as designed.
5. Log wrap-around test: > 3× log size worth of transactions, then crash at random points.
6. Performance: journaling overhead (ops/sec with and without journal), recorded.

**Gate:** 100% of crash points × scenarios pass; results table saved as `reports/crash_matrix.csv`.

---

### M11. fsck (`fsck.h/.c`, `tools/fsck_minifs.c`)
**Goal:** verify and repair file-system consistency.

**Passes**
1. **Superblock check:** magic, crc, geometry; free counts vs. computed.
2. **Inode scan:** for each inode in the inode bitmap: type valid, `size` consistent with `blocks`, all block pointers within `[data_start, total)`, no pointer to metadata area.
3. **Block ownership map:** `owner[blk]` array; walk every inode's block tree (direct, indirect pointer blocks, data blocks, double-indirect); a block claimed twice → **duplicate block** error.
4. **Namespace walk from root (DFS):** every directory entry points to an in-use inode; compute `ref_count[ino]`; each directory has valid `.` and `..`; detect directory loops and unreachable directories; names valid, no duplicate names.
5. **Cross-checks:**
   - Block bitmap vs. `owner[]`: bit set but unowned = **leaked block**; owned but bit clear = **missing allocation** (dangerous).
   - Inode bitmap vs. reachable inodes: used but unreachable = **orphan inode**.
   - `inode.nlink` vs. `ref_count` (for dirs: subdirs + 2).
   - Superblock free counts vs. bitmaps.

**Repair mode (`--repair`)**
| Problem | Repair |
|---|---|
| Wrong free counts | Recompute and rewrite |
| Leaked block | Clear bit |
| Block in use but bitmap clear | Set bit |
| Wrong link count | Set to `ref_count` |
| Orphan inode with data | Move to `/lost+found` as `#<ino>` (create `/lost+found` at mkfs); orphan with `nlink==0` → free |
| Dentry → free/invalid inode | Remove dentry |
| Duplicate block claim | Clone the block for the second owner if space, otherwise truncate that file at that point |
| Missing/bad `.` or `..` | Rewrite |
| Out-of-range block pointer | Zero pointer, adjust size |
Run fsck twice after repair: the second run must report **zero** problems.

**Tests**
1. Clean image (after complex workload) → 0 problems.
2. **Corruption matrix** (each injected programmatically in a copy of a populated image): flip bitmap bit set→clear / clear→set; change `nlink` ±1; zero an inode used by a dirent; point two inodes to same block; pointer beyond disk; remove `.`; make directory loop; corrupt superblock free count; orphan inode with data.
   - Expected: fsck **detects** each one (exit code + message names the issue). With `--repair`: second run is clean, and uncorrupted files remain byte-identical (hash check).
3. fsck runs on 1 GB image with 100k files in acceptable time (report seconds).
4. fsck is read-only without `--repair` (image hash unchanged).

**Gate:** 100% detection; repair converges in one pass for every matrix case.

---

### M12. CLI shell (`main.c`, `shell.c`)
**Goal:** administration interface (interactive REPL + scriptable).

**Invocation:** `minifs <image> [-c "cmd"] [-f script.txt]` or `minifs` for REPL. Standalone tools: `mkfs.minifs`, `fsck.minifs`, `mkdisk`.

**Commands**
| Command | Behavior |
|---|---|
| `mkfs <size> / mount / umount` | format/mount/clean-unmount |
| `ls [-l] [path]` | list (long: mode, nlink, uid, gid, size, mtime, name) |
| `cd`, `pwd`, `mkdir [-p]`, `rmdir`, `touch` | namespace ops |
| `cat <f>`, `write <f> "text"`, `append`, `truncate <f> <size>` | data ops |
| `rm [-r]`, `mv`, `cp`, `ln` | file ops |
| `stat <path>`, `tree [path]` | inspection |
| `chmod`, `chown`, `su <uid> <gid>`, `umask` | permissions |
| `cp-in <hostfile> <path>` / `cp-out <path> <hostfile>` | host ↔ image transfer (chunked, handles large files) |
| `df` | total/used/free blocks and inodes, utilization % |
| `fsck [--repair]` | run consistency check |
| `journal status`, `cache stats`, `metrics [reset|csv]` | internals |
| `crash <n>` | arm fault injection for demos |
| `bench seq|rand|meta|dir` | built-in benchmarks (M13) |
| `help`, `exit` | |

**Logic:** tokenizer with quote handling; command table with function pointers; every error printed as `error: <strerror>` with non-zero exit code in script mode; no command may crash on bad input (missing args, huge numbers, non-existent paths).

**Tests**
1. **Golden-file E2E:** `scripts/e2e.sh` runs a script covering every command; output diffed against `tests/golden/e2e.out`.
2. Invalid inputs (empty line, unknown command, missing args, unterminated quote, 10k-char line) → graceful error, process continues.
3. `cp-in` / `cp-out` round-trip of 0 B, 1 B, 4096 B, 10 MB, 200 MB files → `cmp` identical.
4. Unmount/mount cycle through the CLI retains data.
5. Run the entire e2e under Valgrind.

**Gate:** diff clean, Valgrind clean.

---

### M13. Metrics engine (`metrics.h/.c`)
**Goal:** instrument and report performance and storage health.

**Collected live (cheap counters):** block reads/writes/syncs (from M1), cache hits/misses/evictions (M9), journal commits/replays/log bytes (M10), allocator calls and search steps (M3), directory lookups and entries scanned (M6), per-operation latency (nanosecond timers using `clock_gettime(CLOCK_MONOTONIC)`), with min/avg/p50/p99/max.

**Computed metrics (definitions):**
| Metric | Formula |
|---|---|
| Block utilization | `(total_blocks − free_blocks) / total_blocks` |
| Inode utilization | `(total_inodes − free_inodes) / total_inodes` |
| Internal fragmentation | `(allocated_data_bytes − Σ file_size) / allocated_data_bytes` (excluding sparse holes) |
| External fragmentation | `1 − (largest_free_run / total_free_blocks)` |
| File fragmentation | for each file, number of discontinuities in its physical block list; report average and % files fragmented |
| Allocation efficiency | `% of allocations placed adjacent to goal block` and `avg bitmap bits scanned per alloc` |
| Sequential/random throughput (MB/s) | bytes ÷ elapsed for file read/write, reported both at FS level and physical-I/O level |
| IOPS | ops ÷ second (4 KB random read/write) |
| Cache hit ratio | `hits / (hits + misses)` |
| Write amplification | `physical bytes written / logical bytes written` (shows journal cost) |
| Directory access time | avg lookup latency vs. number of entries (1, 10, 100, 1k, 10k, 100k) |

**Benchmarks (`scripts/bench.sh` → `reports/bench/*.csv`):**
1. Throughput vs. I/O size (512 B … 1 MB), sequential and random.
2. Cache on vs. off vs. size sweep (16…4096 buffers).
3. Journal on vs. off (metadata-heavy: create 10k small files).
4. Directory lookup vs. size (linear vs. hashed if implemented).
5. Fragmentation vs. fill level (age the FS: random create/delete/grow to 30/60/90% full; measure external fragmentation and file fragmentation).
6. Allocation policy comparison: first-fit vs. next-fit-with-goal.
Each benchmark: warm-up run, ≥ 5 repetitions, report mean ± stddev; drop host page cache between runs where relevant (`posix_fadvise DONTNEED` or `O_DIRECT` option).

**Tests**
1. Counters exact against synthetic known workloads (e.g., write exactly 100 blocks → `blocks_written ≥ 100`, utilization matches by hand-calculation).
2. Metrics vs. independent computation: fsck-style tool recomputes utilization and fragmentation from scratch; values must match.
3. Timer sanity: measured sleep of 100 ms within tolerance.
4. Benchmark script is reproducible (two runs within ±15%) and produces valid CSVs.
5. `metrics csv` output parseable.

**Gate:** pass; charts generated (matplotlib script `scripts/plot.py`, optional).

---

### M14. Integration, fuzzing, stress, and concurrency
**Goal:** prove the whole system works together under adversarial and long-running conditions.

1. **Model-based fuzzer (`tools/fuzz.c`):** seeded PRNG generates random op sequences (create, mkdir, write at random offset, truncate, rename, link, unlink, rmdir, chmod, remount, flush) and applies them to **minifs and an in-memory reference model** (tree + byte arrays). After each op: compare return code; every 1,000 ops: full tree + content-hash comparison and `fsck` (must be clean). Run **1,000,000 ops** over ≥ 5 seeds; on divergence, print seed and minimized op log (replay support: `--seed N --until K`).
2. **Crash-fuzz:** random op sequence with random crash points (journal on); after remount: fsck clean and state equals the model at some transaction boundary (prefix of ops).
3. **Disk-full stress:** run workload until `-ENOSPC` repeatedly, delete, continue; no corruption, free counts correct.
4. **Soak test:** 1+ hour mixed workload (script), memory use flat (check RSS), fsck clean at the end.
5. **Concurrency:** N=4–8 pthreads doing independent ops; global mutex first; assert no races with `-fsanitize=thread`; invariants hold. (Advanced: per-inode/range locks.)
6. **Invariant checker (debug build):** after every mutating op, assert: `free_blocks == popcount`, every dirent target in-use, `nlink == dirent refs`, no duplicated block.
7. **Coverage:** `gcov/lcov` ≥ 85% line coverage overall, ≥ 90% for allocator/inode/journal; uncovered lines justified in report.
8. **Static analysis:** `cppcheck --enable=all`, `clang-tidy`; zero high-severity findings.

**Gate:** all of the above pass → tag `v1.0`.

---

## 4. Cross-cutting design rules

1. **Single write path:** upper layers → `fs_write_block()` → (tx? `tx_write` : cache). No raw `blk_write` elsewhere (grep check in CI).
2. **Metadata ordering:** inside a transaction the order of updates does not matter (atomic), but outside a transaction (e.g., if the journal is disabled for benchmarking), follow the safe order: allocate → initialize → link; unlink → free.
3. **Zero on allocate:** newly allocated blocks (data, pointer, directory) are zeroed before use.
4. **Lock order:** `fs->lock` → `cache->lock` → `blk` (no reverse acquisition).
5. **Superblock free counts** are a cached summary: update inside the same transaction as the bitmap change; fsck can rebuild them.
6. **No hidden state in RAM** that is required for correctness after crash except the open-file/orphan table (rebuilt by orphan recovery at mount).
7. **Time:** use `time(NULL)` through a wrapper that tests can override for deterministic output.

## 5. Order of work, timeline, and checkpoints

| Week | Modules | Evidence produced |
|---|---|---|
| 1 | M0, M1, M2, M3 | `module_0..3.md`, hexdump of formatted image |
| 2 | M4, M5, M6 | model-test logs, directory baseline CSV |
| 3 | M7, M8, M9 | differential-test log, permission matrix, cache speedup chart |
| 4 | M10, M11 | **crash matrix CSV**, fsck corruption matrix |
| 5 | M12, M13, M14 | golden E2E, benchmark CSVs/charts, fuzz/soak/coverage reports |
| 5 (end) | Final report + demo | `reports/final_report.md`, demo script |

Per-module loop for the agent:
```
write header + stubs → write tests first (they fail) → implement → run `scripts/run_gate.sh N`
→ fix until green (ASan, UBSan, Valgrind) → write reports/module_N.md → update CHANGELOG
→ git commit + tag → proceed
```
**Regression rule:** every gate also re-runs all earlier modules' tests (`make test` runs the full suite). A later module breaking an earlier one blocks progress.

## 6. Advanced options (only after M14 passes; each with own test + benchmark)

| ID | Feature | Test / evidence |
|---|---|---|
| A1 | **Hashed directory index** (in-memory hash built on first access, or on-disk hash blocks per directory) | Lookup time flat vs. entries; equivalence with linear scan on fuzz |
| A2 | **Extents** (start, length) instead of per-block pointers | Smaller metadata for large files; fragmentation metrics improve |
| A3 | **Delayed allocation** + preallocation | Fewer fragments in append workloads |
| A4 | **Read-ahead** in cache | Sequential read speedup |
| A5 | **Symbolic links** | resolve with loop limit (40), `readlink` |
| A6 | **FUSE mount** (`libfuse3`) so real `ls`, `cp`, `rsync` work | Run `fsx`/`pjdfstest` subsets |
| A7 | **Per-inode locking** / reader-writer locks | TSan clean, throughput vs. threads |
| A8 | **Data checksums / scrubbing** | Corrupt data block detected by `scrub` |
| A9 | **Online snapshot** (copy-on-write metadata) | Snapshot stays stable while live FS changes |

## 7. Final report plan (`reports/final_report.md`)

1. Abstract and problem statement
2. Background: block devices, inodes, directories, allocation, caching, journaling, fsck (cite OSTEP ch. 39–42, ext2/ext4 docs, xv6 book ch. 8)
3. System design: layered architecture diagram, on-disk layout diagram, struct tables, trade-off discussion (block size, inode ratio, journal mode)
4. Module-wise implementation (M1–M14): purpose, algorithm, complexity, key snippets (assembled from `module_N.md`)
5. Testing: gate-results table, **crash-injection matrix**, **fsck corruption matrix**, fuzz summary (ops, seeds, divergences = 0), coverage, sanitizer/Valgrind summaries
6. Performance evaluation: the six benchmarks with charts and interpretation (throughput, cache effect, journal overhead, directory scaling, fragmentation, allocation policy)
7. Limitations and future work
8. Conclusion; appendices (CLI reference, command transcript, struct definitions)

## 8. Final acceptance checklist (all must be ✔)

- [ ] M0–M14 gates passed, tags `m0-pass` … `m14-pass`, `v1.0`
- [ ] Persistent hierarchical namespace across remounts
- [ ] Files/dirs: create, read, write, truncate, rename, link, unlink, rmdir, stat
- [ ] Permissions enforced with 100% matrix pass
- [ ] Block allocation, free-space tracking, sparse files, files up to ~4 GiB
- [ ] Superblock with CRC, inode table, bitmaps, directory structure
- [ ] LRU write-back cache with measured speedup
- [ ] Journal: 100% crash-point matrix passed, replay idempotent
- [ ] fsck detects/repairs 100% of corruption matrix
- [ ] CLI complete, golden E2E passes
- [ ] Metrics: utilization, fragmentation, throughput, IOPS, allocation efficiency, directory access reported, validated
- [ ] 1M-op fuzz, soak, TSan, ≥ 85% coverage, 0 sanitizer/Valgrind errors
- [ ] Final report, per-module reports, benchmark CSVs, demo script

## 9. Common pitfalls to avoid (agent checklist)

| Pitfall | Prevention |
|---|---|
| Struct padding changes on-disk size | `_Static_assert(sizeof(inode_t)==128)` etc. |
| Off-by-one at block/bitmap boundaries | Boundary tests in M3/M4/M5 |
| Freed indirect block leaks | M4 test 6, fsck leaked-block check |
| Stale data exposed after truncate/grow | Zero tail + zero-on-allocate; M5 test 5 |
| Cache returns stale data after journal replay | `cache_invalidate_all()` after replay |
| Journal commit without sync ordering | Explicit `blk_sync` between steps; crash sweep catches violations |
| Unlink of open file frees data early | Orphan list + deferred destroy (M7) |
| Rename directory into own subtree | Ancestor walk check (M7) |
| Treating block 0 as valid pointer | 0 = null; assert in `bmap` |
| Hidden test-only behavior | Tests use public API only (plus fault-injection hooks) |
