# Windows initial cache for the MyLite MariaDB embedded-library baseline.

include("${CMAKE_CURRENT_LIST_DIR}/mariadb-embedded-baseline.cmake")

# Use MariaDB's pinned WolfSSL baseline on Windows. The PowerShell build
# wrapper fetches and verifies the omitted upstream submodule payload before
# configure; WolfSSL and PCRE objects are then merged into mysqlserver.lib.
set(WITH_SSL "bundled" CACHE STRING "Use bundled WolfSSL on Windows" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "Use the DLL CRT" FORCE)
