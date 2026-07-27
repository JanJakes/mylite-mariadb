add_library(mylite_pdo_php_extension MODULE
  src/php_pdo_mylite.c
)

add_dependencies(mylite_pdo_php_extension mylite_php_extension)
target_include_directories(mylite_pdo_php_extension PRIVATE
  "${PROJECT_SOURCE_DIR}/packages/libmylite/include"
  "${PROJECT_BINARY_DIR}/packages/libmylite/include"
)
mylite_configure_php_extension(mylite_pdo_php_extension)
mylite_configure_c_target(mylite_pdo_php_extension)
set_target_properties(mylite_pdo_php_extension PROPERTIES
  OUTPUT_NAME pdo_mylite
)

if(BUILD_TESTING)
  mylite_add_php_test(
    php-ext-pdo-mylite.api
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/pdo_api_test.php"
    -d "extension=$<TARGET_FILE:mylite_pdo_php_extension>"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    mylite_add_php_test(
      php-ext-pdo-mylite.ownerless
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/pdo_ownerless_test.php"
      -d "extension=$<TARGET_FILE:mylite_pdo_php_extension>"
    )
    set_tests_properties(php-ext-pdo-mylite.ownerless PROPERTIES
      ENVIRONMENT
        "MYLITE_PHP_CORE_EXTENSION=$<TARGET_FILE:mylite_php_extension>;MYLITE_PHP_PDO_EXTENSION=$<TARGET_FILE:mylite_pdo_php_extension>"
      LABELS "php;compat.integration;compat.ownerless-application"
      TIMEOUT 120
    )
  endif()
endif()
