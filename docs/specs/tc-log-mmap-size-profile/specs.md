# TC Log Mmap Size Profile

## Problem Statement

The aggressive embedded minsize profile still links MariaDB's memory-mapped
transaction coordinator log. That code owns the server-side `tc.log` file used
for two-phase commit recovery when the binary log is not the coordinator. MyLite
must not rely on an inherited external durable `tc.log` sidecar for its final
single-file runtime; recovery has to be owned by the `.mylite` file format and
documented MyLite companion files.

The current baseline after the deeper no-binlog cleanup is:

| Artifact | Bytes |
| --- | ---: |
| `libmysqld/libmariadbd.a` | 30,703,028 |
| stripped `mylite-open-close-smoke` | 5,757,656 |

## Source Findings

- Imported MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `vendor/mariadb/server/sql/log.h` defines `TC_LOG`, `TC_LOG_DUMMY`, and
  `TC_LOG_MMAP`. When `HAVE_MMAP` is set, the real `TC_LOG_MMAP` class contains
  `logname`, `fd`, mmap state, page state, mutexes, condition variables, and
  checkpoint bookkeeping for a file-backed transaction coordinator log.
- `vendor/mariadb/server/sql/log.cc` implements
  `TC_LOG_MMAP::open()`, `close()`, `recover()`, `log_and_order()`,
  `log_one_transaction()`, `sync()`, `unlog()`, and checkpoint cleanup. The
  implementation opens or creates `opt_tc_log_file`, mmaps it, writes a magic
  header, runs recovery through `ha_recover()`, and deletes the file on clean
  close.
- `vendor/mariadb/server/sql/mysqld.cc` defaults `opt_tc_log_file` to
  `tc.log`, opens `tc_log` during server startup, and exposes status variables
  such as `Tc_log_max_pages_used`, `Tc_log_page_size`, and
  `Tc_log_page_waits`.
- `vendor/mariadb/server/sql/handler.cc` uses `tc_log` in the transaction
  commit path through `tc_log->log_and_order()`, `tc_log->unlog()`, and
  `tc_log->commit_checkpoint_notify()`.
- The linked smoke binary still contains the real `TC_LOG_MMAP` vtable and
  methods. The directly visible linked methods are small, but they are
  inherited file-backed recovery code that is not part of MyLite's embedded
  storage contract.

## Scope

Add a MyLite minsize option that disables the real mmap transaction coordinator
log only for the embedded library. The option will:

- keep the base `TC_LOG` and `TC_LOG_DUMMY` types;
- make `TC_LOG_MMAP` an alias of `TC_LOG_DUMMY` for the embedded minsize build;
- omit the real `TC_LOG_MMAP` method implementations from `log.cc`;
- leave `opt_tc_log_size` and `Tc_log_*` status variables defined as inert
  zero-valued globals so system variable/status tables continue to link;
- choose `tc_log_dummy` in the no-binlog embedded profile; and
- keep non-minsize MariaDB-derived embedded builds unchanged.

## Non-Goals

- Do not remove transaction coordinator call sites from `handler.cc`.
- Do not claim durable two-phase commit recovery for multiple XA-capable
  engines in the aggressive profile.
- Do not remove `tc-heuristic-recover` parser/options in this slice.
- Do not implement MyLite's final transaction recovery format.
- Do not change public `libmylite` API or `.mylite` file format.

## Proposed Design

Add `MYLITE_DISABLE_TC_LOG_MMAP` to
`vendor/mariadb/server/libmysqld/CMakeLists.txt` and enable it from
`tools/build-mariadb-minsize.sh`. Require
`MYLITE_DISABLE_BINLOG_CORE=ON` when this option is enabled, because the
aggressive profile already removes binlog-as-transaction-coordinator behavior.

In `log.h`, define a small `MYLITE_HAVE_TC_LOG_MMAP` helper when MariaDB mmap
support is present and the MyLite disable flag is not active. Use that helper to
compile the real `TC_LOG_MMAP` class only when it is actually available; else
map `TC_LOG_MMAP` to `TC_LOG_DUMMY`.

In `log.cc`, keep the transaction coordinator status globals available, but
guard the real `TC_LOG_MMAP` implementation block with
`MYLITE_HAVE_TC_LOG_MMAP`. The global `tc_log_mmap` remains available; in the
disabled profile it is a dummy object.

In `get_tc_log_implementation()`, return `tc_log_dummy` directly in the
no-binlog embedded profile when `MYLITE_DISABLE_TC_LOG_MMAP` is enabled.

## Affected Subsystems

- Embedded minsize CMake profile.
- MariaDB transaction coordinator selection.
- Startup and shutdown of inherited transaction coordinator logging.
- Status/system variable storage for `tc_log` values.

## Single-File And Embedded-Lifecycle Impact

This slice removes an inherited persistent sidecar mechanism from the linked
embedded runtime. The result is closer to MyLite's target lifecycle because the
default profile no longer contains executable code that creates or recovers
`tc.log`.

The risk is transactional: if a later minsize configuration enables multiple
durable XA-capable engines before MyLite-owned recovery exists, dummy
coordination is not a correct replacement for MariaDB's `tc.log`. That is
acceptable for the current smallest profile only because unrelated durable
engines are disabled and MyLite recovery remains a separate slice.

## Public API Or File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size Impact

Expected linked savings are modest: the currently visible `TC_LOG_MMAP` methods
sum to only a few KiB. The value of the slice is mostly removing server-side
sidecar recovery code from the shipping artifact, with exact archive and linked
deltas recorded after implementation.

Implemented measurements from `build/mariadb-minsize-no-tc-log-mmap`:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 30,703,028 | 30,685,528 | -17,500 |
| unstripped `mylite-open-close-smoke` | 8,001,824 | 7,993,536 | -8,288 |
| stripped `mylite-open-close-smoke` | 5,757,656 | 5,751,536 | -6,120 |

The linked smoke no longer defines real `TC_LOG_MMAP` methods or its vtable.
Only the dummy `tc_log_mmap` object, `opt_tc_log_size`, and zero-valued
`Tc_log_*` status globals remain.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tc-log-mmap \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tc-log-mmap \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-tc-log-mmap \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh
git diff --check
```

Measure:

- `libmysqld/libmariadbd.a` bytes and object count;
- unstripped and stripped `mylite-open-close-smoke`;
- `size` section totals;
- remaining `TC_LOG_MMAP` symbols in the linked smoke binary; and
- sidecar scan results from the compatibility harness.

## Acceptance Criteria

- The minsize build completes.
- The open/close smoke and compatibility harness pass.
- The linked smoke no longer defines real `TC_LOG_MMAP` methods or vtable
  symbols.
- No unexpected `tc.log` or other inherited sidecar appears in the harness.
- Size results are recorded here and in
  `docs/research/production-size-analysis.md`.

All criteria passed for the current aggressive minsize profile.

## Risks And Unresolved Questions

- The status variables remain visible but report zeros in the disabled profile.
- `tc-heuristic-recover` remains accepted as inherited server option plumbing,
  but it has no useful runtime effect without a real `tc.log` coordinator.
- Final MyLite recovery design still needs a dedicated storage/file-format
  slice.
