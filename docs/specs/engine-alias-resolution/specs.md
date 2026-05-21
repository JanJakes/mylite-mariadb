# Engine Alias Resolution

## Problem

MyLite already routes supported table engines to the MyLite handler in normal
`libmylite` sessions, but part of that behavior currently depends on session
defaults such as `default_storage_engine=MYLITE`,
`enforce_storage_engine=MYLITE`, and an empty `sql_mode`.

That is too indirect for a MySQL/MariaDB drop-in. Application DDL such as
`ENGINE=InnoDB` or `ENGINE=MyISAM` should resolve to the MyLite handler when a
MyLite primary file is active, including when `NO_ENGINE_SUBSTITUTION` is set
and native durable engines are absent from the embedded profile.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/handler.cc` resolves engine names through
  `ha_resolve_by_name()`. It first handles `DEFAULT`, then locks a plugin by
  name, then checks upstream historical aliases such as `INNOBASE` to `INNODB`,
  `HEAP` to `MEMORY`, and `Maria` to `Aria`.
- `Storage_engine_name::resolve_storage_engine_with_error()` reports
  `ER_UNKNOWN_STORAGE_ENGINE` for unresolved engines when the statement is not
  substitutable or `NO_ENGINE_SUBSTITUTION` is active.
- `mariadb/storage/mylite/ha_mylite.cc` registers schema hooks only when the
  MyLite storage plugin is loaded. `mylite_schema_hooks_active()` is true only
  while `--mylite-primary-file` is set, so it is the existing SQL-layer signal
  that the current embedded server is operating on a MyLite primary file.

## Design

Add a narrow MyLite alias path to `ha_resolve_by_name()`:

- after `DEFAULT` handling and before normal plugin lookup, check whether
  MyLite schema hooks are active;
- for supported application engine names, lock and return the `MYLITE` storage
  plugin instead of resolving native engines;
- preserve MariaDB's normal resolver for all other names and for server starts
  without an active MyLite primary file.

The alias list is intentionally limited to engines MyLite already documents as
supported routing targets:

- `InnoDB` and the upstream `INNOBASE` alias,
- `MyISAM`,
- `Aria` and the upstream `Maria` alias,
- `BLACKHOLE`,
- `MEMORY` and `HEAP`.

`MRG_MyISAM`, `CSV`, `ARCHIVE`, `SEQUENCE`, and other external or server-owned
engines remain unsupported.

The MyLite handler still records the requested engine token from the parsed SQL
command, so catalog metadata and `SHOW CREATE TABLE` continue to expose the
requested engine while the effective handler is `MYLITE`.

## Compatibility Impact

This improves drop-in behavior for applications that keep
`NO_ENGINE_SUBSTITUTION` enabled or expect native engine names to exist. It does
not claim native engine internals; the routed tables use MyLite storage
semantics already documented in `docs/COMPATIBILITY.md`.

## Single-File And Lifecycle Impact

The change does not add file formats or companion files. It reduces the chance
that MariaDB tries to open native durable engine files for supported routed
engine names while a MyLite primary file is active.

## Binary Size And Fork Hygiene

The patch is a small SQL-layer resolver hook. It reuses the existing
`mylite_schema_hook` boundary and does not import native engine code or add new
dependencies.

## Tests

Add storage-engine smoke coverage that:

- enables `NO_ENGINE_SUBSTITUTION`;
- creates direct and prepared routed tables with supported legacy engine names;
- verifies requested/effective catalog metadata;
- verifies `SHOW CREATE TABLE` preserves requested names where relevant;
- verifies row behavior for durable, row-discarding, and volatile routed
  engines;
- proves unsupported engine requests still fail before catalog publication.

Run at least:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

## Acceptance Criteria

- `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, `ENGINE=BLACKHOLE`,
  `ENGINE=MEMORY`, and `ENGINE=HEAP` resolve to MyLite when a MyLite primary
  file is active, even under `NO_ENGINE_SUBSTITUTION`.
- Unsupported engine requests still fail without catalog publication.
- Docs and compatibility tables state that major supported engine names are
  resolved directly to MyLite in active file-backed sessions.
