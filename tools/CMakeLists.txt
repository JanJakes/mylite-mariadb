if(BUILD_TESTING)
  add_test(
    NAME tools.ownerless-sql-trace-runner
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-runner"
      --self-test
  )
  set_tests_properties(tools.ownerless-sql-trace-runner PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-sql-trace-suite
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-suite"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-sql-trace-suite-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-sql-trace-suite PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-independent-table-stress-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-independent-table-stress-trace"
      --iterations 3
      --reader-polls 6
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-independent-table-stress-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-independent-table-stress-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
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
  add_test(
    NAME tools.ownerless-ddl-lifecycle-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-ddl-lifecycle-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-ddl-lifecycle-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-ddl-lifecycle-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-checksum-stress-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-checksum-stress-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-checksum-stress-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-checksum-stress-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-transaction-stress-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-transaction-stress-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-transaction-stress-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-transaction-stress-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-temporary-table-stress-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-temporary-table-stress-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-temporary-table-stress-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-temporary-table-stress-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-active-reader-pressure-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-active-reader-pressure-trace"
      --rounds 4
      --rows 3
      --reader-polls 6
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-active-reader-pressure-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-active-reader-pressure-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
endif()
