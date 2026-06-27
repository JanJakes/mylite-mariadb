# Ownerless Index Metadata Live Recovery

## Problem

Ownerless dictionary crash coverage proved standalone secondary-index create,
drop, rename, and ignored/not-ignored metadata changes only after the final
live ownerless peer exited. That left a correctness gap for applications that
keep another ownerless process open while a peer dies after MariaDB has applied
the index DDL but before MyLite finishes the ownerless dictionary generation.

## Source Findings

- Base: MariaDB 11.8.6, `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_parse.cc` maps top-level `CREATE INDEX` and `DROP INDEX`
  into `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc` documents that `mysql_alter_table()` handles
  `ALTER TABLE` plus mapped `CREATE|DROP INDEX` statements and can execute
  in-place or copy-style changes.
- `mariadb/storage/innobase/handler/handler0alter.cc` explicitly handles index
  add/drop/rename name-clash cases and deletes InnoDB dictionary rows for
  dropped indexes.

## Design

- Add distinct ownerless dictionary recovery kinds for real index create, drop,
  create-or-replace, rename, and ignored/not-ignored changes.
- Classify only bounded SQL shapes with simple identifier key parts:
  top-level `CREATE [OR REPLACE] [UNIQUE] INDEX`, top-level `DROP INDEX`,
  `ALTER TABLE ... ADD [UNIQUE] INDEX`, `ALTER TABLE ... DROP INDEX`,
  `ALTER TABLE ... RENAME INDEX`, and `ALTER TABLE ... ALTER INDEX ... [NOT]
  IGNORED`.
- Use pre-execution `information_schema.statistics` and
  `information_schema.columns` checks to prove source indexes/columns exist and
  target indexes are absent where required.
- Treat real index create/drop/create-or-replace as native file-operation
  recovery and force the checkpoint marker when the native hook does not expose
  one before the dictionary finish hook.
- Treat index rename and ignored/not-ignored changes as metadata-only recovery:
  the live peer may finish the dictionary generation without retaining a native
  file-operation marker.

## Non-Goals

- No new support for `FULLTEXT`, `SPATIAL`, partitioned-table indexes,
  expression indexes, prefix lengths, generated-column index variants, or
  multi-clause mixed ALTER statements.
- No claim that the SQL-level table-lock fault-injection callback is reachable
  through these statements.

## Compatibility Impact

Supported MySQL/MariaDB-compatible index DDL behavior is unchanged. The slice
only changes crash recovery after successful ownerless index DDL and keeps
unsupported special index families on the existing explicit rejection path.

## Storage And Lifecycle Impact

Durable state remains in the MyLite database directory. Physical secondary
index build/drop/replacement retains the native file-operation checkpoint marker
while a peer is live and drains it on final no-live recovery. Metadata-only
rename and ignored/not-ignored recovery keeps that marker clear.

## Tests

- Primitive dictionary-state recovery-kind coverage for the five new kinds.
- Hook-build crash selectors for ordinary secondary-index create/drop,
  unique-index replace/drop, secondary-index rename, and secondary-index
  ignored/not-ignored metadata.
- Focused selectors assert live-peer recovery, marker retention or absence,
  post-recovery writes, ownerless/native reopen, and forced `.shm` rebuild.
- Broader verification should include adjacent idempotent index crash selectors,
  ownerless DDL stress, production focused runs, format, and diff checks.

## Acceptance Criteria

- A peer killed after successful bounded real index create/drop/replacement and
  before ownerless dictionary finish can be recovered while another ownerless
  peer remains live.
- The native file-operation marker remains set for physical index
  create/drop/replacement until the final live peer exits.
- A peer killed after bounded index rename or ignored/not-ignored metadata can
  be recovered while another ownerless peer remains live without setting the
  native file-operation marker.
- Final ownerless and native exclusive reopens observe the same index metadata
  and enforcement before and after forced shared-memory rebuild.
