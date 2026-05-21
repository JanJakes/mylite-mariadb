# Binlog Replay Trim

## Problem Statement

The MyLite embedded profile rejects replication and binlog topology surfaces,
and MariaDB's embedded dispatcher already fail-closes SQL `BINLOG` replay. The
default embedded archive still linked `sql_binlog.cc`, which implements SQL
`BINLOG` base64 event replay for server replication workflows.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `BINLOG '...'` and fragmented
  `BINLOG @a,@b` as `SQLCOM_BINLOG_BASE64_EVENT`.
- `mariadb/sql/sql_parse.cc` returns an embedded-server error for
  `SQLCOM_BINLOG_BASE64_EVENT` when `EMBEDDED_LIBRARY` is defined.
- `mariadb/libmysqld/CMakeLists.txt` still linked `../sql/sql_binlog.cc` into
  `libmariadbd.a`.
- MyLite policy already rejects the surrounding binlog and replication command
  families; SQL `BINLOG` replay was the uncovered direct replay form.

## Proposed Design

Add `MYLITE_WITH_BINLOG_REPLAY`, defaulting to `ON` for normal embedded builds
and forced `OFF` in the MyLite embedded baseline. When disabled, `libmysqld`
omits `sql_binlog.cc` from the embedded archive. MyLite rejects direct and
prepared SQL `BINLOG` replay before dispatch.

## Compatibility Impact

No supported application-data behavior is removed. SQL `BINLOG` replay is a
replication/binlog replay surface. Ordinary SQL execution, native storage,
JSON, GEOMETRY, and retained binlog compatibility variables such as
`@@log_bin=0` are unchanged.

## Binary-Size Impact

Measured with `tools/mariadb-embedded-build all`: `libmariadbd.a` is
26,474,416 bytes / 25.25 MiB with 699 members, down 3,640 bytes from the prior
26,478,056-byte embedded profile.

## Test And Verification Plan

- Run `tools/mariadb-embedded-build all`.
- Confirm `MYLITE_WITH_BINLOG_REPLAY=OFF` appears in the embedded CMake cache.
- Confirm `sql_binlog.cc.o` is absent from `libmariadbd.a`.
- Verify direct and prepared SQL `BINLOG` replay statements fail through MyLite
  server-surface policy.
- Run the normal embedded and first-party CMake test, format, and tidy gates.

## Acceptance Criteria

- The embedded archive omits MariaDB's SQL `BINLOG` replay implementation.
- SQL `BINLOG` replay is rejected explicitly by MyLite policy coverage.
- Normal application SQL and native storage behavior remain covered by the
  existing test suite.
