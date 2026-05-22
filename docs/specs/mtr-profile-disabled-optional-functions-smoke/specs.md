# MTR Profile Disabled Optional Functions Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_optional_functions` case. The test covers optional SQL
function families that the MyLite MTR smoke profile intentionally compiles out
of the embedded server build.

## Non-Goals

- Full optional-function coverage.
- `libmylite` user-facing unsupported-function diagnostics.
- Re-enabling server utility, XML, JSON schema, SFORMAT, or vector function
  runtimes.
- Dynamic-column function coverage; those functions remain registered but route
  through disabled packed-BLOB helpers and need separate classification.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables
  `MYLITE_WITH_SERVER_UTILITY_FUNCTIONS`,
  `MYLITE_WITH_XML_SQL_FUNCTIONS`, `MYLITE_WITH_VECTOR_SQL_RUNTIME`,
  `MYLITE_WITH_SFORMAT_SQL_FUNCTION`, and
  `MYLITE_WITH_JSON_SCHEMA_VALID`.
- `mariadb/sql/CMakeLists.txt` turns those switches into preprocessor symbols
  for SQL function registration.
- `mariadb/sql/item_create.cc` omits registry entries for `BENCHMARK()`,
  `SLEEP()`, `UUID_SHORT()`, `EXTRACTVALUE()`, `UPDATEXML()`,
  `JSON_SCHEMA_VALID()`, `SFORMAT()`, and vector functions when the associated
  profile switches are off.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_optional_functions`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled optional SQL function absence. This is raw
embedded-profile evidence. The public `libmylite` tests remain responsible for
clearer unsupported-surface diagnostics before MariaDB execution.

## Design

- Add
  `mariadb/mysql-test/suite/mylite/t/profile_disabled_optional_functions.test`
  and its expected result file.
- Add `mylite.profile_disabled_optional_functions` to the default curated MTR
  smoke list near the other MyLite profile tests.
- Assert missing-function diagnostics for representative disabled families:
  - server utility functions: `BENCHMARK()`, `SLEEP()`, `UUID_SHORT()`;
  - XML functions: `EXTRACTVALUE()`, `UPDATEXML()`;
  - JSON schema validation: `JSON_SCHEMA_VALID()`;
  - formatting extension: `SFORMAT()`;
  - vector runtime: `VEC_TOTEXT()` / `VEC_FROMTEXT()`.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile.

## Build, Size, And Dependencies

No production dependency or binary-size change. The new test exercises existing
compile-time function registry switches.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_optional_functions`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes
  `mylite.profile_disabled_optional_functions`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled optional SQL function absence.

## Risks And Open Questions

- This is intentionally not a compatibility claim for full optional SQL
  function support. MyLite keeps these server-oriented or heavyweight function
  families out of the embedded profile.
- Dynamic-column functions need a separate slice because their SQL names remain
  registered while their packed-BLOB runtime is disabled.
