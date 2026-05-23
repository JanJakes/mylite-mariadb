# MTR routed storage foreign_key_checks smoke

## Problem

MyLite first-party storage smoke covers session `foreign_key_checks=0` for
supported FK row-DML bypass and parent-table truncate bypass. The raw embedded
storage MTR runner does not yet prove that behavior when applications request
`ENGINE=InnoDB` but route to MyLite, or when they request `ENGINE=MYLITE`
directly.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sys_vars.cc::Sys_foreign_key_checks` stores the session option
  in `THD::variables.option_bits` through the reverse
  `OPTION_NO_FOREIGN_KEY_CHECKS` bit.
- `mariadb/sql/sql_priv.h` defines `OPTION_NO_FOREIGN_KEY_CHECKS` as a THD,
  user, and binlog option.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_foreign_key_checks_disabled()`
  reads that option from the handler `THD`.
- `mariadb/storage/mylite/ha_mylite.cc::write_row()`, `update_row()`, and
  `delete_row()` skip supported child and parent row checks when the session
  option disables FK checks.
- `mariadb/sql/sql_truncate.cc::mysql_truncate()` skips parent-FK truncate
  checks when `OPTION_NO_FOREIGN_KEY_CHECKS` is set, before dispatching to the
  handler truncate path.

## Design

Add `mylite.routed_storage_foreign_key_checks` to the storage MTR list. The test
uses enforced MyLite storage with a primary `.mylite` file and checks both
routed `ENGINE=InnoDB` and explicit `ENGINE=MYLITE` tables. It verifies:

- `foreign_key_checks=0` is visible as session state;
- child rows can be inserted before matching parent rows;
- parent updates and deletes that would orphan children are allowed while
  checks are disabled;
- re-enabling checks does not retroactively reject existing orphan rows;
- subsequent child inserts and parent updates/deletes are enforced again;
- referenced parent `TRUNCATE TABLE` fails with checks enabled;
- the same parent truncate succeeds with checks disabled; and
- no native durable sidecars appear.

## Scope

This is MTR compatibility coverage only. It does not change handler code,
expand FK shape support, add retrospective validation, or add transactional
truncate rollback.

## Compatibility Impact

The storage-routed MTR runner gains raw embedded evidence for dump-style FK
import bypass and parent-truncate bypass under the documented supported FK
subset. Compatibility remains partial: disabling checks does not admit
unsupported FK definitions, and existing orphan rows are not retrospectively
validated when checks are re-enabled.

## Storage And Lifecycle Impact

Rows written while checks are disabled and rows orphaned by parent truncate
remain ordinary durable rows in the primary `.mylite` file. The session option
is not persisted in the file. The existing sidecar assertion guards the
single-file lifecycle.

## Public API, File-Format, And Size Impact

No public `libmylite` API, file-format, dependency, or production binary-size
change. This adds only MTR coverage and documentation.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_foreign_key_checks`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_foreign_key_checks`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The MTR case demonstrates row-DML bypass while checks are disabled and
  re-enabled enforcement for later row DML.
- The MTR case demonstrates parent truncate rejection while checks are enabled
  and parent truncate bypass while checks are disabled.
- Existing orphan rows remain visible after checks are re-enabled.
- The full storage-routed MTR list passes.

## Risks

This intentionally proves compatibility behavior that can leave orphan child
rows. The compatibility matrix must continue to say MyLite does not perform
retrospective validation when `foreign_key_checks` returns to `1`.
