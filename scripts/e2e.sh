#!/usr/bin/env bash
set -euo pipefail

IMAGE="/tmp/minifs_e2e.img"
SCRIPT_FILE="/tmp/e2e_commands.txt"
ACTUAL_OUT="/tmp/e2e_actual.out"
GOLDEN_OUT="tests/golden/e2e.out"

rm -f "$IMAGE" "$SCRIPT_FILE" "$ACTUAL_OUT"
mkdir -p tests/golden

# 1. Generate the comprehensive commands script
cat << 'EOF' > "$SCRIPT_FILE"
mkfs 4096
mkdir -p /dir1/subdir
cd /dir1/subdir
pwd
touch testfile.txt
write testfile.txt "Hello E2E Test!"
cat testfile.txt
append testfile.txt " More Content"
cat testfile.txt
truncate testfile.txt 5
cat testfile.txt
cp testfile.txt copied.txt
ln testfile.txt linked.txt
mv copied.txt moved.txt
ls
rm linked.txt
cd /
tree
chmod 0600 /dir1/subdir/testfile.txt
chown 500:600 /dir1/subdir/testfile.txt
su 1000 1000
umask 0022
umask
su 0 0
rm -r /dir1
journal status
cache stats
sync
fsck
help
exit
EOF

echo "--> Running minifs script for Golden E2E..."
./build/bin/minifs "$IMAGE" -f "$SCRIPT_FILE" > "$ACTUAL_OUT"

if [ ! -f "$GOLDEN_OUT" ]; then
    echo "Creating initial golden file $GOLDEN_OUT..."
    cp "$ACTUAL_OUT" "$GOLDEN_OUT"
else
    echo "Diffing actual output against golden output..."
    diff -u "$GOLDEN_OUT" "$ACTUAL_OUT"
    echo "Golden file diff PASSED!"
fi

# 2. Test cp-in / cp-out round-trip for various sizes (0 B, 1 B, 4096 B, 1 MB, 10 MB)
echo "--> Testing cp-in / cp-out round-trip..."
rm -f "$IMAGE"
./build/bin/minifs "$IMAGE" -c "mkfs 16384" >/dev/null

SIZES=(0 1 4096 1048576 10485760)
for sz in "${SIZES[@]}"; do
    HOST_IN="/tmp/e2e_in_${sz}.bin"
    HOST_OUT="/tmp/e2e_out_${sz}.bin"
    rm -f "$HOST_IN" "$HOST_OUT"

    if [ "$sz" -eq 0 ]; then
        touch "$HOST_IN"
    else
        head -c "$sz" /dev/urandom > "$HOST_IN"
    fi

    ./build/bin/minifs "$IMAGE" -c "cp-in $HOST_IN /file_${sz}.bin" >/dev/null
    ./build/bin/minifs "$IMAGE" -c "cp-out /file_${sz}.bin $HOST_OUT" >/dev/null

    cmp "$HOST_IN" "$HOST_OUT"
    echo "  Round-trip size $sz bytes: OK"
    rm -f "$HOST_IN" "$HOST_OUT"
done

# Clean up
rm -f "$IMAGE" "$SCRIPT_FILE" "$ACTUAL_OUT"
echo "=== E2E Integration Suite Passed ==="
