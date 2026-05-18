# Handler Savepoint Hooks

## Problem

MyLite supports direct and prepared savepoint SQL through the public
`libmylite` policy layer, but the MyLite handlerton still does not implement
MariaDB's native savepoint callbacks. That means raw embedded SQL paths that
reach MariaDB's transaction layer can report `SAVEPOINT` as unsupported after a
routed MyLite table participates in the transaction.

This slice adds bounded handler-level savepoint hooks for routed durable
MyLite row DML. It does not remove the existing `libmylite` savepoint path and
does not claim full transactional engine semantics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:1269-1306` defines per-savepoint handlerton storage
  through `savepoint_offset`, `savepoint_set`, `savepoint_rollback`, and
  `savepoint_release`.
- `mariadb/sql/handler.cc:3195-3240` calls every registered transaction
  participant's `savepoint_set`; missing hooks make `SAVEPOINT` unsupported.
- `mariadb/sql/handler.cc:3120-3186` rolls back participants that existed at
  the savepoint through `savepoint_rollback` and rolls back participants that
  were registered after the savepoint as whole transactions.
- `mariadb/sql/transaction.cc:609-690` replaces a duplicate savepoint by
  calling `ha_release_savepoint()` for the older savepoint before pushing the
  new savepoint.
- `mariadb/sql/transaction.cc:711-783` keeps a rolled-back target savepoint
  active and discards later savepoints; explicit release removes the target and
  later savepoints.
- `mariadb/storage/mylite/ha_mylite.cc` already owns a per-THD transaction
  context and storage transaction/statement checkpoints.

## Design

Install MyLite handlerton savepoint callbacks:

- `savepoint_offset` stores a `Mylite_savepoint_reference`, which points at a
  MyLite-owned savepoint frame in the per-connection transaction context.
- `savepoint_set` starts a nested MyLite storage checkpoint and pushes it onto
  the context's savepoint stack.
- `savepoint_rollback` rolls back frames down to the target frame, then creates
  a fresh frame for the same SQL savepoint so MariaDB's target savepoint remains
  usable after `ROLLBACK TO`.
- `savepoint_release` commits frames down to the target for explicit release.
  When MariaDB calls release while replacing a duplicate savepoint during
  `SAVEPOINT name`, MyLite detaches the SQL reference but leaves the older
  physical checkpoint in the stack so newer savepoints stay valid.
- Whole transaction commit, rollback, and connection close unwind any remaining
  handler-owned savepoint frames before finishing the outer transaction
  checkpoint.

`savepoint_rollback_can_release_mdl` stays conservative and returns false. The
current storage checkpoint can restore MyLite row state, but MyLite does not yet
claim transactional DDL or broad MDL-release safety.

## Compatibility Impact

Raw embedded MariaDB SQL can now execute native `SAVEPOINT`, `ROLLBACK TO
SAVEPOINT`, and `RELEASE SAVEPOINT` after routed durable `ENGINE=InnoDB` tables
participate in a transaction. Public `libmylite` direct and prepared savepoints
continue to use the existing MyLite-owned path.

Compatibility remains partial:

- `HA_NO_TRANSACTIONS` remains advertised.
- Fully transactional engine flags remain planned.
- Transactional DDL, isolation, XA, and WAL/checkpoint work remain planned or
  explicitly rejected.
- MEMORY/HEAP savepoint behavior is covered by the later
  [Volatile Row Transaction Snapshots](../volatile-row-transaction-snapshots/specs.md)
  slice.

## Single-File And Lifecycle Impact

No new durable companion files are introduced. Handler savepoints are nested
checkpoints over the same primary `.mylite` file and the existing transaction
journal behavior. Active savepoint frames are cleaned up on explicit transaction
completion and connection close.

## Test And Verification Plan

- Add a raw embedded MariaDB storage-smoke test that starts MariaDB directly
  with `--mylite-primary-file`, routes `ENGINE=InnoDB` to MyLite, and verifies:
  - native `SAVEPOINT` succeeds after routed row DML registers MyLite;
  - `ROLLBACK TO SAVEPOINT` removes later rows and keeps the target usable;
  - `RELEASE SAVEPOINT` preserves changes;
  - duplicate savepoint replacement preserves newer savepoints;
  - full transaction rollback unwinds active savepoint frames.
- Run the focused storage-smoke targets and transaction harness groups.
- Run shell syntax and whitespace checks.

## Acceptance Criteria

- MyLite installs handler-level savepoint hooks.
- Native MariaDB savepoint SQL works for routed durable MyLite row DML in the
  raw embedded path.
- Existing public direct/prepared savepoint tests remain green.
- Docs keep full transactional engine semantics listed as planned.
