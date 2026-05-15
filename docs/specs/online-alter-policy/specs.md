# Online ALTER Policy

## Goal

Reject online and in-place `ALTER TABLE` forms explicitly through the MyLite SQL
policy layer until MyLite has online DDL, transaction-aware index maintenance,
and lock integration. Supported `ALTER TABLE` remains the copy-rebuild path.

## Non-Goals

- Implementing `ALGORITHM=INPLACE`, `ALGORITHM=INSTANT`, `ALGORITHM=NOCOPY`,
  or online copy ALTER.
- Implementing `LOCK=NONE` or weaker-than-exclusive ALTER semantics.
- Changing supported `ALGORITHM=COPY` rebuild behavior.
- Full SQL transaction, savepoint, or online DDL rollback semantics.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:95-110` defines storage-engine return values for
  in-place ALTER support checks.
- `mariadb/sql/handler.h:389` defines `HA_NO_ONLINE_ALTER` for engines that
  are not compatible with online ALTER TABLE.
- `mariadb/storage/mylite/ha_mylite.h:88-95` includes
  `HA_NO_ONLINE_ALTER` in `ha_mylite::table_flags()`.
- `mariadb/sql/sql_table.cc:10576-10590` checks `HA_NO_ONLINE_ALTER` when
  deciding whether `LOCK=NONE` can be honored.
- `mariadb/sql/sql_table.cc:11446-11458` forces the copy algorithm when
  in-place ALTER is impossible and reports `ALGORITHM=COPY` as the supported
  alternative for no-copy requests.
- `mariadb/sql/sql_table.cc:11589-11728` enters the in-place ALTER flow only
  when the requested/default algorithm is not copy and the handler reports
  support.
- `mariadb/sql/sql_table.cc:11845-11854` rejects `LOCK=NONE` when online ALTER
  is unavailable and suggests `LOCK=SHARED`.

## Compatibility Impact

`ALTER TABLE` remains partial. MyLite will explicitly reject representative
online/in-place ALTER syntax before MariaDB execution instead of relying on
handler fallback errors. This keeps the compatibility claim narrow: copy ALTER
is supported for covered table shapes; online and in-place DDL are unsupported.

## Design

Add a narrow SQL-policy detector for `ALTER TABLE` statements that request:

- `ALTER ONLINE TABLE ...`;
- `ALGORITHM=INPLACE`;
- `ALGORITHM=INSTANT`;
- `ALGORITHM=NOCOPY`;
- `LOCK=NONE`.

The detector should use the existing token scanner so quoted strings and
comments do not create false positives. It should not reject supported
`ALGORITHM=COPY`, `LOCK=EXCLUSIVE`, or ordinary column names that merely
contain these words in quotes. It should still scan algorithm and lock markers
when `ALTER TABLE` includes the optional `IGNORE` or `OFFLINE` modifiers.

The rejection message is a stable MyLite error with no MariaDB errno, matching
the existing unsupported partition, foreign-key, locking, and non-table object
policies.

## File Lifecycle

Rejected online/in-place ALTER statements must not reach MariaDB table
preparation, must not create temporary `.frm` files, must not publish MyLite
catalog records, and must not introduce forbidden durable sidecars.

## Embedded Lifecycle And API

No public API changes. Both direct and prepared execution use the same
unsupported SQL policy before MariaDB execution.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact is limited to one
small SQL scanner path and tests.

## Test Plan

- Extend embedded direct-execution policy coverage for:
  - `ALTER ONLINE TABLE ...`;
  - `ALTER TABLE ... ALGORITHM=INPLACE`;
  - `ALTER TABLE ... ALGORITHM=INSTANT`;
  - `ALTER TABLE ... ALGORITHM=NOCOPY`;
  - `ALTER OFFLINE TABLE ... ALGORITHM=INPLACE`;
  - `ALTER TABLE ... LOCK=NONE`.
- Extend prepared-statement diagnostics coverage for an in-place ALTER request.
- Extend storage-engine smoke coverage to verify representative rejected
  online/in-place ALTER statements leave routed catalog metadata unchanged.
- Preserve an existing successful `ALGORITHM=COPY` path.
- Run focused embedded/storage-smoke tests, routed DDL/DML and sidecar harness
  reports, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Representative online/in-place ALTER SQL returns a stable MyLite unsupported
  online ALTER error without MariaDB errno.
- Supported copy ALTER still succeeds.
- Routed catalog counts and metadata are unchanged after rejected online ALTER
  statements.
- Compatibility, roadmap, storage architecture, copy-ALTER spec, and harness
  descriptions identify online/in-place ALTER as explicitly rejected while
  broader implementation remains planned.

## Risks And Open Questions

- MariaDB has many ALTER syntactic variants. This slice covers representative
  top-level online/in-place markers and keeps deeper online DDL behavior
  planned.
- `LOCK=SHARED` may still be rejected by MariaDB for some MyLite paths; this
  slice only makes `LOCK=NONE` explicit because it implies online writer
  concurrency.
