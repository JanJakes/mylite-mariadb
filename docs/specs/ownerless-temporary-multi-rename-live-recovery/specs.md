# Ownerless Temporary Multi-Rename Live Recovery

## Problem Statement

Ownerless temporary-table DDL recovery now handles single-pair temporary
`RENAME TABLE` and simple `ALTER TABLE ... RENAME` as metadata-only dictionary
recovery. MariaDB also allows multi-pair `RENAME TABLE` lists where temporary
table identity is carried forward from earlier pairs in the same statement.

The current ownerless recovery classifier still rejects comma-separated
temporary rename lists in the temporary metadata-only lane. Pure temporary
rename chains can therefore fall through to the native durable table-rename
recovery kind even though no shared durable table files were moved. Mixed
temporary plus permanent rename lists also lack focused crash coverage proving
that the permanent pair keeps the durable native file-operation lane while the
temporary pair disappears with the killed session.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc:6346-6418` documents and implements
  temporary-table identity propagation for `RENAME TABLE` lists. In
  `A->B, B->C`, the second `B` is temporary if `A` was temporary; in
  `A->B, A->C`, the second `A` is no longer temporary after the first pair.
- `mariadb/sql/sql_parse.cc:6405-6419` annotates each old/new rename pair with
  the resolved temporary table before grant and rename checks continue.
- `mariadb/sql/sql_table.cc:10085-10133` keeps simple temporary-table
  `ALTER TABLE ... RENAME` on a temporary-table path that does not touch
  durable `.FRM` metadata.
- `packages/libmylite/src/database.cc` already updates tracked temporary table
  names across successful multi-pair `RENAME TABLE` statements.
- `docs/specs/ownerless-temporary-ddl-live-recovery/specs.md` intentionally
  left multi-pair temporary rename chains and mixed permanent/temporary rename
  lists as follow-up work.

## Scope And Non-Goals

In scope:

- Classify pure multi-pair temporary `RENAME TABLE` chains as the existing
  metadata-only temporary-table recovery kind.
- Preserve the conservative durable native file-operation recovery lane for
  mixed temporary/permanent rename lists.
- Add hook crash coverage for a pure temporary rename chain while another
  ownerless peer remains live.
- Add hook crash coverage for one mixed temporary/permanent rename list while
  another ownerless peer remains live.
- Verify shadowed permanent tables, durable renamed permanent tables, native
  file-operation marker state, ownerless/native reopen, and forced `.shm`
  rebuild.

Out of scope:

- New SQL syntax support.
- Changing `ALTER TABLE ... RENAME` beyond the already-covered single temporary
  source shape.
- Crash injection inside MariaDB's internal pair loop.
- Broader rename variants, partition DDL, and unrelated DDL file-lifecycle
  classes.

## Design

Extend the temporary `RENAME TABLE` recovery classifier to walk the whole rename
list. It maintains a local copy of the current handle's tracked temporary table
names and applies the same rename-pair state transition used after successful
SQL execution:

- the source of every pair must currently be tracked as temporary;
- the source name is removed and the target name is added for later pairs;
- semicolon-only tails are accepted;
- any untracked source makes the temporary classifier return false.

That false result is intentional for mixed lists. A statement such as
`RENAME TABLE temp_shadow TO temp_shadow_gone, durable_src TO durable_dst`
then reaches the existing durable `RENAME_TABLE` recovery kind, which keeps the
native file-operation checkpoint marker until no-live checkpoint drain. A pure
temporary chain instead uses `TEMPORARY_TABLE`, stays metadata-only, and skips
the native file-operation marker.

## Compatibility Impact

No normal SQL behavior changes. The slice only changes ownerless crash recovery
classification after MariaDB has already accepted and executed the statement.
Temporary tables remain connection-local and disappear with the killed session.

## Directory, Lifecycle, And Native Storage Impact

Pure temporary rename chains do not create, move, or remove durable database
files. Mixed lists continue to rely on the native file-operation marker for the
permanent table rename in the same statement. No directory layout or native file
format changes are introduced.

## Public API, Build, Size, License

No public API, dependency, license, or meaningful binary-size impact. The slice
adds bounded classifier logic, hook tests, CTest registration, and docs.

## Test And Verification Plan

- Add direct hook selectors:
  - `temporary-multi-rename-crash`
  - `temporary-mixed-rename-crash`
- Register both selectors in the `ownerless-test-hooks` CTest preset.
- Run the selectors directly and through CTest.
- Run adjacent temporary rename tracking and single temporary rename crash
  selectors.
- Run focused ownerless primitives, temporary stress, production-build guards,
  format-check, and `git diff --check`.

## Acceptance Criteria

- A killed pure temporary multi-pair `RENAME TABLE` writer can be recovered
  while another ownerless peer remains live, with the native file-operation
  marker clear.
- The shadowed permanent source remains durable and the temporary target names
  do not appear as permanent tables after live recovery, ownerless/native
  reopen, and forced `.shm` rebuild.
- A killed mixed temporary/permanent multi-pair `RENAME TABLE` writer can be
  recovered while another ownerless peer remains live, with the native
  file-operation marker retained until the final peer exits and drained after
  no-live checkpoint recovery.
- The permanent rename in the mixed list is durable through ownerless/native
  reopen and forced `.shm` rebuild, while the temporary rename target is absent
  as a permanent table.

## Risks And Follow-Up

- The classifier remains token-level and bounded to simple table identifiers.
- The mixed-list test proves one representative pair ordering. Crash injection
  inside MariaDB's rename pair loop and broader rename variants remain planned.
