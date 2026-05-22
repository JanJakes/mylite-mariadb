# MTR Profile Disabled Metadata Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_metadata` case. The test covers status,
process-list, and routine metadata producers that the MyLite MTR smoke profile
intentionally compiles out of the embedded server build.

## Non-Goals

- Full information-schema coverage.
- `libmylite` unsupported-surface preflight coverage.
- Re-enabling status, process-list, or routine metadata producers.
- Normalizing upstream MTR tests that assume full server metadata.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables
  `MYLITE_WITH_STATUS_METADATA`, `MYLITE_WITH_PROCESSLIST_METADATA`, and
  `MYLITE_WITH_ROUTINE_METADATA`.
- `mariadb/sql/sql_show.cc` routes disabled
  `INFORMATION_SCHEMA.GLOBAL_STATUS`, `SESSION_STATUS`, `PROCESSLIST`,
  `ROUTINES`, and `PARAMETERS` tables to an empty schema-table filler.
- `mariadb/sql/sql_parse.cc` reports `ER_NOT_SUPPORTED_YET` for
  `SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` when
  `MYLITE_WITH_PROCESSLIST_METADATA` is disabled.
- Upstream `main.empty_table` is not accepted in the curated list because it
  expects normal status metadata output after `SHOW STATUS LIKE
  'Empty_queries'`; under the MyLite embedded profile that producer is
  intentionally empty.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_metadata`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled metadata behavior. This is raw embedded-profile
evidence only. It does not replace `libmylite` tests for user-facing
unsupported-surface diagnostics.

## Design

- Add `mariadb/mysql-test/suite/mylite/t/profile_disabled_metadata.test` and
  its expected result file.
- Add `mylite.profile_disabled_metadata` to the default curated MTR smoke list
  after the bootstrap-schema check and before the broader disabled-surface
  smoke.
- Keep assertions stable and profile-specific:
  - `SHOW STATUS LIKE 'Empty_queries'` returns no rows;
  - status information-schema tables are empty;
  - `SHOW PROCESSLIST` and `SHOW FULL PROCESSLIST` reject with
    `ER_NOT_SUPPORTED_YET`;
  - process-list information-schema rows are empty;
  - procedure/function status and routine/parameter information-schema rows are
    empty.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile.

## Build, Size, And Dependencies

No production dependency or binary-size change. The new test exercises existing
compile-time profile switches and empty-filler code paths.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_metadata`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `mylite.profile_disabled_metadata`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled metadata behavior.

## Risks And Open Questions

- This is intentionally not a compatibility claim for normal MariaDB metadata
  introspection. MyLite trims these server-oriented producers in the embedded
  profile and covers that product choice explicitly.
- Upstream MTR cases that assume these producers exist should stay out of the
  curated list until a comparison slice can classify the expected profile
  difference.
