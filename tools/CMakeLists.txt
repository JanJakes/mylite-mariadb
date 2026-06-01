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
  add_test(
    NAME tools.ownerless-fk-graph-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-fk-graph-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-fk-graph-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-fk-graph-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-ddl-stress-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-ddl-stress-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-ddl-stress-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-ddl-stress-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
endif()
