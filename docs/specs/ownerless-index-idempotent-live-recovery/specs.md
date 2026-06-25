# Ownerless Index Idempotent Live Recovery

## Problem Statement

Ownerless top-level and ALTER secondary-index idempotent crash selectors already
prove duplicate `CREATE INDEX IF NOT EXISTS`, missing `DROP INDEX IF EXISTS`,
duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS`, and missing
`ALTER TABLE ... DROP INDEX IF EXISTS` no-op recovery. They still use no-live
recovery: a live ownerless peer keeps cleanup busy until the peer exits.

These no-op branches do not create, remove, or rewrite native InnoDB index
files. MyLite should recover the completed MariaDB dictionary boundary while
another ownerless peer remains live, but only when pre-execution metadata proves
the statement is the no-op branch. Missing-create and existing-drop index
statements remain mutating DDL and are outside this metadata-only slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses top-level `CREATE INDEX` with
  `create_or_replace INDEX_SYM opt_if_not_exists`, and top-level `DROP INDEX`
  with `opt_if_exists_table_element`.
- `mariadb/sql/sql_yacc.yy:key_def` parses `ALTER TABLE ... ADD INDEX` through
  `key_or_index opt_if_not_exists`, and `alter_list_item` parses
  `DROP key_or_index opt_if_exists_table_element field_ident`.
- `mariadb/sql/sql_table.cc:handle_if_exists_options()` removes duplicate
  `ADD KEY IF NOT EXISTS` and missing `DROP INDEX IF EXISTS` work items before
  native execution, preserving the current table/index metadata while returning
  success with diagnostics.

## Design

- Add two MyLite dictionary recovery kinds:
  - duplicate index create/add no-op,
  - missing index drop no-op.
- Add conservative recovery classifiers for:
  - `CREATE INDEX IF NOT EXISTS <index> ON <table> (...)`,
  - `DROP INDEX IF EXISTS <index> ON <table>`,
  - `ALTER TABLE <table> ADD INDEX IF NOT EXISTS <index> (...)`,
  - `ALTER TABLE <table> DROP INDEX IF EXISTS <index>`.
- Each classifier queries `information_schema.statistics` before native
  execution:
  - create/add is marked recoverable only if the named index already exists,
  - drop is marked recoverable only if the named index is already absent.
- Metadata lookup failure is conservative: the statement is not promoted to the
  metadata-only live-recovery lane.
- Keep unique, primary-key, foreign-key, full-text, spatial/vector,
  generated-column, prefix/direction/online-option, multi-action ALTER, and
  replacement index forms outside this slice.
- Treat the new recovery kinds as metadata-only, so cleanup can finish the
  dictionary generation while a peer remains live and the native file-operation
  checkpoint marker stays clear.
- Promote the four existing secondary-index idempotent crash selectors to a
  held-live-peer recovery path and register them as standalone hook CTests for
  visible timing and failure attribution.

## Scope

In scope:

- Duplicate top-level `CREATE INDEX IF NOT EXISTS` no-op live recovery.
- Missing top-level `DROP INDEX IF EXISTS` no-op live recovery.
- Duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS` no-op live recovery.
- Missing `ALTER TABLE ... DROP INDEX IF EXISTS` no-op live recovery.
- Preserved index key-part metadata, forced-index reads, duplicate-name retry
  diagnostics, ownerless/native reopen, and forced `.shm` rebuild checks.

Out of scope:

- Missing index create/add that actually creates a native index.
- Existing index drop that actually removes a native index.
- Unique-index, primary-key, foreign-key, full-text, spatial/vector, generated
  column, prefix/direction, online-option, replacement, and multi-action ALTER
  crash variants.
- SQL-level table-lock fault injection.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice narrows an ownerless recovery gap
for MariaDB-compatible idempotent secondary-index migration statements by
finishing proven no-op dictionary recovery while another ownerless peer remains
live.

## Affected Layers

- Ownerless dictionary-recovery classification and recovery-kind state.
- Ownerless cross-process SQL hook coverage and CTest registration.
- Compatibility and ownerless concurrency evidence docs.

## DDL Metadata Routing Impact

The classifiers only mark a statement recoverable when MariaDB-visible
`information_schema.statistics` state already proves that the idempotent index
DDL will be a no-op. They do not change how MariaDB routes table metadata,
index metadata, storage-engine DDL, or warnings during normal statement
execution.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The slice uses existing native
InnoDB secondary-index metadata inside the table's tablespace, existing
ownerless dictionary generation state, and the existing ownerless/native reopen
lifecycle.

## Native Storage Impact

The promoted paths are explicitly pre-proved no-ops. Mutating index DDL stays on
the existing conservative path until broader native file-lifecycle recovery
proves those cases.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-index-idempotent-create-crash`
  - `dictionary-index-idempotent-drop-crash`
  - `dictionary-alter-index-idempotent-create-crash`
  - `dictionary-alter-index-idempotent-drop-crash`
- Run the registered hook CTest subset for those selectors.
- Run production embedded `index-idempotent-ddl`.
- Run ownerless DDL stress because dictionary recovery classification changed.
- Run ownerless primitive recovery-kind coverage, `format-check`, CI
  production-build guards, and `git diff --check`.

## Acceptance Criteria

- Duplicate create/add and missing drop recovery completes while another
  ownerless peer remains live.
- The native file-operation checkpoint marker remains clear for the live
  no-op recovery path.
- The original secondary-index key part remains present and usable.
- The attempted replacement key part and missing index remain absent.
- Plain duplicate create/add retries keep returning MariaDB errno 1061.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same rows and index metadata.

## Risks And Unresolved Questions

- The supported live-recovery parser shape is intentionally narrower than
  MariaDB's full index grammar; rejected forms remain on the existing
  conservative recovery path.
- Unique, primary-key, special-index, generated-column, online-option, and
  multi-action ALTER variants still need their own proof before live recovery
  can be claimed.
