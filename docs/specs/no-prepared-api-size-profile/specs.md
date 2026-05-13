# No Prepared API Size Profile

## Problem Statement

The current aggressive minsize profile still keeps the public MyLite prepared
statement implementation. That preserves the intended embedded API surface, but
it also roots MariaDB's inherited `MYSQL_STMT` client facade, binary
`COM_STMT_*` dispatch, `Prepared_statement` internals, parameter binding, result
binding, and the MyLite prepared smoke coverage.

For a "how low can it go" size experiment, measure a profile that keeps the
exported `libmylite` prepared-statement symbols but makes them explicit
unsupported stubs. This gives a concrete lower bound for a no-prepared
embedded profile without changing the ABI shape or pretending the feature still
works.

## Source Findings

MariaDB source references are from the imported MariaDB Server tag
`mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `vendor/mariadb/server/mylite/mylite.cc` stores `MYSQL_STMT *`,
  `MYSQL_RES *`, `MYSQL_BIND` arrays, parameter state, result buffers, and
  column metadata in `mylite_stmt`.
- `mylite_prepare()` calls `mysql_stmt_init()`, `mysql_stmt_prepare()`,
  `mysql_stmt_result_metadata()`, and `mysql_stmt_param_count()`.
- `mylite_step()` uses `mysql_stmt_execute()`, `mysql_stmt_store_result()`,
  `mysql_stmt_bind_result()`, `mysql_stmt_fetch()`, and
  `mysql_stmt_fetch_column()` through helper functions.
- `vendor/mariadb/server/libmysqld/libmysql.c` implements the inherited
  `mysql_stmt_*` client facade, including local `MYSQL_STMT` ownership and
  bind/result conversion.
- `vendor/mariadb/server/libmysqld/lib_sql.cc` implements the embedded
  `emb_stmt_execute()` path by dispatching `COM_STMT_EXECUTE`.
- `vendor/mariadb/server/sql/sql_parse.cc` dispatches binary prepared commands
  `COM_STMT_PREPARE`, `COM_STMT_EXECUTE`, `COM_STMT_FETCH`,
  `COM_STMT_SEND_LONG_DATA`, `COM_STMT_CLOSE`, `COM_STMT_RESET`, and
  `COM_STMT_BULK_EXECUTE`.
- `vendor/mariadb/server/sql/sql_prepare.cc` implements the binary
  `mysqld_stmt_*` handlers and `Prepared_statement` internals. The prior
  `sql-prepare-command-size-profile` already removed the SQL-language
  `PREPARE` / `EXECUTE` command entry points from the aggressive profile.

Current post-`sql-prepare-command-size-profile` evidence:

- `mylite-open-close-smoke` still exports `mylite_prepare` and `mylite_step`.
- The linked smoke still contains `mysql_stmt_prepare`,
  `mysql_stmt_execute`, `mysql_stmt_store_result`, `mysql_stmt_bind_param`,
  and `mysql_stmt_bind_result`.
- `libmylite.dir/mylite.cc.o` is 33,381 bytes.
- `libmysql.c.o` is 19,217 bytes and `sql_prepare.cc.o` is 30,365 bytes.

## Scope

This slice may:

- add `MYLITE_DISABLE_PREPARED_STATEMENT_API`,
- enable it in `tools/build-mariadb-minsize.sh`,
- compile the public prepared-statement API functions as explicit unsupported
  stubs in the aggressive profile,
- compile out MyLite-owned prepared statement state, bind helpers, result
  helpers, and `MYSQL_STMT` error mapping when disabled,
- reject binary `COM_STMT_*` dispatch in the embedded server when the prepared
  API is disabled, and
- update smoke coverage to prove the public symbols remain callable and report
  a stable unsupported diagnostic.

## Non-Goals

This slice does not:

- remove the public `mylite_prepare`, `mylite_step`, `mylite_reset`,
  `mylite_finalize`, bind, or column accessor symbols,
- remove ordinary `mylite_exec()` query execution,
- remove MariaDB SQL parsing or ordinary `SELECT`, `INSERT`, `UPDATE`, or
  `DELETE`,
- replace prepared statements with SQL string interpolation, or
- claim this is the preferred default product profile.

## Proposed Design

When `MYLITE_DISABLE_PREPARED_STATEMENT_API` is enabled:

- `mylite_prepare()` validates the database handle and output pointers, sets
  `*out_stmt` to `NULL`, preserves the optional `tail` initialization, and
  returns `MYLITE_ERROR` with SQLSTATE `0A000`, MariaDB errno
  `ER_NOT_SUPPORTED_YET`, and message `prepared statements`;
- `mylite_step()` and bind APIs return `MYLITE_MISUSE` for invalid statement
  handles because no valid statement can be created;
- `mylite_reset()` returns `MYLITE_MISUSE` for invalid statement handles;
- `mylite_finalize(NULL)` remains `MYLITE_OK`;
- column accessors keep their existing null/zero behavior for invalid
  statements;
- MyLite's prepared helper functions and `MYSQL_STMT` fields are excluded; and
- `dispatch_command()` rejects binary `COM_STMT_*` commands with
  `ER_NOT_SUPPORTED_YET` so `sql_prepare.cc` can be dropped when no other
  retained path references it.

The diagnostic is intentionally explicit rather than a parse error or missing
symbol. This keeps the ABI callable while making the feature boundary clear.

## Affected Subsystems

- `libmylite` public prepared-statement implementation.
- Embedded binary prepared command dispatch in `sql_parse.cc`.
- Aggressive minsize build configuration.
- `mylite-open-close-smoke` prepared-statement coverage.
- Production size analysis.

## DDL Metadata Routing Impact

No DDL metadata routing should change. Ordinary DDL still runs through
`mylite_exec()` and MariaDB SQL execution. Prepared DDL is unavailable only
because all prepared statements are unavailable in this experimental profile.

## Single-File And Embedded-Lifecycle Impact

No file ownership, locking, catalog, recovery, or runtime bootstrap behavior
changes. The profile removes a statement-lifecycle surface; it must not change
open/close behavior, read-only mode, sidecar checks, or storage-engine state.

## Public API Or File-Format Impact

The public ABI symbols remain present, but prepared statements become
unsupported at runtime in the aggressive minsize profile. This is a major API
compatibility loss for applications that need reusable parameterized execution,
including likely PDO-style integrations.

There is no `.mylite` file-format change.

## Binary-Size Impact

Expected savings should be materially larger than the SQL-language PREPARE
command cut because this profile can drop MyLite's prepared implementation and
the binary `COM_STMT_*` server roots. The current object upper bound is roughly
0.08 MiB across `mylite.cc.o`, `libmysql.c.o`, and `sql_prepare.cc.o`, but the
linked stripped result is the deciding measurement.

Implemented measurements against the preceding
`sql-prepare-command-size-profile` baseline:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| `libmysqld/libmariadbd.a` | 25,493,040 | 25,489,666 | -3,374 |
| `mylite/libmylite.a` | 122,792 | 76,130 | -46,662 |
| `storage/mylite/libmylite_embedded.a` | 388,456 | 388,456 | 0 |
| unstripped `mylite-open-close-smoke` | 6,582,456 | 6,503,344 | -79,112 |
| stripped `mylite-open-close-smoke` | 4,626,880 | 4,566,544 | -60,336 |

`llvm-size` total for the linked open-close smoke changed from 4,848,276 to
4,790,812 bytes (-57,464). `mylite.cc.o` changed from 33,381 to 19,738 bytes
(-13,643), and `lib_sql.cc.o` changed from 94,088 to 93,144 bytes (-944).
`sql_prepare.cc.o` remains 30,365 bytes because it also provides shared
`Item_param`, reprepare, and bulk-parameter helpers that ordinary SQL/type code
still references. The linked smoke retains `mylite_prepare` and `mylite_step`
but no longer contains `mysql_stmt_*`, `mysqld_stmt_*`,
`Prepared_statement::prepare`, `emb_stmt_execute`, or
`emb_read_prepare_result`.

## License, Trademark, And Dependency Impact

No new dependency or license impact. This is a GPL-2.0-only MariaDB-derived
build-profile experiment.

## Test And Verification Plan

Run:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
bash -n tools/build-mariadb-minsize.sh tools/run-libmylite-open-close-smoke.sh \
  tools/run-storage-engine-smoke.sh tools/run-embedded-bootstrap-smoke.sh \
  tools/run-compatibility-test-harness.sh
git diff --check
```

