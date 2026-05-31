configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/mylite/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/mylite/version.h"
  @ONLY
)

add_library(mylite STATIC
  src/database.cc
  src/ownerless_autoinc_registry.cc
  src/ownerless_dictionary_state.cc
  src/ownerless_latch.cc
  src/ownerless_lock_table.cc
  src/ownerless_innodb_lock_registry.cc
  src/ownerless_mdl.cc
  src/ownerless_page_index.cc
  src/ownerless_page_log.cc
  src/ownerless_page_pin_registry.cc
  src/ownerless_process_registry.cc
  src/ownerless_probe.cc
  src/ownerless_read_view_registry.cc
  src/ownerless_redo_state.cc
  src/ownerless_tablespace_replay.cc
  src/ownerless_trx_registry.cc
  src/ownerless_wait.cc
  src/version.c
)
add_library(MyLite::mylite ALIAS mylite)
set_target_properties(mylite PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(mylite
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)

mylite_configure_c_target(mylite)
mylite_configure_cxx_target(mylite)
mylite_link_mariadb_embedded(mylite)
if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
  target_compile_definitions(mylite PRIVATE
    MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS=1
    MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES=16384
  )
else()
  target_compile_definitions(mylite PRIVATE MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS=0)
endif()

if(MYLITE_BUILD_BUNDLE_AUDIT_PROBE)
  if(NOT MYLITE_WITH_MARIADB_EMBEDDED)
    message(FATAL_ERROR "MYLITE_BUILD_BUNDLE_AUDIT_PROBE requires MYLITE_WITH_MARIADB_EMBEDDED=ON")
  endif()

  add_library(mylite_php_probe MODULE
    audit/mylite_php_probe.c
  )
  target_link_libraries(mylite_php_probe PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_php_probe)
  if(NOT MSVC)
    target_compile_options(mylite PRIVATE -ffunction-sections -fdata-sections)
    target_compile_options(mylite_php_probe PRIVATE -ffunction-sections -fdata-sections)
  endif()
  set_target_properties(mylite_php_probe PROPERTIES
    C_VISIBILITY_PRESET hidden
    PREFIX ""
    OUTPUT_NAME mylite_php_probe
  )

  set(mylite_php_probe_map "${CMAKE_CURRENT_BINARY_DIR}/mylite_php_probe.map")
  if(APPLE)
    set_target_properties(mylite_php_probe PROPERTIES SUFFIX ".so")
    target_link_options(mylite_php_probe PRIVATE
      "LINKER:-dead_strip"
      "LINKER:-exported_symbol,_get_module"
      "LINKER:-map,${mylite_php_probe_map}"
    )
  elseif(UNIX)
    set(mylite_php_probe_exports "${CMAKE_CURRENT_BINARY_DIR}/mylite_php_probe.exports")
    file(WRITE "${mylite_php_probe_exports}" "{\n  global: get_module;\n  local: *;\n};\n")
    target_link_options(mylite_php_probe PRIVATE
      "LINKER:--exclude-libs,ALL"
      "LINKER:--gc-sections"
      "LINKER:--version-script=${mylite_php_probe_exports}"
      "LINKER:-Map,${mylite_php_probe_map}"
    )
  endif()
endif()

if(BUILD_TESTING)
  add_executable(mylite_api_test
    tests/api_test.c
  )
  target_link_libraries(mylite_api_test PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_api_test)

  add_executable(mylite_version_test
    tests/version_test.c
  )
  target_link_libraries(mylite_version_test PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_version_test)

  add_test(NAME libmylite.api COMMAND mylite_api_test)
  add_test(NAME libmylite.version COMMAND mylite_version_test)

  if(MYLITE_WITH_MARIADB_EMBEDDED)
    function(mylite_configure_embedded_c_test target)
      target_link_libraries(${target} PRIVATE MyLite::mylite)
      mylite_configure_c_target(${target})
      target_compile_definitions(${target} PRIVATE _XOPEN_SOURCE=700)
      if(APPLE)
        target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
      endif()
    endfunction()

    add_executable(mylite_embedded_open_close_test
      tests/embedded_open_close_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_open_close_test)
    target_include_directories(mylite_embedded_open_close_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${PROJECT_SOURCE_DIR}/mariadb/storage/innobase/include"
    )

    add_executable(mylite_embedded_exec_test
      tests/embedded_exec_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_exec_test)

    add_executable(mylite_embedded_engine_schema_test
      tests/embedded_engine_schema_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_engine_schema_test)

    add_executable(mylite_embedded_memory_database_test
      tests/embedded_memory_database_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_memory_database_test)

    add_executable(mylite_embedded_storage_test
      tests/embedded_storage_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_storage_test)

    add_executable(mylite_embedded_ddl_coverage_test
      tests/embedded_ddl_coverage_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ddl_coverage_test)

    add_executable(mylite_embedded_ddl_lifecycle_test
      tests/embedded_ddl_lifecycle_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ddl_lifecycle_test)

    add_executable(mylite_embedded_locking_concurrency_test
      tests/embedded_locking_concurrency_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_locking_concurrency_test)

    add_executable(mylite_embedded_same_process_concurrency_test
      tests/embedded_same_process_concurrency_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_same_process_concurrency_test)

    add_executable(mylite_embedded_ownerless_mdl_hooks_test
      tests/embedded_ownerless_mdl_hooks_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ownerless_mdl_hooks_test)
    target_include_directories(mylite_embedded_ownerless_mdl_hooks_test PRIVATE
      "${PROJECT_SOURCE_DIR}/mariadb/sql"
    )

    add_executable(mylite_embedded_ownerless_trx_hooks_test
      tests/embedded_ownerless_trx_hooks_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ownerless_trx_hooks_test)
    target_include_directories(mylite_embedded_ownerless_trx_hooks_test PRIVATE
      "${PROJECT_SOURCE_DIR}/mariadb/storage/innobase/include"
    )

    add_executable(mylite_embedded_ownerless_innodb_lock_hooks_test
      tests/embedded_ownerless_innodb_lock_hooks_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ownerless_innodb_lock_hooks_test)
    target_include_directories(mylite_embedded_ownerless_innodb_lock_hooks_test PRIVATE
      "${PROJECT_SOURCE_DIR}/mariadb/storage/innobase/include"
    )

    add_executable(mylite_ownerless_primitives_test
      tests/ownerless_primitives_test.c
    )
    mylite_configure_embedded_c_test(mylite_ownerless_primitives_test)
    target_include_directories(mylite_ownerless_primitives_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    add_executable(mylite_ownerless_cross_process_sql_test
      tests/ownerless_cross_process_sql_test.c
    )
    mylite_configure_embedded_c_test(mylite_ownerless_cross_process_sql_test)
    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      target_compile_definitions(mylite_ownerless_cross_process_sql_test PRIVATE
        MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS=1
      )
    else()
      target_compile_definitions(mylite_ownerless_cross_process_sql_test PRIVATE
        MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS=0
      )
    endif()

    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      add_executable(mylite_ownerless_negative_proof_test
        tests/ownerless_negative_proof_test.c
      )
      mylite_configure_embedded_c_test(mylite_ownerless_negative_proof_test)
    endif()

    add_executable(mylite_embedded_compatibility_harness_test
      tests/embedded_compatibility_harness_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_compatibility_harness_test)

    add_executable(mylite_embedded_native_table_operations_test
      tests/embedded_native_table_operations_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_native_table_operations_test)

    add_executable(mylite_embedded_prepared_statement_test
      tests/embedded_prepared_statement_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_prepared_statement_test)

    add_executable(mylite_embedded_server_surface_policy_test
      tests/embedded_server_surface_policy_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_server_surface_policy_test)

    add_executable(mylite_embedded_transactions_recovery_test
      tests/embedded_transactions_recovery_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_transactions_recovery_test)

    add_test(NAME libmylite.embedded-ddl-lifecycle COMMAND mylite_embedded_ddl_lifecycle_test)
    add_test(NAME libmylite.embedded-ddl-coverage COMMAND mylite_embedded_ddl_coverage_test)
    add_test(NAME libmylite.embedded-engine-schema COMMAND mylite_embedded_engine_schema_test)
    add_test(NAME libmylite.embedded-exec COMMAND mylite_embedded_exec_test)
    add_test(
      NAME libmylite.embedded-locking-concurrency
      COMMAND mylite_embedded_locking_concurrency_test
    )
    add_test(
      NAME libmylite.embedded-same-process-concurrency
      COMMAND mylite_embedded_same_process_concurrency_test
    )
    add_test(
      NAME libmylite.embedded-ownerless-mdl-hooks
      COMMAND mylite_embedded_ownerless_mdl_hooks_test
    )
    add_test(
      NAME libmylite.embedded-ownerless-trx-hooks
      COMMAND mylite_embedded_ownerless_trx_hooks_test
    )
    add_test(
      NAME libmylite.embedded-ownerless-innodb-lock-hooks
      COMMAND mylite_embedded_ownerless_innodb_lock_hooks_test
    )
    add_test(
      NAME libmylite.ownerless-primitives
      COMMAND mylite_ownerless_primitives_test
    )
    add_test(
      NAME libmylite.ownerless-cross-process-sql
      COMMAND mylite_ownerless_cross_process_sql_test
    )
    if(MYLITE_ENABLE_OWNERLESS_STRESS_TESTS)
      add_test(
        NAME libmylite.ownerless-cross-process-stress
        COMMAND mylite_ownerless_cross_process_sql_test stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-ddl-stress
        COMMAND mylite_ownerless_cross_process_sql_test ddl-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-temporary-stress
        COMMAND mylite_ownerless_cross_process_sql_test temp-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-transaction-stress
        COMMAND mylite_ownerless_cross_process_sql_test tx-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-checksum-stress
        COMMAND mylite_ownerless_cross_process_sql_test checksum-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-random-transaction-stress
        COMMAND mylite_ownerless_cross_process_sql_test random-tx-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-active-reader-pressure-stress
        COMMAND mylite_ownerless_cross_process_sql_test active-reader-pressure
      )
    endif()
    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      add_test(
        NAME libmylite.ownerless-negative-proof
        COMMAND mylite_ownerless_negative_proof_test
      )
    endif()
    add_test(
      NAME libmylite.compat.mariadb-comparison
      COMMAND mylite_embedded_compatibility_harness_test mariadb-comparison
    )
    add_test(
      NAME libmylite.compat.application-queries
      COMMAND mylite_embedded_compatibility_harness_test application-queries
    )
    add_test(
      NAME libmylite.embedded-native-table-operations
      COMMAND mylite_embedded_native_table_operations_test
    )
    add_test(
      NAME libmylite.embedded-memory-database
      COMMAND mylite_embedded_memory_database_test
    )
    add_test(
      NAME libmylite.embedded-prepared-statement
      COMMAND mylite_embedded_prepared_statement_test
    )
    add_test(
      NAME libmylite.embedded-server-surface-policy
      COMMAND mylite_embedded_server_surface_policy_test
    )
    add_test(NAME libmylite.embedded-open-close COMMAND mylite_embedded_open_close_test)
    add_test(NAME libmylite.embedded-storage COMMAND mylite_embedded_storage_test)
    add_test(
      NAME libmylite.embedded-transactions-recovery
      COMMAND mylite_embedded_transactions_recovery_test
    )

    set_tests_properties(libmylite.embedded-open-close PROPERTIES
      LABELS "compat.lifecycle;compat.directory-boundary;compat.ownerless-innodb-lock"
    )
    set_tests_properties(libmylite.embedded-storage PROPERTIES
      LABELS "compat.lifecycle;compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-exec PROPERTIES
      LABELS "compat.lifecycle;compat.query"
    )
    set_tests_properties(libmylite.embedded-ddl-lifecycle PROPERTIES
      LABELS "compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-ddl-coverage PROPERTIES
      LABELS "compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-engine-schema PROPERTIES
      LABELS "compat.engine;compat.application-query;compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-locking-concurrency PROPERTIES
      LABELS "compat.directory-boundary;compat.crash-reopen"
    )
    set_tests_properties(libmylite.embedded-same-process-concurrency PROPERTIES
      LABELS "compat.concurrency;compat.query"
    )
    set_tests_properties(libmylite.embedded-ownerless-mdl-hooks PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-mdl"
    )
    set_tests_properties(libmylite.embedded-ownerless-trx-hooks PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-transaction"
    )
    set_tests_properties(libmylite.embedded-ownerless-innodb-lock-hooks PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-innodb-lock"
    )
    set_tests_properties(libmylite.ownerless-primitives PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-primitives;compat.platform"
    )
    set_tests_properties(libmylite.ownerless-cross-process-sql PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 600
    )
    if(MYLITE_ENABLE_OWNERLESS_STRESS_TESTS)
      set_tests_properties(libmylite.ownerless-cross-process-stress PROPERTIES
        ENVIRONMENT
          "MYLITE_OWNERLESS_STRESS_ITERATIONS=200;MYLITE_OWNERLESS_STRESS_READER_POLLS=400"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-ddl-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=8"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-temporary-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_TEMP_STRESS_ROUNDS=40"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-transaction-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_TX_STRESS_ROUNDS=80"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-checksum-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=160"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-random-transaction-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=120"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-active-reader-pressure-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS=48"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
    endif()
    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      set_tests_properties(libmylite.ownerless-negative-proof PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-negative-proof"
      )
    endif()
    set_tests_properties(libmylite.embedded-native-table-operations PROPERTIES
      LABELS "compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-memory-database PROPERTIES
      LABELS "compat.lifecycle;compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-prepared-statement PROPERTIES
      LABELS "compat.query"
    )
    set_tests_properties(libmylite.embedded-server-surface-policy PROPERTIES
      LABELS "compat.server-surface;compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-transactions-recovery PROPERTIES
      LABELS "compat.directory-boundary;compat.crash-reopen;compat.query"
    )
    set_tests_properties(libmylite.compat.mariadb-comparison PROPERTIES
      LABELS "compat.mariadb-comparison;compat.query"
    )
    set_tests_properties(libmylite.compat.application-queries PROPERTIES
      LABELS "compat.application-query;compat.directory-boundary;compat.query"
    )
  endif()
endif()
