if(NOT DEFINED MYLITE_MARIADB_SOURCE_DIR)
  message(FATAL_ERROR "MYLITE_MARIADB_SOURCE_DIR is required")
endif()
if(NOT DEFINED MYLITE_MARIADB_ARCHIVE)
  message(FATAL_ERROR "MYLITE_MARIADB_ARCHIVE is required")
endif()
if(NOT DEFINED MYLITE_MARIADB_PROFILE)
  message(FATAL_ERROR "MYLITE_MARIADB_PROFILE is required")
endif()

if(NOT EXISTS "${MYLITE_MARIADB_ARCHIVE}")
  message(FATAL_ERROR
    "MariaDB embedded archive does not exist: ${MYLITE_MARIADB_ARCHIVE}. "
    "Run tools/mariadb-embedded-build all first."
  )
endif()

file(TIMESTAMP "${MYLITE_MARIADB_ARCHIVE}" archive_timestamp "%s" UTC)
set(candidate_files "${MYLITE_MARIADB_PROFILE}")
set(source_patterns
  "*.c"
  "*.cc"
  "*.cpp"
  "*.cxx"
  "*.h"
  "*.hh"
  "*.hpp"
  "*.ic"
  "*.in"
  "*.cmake"
  "CMakeLists.txt"
)

foreach(source_pattern IN LISTS source_patterns)
  file(GLOB_RECURSE matched_files
    LIST_DIRECTORIES false
    "${MYLITE_MARIADB_SOURCE_DIR}/${source_pattern}"
  )
  list(APPEND candidate_files ${matched_files})
endforeach()

list(REMOVE_DUPLICATES candidate_files)
foreach(candidate_file IN LISTS candidate_files)
  if(NOT EXISTS "${candidate_file}")
    continue()
  endif()
  file(TIMESTAMP "${candidate_file}" candidate_timestamp "%s" UTC)
  if(candidate_timestamp GREATER archive_timestamp)
    message(FATAL_ERROR
      "MariaDB embedded archive is stale: ${MYLITE_MARIADB_ARCHIVE} is older "
      "than ${candidate_file}. Run tools/mariadb-embedded-build build before "
      "building embedded MyLite targets."
    )
  endif()
endforeach()
