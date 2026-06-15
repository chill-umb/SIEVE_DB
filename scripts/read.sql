\set naccounts 1000 * 100000
\set hotspot_max :naccounts / 10

BEGIN;

\set r random(1,100)
\if :r <= 90
  \set aid random(1, :hotspot_max)
\else
  \set aid random(:hotspot_max + 1, :naccounts)
\endif

SELECT abalance
  FROM pgbench_accounts
 WHERE aid = :aid;

END;