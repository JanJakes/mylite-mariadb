# Dynamic Plugin Loading Trim

## Problem

MyLite's core runtime is an embedded, file-owned library. Dynamic server
plugins require host shared-library discovery, a plugin directory, SQL
installation commands, and optional rows in server-owned `mysql.plugin`
metadata. The default embedded profile already rejects plugin installation SQL
and configures `WITHOUT_DYNAMIC_PLUGINS=ON`, but the generated MariaDB
configuration still defines `HAVE_DLOPEN`, reports dynamic loading capability,
and retains dynamic loader branches in `sql_plugin.cc`.

That is misleading for callers and keeps an unnecessary server extension path
inside the default embedded profile.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents plugins as server extensions that can be loaded at startup
  or runtime without rebuilding the server:
  <https://mariadb.com/docs/server/reference/plugins/plugin-overview>.
- The same MariaDB plugin overview describes `INSTALL SONAME`,
  `INSTALL PLUGIN`, `--plugin-load`, `--plugin-load-add`, and the
  `plugin_dir` system variable as dynamic plugin installation/loading paths.
- MariaDB documents CMake plugin build choices separately from runtime loading;
  `PLUGIN_<name>=NO` removes a plugin from the build, while other values build
  it statically or dynamically:
  <https://mariadb.com/kb/en/specifying-which-plugins-to-build/>.
- `mariadb/configure.cmake` checks the platform `dlopen()` function and sets
  `HAVE_DLOPEN` independently of MyLite's `WITHOUT_DYNAMIC_PLUGINS=ON` profile
  setting.
- `mariadb/config.h.cmake` exposes `HAVE_DLOPEN` to the SQL layer, and the
  current default embedded cache still generates `#define HAVE_DLOPEN 1`.
- `mariadb/sql/mysqld.cc` sets `have_dynamic_loading` to `YES` whenever
  `HAVE_DLOPEN` is defined, even though MyLite rejects plugin installation
  SQL and starts with a MyLite-owned empty plugin directory.
- `mariadb/sql/sql_plugin.cc` compiles the `plugin_dl_add()` `dlopen()` path,
  plugin declaration readers, service pointer backups, `dlsym()` calls, and
  `dlclose()` cleanup under `HAVE_DLOPEN`.
- `mariadb/sql/sql_plugin.cc` already has a no-`HAVE_DLOPEN` fail-closed path:
  `plugin_dl_add()` raises `ER_FEATURE_DISABLED` for `"plugin"` /
  `"HAVE_DLOPEN"`.
- `mariadb/include/my_stacktrace.h` enables the fork-based address resolver on
  non-Windows platforms; that path calls `dladdr()`, so a no-loader embedded
  profile must also disable `MY_ADDR_RESOLVE_FORK`.
- `packages/libmylite/src/database.cc` rejects top-level `INSTALL PLUGIN`,
  `INSTALL SONAME`, `UNINSTALL PLUGIN`, and `UNINSTALL SONAME` before MariaDB
  execution as unsupported server-oriented SQL.

## Design

- Add `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING`, defaulting to `ON` for
  upstream-style builds and forced `OFF` in MyLite's embedded baseline.
- When disabled, force `HAVE_DLOPEN=FALSE` after MariaDB's platform function
  probe so generated embedded configuration headers omit `HAVE_DLOPEN`; clear
  related `HAVE_DLADDR`, `HAVE_DLERROR`, and `HAVE_DLFCN_H` probes at the same
  boundary so the generated header does not include dynamic-loader declarations.
- Emit `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0` into the generated configuration
  so platform compatibility shims, including the Windows `LoadLibrary` wrapper,
  cannot recreate the runtime-loader capability after the probes are cleared.
- Keep `WITHOUT_DYNAMIC_PLUGINS=ON`; this slice complements the existing
  dynamic module build switch by disabling the runtime loader capability.
- Keep static built-in plugin registration intact. Storage engines and
  built-in type/function plugins that remain in the profile must continue to
  initialize through the normal static plugin arrays.
- Keep the MyLite-owned empty plugin directory startup argument for inherited
  MariaDB paths and visible `plugin_dir` metadata. The directory must not
  imply that host shared-library loading is supported.
- Teach first-party CMake integration to skip `dl` when the referenced embedded
  archive cache has dynamic plugin loading disabled.
- Extend direct and prepared SQL tests to prove `have_dynamic_loading=NO` and
  representative plugin install/uninstall SQL remains rejected.

## Affected Subsystems

