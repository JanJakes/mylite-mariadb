# CREATE TABLE SELECT Duplicate Modes

## Goal

Cover representative `CREATE TABLE ... IGNORE SELECT` and
`CREATE TABLE ... REPLACE SELECT` behavior for MyLite-routed CTAS targets with
supported primary, unique, and secondary indexes.

## Non-Goals

- Exhaustive duplicate-mode matrices across all key orders, generated columns,
  triggers, partitions, foreign keys, or unsupported index classes.
- Physical cleanup of orphaned pages written before a failed CTAS abort beyond
  the existing statement-checkpoint and drop paths.
- SQL transaction, savepoint, or multi-statement rollback semantics.
- Wire-protocol or public `libmylite` API changes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:4821-4829` parses CTAS through
  `opt_create_select`, which includes an optional duplicate mode before the
  `SELECT`.
- `mariadb/sql/sql_yacc.yy:15326-15330` maps omitted duplicate mode to
  `DUP_ERROR`, `REPLACE` to `DUP_REPLACE`, and `IGNORE` to
  `Lex->ignore = 1`.
- `mariadb/sql/sql_table.cc:13570-13585` marks
  `CREATE TABLE ... IGNORE/REPLACE SELECT` as binlog-unsafe because selected
  row order determines which duplicate rows are ignored or replaced.
- `mariadb/sql/sql_insert.cc:706-720` prepares duplicate handling by enabling
  ignore-duplicate behavior and duplicate lookup handlers when `IGNORE` or
  non-error duplicate modes are active.
- `mariadb/sql/sql_insert.cc:2034-2100` locates a duplicate row either by
  `rnd_pos()` when the handler supports duplicate positions or by
  `ha_index_read_idx_map()` for the duplicated key.
- `mariadb/sql/sql_insert.cc:2134-2210` implements `REPLACE` as either an
  update of the duplicate row or delete plus insert.
- `mariadb/sql/sql_insert.cc:2402-2424` sends `DUP_ERROR`, `DUP_UPDATE`, and
  `DUP_REPLACE` through the normal `Write_record` insert path.
- `mariadb/sql/sql_insert.cc:5125-5229` builds CTAS targets through
  `select_create::prepare()`, calls `prepare_for_replace()`, and constructs
  `Write_record` for row writes.
- MariaDB tests use syntax such as
  `CREATE TABLE t (a INT, PRIMARY KEY(a)) IGNORE SELECT ...` and
  `CREATE TABLE t (a INT UNIQUE) REPLACE SELECT ...`.
- `mariadb/storage/mylite/ha_mylite.cc:1015-1063` implements
  `index_read_map()` and `index_read_idx_map()` over MyLite index entries.
- `mariadb/storage/mylite/ha_mylite.cc:1234-1238` stores the current row id
  for handler position reads, and `mariadb/storage/mylite/ha_mylite.cc:1241-1279`
  reports the duplicated key through `info(HA_STATUS_ERRKEY)`.
- `mariadb/storage/mylite/ha_mylite.cc:1359-1428` detects duplicate keys in
  `write_row()` and records the duplicate key number for MariaDB.
- `mariadb/storage/mylite/ha_mylite.cc:1431-1522` supports update and delete
  of the row most recently positioned by full-scan, index, or `rnd_pos()`
  reads.

## Compatibility Impact

`CREATE TABLE ... SELECT` remains partial, but the planned `IGNORE` and
`REPLACE` duplicate-mode gap moves to covered representative storage-smoke
behavior for supported MyLite-routed tables. The claim is intentionally scoped
to deterministic `ORDER BY` SELECT inputs and supported target index shapes.

## Design

No new CTAS executor is added. The slice relies on MariaDB's existing
`select_create` and `Write_record` paths:

1. Let MariaDB parse the duplicate mode and prepare CTAS duplicate handling.
2. Let MyLite `write_row()` continue to detect duplicates and report the
   duplicate key.
3. Let MariaDB use MyLite indexed lookup to find the duplicate row for
   `REPLACE`, then call the existing `update_row()` or `delete_row()` handler
   path.
4. Verify deterministic row-order semantics with `ORDER BY` in the SELECT.

If the existing handler path fails, fixes belong in the MyLite handler's
duplicate-key lookup, index positioning, or row lifecycle implementation, not
in a parallel SQL-layer workaround.

## File Lifecycle

Successful duplicate-mode CTAS writes normal MyLite catalog metadata, rows, and
index entries into the primary `.mylite` file. It must not create durable
`.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, Aria log, binlog, relay-log, or
plugin-owned table files. Existing rollback-journal behavior remains the only
allowed companion-file path for this slice.

## Embedded Lifecycle And API

No public API changes. The smoke test must cover close/reopen visibility and
must verify the reopened catalog does not depend on runtime schema directories.

## Build, Size, And Dependencies

No dependency is added. Binary-size impact is limited to test and documentation
unless a handler fix is required.

## Test Plan

- Add storage-engine smoke coverage for deterministic:
  - `CREATE TABLE ... IGNORE SELECT` with duplicate unique-key input;
  - `CREATE TABLE ... REPLACE SELECT` with duplicate unique-key input;
  - primary, unique, and secondary index visibility after duplicate handling;
  - requested/effective engine metadata for the CTAS targets;
  - close/reopen visibility and sidecar gates.
- Run focused storage-smoke build and test, compatibility harness reports,
  formatting checks, shell checks, tidy, and the dev/embedded/storage-smoke test
  presets before committing.

## Acceptance Criteria

- `IGNORE` CTAS keeps the first selected row for a duplicated key and skips the
  later duplicate row.
- `REPLACE` CTAS keeps the last selected row for a duplicated key and removes
  the replaced row from primary, unique, and secondary access paths.
- Catalog metadata records `InnoDB` as requested and `MYLITE` as effective for
  both representative CTAS targets.
- Rows and indexes survive close/reopen without durable forbidden sidecars or
  runtime schema directories.
- Compatibility, roadmap, storage architecture, CTAS spec, and harness docs no
  longer list `IGNORE` / `REPLACE` CTAS as an uncovered planned gap.

## Risks And Open Questions

- MariaDB labels duplicate-mode CTAS unsafe without deterministic row ordering;
  this slice uses `ORDER BY` and does not claim unordered-result determinism.
- The representative `REPLACE` path exercises one duplicated unique key. Broader
  multi-unique-key conflict ordering remains planned.
- Orphaned page cleanup before broader SQL rollback and compaction remains a
  storage roadmap item.
