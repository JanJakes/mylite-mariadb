configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/mylite/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/include/mylite/version.h"
  @ONLY
)

add_library(mylite STATIC
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

if(BUILD_TESTING)
  add_executable(mylite_version_test
    tests/version_test.c
  )
  target_link_libraries(mylite_version_test PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_version_test)

  add_test(NAME libmylite.version COMMAND mylite_version_test)
endif()
