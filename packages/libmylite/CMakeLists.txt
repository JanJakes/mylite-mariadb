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
target_link_libraries(mylite PRIVATE MyLite::storage)
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
  set_tests_properties(
    libmylite.api
    PROPERTIES LABELS "compat-public-api"
  )
  set_tests_properties(
    libmylite.version
    PROPERTIES LABELS "compat-public-api"
  )

  if(MYLITE_WITH_MARIADB_EMBEDDED)
    add_executable(mylite_embedded_open_close_test
      tests/embedded_open_close_test.c
    )
    target_include_directories(
      mylite_embedded_open_close_test
      PRIVATE "${PROJECT_SOURCE_DIR}/packages/mylite-storage/src"
    )
    target_link_libraries(mylite_embedded_open_close_test PRIVATE MyLite::mylite MyLite::storage)
    mylite_configure_c_target(mylite_embedded_open_close_test)

    add_executable(mylite_embedded_exec_test
      tests/embedded_exec_test.c
    )
    target_link_libraries(mylite_embedded_exec_test PRIVATE MyLite::mylite)
    mylite_configure_c_target(mylite_embedded_exec_test)

    add_executable(mylite_embedded_statement_test
      tests/embedded_statement_test.c
    )
    target_link_libraries(mylite_embedded_statement_test PRIVATE MyLite::mylite)
    mylite_configure_c_target(mylite_embedded_statement_test)

    add_executable(mylite_embedded_warning_test
      tests/embedded_warning_test.c
    )
    target_link_libraries(mylite_embedded_warning_test PRIVATE MyLite::mylite)
    mylite_configure_c_target(mylite_embedded_warning_test)

    add_executable(mylite_embedded_comparison_test
      tests/embedded_comparison_test.cc
    )
    target_link_libraries(mylite_embedded_comparison_test PRIVATE MyLite::mylite)
    mylite_configure_cxx_target(mylite_embedded_comparison_test)
    mylite_link_mariadb_embedded(mylite_embedded_comparison_test)

    add_test(NAME libmylite.embedded-exec COMMAND mylite_embedded_exec_test)
    set_tests_properties(
      libmylite.embedded-exec
      PROPERTIES LABELS "compat-direct-sql;compat-embedded-lifecycle;compat-server-surface;compat-transaction;compat-locking;compat-foreign-key"
    )
    add_test(NAME libmylite.embedded-statement COMMAND mylite_embedded_statement_test)
    set_tests_properties(
      libmylite.embedded-statement
      PROPERTIES LABELS "compat-prepared-statement;compat-column-metadata;compat-large-value;compat-embedded-lifecycle;compat-public-api;compat-server-surface;compat-transaction;compat-locking;compat-foreign-key"
    )
    add_test(NAME libmylite.embedded-warning COMMAND mylite_embedded_warning_test)
    set_tests_properties(
      libmylite.embedded-warning
      PROPERTIES LABELS "compat-warning;compat-direct-sql;compat-prepared-statement;compat-public-api"
    )
    add_test(NAME libmylite.embedded-comparison COMMAND mylite_embedded_comparison_test)
    set_tests_properties(
      libmylite.embedded-comparison
      PROPERTIES LABELS
        "compat-sql-comparison;compat-direct-sql;compat-prepared-statement;compat-column-metadata;compat-warning"
    )

    if(MYLITE_MARIADB_HAS_MYLITE_SE)
      add_executable(mylite_embedded_storage_engine_test
        tests/embedded_storage_engine_test.c
      )
      target_link_libraries(
        mylite_embedded_storage_engine_test
        PRIVATE MyLite::mylite MyLite::storage
      )
      target_compile_definitions(
        mylite_embedded_storage_engine_test
        PRIVATE MYLITE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
      )
      mylite_configure_c_target(mylite_embedded_storage_engine_test)

      add_test(
        NAME libmylite.embedded-storage-engine
        COMMAND mylite_embedded_storage_engine_test
      )
      set_tests_properties(
        libmylite.embedded-storage-engine
        PROPERTIES LABELS
          "compat-storage-engine;compat-sidecar;compat-routed-ddl-dml;compat-application-schema;compat-server-surface;compat-transaction;compat-transaction-hooks;compat-statement-rollback;compat-locking;compat-foreign-key;compat-check-constraint;compat-generated-column;compat-unsupported-index"
      )
    endif()

    add_test(NAME libmylite.embedded-open-close COMMAND mylite_embedded_open_close_test)
    set_tests_properties(
      libmylite.embedded-open-close
      PROPERTIES LABELS "compat-embedded-lifecycle;compat-locking"
    )
  endif()
endif()
