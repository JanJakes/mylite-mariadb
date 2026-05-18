find_program(PHP_CONFIG_EXECUTABLE NAMES php-config REQUIRED)
find_program(PHP_EXECUTABLE NAMES php REQUIRED)

execute_process(
  COMMAND "${PHP_CONFIG_EXECUTABLE}" --includes
  OUTPUT_VARIABLE php_config_includes
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
separate_arguments(php_config_includes NATIVE_COMMAND "${php_config_includes}")

set(php_include_dirs)
foreach(include_arg IN LISTS php_config_includes)
  if(include_arg MATCHES "^-I(.+)$")
    list(APPEND php_include_dirs "${CMAKE_MATCH_1}")
  endif()
endforeach()

if(NOT php_include_dirs)
  message(FATAL_ERROR "php-config did not report PHP include directories")
endif()

add_library(mylite_mysqli MODULE
  src/mylite_mysqli.c
)

target_include_directories(mylite_mysqli
  SYSTEM PRIVATE ${php_include_dirs}
)
target_link_libraries(mylite_mysqli PRIVATE MyLite::mylite)
target_compile_definitions(mylite_mysqli PRIVATE ZEND_COMPILE_DL_EXT=1)
mylite_configure_c_target(mylite_mysqli)
if(NOT MSVC)
  target_compile_options(mylite_mysqli PRIVATE -Wno-unused-parameter)
endif()

set_target_properties(mylite_mysqli PROPERTIES
  PREFIX ""
  OUTPUT_NAME "mylite_mysqli"
  SUFFIX ".so"
  LINKER_LANGUAGE CXX
)

if(APPLE)
  target_link_options(mylite_mysqli PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()

if(BUILD_TESTING AND MYLITE_WITH_MARIADB_EMBEDDED)
  add_test(
    NAME php-ext-mylite-mysqli.smoke
    COMMAND
      "${PHP_EXECUTABLE}"
      -n
      -d "extension=$<TARGET_FILE:mylite_mysqli>"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.php"
  )
  set_tests_properties(
    php-ext-mylite-mysqli.smoke
    PROPERTIES
      LABELS "compat-php;compat-mysqli;compat-wordpress"
      SKIP_RETURN_CODE 77
  )
endif()
