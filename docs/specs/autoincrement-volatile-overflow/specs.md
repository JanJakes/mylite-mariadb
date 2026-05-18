# Autoincrement Volatile Overflow

## Goal

Cover `AUTO_INCREMENT` overflow behavior for MyLite-routed `ENGINE=MEMORY` and
`ENGINE=HEAP` tables, where rows and autoincrement state are runtime-volatile
but table metadata is durable.

## Non-Goals

- Do not change MEMORY/HEAP volatility semantics.
- Do not claim exhaustive integer-width or offset/increment coverage.
- Do not cover native MEMORY hash-index parity, memory limits, replication, or
  transaction/savepoint semantics.
- Do not change the MyLite file format or public API.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:handler::update_auto_increment()` returns
  `HA_ERR_AUTOINC_ERANGE` when a generated value exceeds the field maximum
  before row publication.
- `mariadb/sql/handler.cc:compute_next_insert_id()` rounds generated values to
  the active `auto_increment_offset + N * auto_increment_increment` sequence.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::get_auto_increment()` reads
  volatile autoincrement state through `mylite_volatile_read_auto_increment()`
  for MEMORY/HEAP tables.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` advances
  volatile autoincrement state through
  `mylite_volatile_advance_auto_increment()` after successful writes.
- `mariadb/storage/mylite/mylite_volatile_rows.cc:mylite_volatile_truncate_table()`
  clears volatile rows and resets volatile autoincrement state to `1`.
- The existing `memory-engine-routing` slice records MariaDB MEMORY semantics:
  table definitions survive restart, while rows are in-memory and empty after
  restart.

## Compatibility Impact

This moves representative temporary/volatile autoincrement overflow coverage
from planned to partial for MyLite-routed MEMORY/HEAP tables. It does not
change the durable-row autoincrement guarantees.

## Design

Add storage-engine smoke coverage for:

- an explicit `ENGINE=MEMORY` unsigned `TINYINT` autoincrement table whose
  active runtime accepts `254` and `255`, then rejects the next generated row;
- an explicit `ENGINE=HEAP` unsigned `SMALLINT` autoincrement table that rounds
  near `65535` under `auto_increment_offset=5` and
  `auto_increment_increment=10`, accepts `65535`, then rejects the next
  generated row;
- close/reopen behavior showing the same table metadata survives, volatile rows
  are empty, and the MEMORY table's generated value starts again from `1`.

## File Lifecycle

No durable row or index pages are introduced for MEMORY/HEAP rows. The primary
`.mylite` file stores only table metadata for these tables; runtime rows and
autoincrement state remain process-local volatile state.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary SQL inserts before and after closing the embedded runtime.

## Storage-Engine Routing

The coverage applies to explicit `ENGINE=MEMORY` and `ENGINE=HEAP` declarations
that route to the MyLite handler with MEMORY/HEAP requested-engine metadata.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced.

## Test Plan

- Extend `mylite_embedded_storage_engine_test` with volatile overflow coverage.
- Re-run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- MEMORY and HEAP tables reject generated overflow without publishing the
  failed row.
- MEMORY/HEAP rows remain absent from durable MyLite row storage.
- Reopen preserves metadata, clears volatile rows, and resets volatile
  autoincrement state.
- Docs narrow the remaining autoincrement gap to exhaustive integer-width
  matrices.

## Risks And Unresolved Questions

- This remains representative smoke coverage; exhaustive volatile
  offset/increment matrices are still out of scope.
- `BIGINT UNSIGNED` maximum-state handling is covered by the separate
  `autoincrement-bigint-unsigned-maximum` slice.
