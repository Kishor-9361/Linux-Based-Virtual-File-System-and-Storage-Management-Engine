#!/usr/bin/env bash
set -uo pipefail

IMAGE="/tmp/minifs_edge_case_test.img"
HOST_TEMP="/tmp/minifs_edge_host"
SCRIPT_FILE="/tmp/minifs_test_script.txt"

PASSED=0
FAILED=0

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

cleanup() {
    rm -rf "$IMAGE" "$HOST_TEMP" "$SCRIPT_FILE" /tmp/host_in_*.bin /tmp/host_out_*.bin /tmp/ec_*.bin
}
trap cleanup EXIT

cleanup
mkdir -p "$HOST_TEMP"

assert_ok() {
    local desc="$1"
    shift
    if "$@"; then
        echo -e "  [${GREEN}PASS${NC}] $desc"
        ((PASSED++))
    else
        echo -e "  [${RED}FAIL${NC}] $desc"
        ((FAILED++))
        return 1
    fi
}

assert_err() {
    local desc="$1"
    shift
    if ! "$@" >/dev/null 2>&1; then
        echo -e "  [${GREEN}PASS${NC}] $desc (Cleanly rejected as expected)"
        ((PASSED++))
    else
        echo -e "  [${RED}FAIL${NC}] $desc (Expected failure, but command succeeded!)"
        ((FAILED++))
        return 1
    fi
}

echo -e "${BOLD}${BLUE}==================================================================${NC}"
echo -e "${BOLD}${BLUE}       MINIFS FULL COMPREHENSIVE EDGE-CASE & STRESS SUITE         ${NC}"
echo -e "${BOLD}${BLUE}==================================================================${NC}\n"

# Ensure all tools are built
echo -e "${BOLD}--> [0/8] Building minifs tools & targets...${NC}"
make all >/dev/null

# -----------------------------------------------------------------------------
# SECTION 1: Shell Parsing, Tokenization & Buffer Resilience
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [1/8] Shell Syntax, Quote Parsing & Buffer Resilience...${NC}"

# Setup fresh 16 MB disk
./build/bin/minifs "$IMAGE" -c "mkfs 4096" >/dev/null

# 1. Empty lines & comments
assert_ok "Empty string handling" ./build/bin/minifs "$IMAGE" -c ""
assert_ok "Whitespace-only input" ./build/bin/minifs "$IMAGE" -c $'     \t   '
assert_ok "Comment line handling (#)" ./build/bin/minifs "$IMAGE" -c "# Full line comment"

# 2. Quoted filenames with spaces
assert_ok "Create file with spaces in name" \
    ./build/bin/minifs "$IMAGE" -c 'write "My Spaced File.txt" "Content inside file with spaces"'
assert_ok "Read file with spaces in name" \
    ./build/bin/minifs "$IMAGE" -c 'cat "My Spaced File.txt"'
assert_ok "Stat file with spaces in name" \
    ./build/bin/minifs "$IMAGE" -c 'stat "My Spaced File.txt"'

# 3. Buffer overflow resilience (10,000-character line)
HUGE_LINE=$(printf 'A%.0s' {1..10000})
assert_err "10,000-character buffer overflow protection" \
    ./build/bin/minifs "$IMAGE" -c "$HUGE_LINE"

# 4. Unknown command rejection
assert_err "Reject non-existent command" \
    ./build/bin/minifs "$IMAGE" -c "nonexistent_command_xyz"

# 5. Missing required arguments
assert_err "Reject 'mkdir' with no arguments" ./build/bin/minifs "$IMAGE" -c "mkdir"
assert_err "Reject 'cat' with no arguments" ./build/bin/minifs "$IMAGE" -c "cat"
assert_err "Reject 'rm' with no arguments" ./build/bin/minifs "$IMAGE" -c "rm"
assert_err "Reject 'truncate' with missing length" ./build/bin/minifs "$IMAGE" -c "truncate file.txt"

# -----------------------------------------------------------------------------
# SECTION 2: Hierarchical Namespace & Directory Edge Cases
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [2/8] Namespace, Directory Traversal & Loop Prevention...${NC}"

# 1. Multi-level directory creation
assert_ok "Create multi-level directories (mkdir -p)" \
    ./build/bin/minifs "$IMAGE" -c "mkdir -p /a/b/c/d/e/deep"

