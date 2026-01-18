# benchmark_loop_optimizations.ps1
# Automated benchmark for loop-level optimizations with detailed metrics (Windows)

param(
    [string]$OptLevel = "-O0"
)

# Normalize optimization level
if ($OptLevel -eq "O0" -or $OptLevel -eq "-O0") {
    $OptLevel = "-O0"
} elseif ($OptLevel -eq "O3" -or $OptLevel -eq "-O3") {
    $OptLevel = "-O3"
} else {
    $OptLevel = "-O0"
}

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Loop-Level Optimization Benchmarks" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Optimization Level: $OptLevel" -ForegroundColor Blue
Write-Host ""

# Configuration
$RUNS = 3
$DATA_FILE = "..\benchmark.json"
$TEST_FILE = "..\test.json"

# Check for data file
if (-not (Test-Path $DATA_FILE)) {
    Write-Host "Warning: $DATA_FILE not found!" -ForegroundColor Yellow
    Write-Host "Using test.json instead for benchmarking" -ForegroundColor Yellow
    $DATA_FILE = $TEST_FILE
}

# Try to find gcc
$gccPath = $null
$gccCandidates = @(
    "gcc",
    "C:\msys64\ucrt64\bin\gcc.exe",
    "C:\msys64\mingw64\bin\gcc.exe",
    "C:\MinGW\bin\gcc.exe"
)

foreach ($candidate in $gccCandidates) {
    try {
        $result = & $candidate --version 2>&1
        if ($LASTEXITCODE -eq 0) {
            $gccPath = $candidate
            Write-Host "Found GCC: $gccPath" -ForegroundColor Green
            break
        }
    } catch {
        continue
    }
}

if (-not $gccPath) {
    Write-Host "ERROR: gcc not found" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please install GCC (MSYS2 recommended):" -ForegroundColor Yellow
    Write-Host "  https://www.msys2.org/" -ForegroundColor Gray
    Write-Host ""
    Write-Host "Or build manually:" -ForegroundColor Yellow
    Write-Host "  gcc -std=c11 $OptLevel -Wall ../json2csv_baseline.c -o json2csv_baseline.exe" -ForegroundColor Gray
    Write-Host "  gcc -std=c11 $OptLevel -Wall v4_loop_optimized.c -o json2csv_v4.exe" -ForegroundColor Gray
    exit 1
}

# Build both versions
Write-Host ""
Write-Host "[1/4] Building baseline version ($OptLevel)..." -ForegroundColor Yellow
& $gccPath -std=c11 $OptLevel -Wall ../json2csv_baseline.c -o json2csv_baseline.exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to build baseline" -ForegroundColor Red
    exit 1
}

Write-Host "[2/4] Building loop-optimized version ($OptLevel)..." -ForegroundColor Yellow
& $gccPath -std=c11 $OptLevel -Wall v4_loop_optimized.c -o json2csv_v4.exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to build v4" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Correctness Test" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan

Write-Host "[3/4] Testing correctness..." -ForegroundColor Yellow
.\json2csv_baseline.exe $TEST_FILE > out_baseline.csv
.\json2csv_v4.exe $TEST_FILE > out_v4.csv

$diff = Compare-Object (Get-Content out_baseline.csv) (Get-Content out_v4.csv)
if ($null -eq $diff) {
    Write-Host "✓ Correctness test PASSED - outputs match" -ForegroundColor Green
} else {
    Write-Host "✗ WARNING: Outputs differ!" -ForegroundColor Red
    $diff | Select-Object -First 20
    exit 1
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Performance Measurements" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan

Write-Host "[4/4] Running detailed benchmarks..." -ForegroundColor Yellow
$dataSize = (Get-Item $DATA_FILE).Length / 1MB
Write-Host "Dataset: $DATA_FILE ($([math]::Round($dataSize, 1)) MB)" -ForegroundColor Blue
Write-Host "Runs per configuration: $RUNS" -ForegroundColor Blue
Write-Host ""

# Function to run benchmark
function Run-Benchmark {
    param(
        [string]$Name,
        [string]$Binary
    )
    
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    
    $times = @()
    
    for ($i = 1; $i -le $RUNS; $i++) {
        Write-Host "  Run $i/$RUNS... " -NoNewline
        
        $elapsed = Measure-Command { 
            & $Binary $DATA_FILE | Out-Null 
        }
        
        $timeSeconds = $elapsed.TotalSeconds
        $times += $timeSeconds
        
        Write-Host "$([math]::Round($timeSeconds, 3))s" -ForegroundColor Green
    }
    
    # Calculate average
    $avgTime = ($times | Measure-Object -Average).Average
    
    Write-Host ""
    Write-Host "  Average Runtime: $([math]::Round($avgTime, 3))s" -ForegroundColor Blue
    Write-Host ""
    
    return $avgTime
}

# Run benchmarks
$baselineTime = Run-Benchmark -Name "BASELINE" -Binary ".\json2csv_baseline.exe"
$optimizedTime = Run-Benchmark -Name "OPTIMIZED" -Binary ".\json2csv_v4.exe"

# Calculate speedup
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "RESULTS SUMMARY ($OptLevel)" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

$speedup = $baselineTime / $optimizedTime
$improvement = (($baselineTime - $optimizedTime) / $baselineTime) * 100

Write-Host "Runtime:" -ForegroundColor Blue
Write-Host "  Baseline:  $([math]::Round($baselineTime, 3))s" -ForegroundColor White
Write-Host "  Optimized: $([math]::Round($optimizedTime, 3))s" -ForegroundColor White
Write-Host "  Speedup:   $([math]::Round($speedup, 2))x ($([math]::Round($improvement, 1))% faster)" -ForegroundColor Green
Write-Host ""

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Summary" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Built: json2csv_baseline.exe, json2csv_v4.exe" -ForegroundColor Green
Write-Host "Test outputs: out_baseline.csv, out_v4.csv" -ForegroundColor Green
Write-Host ""
Write-Host "Optimizations Applied:" -ForegroundColor White
Write-Host "  1. Manual loop unrolling (4x)" -ForegroundColor Gray
Write-Host "  2. Code motion (hoisting invariants)" -ForegroundColor Gray
Write-Host "  3. Combined conditional tests" -ForegroundColor Gray
Write-Host "  4. Sentinel techniques for string scanning" -ForegroundColor Gray
Write-Host ""

Write-Host "To benchmark with -O3:" -ForegroundColor Cyan
Write-Host "  .\benchmark_loop_optimizations.ps1 -OptLevel O3" -ForegroundColor Gray
Write-Host ""

Write-Host "Note: For detailed CPU metrics (cycles, IPC, branches):" -ForegroundColor Yellow
Write-Host "  Use Linux/Cloud VM with 'perf' tool" -ForegroundColor Gray
Write-Host "  Or run ./benchmark_loop_optimizations.sh in WSL/MSYS2" -ForegroundColor Gray
Write-Host ""

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Benchmark Complete!" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
