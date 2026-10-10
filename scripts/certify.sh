#!/usr/bin/env bash
set -euo pipefail

echo "=========================================================="
echo "      MINIFS SYSTEM CERTIFICATION & HARDENING PIPELINE    "
echo "=========================================================="

# 1. Clean & Build
echo "--> [1/5] Clean build all modules, tools, tests..."
make clean
make dirs
make all

# 2. Cumulative Unit & Regression Tests (all 18 test binaries)
echo "--> [2/5] Running cumulative unit & regression suites..."
make test

# 3. Model-based Fuzzing (High iteration test)
echo "--> [3/5] Running model-based randomized differential fuzzer..."
./build/bin/fuzz --ops 10000 --seed 123456
./build/bin/fuzz --ops 10000 --seed 654321

# 4. Benchmark Suite Execution
echo "--> [4/5] Executing performance benchmark suite..."
./scripts/bench.sh

# 5. Static Analysis & Diagnostics Check
echo "--> [5/5] Static analysis and compiler diagnostic validation..."
gcc -std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion -fsyntax-only -Iinclude src/*.c tools/*.c tests/*.c
echo "Compiler syntax and diagnostics: 0 warnings, 0 errors."

echo "=========================================================="
echo "    CERTIFICATION COMPLETE: ALL GATES PASSED (v1.0.0)     "
echo "=========================================================="
