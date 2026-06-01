# Ownerless External Trace Runner

## Problem

The ownerless stress traces now cover each deterministic stress shape as
portable SQL files, but Phase 12 still needs a concrete path from those files
to external MariaDB/RQG-style execution. A full external MariaDB server farm or
RQG integration is larger than one correctness slice. The next bounded step is
a dependency-free replay harness that can run any generated trace directory
against a user-supplied MariaDB-compatible command-line client while CI keeps a
serverless fake-client replay smoke test.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/client/mysql.cc:1214` initializes batch input readers for SQL files
  and stdin.
- `mariadb/client/mysql.cc:1296` starts the command-line client entry point,
  and `mariadb/client/mysql.cc:2276` defines `read_and_execute()` for batch
  query execution.
- `mariadb/client/mysql.cc:5403-5425` formats batch errors as `ERROR <errno>`
  and returns failure when errors are not ignored.
- Existing trace exporters define the repository trace package contract:
  `schema.sql`, concurrent worker/reader SQL, optional `post.sql`, optional
  `negative-worker-*.sql` expected-error probes, `expected.sql`, and
  `manifest.txt`.
- `tools/ownerless-fk-graph-trace` emits `negative-worker-*.sql` files with
  `-- expected_errno=<errno>` comments before single-statement probes; the
  runner can validate those probes without requiring RQG-specific machinery.

## Scope And Non-Goals

In scope:

- Add `tools/ownerless-sql-trace-runner`, an opt-in shell harness for replaying
  a generated ownerless SQL trace directory through `mysql`, `mariadb`, or a
  compatible client selected by `--client`.
- Support repeated `--client-arg` flags for socket, host, port, user, password,
  TLS, or other site-specific client settings without hard-coding credentials.
- Run `schema.sql`, execute worker and reader files concurrently, run
  `post.sql` when present, execute `negative-worker-*.sql` expected-error
  probes by checking client stderr for the requested errno, then run
  `expected.sql`.
- Fail the replay when any SQL process fails unexpectedly or any captured
  output contains the generated oracle value `mismatch`.
- Add a dependency-free CTest smoke check that validates trace-plan logic,
  concurrent replay orchestration, oracle scanning, and expected-error handling
  through a fake SQL client without requiring an external MariaDB server.
- Update compatibility and ownerless-concurrency docs to mark external trace
  replay harnessing as covered while long-running external execution remains
  opt-in outside CI.

Out of scope:

- Starting, configuring, or destroying an external MariaDB server.
- Running RQG, SQLancer, or long-lived external stress in CI.
- Owning credentials, sockets, ports, TLS material, or test database lifecycle
  outside the generated SQL trace.
- Making a failed `SELECT 'mismatch'` oracle raise a SQL error in the generated
  trace files themselves.

## Design

`tools/ownerless-sql-trace-runner` accepts:

- `--trace-dir DIR` to identify a generated trace package,
- `--client PATH` to choose the SQL client executable, defaulting to `mysql`,
- repeated `--client-arg ARG` values passed verbatim to the client,
- `--log-dir DIR` for captured stdout/stderr,
- `--check` or `--dry-run` to validate and print the execution plan without a
  client,
- `--skip-negative` to leave `negative-worker-*.sql` files for a different
  expected-error harness,
- `--self-test` for CTest with a generated fake SQL client.

The runner validates that `schema.sql`, `expected.sql`, `manifest.txt`, and at
least one concurrent worker or reader SQL file are non-empty. It recognizes
`ddl-worker-*.sql`, `dml-worker-*.sql`, `worker-*.sql`, and `reader.sql` as the
concurrent group. It recognizes `post.sql` as a post-worker step for temporary
table traces. It recognizes `negative-worker-*.sql` as expected-error probes.

For real execution, the runner starts by running `schema.sql` in one client
process. It then starts every concurrent SQL file in a separate client process
and waits for all to finish. It runs `post.sql` if present, then expected-error
probe statements, then `expected.sql`. It writes one stdout and stderr log per
SQL file or expected-error statement. After the final oracle, it scans captured
stdout for `mismatch` so trace-provided `SELECT CASE` oracles fail the replay
even when the SQL client exits zero.

Expected-error probe support is intentionally narrow: the generated FK graph
negative files contain `-- expected_errno=<errno>` followed by a single SQL
statement. The runner replays each probe as its own client call, expects a
non-zero exit, and checks stderr for the MariaDB batch error prefix
`ERROR <errno>`.

## Compatibility Impact

No MyLite SQL behavior, public API, storage format, or runtime behavior
changes. The slice turns previously exported trace files into executable
external-harness inputs, but actual external MariaDB/RQG runs remain opt-in and
environment-owned.

## Database Directory And Lifecycle Impact

No MyLite database directory layout changes. External replay targets a separate
MariaDB-compatible server selected by the caller's client arguments.

## Native Storage Impact

No native storage runtime changes. When used against MariaDB or a compatible
target, the runner executes the generated InnoDB stress traces and final
oracles in the target server's own data directory.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds one shell tool, CTest smoke
coverage, and documentation.

## Test Plan

- Run `tools/ownerless-sql-trace-runner --self-test`.
- Generate a small independent-table trace and run
  `tools/ownerless-sql-trace-runner --trace-dir <tmp> --check`.
- Run `ctest --preset embedded-dev -R tools.ownerless-sql-trace-runner
  --output-on-failure`.
- Run all ownerless trace-tool CTests to prove the new runner test coexists
  with existing exporters.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The runner validates and self-tests a trace package without requiring an
  external server.
- The execution plan includes schema, concurrent files, optional post files,
  optional negative files, expected SQL, and manifest paths.
- Real execution support can run SQL files concurrently through a user-supplied
  client and fail on process errors, expected-error mismatches, or trace oracle
  output containing `mismatch`.
- Documentation and compatibility notes mark trace replay harnessing as covered
  while full external MariaDB/RQG long-running execution remains outside CI.

## Risks And Follow-Up

- The runner depends on MariaDB/MySQL command-line client error formatting for
  expected-error probes; other compatible clients may require `--skip-negative`
  or a later parser extension.
- The runner does not provision external servers, isolate database names, or
  run RQG/SQLancer. Those remain environment-owned follow-up work.
