if(MYLITE_WITH_MARIADB_EMBEDDED)
  add_executable(mylite_public_open_close_bench
    mylite-public-open-close-bench.c
  )
  target_link_libraries(mylite_public_open_close_bench PRIVATE MyLite::mylite)
  mylite_configure_c_target(mylite_public_open_close_bench)
endif()

if(BUILD_TESTING)
  add_test(
    NAME tools.ci-production-builds
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/check-ci-production-builds"
  )
  set_tests_properties(tools.ci-production-builds PROPERTIES
    LABELS "tools;compat.build"
  )
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
    NAME tools.ownerless-external-mariadb-trace-smoke-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-trace-smoke"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-trace-smoke-check"
      --scale 2
      --trace random-tx
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-trace-smoke-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-ddl-seed-smoke-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-ddl-seed-smoke"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-ddl-seed-smoke-check"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-ddl-seed-smoke-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-random-tx-seed-smoke-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-random-tx-seed-smoke"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-random-tx-seed-smoke-check"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-random-tx-seed-smoke-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-fk-graph-seed-smoke-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-fk-graph-seed-smoke"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-fk-graph-seed-smoke-check"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-fk-graph-seed-smoke-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-seed-sweep-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-seed-sweep"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-seed-sweep-check"
      --random-tx-rounds 3
      --ddl-rounds 3
      --fk-graph-rounds 3
      --seed-range 0:15
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-seed-sweep-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-seed-sweep-wide-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-seed-sweep"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-seed-sweep-wide-check"
      --random-tx-rounds 2
      --ddl-rounds 2
      --fk-graph-rounds 2
      --seed-range 0:63
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-seed-sweep-wide-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-external-mariadb-seed-sweep-next-check
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-external-mariadb-seed-sweep"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-external-mariadb-seed-sweep-next-check"
      --random-tx-rounds 2
      --ddl-rounds 2
      --fk-graph-rounds 2
      --seed-range 320:383
      --check
  )
  set_tests_properties(tools.ownerless-external-mariadb-seed-sweep-next-check PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-sql-trace-suite-scaled
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-suite"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-sql-trace-suite-scaled-smoke"
      --trace random-tx
      --scale 2
      --check
  )
  set_tests_properties(tools.ownerless-sql-trace-suite-scaled PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-sql-trace-suite-full-scaled
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-suite"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-sql-trace-suite-full-scaled-smoke"
      --scale 2
      --check
  )
  set_tests_properties(tools.ownerless-sql-trace-suite-full-scaled PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-sql-trace-suite-pressure-scaled
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-suite"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-sql-trace-suite-pressure-scaled-smoke"
      --trace active-reader-pressure
      --trace blob-pressure
      --scale 2
      --check
  )
  set_tests_properties(tools.ownerless-sql-trace-suite-pressure-scaled PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-sql-trace-suite-pressure-wide-scaled
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-sql-trace-suite"
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-sql-trace-suite-pressure-wide-scaled-smoke"
      --trace active-reader-pressure
      --trace blob-pressure
      --scale 3
      --check
  )
  set_tests_properties(tools.ownerless-sql-trace-suite-pressure-wide-scaled PROPERTIES
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
    NAME tools.ownerless-random-tx-seed-suite
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-random-tx-seed-suite"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-random-tx-seed-suite-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-random-tx-seed-suite PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-fk-graph-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-fk-graph-trace"
      --rounds 3
      --seed 17
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-fk-graph-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-fk-graph-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-fk-graph-seed-suite
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-fk-graph-seed-suite"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-fk-graph-seed-suite-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-fk-graph-seed-suite PROPERTIES
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
    NAME tools.ownerless-ddl-stress-seed-suite
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-ddl-stress-seed-suite"
      --rounds 3
      --seed 0
      --seed 17
      --seed 83
      --seed 211
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-ddl-stress-seed-suite-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-ddl-stress-seed-suite PROPERTIES
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
    NAME tools.ownerless-ctas-dml-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-ctas-dml-trace"
      --rounds 3
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-ctas-dml-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-ctas-dml-trace PROPERTIES
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
  add_test(
    NAME tools.ownerless-compressed-row-format-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-compressed-row-format-trace"
      --rounds 3
      --rows 2
      --payload-bytes 12000
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-compressed-row-format-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-compressed-row-format-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
  add_test(
    NAME tools.ownerless-blob-pressure-trace
    COMMAND
      "${CMAKE_CURRENT_SOURCE_DIR}/ownerless-blob-pressure-trace"
      --rounds 3
      --rows 2
      --payload-bytes 12000
      --reader-polls 6
      --output "${CMAKE_CURRENT_BINARY_DIR}/ownerless-blob-pressure-trace-smoke"
      --check
  )
  set_tests_properties(tools.ownerless-blob-pressure-trace PROPERTIES
    LABELS "tools;compat.concurrency"
  )
endif()
