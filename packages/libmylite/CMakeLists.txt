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
    add_executable(mylite_embedded_open_close_test
      tests/embedded_open_close_test.c
    )
    target_link_libraries(mylite_embedded_open_close_test PRIVATE MyLite::mylite)
    mylite_configure_c_target(mylite_embedded_open_close_test)

    add_executable(mylite_embedded_exec_test
      tests/embedded_exec_test.c
    )
    target_link_libraries(mylite_embedded_exec_test PRIVATE MyLite::mylite)
    mylite_configure_c_target(mylite_embedded_exec_test)

    add_test(NAME libmylite.embedded-exec COMMAND mylite_embedded_exec_test)

    if(MYLITE_MARIADB_HAS_MYLITE_SE)
      add_executable(mylite_embedded_storage_engine_test
        tests/embedded_storage_engine_test.c
      )
      target_link_libraries(mylite_embedded_storage_engine_test PRIVATE MyLite::mylite)
      mylite_configure_c_target(mylite_embedded_storage_engine_test)

      add_test(
        NAME libmylite.embedded-storage-engine
        COMMAND mylite_embedded_storage_engine_test
      )
    endif()

    add_test(NAME libmylite.embedded-open-close COMMAND mylite_embedded_open_close_test)
  endif()
endif()
