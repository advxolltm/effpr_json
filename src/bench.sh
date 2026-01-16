#!/usr/bin/env bash
set -euo pipefail

# ===== Config =====
INPUT="${INPUT:-benchmark.json}"
BASE="${BASE:-./json2csv_O2g}"
OPT="${OPT:-./json2csv_buf_O2g}"      # optional (skip experiments if missing)
TARGET_SEC="${TARGET_SEC:-4}"         # target duration for perf loop
RUNS="${RUNS:-5}"                     # perf stat repeats
BUFS="${BUFS:-4096 16384 65536 262144 1048576}"
BATCHES="${BATCHES:-4096 16384 65536 262144 1048576}"
OUTDIR="${OUTDIR:-bench_out}"
# ==================

mkdir -p "$OUTDIR"

[[ -f "$INPUT" ]] || { echo "Missing INPUT: $INPUT"; exit 1; }
[[ -x "$BASE"  ]] || { echo "Missing BASE binary: $BASE"; exit 1; }

# Measure single-run real time safely (time writes to stderr, so use -o)
single_real_sec() {
  local tmp="$1"; shift
  /usr/bin/time -f "%e" -o "$tmp" "$@" > /dev/null 2>/dev/null || true
  cat "$tmp" 2>/dev/null || echo "0"
}

compute_n() {
  # n ~= TARGET_SEC / single; guard against 0/empty
  awk -v s="$1" -v t="$2" 'BEGIN{
    if (s+0 <= 0) { print 1; exit }
    n = int((t/s) + 0.5);
    if (n < 1) n = 1;
    print n
  }'
}

echo "== Single-run baseline (time -v) ==" | tee "$OUTDIR/01_time_single.txt"
{ /usr/bin/time -v "$BASE" "$INPUT" > /dev/null; } 2>> "$OUTDIR/01_time_single.txt"

SINGLE="$(single_real_sec "$OUTDIR/.single.tmp" "$BASE" "$INPUT")"
N="$(compute_n "$SINGLE" "$TARGET_SEC")"
echo "single_real=${SINGLE}s  target~${TARGET_SEC}s  => N=$N" | tee "$OUTDIR/02_loop_params.txt"

echo "== Looped baseline time ==" | tee "$OUTDIR/03_time_loop.txt"
{ /usr/bin/time -f "real=%e user=%U sys=%S" bash -c \
  'for i in $(seq 1 '"$N"'); do '"$BASE"' '"$INPUT"' > /dev/null; done'; } \
  2>> "$OUTDIR/03_time_loop.txt"

echo "== perf stat baseline ==" | tee "$OUTDIR/04_perfstat_base.txt"
{ perf stat -r "$RUNS" bash -c \
  'for i in $(seq 1 '"$N"'); do '"$BASE"' '"$INPUT"' > /dev/null; done'; } \
  2>> "$OUTDIR/04_perfstat_base.txt"

echo "== perf record/report baseline (top 60) ==" | tee "$OUTDIR/05_perfreport_base_head60.txt"
if perf record -F 999 -g --output "$OUTDIR/base.perf.data" -- bash -c \
   'for i in $(seq 1 '"$N"'); do '"$BASE"' '"$INPUT"' > /dev/null; done' \
   > /dev/null 2> "$OUTDIR/05_perf_record_base.err"
then
  perf report --stdio --input "$OUTDIR/base.perf.data" 2>/dev/null | head -n 60 \
    | tee -a "$OUTDIR/05_perfreport_base_head60.txt" >/dev/null
else
  echo "perf record failed (see 05_perf_record_base.err)" | tee -a "$OUTDIR/05_perfreport_base_head60.txt" >/dev/null
fi

# ---- Optional: buffered-output experiments ----
if [[ -x "$OPT" ]]; then
  echo "== Single-run OPT (time -v) ==" | tee "$OUTDIR/06_time_single_opt.txt"
  { /usr/bin/time -v "$OPT" "$INPUT" > /dev/null; } 2>> "$OUTDIR/06_time_single_opt.txt"

  echo "== Buffer size sweep (OUTBUF=OUTBATCH) ==" > "$OUTDIR/07_buf_sweep.txt"
  for b in $BUFS; do
    /usr/bin/time -f "%e" -o "$OUTDIR/.t.tmp" env OUTBUF="$b" OUTBATCH="$b" "$OPT" "$INPUT" > /dev/null 2>/dev/null || true
    t="$(cat "$OUTDIR/.t.tmp" 2>/dev/null || echo NA)"
    echo "BUF=$b  real=$t" | tee -a "$OUTDIR/07_buf_sweep.txt" >/dev/null
  done

  echo "== Batch sweep (fixed large OUTBUF, vary OUTBATCH) ==" > "$OUTDIR/08_batch_sweep.txt"
  BUF_FIXED="$(echo "$BUFS" | awk '{print $NF}')"
  for batch in $BATCHES; do
    /usr/bin/time -f "%e" -o "$OUTDIR/.t.tmp" env OUTBUF="$BUF_FIXED" OUTBATCH="$batch" "$OPT" "$INPUT" > /dev/null 2>/dev/null || true
    t="$(cat "$OUTDIR/.t.tmp" 2>/dev/null || echo NA)"
    echo "BUF=$BUF_FIXED  BATCH=$batch  real=$t" | tee -a "$OUTDIR/08_batch_sweep.txt" >/dev/null
  done
else
  echo "OPT binary not found ($OPT). Skipping buffer/batch experiments." | tee "$OUTDIR/06_opt_skipped.txt"
fi

echo "Done. Results in $OUTDIR"
