#!/bin/bash
set -euo pipefail

##############################################################################
# Configuration
##############################################################################
DATABASE_NAME="test"
SCALE=1000

# Warmup + measured runs
WARMUP_RUNS=0
MEASURED_RUNS=3
TOTAL_RUNS=$((WARMUP_RUNS + MEASURED_RUNS))

ALGORITHMS=(sieve_db clock sieve lru cflru lruwsr)

# ---- Add your actual PGDATA path ----
PGDATA="$HOME/pg19-test-data"

# ---- Use local SQL scripts automatically ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
READ_SQL="$SCRIPT_DIR/read.sql"
WRITE_SQL="$SCRIPT_DIR/write.sql"

# ---- Use your PostgreSQL port ----
PORT=5433

RESULTS_DIR="/home/aroma/projects/postgres18-2/test/results"
mkdir -p "$RESULTS_DIR"

# Duration (seconds)
DURATION=600

##############################################################################
# Helper: restart required once per algorithm
##############################################################################
restart_with_algo () {
  local algo="$1"
  local log="$2"

  echo "Setting eviction_algorithm = ${algo} (restart required) ..." | tee -a "$log"
  psql -p "$PORT" -d postgres -v ON_ERROR_STOP=1 \
    -c "ALTER SYSTEM SET eviction_algorithm = '${algo}';" >>"$log" 2>&1

  echo "Restarting PostgreSQL..." | tee -a "$log"
  pg_ctl -D "$PGDATA" -l "$HOME/pgbench_test.log" restart -w >>"$log" 2>&1

  # sanity: confirm setting took effect
  echo "Confirming eviction_algorithm after restart..." | tee -a "$log"
  psql -p "$PORT" -d postgres -v ON_ERROR_STOP=1 \
    -c "SHOW eviction_algorithm;" >>"$log" 2>&1
}

##############################################################################
for ALGO in "${ALGORITHMS[@]}"; do
  RESULT_LOG="$RESULTS_DIR/${ALGO}_result.log"
  : > "$RESULT_LOG"   # Clear the log for this algo

  echo "====================================================" | tee -a "$RESULT_LOG"
  echo "Eviction algorithm: $ALGO" | tee -a "$RESULT_LOG"
  echo "Warmup runs: $WARMUP_RUNS, Measured runs: $MEASURED_RUNS (Total: $TOTAL_RUNS)" | tee -a "$RESULT_LOG"
  echo "====================================================" | tee -a "$RESULT_LOG"

  # 0) Set algo + restart ONCE per algorithm
  restart_with_algo "$ALGO" "$RESULT_LOG"

  echo "Initializing pgbench tables ONCE (scale=$SCALE)..." | tee -a "$RESULT_LOG"
  pgbench -q -p "$PORT" -i -s "$SCALE" "$DATABASE_NAME" >>"$RESULT_LOG" 2>&1

  for i in $(seq 1 "$TOTAL_RUNS"); do
    echo "----------------------------------------------" | tee -a "$RESULT_LOG"
    if [ "$i" -le "$WARMUP_RUNS" ]; then
      echo "Warmup run #$i: $(date)" | tee -a "$RESULT_LOG"
    else
      echo "Measured run #$((i - WARMUP_RUNS)) (overall #$i): $(date)" | tee -a "$RESULT_LOG"
    fi

    # 2) Reset stats each run (ok)
    echo "Resetting database statistics..." | tee -a "$RESULT_LOG"
    psql -p "$PORT" -d "$DATABASE_NAME" -v ON_ERROR_STOP=1 \
      -c "SELECT pg_stat_reset();" >>"$RESULT_LOG" 2>&1

    # 3) Run pgbench
    if [ "$i" -le "$WARMUP_RUNS" ]; then
      echo "Running pgbench WARMUP for $DURATION seconds (results will be discarded)..." | tee -a "$RESULT_LOG"
      pgbench -p "$PORT" -f "$READ_SQL"@1 -f "$WRITE_SQL"@1 -T "$DURATION" "$DATABASE_NAME" \
        >>"$RESULT_LOG" 2>&1
      echo "Warmup run #$i finished: $(date)" | tee -a "$RESULT_LOG"
      continue
    fi

    echo "Running pgbench MEASURED for $DURATION seconds..." | tee -a "$RESULT_LOG"
    pgbench -p "$PORT" -f "$READ_SQL"@1 -f "$WRITE_SQL"@1 -T "$DURATION" "$DATABASE_NAME" \
      | tee -a "$RESULT_LOG"

    # 4) Cache hit ratio (measured runs only)
    echo "Collecting cache hit statistics..." | tee -a "$RESULT_LOG"
    HIT_STATS=$(psql -p "$PORT" -d "$DATABASE_NAME" -t -c "
      SELECT sum(blks_hit) AS total_blks_hit,
             sum(blks_read) AS total_blks_read,
             to_char(
               sum(blks_hit)*1.0 / NULLIF(sum(blks_hit+blks_read), 0),
               'FM999.9999'
             ) AS hit_ratio
        FROM pg_stat_database
       WHERE datname = '$DATABASE_NAME';
    ")
    echo "Cache hit stats (blks_hit | blks_read | hit_ratio): $HIT_STATS" | tee -a "$RESULT_LOG"

    echo "Measured run #$((i - WARMUP_RUNS)) finished: $(date)" | tee -a "$RESULT_LOG"
  done

  echo "All tests completed for $ALGO! Results are in $RESULT_LOG." | tee -a "$RESULT_LOG"
done

echo "All algorithms completed!"

