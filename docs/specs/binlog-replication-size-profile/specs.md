# Binlog Replication Size Profile

## Problem

The aggressive MyLite minsize profile still links MariaDB binary-log and
replication code even though MyLite's embedded product surface does not expose
replication, `mysqlbinlog`, network binlog streaming, or durable binlog files.

The current `regex-function-size-profile` build measures:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 33,699,880 |
| stripped `mylite-open-close-smoke` | 6,749,888 |

The linked smoke binary still contains `MYSQL_BIN_LOG`, `Log_event`,
`Rows_log_event`, `Gtid_log_event`, `rpl_binlog_state`, and
`Gtid_index_writer` symbols. This slice tests how much can be removed from the
embedded minsize profile without changing MyLite's single-file API surface.

## Source Findings

MariaDB base version: `vendor/mariadb/server/VERSION` reports 11.8.6.

Embedded builds define `EMBEDDED_LIBRARY` in
`vendor/mariadb/server/libmysqld/CMakeLists.txt`, so
`vendor/mariadb/server/include/my_global.h` does not define
`HAVE_REPLICATION` for `libmysqld`. That already compiles out many server
replication blocks.

The embedded SQL archive still compiles the following binlog and replication
source files from `vendor/mariadb/server/libmysqld/CMakeLists.txt`:

- `../sql/log.cc`
- `../sql/log_event.cc`
- `../sql/log_event_server.cc`
- `../sql/log_cache.cc`
- `../sql/sql_binlog.cc`
- `../sql/sql_repl.cc`
- `../sql/slave.cc`
- `../sql/repl_failsafe.cc`
- `../sql/rpl_filter.cc`
- `../sql/rpl_record.cc`
- `../sql/rpl_injector.cc`
- `../sql/rpl_utility.cc`
- `../sql/rpl_utility_server.cc`
- `../sql/rpl_reporting.cc`
- `../sql/rpl_gtid.cc`
- `../sql/gtid_index.cc`

Most command-level replication code is already empty in embedded builds because
`sql_repl.cc`, `slave.cc`, and `repl_failsafe.cc` are largely guarded by
`HAVE_REPLICATION`. The remaining non-trivial objects are:

| Object | Current bytes |
| --- | ---: |
| `rpl_utility_server.cc.o` | 401,688 |
| `log.cc.o` | 334,456 |
| `log_event_server.cc.o` | 289,376 |
| `log_event.cc.o` | 138,816 |
| `rpl_gtid.cc.o` | 126,464 |
| `gtid_index.cc.o` | 58,960 |
| `rpl_filter.cc.o` | 37,728 |

`vendor/mariadb/server/sql/log.h` defines the shared transaction coordinator
types, `Event_log`, and `MYSQL_BIN_LOG`. `log.cc` defines the global
`mysql_bin_log`, `binlog_tp`, binlog plugin declaration, binlog cache helpers,
and `MYSQL_BIN_LOG` methods. Generic SQL paths still reference those symbols:

- `handler.cc` transaction ordering special-cases `binlog_tp` and calls
  `binlog_commit()` / `binlog_rollback()`.
- `sql_class.cc` checks `mysql_bin_log.is_open()`, starts and stops union
  events, and writes query events when binlogging is enabled.
- `sys_vars.cc` exposes binlog and GTID state variables and calls
  `mysql_bin_log` methods.
- `libmysqld/lib_sql.cc` allocates `binlog_filter` and `global_rpl_filter`.

`sql_binlog.cc` contains `BINLOG` statement execution and event defragmenting.
It is not fully guarded by `HAVE_REPLICATION`, but `sql_parse.cc` already
rejects `SQLCOM_BINLOG_BASE64_EVENT` under `EMBEDDED_LIBRARY`, so the embedded
minsize profile can omit the replay implementation without a replacement shim.

`rpl_utility_server.cc` contains field-conversion helpers that are referenced
from retained field/type code even without replication. That file is currently
the largest replication-named object, but it is not safely removable by source
list pruning alone.

## Scope

This slice may:

- add a MyLite-owned `MYLITE_DISABLE_BINLOG_REPLICATION` minsize option,
- omit command-level replication sources that compile to empty or nearly empty
  objects,
- omit `sql_binlog.cc` if a small unsupported-command shim satisfies retained
  references,
- keep `log.cc` unless a narrowly scoped no-binlog stub can satisfy all generic
  transaction and SQL references,
- update smoke tests for binlog/replication SQL surfaces, and
- update production size analysis with measured archive, linked-runtime, and
  symbol deltas.

## Non-Goals

This slice does not remove:

- the transaction coordinator abstraction in `log.h`,
- generic binlog type metadata used by field/type code,
- row-format helper types that retained SQL/table code still needs,
- `rpl_utility_server.cc` unless the implementation proves it is dead in the
  embedded minsize link, or
- all `MYSQL_BIN_LOG` code if doing so requires a broad rewrite of transaction
  handling.

It also does not claim final compatibility for replication-related system
variables until the implementation is measured and tested.

## Proposed Design

Add `MYLITE_DISABLE_BINLOG_REPLICATION` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it from
`tools/build-mariadb-minsize.sh`.

First, remove sources whose embedded objects are empty or command-only:

- `../sql/slave.cc`
- `../sql/sql_repl.cc`
- `../sql/repl_failsafe.cc`
- `../sql/rpl_reporting.cc`

Second, remove `../sql/sql_binlog.cc` if embedded dispatch already rejects
`SQLCOM_BINLOG_BASE64_EVENT`. In MariaDB 11.8.6,
`vendor/mariadb/server/sql/sql_parse.cc` reports
`ER_OPTION_PREVENTS_STATEMENT` for `BINLOG` under `EMBEDDED_LIBRARY`, so the
full replay implementation should not be needed for MyLite's embedded
profile.

