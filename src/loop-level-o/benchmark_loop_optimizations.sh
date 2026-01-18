#!/bin/bash
# benchmark_loop_optimizations.sh
# Compare baseline vs v4_loop_optimized with detailed metrics

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}Loop-Level Optimization Benchmarks${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""

# Configuration
RUNS=3
DATA_FILE="../benchmark.json"
TEST_FILE="../test.json"

# Check for required files
if [ ! -f "$DATA_FILE" ]; then
    echo -e "${RED}Warning: $DATA_FILE not found!${NC}"
    echo "Using test.json instead for benchmarking"
    DATA_FILE="$TEST_FILE"
fi

# Detect optimization level from arguments
OPT_LEVEL="-O0"
if [ "$1" == "-O3" ] || [ "$1" == "O3" ]; then
    OPT_LEVEL="-O3"
    echo -e "${BLUE}Using optimization level: ${OPT_LEVEL}${NC}"
elif [ "$1" == "-O0" ] || [ "$1" == "O0" ]; then
    OPT_LEVEL="-O0"
    echo -e "${BLUE}Using optimization level: ${OPT_LEVEL}${NC}"
else
    echo -e "${BLUE}Using default optimization level: ${OPT_LEVEL}${NC}"
    echo -e "${CYAN}Tip: Use ./benchmark_loop_optimizations.sh -O3 for O3 benchmarks${NC}"
fi
echo ""

# Build both versions
echo -e "${YELLOW}[1/4] Building baseline version (${OPT_LEVEL})...${NC}"
gcc -std=c11 ${OPT_LEVEL} -Wall -Wextra ../json2csv_baseline.c -o json2csv_baseline
if [ $? -ne 0 ]; then
    echo -e "${RED}ERROR: Failed to build baseline${NC}"
    exit 1
fi

echo -e "${YELLOW}[2/4] Building loop-optimized version (${OPT_LEVEL})...${NC}"
gcc -std=c11 ${OPT_LEVEL} -Wall -Wextra v4_loop_optimized.c -o json2csv_v4
if [ $? -ne 0 ]; then
    echo -e "${RED}ERROR: Failed to build v4${NC}"
    exit 1
fi

echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}Correctness Test${NC}"
echo -e "${GREEN}=========================================${NC}"

echo -e "${YELLOW}[3/4] Testing correctness...${NC}"
./json2csv_baseline $TEST_FILE > out_baseline.csv 2>/dev/null
./json2csv_v4 $TEST_FILE > out_v4.csv 2>/dev/null

# Check if diff is available
if command -v diff &> /dev/null; then
    if diff -q out_baseline.csv out_v4.csv > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Correctness test PASSED - outputs match${NC}"
    else
        echo -e "${RED}✗ WARNING: Outputs differ!${NC}"
        diff out_baseline.csv out_v4.csv | head -20
        exit 1
    fi
else
    echo -e "${YELLOW}⚠ diff not available - skipping correctness check${NC}"
    echo "  Install with: sudo apt install diffutils (Ubuntu/Debian)"
    echo "  Or: pacman -S diffutils (MSYS2)"
fi

echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}Performance Measurements${NC}"
echo -e "${GREEN}=========================================${NC}"

echo -e "${YELLOW}[4/4] Running detailed benchmarks...${NC}"
echo -e "${BLUE}Dataset:${NC} $DATA_FILE ($(du -h $DATA_FILE 2>/dev/null | cut -f1 || echo 'unknown'))"
echo -e "${BLUE}Runs per configuration:${NC} $RUNS"
echo ""

# Function to extract metric from perf output
extract_metric() {
    local output="$1"
    local pattern="$2"
    local value=$(echo "$output" | grep "$pattern" | awk '{print $1}' | tr -d ',' | head -1)
    # Return 0 if empty or not a number
    if [ -z "$value" ] || ! [[ "$value" =~ ^[0-9]+$ ]]; then
        echo "0"
    else
        echo "$value"
    fi
}

