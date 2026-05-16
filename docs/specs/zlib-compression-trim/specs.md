# Zlib Compression Trim

## Problem

MyLite's embedded profile is an in-process, file-owned runtime with no daemon
wire protocol, no binary log, and no durable InnoDB file ownership. The current
profile still links zlib-backed compression code for SQL `COMPRESS()` /
`UNCOMPRESS()`, storage-engine-independent compressed columns, compressed
network packet helpers, compressed binlog events, and InnoDB zlib page
compression paths.

That retained code keeps zlib in the linked runtime dependency set even though
the current product surface does not use it for supported MyLite storage or API
workflows.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents `COMPRESS()` as requiring a compression library such as
  zlib; otherwise it returns `NULL`, and `have_compress` reports whether the
  library is present:
  <https://mariadb.com/docs/server/reference/sql-functions/secondary-functions/encryption-hashing-and-compression-functions/compress>.
- MariaDB documents `UNCOMPRESS()` with the same zlib/library dependency and
  `NULL` behavior when compression support is unavailable:
  <https://mariadb.com/kb/en/uncompress/>.
- MariaDB's storage-engine-independent column compression supports
  `COMPRESSED[=<compression_method>]` on string/blob columns, and currently only
  zlib is supported:
  <https://mariadb.com/kb/en/storage-engine-independent-column-compression/>.
- MariaDB documents binary-log event compression as optional, transparent
  compression for selected query and row events using zlib:
  <https://mariadb.com/docs/server/server-management/server-monitoring-logs/binary-log/compressing-events-to-reduce-size-of-the-binary-log>.
- `mariadb/cmake/zlib.cmake` unconditionally sets `HAVE_COMPRESS=1` after
  finding or building zlib, which makes `mariadb/config.h.cmake` expose zlib
  compression support to SQL, protocol, and diagnostics code.
- `mariadb/mysys/my_compress.c` implements packet compression helpers
  `my_compress()`, `my_uncompress()`, `my_compress_alloc()`, and
  `my_compress_buffer()` behind `HAVE_COMPRESS`.
- `mariadb/sql/item_strfunc.cc` implements zlib-backed SQL `COMPRESS()` and
  `UNCOMPRESS()` only under `HAVE_COMPRESS`; `mariadb/sql/item_strfunc.h`
  already returns `NULL` for those functions when `HAVE_COMPRESS` is absent.
- `mariadb/sql/field_comp.cc` defines the zlib column-compression method table,
  and `mariadb/sql/field.cc` uses `zlib_compression_method` when parsing the
  `COMPRESSED` column attribute.
- `mariadb/sql/net_serv.cc` compiles compressed packet send/read helpers under
  `HAVE_COMPRESS`.
- `mariadb/sql/log_event.cc` unconditionally includes zlib and implements
  compressed binlog event helpers with `compressBound()`, `compress()`, and
  `uncompress()`. These symbols remain unresolved from `log_event.cc.o` even
  when MyLite's no-binlog profile does not write binlogs.
- `mariadb/storage/innobase/fil/fil0pagecompress.cc` unconditionally includes
  zlib and uses `compress2()` / `uncompress()` for the zlib page-compression
  algorithm. The current embedded archive still includes static InnoDB sources,
  even though supported `ENGINE=InnoDB` table DDL routes to MyLite storage.
- Retained InnoDB compressed-page objects in `btr0cur.cc`, `fts0opt.cc`, and
  `page0zip.cc` also reference zlib directly even though native compressed
  InnoDB pages are not part of MyLite's routed storage surface.
- Current linked embedded smoke binaries depend on `/usr/lib/libz.1.dylib` and
  retain unresolved zlib symbols from `my_compress.c`, `field_comp.cc`,
  `item_strfunc.cc`, `log_event.cc`, and InnoDB page-compression code.

## Design

- Add `MYLITE_WITH_ZLIB_COMPRESSION`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, `mariadb/cmake/zlib.cmake` must not find, link, or define
  zlib compression support for the embedded server profile. Connector/C
  configure metadata can still describe its private bundled zlib plugin, but
  the `libmariadbd.a` target and first-party linked embedded smoke binaries
  must not link system or bundled zlib.
- Rely on MariaDB's existing no-`HAVE_COMPRESS` SQL function behavior:
  `COMPRESS()` and `UNCOMPRESS()` remain valid functions but return `NULL`.
  `UNCOMPRESSED_LENGTH()` remains available because it reads the length prefix
  and does not need zlib.
- Replace `field_comp.cc` in the disabled profile with a MyLite compression
  method table that contains no supported methods.
- Change `Column_definition::set_compressed()` so a no-`HAVE_COMPRESS` build
  rejects `COMPRESSED` column attributes with an explicit unsupported
  diagnostic instead of constructing fields with a null compression method.
- Compile compressed binlog helper functions as fail-closed no-op helpers when
  `HAVE_COMPRESS` is absent. They should report failure to callers without
  linking zlib.
- Compile InnoDB zlib page compression branches as unavailable when
  `HAVE_COMPRESS` is absent. Non-zlib branches can remain because they use
  existing provider-service stubs and are outside this zlib slice.
- Compile retained InnoDB compressed-page helper sources against local
  fail-closed zlib API shims when `HAVE_COMPRESS` is absent so static objects
  do not retain unresolved zlib symbols or pollute the final binary with global
  replacement zlib symbols.
