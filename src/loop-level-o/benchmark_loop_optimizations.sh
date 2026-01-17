#!/bin/bash
# benchmark_loop_optimizations.sh
# Compare baseline vs v4_loop_optimized for presentation

echo "====================================="
echo "Loop-Level Optimization Benchmarks"
echo "====================================="
echo ""

# Build both versions
echo "[1/5] Building baseline version..."
gcc -std=c11 -O0 -Wall -Wextra -Werror ../json2csv_baseline.c -o json2csv_baseline
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build baseline"
    exit 1
fi

echo "[2/5] Building loop-optimized version..."
gcc -std=c11 -O0 -Wall -Wextra -Werror v4_loop_optimized.c -o json2csv_v4
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to build v4"
    exit 1
fi

echo ""
echo "====================================="
echo "Correctness Test"
echo "====================================="

echo "[3/5] Testing correctness..."
./json2csv_baseline ../test.json > out_baseline.csv
./json2csv_v4 ../test.json > out_v4.csv

# Check if diff is available
if command -v diff &> /dev/null; then
    if diff -q out_baseline.csv out_v4.csv > /dev/null; then
        echo "✓ Correctness test PASSED - outputs match"
    else
        echo "✗ WARNING: Outputs differ!"
        diff out_baseline.csv out_v4.csv | head -20
    fi
else
    echo "⚠ diff not available - skipping correctness check"
    echo "  Install with: pacman -S diffutils"
    echo "  Or manually compare: out_baseline.csv and out_v4.csv"
fi

echo ""
echo "====================================="
echo "Performance Measurements"
echo "====================================="

# Timing measurements
echo "[4/5] Running timing benchmarks (benchmark.json)..."
echo ""
echo "Baseline version:"
time ./json2csv_baseline ../benchmark.json > /dev/null
echo ""
echo "Loop-optimized version:"
time ./json2csv_v4 ../benchmark.json > /dev/null

echo ""
echo "====================================="
echo "Instruction Count Analysis"
echo "====================================="
echo "[5/5] Measuring instruction counts with perf..."
echo ""

# Check if perf is available
if command -v perf &> /dev/null; then
    echo "Baseline instructions:"
    perf stat -e instructions ./json2csv_baseline ../benchmark.json > /dev/null 2>&1
    
    echo ""
    echo "Loop-optimized instructions:"
    perf stat -e instructions ./json2csv_v4 ../benchmark.json > /dev/null 2>&1
else
    echo "perf not available - skipping instruction count"
fi

echo ""
echo "====================================="
echo "Summary"
echo "====================================="
echo "Built: json2csv_baseline, json2csv_v4"
echo "Test outputs: out_baseline.csv, out_v4.csv"
echo ""
echo "To run detailed profiling:"
echo "  perf record -g ./json2csv_v4 benchmark.json > /dev/null"
echo "  perf report"
echo ""
echo "To compare branch predictions:"
echo "  perf stat -e branches,branch-misses ./json2csv_baseline benchmark.json > /dev/null"
echo "  perf stat -e branches,branch-misses ./json2csv_v4 benchmark.json > /dev/null"
