add_library(mylite_mysqli_php_extension MODULE
  src/php_mysqli_mylite.c
)

add_dependencies(mylite_mysqli_php_extension mylite_php_extension)
target_include_directories(mylite_mysqli_php_extension PRIVATE
  "${PROJECT_SOURCE_DIR}/packages/libmylite/include"
  "${PROJECT_BINARY_DIR}/packages/libmylite/include"
)
mylite_configure_php_extension(mylite_mysqli_php_extension)
mylite_configure_c_target(mylite_mysqli_php_extension)
set_target_properties(mylite_mysqli_php_extension PROPERTIES
  OUTPUT_NAME mysqli_mylite
)

if(BUILD_TESTING)
  mylite_add_php_test(
    php-ext-mysqli-mylite.api
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mysqli_api_test.php"
    -d "extension=$<TARGET_FILE:mylite_mysqli_php_extension>"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    mylite_add_php_test(
      php-ext-mysqli-mylite.ownerless
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/mysqli_ownerless_test.php"
      -d "extension=$<TARGET_FILE:mylite_mysqli_php_extension>"
    )
    set_tests_properties(php-ext-mysqli-mylite.ownerless PROPERTIES
      ENVIRONMENT
        "MYLITE_PHP_CORE_EXTENSION=$<TARGET_FILE:mylite_php_extension>;MYLITE_PHP_MYSQLI_EXTENSION=$<TARGET_FILE:mylite_mysqli_php_extension>"
      LABELS "php;compat.integration;compat.ownerless-application"
      TIMEOUT 120
    )
  endif()
  mylite_add_php_test(
    php-ext-mysqli-mylite.profile
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mysqli_profile_test.php"
    -d "extension=$<TARGET_FILE:mylite_mysqli_php_extension>"
  )
  set_tests_properties(php-ext-mysqli-mylite.profile PROPERTIES
    ENVIRONMENT "MYLITE_MYSQLI_PROFILE=1"
    PASS_REGULAR_EXPRESSION
      "mylite_mysqli_profile_embedded_open_open_calls=[1-9][0-9]*;mylite_mysqli_profile_embedded_open_start_runtime_calls=[1-9][0-9]*;mylite_mysqli_profile_embedded_open_release_runtime_calls=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_calls=[1-9][0-9]*;mylite_mysqli_profile_query_verb_dml_calls=[1-9][0-9]*;mylite_mysqli_profile_query_shape_top_count=[1-9][0-9]*;mylite_mysqli_profile_query_shape_1_calls=[1-9][0-9]*;mylite_mysqli_profile_query_cache_hits=[1-9][0-9]*;mylite_mysqli_profile_query_cache_preserved_no_result_calls=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_native_control_calls=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_native_control_start_transaction_calls=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_native_control_commit_calls=[1-9][0-9]*;mylite_mysqli_profile_libmylite_exec_result_native_control_rollback_calls=[1-9][0-9]*;mylite_mysqli_profile_query_transaction_start_calls=[1-9][0-9]*;mylite_mysqli_profile_query_transaction_end_calls=[1-9][0-9]*;mylite_mysqli_profile_query_transaction_savepoint_calls=[1-9][0-9]*"
  )
  mylite_add_php_test(
    php-ext-mysqli-mylite.profile-context
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mysqli_profile_test.php"
    -d "extension=$<TARGET_FILE:mylite_mysqli_php_extension>"
  )
  set_tests_properties(php-ext-mysqli-mylite.profile-context PROPERTIES
    ENVIRONMENT "MYLITE_MYSQLI_PROFILE=1;MYLITE_MYSQLI_PROFILE_CONTEXT=profile-context"
    PASS_REGULAR_EXPRESSION "mylite_mysqli_profile_context=profile-context"
  )
endif()
