# Ownerless Page-Log CRC32C Checksum

## Problem

The ownerless page-log append profile showed that 200 ownerless autocommit
inserts spent `150.829ms` in page-log append work, with `86.024ms` in payload
checksum calculation and `46.033ms` in payload writes. Lock acquisition,
header validation, `fstat()`, and record-header writes were comparatively
small.

The current page-version WAL checksum is an internal byte-at-a-time FNV64
calculation over every page image. It is substantially slower than MariaDB's
optimized CRC32C implementation, which InnoDB already uses for native page and
redo validation. Replacing it naively would be unsafe because retained WAL
records created by older ownerless builds still store FNV64 values in the same
64-bit checksum field.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `26d90657`.
- `mariadb/include/my_sys.h` declares `my_crc32c()`.
- `mariadb/mysys/crc32/crc32c.cc` chooses an optimized CRC32C implementation
  and exposes `my_crc32c()`.
- InnoDB uses `my_crc32c()` for native page and redo checksums in paths such as
  `mariadb/storage/innobase/buf/buf0checksum.cc`,
  `mariadb/storage/innobase/buf/buf0flu.cc`, and
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`.
- `packages/libmylite/src/ownerless_page_log.cc::append_locked()` computes the
  page-version record checksum before writing the payload and record header.
- `record_payload_status()`, `find_latest_in_snapshot()`,
  `read_record_at_locked()`, `checkpoint_locked()`, and
  `checkpoint_preserving_oldest_snapshot_locked()` validate existing payloads
  before returning, replaying, or preserving records.

## Design

Use MariaDB CRC32C for new ownerless page-log records in embedded builds while
keeping the 64-bit checksum field and record layout unchanged.

New records store two CRC32C values in the existing `payload_checksum` field:

- low 32 bits: `my_crc32c(0, page, page_size)`,
- high 32 bits: `my_crc32c(0xa5a5a5a5, page, page_size)`.

The dual-seed encoding keeps a 64-bit payload guard while using MariaDB's
optimized CRC32C path. In non-embedded builds, where `my_crc32c()` is not
linked, `checksum_bytes()` keeps the legacy FNV64 calculation.

Readers validate the current checksum first. If it does not match, they also
compute and accept the legacy FNV64 checksum. This lets a newer build read,
replay, and checkpoint retained WAL records produced by previous ownerless
builds. Corrupt records still fail unless the corrupted bytes collide with one
of the checksum algorithms.

All validation sites must use the compatibility-aware checksum predicate rather
than directly comparing `checksum_bytes()`.

## Compatibility Impact

No SQL, public C API, PHP API, durable file name, record header size, or page-log
offset changes.

The internal durable checksum encoding for newly appended page-version records
changes in embedded builds, but the reader remains backward-compatible with
legacy FNV64 records. Forward compatibility with old binaries reading new WAL
is not claimed; MyLite's ownerless concurrency branch does not support mixed
binary-version concurrent writers.

## Directory And Lifecycle Impact

No new files or directory state. Retained page-version WAL created before this
slice remains readable by the new code.

## Native Storage Impact

No native InnoDB format changes. The page-version WAL still writes payload
bytes before the record header, and incomplete or checksum-corrupt tail records
remain invisible to readers and checkpoint cleanup.

## Build And Performance Impact

Embedded builds already link the MariaDB embedded archive that provides
`my_crc32c()`. No new dependency is introduced.

The expected append improvement is a lower
`*_page_log_append_checksum_ms` value in the stats-enabled embedded performance
probe. Payload write and refresh/read costs are not directly changed.

## Performance Findings

Local profiling on 2026-06-08 at slice start head `26d90657` used
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

The prior append-profile sample reported, for 200 ownerless autocommit
inserts:

- ownerless autocommit throughput: `111.33 ops/s`,
- page-publish hook append time: `151.361ms`,
- page-log append total: `150.829ms`,
- page-log append checksum: `86.024ms`,
- page-log append payload write: `46.033ms`,
- page-read total: `173.036ms`,
- page-write refresh: `163.574ms`.

After switching new records to CRC32C-derived checksums, the same probe shape
reported:

- ownerless autocommit throughput: `85.73 ops/s`,
- page-publish hook append time: `80.763ms`,
- page-log append total: `79.326ms`,
- page-log append checksum: `4.386ms`,
- page-log append payload write: `52.507ms`,
- page-read total: `240.653ms`,
- page-write refresh: `180.245ms`,
- page-publish failures: `0`,
- page-version checksum failures: `0`,
- disk checksum failures: `0`.

The targeted append checksum cost dropped by roughly `95%`
(`86.024ms` to `4.386ms`). The full ownerless autocommit rate did not improve
in this single noisy sample because page-read and page-write refresh costs
were higher than the previous run and remain larger than append checksum cost.
The next performance target remains refresh/read reduction or payload-write
reduction, not checksum computation.

## Test Plan

- Add primitive coverage that rewrites an appended page-log record checksum to
  the legacy FNV64 value and verifies read, direct-offset read, replay, and
  checkpoint paths accept it.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run the stats-enabled embedded performance probe and compare
  `*_page_log_append_checksum_ms` against the prior `86.024ms` ownerless
  autocommit append-checksum sample.
- Run focused live visibility selectors:
  `prepared-committed-read` and `local-write-first-read`.
- Run focused DDL/CTAS guard selectors:
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run focused ownerless primitive CTests.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_ownerless_cross_process_sql_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_primitives_test`
  passed, including the new legacy-checksum record test and existing
  corrupt-tail/interior checksum coverage.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and emitted the performance counters recorded above.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  local-write-first-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ctas-post-create-dml` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ddl-broader` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  online-ddl-options` passed.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed two tests.
- `cmake --build --preset dev --target mylite` passed, covering the
  non-embedded build path where `checksum_bytes()` keeps the legacy FNV64
  implementation.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- New page-log records in embedded builds use the CRC32C-derived checksum.
- Legacy FNV64 page-log records remain readable and checkpointable.
- Existing corrupt-tail and corrupt-interior record behavior stays covered.
- The stats-enabled probe shows lower ownerless append checksum time, without
  page-publish failures or refresh checksum failures.
- Focused ownerless correctness tests pass.

## Risks And Follow-Up

- CRC32C reduces checksum CPU cost but does not address payload write cost,
  page-version read cost, or page-write refresh cost.
- Mixed old/new binary access to the same live ownerless directory remains out
  of scope. The compatibility guarantee is for retained WAL read by newer code.
