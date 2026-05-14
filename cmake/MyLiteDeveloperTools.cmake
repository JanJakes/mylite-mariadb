function(mylite_add_developer_tool_targets)
  file(GLOB_RECURSE mylite_first_party_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/packages/*.c"
    "${PROJECT_SOURCE_DIR}/packages/*.cc"
    "${PROJECT_SOURCE_DIR}/packages/*.cpp"
    "${PROJECT_SOURCE_DIR}/packages/*.h"
    "${PROJECT_SOURCE_DIR}/packages/*.hpp"
    "${PROJECT_SOURCE_DIR}/tests/*.c"
    "${PROJECT_SOURCE_DIR}/tests/*.cc"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    "${PROJECT_SOURCE_DIR}/tools/*.c"
    "${PROJECT_SOURCE_DIR}/tools/*.cc"
    "${PROJECT_SOURCE_DIR}/tools/*.cpp"
    "${PROJECT_SOURCE_DIR}/tools/*.h"
    "${PROJECT_SOURCE_DIR}/tools/*.hpp"
  )
  list(SORT mylite_first_party_sources)

  find_program(MYLITE_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
  if(MYLITE_CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
      COMMAND "${MYLITE_CLANG_FORMAT_EXECUTABLE}" -i ${mylite_first_party_sources}
      COMMENT "Formatting first-party sources"
      VERBATIM
    )

    add_custom_target(format-check
      COMMAND "${MYLITE_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${mylite_first_party_sources}
      COMMENT "Checking first-party source formatting"
      VERBATIM
    )
  else()
    add_custom_target(format
      COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found. Install LLVM and put it on PATH."
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM
    )

    add_custom_target(format-check
      COMMAND "${CMAKE_COMMAND}" -E echo "clang-format not found. Install LLVM and put it on PATH."
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM
    )
  endif()

  find_program(MYLITE_RUN_CLANG_TIDY_EXECUTABLE NAMES run-clang-tidy run-clang-tidy.py)
  if(MYLITE_RUN_CLANG_TIDY_EXECUTABLE)
    add_custom_target(tidy
      COMMAND "${MYLITE_RUN_CLANG_TIDY_EXECUTABLE}" -p "${CMAKE_BINARY_DIR}" packages tools tests
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      COMMENT "Running clang-tidy on first-party sources"
      VERBATIM
    )
  else()
    add_custom_target(tidy
      COMMAND "${CMAKE_COMMAND}" -E echo "run-clang-tidy not found. Install LLVM and put it on PATH."
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM
    )
  endif()
endfunction()
