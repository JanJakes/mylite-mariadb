set(MYLITE_PHP_CONFIG_EXECUTABLE
  "php-config"
  CACHE FILEPATH
  "php-config executable used to build MyLite PHP extensions"
)
set(MYLITE_PHP_EXECUTABLE
  "php"
  CACHE FILEPATH
  "PHP CLI executable used to test MyLite PHP extensions"
)

function(mylite_configure_php)
  if(DEFINED MYLITE_PHP_INCLUDE_DIRS)
    return()
  endif()

  find_program(MYLITE_RESOLVED_PHP_CONFIG_EXECUTABLE
    NAMES "${MYLITE_PHP_CONFIG_EXECUTABLE}"
    REQUIRED
  )
  find_program(MYLITE_RESOLVED_PHP_EXECUTABLE
    NAMES "${MYLITE_PHP_EXECUTABLE}"
    REQUIRED
  )

  execute_process(
    COMMAND "${MYLITE_RESOLVED_PHP_CONFIG_EXECUTABLE}" --includes
    OUTPUT_VARIABLE mylite_php_include_flags
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(
    COMMAND "${MYLITE_RESOLVED_PHP_CONFIG_EXECUTABLE}" --version
    OUTPUT_VARIABLE mylite_php_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )

  separate_arguments(mylite_php_include_flags NATIVE_COMMAND "${mylite_php_include_flags}")
  set(mylite_php_include_dirs)
  foreach(flag IN LISTS mylite_php_include_flags)
    if(flag MATCHES "^-I(.+)$")
      list(APPEND mylite_php_include_dirs "${CMAKE_MATCH_1}")
    endif()
  endforeach()

  if(NOT mylite_php_include_dirs)
    message(FATAL_ERROR "php-config --includes did not return PHP include directories")
  endif()

  set(MYLITE_PHP_INCLUDE_DIRS
    "${mylite_php_include_dirs}"
    CACHE INTERNAL
    "PHP include directories"
  )
  set(MYLITE_PHP_CLI
    "${MYLITE_RESOLVED_PHP_EXECUTABLE}"
    CACHE INTERNAL
    "Resolved PHP CLI executable"
  )
  set(MYLITE_PHP_VERSION
    "${mylite_php_version}"
    CACHE INTERNAL
    "Resolved PHP version"
  )

  message(STATUS "Building MyLite PHP extensions for PHP ${MYLITE_PHP_VERSION}")
endfunction()

function(mylite_configure_php_extension target)
  mylite_configure_php()

  target_include_directories("${target}" SYSTEM PRIVATE ${MYLITE_PHP_INCLUDE_DIRS})
  target_compile_definitions("${target}" PRIVATE ZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
  set_target_properties("${target}" PROPERTIES
    C_VISIBILITY_PRESET hidden
    PREFIX ""
    SUFFIX ".so"
  )

  if(APPLE)
    target_link_options("${target}" PRIVATE "LINKER:-undefined,dynamic_lookup")
  endif()
endfunction()

function(mylite_add_php_test test_name script)
  mylite_configure_php()
  add_test(
    NAME "${test_name}"
    COMMAND "${MYLITE_PHP_CLI}"
      -d "extension=$<TARGET_FILE:mylite_php_extension>"
      ${ARGN}
      "${script}"
  )
  set_tests_properties("${test_name}" PROPERTIES
    LABELS "php;compat.integration"
  )
endfunction()
