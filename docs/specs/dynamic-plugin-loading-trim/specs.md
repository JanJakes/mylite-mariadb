# Dynamic Plugin Loading Trim

## Problem

The embedded profile still compiles MariaDB's runtime dynamic plugin loader.
The core MyLite library already rejects `INSTALL PLUGIN` and `UNINSTALL
PLUGIN`, points `plugin_dir` at transient database-local runtime state, and does
not expose external durable engines. Keeping the shared-object loader adds code
for a server extension model that the embedded API does not support.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/sql_plugin.cc` owns plugin registration, built-in plugin
  initialization, `--plugin-load`, `mysql.plugin` loading, `dlopen()`, plugin
  declaration discovery, and dynamic service pointer wiring.
- `mariadb/sql/mysqld.cc` sets `have_dynamic_loading` from `HAVE_DLOPEN`.
- `packages/libmylite/src/database.cc` already rejects dynamic plugin SQL
  before direct execution or prepared-statement preparation.
- `packages/libmylite/tests/embedded_server_surface_policy_test.c` already
  covers rejected plugin SQL and the database-local transient `plugin_dir`.

## Design

Add `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING`, defaulting to `ON`, and set it to
`OFF` in the embedded baseline.

When the option is off:

- keep static built-in plugin registration and initialization unchanged;
- keep storage engines, data-type plugins, and compression-provider fallback
  services available to the embedded runtime;
- do not compile the `dlopen()` plugin declaration reader or
  command-line/plugin-table dynamic loading paths;
- do not compile the dynamic plugin service-injection table;
- make runtime attempts to inspect a dynamic plugin shared object fail through
  the existing disabled-feature path;
- report `@@have_dynamic_loading=NO` in the embedded profile.

Normal MariaDB server builds keep dynamic plugin loading unless the MyLite
option is explicitly disabled.

## Non-Goals

- Do not remove static built-in plugins or native storage engines.
- Do not remove JSON, GEOMETRY/GIS, collations, prepared statements, DDL, DML,
  transactions, or application SQL behavior.
- Do not remove the transient database-local `plugin_dir`; it remains part of
  the contained startup contract for retained MariaDB code that expects a plugin
  directory.
- Do not change the existing policy rejection for `INSTALL PLUGIN` and
  `UNINSTALL PLUGIN`.

## Compatibility Impact

Dynamic plugin loading is already outside the core embedded API. Applications
using supported MySQL/MariaDB SQL, native storage engines, prepared statements,
diagnostics, and result metadata are unaffected. Applications that expect to
load external server plugins must use a future adapter or custom build profile,
not the default embedded core library.

## Directory And Lifecycle Impact

No durable file behavior changes. `plugin_dir` still points inside
`<db>.mylite/run/plugins` while the database is open and is removed with other
runtime state on clean close. The loader trim prevents the default embedded
profile from opening shared objects from that directory.

## Test Plan

Run:

```sh
tools/mariadb-embedded-build build
tools/mariadb-embedded-build measure
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- The embedded baseline reports `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=OFF`.
- `@@have_dynamic_loading` reports `NO`.
- Static built-in storage engines covered by the test suite still initialize.
- `INSTALL PLUGIN` and `UNINSTALL PLUGIN` remain rejected by policy coverage.
- The measured archive size and member count are recorded in the build and
  size-profile docs.
- The default embedded archive measures 26,623,920 bytes, 25.39 MiB, with 705
  members on the recorded 2026-05-21 macOS arm64 build host.

## Risks

- Accidentally disabling the whole plugin subsystem would break static storage
  engines. The implementation must only remove runtime shared-object loading.
- Some inherited MariaDB diagnostic paths can still mention `plugin_dir`; that
  is acceptable as long as the directory remains transient and contained.