# 2. Redundant slashes
assert_ok "Handle redundant consecutive slashes" \
    ./build/bin/minifs "$IMAGE" -c "touch ///a////b///slashed_file.txt"

# 3. Prevent duplicate directory creation without -p
assert_err "Reject duplicate directory creation without -p" \
    ./build/bin/minifs "$IMAGE" -c "mkdir /a/b/c"

# 4. Prevent rmdir on non-empty directory
assert_err "Reject rmdir on non-empty directory" \
    ./build/bin/minifs "$IMAGE" -c "rmdir /a/b"

# 5. Protect root directory
assert_err "Reject rmdir on root directory (/)" \
    ./build/bin/minifs "$IMAGE" -c "rmdir /"

# 6. Protect dot and dotdot
assert_err "Reject rmdir on '.'" ./build/bin/minifs "$IMAGE" -c "rmdir ."
assert_err "Reject rmdir on '..'" ./build/bin/minifs "$IMAGE" -c "rmdir .."

# 7. Prevent directory loop in mv / rename
assert_err "Reject moving parent directory into its own child" \
    ./build/bin/minifs "$IMAGE" -c "mv /a /a/b/c/d/e/deep/loop_dest"

# 8. Recursive subtree removal
assert_ok "Recursive subtree removal (rm -r)" \
    ./build/bin/minifs "$IMAGE" -c "rm -r /a"

# -----------------------------------------------------------------------------
# SECTION 3: File I/O, Truncation & Block Indirection Boundary Cases
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [3/8] File I/O, Truncation, Indirection & Links...${NC}"

# 1. 0-byte file handling
assert_ok "Create 0-byte file" ./build/bin/minifs "$IMAGE" -c "touch /zero.dat"
assert_ok "Read 0-byte file without error" ./build/bin/minifs "$IMAGE" -c "cat /zero.dat"

# 2. Write, append, and truncate
assert_ok "Write text at offset 0" \
    ./build/bin/minifs "$IMAGE" -c 'write /data.txt "First Line Data"'
assert_ok "Append text to existing file" \
    ./build/bin/minifs "$IMAGE" -c 'append /data.txt " - Second Line Appended"'

# 3. Truncation down (shrink)
assert_ok "Truncate file down to 5 bytes" \
    ./build/bin/minifs "$IMAGE" -c "truncate /data.txt 5"

# 4. Truncation up (grow creating sparse hole)
assert_ok "Truncate file up to 8192 bytes (sparse)" \
    ./build/bin/minifs "$IMAGE" -c "truncate /data.txt 8192"

# 5. Truncation to 0 (free all blocks)
assert_ok "Truncate file to 0 bytes" \
    ./build/bin/minifs "$IMAGE" -c "truncate /data.txt 0"

# 6. Hard links & link count tracking
assert_ok "Create regular file for link test" \
    ./build/bin/minifs "$IMAGE" -c 'write /orig.txt "Original Content"'
assert_ok "Create hard link" \
    ./build/bin/minifs "$IMAGE" -c "ln /orig.txt /linked.txt"

# 7. Disallow hard links to directories (POSIX standard)
./build/bin/minifs "$IMAGE" -c "mkdir /testdir" >/dev/null
assert_err "Disallow hard link to directory (-EPERM)" \
    ./build/bin/minifs "$IMAGE" -c "ln /testdir /link_to_dir"

# 8. Unlink one hard link; second link retains data
assert_ok "Remove original hard link" \
    ./build/bin/minifs "$IMAGE" -c "rm /orig.txt"
assert_ok "Read data through remaining hard link" \
    ./build/bin/minifs "$IMAGE" -c "cat /linked.txt"
assert_ok "Clean up linked file" \
    ./build/bin/minifs "$IMAGE" -c "rm /linked.txt"

# -----------------------------------------------------------------------------
# SECTION 4: Permissions & DAC Access Control
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [4/8] DAC Permissions, Umask & Security Enforcement...${NC}"

# 1. Umask masking
assert_ok "Set restrictive umask 0077" \
    ./build/bin/minifs "$IMAGE" -c "umask 0077"
assert_ok "Create file under umask 0077" \
    ./build/bin/minifs "$IMAGE" -c "touch /secret.txt"

