# Log Event Parsing Trim

## Problem

The default embedded profile rejects replication and binary-log command
families, starts with binary logging disabled, omits SQL `BINLOG` replay, and
replaces server-side binary-log event writers with fail-closed stubs, but the
archive still builds MariaDB's common binary-log event parser and reader from
`mariadb/sql/log_event.cc`.

That object decodes binary-log event payloads, creates event instances from
`IO_CACHE`, handles format-description state, and supports replay/replication
paths that are outside the core embedded profile. It is not ordinary SQL
parsing, native storage, JSON, GEOMETRY/GIS, transaction, or public C API
behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/log_event.cc` implements binary-log event decoding and reader
  helpers such as `Log_event::read_log_event()`, format-description event
  setup, compressed-event helpers, and event-class virtual methods used by
  replay and replication paths.
- The default embedded profile already omits `rpl_record.cc`,
  `sql_binlog.cc`, `log_event_server.cc`, `rpl_gtid.cc`, and
  `rpl_utility_server.cc`.
- Retained MariaDB code still references a small set of log-event class
  symbols, typeinfo/vtables, checksum metadata, and the `str_to_hex()` helper
  used by `append_query_string()`.
- The retained ordinary SQL helper is string literal rendering, not binary-log
  event decode or replay.

## Design

Add `MYLITE_WITH_LOG_EVENT_PARSING`, defaulting to `ON` for upstream-style
embedded builds and forced `OFF` in the MyLite embedded baseline.

When disabled:

- omit `log_event.cc` from `libmysqld` embedded SQL sources;
- keep the option local to the embedded archive so normal MariaDB server builds
  preserve upstream event parsing;
- reject the invalid custom-profile combination where event parsing is disabled
  but upstream server event writers remain enabled;
- extend `mylite_log_event_server_disabled.cc` with the minimal common
  log-event link contract retained code needs;
- keep `append_query_string()` and its `str_to_hex()` helper for ordinary SQL
  literal rendering;
- make `Log_event::read_log_event()` fail closed;
- keep minimal destructors, vtables, checksum metadata, and format-description
  symbols required by retained unsupported paths.

## Compatibility Impact

No supported SQL or public API behavior changes. Binary-log event decode and
replay are already outside the embedded core. Replication and binlog SQL remain
rejected by policy, and `@@log_bin=0` remains covered. Ordinary SQL parsing,
prepared statements, diagnostics, native storage engines, transactions, JSON,
GEOMETRY/GIS, sequence handling, and directory lifecycle stay on retained
non-binary-log paths.

## Directory And Lifecycle Impact

No file-format change and no new durable, temporary, lock, metadata, or runtime
paths. The slice only removes binary-log event parser/reader implementation
from the default embedded archive.

## Binary Size Impact

On this branch, omitting `log_event.cc.o` and extending the disabled embedded
event source reduced the stripped archive from 26,258,720 bytes / 25.04 MiB
to 26,195,576 bytes / 24.98 MiB. The member count dropped from 698 to 697.
The pre-strip archive moved from 26,822,408 bytes to 26,758,104 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
ar -t build/mariadb-embedded/libmysqld/libmariadbd.a | rg '(^|/)log_event\.cc|mylite_log_event_server'
tools/mariadb-embedded-build measure
cmake --preset embedded-dev
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_LOG_EVENT_PARSING=OFF` appears in the embedded CMake cache.
- `log_event.cc.o` is absent from `libmariadbd.a`.
- `mylite_log_event_server_disabled.cc.o` is present in `libmariadbd.a`.
- Replication and binlog policy coverage still rejects unsupported server
  topology surfaces.
- Supported SQL, native storage, transactions, prepared statements, JSON,
  GEOMETRY/GIS, sequence handling, and directory lifecycle coverage still
  pass.
