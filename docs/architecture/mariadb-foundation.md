# MariaDB Foundation

MyLite is a MariaDB-derived embedded database. MariaDB remains the authority for
SQL syntax, type semantics, diagnostics, metadata behavior, optimizer behavior,
and handler integration.

## Base Line

The initial source base is MariaDB 11.8 LTS.

| Item | Value |
| --- | --- |
| Upstream repository | <https://github.com/MariaDB/server> |
| Branch inspected | `11.8` |
| Branch head inspected | `04e09010773caf0b302b2933fff3fe95381a5e13` |
| Import tag | `mariadb-11.8.6` |
| Import commit | `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7` |
| Tag recheck | 2026-05-14 |

`mariadb-11.8.6` is a stable MariaDB 11.8 release. The import should pin the
tag commit, not the floating `11.8` branch.

## Embedded Runtime

MariaDB already has embedded server support through `libmysqld`:

- `WITH_EMBEDDED_SERVER` adds the embedded server build.
- `libmysqld` exposes the same broad C API shape as the MariaDB client library.
- Applications initialize and shut down the embedded runtime with
  `mysql_library_init()` and `mysql_library_end()`.
- MariaDB's public embedded interface is still server-shaped: option loading,
  datadir state, temporary directories, system variables, time zones, ACL/grant
  paths, DDL recovery, and other server facilities are initialized.

MyLite uses this embedded foundation but does not expose `mysql_library_init()`
as the product API. `libmylite` owns directory-oriented open/close,
diagnostics, and configuration.

The first MyLite bootstrap keeps MariaDB's server-shaped state inside a
MyLite-owned temporary runtime directory. That runtime starts with
`--no-defaults`, `--skip-grant-tables`, `--skip-networking`,
`--default-storage-engine=MyISAM`, and `--innodb=OFF`. Disabling InnoDB here is
a temporary bootstrap limitation, not a compatibility decision for application
DDL; it only avoids uncontrolled InnoDB files before the native-storage
directory lifecycle is configured and tested.

MariaDB 11.8.6 needed two narrow embedded-restart fixes for repeated
`mylite_open()` / `mylite_close()` tests in one process:

- `mariadb/sql/mysqld.cc` restores the embedded scheduler pointers after
  `mysql_server_end()` cleanup.
- `mariadb/sql/sql_locale.cc` keeps the active error-message table alive until
  the next embedded initialization releases it through `init_errmessage()`.

## Metadata And Native Storage

MariaDB 11.8 still has `.frm` table-definition paths and native engine file
layouts. In the MyLite directory model, those files are acceptable durable state
when they stay inside the MyLite database directory and are covered by lifecycle
tests.

MariaDB also exposes storage-engine table discovery:

- `discover_table()` can initialize a `TABLE_SHARE` from engine metadata.
- `discover_table_names()` lists engine-owned tables.
- `discover_table_existence()` checks for a table without opening a datadir
  definition.
- MariaDB discovery supports `TABLE_SHARE::init_from_binary_frm_image()` and
  `TABLE_SHARE::init_from_sql_statement_string()`.
- `tabledef_version` and `HA_ERR_TABLE_DEF_CHANGED` let the engine force
  rediscovery when catalog metadata changes.

Those hooks are useful when a supported native engine provides discovery, but
they are no longer the primary MyLite metadata plan. The primary plan is to use
MariaDB's native metadata and engine files inside the MyLite database directory,
then test that DDL does not write durable state outside that directory.

## Native Engine Surface

The core storage integration is MariaDB's existing native handler surface:

- open, create, drop, rename, and truncate,
- table scans and index scans,
- insert, update, and delete,
- autoincrement state,
- table and row locks,
- transaction hooks,
- discovery hooks,
- metadata version checks,
- enough `ALTER TABLE` behavior to support MariaDB DDL.

MyLite should avoid replacing that handler surface with a custom engine unless a
future slice proves native storage cannot meet a required compatibility goal.
The main MyLite boundary is configuration, directory ownership, and lifecycle
validation around those engines.

## Existing Engines

Existing MariaDB engines are the storage foundation:

- InnoDB normally uses tablespaces plus redo/undo infrastructure.
- MyISAM uses table files such as `.MYD` and `.MYI`.
- Aria uses `.MAI`, `.MAD`, `aria_log.*`, and `aria_log_control` state.
- MariaDB system-table SQL commonly uses Aria.

MyLite may support these engines when their durable files stay inside the MyLite
database directory. Engines or plugins that need durable state outside that
directory are outside the default embedded profile.

## Licensing

MariaDB Server is GPL-2.0-only. MyLite remains GPL-2.0-only while it contains
MariaDB-derived server code. SQLite-like file ownership or API style does not
change the license.

Public packaging must also avoid implying MariaDB or MySQL affiliation.

## Source References

Source anchors at import commit `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`:

- `CMakeLists.txt`: `WITH_EMBEDDED_SERVER` embedded server build option.
- `include/mysql.h`: `mysql_library_init` / `mysql_library_end` public macros
  and broad `MYSQL` C API declarations.
- `sql/handler.h`: `handlerton::discover_table`,
  `handlerton::discover_table_names`, `handlerton::discover_table_existence`,
  `tabledef_version`, and `HA_ERR_TABLE_DEF_CHANGED`.
- `sql/table.h`: `TABLE_SHARE::init_from_binary_frm_image()` and
  `TABLE_SHARE::init_from_sql_statement_string()`.

Supporting documentation:

- MariaDB 11.8.6 release notes: <https://mariadb.com/docs/release-notes/community-server/11.8/11.8.6>
- Embedded MariaDB interface: <https://mariadb.com/kb/en/embedded-mariadb-interface/>
- Table discovery: <https://mariadb.com/kb/en/table-discovery/>
- MariaDB Server licensing: <https://github.com/MariaDB/server#licensing>
- MariaDB licensing FAQ: <https://mariadb.com/docs/general-resources/community/community/faq/licensing-questions/licensing-faq>
