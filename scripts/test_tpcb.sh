#!/bin/bash

# Usage: ./test_tpcb.sh <algorithm> <buffer_size>
# Example: ./test_tpcb.sh sieve_protected 1GB

if [ $# -ne 2 ]; then
    echo "Usage: $0 <algorithm> <buffer_size>"
    echo "Example: $0 sieve_protected 1GB"
    echo "Algorithms: clock, lru, cflru, lruwsr, sieve, sieve_db, sieve_protected, clock2bit"
    exit 1
fi

ALGORITHM=$1
BUFFER_SIZE=$2

##############################################################################
# Chameleon environment (this node)
##############################################################################
export PGHOME="$HOME/pg18"
export PGDATA="$HOME/pg19-test-data"
export PATH="$PGHOME/bin:$PATH"
PORT=5433

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
READ_SQL="$SCRIPT_DIR/read.sql"
WRITE_SQL="$SCRIPT_DIR/write.sql"

SCALE=7000
NACCOUNTS=$((SCALE * 100000))

BASE_DIR="$HOME/TPCB/102GB_DB"
RESULTS_BASE="${BASE_DIR}/results_${ALGORITHM}_${BUFFER_SIZE}"

echo "========================================================================"
echo "TPC-B Testing: $ALGORITHM with $BUFFER_SIZE"
echo "========================================================================"

##############################################################################
# SETUP — fresh cluster per combo
##############################################################################

echo "Stopping PostgreSQL..."
pg_ctl -D "$PGDATA" stop -m fast || true
sleep 3

echo "Removing old data directory..."
rm -rf "$PGDATA"

echo "Initializing new database cluster..."
initdb -D "$PGDATA"

echo "Configuring PostgreSQL..."
cat > "$PGDATA/postgresql.conf" << EOF
port = $PORT
shared_buffers = '$BUFFER_SIZE'
max_wal_size = '8GB'
checkpoint_timeout = '15min'
checkpoint_completion_target = 0.9
wal_compression = on
eviction_algorithm = '$ALGORITHM'
autovacuum = off
maintenance_work_mem = '128MB'
work_mem = '16MB'
effective_cache_size = '8GB'
max_connections = 100
EOF

echo "Starting PostgreSQL..."
pg_ctl -D "$PGDATA" -l "$PGDATA/server.log" start
sleep 5

CONFIRMED_ALGO=$(psql -p "$PORT" -d postgres -Atqc "SHOW eviction_algorithm;")
CONFIRMED_BUF=$(psql -p "$PORT" -d postgres -Atqc "SHOW shared_buffers;")
echo "Confirmed eviction_algorithm: $CONFIRMED_ALGO"
echo "Confirmed shared_buffers:     $CONFIRMED_BUF"

if [[ "$CONFIRMED_ALGO" != "$ALGORITHM" ]]; then
    echo "ERROR: algorithm mismatch (wanted $ALGORITHM, got $CONFIRMED_ALGO), aborting."
    exit 1
fi

echo "Creating database..."
psql -p "$PORT" -d postgres -c "DROP DATABASE IF EXISTS test;"
psql -p "$PORT" -d postgres -c "CREATE DATABASE test;"

echo "Loading pgbench data (scale $SCALE)..."
pgbench -p "$PORT" -i -s "$SCALE" test

psql -p "$PORT" -d test -c "SELECT pg_size_pretty(pg_database_size('test'));"

##############################################################################
# RUN BENCHMARKS — 5 runs, cache-drop between each, no restart between runs
##############################################################################

declare -a TPS_ARR LAT_ARR HIT_ARR IO_WRITES_ARR IO_EVICTIONS_ARR

for run in 1 2 3 4 5; do
    RUN_DIR="${RESULTS_BASE}_run${run}"
    mkdir -p "$RUN_DIR"

    echo ""
    echo "========================================================================"
    echo "RUN $run of 5"
    echo "========================================================================"

    psql -p "$PORT" -d test -c "SELECT pg_stat_reset(); SELECT pg_stat_reset_shared('bgwriter'); SELECT pg_stat_reset_shared('io');"
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sleep 3

    pgbench -p "$PORT" -D naccounts="$NACCOUNTS" -f "$READ_SQL"@1 -f "$WRITE_SQL"@1 -T 600 test | tee "$RUN_DIR/pgbench_output.txt"

    TPS=$(grep -oP '(?<=tps = )[0-9.]+' "$RUN_DIR/pgbench_output.txt" | head -1)
    LAT=$(grep -oP '(?<=latency average = )[0-9.]+' "$RUN_DIR/pgbench_output.txt" | head -1)

    psql -p "$PORT" -d test -c "
    SELECT
        sum(blks_read) as blks_read,
        sum(blks_hit) as blks_hit,
        round(100.0 * sum(blks_hit) / nullif(sum(blks_hit) + sum(blks_read), 0), 2) as cache_hit_pct
    FROM pg_stat_database WHERE datname = 'test';
    " | tee "$RUN_DIR/cache_stats.txt"

    HIT=$(psql -p "$PORT" -d test -Atqc "
      SELECT round(100.0 * sum(blks_hit) / nullif(sum(blks_hit) + sum(blks_read), 0), 2)
      FROM pg_stat_database WHERE datname = 'test';
    ")

    psql -p "$PORT" -d test -c "
    SELECT
        sum(writes) as total_dirty_writes,
        sum(evictions) as total_evictions
    FROM pg_stat_io
    WHERE object = 'relation';
    " | tee "$RUN_DIR/dirty_writes.txt"

    IO_ROW=$(psql -p "$PORT" -d test -Atqc "
      SELECT COALESCE(sum(writes),0), COALESCE(sum(evictions),0)
      FROM pg_stat_io WHERE object = 'relation';
    ")
    IFS='|' read -r IO_WRITES IO_EVICTIONS <<< "$IO_ROW"

    psql -p "$PORT" -d test -c "
    SELECT
        backend_type,
        context,
        writes,
        evictions
    FROM pg_stat_io
    WHERE object = 'relation' AND (writes > 0 OR evictions > 0)
    ORDER BY backend_type, context;
    " | tee "$RUN_DIR/io_stats.txt"

    RESULT_LINE="Run #$run -> TPS: $TPS | Latency: ${LAT}ms | Cache hit: ${HIT}% | dirty_writes: $IO_WRITES | evictions: $IO_EVICTIONS"
    echo "$RESULT_LINE" | tee "$RUN_DIR/run_summary.txt"

    TPS_ARR+=("$TPS")
    LAT_ARR+=("$LAT")
    HIT_ARR+=("$HIT")
    IO_WRITES_ARR+=("$IO_WRITES")
    IO_EVICTIONS_ARR+=("$IO_EVICTIONS")

    echo "Run $run complete."
done

##############################################################################
# AVERAGE SUMMARY
##############################################################################
AVG_TPS=$(printf '%s\n' "${TPS_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG_LAT=$(printf '%s\n' "${LAT_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG_HIT=$(printf '%s\n' "${HIT_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.2f", sum/n}')
AVG_IO_WRITES=$(printf '%s\n' "${IO_WRITES_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.1f", sum/n}')
AVG_IO_EVICTIONS=$(printf '%s\n' "${IO_EVICTIONS_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.1f", sum/n}')

SUMMARY_FILE="${BASE_DIR}/${ALGORITHM}_${BUFFER_SIZE}_AVERAGE_summary.txt"
{
  echo "========================================================================"
  echo "AVERAGE over 5 runs — Algorithm: $ALGORITHM  Buffer: $BUFFER_SIZE"
  echo "========================================================================"
  echo "  Avg TPS:          $AVG_TPS"
  echo "  Avg Latency:      ${AVG_LAT} ms"
  echo "  Avg Cache Hit %:  ${AVG_HIT}%"
  echo "  Avg Dirty Writes: $AVG_IO_WRITES  (pg_stat_io, object=relation)"
  echo "  Avg Evictions:    $AVG_IO_EVICTIONS  (pg_stat_io, object=relation)"
  echo "Finished: $(date)"
  echo "========================================================================"
} | tee "$SUMMARY_FILE"

echo ""
echo "========================================================================"
echo "ALL TESTS COMPLETE! Summary: $SUMMARY_FILE"
echo "========================================================================"
