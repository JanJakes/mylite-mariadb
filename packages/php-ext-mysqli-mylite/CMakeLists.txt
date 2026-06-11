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
  mylite_add_php_test(
    php-ext-mysqli-mylite.profile
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mysqli_profile_test.php"
    -d "extension=$<TARGET_FILE:mylite_mysqli_php_extension>"
  )
  set_tests_properties(php-ext-mysqli-mylite.profile PROPERTIES
    ENVIRONMENT "MYLITE_MYSQLI_PROFILE=1"
    PASS_REGULAR_EXPRESSION "mylite_mysqli_profile_query_cache_hits=4"
  )
endif()
