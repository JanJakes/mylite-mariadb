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
  mylite_enable_c_test_assertions(mylite_api_test)

  add_executable(mylite_version_test
    tests/version_test.c
  )
  target_link_libraries(mylite_version_test PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_version_test)
  mylite_enable_c_test_assertions(mylite_version_test)

  add_test(NAME libmylite.api COMMAND mylite_api_test)
  add_test(NAME libmylite.version COMMAND mylite_version_test)

  if(MYLITE_WITH_MARIADB_EMBEDDED)
    function(mylite_configure_embedded_c_test target)
      target_link_libraries(${target} PRIVATE MyLite::mylite)
      mylite_configure_c_target(${target})
      mylite_enable_c_test_assertions(${target})
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
      "${PROJECT_SOURCE_DIR}/mariadb/sql"
      "${PROJECT_SOURCE_DIR}/mariadb/storage/innobase/include"
    )

    add_executable(mylite_embedded_performance_probe
      tests/embedded_performance_probe.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_performance_probe)

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
      "${PROJECT_SOURCE_DIR}/mariadb/storage/innobase/include"
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
    set(MYLITE_OWNERLESS_SQL_SHARD_COUNT 16)
    math(EXPR MYLITE_OWNERLESS_SQL_LAST_SHARD_INDEX
      "${MYLITE_OWNERLESS_SQL_SHARD_COUNT} - 1"
    )
    foreach(MYLITE_OWNERLESS_SQL_SHARD_INDEX RANGE 0 ${MYLITE_OWNERLESS_SQL_LAST_SHARD_INDEX})
      add_test(
        NAME libmylite.ownerless-cross-process-sql.${MYLITE_OWNERLESS_SQL_SHARD_INDEX}
        COMMAND
          mylite_ownerless_cross_process_sql_test
          sql-weighted-shard
          ${MYLITE_OWNERLESS_SQL_SHARD_INDEX}
          ${MYLITE_OWNERLESS_SQL_SHARD_COUNT}
      )
    endforeach()
    add_test(
      NAME libmylite.ownerless-single-owner-foreground-reclaim-budget
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-foreground-reclaim-budget
    )
    add_test(
      NAME libmylite.ownerless-single-owner-page-write-refresh-skip
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-page-write-refresh-skip
    )
    add_test(
      NAME libmylite.ownerless-single-owner-external-refresh-skip
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-external-refresh-skip
    )
    add_test(
      NAME libmylite.ownerless-single-owner-history-wal-proof
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-history-wal-proof
    )
    add_test(
      NAME libmylite.ownerless-single-owner-native-support-page-wal-elision
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-native-support-page-wal-elision
    )
    add_test(
      NAME libmylite.ownerless-explicit-transaction-update-history-proof
      COMMAND mylite_ownerless_cross_process_sql_test explicit-transaction-update-history-proof
    )
    add_test(
      NAME libmylite.ownerless-explicit-transaction-delete-history-proof
      COMMAND mylite_ownerless_cross_process_sql_test explicit-transaction-delete-history-proof
    )
    add_test(
      NAME libmylite.ownerless-explicit-transaction-replace-history-proof
      COMMAND mylite_ownerless_cross_process_sql_test explicit-transaction-replace-history-proof
    )
    add_test(
      NAME libmylite.ownerless-explicit-transaction-mixed-dml-history-proof
      COMMAND mylite_ownerless_cross_process_sql_test explicit-transaction-mixed-dml-history-proof
    )
    add_test(
      NAME libmylite.ownerless-explicit-transaction-order-limit-history-proof
      COMMAND mylite_ownerless_cross_process_sql_test explicit-transaction-order-limit-history-proof
    )
    add_test(
      NAME libmylite.ownerless-explicit-dml-savepoint-marker-discard
      COMMAND
        mylite_ownerless_cross_process_sql_test
        native-explicit-dml-savepoint-file-op-marker-discard
    )
    add_test(
      NAME libmylite.ownerless-explicit-dml-savepoint-marker-retain
      COMMAND
        mylite_ownerless_cross_process_sql_test
        native-explicit-dml-savepoint-file-op-marker-retain
    )
    add_test(
      NAME libmylite.ownerless-concurrent-savepoint-rollback-handoff
      COMMAND
        mylite_ownerless_cross_process_sql_test
        concurrent-savepoint-rollback-handoff
    )
    add_test(
      NAME libmylite.ownerless-concurrent-savepoint-same-page-handoff
      COMMAND
        mylite_ownerless_cross_process_sql_test
        concurrent-savepoint-same-page-handoff
    )
    add_test(
      NAME libmylite.ownerless-killed-savepoint-dml-marker-recovery
      COMMAND
        mylite_ownerless_cross_process_sql_test
        native-killed-savepoint-dml-file-op-marker-recovery
    )
    add_test(
      NAME libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-multi-row-insert-visible-fast-path
    )
    add_test(
      NAME libmylite.ownerless-no-live-native-checkpoint-cutover-proof
      COMMAND mylite_ownerless_cross_process_sql_test no-live-native-checkpoint-cutover-proof
    )
    add_test(
      NAME libmylite.ownerless-no-live-pressure-reclaim-proof
      COMMAND mylite_ownerless_cross_process_sql_test no-live-pressure-reclaim
    )
    add_test(
      NAME libmylite.ownerless-insert-fk-fast-path-cache
      COMMAND mylite_ownerless_cross_process_sql_test insert-fk-fast-path-cache
    )
    add_test(
      NAME libmylite.ownerless-cyclic-foreign-key-truncate-policy
      COMMAND mylite_ownerless_cross_process_sql_test cyclic-foreign-key-truncate-policy
    )
    add_test(
      NAME libmylite.ownerless-temporary-table-rename-tracking
      COMMAND mylite_ownerless_cross_process_sql_test temporary-table-rename-tracking
    )
    add_test(
      NAME libmylite.ownerless-uncommitted-peer-hidden
      COMMAND mylite_ownerless_cross_process_sql_test uncommitted-peer-hidden
    )
    add_test(
      NAME libmylite.ownerless-single-owner-foreground-reclaim-peer-history
      COMMAND mylite_ownerless_cross_process_sql_test single-owner-foreground-reclaim-peer-history
    )
    add_test(
      NAME libmylite.ownerless-random-transaction-rollback-handoff
      COMMAND
        ${CMAKE_COMMAND}
        -E
        env
        MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=3
        $<TARGET_FILE:mylite_ownerless_cross_process_sql_test>
        random-tx-stress
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
        NAME libmylite.ownerless-cross-process-ddl-stress-statement-lock-retry
        COMMAND
          ${CMAKE_COMMAND}
          -E
          env
          MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=2
          MYLITE_OWNERLESS_DDL_STRESS_LOCK_WAIT_TIMEOUT=0
          $<TARGET_FILE:mylite_ownerless_cross_process_sql_test>
          ddl-stress
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
        NAME libmylite.ownerless-cross-process-foreign-key-graph-stress
        COMMAND mylite_ownerless_cross_process_sql_test fk-graph-stress
      )
      add_test(
        NAME libmylite.ownerless-cross-process-child-failure-cleanup
        COMMAND mylite_ownerless_cross_process_sql_test child-failure-cleanup
      )
      add_test(
        NAME libmylite.ownerless-cross-process-active-reader-pressure-stress
        COMMAND mylite_ownerless_cross_process_sql_test active-reader-pressure
      )
      add_test(
        NAME libmylite.ownerless-cross-process-expanding-page-pressure-stress
        COMMAND mylite_ownerless_cross_process_sql_test expanding-page-pressure
      )
      add_test(
        NAME libmylite.ownerless-cross-process-blob-page-pressure-stress
        COMMAND mylite_ownerless_cross_process_sql_test blob-page-pressure
      )
      add_test(
        NAME libmylite.ownerless-cross-process-compressed-blob-page-pressure-stress
        COMMAND mylite_ownerless_cross_process_sql_test compressed-blob-page-pressure
      )
    endif()
    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      add_test(
        NAME libmylite.ownerless-negative-proof
        COMMAND mylite_ownerless_negative_proof_test
      )
      add_test(
        NAME libmylite.ownerless-table-wait-negative-proof
        COMMAND mylite_ownerless_cross_process_sql_test table-lock-wait-negative-proof
      )
      add_test(
        NAME libmylite.ownerless-native-table-wait
        COMMAND mylite_ownerless_cross_process_sql_test native-table-wait
      )
      add_test(
        NAME libmylite.ownerless-native-table-wait-crash
        COMMAND mylite_ownerless_cross_process_sql_test native-table-wait-crash
      )
      add_test(
        NAME libmylite.ownerless-native-file-modify-redo-observation
        COMMAND
          mylite_ownerless_cross_process_sql_test
          native-file-modify-redo-observation
      )
      add_test(
        NAME libmylite.ownerless-savepoint-rollback-before-state-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          savepoint-rollback-before-state-crash
      )
      add_test(
        NAME libmylite.ownerless-savepoint-rollback-prewrite-before-state-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          savepoint-rollback-prewrite-before-state-crash
      )
      add_test(
        NAME libmylite.ownerless-transaction-rollback-before-state-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          transaction-rollback-before-state-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-drop-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-alter-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-alter-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-multi-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-multi-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-mixed-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-mixed-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-mixed-rename-reverse-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-mixed-rename-reverse-crash
      )
      add_test(
        NAME libmylite.ownerless-temporary-mixed-chain-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test temporary-mixed-chain-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-stale-drop-crash-recovery
        COMMAND mylite_ownerless_cross_process_sql_test stale-drop-crash-recovery
      )
      add_test(
        NAME libmylite.ownerless-dictionary-create-table-live-recovery
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-create-table-live-recovery
      )
      add_test(
        NAME libmylite.ownerless-dictionary-rename-file-op-marker-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-rename-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-implicit-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-implicit-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-alter-table-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-alter-table-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-create-like-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-create-like-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-ctas-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-ctas-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-create-or-replace-like-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-create-or-replace-like-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-create-or-replace-ctas-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-create-or-replace-ctas-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-force-rebuild-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-force-rebuild-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-force-rebuild-plain-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-force-rebuild-plain-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-engine-rebuild-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-engine-rebuild-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-engine-rebuild-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-engine-rebuild-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-charset-convert-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-charset-convert-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-charset-convert-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-charset-convert-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-row-format-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-row-format-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-row-format-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-row-format-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-key-block-1-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-key-block-1-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-key-block-2-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-key-block-2-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-key-block-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-key-block-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-key-block-16-file-op-marker-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-key-block-16-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-compressed-row-format-key-block-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-compressed-row-format-key-block-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-cross-schema-multi-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-cross-schema-multi-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-multi-add-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-multi-add-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-multi-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-multi-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-mixed-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-mixed-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-mixed-comment-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-mixed-comment-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-mixed-default-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-mixed-default-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-mixed-column-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-mixed-column-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-fk-multi-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-multi-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-fk-cross-schema-multi-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-cross-schema-multi-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-table-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-table-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-add-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-add-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-modify-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-modify-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-rename-expression-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-rename-expression-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-change-expression-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-change-expression-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-default-set-expression-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-default-set-expression-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-default-drop-expression-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-default-drop-expression-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-change-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-change-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-default-set-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-default-set-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-idempotent-default-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-idempotent-default-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-index-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-index-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-secondary-index-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-secondary-index-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-secondary-index-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-secondary-index-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-index-idempotent-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-index-idempotent-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-alter-index-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-alter-index-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-alter-index-idempotent-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-alter-index-idempotent-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-unique-index-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-unique-index-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-alter-unique-index-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-alter-unique-index-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-unique-index-replace-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-unique-index-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-unique-index-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-unique-index-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-secondary-index-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-secondary-index-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-secondary-index-ignorability-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-secondary-index-ignorability-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-primary-key-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-primary-key-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-descending-primary-key-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-descending-primary-key-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-composite-direction-primary-key-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-composite-direction-primary-key-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-primary-key-autoinc-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-primary-key-autoinc-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-primary-key-autoinc-descending-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-primary-key-autoinc-descending-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-primary-key-idempotent-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-primary-key-idempotent-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-add-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-add-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-add-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-add-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-drop-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-drop-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-modify-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-modify-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-modify-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-modify-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-change-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-change-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-change-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-change-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-rename-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-rename-copy-lock-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-rename-copy-lock-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-rename-expression-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-rename-expression-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-column-default-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-column-default-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-table-comment-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-table-comment-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-create-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-view-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-drop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-view-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-replace-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-view-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-alter-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-view-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-rename-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-view-rename-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-column-list-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-column-list-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-column-list-replace-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-column-list-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-column-list-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-column-list-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-check-option-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-check-option-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-check-option-replace-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-check-option-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-check-option-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-check-option-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-nested-check-option-outer-replace-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-nested-check-option-outer-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-nested-check-option-inner-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-nested-check-option-inner-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-security-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-security-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-security-replace-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-security-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-security-alter-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-security-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-idempotent-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-idempotent-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-view-invalid-dependency-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-view-invalid-dependency-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-generated-column-foreign-key-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-generated-column-foreign-key-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-generated-column-foreign-key-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-generated-column-foreign-key-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-trigger-definer-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-trigger-definer-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-trigger-order-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-trigger-order-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-trigger-replace-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-trigger-replace-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-trigger-idempotent-create-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-trigger-idempotent-create-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-trigger-idempotent-drop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-trigger-idempotent-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-truncate-file-op-marker-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-truncate-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-implicit-truncate-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-implicit-truncate-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-child-truncate-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-child-truncate-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-foreign-key-truncate-variants-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-foreign-key-truncate-variants-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-unchecked-cyclic-foreign-key-truncate-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-unchecked-cyclic-foreign-key-truncate-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-drop-file-op-marker-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-drop-file-op-marker-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-implicit-drop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-implicit-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-multi-drop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-multi-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-cross-schema-multi-drop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-cross-schema-multi-drop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-multi-drop-loop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-multi-drop-loop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-cross-schema-multi-drop-loop-crash
        COMMAND
          mylite_ownerless_cross_process_sql_test
          dictionary-cross-schema-multi-drop-loop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-schema-drop-loop-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-schema-drop-loop-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-schema-comment-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-schema-comment-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-current-schema-comment-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-current-schema-comment-crash
      )
      add_test(
        NAME libmylite.ownerless-dictionary-current-schema-alter-crash
        COMMAND mylite_ownerless_cross_process_sql_test dictionary-current-schema-alter-crash
      )
      add_test(
        NAME libmylite.ownerless-platform-probe-failure
        COMMAND mylite_ownerless_cross_process_sql_test platform-probe-failure
      )
      add_test(
        NAME libmylite.ownerless-history-proof-publish-failure-fallback
        COMMAND mylite_ownerless_cross_process_sql_test history-proof-publish-failure-fallback
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
    add_test(
      NAME libmylite.embedded-open-close
      COMMAND mylite_embedded_open_close_test baseline
    )
    add_test(
      NAME libmylite.embedded-ownerless-directory-lifecycle
      COMMAND mylite_embedded_open_close_test ownerless-directory
    )
    add_test(
      NAME libmylite.embedded-ownerless-product-hooks
      COMMAND mylite_embedded_open_close_test ownerless-product-hooks
    )
    add_test(NAME libmylite.embedded-storage COMMAND mylite_embedded_storage_test)
    add_test(
      NAME libmylite.embedded-transactions-recovery
      COMMAND mylite_embedded_transactions_recovery_test
    )

    set_tests_properties(libmylite.embedded-open-close PROPERTIES
      LABELS "compat.lifecycle;compat.directory-boundary"
    )
    set_tests_properties(libmylite.embedded-ownerless-directory-lifecycle PROPERTIES
      LABELS "compat.concurrency;compat.directory-boundary;compat.ownerless-primitives;compat.ownerless-readonly"
    )
    set_tests_properties(libmylite.embedded-ownerless-product-hooks PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-transaction;compat.ownerless-innodb-lock"
      TIMEOUT 90
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
    foreach(MYLITE_OWNERLESS_SQL_SHARD_INDEX RANGE 0 ${MYLITE_OWNERLESS_SQL_LAST_SHARD_INDEX})
      set_tests_properties(
        libmylite.ownerless-cross-process-sql.${MYLITE_OWNERLESS_SQL_SHARD_INDEX}
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
          TIMEOUT 900
      )
    endforeach()
    # This weighted shard includes the TEXT/BLOB prefix-index DDL refresh case,
    # which is quick alone but can hit the per-case timeout under sustained
    # parallel ownerless SQL pressure.
    set_tests_properties(libmylite.ownerless-cross-process-sql.8 PROPERTIES
      RUN_SERIAL TRUE
    )
    # This weighted shard includes the FK cross-schema child-rename case, which
    # is quick alone but can hit the per-case timeout under sustained parallel
    # ownerless SQL pressure.
    set_tests_properties(libmylite.ownerless-cross-process-sql.11 PROPERTIES
      RUN_SERIAL TRUE
    )
    set_tests_properties(libmylite.ownerless-single-owner-foreground-reclaim-budget PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-page-write-refresh-skip PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-external-refresh-skip PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-history-wal-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-native-support-page-wal-elision PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-transaction-update-history-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-transaction-delete-history-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-transaction-replace-history-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-transaction-mixed-dml-history-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-transaction-order-limit-history-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-dml-savepoint-marker-discard PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-explicit-dml-savepoint-marker-retain PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-concurrent-savepoint-rollback-handoff PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-concurrent-savepoint-same-page-handoff PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-killed-savepoint-dml-marker-recovery PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-no-live-native-checkpoint-cutover-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-no-live-pressure-reclaim-proof PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-insert-fk-fast-path-cache PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-cyclic-foreign-key-truncate-policy PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-temporary-table-rename-tracking PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-uncommitted-peer-hidden PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-single-owner-foreground-reclaim-peer-history PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 90
    )
    set_tests_properties(libmylite.ownerless-random-transaction-rollback-handoff PROPERTIES
      LABELS "compat.concurrency;compat.ownerless-cross-process-sql"
      TIMEOUT 120
    )
    if(MYLITE_ENABLE_OWNERLESS_STRESS_TESTS)
      set_tests_properties(libmylite.ownerless-cross-process-stress PROPERTIES
        ENVIRONMENT
          "MYLITE_OWNERLESS_STRESS_ITERATIONS=200;MYLITE_OWNERLESS_STRESS_READER_POLLS=400"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-ddl-stress PROPERTIES
        ENVIRONMENT
          "MYLITE_OWNERLESS_DDL_STRESS_ROUNDS=8;MYLITE_OWNERLESS_DDL_STRESS_LOCK_WAIT_TIMEOUT=1"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-ddl-stress-statement-lock-retry PROPERTIES
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
        ENVIRONMENT "MYLITE_OWNERLESS_CHECKSUM_STRESS_ROUNDS=48"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-random-transaction-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_RANDOM_TX_STRESS_ROUNDS=120"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-foreign-key-graph-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_FK_GRAPH_STRESS_ROUNDS=48"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-child-failure-cleanup PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 30
      )
      set_tests_properties(libmylite.ownerless-cross-process-active-reader-pressure-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS=48"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-expanding-page-pressure-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_EXPANDING_PAGE_PRESSURE_ROWS=48"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-blob-page-pressure-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_BLOB_PAGE_PRESSURE_ROWS=12"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
      set_tests_properties(libmylite.ownerless-cross-process-compressed-blob-page-pressure-stress PROPERTIES
        ENVIRONMENT "MYLITE_OWNERLESS_COMPRESSED_BLOB_PAGE_PRESSURE_ROWS=8"
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-stress"
        TIMEOUT 900
      )
    endif()
    if(MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS)
      set_tests_properties(libmylite.ownerless-negative-proof PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-negative-proof"
      )
      set_tests_properties(libmylite.ownerless-table-wait-negative-proof PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-negative-proof"
      )
      set_tests_properties(libmylite.ownerless-native-table-wait PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-innodb-lock"
        TIMEOUT 90
      )
      set_tests_properties(libmylite.ownerless-native-table-wait-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-innodb-lock;compat.crash-reopen"
        TIMEOUT 90
      )
      set_tests_properties(libmylite.ownerless-native-file-modify-redo-observation PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 90
      )
      set_tests_properties(libmylite.ownerless-savepoint-rollback-before-state-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 90
      )
      set_tests_properties(
        libmylite.ownerless-savepoint-rollback-prewrite-before-state-crash
        PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 90
      )
      set_tests_properties(libmylite.ownerless-transaction-rollback-before-state-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 90
      )
      set_tests_properties(
        libmylite.ownerless-temporary-drop-crash
        libmylite.ownerless-temporary-rename-crash
        libmylite.ownerless-temporary-alter-rename-crash
        libmylite.ownerless-temporary-multi-rename-crash
        libmylite.ownerless-temporary-mixed-rename-crash
        libmylite.ownerless-temporary-mixed-rename-reverse-crash
        libmylite.ownerless-temporary-mixed-chain-rename-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-stale-drop-crash-recovery PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-create-table-live-recovery PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-rename-file-op-marker-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-implicit-rename-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-alter-table-rename-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-create-like-file-op-marker-crash
        libmylite.ownerless-dictionary-ctas-file-op-marker-crash
        libmylite.ownerless-dictionary-create-or-replace-like-file-op-marker-crash
        libmylite.ownerless-dictionary-create-or-replace-ctas-file-op-marker-crash
        libmylite.ownerless-dictionary-force-rebuild-file-op-marker-crash
        libmylite.ownerless-dictionary-force-rebuild-plain-crash
        libmylite.ownerless-dictionary-engine-rebuild-file-op-marker-crash
        libmylite.ownerless-dictionary-engine-rebuild-copy-lock-crash
        libmylite.ownerless-dictionary-charset-convert-crash
        libmylite.ownerless-dictionary-charset-convert-copy-lock-crash
        libmylite.ownerless-dictionary-row-format-file-op-marker-crash
        libmylite.ownerless-dictionary-row-format-copy-lock-crash
        libmylite.ownerless-dictionary-compressed-row-format-file-op-marker-crash
        libmylite.ownerless-dictionary-compressed-row-format-key-block-1-file-op-marker-crash
        libmylite.ownerless-dictionary-compressed-row-format-key-block-2-file-op-marker-crash
        libmylite.ownerless-dictionary-compressed-row-format-key-block-file-op-marker-crash
        libmylite.ownerless-dictionary-compressed-row-format-key-block-16-file-op-marker-crash
        libmylite.ownerless-dictionary-compressed-row-format-key-block-copy-lock-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-cross-schema-multi-rename-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-foreign-key-multi-add-crash
        libmylite.ownerless-dictionary-foreign-key-multi-drop-crash
        libmylite.ownerless-dictionary-foreign-key-mixed-alter-crash
        libmylite.ownerless-dictionary-foreign-key-mixed-comment-alter-crash
        libmylite.ownerless-dictionary-foreign-key-mixed-default-alter-crash
        libmylite.ownerless-dictionary-foreign-key-mixed-column-alter-crash
        libmylite.ownerless-dictionary-fk-multi-rename-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-fk-cross-schema-multi-rename-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-index-idempotent-create-crash
        libmylite.ownerless-dictionary-secondary-index-crash
        libmylite.ownerless-dictionary-secondary-index-drop-crash
        libmylite.ownerless-dictionary-index-idempotent-drop-crash
        libmylite.ownerless-dictionary-alter-index-idempotent-create-crash
        libmylite.ownerless-dictionary-alter-index-idempotent-drop-crash
        libmylite.ownerless-dictionary-unique-index-idempotent-create-crash
        libmylite.ownerless-dictionary-alter-unique-index-idempotent-create-crash
        libmylite.ownerless-dictionary-unique-index-replace-crash
        libmylite.ownerless-dictionary-unique-index-drop-crash
        libmylite.ownerless-dictionary-secondary-index-rename-crash
        libmylite.ownerless-dictionary-secondary-index-ignorability-crash
        libmylite.ownerless-dictionary-primary-key-crash
        libmylite.ownerless-dictionary-descending-primary-key-crash
        libmylite.ownerless-dictionary-composite-direction-primary-key-crash
        libmylite.ownerless-dictionary-primary-key-autoinc-crash
        libmylite.ownerless-dictionary-primary-key-autoinc-descending-crash
        libmylite.ownerless-dictionary-primary-key-idempotent-crash
        libmylite.ownerless-dictionary-column-add-crash
        libmylite.ownerless-dictionary-column-add-copy-lock-crash
        libmylite.ownerless-dictionary-column-drop-crash
        libmylite.ownerless-dictionary-column-drop-copy-lock-crash
        libmylite.ownerless-dictionary-column-modify-crash
        libmylite.ownerless-dictionary-column-modify-copy-lock-crash
        libmylite.ownerless-dictionary-column-change-crash
        libmylite.ownerless-dictionary-column-change-copy-lock-crash
        libmylite.ownerless-dictionary-column-rename-crash
        libmylite.ownerless-dictionary-column-rename-copy-lock-crash
        libmylite.ownerless-dictionary-column-rename-expression-crash
        libmylite.ownerless-dictionary-table-idempotent-create-crash
        libmylite.ownerless-dictionary-column-idempotent-add-crash
        libmylite.ownerless-dictionary-column-idempotent-drop-crash
        libmylite.ownerless-dictionary-column-idempotent-modify-crash
        libmylite.ownerless-dictionary-column-idempotent-rename-crash
        libmylite.ownerless-dictionary-column-idempotent-rename-expression-crash
        libmylite.ownerless-dictionary-column-idempotent-change-expression-crash
        libmylite.ownerless-dictionary-column-idempotent-default-set-expression-crash
        libmylite.ownerless-dictionary-column-idempotent-default-drop-expression-crash
        libmylite.ownerless-dictionary-column-idempotent-change-crash
        libmylite.ownerless-dictionary-column-idempotent-default-set-crash
        libmylite.ownerless-dictionary-column-idempotent-default-drop-crash
        libmylite.ownerless-dictionary-column-default-crash
        libmylite.ownerless-dictionary-table-comment-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-view-create-crash
        libmylite.ownerless-dictionary-view-drop-crash
        libmylite.ownerless-dictionary-view-replace-crash
        libmylite.ownerless-dictionary-view-alter-crash
        libmylite.ownerless-dictionary-view-rename-crash
        libmylite.ownerless-dictionary-view-column-list-create-crash
        libmylite.ownerless-dictionary-view-column-list-replace-crash
        libmylite.ownerless-dictionary-view-column-list-alter-crash
        libmylite.ownerless-dictionary-view-check-option-create-crash
        libmylite.ownerless-dictionary-view-check-option-replace-crash
        libmylite.ownerless-dictionary-view-check-option-alter-crash
        libmylite.ownerless-dictionary-view-nested-check-option-outer-replace-crash
        libmylite.ownerless-dictionary-view-nested-check-option-inner-alter-crash
        libmylite.ownerless-dictionary-view-security-create-crash
        libmylite.ownerless-dictionary-view-security-replace-crash
        libmylite.ownerless-dictionary-view-security-alter-crash
        libmylite.ownerless-dictionary-view-idempotent-create-crash
        libmylite.ownerless-dictionary-view-idempotent-drop-crash
        libmylite.ownerless-dictionary-view-invalid-dependency-drop-crash
        libmylite.ownerless-dictionary-trigger-definer-crash
        libmylite.ownerless-dictionary-trigger-order-crash
        libmylite.ownerless-dictionary-trigger-replace-crash
        libmylite.ownerless-dictionary-trigger-idempotent-create-crash
        libmylite.ownerless-dictionary-trigger-idempotent-drop-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-truncate-file-op-marker-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-implicit-truncate-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-foreign-key-child-truncate-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-foreign-key-truncate-variants-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-unchecked-cyclic-foreign-key-truncate-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-drop-file-op-marker-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-dictionary-implicit-drop-crash PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
        TIMEOUT 120
      )
      set_tests_properties(
        libmylite.ownerless-dictionary-multi-drop-crash
        libmylite.ownerless-dictionary-cross-schema-multi-drop-crash
        libmylite.ownerless-dictionary-multi-drop-loop-crash
        libmylite.ownerless-dictionary-cross-schema-multi-drop-loop-crash
        libmylite.ownerless-dictionary-schema-drop-loop-crash
        libmylite.ownerless-dictionary-schema-comment-crash
        libmylite.ownerless-dictionary-current-schema-comment-crash
        libmylite.ownerless-dictionary-current-schema-alter-crash
        PROPERTIES
          LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.crash-reopen"
          TIMEOUT 120
      )
      set_tests_properties(libmylite.ownerless-platform-probe-failure PROPERTIES
        LABELS "compat.concurrency;compat.platform;compat.ownerless-negative-proof"
      )
      set_tests_properties(libmylite.ownerless-history-proof-publish-failure-fallback PROPERTIES
        LABELS "compat.concurrency;compat.ownerless-cross-process-sql;compat.ownerless-negative-proof"
        TIMEOUT 90
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
