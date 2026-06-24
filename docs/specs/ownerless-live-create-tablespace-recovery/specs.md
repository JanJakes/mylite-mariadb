# Ownerless Live Create Tablespace Recovery

## Problem Statement

Ownerless DDL crash coverage has focused on no-live recovery: a writer killed
after native DDL but before MyLite dictionary finish leaves the shared
dictionary generation active, live peers stay blocked, and a later no-live
startup rebuilds volatile ownerless state from native files.

Full ownerless concurrency still needs live-peer DDL/file-lifecycle recovery
for high-value native file classes. This slice starts with the smallest safe
class: a plain non-temporary InnoDB `CREATE TABLE` killed after native file
creation has reached MyLite's pre-finish boundary. A surviving live peer should
be able to finish the ownerless dictionary generation, observe the created
table, and continue work without waiting for all peers to close. Native
file-operation checkpoint draining remains deferred to the existing
checkpoint/no-live path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc`
  - `mysql_create_table()` starts around line `5231`, opens and locks the
    destination table name, chooses ordinary create mode, and delegates to
    `mysql_create_table_no_lock()`.
  - `mysql_create_table_no_lock()` starts around line `5013`, builds the
    table path, calls `create_table_impl()`, and owns ordinary destination
    metadata creation for SQL `CREATE TABLE`.
  - `create_table_impl()` starts around line `4652`, checks existence, handles
    `CREATE OR REPLACE` separately, writes the table definition through
    MariaDB's create path, and calls the engine create path for non-temporary
    tables.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `create_table_info_t::create_table_def()` starts around line `10984`,
    builds the InnoDB dictionary table object, and calls
    `row_create_table_for_mysql()` for durable non-temporary tables.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_create_table_for_mysql()` starts around line `2144`, executes the
    InnoDB create graph under a dictionary transaction and rolls back on
    engine error.
- `mariadb/storage/innobase/fil/fil0fil.cc`
  - `mtr_t::log_file_op()` starts around line `1607` and is the native redo
    hook MyLite uses as file-operation evidence.
  - `fil_ibd_create()` starts around line `2032`; it logs `FILE_CREATE` around
    line `2097`, writes the redo up to the mini-transaction commit LSN, and
    then creates the file-per-table `.ibd`.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` records an odd ownerless dictionary
    generation before MariaDB executes the DDL and clears stale process-local
    file-op redo evidence.
  - `ownerless_finish_dictionary_ddl()` marks the durable native file-op
    checkpoint-needed bit before the `dictionary-before-finish` hook and then
    advances the dictionary generation to even.
  - Dead-owner cleanup currently treats any active dictionary owner as
    recovery-sensitive, so live peers return `MYLITE_BUSY` until no-live
    shared-memory rebuild.
- `packages/libmylite/src/ownerless_dictionary_state.cc`
  - The dictionary state segment is 64 bytes. Bytes through the wait word at
    offset `32` are used; the remaining bytes can carry a small recoverable
    pre-finish marker without increasing the shared-memory layout size.

## Scope And Non-Goals

In scope:

- Add a recoverable dictionary pre-finish marker for plain non-temporary
  `CREATE TABLE` statements only.
- Publish the marker only after native file-op checkpoint evidence has been
  durably recorded and before the `dictionary-before-finish` hook can kill the
  writer.
- Allow dead-owner cleanup to finish that marked dictionary generation while
  peers remain live, but only after transaction, InnoDB lock, page-write, and
  redo owner state are idle.
- Keep the global native file-op checkpoint-needed marker set after live
  dictionary recovery; existing checkpoint/no-live logic remains responsible
  for draining it.
- Add hook coverage proving live-peer recovery for killed plain
  `CREATE TABLE`.

Out of scope:

- `CREATE TABLE ... LIKE`, CTAS, `CREATE OR REPLACE`, `RENAME`, `DROP`,
  `TRUNCATE`, table rebuild, schema DDL, views, triggers, foreign-key
  multi-DDL, partition/import/export, or metadata-only DDL live recovery.
- Reconstructing missing native files from page-version WAL.
- Clearing native file-op markers with live peers.
- SQL-level local table-lock wait fault injection; current evidence keeps that
  as unclaimed research rather than a completion gate.
- External MariaDB/RQG stress for the broader DDL matrix.

## Design

Add a narrow recovery kind to the first-party ownerless dictionary state:

- `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_CREATE_TABLE` records that the active
  dictionary owner reached a recoverable pre-finish boundary for a plain
  non-temporary file-per-table `CREATE TABLE`.
- `mylite_ownerless_dictionary_state_mark_recoverable()` writes the kind,
  owner id, and owner generation while the matching owner is still active.
- `mylite_ownerless_dictionary_state_recover_dead_owner()` validates the
  active owner, matching generation, and recovery kind, then advances the
  dictionary generation through the same even-generation transition as normal
  finish.
- Normal finish clears any recoverable marker.

`ownerless_begin_dictionary_ddl()` records the statement's recovery kind in
the local database handle. For this slice the classifier accepts ordinary
`CREATE TABLE`, including `IF NOT EXISTS`, rejects `TEMPORARY`, `LIKE`,
`SELECT`/`AS SELECT`, `OR REPLACE`, `SEQUENCE`, `VIEW`, `TRIGGER`, and
unsupported non-table create classes, and relies on MyLite's existing
non-InnoDB rejection for storage-engine narrowing.

`ownerless_finish_dictionary_ddl()` keeps the existing order:

1. Consume MariaDB file-op redo evidence and durably mark
   `concurrency/mylite-concurrency.ckpt` when native file operations occurred.
2. If the local statement kind is plain create-table and the durable marker is
   present, write the per-owner recoverable pre-finish marker.
3. Run the `dictionary-before-finish` hook.
4. Finish the dictionary generation normally.

Dead-owner cleanup is extended only at the final dictionary-state gate. After
it has already proven the dead owner has no active transaction, InnoDB lock,
page-write lock, redo owner, or redo latch state, it checks the durable native
file-op checkpoint-needed marker and attempts
`mylite_ownerless_dictionary_state_recover_dead_owner()` for the matching
owner/generation and create-table recovery kind. If that succeeds, cleanup can
release ordinary MDL/read-view/page-pin owner state and the live peer can open.
If any proof is absent, cleanup keeps returning busy and no-live recovery keeps
the old behavior.

## Compatibility Impact

SQL results are unchanged for successful statements. The crash behavior
improves for one ownerless hook boundary: a plain `CREATE TABLE` killed after
native create and pre-finish marker publication no longer requires all peers
to close before the created table becomes visible to ownerless peers.

Broader ownerless DDL/file-lifecycle recovery remains partial. The
compatibility matrix must continue to mark unsupported or unproved classes as
planned until each class has equivalent source-linked recovery proof.

## DDL Metadata Routing Impact

The ownerless dictionary generation remains the metadata routing boundary.
This slice only adds an explicit dead-owner recovery transition for a marked
pre-finish create-table owner. Peers still refresh MariaDB metadata caches
after observing the even generation.

## Directory And Lifecycle Impact

No directory layout changes. The slice uses existing files:

- `datadir/app/<table>.frm`,
- `datadir/app/<table>.ibd`,
- `concurrency/mylite-concurrency.shm`,
- `concurrency/mylite-concurrency.ckpt`.

The native file-op checkpoint-needed marker remains durable after live
dictionary recovery. It is drained by existing native checkpoint/no-live
reclaim logic, not by the live cleanup path.

## Native Storage Impact

No MariaDB native format changes. The final native table definition and
file-per-table tablespace created by MariaDB remain authoritative. MyLite does
not replay page-version WAL into the created tablespace during live recovery.

## Public API, Wire Protocol, Build, Size, License

No public API, wire-protocol, dependency, license, or binary-size-sensitive
build-profile changes. The production code gains a small shared-memory marker
inside the existing dictionary-state segment. Hook coverage is compiled only
when unsafe ownerless test hooks are enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run the focused hook selector for the live create recovery case.
- Run adjacent hook selectors that must remain blocked until no-live recovery:
  rename file-op marker crash, create-like file-op marker crash, CTAS file-op
  marker crash, truncate file-op marker crash, and drop file-op marker crash.
- Build the production `php-embedded-prod` ownerless SQL target to prove hooks
  compile out.
- Run production focused table-create/replay and native file-op marker drain
  selectors.
- Run `format-check-prod`, `git diff --check`, and cached diff checks.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Hook focused selectors passed:
  `dictionary-create-table-live-recovery`,
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-create-like-file-op-marker-crash`,
  `dictionary-ctas-file-op-marker-crash`,
  `dictionary-truncate-file-op-marker-crash`, and
  `dictionary-drop-file-op-marker-crash`.
