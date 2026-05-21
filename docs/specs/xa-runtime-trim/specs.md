# XA Runtime Trim

## Problem Statement

The embedded profile now rejects external `XA` SQL and omits the mmap-backed
`tc.log` coordinator used for durable external-XA recovery. The full MariaDB
`xa.cc` runtime still links XID-cache management, explicit XA state
transitions, XA recovery result production, and slave-applier cleanup paths
that are not part of MyLite's current local embedded API.

This slice replaces only the explicit external-XA command runtime with a small
embedded-disabled source. It must not weaken ordinary local transactions,
InnoDB recovery, savepoints, or native engine execution.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/xa.h` declares `XID_STATE`, XID-cache lifecycle helpers,
  `trans_xa_start()`, `trans_xa_end()`, `trans_xa_prepare()`,
  `trans_xa_commit()`, `trans_xa_rollback()`, `trans_xa_detach()`,
  `mysql_xa_recover()`, and `xa_recover_get_fields()`.
- `mariadb/sql/xa.cc` implements lock-free XID-cache storage, external XA
  state transitions, prepared-XA commit/rollback by XID, and `XA RECOVER`
  result rows.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_XA_*` commands to
  `trans_xa_*()` and `mysql_xa_recover()`.
- `mariadb/sql/sql_prepare.cc` calls `xa_recover_get_fields()` while preparing
  `XA RECOVER`, so a disabled embedded source still needs the metadata helper
  for retained parser/prepare links.
- `mariadb/sql/mysqld.cc` calls `xid_cache_init()` and `xid_cache_free()` at
  embedded runtime startup and shutdown.
- Ordinary local transaction paths use `THD::get_xid()`, which returns the
  implicit transaction XID unless `XID_STATE::is_explicit_XA()` is true.
  MyLite's disabled source never creates an explicit XA state.

## Design

Add `MYLITE_WITH_XA_RUNTIME`, defaulting `ON` for inherited embedded builds
and forced `OFF` by the MyLite embedded baseline. When disabled, the option
requires the no-binlog profile and disabled mmap `tc.log` coordinator, because
external XA recovery is valid only when a durable transaction coordinator is
available.

When disabled:

- `mariadb/sql/xa.cc` is removed from `sql_embedded`;
- `mariadb/sql/mylite_xa_disabled.cc` provides the retained symbols;
- XID-cache startup and shutdown become inert;
- explicit XA command functions fail closed with `ER_NOT_SUPPORTED_YET`;
- `XID_STATE` remains in the non-XA state and does not allocate XID-cache pins;
- `xa_recover_get_fields()` still builds the inherited `XA RECOVER` metadata
  for retained prepare-time paths; and
- top-level direct and prepared `XA` statements remain rejected earlier by
  MyLite's public SQL policy.

## Compatibility Impact

External distributed transactions remain out of scope. This slice does not add
or remove any supported MyLite public API behavior because direct and prepared
`XA` SQL is already rejected by policy.

Supported ordinary transaction behavior must remain unchanged: `START
TRANSACTION`, `COMMIT`, `ROLLBACK`, savepoints, clean reopen, child-process
crash recovery, and native engine table coverage continue to run through the
existing transaction tests.

## Database-Directory Impact

The disabled runtime does not create XID-cache state or durable recovery files.
The previous `tc.log` trim already asserts no `tc.log` sidecar appears in the
MyLite database directory during supported server-surface coverage.

## Binary-Size Impact

Before implementation, the measured stripped embedded archive was:

| Artifact | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,039,248 | 24.83 | 693 |

After replacing `xa.cc` with the disabled embedded source, the measured
stripped archive is:

| Artifact | Bytes | MiB | Members |
| --- | ---: | ---: | ---: |
| `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,028,560 | 24.82 | 693 |

The slice saves 11,232 pre-strip bytes and 10,688 stripped bytes with no
archive member-count change.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

Additional checks:

- the embedded archive reports `MYLITE_WITH_XA_RUNTIME:BOOL=OFF`;
- the embedded archive no longer contains `xa.cc.o`;
- direct and prepared `XA` SQL continue to fail with the stable
  server-surface diagnostic; and
- ordinary transaction and recovery tests still pass.

## Acceptance Criteria

- The embedded archive builds and measures with `MYLITE_WITH_XA_RUNTIME=OFF`.
- External XA remains explicitly unsupported in docs and tests.
- Existing transaction, recovery, native engine, and server-surface policy
  tests pass.
- Build, test, format, tidy, and diff checks pass.

## Risks And Unresolved Questions

- A future MyLite-owned external-XA design would need to restore a durable
  transaction-coordinator story and a tested recovery model.
- Raw inherited embedded API callers that bypass `libmylite` will now see
  fail-closed XA routines instead of MariaDB's full external-XA behavior in the
  MyLite baseline.
