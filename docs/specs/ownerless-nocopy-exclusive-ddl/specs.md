# Ownerless NOCOPY Exclusive DDL

## Problem

Ownerless online DDL coverage already proves peer refresh for representative
`NOCOPY`, `INPLACE`, `INSTANT`, and `COPY` option combinations. The ordinary
secondary-index matrix still had one bounded accepted MariaDB combination left
unnamed: `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE` for add/drop.

MyLite should prove this accepted combination publishes the same ownerless
dictionary boundary as the other online secondary-index variants, and that
already-open peers observe the add/drop without stale index metadata.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_alter.cc` parses `ALGORITHM=NOCOPY` and `LOCK=EXCLUSIVE`
  into `Alter_info` requested algorithm and lock values.
- `mariadb/sql/sql_table.cc` routes accepted non-copy ALTER operations through
  `mysql_inplace_alter_table()`, asks the handler for support, and validates
  the requested algorithm/lock pair.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` handles ordinary
  secondary-index add/drop as in-place/no-copy operations for supported table
  shapes.
- The existing `online-ddl-options` selector already proves peer refresh,
  forced-index usability, final ownerless/native reopen, and forced `.shm`
  rebuild for nearby `NOCOPY` and `INPLACE` variants.

## Scope And Non-Goals

In scope:

- Add `ALTER TABLE ... ADD INDEX ..., ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`.
- Add `ALTER TABLE ... DROP INDEX ..., ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`.
- Verify already-open ownerless peers see the added index through
  `INFORMATION_SCHEMA.STATISTICS` and can use it through `FORCE INDEX`.
- Verify peers and reopen paths see the dropped index as absent.

Out of scope:

- Exhaustive online DDL option matrices.
- Full-text, spatial, partitioned, generated-column, primary-key, foreign-key,
  and tablespace detach/import variants.
- SQL-level table-lock fault injection.
- External randomized MariaDB/RQG DDL oracle execution.

## Design

Extend `test_ownerless_online_ddl_options_refresh_peer_dictionary()` rather
than creating a new harness. The child DDL process adds a composite secondary
index with `ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`, then drops it with the same
algorithm/lock pair. The parent ownerless peer synchronizes after each DDL
boundary and verifies:

- metadata row count for the added composite index,
- forced-index query usability while present,
- metadata absence after drop,
- forced-index rejection after drop.

The existing final-state helper is extended so ownerless reopen, ordinary
native exclusive reopen, forced `.shm` rebuild, and native reopen after rebuild
all verify that the temporary index remains absent.

## Compatibility Impact

No SQL semantics change. The slice narrows the ownerless online DDL option
matrix for accepted ordinary InnoDB secondary-index ALTER forms. Broader
randomized DDL oracles and unsupported DDL classes remain planned or rejected.

## Directory And Lifecycle Impact

No directory layout change. Native InnoDB index metadata remains inside the
MyLite database directory, and the final state is verified through the standard
ownerless/native reopen lifecycle.

## Native Storage Impact

No native storage format change. The slice relies on MariaDB/InnoDB native
no-copy secondary-index DDL and MyLite ownerless dictionary-generation refresh.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, dependency, or license change.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `online-ddl-options` in `embedded-dev`.
- Build and run focused `online-ddl-options` in `ownerless-test-hooks`.
- Run adjacent embedded/hook ownerless SQL CTest coverage as appropriate.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open ownerless peer observes the added `NOCOPY, LOCK=EXCLUSIVE`
  secondary index and can use it with `FORCE INDEX`.
- The already-open ownerless peer observes the dropped index and `FORCE INDEX`
  fails afterward.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild
  keep the index absent.
- Compatibility docs name the new coverage while keeping broader online DDL
  and external randomized DDL oracles partial/planned.

## Risks And Follow-Up

- MariaDB can reject this option pair for more complex table shapes; this slice
  intentionally covers an ordinary non-partitioned InnoDB secondary index.
- SQL-level table-lock wait fault injection remains planned because explored
  SQL shapes still do not reach the ownerless table-wait callback.