- `ctest --preset ownerless-test-hooks -R
  'libmylite\.ownerless-primitives|libmylite\.ownerless-dictionary-(create-table-live-recovery|rename-file-op-marker-crash|create-like-file-op-marker-crash|ctas-file-op-marker-crash|truncate-file-op-marker-crash|drop-file-op-marker-crash)'
  --output-on-failure` passed 7/7.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_primitives_test mylite_ownerless_cross_process_sql_test -j2`
  passed.
- Production focused selectors passed:
  `mylite_ownerless_primitives_test`, `created-tablespace-replay`,
  `native-file-op-marker-drain`, and hook-disabled
  `dictionary-create-table-live-recovery`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after plain
  `CREATE TABLE app.ownerless_ddl_crash (...) ENGINE=InnoDB` leaves the native
  file-op checkpoint-needed marker set while a peer is live.
- A new live ownerless opener recovers the dead dictionary owner instead of
  returning `MYLITE_BUSY`.
- The recovered live opener observes the created table metadata and native
  `.frm`/`.ibd` files, inserts rows, and reads the inserted aggregate.
- The native file-op marker remains set immediately after live recovery and
  clears only after the existing checkpoint/no-live recovery path runs.
- Existing non-create-table DDL crash selectors continue to require no-live
  recovery unless explicitly covered by a later slice.

## Risks And Follow-Up

- The statement classifier is intentionally conservative. If a valid create
  shape is not recognized, it keeps the previous busy-until-no-live behavior.
- The recoverable marker does not prove every DDL native side effect is safe.
  Later slices must add separate markers or recovery kinds for LIKE, CTAS,
  replacement-copy, rename, drop, truncate, rebuild, schema, view, trigger, and
  foreign-key multi-DDL classes.
- This does not replace broader native redo/checkpoint reconciliation or
  external MariaDB/RQG stress.