# Function to run benchmark
run_benchmark() {
    local name=$1
    local binary=$2
    
    echo -e "${CYAN}=== $name ===${NC}"
    
    local total_time=0
    local total_cycles=0
    local total_instructions=0
    local total_branches=0
    local total_branch_misses=0
    
    # Check if perf is available
    local use_perf=false
    if command -v perf &> /dev/null; then
        # Test if perf works
        if perf stat -e cycles echo test > /dev/null 2>&1; then
            use_perf=true
        else
            echo -e "${YELLOW}  Note: perf available but may need permissions${NC}"
            echo -e "${YELLOW}  Run: sudo sysctl -w kernel.perf_event_paranoid=-1${NC}"
        fi
    fi
    
    for i in $(seq 1 $RUNS); do
        echo -ne "  Run $i/$RUNS... "
        
        if [ "$use_perf" = true ]; then
            # Run with perf
            local output=$(perf stat -e cycles,instructions,branches,branch-misses \
                $binary $DATA_FILE 2>&1 > /dev/null)
            
            local time=$(echo "$output" | grep "seconds time elapsed" | awk '{print $1}')
            local cycles=$(extract_metric "$output" "cycles")
            local instructions=$(extract_metric "$output" "instructions")
            local branches=$(extract_metric "$output" "branches")
            local branch_misses=$(extract_metric "$output" "branch-misses")
            
            # Safe addition with bc, handling empty values
            if [ -n "$time" ] && [ "$time" != "0" ]; then
                total_time=$(echo "$total_time + $time" | bc -l 2>/dev/null || echo "$total_time")
            fi
            if [ "$cycles" != "0" ]; then
                total_cycles=$(echo "$total_cycles + $cycles" | bc -l 2>/dev/null || echo "$total_cycles")
            fi
            if [ "$instructions" != "0" ]; then
                total_instructions=$(echo "$total_instructions + $instructions" | bc -l 2>/dev/null || echo "$total_instructions")
            fi
            if [ "$branches" != "0" ]; then
                total_branches=$(echo "$total_branches + $branches" | bc -l 2>/dev/null || echo "$total_branches")
            fi
            if [ "$branch_misses" != "0" ]; then
                total_branch_misses=$(echo "$total_branch_misses + $branch_misses" | bc -l 2>/dev/null || echo "$total_branch_misses")
            fi
            
            echo -e "${GREEN}${time}s${NC}"
        else
            # Fallback to time command
            local start=$(date +%s.%N)
            $binary $DATA_FILE > /dev/null 2>&1
            local end=$(date +%s.%N)
            local time=$(echo "$end - $start" | bc 2>/dev/null || echo "N/A")
            total_time=$(echo "$total_time + $time" | bc 2>/dev/null || echo "$total_time")
            echo -e "${GREEN}${time}s${NC}"
        fi
    done
    
    # Calculate averages
    if [ "$use_perf" = true ] && command -v bc &> /dev/null; then
        local avg_time=$(echo "scale=3; $total_time / $RUNS" | bc -l 2>/dev/null || echo "0")
        local avg_cycles=$(echo "scale=0; $total_cycles / $RUNS" | bc -l 2>/dev/null || echo "0")
        local avg_instructions=$(echo "scale=0; $total_instructions / $RUNS" | bc -l 2>/dev/null || echo "0")
        local avg_branches=$(echo "scale=0; $total_branches / $RUNS" | bc -l 2>/dev/null || echo "0")
        local avg_branch_misses=$(echo "scale=0; $total_branch_misses / $RUNS" | bc -l 2>/dev/null || echo "0")
        
        # Calculate IPC (only if both values are non-zero)
        local ipc="N/A"
        if [ "$avg_cycles" != "0" ] && [ "$avg_instructions" != "0" ]; then
            ipc=$(echo "scale=3; $avg_instructions / $avg_cycles" | bc -l 2>/dev/null || echo "N/A")
        fi
        
        # Calculate branch miss rate (only if branches > 0)
        local branch_miss_rate="N/A"
        if [ "$avg_branches" != "0" ] && [ "$avg_branch_misses" != "0" ]; then
            branch_miss_rate=$(echo "scale=2; ($avg_branch_misses / $avg_branches) * 100" | bc -l 2>/dev/null || echo "N/A")
        fi
        
        echo ""
        echo -e "  ${BLUE}Average Runtime:${NC}       ${avg_time}s"
        echo -e "  ${BLUE}Average Cycles:${NC}        $(printf "%'d" $avg_cycles 2>/dev/null || echo $avg_cycles)"
        echo -e "  ${BLUE}Average Instructions:${NC}  $(printf "%'d" $avg_instructions 2>/dev/null || echo $avg_instructions)"
        echo -e "  ${BLUE}IPC:${NC}                   ${ipc}"
        echo -e "  ${BLUE}Branches:${NC}              $(printf "%'d" $avg_branches 2>/dev/null || echo $avg_branches)"
        if [ "$branch_miss_rate" != "N/A" ]; then
            echo -e "  ${BLUE}Branch Misses:${NC}         $(printf "%'d" $avg_branch_misses 2>/dev/null || echo $avg_branch_misses) (${branch_miss_rate}%)"
        else
            echo -e "  ${BLUE}Branch Misses:${NC}         $(printf "%'d" $avg_branch_misses 2>/dev/null || echo $avg_branch_misses)"
        fi
        echo ""
        
        # Store results
        eval "${name}_time=$avg_time"
        eval "${name}_cycles=$avg_cycles"
        eval "${name}_instructions=$avg_instructions"
        eval "${name}_ipc=$ipc"
        eval "${name}_branches=$avg_branches"
        eval "${name}_branch_misses=$avg_branch_misses"
    else
        local avg_time=$(echo "scale=3; $total_time / $RUNS" | bc 2>/dev/null || echo "$total_time")
        echo ""
        echo -e "  ${BLUE}Average Runtime:${NC} ${avg_time}s"
        echo ""
        
        eval "${name}_time=$avg_time"
    fi
}