Additional checks:

- `mylite_prepare()` reports explicit unsupported behavior with `0A000`,
  `ER_NOT_SUPPORTED_YET`, and `prepared statements`;
- `mylite_finalize(NULL)` still returns `MYLITE_OK`;
- open/close, `mylite_exec()`, read-only mode, storage-engine, and
  compatibility harness groups remain green; and
- linked symbol checks confirm `mysql_stmt_*`, `mysqld_stmt_*`, and
  `Prepared_statement::prepare` are absent while `mylite_prepare` remains.

## Acceptance Criteria

- Passed: the minsize build succeeds with
  `MYLITE_DISABLE_PREPARED_STATEMENT_API=ON`.
- Passed: public prepared API symbols remain linkable and fail explicitly.
- Passed: ordinary query execution and current storage/catalog smokes still
  pass.
- Passed: binary size results are recorded here and in
  `docs/research/production-size-analysis.md`.

## Verification

Passed:

```sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/build-mariadb-minsize.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-libmylite-open-close-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-storage-engine-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-embedded-bootstrap-smoke.sh
MYLITE_MARIADB_BUILD_DIR=build/mariadb-minsize-no-prepared-api \
  MYLITE_BUILD_JOBS=8 tools/run-compatibility-test-harness.sh
```

The open-close reports include:

```text
prepared_unsupported_message=This version of MariaDB doesn't yet support 'prepared statements'
readonly_prepare_message=This version of MariaDB doesn't yet support 'prepared statements'
```

Linked symbol checks confirm that `mylite_prepare` and `mylite_step` remain,
while the linked open-close smoke contains no `mysql_stmt_*`,
`mysqld_stmt_*`, `Prepared_statement::prepare`, `emb_stmt_execute`, or
`emb_read_prepare_result` symbols.

An attempted follow-up removal of `sql_prepare.cc.o` from the static archive
was discarded: the link still needs shared `Reprepare_observer`, `Item_param`,
and bulk-parameter definitions from that object for ordinary SQL/type code.

## Risks And Unresolved Questions

- This is a major product compatibility loss and probably not suitable for the
  default profile if MyLite needs PDO-like prepared statements.
- Some inherited MariaDB code may still reference `COM_STMT_*` helpers through
  status flags, virtual methods, or parser state even after dispatch is
  rejected.
- If the archive win is mostly `libmylite.a` and not the final linked runtime,
  this experiment may be useful only as a lower-bound data point.
