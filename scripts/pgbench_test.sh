#!/bin/bash
set -euo pipefail

##############################################################################
# Configuration
##############################################################################

# PostgreSQL install + data
PGHOME="$HOME/pg18"
PGDATA="$HOME/pg19-test-data"
PORT=5433
DB="test"

# Experiment control
RUNS=3
SCALE=1000
DURATION=600          # seconds
CLIENTS=1
THREADS=1

# pgbench behavior
PROGRESS=60           # seconds between progress reports

# Test behavior
REINIT_EACH_RUN=1     # 0 = init once
RESTART_EACH_RUN=1    # restart server each run
RESET_STATS_EACH_RUN=1


# Workload
DURATION="${DURATION:-600}"
CLIENTS="${CLIENTS:-0}"
THREADS="${THREADS:-0}"
PROGRESS="${PROGRESS:-60}"
WARMUP_DURATION=60      # seconds
WARMUP_ENABLED=0        # 1=on, 0=off


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
READ_SQL="${READ_SQL:-$SCRIPT_DIR/read.sql}"
WRITE_SQL="${WRITE_SQL:-$SCRIPT_DIR/write.sql}"

# Behavior
REINIT_EACH_RUN="${REINIT_EACH_RUN:-1}"     # 0 = init once (recommended), 1 = init every run
RESTART_EACH_RUN="${RESTART_EACH_RUN:-1}"   # restart each run to reduce cache bias
RESET_STATS_EACH_RUN="${RESET_STATS_EACH_RUN:-1}"

LOGDIR="${LOGDIR:-$SCRIPT_DIR/results}"
mkdir -p "$LOGDIR"

##############################################################################
# Helpers
##############################################################################
psqlc() {
  "$PGHOME/bin/psql" -p "$PORT" -d "$DB" -X -Atq -c "$1"
}

psqlc_postgres() {
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -Atq -c "$1"
}

timestamp() { date +"%Y%m%d_%H%M%S"; }

##############################################################################
# Pre-flight
##############################################################################
if [[ ! -f "$READ_SQL" ]]; then echo "ERROR: missing $READ_SQL"; exit 1; fi
if [[ ! -f "$WRITE_SQL" ]]; then echo "ERROR: missing $WRITE_SQL"; exit 1; fi

ALGO="$(psqlc_postgres "SHOW eviction_algorithm;" | xargs || true)"
if [[ -z "${ALGO:-}" ]]; then ALGO="unknown_algo"; fi

RUN_TAG="${ALGO}_s${SCALE}_c${CLIENTS}_j${THREADS}_T${DURATION}_$(timestamp)"
RESULT_LOG="$LOGDIR/${RUN_TAG}.log"
PGCTL_LOG="$LOGDIR/${RUN_TAG}.pg_ctl.log"

echo "Writing results to: $RESULT_LOG"
echo "pg_ctl log to:      $PGCTL_LOG"
echo

{
  echo "============================================================"
  echo "Run tag:            $RUN_TAG"
  echo "Algorithm:          $ALGO"
  echo "PGHOME:             $PGHOME"
  echo "PGDATA:             $PGDATA"
  echo "PORT:               $PORT"
  echo "DB:                 $DB"
  echo "RUNS:               $RUNS"
  echo "SCALE:              $SCALE"
  echo "DURATION:           $DURATION"
  echo "CLIENTS/THREADS:    $CLIENTS / $THREADS"
  echo "READ_SQL:           $READ_SQL"
  echo "WRITE_SQL:          $WRITE_SQL"
  echo "REINIT_EACH_RUN:    $REINIT_EACH_RUN"
  echo "RESTART_EACH_RUN:   $RESTART_EACH_RUN"
  echo "RESET_STATS_EACH_RUN:$RESET_STATS_EACH_RUN"
  echo "Started at:         $(date)"
  echo "============================================================"
  echo
} | tee "$RESULT_LOG"

# Confirm config is active (important for your experiments)
{
  echo "=== Server identity ==="
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SELECT version();"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW data_directory;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW config_file;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW eviction_algorithm;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW shared_buffers;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW max_wal_size;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW checkpoint_timeout;"
  "$PGHOME/bin/psql" -p "$PORT" -d postgres -X -c "SHOW synchronous_commit;"
  echo
} | tee -a "$RESULT_LOG"