# Run benchmarks
run_benchmark "BASELINE" "./json2csv_baseline"
run_benchmark "OPTIMIZED" "./json2csv_v4"

# Calculate speedup
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}RESULTS SUMMARY (${OPT_LEVEL})${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""

if command -v bc &> /dev/null && [ ! -z "$BASELINE_time" ] && [ ! -z "$OPTIMIZED_time" ]; then
    speedup=$(echo "scale=3; $BASELINE_time / $OPTIMIZED_time" | bc -l 2>/dev/null || echo "N/A")
    
    echo -e "${BLUE}Runtime:${NC}"
    echo -e "  Baseline:  ${BASELINE_time}s"
    echo -e "  Optimized: ${OPTIMIZED_time}s"
    echo -e "  ${GREEN}Speedup:   ${speedup}x${NC}"
    echo ""
    
    if [ ! -z "$BASELINE_cycles" ] && [ ! -z "$OPTIMIZED_cycles" ] && [ "$BASELINE_cycles" != "0" ] && [ "$OPTIMIZED_cycles" != "0" ]; then
        cycle_reduction=$(echo "scale=2; (($BASELINE_cycles - $OPTIMIZED_cycles) / $BASELINE_cycles) * 100" | bc -l 2>/dev/null || echo "N/A")
        
        if [ ! -z "$BASELINE_instructions" ] && [ "$BASELINE_instructions" != "0" ] && [ "$OPTIMIZED_instructions" != "0" ]; then
            instruction_reduction=$(echo "scale=2; (($BASELINE_instructions - $OPTIMIZED_instructions) / $BASELINE_instructions) * 100" | bc -l 2>/dev/null || echo "N/A")
        else
            instruction_reduction="N/A"
        fi
        
        if [ ! -z "$BASELINE_branches" ] && [ "$BASELINE_branches" != "0" ] && [ "$OPTIMIZED_branches" != "0" ]; then
            branch_reduction=$(echo "scale=2; (($BASELINE_branches - $OPTIMIZED_branches) / $BASELINE_branches) * 100" | bc -l 2>/dev/null || echo "N/A")
        else
            branch_reduction="N/A"
        fi
        
        if [ ! -z "$BASELINE_branch_misses" ] && [ "$BASELINE_branch_misses" != "0" ] && [ "$OPTIMIZED_branch_misses" != "0" ]; then
            branch_miss_reduction=$(echo "scale=2; (($BASELINE_branch_misses - $OPTIMIZED_branch_misses) / $BASELINE_branch_misses) * 100" | bc -l 2>/dev/null || echo "N/A")
        else
            branch_miss_reduction="N/A"
        fi
        
        echo -e "${BLUE}Detailed Metrics:${NC}"
        echo -e "  Cycle Reduction:         ${cycle_reduction}%"
        if [ "$instruction_reduction" != "N/A" ]; then
            echo -e "  Instruction Reduction:   ${instruction_reduction}%"
        fi
        if [ "$branch_reduction" != "N/A" ]; then
            echo -e "  Branch Reduction:        ${branch_reduction}%"
        fi
        if [ "$branch_miss_reduction" != "N/A" ]; then
            echo -e "  Branch Miss Reduction:   ${branch_miss_reduction}%"
        fi
        if [ ! -z "$BASELINE_ipc" ] && [ "$BASELINE_ipc" != "N/A" ]; then
            echo -e "  IPC (Baseline):          ${BASELINE_ipc}"
        fi
        if [ ! -z "$OPTIMIZED_ipc" ] && [ "$OPTIMIZED_ipc" != "N/A" ]; then
            echo -e "  IPC (Optimized):         ${OPTIMIZED_ipc}"
        fi
        echo ""
    fi
fi

echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}Benchmark Complete!${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""
echo -e "${CYAN}Files generated:${NC}"
echo "  • json2csv_baseline, json2csv_v4 (executables)"
echo "  • out_baseline.csv, out_v4.csv (test outputs)"
echo ""
echo -e "${CYAN}To benchmark with -O3:${NC}"
echo "  ./benchmark_loop_optimizations.sh -O3"
echo ""
if ! command -v perf &> /dev/null; then
    echo -e "${YELLOW}Note: Install 'perf' for detailed CPU metrics${NC}"
    echo "  Ubuntu/Debian: sudo apt install linux-tools-\$(uname -r)"
    echo "  MSYS2: Not available (use WSL or cloud VM)"
fi
