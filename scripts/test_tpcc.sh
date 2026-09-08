#!/bin/bash

# Usage: ./test_tpcc.sh <algorithm> <buffer_size>
# Example: ./test_tpcc.sh sieve_protected 500MB

if [ $# -ne 2 ]; then
    echo "Usage: $0 <algorithm> <buffer_size>"
    echo "Example: $0 sieve_protected 500MB"
    echo "Algorithms: clock, clock2bit, sieve, sieve_protected, sieve_db"
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

BENCHBASE_DIR="$HOME/benchbase/target/benchbase-postgres"
CONFIG_FILE="$BENCHBASE_DIR/config/postgres/my_tpcc_config.xml"

BASE_DIR="$HOME/TPCC"
RESULTS_BASE="${BASE_DIR}/results_${ALGORITHM}_${BUFFER_SIZE}"
mkdir -p "$BASE_DIR"

echo "========================================================================"
echo "TPC-C Testing: $ALGORITHM with $BUFFER_SIZE"
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
max_wal_size = '32GB'
checkpoint_timeout = '30min'
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
psql -p "$PORT" -d postgres -c "DROP DATABASE IF EXISTS tpcc;"
psql -p "$PORT" -d postgres -c "CREATE DATABASE tpcc;"

echo "Loading TPC-C data (500 warehouses)..."
cd "$BENCHBASE_DIR"
java -jar benchbase.jar -b tpcc -c "$CONFIG_FILE" --create=true --load=true

psql -p "$PORT" -d tpcc -c "SELECT pg_size_pretty(pg_database_size('tpcc'));"

##############################################################################
# RUN BENCHMARKS — 5 runs, cache-drop between each, no restart between runs
##############################################################################

declare -a TPS_ARR LAT_ARR HIT_ARR IO_WRITES_ARR IO_EVICTIONS_ARR

for run in 1 2 3 4 5; do
    RUN_DIR="${RESULTS_BASE}_run${run}"
    rm -rf "$RUN_DIR"
    mkdir -p "$RUN_DIR"

    echo ""
    echo "========================================================================"
    echo "RUN $run of 5"
    echo "========================================================================"

    psql -p "$PORT" -d tpcc -c "SELECT pg_stat_reset(); SELECT pg_stat_reset_shared('bgwriter'); SELECT pg_stat_reset_shared('io');"
    sync && echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
    sleep 3

    cd "$BENCHBASE_DIR"
    java -jar benchbase.jar -b tpcc -c "$CONFIG_FILE" --execute=true -d "$RUN_DIR"

    SUMMARY_JSON=$(ls -t "$RUN_DIR"/*.summary.json 2>/dev/null | head -1)
    if [ -n "$SUMMARY_JSON" ]; then
        TPS=$(grep -oP '"Throughput \(requests/second\)"\s*:\s*\K[0-9.]+' "$SUMMARY_JSON" | head -1)
        LAT=$(grep -oP '"Average Latency \(microseconds\)"\s*:\s*\K[0-9.]+' "$SUMMARY_JSON" | head -1)
    else
        TPS="NA"
        LAT="NA"
    fi

    psql -p "$PORT" -d tpcc -c "
    SELECT
        sum(blks_read) as blks_read,
        sum(blks_hit) as blks_hit,
        round(100.0 * sum(blks_hit) / nullif(sum(blks_hit) + sum(blks_read), 0), 2) as cache_hit_pct
    FROM pg_stat_database WHERE datname = 'tpcc';
    " | tee "$RUN_DIR/cache_stats.txt"

    HIT=$(psql -p "$PORT" -d tpcc -Atqc "
      SELECT round(100.0 * sum(blks_hit) / nullif(sum(blks_hit) + sum(blks_read), 0), 2)
      FROM pg_stat_database WHERE datname = 'tpcc';
    ")

    psql -p "$PORT" -d tpcc -c "
    SELECT
        sum(writes) as total_dirty_writes,
        sum(evictions) as total_evictions
    FROM pg_stat_io
    WHERE object = 'relation';
    " | tee "$RUN_DIR/dirty_writes.txt"

    IO_ROW=$(psql -p "$PORT" -d tpcc -Atqc "
      SELECT COALESCE(sum(writes),0), COALESCE(sum(evictions),0)
      FROM pg_stat_io WHERE object = 'relation';
    ")
    IFS='|' read -r IO_WRITES IO_EVICTIONS <<< "$IO_ROW"

    psql -p "$PORT" -d tpcc -c "
    SELECT
        backend_type,
        context,
        writes,
        evictions
    FROM pg_stat_io
    WHERE object = 'relation' AND (writes > 0 OR evictions > 0)
    ORDER BY backend_type, context;
    " | tee "$RUN_DIR/io_stats.txt"

    RESULT_LINE="Run #$run -> TPS: $TPS | Latency(us): $LAT | Cache hit: ${HIT}% | dirty_writes: $IO_WRITES | evictions: $IO_EVICTIONS"
    echo "$RESULT_LINE" | tee "$RUN_DIR/run_summary.txt"

    TPS_ARR+=("$TPS")
    LAT_ARR+=("$LAT")
    HIT_ARR+=("$HIT")
    IO_WRITES_ARR+=("$IO_WRITES")
    IO_EVICTIONS_ARR+=("$IO_EVICTIONS")

    echo "Run $run complete."
done

##############################################################################
# AVERAGE SUMMARY (Run1-5 and Run2-5)
##############################################################################
AVG15_TPS=$(printf '%s\n' "${TPS_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG15_LAT=$(printf '%s\n' "${LAT_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG15_HIT=$(printf '%s\n' "${HIT_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.2f", sum/n}')
AVG15_WRITES=$(printf '%s\n' "${IO_WRITES_ARR[@]}" | awk '{sum+=$1; n++} END {printf "%.1f", sum/n}')

AVG25_TPS=$(printf '%s\n' "${TPS_ARR[@]:1}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG25_LAT=$(printf '%s\n' "${LAT_ARR[@]:1}" | awk '{sum+=$1; n++} END {printf "%.3f", sum/n}')
AVG25_HIT=$(printf '%s\n' "${HIT_ARR[@]:1}" | awk '{sum+=$1; n++} END {printf "%.2f", sum/n}')
AVG25_WRITES=$(printf '%s\n' "${IO_WRITES_ARR[@]:1}" | awk '{sum+=$1; n++} END {printf "%.1f", sum/n}')

SUMMARY_FILE="${BASE_DIR}/${ALGORITHM}_${BUFFER_SIZE}_AVERAGE_summary.txt"
{
  echo "========================================================================"
  echo "TPC-C — Algorithm: $ALGORITHM  Buffer: $BUFFER_SIZE"
  echo "========================================================================"
  echo "  Avg(Run2-5) TPS:          $AVG25_TPS"
  echo "  Avg(Run2-5) Latency(us):  $AVG25_LAT"
  echo "  Avg(Run2-5) Cache Hit %:  ${AVG25_HIT}%"
  echo "  Avg(Run2-5) Dirty Writes: $AVG25_WRITES"
  echo "  ------------------------------------------------------"
  echo "  Avg(Run1-5) TPS:          $AVG15_TPS"
  echo "  Avg(Run1-5) Latency(us):  $AVG15_LAT"
  echo "  Avg(Run1-5) Cache Hit %:  ${AVG15_HIT}%"
  echo "  Avg(Run1-5) Dirty Writes: $AVG15_WRITES"
  echo "Finished: $(date)"
  echo "========================================================================"
} | tee "$SUMMARY_FILE"

echo ""
echo "========================================================================"
echo "ALL TESTS COMPLETE! Summary: $SUMMARY_FILE"
echo "========================================================================"
