# MTR Profile Disabled Engine Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_engines` case. The test covers native storage engines
that the MyLite MTR smoke profile intentionally leaves out of the embedded
server build.

## Non-Goals

- MyLite storage-engine routing coverage.
- Engine DDL fallback or substitution behavior.
- Re-enabling native durable engines for upstream MTR compatibility.
- Full `INFORMATION_SCHEMA.ENGINES` coverage.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables native MyISAM,
  MRG_MyISAM, CSV, and Performance Schema in the embedded baseline.
- `tools/mylite-mtr-harness` requires the MTR smoke cache to keep
  `PLUGIN_INNOBASE=NO`, `PLUGIN_PARTITION=NO`, `PLUGIN_CSV=NO`,
  `MYLITE_WITH_NATIVE_MYISAM_STORAGE_ENGINE=OFF`,
  `MYLITE_WITH_NATIVE_MYISAMMRG_STORAGE_ENGINE=OFF`, and
  `MYLITE_WITH_CSV_STORAGE_ENGINE=OFF`.
- Several upstream MTR candidates are skipped by MTR or drift under the MyLite
  smoke profile because they require Sequence, InnoDB, MyISAM, CSV,
  Performance Schema, or partition support that the raw embedded profile
  deliberately omits.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_engines`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled native-engine absence. This is raw embedded-profile
evidence only; MyLite storage-engine routing remains covered by the
storage-smoke and `libmylite` tests, not by this MTR profile test.

## Design

- Add `mariadb/mysql-test/suite/mylite/t/profile_disabled_engines.test` and
  its expected result file.
- Add `mylite.profile_disabled_engines` to the default curated MTR smoke list
  near the other MyLite profile tests.
- Assert that `INFORMATION_SCHEMA.ENGINES` has no rows for the intentionally
  omitted engines: `CSV`, `InnoDB`, `MyISAM`, `MRG_MyISAM`,
  `PERFORMANCE_SCHEMA`, and `SEQUENCE`.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile.

## Build, Size, And Dependencies

No production dependency or binary-size change. The new test exercises existing
compile-time profile switches and the MTR smoke profile's required cache
entries.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_engines`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `mylite.profile_disabled_engines`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled native-engine absence.

## Risks And Open Questions

- This is intentionally not a storage-routing claim. File-backed MyLite
  sessions route supported engine names to MyLite in the storage-smoke profile,
  while the raw MTR smoke profile documents the underlying native-engine trims.
- Broader upstream MTR engine suites should stay out of the curated list until
  a comparison slice can classify expected raw-profile differences.
