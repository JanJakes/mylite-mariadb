add_library(mylite_storage STATIC
  src/storage.c
)
add_library(MyLite::storage ALIAS mylite_storage)

target_include_directories(mylite_storage
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

mylite_configure_c_target(mylite_storage)

if(BUILD_TESTING)
  add_executable(mylite_storage_test
    tests/storage_test.c
  )
  target_include_directories(mylite_storage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(mylite_storage_test PRIVATE MyLite::storage)
  mylite_configure_c_target(mylite_storage_test)

  add_test(NAME mylite-storage.capabilities COMMAND mylite_storage_test)
  set_tests_properties(
    mylite-storage.capabilities
    PROPERTIES LABELS "compat-storage;compat-crash-recovery;compat-locking;compat-statement-rollback"
  )
endif()
