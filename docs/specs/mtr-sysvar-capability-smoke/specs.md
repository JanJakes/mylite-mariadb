# MTR Sysvar Capability Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with accepted upstream
system-variable tests for embedded-profile capability and default-state
surfaces:

- `sys_vars.have_compress_basic`
- `sys_vars.have_crypt_basic`
- `sys_vars.have_dynamic_loading_basic`
- `sys_vars.have_geometry_basic`
- `sys_vars.have_openssl_basic`
- `sys_vars.have_profiling_basic`
- `sys_vars.have_query_cache_basic`
- `sys_vars.have_rtree_keys_basic`
- `sys_vars.have_ssl_basic`
- `sys_vars.have_symlink_basic`
- `sys_vars.local_infile_basic`
- `sys_vars.secure_auth_basic`
- `sys_vars.general_log_file_basic`
- `sys_vars.init_connect_basic`
- `sys_vars.ft_boolean_syntax_basic`

## Non-Goals

- Broad system-variable MTR coverage.
- Re-enabling disabled SQL surfaces such as log tables, events, native MyISAM,
  native InnoDB bootstrap metadata, or enforced/default storage-engine tests
  that upstream marks unsuitable for embedded.
- Changing default system-variable values or MyLite runtime policy.
- Running these tests against MyLite storage-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/suite/sys_vars/t/have_*_basic.test` files check
  compiled capability variables such as compression, dynamic loading,
  geometry, profiling, query cache, RTREE support, SSL, and symlink support.
- `mariadb/mysql-test/suite/sys_vars/t/local_infile_basic.test` checks the
  `local_infile` variable surface without executing host-file SQL I/O.
- `mariadb/mysql-test/suite/sys_vars/t/secure_auth_basic.test` checks the
  retained authentication-mode variable surface under the embedded profile.
- `mariadb/mysql-test/suite/sys_vars/t/general_log_file_basic.test`,
  `init_connect_basic.test`, and `ft_boolean_syntax_basic.test` cover retained
  path/string system-variable surfaces without requiring disabled runtime
  producers.
- The selected tests pass under the MyLite MTR smoke profile without upstream
  source changes.
- Probed candidates intentionally left out of accepted coverage:
  - `sys_vars.default_storage_engine_basic`,
    `sys_vars.event_scheduler_basic`, and
    `sys_vars.enforce_storage_engine_basic` are skipped by upstream MTR under
    embedded server.
  - `sys_vars.default_tmp_storage_engine_basic` assigns native MyISAM as the
    default temporary engine, which is absent in the trimmed profile.
  - `sys_vars.general_log_basic` enables `general_log` and expects
    `mysql.general_log`, which the trimmed bootstrap schema omits.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected system-variable capability, local-infile, security, log-path,
initialization, and FULLTEXT syntax variable behavior. Known unsupported/probed
candidates remain non-coverage and do not change SQL, C API, storage-engine, or
file-format behavior.

## Design

- Add the selected passing tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Add the understood non-coverage probes to the harness unsupported inventory
  with reason categories.
- Do not modify upstream MariaDB test files.
- Keep skipped, disabled-engine, unsupported-surface, and profile-drift
  candidates outside the accepted list.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice expands opt-in MariaDB embedded MTR
baseline coverage and probe inventory only.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe sys_vars.have_compress_basic sys_vars.have_crypt_basic sys_vars.have_dynamic_loading_basic sys_vars.have_geometry_basic sys_vars.have_openssl_basic sys_vars.have_profiling_basic sys_vars.have_query_cache_basic sys_vars.have_rtree_keys_basic sys_vars.have_ssl_basic sys_vars.have_symlink_basic sys_vars.default_storage_engine_basic sys_vars.default_tmp_storage_engine_basic sys_vars.local_infile_basic sys_vars.secure_auth_basic sys_vars.general_log_basic sys_vars.general_log_file_basic sys_vars.init_connect_basic sys_vars.event_scheduler_basic sys_vars.enforce_storage_engine_basic sys_vars.ft_boolean_syntax_basic`
- `tools/mylite-mtr-harness run sys_vars.have_compress_basic sys_vars.have_crypt_basic sys_vars.have_dynamic_loading_basic sys_vars.have_geometry_basic sys_vars.have_openssl_basic sys_vars.have_profiling_basic sys_vars.have_query_cache_basic sys_vars.have_rtree_keys_basic sys_vars.have_ssl_basic sys_vars.have_symlink_basic sys_vars.local_infile_basic sys_vars.secure_auth_basic sys_vars.general_log_file_basic sys_vars.init_connect_basic sys_vars.ft_boolean_syntax_basic`
- `tools/mylite-mtr-harness coverage`
- `tools/mylite-mtr-harness list-unsupported`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes the selected system-variable tests.
- All selected tests pass under the MyLite MTR smoke profile.
- The unsupported inventory contains the newly understood non-coverage probes
  without overlapping accepted curated tests.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Verification Results

- `tools/mylite-mtr-harness probe sys_vars.have_compress_basic sys_vars.have_crypt_basic sys_vars.have_dynamic_loading_basic sys_vars.have_geometry_basic sys_vars.have_openssl_basic sys_vars.have_profiling_basic sys_vars.have_query_cache_basic sys_vars.have_rtree_keys_basic sys_vars.have_ssl_basic sys_vars.have_symlink_basic sys_vars.default_storage_engine_basic sys_vars.default_tmp_storage_engine_basic sys_vars.local_infile_basic sys_vars.secure_auth_basic sys_vars.general_log_basic sys_vars.general_log_file_basic sys_vars.init_connect_basic sys_vars.event_scheduler_basic sys_vars.enforce_storage_engine_basic sys_vars.ft_boolean_syntax_basic`: 15 passed, 2 failed, and 3 skipped.
- `tools/mylite-mtr-harness run sys_vars.have_compress_basic sys_vars.have_crypt_basic sys_vars.have_dynamic_loading_basic sys_vars.have_geometry_basic sys_vars.have_openssl_basic sys_vars.have_profiling_basic sys_vars.have_query_cache_basic sys_vars.have_rtree_keys_basic sys_vars.have_ssl_basic sys_vars.have_symlink_basic sys_vars.local_infile_basic sys_vars.secure_auth_basic sys_vars.general_log_file_basic sys_vars.init_connect_basic sys_vars.ft_boolean_syntax_basic`: all 15 selected tests passed.
- `tools/mylite-mtr-harness run`: 8 MyLite profile tests, 204 upstream `main`
  tests, and 209 upstream `sys_vars` tests passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 413 accepted upstream baseline tests, 8 accepted MyLite profile tests, 17 accepted MyLite storage-routed tests, 438 accepted total tests, and 92 known unsupported upstream probes.
- `tools/mylite-mtr-harness list-unsupported`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- Broader system-variable MTR suites need separate disabled-surface and
  profile-output normalization review.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing behavior.
