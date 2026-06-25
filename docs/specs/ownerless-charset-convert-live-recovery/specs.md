# Ownerless Charset Convert Live Recovery

## Problem Statement

Ownerless charset-conversion crash coverage already killed a writer after
MariaDB completed `ALTER TABLE ... CONVERT TO CHARACTER SET` but before MyLite
published dictionary finish. That coverage proved no-live recovery after the
remaining peer closed. It did not prove the higher-risk ownerless case where
another process remains live and a new opener must recover the completed native
ALTER boundary without deleting the live peer's evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...` through the
  `CONVERT_SYM TO_SYM charset charset_name_or_default` alter-table grammar.
- `mariadb/sql/sql_table.cc::HA_CREATE_INFO::resolve_to_charset_collation_context()`
  resolves `alter_table_convert_to_charset`.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` is the native ALTER TABLE
  execution path and may use copy-style temporary table lifecycle for table
  changes.
- `packages/libmylite/src/database.cc` exposes
  `dictionary-before-finish` after native SQL execution and before ownerless
  dictionary finish. Recoverable dictionary markers are intentionally limited to
  classified statements with native file-operation checkpoint evidence.

## Scope And Non-Goals

In scope:

- Classify focused
  `ALTER TABLE <table> CONVERT TO CHARACTER SET <charset> [COLLATE <collation>]`
  statements as recoverable ownerless dictionary DDL.
- Add live-peer recovery evidence for the existing
  `ALTER TABLE app.ownerless_charset_convert_base CONVERT TO CHARACTER SET
  utf8mb4 COLLATE utf8mb4_general_ci` hook selector.
- Prove converted metadata and retained rows are visible while an ownerless peer
  remains live.
- Prove native file-operation checkpoint evidence remains durable until the
  final live peer exits and then drains on no-live close.

Out of scope:

- Exhaustive charset and collation matrices.
- Index prefix-width edge cases caused by wider character sets.
- Partition, external-directory, import/discard tablespace, and encrypted-table
  ALTER variants.
- Same-statement multi-DDL or randomized external oracle stress.

## Design

Add a new bounded ownerless dictionary recovery kind for charset conversion.
The classifier accepts:

- `ALTER TABLE`,
- a one- or two-part table identifier,
- `CONVERT TO CHARACTER SET`,
- a charset identifier,
- optional `COLLATE` plus a collation identifier, and
- only trailing semicolons.

This intentionally does not classify `CONVERT PARTITION`, expression
`CONVERT()`, general charset table options, or broader ALTER TABLE changes.

Upgrade the existing `dictionary-charset-convert-crash` selector to hold the
live peer after killing the writer. A new ownerless opener must recover the dead
owner while the peer is still live, observe `utf8mb4_general_ci` metadata, read
the two original rows, and leave the native file-operation checkpoint marker
set. After the peer exits, the final opener verifies the same metadata, inserts
a third row, closes, and proves the marker drained. Existing ownerless/native
reopen and forced `.shm` rebuild checks remain.

## Compatibility Impact

No SQL feature is newly enabled. This strengthens ownerless ALTER TABLE crash
compatibility for a common MariaDB metadata/storage rewrite by proving live-peer
recovery instead of only no-live recovery.

## Directory And Lifecycle Impact

No directory layout changes. The slice uses existing MyLite-owned concurrency
state under the database directory, existing native file-operation checkpoint
markers, and existing shared-memory rebuild behavior.

## Native Storage Impact

MariaDB remains responsible for the charset conversion and any native InnoDB
rewrite. MyLite only classifies the completed native boundary so dead-owner
dictionary recovery can publish a stable generation while a peer remains live.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive build-profile
changes. The implementation adds one recovery-kind value, one bounded parser,
and focused test/docs coverage.

## Test And Verification Plan

- Build hook target `mylite_ownerless_cross_process_sql_test`.
- Run focused hook selector `dictionary-charset-convert-crash`.
- Run adjacent rebuild selectors:
  `dictionary-force-rebuild-file-op-marker-crash` and
  `dictionary-row-format-file-op-marker-crash`.
- Run the registered hook CTest entry
  `libmylite.ownerless-dictionary-charset-convert-crash`.
- Build production ownerless SQL target and run representative non-hook
  `charset-convert-ddl`, `row-format-ddl`, and `ddl-broader` selectors.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- The charset-convert writer reaches `dictionary-before-finish` and is killed.
- A live peer remains open while a new ownerless opener completes dead-owner
  dictionary recovery.
- The live recovery opener observes converted `utf8mb4_general_ci` metadata,
  retained rows, and intact native `.frm`/`.ibd` files.
- The native file-operation checkpoint marker remains set while the peer is
  live and drains after final no-live close.
- Ownerless and ordinary native reopen before and after forced `.shm` rebuild
  preserve the converted metadata and final rows.

## Verification Results

- Hook build: `mylite_ownerless_cross_process_sql_test`.
- Hook selector: `dictionary-charset-convert-crash`.
- Adjacent hook selectors: `dictionary-force-rebuild-file-op-marker-crash`,
  `dictionary-row-format-file-op-marker-crash`, and
  `dictionary-row-format-crash`.
- Hook CTest: `libmylite.ownerless-dictionary-charset-convert-crash`.
- Production embedded build: `mylite_ownerless_cross_process_sql_test`.
- Production selectors: `charset-convert-ddl`, `row-format-ddl`, and
  `ddl-broader`.

## Risks And Follow-Up

- The parser is intentionally conservative and only covers table-wide
  `CONVERT TO CHARACTER SET` forms, not every ALTER TABLE syntax that can
  rewrite table metadata.
- Broader ALTER rebuild, partition/FK truncate, schema, view, trigger, and
  non-rename foreign-key multi-DDL live-peer recovery remain planned.
- External MariaDB/RQG stress remains needed after bounded recovery gates stop
  producing new correctness issues.
