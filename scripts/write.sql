\set naccounts 1000 * 100000
\set hotspot_max :naccounts / 10

BEGIN;

\set r random(1,100)
\if :r <= 90
  \set aid random(1, :hotspot_max)
\else
  \set aid random(:hotspot_max + 1, :naccounts)
\endif

UPDATE pgbench_accounts
   SET abalance = abalance + 1
 WHERE aid = :aid;

-- SELECT abalance
--   FROM pgbench_accounts
--  WHERE aid = :aid;

-- \set ntellers 1000 * 10
-- \set nbranches 1000
-- \set tid random(1, :ntellers)
-- \set bid random(1, :nbranches)

-- UPDATE pgbench_tellers
--    SET tbalance = tbalance + 1
--  WHERE tid = :tid;

-- UPDATE pgbench_branches
--    SET bbalance = bbalance + 1
--  WHERE bid = :bid;

-- INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
--      VALUES (:tid, :bid, :aid, 1, CURRENT_TIMESTAMP);

END;