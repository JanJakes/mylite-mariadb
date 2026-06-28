# Ownerless Copy Lock Option Order Live Recovery

## Problem Statement

Ownerless DDL live recovery already accepted exact explicit copy-lock ALTER
tails written as:

```sql
, ALGORITHM=COPY, LOCK=EXCLUSIVE
```

MariaDB also accepts the same options in the reversed order:

```sql
, LOCK=EXCLUSIVE, ALGORITHM=COPY
```

Before this slice, MyLite's shared ownerless classifier rejected that reversed
order. A writer killed after MariaDB completed the native rebuild but before
ownerless dictionary finish could therefore fall back to less-specific recovery
instead of the focused marker-retaining live-peer rebuild lane.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy:7965-7968` defines `alter_list` as a comma-separated
  sequence of `alter_list_item` entries.
- `mariadb/sql/sql_yacc.yy:8164-8165` accepts `alter_algorithm_option` and
  `alter_lock_option` as ordinary ALTER list items, so comma-separated order is
  not special to the grammar.
- `mariadb/sql/sql_yacc.yy:8204-8207` also accepts adjacent
  `alter_lock_option alter_algorithm_option` and
  `alter_algorithm_option alter_lock_option` pairs for the index option form.
- `mariadb/sql/sql_yacc.yy:5693-5708` parses `ENGINE [=] ident_or_text` as a
  table option and sets `HA_CREATE_USED_ENGINE`.
- `packages/libmylite/src/database.cc` uses
  `consume_ownerless_optional_copy_exclusive_alter_tail()` for focused
  copy-lock column ALTER, same-engine rebuild, force rebuild, charset
  conversion, dynamic row-format rebuild, and compressed row-format rebuild
  classification.

## Scope And Non-Goals

In scope:

- Accept the exact reversed tail
  `, LOCK=EXCLUSIVE, ALGORITHM=COPY` wherever the shared
  `consume_ownerless_optional_copy_exclusive_alter_tail()` helper is used.
- Preserve the existing no-tail form and the existing
  `, ALGORITHM=COPY, LOCK=EXCLUSIVE` form.
- Keep the classifier conservative: no extra ALTER clauses, duplicated options,
  different lock modes, different algorithms, or arbitrary option matrices.
- Add focused hook coverage for one representative native rebuild,
  `ALTER TABLE ... ENGINE=InnoDB, LOCK=EXCLUSIVE, ALGORITHM=COPY`, killed at
  `dictionary-before-finish` while another ownerless peer remains live.

Out of scope:

- Exhaustive option-order tests for every shared-tail caller.
- `LOCK=DEFAULT`, `LOCK=SHARED`, `LOCK=NONE`, `ALGORITHM=INPLACE`,
  `ALGORITHM=NOCOPY`, or `ALGORITHM=DEFAULT`.
- General ALTER parsing, partition DDL, non-InnoDB engines, import/discard
  tablespace, OPTIMIZE/ANALYZE/CHECK/REPAIR, or server-only locked-table modes.
- External randomized MariaDB/RQG DDL stress.

## Design

Broaden `consume_ownerless_optional_copy_exclusive_alter_tail()` so the shared
helper accepts either exact token sequence:

- `, ALGORITHM = COPY , LOCK = EXCLUSIVE`
- `, LOCK = EXCLUSIVE , ALGORITHM = COPY`

The helper still requires the statement to end after the pair, apart from
optional semicolons. It does not accept partial pairs or additional clauses.

The representative hook test uses the same same-engine InnoDB rebuild recovery
kind as the existing copy-lock test. The table metadata precheck still proves
that the source table is an InnoDB base table before MyLite classifies the
statement as recoverable.

## Compatibility Impact

SQL semantics remain MariaDB-owned. MyLite only broadens ownerless recovery
classification for a MariaDB-accepted option ordering after MariaDB has
completed the native DDL and the unsafe hook kills the writer before ownerless
dictionary finish.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or durable format changes. The native `.frm` and `.ibd`
files remain MariaDB/InnoDB-owned inside the MyLite database directory. MyLite
uses the existing recoverable dictionary marker and native file-operation
checkpoint-needed marker for the focused rebuild lane.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive build-profile
change. The slice adds one bounded token-order branch, one hook selector, one
CTest, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `dictionary-engine-rebuild-lock-copy-crash` directly.
- Run adjacent rebuild CTests covering plain same-engine,
  algorithm-then-lock copy-lock, and lock-then-algorithm copy-lock.
- Build the production embedded test binary and run the production-build guard
  so this classifier change is checked against optimized build configuration.
- Run format and whitespace checks.

## Acceptance Criteria

- The reversed explicit copy-lock tail is classified only for the exact
  `COPY`/`EXCLUSIVE` pair and optional semicolons.
- A killed same-engine rebuild writer at `dictionary-before-finish` recovers
  while another ownerless peer remains live.
- Rebuilt rows, secondary-index metadata/use, ownerless reopen, forced `.shm`
  rebuild, native exclusive reopen, final native file-operation marker drain,
  and follow-up writes match the existing copy-lock rebuild oracle.

## Risks And Follow-Up

- The shared helper broadens multiple focused classifiers. The representative
  same-engine rebuild crash test proves the shared parser and native
  marker-retaining recovery lane, but exhaustive caller-by-caller reversed-order
  crash coverage remains planned.
- Broader ALTER option matrices and randomized external DDL stress remain
  separate ownerless completion work.