##############################################################################
# Initialize once (recommended)
##############################################################################
if [[ "$REINIT_EACH_RUN" -eq 0 ]]; then
  echo "=== One-time pgbench init (scale=$SCALE) on DB=$DB ===" | tee -a "$RESULT_LOG"
  "$PGHOME/bin/pgbench" -q -p "$PORT" -i -s "$SCALE" "$DB" | tee -a "$RESULT_LOG"
  echo | tee -a "$RESULT_LOG"
fi

##############################################################################
# Main Loop
##############################################################################
for i in $(seq 1 "$RUNS"); do
  {
    echo "=============================================="
    echo "Starting run #$i: $(date)"
    echo "Algorithm: $ALGO"
  } | tee -a "$RESULT_LOG"

  if [[ "$RESTART_EACH_RUN" -eq 1 ]]; then
    echo "Restarting PostgreSQL..." | tee -a "$RESULT_LOG"
    "$PGHOME/bin/pg_ctl" -D "$PGDATA" -l "$PGCTL_LOG" restart -m fast -w >>"$RESULT_LOG" 2>&1
  fi

  if [[ "$REINIT_EACH_RUN" -eq 1 ]]; then
    echo "Re-initializing pgbench tables (scale=$SCALE)..." | tee -a "$RESULT_LOG"
    "$PGHOME/bin/pgbench" -q -p "$PORT" -i -s "$SCALE" "$DB" >>"$RESULT_LOG" 2>&1
  fi

  if [[ "$RESET_STATS_EACH_RUN" -eq 1 ]]; then
    echo "Resetting stats..." | tee -a "$RESULT_LOG"
    "$PGHOME/bin/psql" -p "$PORT" -d "$DB" -X -c "SELECT pg_stat_reset();" >>"$RESULT_LOG" 2>&1
  fi

  # Warm-up (discarded), then reset stats again so measurements are clean
  if [[ "${WARMUP_ENABLED:-0}" -eq 1 ]]; then
    echo "Warm-up pgbench (discarded): -c $CLIENTS -j $THREADS -T $WARMUP_DURATION ..." | tee -a "$RESULT_LOG"
    "$PGHOME/bin/pgbench" -p "$PORT" \
      -c "$CLIENTS" -j "$THREADS" -T "$WARMUP_DURATION" \
      -f "$READ_SQL"@1 -f "$WRITE_SQL"@1 \
      "$DB" >/dev/null 2>&1

    if [[ "$RESET_STATS_EACH_RUN" -eq 1 ]]; then
      echo "Resetting stats after warm-up..." | tee -a "$RESULT_LOG"
      "$PGHOME/bin/psql" -p "$PORT" -d "$DB" -X -c "SELECT pg_stat_reset();" >>"$RESULT_LOG" 2>&1
    fi
  fi

  # Baseline stats before the run
  HIT_BEFORE="$(psqlc "SELECT coalesce(sum(blks_hit),0)||'|'||coalesce(sum(blks_read),0) FROM pg_stat_database WHERE datname=current_database();")"
  BGW_BEFORE="$(psqlc_postgres "SELECT buffers_clean||'|'||maxwritten_clean||'|'||buffers_alloc FROM pg_stat_bgwriter;")"

  echo "Running pgbench: -c $CLIENTS -j $THREADS -T $DURATION ..." | tee -a "$RESULT_LOG"
  "$PGHOME/bin/pgbench" -p "$PORT" \
    -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
    -f "$READ_SQL"@1 -f "$WRITE_SQL"@1 \
    "$DB" | tee -a "$RESULT_LOG"

  # 5) Retrieve and log cache hit statistics
  echo "Collecting cache hit statistics..." | tee -a "$RESULT_LOG"
  HIT_STATS=$(psql -p "$PORT" -d "$DB" -t -c "
    SELECT sum(blks_hit) AS total_blks_hit,
           sum(blks_read) AS total_blks_read,
           to_char(
             sum(blks_hit)*1.0 / NULLIF(sum(blks_hit+blks_read), 0),
             'FM999.9999'
           ) AS hit_ratio
      FROM pg_stat_database
     WHERE datname = '$DB';
  ")

  echo "Cache hit stats (blks_hit | blks_read | hit_ratio): $HIT_STATS" | tee -a "$RESULT_LOG"
  
  
  {
    echo "Finished run #$i: $(date)"
  } | tee -a "$RESULT_LOG"

  echo | tee -a "$RESULT_LOG"
done

echo "All tests completed. Results: $RESULT_LOG"

