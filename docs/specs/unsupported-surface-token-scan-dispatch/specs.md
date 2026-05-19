# Unsupported Surface Token Scan Dispatch

## Problem

Every public direct and prepared SQL entry point checks MyLite's unsupported
SQL surface policy before calling MariaDB. The policy currently runs many
independent token scans over the same SQL text, including separate scans for
file import/export markers, disabled function families, sequence value syntax,
user-statistics metadata, and lock markers.

The storage-smoke performance baseline still shows direct SQL execution much
slower than prepared execution after storage hot-path work. MariaDB parse and
execution overhead remains the main cost, but repeated MyLite policy scans are
avoidable front-door work on every direct statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::exec_impl()` and
  `packages/libmylite/src/database.cc::prepare_impl()` both call
  `unsupported_sql_surface_message()` before handing SQL to MariaDB.
- `unsupported_sql_surface_message()` currently calls many helpers that each
  walk the SQL text through `pop_sql_scanned_token()` or identifier scanners.
- The scanner helpers are policy checks, not parsers. They deliberately run
  before MariaDB execution so unsupported server-oriented, file-I/O, disabled
  function, and lock surfaces fail with stable MyLite diagnostics.

## Design

- Add one token-scan summary helper for unsupported token-level surfaces,
  including a single identifier sub-scan for `INFORMATION_SCHEMA` table names
  that may be quoted.
- Preserve the existing diagnostic priority in `unsupported_sql_surface_message()`
  by checking command-specific helpers and the summary flags in the same order
  as today.
- Split command-specific checks from token-wide checks where a helper currently
  does both, such as statement profiling, optimizer trace, file import,
  user-statistics, and locking policy.
- Keep the existing token helper predicates for function-name classification.
  The slice changes dispatch and scan count, not the supported/unsupported
  policy.

## Compatibility Impact

No supported SQL behavior changes. Existing unsupported surfaces must still
fail before MariaDB execution with the same message fragments used by the
current direct/prepared tests.

## Single-File And Lifecycle Impact

No file-format, sidecar, storage, journal, or lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No storage-engine routing change. The optimization runs before SQL reaches
MariaDB and therefore applies equally to direct SQL and prepared statements.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to a small first-party C++
dispatch struct and scanner.

## Test And Verification Plan

- Run embedded statement tests covering unsupported server surfaces, disabled
  function families, file import/export, locking, partition, and FK policy.
- Run storage-engine smoke because routed direct/prepared SQL also passes
  through the policy layer.
- Run the local performance baseline as noisy evidence for direct SQL overhead.
- Run `git diff --check`.

## Acceptance Criteria

- Common SQL text is scanned by one summary helper for token-wide unsupported
  surfaces instead of once per unsupported surface family.
- Existing unsupported-surface tests pass without changing expected messages.
- Routed storage-engine compatibility smoke passes.
- No new dependency, sidecar, or file-format change is introduced.

## Risks

- This is a front-door policy optimization, not a MariaDB execution bypass.
  It can only reduce MyLite preflight overhead; SQLite-like direct SQL speed
  still requires larger prepared-path ergonomics, storage navigation, and pager
  work.
