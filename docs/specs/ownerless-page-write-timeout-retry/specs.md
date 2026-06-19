# Ownerless Page-Write Timeout Retry

## Problem

The amplified `ownerless-stress` preset exposed two related ownerless
page-write timeout failures:

- concurrent DDL stress could hit InnoDB's `trx->error_state == DB_SUCCESS`
  transaction-start assertion after `TRUNCATE TABLE` completed and the next
  `SELECT COUNT(*)` started with stale `DB_LOCK_WAIT_TIMEOUT`;
- same-name temporary-table stress could segfault while creating an InnoDB
  temporary table because `dict_hdr_get_new_id()` received a null dictionary
  header block after the buffer pre-read ownerless page-write hook returned
  `DB_LOCK_WAIT_TIMEOUT`.

Both failures came from treating low-level ownerless page-write lock timeouts
as SQL statement errors in call paths that cannot safely propagate a SQL error.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` ownerless page-write entry waits
  on the directory-owned page-write registry before publishing or refreshing
  X/SX page writes. The MTR hooks return ownership/refresh decisions, not a
  SQL-layer error code.
- `mariadb/storage/innobase/buf/buf0buf.cc` pre-read page-write locking can
  return an error to `buf_page_get_gen()`, but callers such as
  `dict_hdr_get()` in `mariadb/storage/innobase/dict/dict0boot.cc` assume
  system dictionary pages are always readable and immediately dereference the
  returned block.
- `mariadb/storage/innobase/dict/dict0boot.cc` `dict_hdr_get_new_id()` allocates
  table/index/space IDs during table and index creation, including the
  temporary-table create stack observed in the stress failure.
- MyLite's SQL-visible ownerless pressure behavior is implemented before SQL
  execution in `packages/libmylite/src/database.cc` through the
  page-version WAL pressure limit and ownerless dictionary/table statement
  policy. Those paths return `MYLITE_BUSY` with explicit diagnostics and have
  focused pressure-policy coverage.

## Scope And Non-Goals

In scope:

- Treat ownerless page-write lock timeout from the MTR and buffer pre-read
  hooks as internal physical-page contention that retries after refresh.
- Preserve ownerless deadlock handling, startup/recovery exits, page refresh,
  and SQL-visible pressure policy.
- Prove amplified DDL and temporary-table stress no longer crash.

Out of scope:

- Changing user-visible `lock_wait_timeout` behavior for native row/table
  locks.
- Changing `mylite_open_config.ownerless_page_log_limit_bytes` pressure
  semantics.
- Solving broader native redo/checkpoint reconciliation, DDL/file-lifecycle
  recovery, or external randomized RQG stress.

## Design

The ownerless page-write registry is a physical-page coordination layer between
independent process-local InnoDB buffer pools. A timeout at this layer means the
process did not acquire the synthetic page-write resource within the configured
wait window. It is not, by itself, a safe SQL statement error because the MTR
and buffer pre-read hooks sit below call sites that either lack an error return
path or assume mandatory InnoDB system pages are available.

The MTR page-write timeout policy therefore returns false for "abort the SQL
statement." Existing loops continue to refresh external page state and retry,
or leave during startup/recovery. Deadlock handling still marks the transaction
when the registry reports a real deadlock and the caller cannot safely break
the cycle by releasing transaction-scoped page-write ownership.

The buffer pre-read timeout policy also returns false. This prevents
`buf_page_get_gen()` from returning a null block solely because a transient
ownerless page-write timeout occurred before reading a required page such as
the InnoDB dictionary header. The pre-read path still uses existing refresh and
deadlock handling.

SQL-visible throttling remains in the MyLite statement policy layer. Retained
page-version WAL pressure, dictionary statement-lock busy, and table-write
statement-lock busy continue to return explicit `MYLITE_BUSY` before execution
or at prepared-step boundaries where MyLite can roll back and report an error
cleanly.

## Compatibility Impact

Successful DDL and temporary-table statements no longer inherit an internal
ownerless page-write timeout as stale InnoDB transaction error state, and
mandatory dictionary page reads no longer turn such a timeout into a null-page
crash. User-visible ownerless pressure limits and native InnoDB lock
wait/deadlock behavior are unchanged.

## Database Directory And Lifecycle Impact

No file-format, directory-layout, or durable-state changes. The existing
directory-owned page-write registry, page-version WAL, checkpoint, and
statement-lock files keep their current layout.

## Native Storage Impact

The change affects native InnoDB ownerless coordination only. It preserves
native InnoDB temporary-table creation, dictionary ID allocation, DDL file
lifecycle, and page refresh semantics while avoiding unsafe low-level error
propagation.

## Public API Impact

No public C API, PHP API, mysqli, SQL syntax, or configuration changes.

## Binary Size Impact

Negligible. The implementation removes SQL-command classification in two
ownerless timeout helpers and adds short explanatory comments.

## Test Plan

- Rebuild the MariaDB embedded archive and the ownerless-stress SQL test.
- Run amplified ownerless DDL stress:
  `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`.
- Run amplified ownerless temporary-table stress:
  `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-temporary-stress$'
  --output-on-failure`.
- Run ownerless active-reader pressure write-policy coverage to prove
  SQL-visible pressure `MYLITE_BUSY` still fires.
- Run the full `ownerless-stress` preset and focused ownerless SQL coverage.
- Run formatting and diff checks.

## Acceptance Criteria

- Amplified DDL stress does not hit stale `DB_LOCK_WAIT_TIMEOUT` transaction
  state.
- Amplified temporary-table stress does not segfault in InnoDB dictionary ID
  allocation.
- Ownerless pressure-policy tests still return `MYLITE_BUSY` for blocked DML
  and DDL classes.
- Documentation distinguishes low-level page-write contention retry from
  user-visible ownerless pressure throttling.

## Verification

- `tools/mariadb-embedded-build build` passed after rebuilding
  `mariadb/storage/innobase/buf/buf0buf.cc` and relinking the embedded
  MariaDB archive.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-ddl-stress$' --output-on-failure`
  passed in 68.62 seconds.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-cross-process-temporary-stress$'
  --output-on-failure` passed in 4.25 seconds.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12 stress
  cases in 395.76 seconds.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` passed all 16 ownerless SQL shards in 161.30 seconds.
- `build/php-embedded-prod/packages/libmylite/
  mylite_ownerless_cross_process_sql_test active-reader-pressure-write-policy`
  passed.
- `build/php-embedded-prod/packages/libmylite/
  mylite_ownerless_cross_process_sql_test active-reader-pressure-limit` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_negative_proof_test
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure` passed all 4 tests in 13.25 seconds.
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure` passed in 2.80 seconds.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Risks And Follow-Up

- A permanently stalled ownerless page-write resource can now wait/retry at the
  low-level hook instead of surfacing through that hook as a SQL timeout. The
  existing dead-owner recovery and statement-lock policy remain the intended
  user-visible protection; broader hung-writer diagnostics are still planned.
- Full external MariaDB/RQG stress remains planned. This slice only proves the
  in-process amplified ownerless stress paths.
