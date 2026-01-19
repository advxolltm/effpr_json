# Compiler Optimizations & SIMD Limits

This directory contains experiments analyzing **compiler optimizations and SIMD auto-vectorization limits** for a JSON-to-CSV parser.

The goal is to evaluate whether **structural refactoring and compiler hints** can enable SIMD or improve performance without changing program semantics.

---

## Contents

- `json2csv_simd_barriers.c`  
  Baseline for Step 3.  
  Contains **structural refactoring of hot paths** to remove local SIMD barriers:
  - Branch-light character scanning  
  - Reduced control-flow complexity  
  - Simplified loop structure  
  No algorithmic changes.

- `branchlight_bench_vec_report.txt`  
  GCC vectorization diagnostics (`-fopt-info-vec-all`) for the Step 3 variant.  
  Shows that SIMD was attempted but **no loops were auto-vectorized**.

- `json2csv_compiler_hints.c`  
  Step 4 variant building on Step 3.  
  Adds **compiler hints** such as:
  - `restrict` qualifiers  
  - Cached struct members  
  - Inlining-friendly function structure  
  Aimed at reducing aliasing and improving scalar execution.

- `compiler_hints_bench_vec_report.txt`  
  GCC vectorization diagnostics for the compiler-hints variant.  
  Confirms that auto-vectorization still fails due to structural constraints.

---

## Summary

- SIMD auto-vectorization was not achieved  
- Structural refactoring removed local SIMD barriers but did not enable vectorization  
- Compiler hints produced **measurable performance gains (~2–3%)**  
- Improvements come from **better scalar optimization**, not SIMD

---