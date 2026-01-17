# benchmark_loop_optimizations.ps1
# Automated benchmark for loop-level optimizations (Windows)

Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Loop-Level Optimization Benchmarks" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host ""

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
    Write-Host "  gcc -std=c11 -O0 -Wall ../json2csv_baseline.c -o json2csv_baseline.exe" -ForegroundColor Gray
    Write-Host "  gcc -std=c11 -O0 -Wall v4_loop_optimized.c -o json2csv_v4.exe" -ForegroundColor Gray
    exit 1
}

# Build both versions
Write-Host ""
Write-Host "[1/5] Building baseline version..." -ForegroundColor Yellow
& $gccPath -std=c11 -O0 -Wall ../json2csv_baseline.c -o json2csv_baseline.exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to build baseline" -ForegroundColor Red
    exit 1
}

Write-Host "[2/5] Building loop-optimized version..." -ForegroundColor Yellow
& $gccPath -std=c11 -O0 -Wall v4_loop_optimized.c -o json2csv_v4.exe
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Failed to build v4" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Correctness Test" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan

Write-Host "[3/5] Testing correctness..." -ForegroundColor Yellow
.\json2csv_baseline.exe ..\test.json > out_baseline.csv
.\json2csv_v4.exe ..\test.json > out_v4.csv

$diff = Compare-Object (Get-Content out_baseline.csv) (Get-Content out_v4.csv)
if ($null -eq $diff) {
    Write-Host "? Correctness test PASSED - outputs match" -ForegroundColor Green
} else {
    Write-Host "? WARNING: Outputs differ!" -ForegroundColor Red
    $diff | Select-Object -First 20
}

Write-Host ""
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Performance Measurements" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan

# Timing measurements
Write-Host "[4/5] Running timing benchmarks..." -ForegroundColor Yellow
Write-Host ""
Write-Host "Baseline version:" -ForegroundColor White
$baselineTime = Measure-Command { .\json2csv_baseline.exe ..\benchmark.json | Out-Null }
Write-Host "  Time: $($baselineTime.TotalSeconds) seconds" -ForegroundColor Cyan

Write-Host ""
Write-Host "Loop-optimized version:" -ForegroundColor White
$optimizedTime = Measure-Command { .\json2csv_v4.exe ..\benchmark.json | Out-Null }
Write-Host "  Time: $($optimizedTime.TotalSeconds) seconds" -ForegroundColor Cyan

Write-Host ""
$speedup = $baselineTime.TotalSeconds / $optimizedTime.TotalSeconds
Write-Host "Speedup: $($speedup.ToString('0.00'))x" -ForegroundColor Green

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
Write-Host "=====================================" -ForegroundColor Cyan
Write-Host "Benchmark Complete!" -ForegroundColor Cyan
Write-Host "=====================================" -ForegroundColor Cyan
