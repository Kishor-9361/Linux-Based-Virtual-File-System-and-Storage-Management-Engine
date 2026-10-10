# Gate Report: Module 13 - Metrics Engine & Performance Benchmark Suite

- **Date**: 2026-10-10 11:22:48Z
- **Module ID**: M13
- **Status**: PASSED
- **Version**: `0.14.0`

## 1. Objective
Implementation and verification of the `minifs` Metrics Engine and Performance Evaluation Suite (`include/metrics.h`, `src/metrics.c`, `tools/bench.c`, `scripts/bench.sh`). Live counters, latency percentile distributions, on-disk storage health indicators (internal/external/file fragmentation), and comprehensive benchmarking across I/O sizes, cache configurations, journal overhead, directory scaling, and filesystem aging.

## 2. Computed Metrics & Storage Health Formulas
| Metric | Mathematical Definition | Verified Behavior |
|---|---|---|
| **Block Utilization** | `(total_blocks - free_blocks) / total_blocks` | Matches superblock counters exactly |
| **Inode Utilization** | `(total_inodes - free_inodes) / total_inodes` | Matches inode allocation bitmap |
| **Internal Fragmentation** | `(allocated_data_bytes - Σ file_size) / allocated_data_bytes` | Measures slack bytes across regular files |
| **External Fragmentation** | `1.0 - (largest_free_run / total_free_blocks)` | Contiguity loss in unallocated data blocks |
| **File Fragmentation** | Discontinuities in physical block chains (`pbn[i+1] != pbn[i] + 1`) | Tracks % of fragmented files and avg discontinuities |
| **Allocation Efficiency** | `% adjacent to goal block` & `avg bits scanned per alloc` | Derived from allocator statistics |
| **Write Amplification** | `physical_bytes_written / logical_bytes_written` | Captures journal commit and metadata overhead |
| **Cache Hit Ratio** | `hits / (hits + misses)` | Accurate LRU efficiency tracking |
| **Latency Distributions** | Min, Average, p50, p99, and Max (nanosecond resolution) | Reservoir sampling and percentile sorting |

## 3. Empirical Benchmark Results (`reports/bench/*.csv`)

### A. Sequential Throughput vs. I/O Block Size (`reports/bench/throughput_seq.csv`)
- **512 B**: 0.84 MB/s write, 303.85 MB/s read
- **4 KiB**: 4.50 MB/s write, 677.20 MB/s read
- **64 KiB**: 55.06 MB/s write, 874.95 MB/s read
- **1 MiB**: **179.56 MB/s write, 768.60 MB/s read**

### B. 4 KiB Random I/O & Latency Percentiles (`reports/bench/throughput_rand.csv`)
- **Random Read**: **204,040 IOPS** | Avg: 4.43 µs | p50: **4.02 µs** | p99: **12.66 µs**
- **Random Write**: **1,757 IOPS** (with direct/journal sync) | Avg: 567.35 µs | p50: 529.90 µs | p99: 1,284.59 µs

### C. LRU Cache Scale Sweep (`reports/bench/cache_sweep.csv`)
- **Uncached (0 buffers)**: 507.39 MB/s (0.0% hit ratio)
- **16 buffers**: 918.45 MB/s (78.31% hit ratio)
- **64 buffers**: 1,273.53 MB/s (78.32% hit ratio)
- **1024 buffers**: **2,917.30 MB/s (98.01% hit ratio)** (5.75x speedup over uncached)

### D. Journaling Overhead Comparison (`reports/bench/journal_overhead.csv`)
- **Journal OFF**: 17,464 files/sec (0.057s for 1,000 files)
- **Journal ON (WAL ACID guarantees)**: 1,055 files/sec (0.947s for 1,000 transactions)

### E. Directory Scale vs. Lookup Latency (`reports/bench/dir_lookup_scale.csv`)
- **10 entries**: 2.20 µs
- **100 entries**: 3.66 µs
- **1,000 entries**: 21.84 µs
- **10,000 entries**: 69.71 µs

## 4. Test Execution Summary
- **Unit Suite (`tests/test_metrics.c`)**: 7 / 7 passed
  - `m13_test1_timer_sanity`: Monotonic timer verification with sleep tolerance testing.
  - `m13_test2_percentiles_calculation`: Exact min, max, avg, p50, and p99 mathematical verification across 1,000 samples.
  - `m13_test3_synthetic_workload_exact_counters`: Exact 100-block file creation, hand-calculated utilization verification.
  - `m13_test4_health_vs_independent_computation`: Independent scratch scan of files, allocation sizes, and runs vs metrics engine.
  - `m13_test5_file_fragmentation_detection`: Intentional interleaved allocation verification of discontinuity counters.
  - `m13_test6_csv_and_report_output`: Parseable CSV export and summary report formatting.
  - `m13_test7_live_counters_and_reset`: Live counter incrementation and clean zeroing on reset.
- **Cumulative Regression Across M0–M13**: All 15 test suites + Golden E2E passed (87 / 87 tests).
- **Leak Freedom**: Built-in memory tracker reported 0 leaks across all runs.
