if(MYLITE_WITH_MARIADB_EMBEDDED AND MYLITE_MARIADB_HAS_MYLITE_SE)
  add_executable(mylite_perf_baseline
    mylite_perf_baseline.c
  )
  target_link_libraries(mylite_perf_baseline PRIVATE MyLite::mylite MyLite::storage)
  mylite_configure_c_target(mylite_perf_baseline)
endif()
