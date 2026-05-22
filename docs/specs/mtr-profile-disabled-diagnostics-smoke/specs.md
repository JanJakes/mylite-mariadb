# MTR Profile Disabled Diagnostics Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_diagnostics` case. The test covers diagnostic
producers that the MyLite MTR smoke profile intentionally compiles out of the
embedded server build.

## Non-Goals

- Full diagnostic-surface coverage.
- `libmylite` unsupported-surface preflight coverage.
- Re-enabling static `SHOW`, statement profiling, or optimizer trace runtime.
- Normalizing upstream MTR tests that assume full server diagnostics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables
  `MYLITE_WITH_STATIC_SHOW_INFO`, `MYLITE_WITH_OPTIMIZER_TRACE`, and
  `ENABLED_PROFILING`.
- `mariadb/sql/sql_parse.cc` reports `ER_NOT_SUPPORTED_YET` for
  `SHOW AUTHORS`, `SHOW CONTRIBUTORS`, and `SHOW PRIVILEGES` when static
  `SHOW` information producers are disabled.
- `mariadb/sql/sql_parse.cc` and `mariadb/sql/sql_profile.cc` report
  `ER_FEATURE_DISABLED` for `SHOW PROFILES`, `SHOW PROFILE`, and
  `INFORMATION_SCHEMA.PROFILING` when statement profiling is not built.
- `mariadb/sql/mylite_opt_trace_disabled.cc` keeps the optimizer-trace schema
  shape but returns empty trace rows when optimizer trace runtime is omitted.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_diagnostics`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled diagnostic behavior. This remains embedded profile
evidence and does not claim compatibility for server-oriented diagnostic
features that MyLite intentionally omits.

## Design

- Add `mariadb/mysql-test/suite/mylite/t/profile_disabled_diagnostics.test`
  and its expected result file.
- Add `mylite.profile_disabled_diagnostics` to the default curated MTR smoke
  list near the other MyLite profile tests.
- Keep assertions stable and profile-specific:
  - static `SHOW` information commands reject with `ER_NOT_SUPPORTED_YET`;
  - `@@have_profiling` reports `NO`;
  - `SHOW PROFILES`, `SHOW PROFILE`, and `INFORMATION_SCHEMA.PROFILING` reject
    with `ER_FEATURE_DISABLED`;
  - `INFORMATION_SCHEMA.OPTIMIZER_TRACE` remains empty after enabling the
    optimizer-trace variable and executing a simple query.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile.

## Build, Size, And Dependencies

No production dependency or binary-size change. The new test exercises existing
compile-time profile switches and disabled diagnostic code paths.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_diagnostics`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `mylite.profile_disabled_diagnostics`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled diagnostic behavior.

## Risks And Open Questions

- This is intentionally not a compatibility claim for full MariaDB diagnostics.
  MyLite trims these server-oriented producers in the embedded profile and
  covers that product choice explicitly.
- Upstream MTR cases that assume full profiling or optimizer trace output
  should stay out of the curated list until a comparison slice can classify the
  expected profile difference.
