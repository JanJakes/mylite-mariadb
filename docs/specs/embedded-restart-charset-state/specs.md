# Embedded Restart Charset State

## Problem

MyLite repeatedly starts and stops MariaDB's embedded runtime inside one process.
MariaDB's charset registry is mostly process-global state, and UCA collations
such as `utf8mb4_unicode_ci` mutate compiled `CHARSET_INFO` definitions during
initialization. Embedded shutdown frees the once-allocated charset data, but the
compiled charset structs can still look ready on the next startup.

The observed failure was a storage-smoke crash after reopening a file-backed
database and using a `utf8mb4_unicode_ci` secondary index. The comparator reached
the UCA collation handler with `MY_CS_READY` still set and `cs->uca == NULL`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysys/charset.c` keeps compiled collations in static
  `CHARSET_INFO` objects, registers them through `add_compiled_collation()` and
  `add_compiled_extra_collation()`, and stores them in the process-global
  `all_charsets` registry.
- `mariadb/mysys/charset.c` `get_internal_charset()` returns a collation
  immediately when `MY_CS_READY` is already set.
- `mariadb/mysys/charset.c` `free_charsets()` resets the pthread-once guard and
  frees the charset-name hash, but upstream does not restore mutated compiled
  charset structs.
- `mariadb/mysys/my_init.c` `my_end()` calls `free_charsets()` before
  `my_once_free()`, so UCA objects allocated from the once allocator cannot be
  reused after embedded shutdown.
- `mariadb/libmysqld/libmysql.c` `mysql_server_end()` calls `my_end()` when the
  embedded client initialized the MariaDB runtime.
- `mariadb/strings/ctype-uca.c` defines `my_charset_utf8mb4_unicode_ci` with a
  `NULL` `uca` pointer and initializes it through
  `my_uca_coll_init_utf8mb4()`.
- `mariadb/strings/ctype-uca.inl` UCA comparison handlers dereference
  `cs->uca->level[...]`, so stale `MY_CS_READY` state turns into a crash rather
  than a recoverable lookup failure.

## Design

Snapshot each compiled charset definition the first time MariaDB registers it,
then restore those snapshots from `free_charsets()` before the next embedded
startup can reuse the compiled globals. The reset deliberately happens in
`mysys/charset.c`, beside the registry and initialization lifecycle it repairs.

This keeps the fork delta small:

- no MyLite public API change;
- no storage file-format change;
- no new dependency;
- no attempt to special-case `utf8mb4_unicode_ci` only.

The storage-engine smoke also gets a focused regression test that creates a
`utf8mb4_unicode_ci` database and indexed table, closes the runtime, reopens the
same `.mylite` file multiple times, checks duplicate-key behavior, and performs
an indexed lookup through the UCA comparator.

## Affected Subsystems

- MariaDB `mysys` charset registry initialization and teardown.
- MariaDB embedded runtime restart through `mysql_server_init()` /
  `mysql_server_end()`.
- MyLite storage-engine smoke coverage for indexed string keys.

## Compatibility Impact

`utf8mb4_unicode_ci` is a common MySQL/MariaDB collation and is representative of
application schemas such as WordPress. MyLite now has storage-smoke evidence that
this collation can survive same-process embedded restart and remain usable for
supported secondary and unique indexes. This is not a full collation matrix.

## Single-File And Embedded-Lifecycle Impact

The fix only resets process memory during embedded shutdown. It does not add
durable files, sidecars, catalog records, or runtime directories. The regression
test keeps the existing sidecar gate after each close.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

The storage engine already receives MariaDB key images and uses MariaDB collation
handlers for key comparison. The slice makes those handlers restart-safe for the
covered UCA collation; it does not change routed table metadata or requested
engine handling.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Expected impact is negligible: one static snapshot array and two small helper
functions in `mysys/charset.c`. The size report must still be refreshed after
the final build.

## License Or Dependency Impact

None. The change is a local MariaDB-derived source patch under the existing
GPL-2.0-only project license.

## Test And Verification Plan

- Extend the storage-engine smoke with a `utf8mb4_unicode_ci` restart regression.
- Keep the expanded WordPress-shaped schema on `utf8mb4_unicode_ci` database and
  table defaults, with information-schema collation checks before and after
  reopen.
- Run the focused storage-smoke test.
- Run format, tidy, dev, embedded, storage-smoke, compatibility harness, diff,
  and size-report checks before commit.

## Acceptance Criteria

- Repeated same-process embedded restarts do not leave compiled UCA collations in
  a stale ready state.
- The focused `utf8mb4_unicode_ci` indexed lookup and duplicate-key regression
  passes after several close/reopen cycles.
- WordPress-shaped storage-smoke tables retain `utf8mb4_unicode_ci` metadata
  before and after reopen.
- Docs record the charset restart patch and compatibility scope.

## Risks

- Other MariaDB process-global subsystems may still have embedded restart state
  that only appears under broader application-schema coverage.
- The smoke covers one representative UCA collation. A broader charset and
  collation matrix remains future compatibility work.
