#!/bin/bash
# Measurement script for baseline branch behavior
# Run this on a Linux machine with perf installed

PROGRAM="./json2csv_baseline"
INPUT="benchmark.json"
OUTPUT="/dev/null"

echo "==================================================================="
echo "Branch Behavior Measurement - Baseline"
echo "==================================================================="
echo ""

# Compile
echo "[1/4] Compiling baseline..."
gcc -std=c11 -O2 -Wall -Wextra -Werror json2csv_baseline.c -o json2csv_baseline
echo "✓ Compiled successfully"
echo ""

# Basic timing
echo "[2/4] Measuring execution time..."
echo "Running 5 iterations for average..."
for i in {1..5}; do
    /usr/bin/time -f "Run $i: %e seconds (user: %U, sys: %S)" ./$PROGRAM $INPUT > $OUTPUT 2>&1
done
echo ""

# Branch statistics
echo "[3/4] Measuring branch behavior..."
perf stat -e branches,branch-misses,branch-loads,branch-load-misses,instructions,cycles \
    ./$PROGRAM $INPUT > $OUTPUT 2>&1 | grep -E "(branches|instructions|cycles|elapsed)"
echo ""

# Detailed profiling
echo "[4/4] Profiling hot functions..."
perf record -g --call-graph dwarf ./$PROGRAM $INPUT > $OUTPUT 2>&1
perf report --stdio --no-children | head -50
echo ""

echo "==================================================================="
echo "Branch miss rate calculation:"
echo "  Branch miss rate = (branch-misses / branches) * 100%"
echo ""
echo "Typical values:"
echo "  < 1%  = excellent"
echo "  1-3%  = good"
echo "  3-5%  = moderate (room for optimization)"
echo "  > 5%  = poor (significant optimization potential)"
echo "==================================================================="