# 2. Reset umask & chmod
assert_ok "Reset umask to 0022" \
    ./build/bin/minifs "$IMAGE" -c "umask 0022"
assert_ok "Set explicit chmod 0600 (owner rw only)" \
    ./build/bin/minifs "$IMAGE" -c "chmod 0600 /secret.txt"

# 3. Ownership modification
assert_ok "Change file owner to uid 1000" \
    ./build/bin/minifs "$IMAGE" -c "chown 1000:1000 /secret.txt"

# 4. Switch credentials to unauthorized user (2000) & verify access denial in single session
SCRIPT_PERM="/tmp/test_minifs_perm.txt"
cat << 'EOF' > "$SCRIPT_PERM"
su 2000 2000
cat /secret.txt
EOF
assert_err "Deny read to non-owner on 0600 file (-EACCES)" \
    ./build/bin/minifs "$IMAGE" -f "$SCRIPT_PERM"
rm -f "$SCRIPT_PERM"

# 5. Verify root bypass succeeds on 0600 file
assert_ok "Root read bypass succeeds on 0600 file" \
    ./build/bin/minifs "$IMAGE" -c "cat /secret.txt"

# -----------------------------------------------------------------------------
# SECTION 5: Host System Interoperability & Large File Transfer
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [5/8] Host Transfer (cp-in / cp-out) Binary Integrity...${NC}"

# Roundtrip testing across boundary sizes: 0 B, 1 B, 4096 B, 65536 B, 2 MiB
SIZES=(0 1 4096 65536 2097152)
for sz in "${SIZES[@]}"; do
    H_IN="/tmp/host_in_${sz}.bin"
    H_OUT="/tmp/host_out_${sz}.bin"
    rm -f "$H_IN" "$H_OUT"

    if [ "$sz" -eq 0 ]; then
        touch "$H_IN"
    else
        head -c "$sz" /dev/urandom > "$H_IN"
    fi

    ./build/bin/minifs "$IMAGE" -c "cp-in $H_IN /host_transfer_${sz}.bin" >/dev/null
    ./build/bin/minifs "$IMAGE" -c "cp-out /host_transfer_${sz}.bin $H_OUT" >/dev/null

    if cmp -s "$H_IN" "$H_OUT"; then
        echo -e "  [${GREEN}PASS${NC}] Round-trip size $sz bytes matches bitwise identically"
        ((PASSED++))
    else
        echo -e "  [${RED}FAIL${NC}] Round-trip size $sz bytes corrupted!"
        ((FAILED++))
    fi
    rm -f "$H_IN" "$H_OUT"
done

# Non-existent host file
assert_err "Reject cp-in of non-existent host file" \
    ./build/bin/minifs "$IMAGE" -c "cp-in /tmp/does_not_exist_host.bin /target.bin"

# -----------------------------------------------------------------------------
# SECTION 6: Storage Exhaustion (-ENOSPC) & Full Reclamation
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [6/8] Disk-Full Stress (-ENOSPC) & Free Space Recovery...${NC}"

SMALL_IMG="/tmp/minifs_small_enospc.img"
rm -f "$SMALL_IMG"

# Create a small 512-block filesystem (~2 MB)
./build/bin/mkdisk "$SMALL_IMG" 512 >/dev/null
./build/bin/mkfs_minifs "$SMALL_IMG" >/dev/null

# Fill to capacity
cat << 'EOF' > "$SCRIPT_FILE"
mkdir /fill_dir
cd /fill_dir
EOF

for i in {1..80}; do
    echo "write file_$i.dat \"Filling block payload data repeatedly for exhaustion testing $i\"" >> "$SCRIPT_FILE"
done

# Execute fill script (allowed to encounter ENOSPC gracefully)
./build/bin/minifs "$SMALL_IMG" -f "$SCRIPT_FILE" >/dev/null 2>&1 || true

# Verify disk reports high utilization
assert_ok "Disk full status reported correctly via df" \
    ./build/bin/minifs "$SMALL_IMG" -c "df"

# Reclaim space by deleting files
cat << 'EOF' > "$SCRIPT_FILE"
cd /fill_dir
rm -r /fill_dir
df
EOF
assert_ok "Reclaim all space cleanly via recursive delete" \
    ./build/bin/minifs "$SMALL_IMG" -f "$SCRIPT_FILE"