Third, test removing `../sql/log_event_server.cc`, `../sql/rpl_gtid.cc`, and
`../sql/gtid_index.cc`. If generic state-variable or binlog-recovery references
pull them back in, leave them in place and record the blocker. These are likely
larger than the command-only sources but riskier because `log.cc` and sysvars
still reference GTID state and event classes.

Defer full `log.cc` replacement unless the first passes show a clear,
contained stub boundary. `log.cc` is coupled to transaction participants,
`MYSQL_BIN_LOG`, `TC_LOG`, binlog plugin variables, and generic `THD` helpers.
Replacing it safely is a separate deeper slice if this attempt proves it is the
remaining large target.

## Affected Subsystems

- Embedded SQL source list and built-in binlog plugin shape.
- Parser/executor diagnostics for `BINLOG`, replication, and slave/source
  commands.
- Transaction participant and commit ordering paths if `log.cc` is changed.
- System variables and status variables related to binlog and GTID state.

## Single-File and Embedded Lifecycle Impact

Removing binlog and replication code aligns with MyLite's single-file runtime:
no binary log files, relay logs, GTID index files, or replication metadata files
should be created by the embedded minsize profile.

The implementation must not replace removed code with persistent sidecar files
or a hidden server-style datadir dependency.

## Public API and File-Format Impact

No public `libmylite` C API changes are expected.

No MyLite file-format changes are expected. SQL compatibility changes must be
explicit diagnostics for unsupported binlog/replication commands rather than
accidental unresolved behavior.

## Binary-Size Impact

The upper bound from current object files is about 1.40 MiB of archive objects,
but some files provide retained generic helpers and may not be removable in
this slice. The measured first-pass reduction is much smaller:

- command-only sources and `sql_binlog.cc`: 23,172 bytes from the stripped
  archive,
- `rpl_injector.cc`: kept because `lib_sql.cc` cleanup still references
  `injector::free_instance()`,
- `rpl_record.cc`: kept because retained row-binlog helper code still
  references `pack_row()`,
- `log_event_server.cc`, `rpl_gtid.cc`, `gtid_index.cc`: retained because
  `MYSQL_BIN_LOG`, row-event helpers, GTID state, and sysvar-facing code still
  pull the binlog core into linked runtime artifacts,
- `log.cc`: larger and deeper; likely a later slice.

Linked stripped binary savings are effectively zero for this slice because
section GC already drops unused command-level functions and the smoke test adds
one unsupported `BINLOG` assertion.

Measured result on top of `regex-function-size-profile`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 33,699,880 | 33,676,708 | -23,172 |
| archive object count | 458 | 453 | -5 |
| stripped `mylite-open-close-smoke` | 6,749,888 | 6,750,400 | +512 |

## License, Trademark, and Dependency Impact

This is a GPL-2.0-only MariaDB-derived source change. It does not add new
third-party code or dependencies. Existing MariaDB/Oracle copyright notices
remain untouched in upstream-derived files.

## Test and Verification Plan

Run:

- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh`
- `MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh`
- `git diff --check`

Add or extend smoke coverage to verify:

- ordinary create/insert/select/transaction paths still work,
- `BINLOG` fails with a stable unsupported diagnostic,
- the archive no longer contains objects for removed command/replay sources.

Measure:

- `libmysqld/libmariadbd.a` bytes,
- stripped `mylite-open-close-smoke` bytes,
- largest retained binlog/replication symbols, and
- dynamic dependency changes, if any.

## Acceptance Criteria

- The minsize profile builds and current MyLite smokes pass.
- The compatibility harness passes.
- Removed SQL surfaces fail explicitly rather than crashing or silently doing
  server-only work.
- No unexpected sidecar files are introduced.
- Size deltas are recorded in `docs/research/production-size-analysis.md`.
- The implementation remains narrowly scoped to embedded minsize build
  pruning, stubs, tests, and docs.

## Risks and Unresolved Questions

- `log.cc` may dominate retained binlog symbols and may need a separate
  no-binlog transaction-coordinator stub slice.
- Some GTID/binlog types may be retained by system variables or general SQL
  metadata even when command execution is disabled.
- Parser tokens for replication commands may still exist in `yy_mariadb.cc`;
  removing generated parser tables is out of scope for this slice.
- `rpl_utility_server.cc` looks replication-specific by name but provides
  retained field-conversion helpers, so removing it may require broader type
  surgery that is not justified without more measurement.
- `-fno-rtti` is not a viable broad compiler-size lever in the current retained
  SQL layer because `sql/cset_narrowing.h` and `sql/item_strfunc.h` use
  `dynamic_cast`.

## Implementation Result

Implemented `MYLITE_DISABLE_BINLOG_REPLICATION` in the embedded source list
and enabled it from the minsize build script. The option removes:

- `../sql/repl_failsafe.cc`
- `../sql/rpl_reporting.cc`
- `../sql/slave.cc`
- `../sql/sql_binlog.cc`
- `../sql/sql_repl.cc`

The attempted removal of `rpl_injector.cc` and `rpl_record.cc` was rejected by
the linker: retained MariaDB cleanup and row helper paths still need
`injector::free_instance()` and `pack_row()`.

The implementation adds a `BINLOG 'ZmFrZQ=='` smoke assertion. Embedded
MariaDB already rejects this through `sql_parse.cc` with
`ER_OPTION_PREVENTS_STATEMENT` and an `"embedded option"` message, so no MyLite
stub is needed.

Verification passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-binlog-replication MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

This slice is worth keeping as a small archive cleanup and explicit unsupported
surface test. It does not replace a deeper no-binlog-core slice if linked
runtime size remains the priority.
