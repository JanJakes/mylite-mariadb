# MTR Temporal Literal Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.temporal_literal`. This
adds upstream embedded baseline coverage for SQL temporal literal parsing,
metadata inference, zero-date handling, nanosecond rounding, prepared
statements, and temporal literal behavior under strict date SQL modes.

## Non-Goals

- Broad temporal MTR coverage.
- Running MTR against MyLite storage-engine routing.
- Changing MyLite SQL behavior, storage routing, public API behavior, or file
  format.
- Normalizing tests that depend on disabled native MyISAM, InnoDB metadata,
  partitioning, sequence tables, or host-file I/O.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/temporal_literal.test` covers `DATE`, `TIME`, and
  `TIMESTAMP` literal parsing, ODBC literal forms, literal metadata in CTAS,
  zero-date and zero-time mode behavior, nanosecond rounding and rejection,
  temporal literal expressions in prepared statements, and
  `NO_ZERO_IN_DATE`/`NO_ZERO_DATE` transitions.
- The only observed probe drift is `SHOW CREATE TABLE` output from the MTR
  smoke profile's Aria default engine: upstream expected results contain
  `ENGINE=MyISAM`, while the profile reports `ENGINE=Aria ... PAGE_CHECKSUM=1`.
- Existing admitted MTR tests already use
  `--replace_result ENGINE=Aria ENGINE=MyISAM " PAGE_CHECKSUM=1" ""` for the
  same profile-specific default-engine output.
- Nearby rejected temporal probes remain outside this slice:
  - `main.type_temporal_mysql56` opens a copied MyISAM `.frm` table and fails
    because native MyISAM is disabled in the smoke profile.
  - `main.type_timestamp_hires` requires disabled InnoDB information-schema
    option registration during embedded bootstrap.

## Design

- Add profile-specific `SHOW CREATE TABLE` result normalization before each
  CTAS metadata assertion in `temporal_literal.test`.
- Add `main.temporal_literal` to the curated MTR smoke list near the existing
  temporal scale and microsecond parsing tests.
- Keep the compatibility claim scoped to curated opt-in embedded MTR evidence.

## Compatibility Impact

The curated MTR smoke runner gains upstream MariaDB baseline coverage for
temporal literal parsing and CTAS metadata inference. This does not change
MyLite runtime behavior and does not prove MyLite storage-routing temporal
behavior.

## Single-File And Lifecycle Impact

No MyLite `.mylite` file lifecycle change. The test runs in the MTR smoke
vardir.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change. The opt-in MTR
build tree can be reclaimed with `rm -rf build/mariadb-mtr-smoke` or
`rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe main.temporal_literal main.type_temporal_mysql56 main.type_timestamp_hires main.timezone`
- `tools/mylite-mtr-harness run main.temporal_literal main.timezone`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find . -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.temporal_literal` reports an MTR pass under strict harness execution.
- The full curated MTR smoke suite remains green.
- No `.reject` files remain.
- Docs describe temporal literal coverage without claiming broad temporal-suite
  parity.

## Risks And Follow-Up

The broader temporal suite still includes explicit disabled-engine,
disabled-InnoDB, host-file, and environment-dependent cases. Each should remain
outside the curated list until it has a focused admission decision.
