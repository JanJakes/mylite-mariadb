add_library(mylite_php_extension MODULE
  src/php_mylite.c
)

target_link_libraries(mylite_php_extension PRIVATE MyLite::mylite)
mylite_configure_php_extension(mylite_php_extension)
mylite_configure_c_target(mylite_php_extension)
set_target_properties(mylite_php_extension PROPERTIES
  LINKER_LANGUAGE CXX
  OUTPUT_NAME mylite
)

if(BUILD_TESTING)
  mylite_add_php_test(
    php-ext-mylite.core-api
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/core_api_test.php"
  )
endif()
