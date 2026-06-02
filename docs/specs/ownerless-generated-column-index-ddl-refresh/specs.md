# Ownerless Generated Column Index DDL Refresh

## Problem

Ownerless generated-column coverage proves peer refresh for create-time
generated columns and for generated columns added or dropped by `ALTER TABLE`.
Index coverage proves standalone secondary-index create/drop refresh over base
columns, prefix columns, unique columns, and key-part direction metadata.

MyLite still needs bounded evidence that a standalone secondary index created
by one ownerless process over generated columns refreshes an already-open peer,
works for forced indexed reads while present, keeps generated values correct
after peer DML changes base columns, disappears after peer `DROP INDEX`, and
survives ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` checks generated-column key parts in ordinary
  `Key::UNIQUE`, `Key::MULTIPLE`, and `Key::FOREIGN_KEY` preparation paths
  with `Column_definition::check_vcol_for_key()` at lines 3799-3805.
- `mariadb/sql/field.cc:Column_definition::check_vcol_for_key()` rejects only
  non-strictly-deterministic generated expressions for key use, so deterministic
  generated expressions remain eligible for ordinary secondary indexes.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `innobase_indexed_virtual_exist()` explicitly detects indexed virtual
  columns by scanning `KEY_PART_INFO` fields whose `stored_in_db()` is false.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `innobase_create_index_field_def()` records generated-column index fields as
  virtual or stored by setting `index_field->is_v_col` and mapping virtual
  column numbers separately from stored column numbers.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects spatial indexes
  on virtual columns, while ordinary non-spatial generated-column secondary
  indexes follow the normal InnoDB DDL path.
- `packages/libmylite/src/database.cc` treats ownerless `CREATE INDEX`,
  `DROP INDEX`, and `ALTER TABLE` as dictionary DDL: it publishes an in-progress
  dictionary generation and a stable generation so peers refresh table and
  InnoDB dictionary state before using changed metadata.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector for standalone `CREATE INDEX` and
  `DROP INDEX` over one stored generated column and one virtual generated
  column in an InnoDB table.
- Verify an already-open ownerless peer observes both generated-column indexes
  through `INFORMATION_SCHEMA.STATISTICS`.
- Verify forced-index reads work while each index exists.
- Verify peer DML that changes base columns recalculates stored and virtual
  generated values and keeps the generated-column indexes usable.
- Verify peer `DROP INDEX` removes both indexes from the already-open peer,
  makes `FORCE INDEX` fail, and leaves base-table DML/readability intact.
- Verify final rows, generated-value aggregates, and absent-index metadata
  through ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Generated-column expression replacement, nondeterministic expression
  rejection, generated-column primary-key variants, prefix/generated-column
  combinations, online algorithm/lock option matrices, special indexes, crash
  recovery during generated-column index DDL, and external MariaDB/RQG oracle
  stress.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `generated-column-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_generated_index_base` with `stored_sum` as a stored
   generated column over `first_value + second_value` and `virtual_product` as
   a virtual generated column over `first_value * second_value`.
2. The parent opens ownerless before the child creates the table, then verifies
   initial generated-expression aggregates and absence of the generated-column
   indexes.
3. The child creates standalone secondary indexes over `stored_sum` and
   `virtual_product`.
4. The parent verifies `INFORMATION_SCHEMA.STATISTICS` rows for both generated
   column indexes, uses `FORCE INDEX` on each generated expression, updates and
   inserts base-column rows, and verifies indexed reads return recalculated
   generated values.
5. The child drops both indexes.
6. The parent verifies index absence, forced-index failure, continued base DML,
   and final generated-value aggregates.
7. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should not require product-code changes if existing ownerless
dictionary DDL publication and MariaDB/InnoDB generated-column index paths are
correct.

## Compatibility Impact

This extends ownerless index DDL coverage to representative deterministic
stored and virtual generated-column secondary indexes. It does not claim the
full generated-column expression, generated-column key-type, online-option,
crash-recovery, or external-oracle matrix.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native table
definition and secondary-index storage inside the MyLite database directory.
The final state is checked after ordinary ownerless/native reopen and forced
volatile shared-memory rebuild.

## Native Storage Impact

Native storage format is unchanged. The selector exercises MariaDB's existing
InnoDB generated-column secondary-index metadata for stored and virtual
generated columns.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-index-ddl` selector.
- Run adjacent generated-column and index selectors:
  `generated-column-alter`, `generated-column-foreign-key`,
  `generated-column-foreign-key-policy`, `index-ddl`, and `unique-index-ddl`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort or non-reproducing broad-stress timeout appears.

## Acceptance Criteria

- An already-open ownerless peer observes stored and virtual generated-column
  secondary indexes created by another ownerless process.
- Forced-index reads over the generated-column secondary indexes work while the
  indexes exist.
- Peer DML that modifies base columns recalculates generated values and keeps
  generated-column index reads correct.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, base DML still works, and table data remains readable.
- Final generated-value aggregates and absent-index metadata survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Generated-column expression replacement, nondeterministic-expression policy,
  generated-column key-type matrices, online-option matrices, crash recovery,
  and external MariaDB/RQG stress remain separate validation work.
