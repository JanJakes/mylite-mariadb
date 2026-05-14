cmake_minimum_required(VERSION 3.25)

project(MyLite
  VERSION 0.1.0
  DESCRIPTION "Embedded MySQL/MariaDB drop-in database library"
  LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CTest)
include(MyLiteCompilerOptions)
include(MyLiteDeveloperTools)

option(MYLITE_BUILD_TOOLS "Build MyLite command-line tools" ON)
option(MYLITE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_subdirectory(packages/libmylite)

if(MYLITE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

mylite_add_developer_tool_targets()
