# Replication Type Conversion Trim

## Problem

The default embedded profile rejects replication and binary-log command
families, starts with binary logging disabled, and omits SQL `BINLOG` replay,
but the archive still builds MariaDB's row-replication type-conversion helpers
from `mariadb/sql/rpl_utility_server.cc`.

Those helpers compare source and target row-event field definitions, build
conversion tables for row-based replication apply, and render binary-log type
names for replication error reporting. They are not ordinary SQL type
conversion, native storage, transaction, JSON, GEOMETRY/GIS, sequence, or public
C API behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/rpl_utility_server.cc` defines row-replication conversion checks
  such as `Field::*::rpl_conv_type_from()`, `Type_handler::*::show_binlog_type()`,
  and `Type_handler::*::max_display_length_for_field()` implementations used by
  row-event apply.
- `mariadb/sql/rpl_utility.cc` still provides shared table-map metadata helpers
  referenced by retained binary-log event code, so this slice cannot delete all
  replication utility code.
- `packages/libmylite/tests/embedded_server_surface_policy_test.c` already
  rejects replication, binlog, GTID helper functions, and replication execution
  variables directly and in prepared statements.
- The retained SQL sequence code uses normal integer type handlers, not the
  disabled row-event conversion path.

## Design

Add `MYLITE_WITH_RPL_TYPE_CONVERSION`, defaulting to `ON` for upstream-style
embedded builds and forced `OFF` in the MyLite embedded baseline.

When disabled:

- build `mylite_rpl_utility_server_disabled.cc` instead of
  `rpl_utility_server.cc`;
- keep the virtual method link contract required by retained type-handler and
  field classes;
- return `CONV_TYPE_IMPOSSIBLE` from row-replication conversion checks;
- leave binary-log type-rendering strings empty for fail-closed row-event error
  paths;
- retain `rpl_utility.cc` table-map metadata helpers.

## Compatibility Impact

No supported SQL or public API behavior changes. Replication row-event apply is
already outside the default embedded core. Ordinary SQL expression conversion,
DDL/DML, prepared statements, native storage engines, transactions, JSON,
GEOMETRY/GIS, sequence handling, and directory lifecycle stay on retained
non-replication paths.

## Directory And Lifecycle Impact

No file-format change and no new durable, temporary, lock, metadata, or runtime
paths. The slice only removes row-replication conversion implementation from the
default embedded archive.

## Binary Size Impact

On this branch, replacing `rpl_utility_server.cc.o` with
`mylite_rpl_utility_server_disabled.cc.o` reduced the stripped archive from
26,265,424 bytes / 25.05 MiB to 26,258,720 bytes / 25.04 MiB with the member
count unchanged at 698. The pre-strip archive moved from 26,829,192 bytes to
26,822,408 bytes.

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
cmake --build --preset embedded-dev --target format
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_RPL_TYPE_CONVERSION=OFF` appears in the embedded CMake cache.
- `rpl_utility_server.cc.o` is absent from `libmariadbd.a`.
- `mylite_rpl_utility_server_disabled.cc.o` is present in `libmariadbd.a`.
- Replication, binlog, GTID, and replication execution variable policy coverage
  still rejects unsupported server topology surfaces.
- Supported SQL, native storage, transactions, prepared statements, JSON,
  GEOMETRY/GIS, `NOCACHE` sequence persistence, and directory lifecycle
  coverage still pass.
