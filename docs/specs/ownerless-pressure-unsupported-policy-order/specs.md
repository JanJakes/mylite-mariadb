# Ownerless Pressure Unsupported Policy Order

## Problem

Ownerless active-reader pressure returns `MYLITE_BUSY` for supported write
statements when retained page-version WAL reaches the configured soft limit.
Unsupported ownerless SQL surfaces must remain explicit policy errors even when
the same statement shape would otherwise be a write.

Without focused coverage, future pressure-classifier changes could accidentally
mask deliberate unsupported-surface diagnostics with a generic pressure-limit
error.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks table-admin commands such as
  `SQLCOM_ANALYZE`, `SQLCOM_OPTIMIZE`, and `SQLCOM_REPAIR` as command classes
  that can write logs, auto-commit, pre-open temporary tables, or close handler
  state.
- `mariadb/sql/sql_admin.cc:mysql_admin_table()` is the shared path for
  `ANALYZE TABLE`, `CHECK TABLE`, `OPTIMIZE TABLE`, and `REPAIR TABLE`.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_LOCK_TABLES` and
  `SQLCOM_UNLOCK_TABLES` by entering or leaving MariaDB's connection-scoped
  locked-table mode.
- `mariadb/sql/sql_reload.cc` implements `FLUSH TABLES ... WITH READ LOCK` and
  `FLUSH TABLES ... FOR EXPORT` with global read-lock, locked-table, and
  export/quiesce behavior.
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... DISCARD TABLESPACE` and
  `ALTER TABLE ... IMPORT TABLESPACE`; `mariadb/sql/sql_table.cc` treats those
  as standalone alter-table operations.
- `packages/libmylite/src/database.cc:reject_unsupported_sql_policy()` rejects
  ownerless table-admin SQL, `LOCK TABLES`, flush lock/export, tablespace
  detach/import, and unproven table storage options with `MYLITE_ERROR`.
- `packages/libmylite/src/database.cc:exec_impl()` runs
  `reject_unsupported_sql_policy()` before
  `enforce_ownerless_page_log_limit_policy()`, so direct ownerless execution
  should preserve explicit unsupported-surface diagnostics under pressure.

## Scope And Non-Goals

In scope:

- Add retained-WAL pressure coverage proving unsupported ownerless SQL returns
  the explicit policy error instead of `MYLITE_BUSY`.
- Cover representative unsupported classes that overlap write-like SQL:
  `ANALYZE TABLE`, `LOCK TABLES`, `FLUSH TABLES ... WITH READ LOCK`,
  `ALTER TABLE ... DISCARD TABLESPACE`, and a rejected table storage option.
- Verify the rejected storage-option statement does not create a table.

Out of scope:

- Supporting any of the rejected SQL surfaces.
- Changing production execution order, unless the new regression test exposes
  a masking bug.
- Extending prepared-statement policy coverage; unsupported prepared SQL is
  already rejected during `mylite_prepare()`.
- External randomized pressure or RQG stress.

## Design

Extend the existing `active-reader-pressure-write-policy` selector:

1. Build the normal retained-WAL pressure state with a live repeatable-read
   snapshot pin.
2. Reopen a writer with `ownerless_page_log_limit_bytes` equal to the retained
   WAL size.
3. Keep the existing supported write-class checks that expect `MYLITE_BUSY`.
4. Add policy-error checks for representative unsupported ownerless SQL while
   pressure is active.
5. Assert the rejected storage-option table is absent.
6. Release the reader and keep the existing final ownerless/native reopen and
   forced `.shm` rebuild checks.

## Compatibility Impact

No SQL surface becomes supported. The compatibility claim is narrower and more
explicit: ownerless pressure throttling applies to supported writes, while
deliberately unsupported ownerless SQL continues to fail with its documented
policy diagnostic before pressure throttling is considered.

## Directory And Lifecycle Impact

No directory layout changes. The test continues to use the existing ownerless
page-version WAL, checkpoint, shared-memory rebuild, and native exclusive
reopen lifecycle.

## Native Storage Impact

No native storage format changes. Unsupported statements must not enter native
table-admin, locked-table, export, tablespace detach/import, or storage-option
file-layout paths under pressure.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL shard containing the selector in both presets.
- Run focused adjacent pressure stress or trace selectors if the change affects
  pressure runtime behavior.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Supported write statements still return `MYLITE_BUSY` with the pressure-limit
  diagnostic while retained WAL is at the configured limit.
- Representative unsupported ownerless statements return `MYLITE_ERROR`, have
  MariaDB errno zero, and include their explicit policy diagnostic while the
  same pressure limit is active.
- The rejected storage-option create statement leaves no table metadata.
- Existing post-pressure success and ownerless/native reopen checks still pass.

## Risks And Follow-Up

- This is representative deterministic SQL coverage, not an exhaustive
  unsupported-surface matrix.
- The remaining unsupported surfaces still require their own design work before
  they can be enabled in ownerless mode.
- Full external MariaDB/RQG stress remains planned separately.
