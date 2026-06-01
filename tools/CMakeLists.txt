if(BUILD_TESTING)
  add_test(
    NAME tools.ownerless-random-tx-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-random-tx-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-random-tx-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-random-tx-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
endif()
