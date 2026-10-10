#!/usr/bin/env bash
set -euo pipefail

BENCH_BIN="build/bin/bench"
OUT_DIR="reports/bench"
TMP_IMG="/tmp/minifs_bench_run.img"

if [ ! -x "$BENCH_BIN" ]; then
    echo "Building benchmark tool..."
    make build/bin/bench
fi

mkdir -p "$OUT_DIR"

echo "=========================================================="
echo "          RUNNING MINIFS PERFORMANCE BENCHMARKS           "
echo "=========================================================="

echo "--> [1/5] Sequential Throughput vs I/O Size..."
$BENCH_BIN seq "$TMP_IMG" "$OUT_DIR/throughput_seq.csv"

echo "--> [2/5] 4 KB Random I/O & Latency..."
$BENCH_BIN rand "$TMP_IMG" "$OUT_DIR/throughput_rand.csv"

echo "--> [3/5] Cache Buffer Sweep (Uncached vs 16..1024)..."
$BENCH_BIN cache "$TMP_IMG" "$OUT_DIR/cache_sweep.csv"

echo "--> [4/5] Journal Metadata Overhead..."
$BENCH_BIN journal "$TMP_IMG" "$OUT_DIR/journal_overhead.csv"

echo "--> [5/5] Directory Lookup Scale & Fragmentation..."
$BENCH_BIN dir "$TMP_IMG" "$OUT_DIR/dir_lookup_scale.csv"
$BENCH_BIN frag "$TMP_IMG" "$OUT_DIR/fragmentation_fill.csv"

rm -f "$TMP_IMG"
echo "=========================================================="
echo " Benchmarks Complete! CSVs saved to $OUT_DIR/            "
echo "=========================================================="
