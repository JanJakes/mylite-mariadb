# Prepared Transient Bind Reuse

## Problem

Prepared inserts still spend visible time in the libmylite statement path. The
current bind layer marks every transient text/blob bind dirty, so a loop that
updates an integer primary key, an integer secondary key, and a transient text
payload calls `mysql_stmt_bind_param()` before every execution even when the
text payload reuses the same owned vector storage.

The local baseline before this slice was:

- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 1000`:
  `8.402 us/op` for prepared primary-key point selects.
- `tools/mylite-perf-baseline --phase=prepared-secondary-selects 1000 1000`:
  `65.264 us/op` for prepared secondary exact selects returning 100 rows each.
- `tools/mylite-perf-baseline --phase=prepared-leaf-secondary-selects 1000 1000`:
  `61.742 us/op` for published-leaf secondary exact selects returning 100 rows
  each.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000`:
  `118.109 us/op` for prepared inserts and `12.611 us/op` for prepared updates.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc::bind_bytes()` always sets
  `parameter_binds_dirty` for text/blob parameters, including transient binds.
- `packages/libmylite/src/database.cc::bind_statement_parameters()` skips
  `mysql_stmt_bind_param()` only when the statement is already bound and
  `parameter_binds_dirty` is false.
- `BoundValue::owned_data` retains vector capacity across `clear()` and
  reassignment, so many transient payloads reuse the same data pointer.
- `MYSQL_BIND::length` already points at `BoundValue::mysql_length`, so length
  changes do not require rebinding when the previously bound buffer is still
  large enough and its pointer is unchanged.

## Design

Track the buffer length that was last passed to MariaDB for each bound value.
For transient text/blob rebinds of the same kind:

- reuse the existing owned vector storage when possible;
- mark parameter binds dirty only when the vector data pointer changes, the new
  payload exceeds the last bound buffer length, or the statement has not been
  bound yet;
- keep length updates flowing through `BoundValue::mysql_length`, which MariaDB
  already reads through the stable `MYSQL_BIND::length` pointer.

All non-transient binds, kind changes, custom destructors, NULL transitions, and
oversized transient payloads keep the conservative rebind path.

## Compatibility Impact

No SQL-visible behavior should change. The optimization only skips
`mysql_stmt_bind_param()` when MariaDB can keep using the same buffer pointer
and an already-large-enough buffer length with an updated length value.

## Single-File And Lifecycle Impact

No `.mylite` file-format, storage lifecycle, or companion-file change.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

No routing change.

## Build, Size, And Dependencies

No new dependency. Binary impact is limited to a small field on `BoundValue` and
branching in the prepared bind path.

## Test Plan

- Add a libmylite statement regression that repeatedly inserts transient text
  values of shorter, equal, longer, and empty lengths through one prepared
  statement and verifies stored values.
- Build storage-smoke libmylite targets.
- Run `ctest --preset storage-smoke-dev --output-on-failure`.
- Run focused performance baselines for prepared inserts/updates.
- Run `git diff --check`.

## Acceptance Criteria

- Transient prepared text/blob rebinds preserve correct values across length
  changes.
- Existing embedded statement and storage-smoke tests pass.
- Prepared insert performance improves or stays within noise.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_statement_test mylite_perf_baseline`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure -R
  libmylite.embedded-statement`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10 tests.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000`: prepared
  inserts improved from the recorded `118.109 us/op` baseline to
  `34.430 us/op`; prepared primary-key updates were `13.838 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 5000`: prepared
  inserts were `29.914 us/op`; prepared primary-key updates were `4.954 us/op`.
- `git diff --check`: passed.

## Risks And Follow-Up

The optimization relies on MariaDB continuing to read input length through the
stored `MYSQL_BIND::length` pointer. That is the same pointer-based contract
already used by libmylite between `mysql_stmt_bind_param()` and
`mysql_stmt_execute()`. Future prepared-path work should look at reducing reset
and checkpoint overhead once bind churn is no longer the obvious local cost.
