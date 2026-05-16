# Native MyISAM Engine Trim

## Problem

The default embedded profile still registers native MyISAM and MRG_MyISAM
storage engines and pulls the native MyISAM archive into `libmariadbd.a`.
MyLite already routes application `ENGINE=MyISAM` DDL to the MyLite storage
engine in file-backed storage builds, and native `.MYD` / `.MYI` files are not
valid durable application storage for the product.

The historical bundle-size note called this area "MyISAM temp-spill" work.
That label is stale for the current branch: the configured embedded profile
already builds with `USE_ARIA_FOR_TMP_TABLES=ON`, so MariaDB internal disk
temporary tables use Aria rather than MyISAM. The current remaining target is
native MyISAM and MRG_MyISAM engine registration, plus Aria's broad link to the
MyISAM archive for a small set of shared fulltext support symbols.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/myisam/CMakeLists.txt` registers `myisam` as a mandatory
  storage engine with `RECOMPILE_FOR_EMBEDDED`.
- `mariadb/storage/myisammrg/CMakeLists.txt` registers `myisammrg` as a
  mandatory storage engine with `RECOMPILE_FOR_EMBEDDED`.
- `mariadb/storage/maria/CMakeLists.txt` registers Aria as mandatory and links
  it against `myisam`, which currently adds the native MyISAM archive to
  `EMBEDDED_PLUGIN_LIBS` even when MyISAM is not needed for application table
  storage.
- `nm -u build/mariadb-embedded/storage/maria/libaria_embedded.a` shows Aria's
  MyISAM-facing unresolved symbols are the fulltext support globals and helper
  entry points: `ft_boolean_syntax`, `ft_default_parser`, `ft_free_stopwords`,
  `ft_init_stopwords`, `ft_keysegs`, `ft_min_word_len`, `ft_max_word_len`,
  `ft_query_expansion_limit`, `ft_boolean_check_syntax_string`, and
  `is_stopword`.
- `mariadb/sql/sql_class.h` sets `TMP_ENGINE_HTON` to `maria_hton` and
  `TMP_ENGINE_NAME` to `Aria` when `USE_ARIA_FOR_TMP_TABLES` is defined.
  The current embedded CMake cache has `USE_ARIA_FOR_TMP_TABLES=ON`.
- `mariadb/sql/sql_select.cc` routes disk temporary tables and HEAP-to-disk
  conversions through `TMP_ENGINE_HTON`, so the current profile's internal disk
  temp tables use Aria.
- Before this slice, `packages/libmylite/src/database.cc` still started the
  embedded runtime with `--default-storage-engine=MyISAM`. File-backed
  storage-smoke sessions then set `default_storage_engine=MYLITE` and
  `enforce_storage_engine=MYLITE`, but the default embedded and `:memory:`
  bootstrap paths still relied on native MyISAM as the startup default.
- `mariadb/sql/mysqld.cc` and `mariadb/sql/sys_vars.cc` keep several
  server-global MyISAM option and system-variable roots such as `log-isam`,
  `concurrent_insert`, `delay_key_write`, `flush`, `myisam_recover_options`,
  `myisam_flush`, and `myisam_single_user`.
- `mariadb/sql/sql_plugin.cc` initializes native MyISAM before loading
  installed plugins so child `THD` objects have a non-null default table
  plugin during early plugin loading. With native MyISAM disabled, this
  startup placeholder must be an available built-in engine.
- `docs/specs/engine-routing-policy/specs.md` and storage-smoke coverage treat
  `ENGINE=MyISAM` as a requested-engine spelling that routes to effective
  `MYLITE` storage for file-backed sessions.

## Design

- Add `MYLITE_WITH_NATIVE_MYISAM_STORAGE_ENGINE`, defaulting to `ON` for normal
  MariaDB builds and forced `OFF` by the MyLite embedded baseline.
- Add `MYLITE_WITH_NATIVE_MYISAMMRG_STORAGE_ENGINE`, defaulting to `ON` for
  normal MariaDB builds and forced `OFF` by the MyLite embedded baseline.
- When native MyISAM is disabled:
  - do not register the native `myisam` storage engine,
  - do not add `myisam` or `myisam_embedded` to the embedded plugin merge list,
  - do not build MyISAM utility targets as part of the disabled profile,
  - keep current normal MariaDB behavior when the option is enabled.
- When native MRG_MyISAM is disabled or native MyISAM is disabled:
  - do not register the native `myisammrg` storage engine,
  - do not add `myisammrg` or `myisammrg_embedded` to the embedded plugin merge
    list.
- Replace Aria's disabled-profile link to the full native MyISAM target with a
  narrow MyLite-owned Aria fulltext support source. That source provides the
  small shared fulltext globals and parser helpers Aria expects without pulling
  native MyISAM table handlers, row/index operations, repair code, or MRG code
  into `libmariadbd.a`.
- Change the embedded bootstrap default storage engine from `MyISAM` to `Aria`.
  File-backed MyLite storage builds continue to set `MYLITE` as the effective
  default after connecting.
- When native MyISAM is disabled, initialize Aria as MariaDB's early built-in
  storage-engine placeholder before plugin-table loading. Normal configured
  default-engine selection still runs later through MariaDB's existing
  `init_default_storage_engine()` path.
- Omit native-MyISAM-only startup options and system variables from the disabled
  embedded profile instead of retaining compatibility variables that no longer
  control a registered native engine.
- Keep routed application `ENGINE=MyISAM` DDL/DML behavior unchanged in
  file-backed storage-smoke sessions.

## Affected Subsystems

- MariaDB storage plugin CMake registration for MyISAM, MRG_MyISAM, and Aria.
- Embedded runtime startup defaults.
- MyISAM-specific server options and system variables.
- Storage-engine routing tests and server-surface introspection tests.
- Size-reporting documentation and roadmap status.

## MySQL/MariaDB Compatibility Impact

Native MyISAM and MRG_MyISAM engines are not registered in the default embedded
profile. `SHOW ENGINES` should not advertise them as available native engines.
This is a compatibility reduction for applications that expect native MyISAM
files, MERGE tables, MyISAM repair/check behavior, or MyISAM-specific server
variables. Those surfaces conflict with MyLite's file-owned storage model and
are already out of scope or explicitly unsupported.

File-backed application DDL that says `ENGINE=MyISAM` remains compatible at the
MyLite routing layer: the catalog records the requested engine as `MyISAM`, the
effective engine remains `MYLITE`, and no `.MYD` / `.MYI` files are durable
application state.

## DDL Metadata Routing Impact

No catalog format change. The existing requested-engine/effective-engine
metadata split still records `MyISAM` requests as routed MyLite tables. Native
MRG_MyISAM table metadata is not introduced.

## Single-File And Embedded-Lifecycle Impact

The disabled profile must not create native MyISAM `.MYD` / `.MYI` files or
MRG sidecars as durable application storage. Internal Aria temporary files
remain part of the MariaDB-owned temporary runtime directory and are removed on
final close. Storage-smoke sidecar gates must continue to pass for routed
`ENGINE=MyISAM` tables.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

Routed `ENGINE=MyISAM` remains a supported requested-engine spelling in
file-backed MyLite sessions. Native `MyISAM` and `MRG_MyISAM` rows should no
longer appear in the default embedded profile's `SHOW ENGINES` output.

## Binary-Size Impact

The implemented trim removes native MyISAM handler, row/index operation,
fulltext search, RTREE, repair, and MRG_MyISAM objects from the embedded
archive, except for the small Aria fulltext support shim. The measured current
default archive is 25,996,816 bytes / 24.79 MiB with 596 members, a 453,664
byte and 73-member reduction from the pre-slice branch baseline. The measured
current storage-smoke archive is 26,192,208 bytes / 24.98 MiB with 599
members, the same 453,664 byte and 73-member reduction against the matching
pre-slice storage-smoke baseline.

## License And Dependency Impact

No new dependency. The change removes MariaDB-derived native-engine objects
from the disabled embedded profile and adds a small GPL-2.0 MyLite-owned shim
inside the MariaDB-derived tree.

## Test And Verification Plan

- Build and measure the default embedded profile.
- Build and measure the storage-smoke profile with `PLUGIN_MYLITE_SE=STATIC`.
- Confirm `libmariadbd.a` omits native MyISAM and MRG_MyISAM archive members
  such as `ha_myisam.cc.o`, `mi_open.c.o`, `ft_myisam.c.o`,
  `ha_myisammrg.cc.o`, and `myrg_open.c.o`.
- Confirm `SHOW ENGINES` in the default embedded profile does not report
  `MyISAM` or `MRG_MyISAM`.
- Confirm routed storage-smoke `ENGINE=MyISAM` DDL/DML and catalog metadata
  still pass.
- Confirm server-surface tests cover omitted MyISAM variables where the profile
  removes them.
- Run the opt-in MTR smoke profile to confirm MariaDB embedded bootstrap and
  representative scalar SQL still work without native MyISAM.
- Run embedded, storage-smoke, and dev CTest presets.
- Run relevant compatibility harness reports, size report, format checks, tidy,
  shell syntax checks, and `git diff --check`.

## Acceptance Criteria

- Default embedded and storage-smoke archives omit native MyISAM and
  MRG_MyISAM engine objects.
- Aria still builds and starts without linking the native MyISAM target into the
  embedded archive.
- The embedded runtime starts with an available default storage engine when the
  native MyISAM engine is absent.
- The opt-in MTR smoke profile starts from a trimmed Aria bootstrap and passes
  its curated embedded tests.
- File-backed routed `ENGINE=MyISAM` behavior remains covered and unchanged.
- Documentation records the unsupported native-engine boundary and measured
  size impact.

## Verification Results

- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
  - `size_bytes=25996816`, `size_mib=24.79`, `members=596`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
  - `size_bytes=26192208`, `size_mib=24.98`, `members=599`
- Exact archive member probes show no native MyISAM/MRG objects matching
  `ha_myisam`, `ha_myisammrg`, `ft_myisam`, `mi_`, or `myrg_` in either
  archive, while Aria still contributes its own `ma_ft_*`, `ft_maria.c.o`,
  `ha_maria.cc.o`, and the MyLite `mylite_maria_fulltext_support.c.o` shim.
- `build/mariadb-embedded/CMakeCache.txt` and
  `build/mariadb-mylite-storage-smoke/CMakeCache.txt` set
  `MYLITE_WITH_NATIVE_MYISAM_STORAGE_ENGINE=OFF` and
  `MYLITE_WITH_NATIVE_MYISAMMRG_STORAGE_ENGINE=OFF`; `EMBEDDED_PLUGIN_LIBS`
  contains no `myisam` or `myisammrg` entry.
- `cmake --preset dev`
- `cmake --preset embedded-dev`
- `cmake --preset storage-smoke-dev`
- `cmake --build --preset dev`
- `cmake --build --preset embedded-dev`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset dev --output-on-failure`
- `ctest --preset embedded-dev --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface storage-engine sidecar routed-ddl-dml`
- `tools/mylite-size-report`
- `tools/mylite-mtr-harness run`
- `cmake --build --preset dev --target format`
- `cmake --build --preset dev --target format-check`
- `cmake --build --preset dev --target tidy`
- `bash -n tools/mariadb-embedded-build tools/mylite-compat-harness tools/mylite-mtr-harness tools/mylite-size-report`
- `git diff --check`

## Risks And Open Questions

- Aria fulltext support currently shares MyISAM fulltext globals. The shim is
  intentionally narrow because MyLite rejects FULLTEXT index definitions for
  routed durable tables, but broader Aria FULLTEXT parity is not a goal of this
  slice.
- The curated MTR smoke harness no longer builds MyISAM utility targets under
  the default embedded profile; broader MTR work should still decide how much
  native utility surface belongs in opt-in test-only builds.
