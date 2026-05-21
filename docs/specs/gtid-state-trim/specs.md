# GTID State Trim

## Problem

MyLite's embedded profile rejects replication, binary-log administration, and
SQL `BINLOG` replay, but the default archive still carries MariaDB's
replication GTID state runtime in `sql/rpl_gtid.cc`.

GTID state is server topology state. At this slice boundary, the embedded
profile still linked retained `log.cc` and `gtid_index.cc` paths, so removing
the object required a small disabled source that preserved the link contract.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/rpl_gtid.cc` implements replication slave state,
  binary-log GTID state, GTID wait queues, GTID parser helpers, and GTID event
  filters.
- A link probe that removed `rpl_gtid.cc.o` failed on retained
  `rpl_binlog_state` and `rpl_binlog_state_base` symbols referenced from
  `log.cc` and `gtid_index.cc`.
- `gtid_index.cc` only needs an empty state contract in the default no-binlog
  embedded profile; MyLite policy already keeps public replication and binlog
  commands unreachable.
- A later binary-log GTID-index trim replaces `gtid_index.cc` with a
  fail-closed embedded link contract.

## Design

Add `MYLITE_WITH_GTID_STATE`. The default MariaDB build keeps the upstream
source. The MyLite embedded baseline sets the option to `OFF`, replaces
`rpl_gtid.cc` with `mylite_rpl_gtid_disabled.cc`, and keeps:

- empty GTID state for retained no-binlog link paths;
- fail-closed state mutation helpers for unsupported binary-log paths;
- parser/helper stubs needed by retained declarations.

MyLite policy rejects direct and prepared `MASTER_GTID_WAIT()`,
`BINLOG_GTID_POS()`, and `WSREP_SYNC_WAIT_UPTO_GTID()` calls before dispatch,
and rejects GTID state variable assignments.

## Compatibility Impact

No supported SQL, native storage, JSON, GEOMETRY, transaction, prepared
statement, or embedded C API behavior should change. Replication GTID state,
GTID helper SQL functions, and GTID state variable assignments remain
unsupported server topology behavior.

## Database Directory And Native Storage Impact

No durable paths, temporary paths, locks, metadata files, or native storage
engine files change. The slice removes replication GTID-state runtime from the
embedded archive only.

## Binary Size Impact

Measured impact is 53,688 bytes from the stripped archive with no archive member
count change: `rpl_gtid.cc.o` leaves `libmariadbd.a`, and a smaller disabled
embedded source takes its place.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_GTID_STATE=OFF` appears in the embedded CMake cache.
- Confirm `rpl_gtid.cc.o` is absent from `libmariadbd.a` and
  `mylite_rpl_gtid_disabled.cc.o` is present.
- Run the embedded and default CMake builds and tests.
- Run format, tidy, `git diff --check`, and archive measurement.
- Confirm server-surface policy coverage rejects direct and prepared
  `MASTER_GTID_WAIT()`, `BINLOG_GTID_POS()`, `WSREP_SYNC_WAIT_UPTO_GTID()`,
  and GTID state variable assignments.

## Acceptance Criteria

- Ordinary SQL execution, prepared statements, native storage, transactions,
  recovery, and compatibility harness tests pass.
- Public unsupported-surface diagnostics remain stable.
- The measured embedded archive size and member count are updated in the build
  docs.
- Documentation records the new GTID-state trim boundary.
