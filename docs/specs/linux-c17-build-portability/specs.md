# Linux C17 Build Portability

## Problem

The Debian 13 VPS build uses GCC 14 with strict C17 flags from MyLite's
first-party CMake targets. The first storage-smoke target build failed before
prepared-insert profiling because several first-party C files use POSIX/GNU
interfaces without enabling their declarations under `-std=c17`, and GCC 14
reports pre-C23 pointer-to-array qualifier conversions under `-Wpedantic`.

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party compiler policy:
  `cmake/MyLiteCompilerOptions.cmake::mylite_configure_c_target()`.
- MyLite MariaDB embedded archive link policy:
  `cmake/MyLiteMariaDB.cmake::mylite_add_mariadb_embedded_target()`.
- MariaDB bundled PCRE setup:
  `mariadb/cmake/pcre.cmake::BUNDLE_PCRE2()` and
  `mariadb/libmysqld/CMakeLists.txt` embedded SQL include directories.
- MariaDB dynamic plugin link-map logging:
  `mariadb/sql/sql_plugin.cc`.
- MariaDB timer-thread startup:
  `mariadb/mysys/thr_timer.c::init_thr_timer()` and
  `mariadb/mysys/my_pthread.c::my_setstacksize()`.
- Affected MyLite storage recovery declarations:
  `packages/mylite-storage/src/storage.c::restore_recovery_journal()`,
  `write_recovery_journal_file()`, and
  `validate_recovery_journal_pages()`.
- Affected storage recovery test helpers:
  `packages/mylite-storage/tests/storage_test.c::write_test_recovery_journal()`,
  `write_test_transaction_journal()`, and
  `write_test_legacy_recovery_journal()`.

## Design

- Define `_GNU_SOURCE` for first-party C targets on Linux while keeping
  `C_EXTENSIONS OFF` and C17 as the language standard. This exposes the
  declarations for the POSIX/GNU APIs already used by storage and embedded
  tests without changing compiler dialect.
- Keep recovery-journal page buffers mutable in function signatures that accept
  page-array scratch buffers. The functions still treat the bytes as read-only,
  but the C17 type no longer asks callers to convert `unsigned char (*)[N]` to
  `const unsigned char (*)[N]`, which GCC 14 diagnoses as a pre-C23 pedantic
  issue.
- Link the imported MariaDB embedded archive with generated sibling archives
  and host libraries that the archive references on this Linux profile:
  `tpool/libtpool.a` when present, and the host `crypt` library when available
  on non-Apple platforms.
- Populate `PCRE_INCLUDE_DIRS` when MariaDB falls back to its bundled PCRE2
  external project. `libmysqld` compiles SQL sources directly and already
  includes `${PCRE_INCLUDE_DIRS}`, so the bundled generated header directory
  must be present there instead of relying only on imported-library usage
  requirements used by other targets.
- Guard `sql_plugin.cc` link-map logging with
  `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING` as well as `HAVE_LINK_H`. The link-map
  code only describes dynamically loaded plugins; when MyLite disables dynamic
  plugin loading, including `<link.h>` can reintroduce `dlfcn.h` declarations
  after MariaDB has replaced `dlopen()` with disabled-profile macros.
- Use MariaDB's platform-adjusted `my_setstacksize()` with
  `my_thread_stack_size` for the timer thread instead of a fixed 64 KiB stack.
  This keeps the embedded startup path aligned with MariaDB's connection-thread
  stack handling and avoids `pthread_create(...)=EINVAL` on toolchains whose
  static TLS/runtime footprint cannot fit under the fixed timer-thread stack.

## Compatibility Impact

No SQL, storage format, public C API, storage-engine routing, or single-file
lifecycle behavior changes. The slice only affects Linux first-party build
portability and static type declarations for internal helpers.

## Scope And Implications

- Affected subsystems: first-party C target compiler flags, the imported
  MariaDB embedded archive link interface, MariaDB bundled PCRE configure
  metadata, dynamic plugin link-map diagnostics, and MariaDB timer-thread
  startup.
- DDL metadata routing, storage-engine routing, wire-protocol integration,
  public API, and file format: no behavior changes.
- Single-file lifecycle: no durable companions or storage layout changes.
- Dependency and binary-size impact: the top-level MyLite link now records
  MariaDB's existing Linux `tpool` archive and host `crypt` dependency instead
  of leaving unresolved symbols. Bundled PCRE remains MariaDB's existing
  fallback and is not newly introduced by this slice.
- Risks: the timer-thread stack change is a narrow upstream-derived delta in a
  MariaDB file. It uses MariaDB's existing `my_setstacksize()` helper and
  configured `my_thread_stack_size`, which is less host-fragile than the fixed
  64 KiB stack on this VPS.

## Tests And Verification

- Reconfigure and rebuild `storage-smoke-dev` first-party targets after the
  failed VPS build.
- Reconfigure MariaDB's embedded storage-smoke archive without generated
  `pcre2.h` or `HAVE_LINK_H` edits.
- Run the normal storage and storage-engine smoke checks.
- Run the prepared-insert component benchmark needed for the next performance
  slice.

## Acceptance Criteria

- Linux strict C17 builds expose declarations for MyLite's existing POSIX/GNU
  calls without implicit-function or missing-type warnings.
- GCC 14 no longer reports the recovery-journal page-array parameter warnings
  under `-Wpedantic -Werror`.
- The storage-smoke benchmark and tests can compile and link on the VPS without
  source workarounds.
- A fresh MariaDB embedded profile configure can regenerate `my_config.h`
  without requiring generated `HAVE_LINK_H` edits, and bundled PCRE2 exposes
  its generated headers to embedded SQL compilation.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`:
  passed after deleting the generated `include/pcre2.h` symlink and affected
  embedded SQL objects.
- `cmake --build build/mariadb-mylite-storage-smoke --target sql_embedded`:
  passed and rebuilt `item_cmpfunc.cc` and `sql_plugin.cc` with regenerated
  `HAVE_LINK_H=1` and `MYLITE_WITH_DYNAMIC_PLUGIN_LOADING=0`.
- `cmake --build build/mariadb-mylite-storage-smoke --target mysqlserver`:
  passed and rebuilt `libmysqld/libmariadbd.a`.
- `BUILD_DIR=build/mariadb-mylite-bundled-pcre-config tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC -DWITH_PCRE=bundled -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`:
  passed; embedded `item_cmpfunc.cc` and `sql_plugin.cc` compile commands
  include `extra/pcre2/src/pcre2-build/interface`.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured 314.674 us/op on the VPS, with
  packed index tail-append scan pages at 5086 and branch tail overlay scan reads
  at 48.
