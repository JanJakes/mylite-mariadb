# XA Transaction Size Profile

## Problem Statement

The aggressive embedded minsize profile still compiles MariaDB's full XA
transaction implementation. MyLite's transaction design explicitly defers XA
and two-phase commit, and the embedded product has no external transaction
manager, recovery service, replication applier, or cross-resource commit
coordinator. The remaining XA code is therefore a server/distributed-
transaction surface in the current embedded profile.

Current baseline after `event-parse-data-size-profile`:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,052,668 |
| `xa.cc.o` object | 130,584 |
| stripped `mylite-open-close-smoke` | 5,726,400 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `docs/specs/deferred-transaction-publication/specs.md` names XA and
  two-phase commit as explicit non-goals for MyLite's first transactional
  storage work.
- `vendor/mariadb/server/sql/sql_yacc.yy` parses `XA START`, `XA END`,
  `XA PREPARE`, `XA COMMIT`, `XA ROLLBACK`, and `XA RECOVER` into
  `SQLCOM_XA_*` commands.
- `vendor/mariadb/server/sql/sql_parse.cc` dispatches `SQLCOM_XA_*` directly
  to `trans_xa_start()`, `trans_xa_end()`, `trans_xa_prepare()`,
  `trans_xa_commit()`, `trans_xa_rollback()`, and `mysql_xa_recover()`.
- `vendor/mariadb/server/sql/xa.cc` owns the lock-free XID cache, explicit XA
  state transitions, external-XID recovery, and `XA RECOVER` result-set
  metadata helpers.
- `vendor/mariadb/server/sql/mysqld.cc` calls `xid_cache_init()` and
  `xid_cache_free()` during server lifecycle, so embedded startup still needs
  inert lifecycle symbols even when XA is disabled.
- `vendor/mariadb/server/sql/sql_prepare.cc` calls `xa_recover_get_fields()`
  during prepared-statement metadata analysis for `XA RECOVER`.
- `vendor/mariadb/server/sql/handler.cc` may call `xid_cache_insert(XID*)`
  while recovering foreign XIDs from storage engines. In the current MyLite
  minsize profile, durable XA recovery is unsupported and replication/applier
  paths are already cut, so a no-op cache insert is acceptable.

## Scope

Add a minsize option that removes the full XA implementation from the embedded
library. The option will:

- remove `../sql/xa.cc` from `SQL_EMBEDDED_SOURCES`;
- add a MyLite-owned XA stub that rejects user-facing XA commands in embedded
  mode;
- keep `xid_cache_init()` and `xid_cache_free()` as no-ops for startup and
  shutdown;
- keep inert `XID_STATE` helpers so non-XA transaction cleanup remains safe;
- reject prepared `XA RECOVER` instead of constructing metadata; and
- keep local transaction commit/rollback behavior unchanged.

## Non-Goals

- Do not implement XA or two-phase commit.
- Do not implement external transaction-manager recovery.
- Do not change ordinary `START TRANSACTION`, `COMMIT`, or `ROLLBACK`
  behavior.
- Do not remove `binlog_tp`, `MYSQL_BIN_LOG`, or remaining binlog transaction
  shell code in this slice.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_XA_TRANSACTIONS` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it in
`tools/build-mariadb-minsize.sh`.

Create `vendor/mariadb/server/libmysqld/mylite_xa_stub.cc`. The stub will:

- define `xid_cache_init()` and `xid_cache_free()` as no-ops;
- make recovered-XID cache insertion a no-op because recovered XA branches are
  unsupported;
- make explicit-XA cache insertion fail defensively;
- define `XID_STATE` helpers as inert when no explicit XA state exists;
- reject `trans_xa_start()`, `trans_xa_end()`, `trans_xa_prepare()`,
  `trans_xa_commit()`, `trans_xa_rollback()`, and `mysql_xa_recover()` with
  MariaDB's embedded-disabled diagnostic; and
- reject prepared-statement metadata setup for `XA RECOVER` in
  `sql_prepare.cc`, leaving `xa_recover_get_fields()` as an inert link stub.

`trans_xa_detach()` should be defensive and clear any impossible explicit XA
pointer without reporting a user error during THD cleanup.

## Affected Subsystems

- Embedded minsize SQL source list.
- SQL XA command execution.
- Prepared-statement metadata for `XA RECOVER`.
- XID cache startup/shutdown lifecycle.
- Binary-size documentation and open/close smoke coverage.

## DDL Metadata Routing Impact

No MyLite table DDL changes. XA commands are transaction-control statements,
not table-definition metadata.

## Single-File And Embedded-Lifecycle Impact

This removes an external transaction-manager and recovered-XID cache surface
from the embedded runtime. It does not create any MyLite companion files and
does not change `.mylite` file ownership.

## Public API Or File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size Impact

Expected archive savings are bounded by the 130,584-byte `xa.cc.o` member
minus the replacement stub. Linked-runtime savings should be more meaningful
than the event parse-data cut because the current open/close smoke retains
explicit XA command handlers, XID cache helpers, and `XID_STATE` methods.

Measured result on top of `event-parse-data-size-profile`:

| Artifact | Bytes | Delta |
| --- | ---: | ---: |
| `libmysqld/libmariadbd.a` | 29,920,080 | -132,588 |
| `mylite_xa_stub.cc.o` object | 8,272 | replaces 130,584-byte `xa.cc.o` |
| `mylite/mylite-open-close-smoke` | 7,951,584 | -8,512 |
| stripped `mylite-open-close-smoke` | 5,719,056 | -7,344 |

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- archive bytes and object count;
- unstripped and stripped linked smoke bytes;
- absence of `xa.cc.o` in `libmariadbd.a`;
- presence and size of the replacement stub;
- linked XA symbols retained by the smoke; and
- `XA START` and `XA RECOVER` unsupported diagnostics from open/close smoke.

## Verification Results

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-xa \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

The archive contains `mylite_xa_stub.cc.o` and no longer contains `xa.cc.o`.
The linked smoke retains only the disabled XA entry points and inert
`XID_STATE` helpers. Open/close smoke records embedded-disabled diagnostics for
`XA START` and `XA RECOVER`, and the compatibility harness reports `status=0`
for all groups.

## Acceptance Criteria

- The minsize build completes.
- Embedded bootstrap, open/close smoke, and compatibility harness pass.
- Open/close smoke verifies XA commands are explicitly unsupported.
- The embedded archive no longer contains `xa.cc.o`.
- Ordinary local transaction behavior remains covered by the compatibility
  harness.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Risks And Unresolved Questions

- This removes a real MariaDB SQL feature, not just daemon plumbing. It belongs
  only in the aggressive embedded-size profile.
- Future MyLite support for XA, external recovery, or two-phase commit must
  disable this option and design a file-owned recovery story.
- A later binlog-transaction-shell slice may expose more XA-related roots after
  `xa.cc` is removed; that should remain separate from this bounded cut.
