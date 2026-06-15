# SIEVE_DB: A Lightweight & Fast Page Replacement Policy for Database Bufferpools

SIEVE_DB is a database-aware, scan-resistant page replacement policy implemented in PostgreSQL 18. It is developed by Aroma Hoque and Tarikul Islam Papon at the University of Massachusetts Boston.

## Overview

SIEVE_DB is a write-efficient adaptation of the SIEVE web cache eviction algorithm, redesigned for database bufferpools. It introduces a lightweight protection mechanism that distinguishes hot OLTP pages from scan pages, while preserving SIEVE's core properties: minimal metadata, lazy promotion, quick demotion, and no queue updates on cache hits.

This repository contains the full implementation of SIEVE_DB and all baseline eviction policies — **CLOCK** (PostgreSQL default), **LRU**, **CFLRU**, **LRU-WSR**, and vanilla **SIEVE** — implemented in PostgreSQL 18.

## Key Results

- Up to **5.6% higher throughput** over CLOCK under multi-threaded execution
- Up to **31.4% fewer dirty writes** over CLOCK at 8 threads
- Maintains scan resistance under YCSB with up to 25% scan workloads
- Consistently reduces disk writes across TPC-B (51 GB and 102 GB), TPC-C (50 GB), and real-world MSR Cambridge traces

## Repository Structure

```
SIEVE_DB/
├── src/
│   ├── backend/storage/buffer/
│   │   ├── freelist.c        # Eviction policy dispatch and SIEVE_DB implementation
│   │   ├── bufmgr.c          # Buffer manager with eviction hook points
│   │   └── buf_init.c        # Buffer pool initialization
│   └── include/storage/
│       ├── eviction.h        # Eviction policy interface (new file)
│       └── buf_internals.h   # Buffer descriptor with SIEVE_DB metadata fields
├── scripts/
│   ├── pgbench_all_algo.sh   # Run all algorithms sequentially (TPC-B)
│   ├── pgbench_test.sh       # Single algorithm benchmark runner
│   ├── read.sql              # Read workload for pgbench
│   └── write.sql             # Write workload for pgbench
└── configs/
    └── postgresql.conf       # PostgreSQL configuration used in experiments
```

## Implementation Details

SIEVE_DB extends each buffer descriptor with three lightweight fields:
- `visited` bit — set on first access since insertion
- `protected` bit — set on second access (indicates sustained reuse)
- `pass_counter` — tracks protection age for the aging mechanism

All eviction policies share a common FIFO queue in `freelist.c`. Policy selection is controlled via the `eviction_algorithm` GUC parameter.

## Building from Source

### Prerequisites

- Ubuntu 22.04 or later
- GCC, Make, and standard build tools
- PostgreSQL build dependencies

```bash
sudo apt-get install -y build-essential libreadline-dev zlib1g-dev \
    flex bison libxml2-dev libxslt-dev libssl-dev libsystemd-dev
```

### Build and Install

```bash
# Set paths
export PGHOME="$HOME/pg18"
export PGDATA="$HOME/pg19-test-data"
export PG_SRC="$HOME/projects/postgres18-2/postgres"
export PATH="$PGHOME/bin:$PATH"

# Configure and build
cd "$PG_SRC"
./configure --prefix="$PGHOME" --enable-debug --enable-cassert CFLAGS="-O0 -g3"
make -j$(nproc)
make install

# Verify
"$PGHOME/bin/postgres" -V
```

### Initialize and Configure

```bash
# Initialize a fresh cluster
"$PGHOME/bin/initdb" -D "$PGDATA"

# Configure PostgreSQL
cat >> "$PGDATA/postgresql.conf" << EOF
port = 5433
shared_buffers = 1GB
max_wal_size = 8GB
checkpoint_timeout = 15min
checkpoint_completion_target = 0.9
eviction_algorithm = sieve_db
EOF

# Start the server
"$PGHOME/bin/pg_ctl" -D "$PGDATA" -l "$PGDATA/server.log" start

# Verify
"$PGHOME/bin/psql" -p 5433 -d postgres -c "SHOW eviction_algorithm;"
```

## Configuring the Eviction Algorithm

Set `eviction_algorithm` in `postgresql.conf` and restart the server:

```
eviction_algorithm = sieve_db    # options: clock, lru, cflru, lruwsr, sieve, sieve_db
```

Restart after changing:

```bash
"$PGHOME/bin/pg_ctl" -D "$PGDATA" restart -m fast
```

Verify:

```bash
"$PGHOME/bin/psql" -p 5433 -d postgres -c "SHOW eviction_algorithm;"
```

## Running Experiments

### TPC-B (pgbench)

Create and load the database:

```bash
"$PGHOME/bin/psql" -p 5433 -d postgres -c "CREATE DATABASE test;"
"$PGHOME/bin/pgbench" -p 5433 -i -s 3500 test   # ~51 GB database
```

Run all algorithms sequentially:

```bash
cd scripts/
chmod +x pgbench_all_algo.sh
./pgbench_all_algo.sh
```

Run a single algorithm:

```bash
chmod +x pgbench_test.sh
./pgbench_test.sh
```

Before each run, clear the OS page cache:

```bash
sync && echo 3 | sudo tee /proc/sys/vm/drop_caches
```

### TPC-C (BenchBase)

```bash
# Create database
psql -p 5433 -d postgres -c "CREATE DATABASE tpcc;"

# Load data (500 warehouses, ~50 GB)
cd ~/benchbase/target/benchbase-postgres
java -jar benchbase.jar -b tpcc -c config/postgres/my_tpcc_config.xml --create=true --load=true

# Run benchmark
java -jar benchbase.jar -b tpcc -c config/postgres/my_tpcc_config.xml --execute=true -d ~/results/
```

## Experimental Setup

Experiments were conducted on two bare-metal nodes on [Chameleon Cloud](https://www.chameleoncloud.org/) hosted at TACC:

- **Node**: Dell PowerEdge R730
- **CPU**: Two Intel Xeon E5-2670 v3 (24 physical cores, 48 logical CPUs)
- **Memory**: 503 GiB DDR4
- **Storage**: 1.8 TB NVMe SSD
- **OS**: Ubuntu 22.04
- **PostgreSQL**: 18.0 (modified)

## SIEVE_DB Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `eviction_algorithm` | `clock` | Active eviction policy |
| Protection age `A` | `2` | Number of complete buffer pool passes before protection expires |

The protection age `A` controls the tradeoff between write efficiency and eviction aggressiveness. `A=2` is recommended as it achieves the best throughput with meaningful write reduction across both OLTP and scan-heavy workloads.

## Citation

If you use this code in your research, please cite:

```bibtex
@inproceedings{sievedb2027,
  title     = {{SIEVE\_DB}: A Lightweight \& Fast Page Replacement Policy for Database Bufferpools},
  author    = {Hoque, Aroma and Papon, Tarikul Islam},
  booktitle = {Proceedings of the International Conference on Extending Database Technology (EDBT)},
  year      = {2027}
}
```

## License

This project is based on PostgreSQL which is released under the [PostgreSQL License](https://www.postgresql.org/about/licence/). Modifications made for SIEVE_DB follow the same license.