- Make the first-party imported MariaDB embedded target link `ZLIB::ZLIB` only
  when the referenced embedded archive cache has zlib compression enabled.
- Keep `have_compress=NO` in the disabled embedded profile.
- Do not remove the zlib source tree from the MariaDB import.
- Do not make a broader decision about final packaging of OpenSSL, PCRE, or
  other dynamic dependencies in this slice.

## Affected Subsystems

- MariaDB CMake zlib detection and embedded baseline configuration.
- SQL compression functions and `have_compress` reporting.
- Storage-engine-independent compressed column parsing and field construction.
- Compressed network packet helpers in retained server protocol code.
- Compressed binlog helper bodies in retained no-binlog objects.
- InnoDB zlib page-compression and retained compressed-page helpers in static
  InnoDB objects.
- Size report and compatibility documentation.

## Compatibility Impact

MariaDB with zlib supports `COMPRESS()`, `UNCOMPRESS()`, compressed column
storage, compressed protocol packets, compressed binlog events, and InnoDB zlib
page compression. MyLite's default embedded profile intentionally reports no
compression library through `have_compress`, returns `NULL` from SQL
`COMPRESS()` / `UNCOMPRESS()`, rejects compressed column DDL, and keeps binlog,
network compression, and InnoDB page compression outside the core runtime.

This is a compatibility tradeoff, not a storage-format change. It matches
MariaDB's documented no-compression-library SQL-function behavior and keeps
unsupported durable compression formats out of MyLite's single-file storage
model.

## DDL Metadata Routing Impact

Compressed columns are rejected during DDL parsing/definition rather than routed
into MyLite catalog metadata. Existing uncompressed string and BLOB/TEXT column
DDL remains unchanged. `ROW_FORMAT=COMPRESSED` and InnoDB page-compression table
options are already outside the supported routed table surface until MyLite has
a deliberate storage-format compression design.

## Single-File And Embedded-Lifecycle Impact

No `.mylite` file-format change and no new companion files. Removing zlib
compression support reduces inherited daemon/storage-engine behavior that would
otherwise be tied to external server files, network packets, binlog files, or
InnoDB page formats rather than MyLite's primary file lifecycle.

## Public API And File-Format Impact

No C API change. Existing direct and prepared SQL execution APIs surface the new
unsupported compressed-column diagnostic through the normal MyLite diagnostics.
SQL `COMPRESS()` and `UNCOMPRESS()` return `NULL` instead of failing.

## Storage-Engine Routing Impact

Supported routed base-table behavior remains uncompressed. Requested engines
such as `InnoDB`, `MyISAM`, and `Aria` continue to resolve to MyLite storage for
covered DDL/DML; this slice does not add MyLite page, row, or column
compression.

## Wire-Protocol Or Integration-Package Impact

The core `libmylite` runtime has no daemon socket startup contract. Future
wire-protocol adapters should treat protocol compression as an adapter-level
feature and must not assume the core library links MariaDB's zlib packet
compression helpers.

## Binary-Size Impact

The bundle-size research ranked zlib compression trimming as a modest linked
runtime and archive win with an important dependency effect: zlib can leave the
runtime dependency set once all retained objects stop referencing zlib symbols.

Implemented measurements on 2026-05-16 against the view-runtime-trim baseline:

- default embedded archive: 26,798,600 bytes, down 43,656 bytes from
  26,842,256 bytes,
- storage-smoke embedded archive: 26,979,184 bytes, down 43,656 bytes from
  27,022,840 bytes,
- linked first-party embedded and storage-smoke binaries no longer link
  `/usr/lib/libz.1.dylib`.

## License And Dependency Impact

No license change. The disabled profile avoids linking the system or bundled
zlib library for the embedded runtime. Upstream MariaDB source still contains
the zlib integration and the bundled zlib tree for upstream-style builds and
Connector/C configure metadata.

## Test And Verification Plan

- Add direct and prepared SQL tests proving:
  - `have_compress` reports `NO`,
  - `COMPRESS()` and `UNCOMPRESS()` return `NULL`,
  - compressed column DDL is rejected with an unsupported diagnostic.
- Run default and storage-smoke MariaDB embedded configure/build/measure flows.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness and size report.
- Verify linked embedded smoke binaries no longer depend on libz and no longer
  retain unresolved zlib symbols.
- Run formatting, shell syntax, diff, and tidy gates.

## Acceptance Criteria

- `MYLITE_WITH_ZLIB_COMPRESSION=OFF` is visible in measured embedded caches.
- `HAVE_COMPRESS` is absent from generated embedded configuration headers.
- Direct and prepared SQL tests pass for `have_compress=NO`, nullable
  SQL-compression functions, and rejected compressed-column DDL.
- No linked embedded smoke binary depends on libz.
- No retained linked embedded smoke binary has unresolved zlib symbols.
- Existing routed DDL/DML, SQL API, storage-engine smoke, and server-surface
  compatibility groups continue to pass.
- Size measurements and compatibility documentation are updated.

## Risks

- Zlib references are spread across otherwise-retained objects. Removing the
  CMake link library without stubbing all retained references will fail at link
  time.
- Compressed column parsing must fail before field construction can install a
  null compression method.
- InnoDB sources are still statically retained even though supported table DDL
  routes `ENGINE=InnoDB` to MyLite. Zlib page-compression branches must fail
  closed without disturbing unrelated retained InnoDB initialization paths.