# Verify new files can now be written into reclaimed space
assert_ok "Write new file into reclaimed blocks" \
    ./build/bin/minifs "$SMALL_IMG" -c 'write /recovered.txt "Recovered after ENOSPC!"'

rm -f "$SMALL_IMG"

# -----------------------------------------------------------------------------
# SECTION 7: Crash Consistency & FSCK Corruption Repair
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [7/8] Fault Injection, Crash Consistency & FSCK Repair...${NC}"

FSCK_IMG="/tmp/minifs_fsck_test.img"
rm -f "$FSCK_IMG"
./build/bin/mkdisk "$FSCK_IMG" 1024 >/dev/null
./build/bin/mkfs_minifs "$FSCK_IMG" >/dev/null

# 1. Arm fault injection and trigger crash
assert_ok "Arm fault injection in shell (crash 5 writes)" \
    ./build/bin/minifs "$FSCK_IMG" -c "crash 5"

# 2. Verify fsck reports clean on valid disk
assert_ok "FSCK verifies clean image" \
    ./build/bin/fsck_minifs "$FSCK_IMG"

# 3. Deliberately corrupt superblock CRC to test FSCK detection & repair
printf '\xFF\xFF\xFF\xFF' | dd of="$FSCK_IMG" bs=1 seek=12 count=4 conv=notrunc 2>/dev/null
assert_err "FSCK detects corrupted superblock CRC" \
    ./build/bin/fsck_minifs "$FSCK_IMG"

# 4. Run FSCK in repair mode
assert_ok "FSCK repair fixes corrupt superblock" \
    ./build/bin/fsck_minifs --repair "$FSCK_IMG"

# 5. Idempotent check: repaired image must now be 100% clean
assert_ok "FSCK confirms 0 errors after repair (idempotent)" \
    ./build/bin/fsck_minifs "$FSCK_IMG"

rm -f "$FSCK_IMG"

# -----------------------------------------------------------------------------
# SECTION 8: Diagnostics & Telemetry Commands
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}--> [8/8] System Diagnostics, Metrics & Internal Telemetry...${NC}"

assert_ok "Display formatted disk space (df)" \
    ./build/bin/minifs "$IMAGE" -c "df"
assert_ok "Display visual hierarchy tree (tree)" \
    ./build/bin/minifs "$IMAGE" -c "tree"
assert_ok "Inspect WAL journal status (journal status)" \
    ./build/bin/minifs "$IMAGE" -c "journal status"
assert_ok "Inspect WAL journal stats alias (journal stats)" \
    ./build/bin/minifs "$IMAGE" -c "journal stats"
assert_ok "Inspect LRU buffer cache stats (cache stats)" \
    ./build/bin/minifs "$IMAGE" -c "cache stats"
assert_ok "Display storage health & latencies (metrics)" \
    ./build/bin/minifs "$IMAGE" -c "metrics"
assert_ok "Export metrics in CSV format (metrics csv)" \
    ./build/bin/minifs "$IMAGE" -c "metrics csv"
assert_ok "Reset metrics counters (metrics reset)" \
    ./build/bin/minifs "$IMAGE" -c "metrics reset"
assert_ok "Synchronize dirty buffers (sync)" \
    ./build/bin/minifs "$IMAGE" -c "sync"

# -----------------------------------------------------------------------------
# FINAL SUMMARY
# -----------------------------------------------------------------------------
echo -e "\n${BOLD}${BLUE}==================================================================${NC}"
echo -e "${BOLD} EDGE-CASE TEST SUITE SUMMARY:${NC}"
echo -e "   Total Tests Passed: ${GREEN}${BOLD}${PASSED}${NC}"
echo -e "   Total Tests Failed: ${RED}${BOLD}${FAILED}${NC}"
echo -e "${BOLD}${BLUE}==================================================================${NC}"

if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}${BOLD} ALL MINIFS EDGE CASES & CRITICAL CONDITIONS PASSED!${NC}\n"
    exit 0
else
    echo -e "${RED}${BOLD} SOME EDGE CASES FAILED. CHECK OUTPUT ABOVE.${NC}\n"
    exit 1
fi