- MariaDB configure-time capability detection and generated config headers.
- Dynamic plugin loading branches in `sql_plugin.cc`.
- Fork-based `dladdr()` address-resolution selection in `my_stacktrace.h`.
- Server variable reporting for `have_dynamic_loading`.
- First-party imported MariaDB embedded target link interface.
- Server-surface compatibility tests and documentation.
- Size measurement reporting.

## Compatibility Impact

MariaDB servers can dynamically install and load plugins from a configured
plugin directory. MyLite's core embedded profile intentionally does not expose
that server extension model. Dynamic plugin installation remains rejected before
MariaDB execution, `have_dynamic_loading` reports `NO`, and the embedded
runtime keeps only statically linked, explicitly configured built-ins.

This preserves the current MyLite policy while making the compiled capability
match the documented unsupported surface.

## DDL Metadata Routing Impact

No table DDL metadata routing change. Dynamic plugin installation writes
server plugin metadata, not MyLite table catalog metadata, and remains out of
scope for the core runtime.

## Single-File And Embedded-Lifecycle Impact

No `.mylite` file-format change and no new companion files. Removing dynamic
loading reduces reliance on host plugin directories and server-owned plugin
state. The inherited runtime plugin directory remains a MyLite-owned empty
directory created and removed as part of the existing embedded lifecycle.

## Public API And File-Format Impact

No C API change. Direct and prepared SQL continue to surface unsupported
plugin-install statements through stable MyLite diagnostics.

## Storage-Engine Routing Impact

Static storage-engine routing remains unchanged. Requested `ENGINE=InnoDB`,
`ENGINE=MyISAM`, `ENGINE=Aria`, `ENGINE=MEMORY`, `ENGINE=HEAP`, and
`ENGINE=BLACKHOLE` behavior is unaffected because those surfaces route through
the existing compiled-in handlers or MyLite policy.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or language adapters should not assume that the core
library can install server plugins dynamically. Adapter-level extension loading
would need its own package boundary and policy.

## Binary-Size Impact

The historical bundle-size research ranked runtime `dlopen()` plugin loading
as a modest archive and linked-runtime win with a clearer capability boundary.
On the current branch, the implemented trim reduced both measured embedded
archives by 16,104 bytes with unchanged member counts:

| Artifact | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Default embedded archive | 26,798,600 bytes / 25.56 MiB | 26,782,496 bytes / 25.54 MiB | -16,104 bytes |
| Storage-smoke archive | 26,979,184 bytes / 25.73 MiB | 26,963,080 bytes / 25.71 MiB | -16,104 bytes |

The rebuilt archives also have no unresolved `dlopen`, `dlsym`, `dlclose`,
`dlerror`, or `dladdr` symbols.

## License And Dependency Impact

No license change and no new dependency. On platforms where the first-party
link interface previously needed `dl` only for MariaDB dynamic plugin loading,
the disabled profile should no longer require that direct link dependency.

## Test And Verification Plan

- Add direct and prepared SQL tests proving:
  - `have_dynamic_loading` reports `NO`,
  - representative `INSTALL PLUGIN` / `INSTALL SONAME` and uninstall forms are
    rejected before MariaDB execution.
- Run default and storage-smoke MariaDB embedded configure/build/measure flows.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness and size report.
- Verify generated embedded headers omit `HAVE_DLOPEN`.
- Verify rebuilt archives have no unresolved dynamic-loader symbols.
- Run formatting, shell syntax, diff, and tidy gates.

## Acceptance Criteria

- `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF` is visible in measured embedded
  caches.
- Generated embedded configuration headers do not define `HAVE_DLOPEN`,
  `HAVE_DLADDR`, `HAVE_DLERROR`, or `HAVE_DLFCN_H`.
- Generated embedded configuration headers define
  `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING` as `0`.
- `have_dynamic_loading=NO` is covered by direct and prepared tests.
- Plugin install/uninstall SQL remains rejected by MyLite policy.
- Existing routed DDL/DML, SQL API, storage-engine smoke, and server-surface
  compatibility groups continue to pass.
- Size measurements and compatibility documentation are updated.

## Risks

- `HAVE_DLOPEN` also gates legacy UDF branches. MyLite already rejects dynamic
  UDF creation and omits UDF runtime, but tests must prove those rejection
  paths stay stable.
- Static plugin initialization still needs `sql_plugin.cc`; this slice must not
  remove the core static plugin registry.
- Some platforms use different dynamic loader libraries. First-party CMake
  should skip explicit `dl` only when the embedded cache proves dynamic plugin
  loading is disabled.
- MariaDB crash/address-symbol resolution is best effort. Disabling
  `MY_ADDR_RESOLVE_FORK` in the no-loader profile removes the `dladdr()`-based
  resolver path rather than keeping a hidden dynamic-loader dependency.
