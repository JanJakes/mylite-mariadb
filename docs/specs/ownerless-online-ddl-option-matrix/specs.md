# Ownerless Online DDL Option Matrix

## Problem

Ownerless DDL coverage already proves representative peer refresh for ordinary
online index creation, copy rebuilds, instant column changes, and several
standalone index classes. The remaining Phase 10 DDL notes still leave broader
online DDL option combinations as planned. A bounded next step is to extend the
existing online DDL option selector with explicit no-lock index drop/re-add and
index visibility toggles while another ownerless peer remains open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `ha_innobase::check_if_supported_inplace_alter()` classifies ordinary
  non-unique index add/drop and index ignorability under InnoDB in-place and
  no-copy paths, returning no-lock variants when the table shape permits online
  execution.
- `mariadb/sql/sql_table.cc` upgrades metadata locks and sets
  `Alter_inplace_info::online` for `LOCK=NONE` in-place/instant alter paths
  before `ha_prepare_inplace_alter_table()` runs.
- MyLite's ownerless DDL boundary is statement-level: the DDL process marks the
  shared dictionary generation active around MariaDB execution, publishes a
  stable generation after success, and already-open peers refresh their local
  dictionary and table cache before later statements.

## Scope And Non-Goals

In scope:

- Extend `test_ownerless_online_ddl_options_refresh_peer_dictionary()` with
  explicit no-lock online secondary-index drop, re-add, ignored, and
  not-ignored transitions.
- Verify an already-open ownerless peer sees each transition through
  `INFORMATION_SCHEMA.STATISTICS` and `FORCE INDEX` behavior.
- Keep final ownerless/native reopen checks before and after forced `.shm`
  rebuild.

Out of scope:

- Exhaust every MariaDB online DDL option, partitioned-table online DDL, full
  text or spatial indexes, and external randomized DDL oracles.
- Change MyLite's public API or directory layout.

## Design

The existing child process that mutates `app.ownerless_ddl_options` gains four
additional ownerless DDL steps:

1. Drop the value secondary index with `ALGORITHM=NOCOPY, LOCK=NONE`.
2. Re-add a covering value index with `ALGORITHM=INPLACE, LOCK=NONE`.
3. Mark the status index ignored with explicit online options.
4. Mark the status index not ignored with explicit online options.

After every step, the parent keeps using its already-open ownerless handle to
prove the shared dictionary generation boundary and pre-statement refresh are
enough for peer-visible metadata and optimizer state.

## Compatibility Impact

This does not add new SQL syntax support. It adds compatibility evidence for
MariaDB-supported InnoDB online DDL option combinations in ownerless mode and
keeps unsupported special index and partition classes explicitly rejected.

## Database Directory And Lifecycle Impact

All native metadata and table files stay in the existing MyLite-owned database
directory. The test verifies final state through ownerless reopen, ordinary
exclusive reopen, forced `.shm` rebuild, and ordinary exclusive reopen after
that rebuild.

## Test Plan

- Focused embedded selector:
  `./build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test online-ddl-options`.
- Focused hook selector:
  `./build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test online-ddl-options`.
- Adjacent DDL selectors when needed: `rename-index-ddl`, `ignored-index-ddl`,
  and `ddl-broader`.
- `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes the dropped index as absent and the re-added
  online index as usable.
- The already-open peer observes ignored and not-ignored metadata transitions
  for the status index.
- Final rows and metadata survive ownerless/native reopen before and after
  forced `.shm` rebuild.
- Existing ownerless DDL policy rejections and broader stress remain unchanged.

## Risks And Open Questions

- This remains a representative matrix, not a complete enumeration of every
  online DDL option and table shape MariaDB supports.
- External randomized DDL oracles remain planned separately.
