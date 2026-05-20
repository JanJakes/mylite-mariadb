configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/mylite/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/mylite/version.h"
  @ONLY
)

add_library(mylite STATIC
  src/database.cc
  src/version.c
)
add_library(MyLite::mylite ALIAS mylite)

target_include_directories(mylite
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)

mylite_configure_c_target(mylite)
mylite_configure_cxx_target(mylite)
mylite_link_mariadb_embedded(mylite)

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

    add_executable(mylite_embedded_exec_test
      tests/embedded_exec_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_exec_test)

    add_executable(mylite_embedded_storage_test
      tests/embedded_storage_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_storage_test)

    add_executable(mylite_embedded_ddl_lifecycle_test
      tests/embedded_ddl_lifecycle_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_ddl_lifecycle_test)

    add_executable(mylite_embedded_locking_concurrency_test
      tests/embedded_locking_concurrency_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_locking_concurrency_test)

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

    add_executable(mylite_embedded_transactions_recovery_test
      tests/embedded_transactions_recovery_test.c
    )
    mylite_configure_embedded_c_test(mylite_embedded_transactions_recovery_test)

    add_test(NAME libmylite.embedded-ddl-lifecycle COMMAND mylite_embedded_ddl_lifecycle_test)
    add_test(NAME libmylite.embedded-exec COMMAND mylite_embedded_exec_test)
    add_test(
      NAME libmylite.embedded-locking-concurrency
      COMMAND mylite_embedded_locking_concurrency_test
    )
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
      NAME libmylite.embedded-prepared-statement
      COMMAND mylite_embedded_prepared_statement_test
    )
    add_test(NAME libmylite.embedded-open-close COMMAND mylite_embedded_open_close_test)
    add_test(NAME libmylite.embedded-storage COMMAND mylite_embedded_storage_test)
    add_test(
      NAME libmylite.embedded-transactions-recovery
      COMMAND mylite_embedded_transactions_recovery_test
    )

    set_tests_properties(libmylite.embedded-open-close PROPERTIES
      LABELS "compat.lifecycle;compat.directory-boundary"
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
    set_tests_properties(libmylite.embedded-locking-concurrency PROPERTIES
      LABELS "compat.directory-boundary;compat.crash-reopen"
    )
    set_tests_properties(libmylite.embedded-native-table-operations PROPERTIES
      LABELS "compat.directory-boundary;compat.query"
    )
    set_tests_properties(libmylite.embedded-prepared-statement PROPERTIES
      LABELS "compat.query"
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
