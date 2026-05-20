# Native Storage Baseline

## Goal

Configure MariaDB's native storage paths under one MyLite database directory and
prove controlled DDL/DML state survives close and reopen without a server
process. New database directories should conventionally be named
`<name>.mylite/`, but the suffix is a best practice rather than an enforced
validity rule.

## Non-Goals

- Do not implement a custom storage engine.
- Do not enable or claim InnoDB lifecycle support yet.
- Do not claim broad DDL, transaction, crash recovery, locking, or concurrency
  compatibility.
- Do not implement read-only opens.
- Do not support multiple active database directories in one process.
- Do not treat `temp_directory` as an override for durable database paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6` / `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/libmysqld/lib_sql.cc:532` implements embedded startup; lines
  597-598 bind `mysql_data_home` to `mysql_real_data_home`.
- `mariadb/sql/sys_vars.cc:1066-1067` defines the `datadir` command-line
  variable.
- `mariadb/sql/sys_vars.cc:3110-3112` defines `plugin_dir`.
- `mariadb/sql/sys_vars.cc:3345-3357` defines `tmpdir`.
- `mariadb/sql/sys_vars.cc:4677-4680` defines `default_storage_engine`.
- `mariadb/storage/maria/ha_maria.cc:168` defines `aria-log-dir-path`; lines
  3958-3959 initialize Aria transaction logs from `maria_data_root`.
- `mariadb/sql/sql_table.cc:587` builds schema and table filenames under
  MariaDB's data home.
- `mariadb/sql/sql_db.cc:748-818` creates schema directories and `.db.opt`
  metadata through MariaDB's native database-create path.

## Design

For durable paths, `mylite_open()` establishes this layout before starting
MariaDB:

```text
app.mylite/
  mylite.meta
  datadir/
  tmp/
  run/
    plugins/
```

The embedded runtime starts with MyLite-owned arguments:

- `--no-defaults`
- `--datadir=<db>/datadir`
- `--tmpdir=<db>/tmp`
- `--plugin-dir=<db>/run/plugins`
- `--aria-log-dir-path=<db>/datadir`
- `--skip-grant-tables`
- `--skip-networking`
- `--default-storage-engine=MyISAM`
- `--innodb=OFF`
- explicit message and character-set directories from the MariaDB build/source

`mylite.meta` is a MyLite lifecycle marker. The first version records the layout
format and MariaDB base tag; future directory-version policy can expand it
without changing the public open path.

Clean shutdown removes `run/` and clears contents under `tmp/`. Durable MariaDB
metadata, MyISAM files, Aria control/log state, and other native-storage state
remain under `datadir/`.

`--default-storage-engine=MyISAM` is a temporary baseline choice. It provides a
small native engine surface for DDL/DML persistence tests while avoiding InnoDB
sidecars until InnoDB's tablespace, redo, undo, and recovery lifecycle is
designed and tested inside the MyLite directory.

`:memory:` remains on the bootstrap temporary-runtime path. That special path
is not durable and is outside this slice's database-directory guarantee.

## Compatibility Impact

This slice turns the database directory from a placeholder into MariaDB's native
storage root for the embedded profile. It proves containment and reopen
persistence for controlled MyISAM tables, but it does not make broad SQL or
engine claims.

The `.mylite/` suffix is documentation and test coverage only. Existing callers
that pass another directory name still open successfully when the path and flags
are otherwise valid.

## Test Plan

1. Extend open/close tests to assert `mylite.meta`, `datadir/`, `tmp/`, and
   `run/plugins/` while open.
2. Assert clean close removes `run/`, keeps `datadir/` and `tmp/`, and leaves
   external `temp_directory` empty for durable database paths.
3. Add coverage proving a directory without the `.mylite` suffix still opens.
4. Add a native-storage smoke test that creates a schema, creates an
   `ENGINE=MyISAM` table, inserts a row, closes, verifies `.MYD` and `.MYI`
   files under `datadir/`, reopens without `MYLITE_OPEN_CREATE`, and reads the
   row back.
5. Keep existing `:memory:` cleanup coverage on the temporary bootstrap path.

## Acceptance Criteria

- Durable database paths use MariaDB native storage under the MyLite database
  directory.
- No durable DDL/DML smoke-test state is written to the configured external
  temporary directory.
- `mylite.meta`, `datadir/`, and `tmp/` survive clean close.
- `run/` is removed on clean close.
- Controlled MyISAM DDL/DML persists across close and reopen.
- Documentation and compatibility tables describe the `.mylite/` naming
  convention and the remaining engine/DDL limits.

## Risks And Open Questions

- InnoDB remains disabled and needs its own lifecycle slice before InnoDB
  compatibility can be claimed.
- MyISAM is non-transactional; persistence in this slice is not transaction or
  crash-recovery evidence.
- Cross-process and multi-writer behavior remain unsupported until locking and
  recovery tests exist.
- `tmp/` cleanup currently covers clean shutdown only; crash leftovers need a
  later recovery/lifecycle policy.
