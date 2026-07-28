# Windows initial cache for the MyLite MariaDB embedded-library baseline.

include("${CMAKE_CURRENT_LIST_DIR}/mariadb-embedded-baseline.cmake")

# Keep the Windows build self-contained. The bundled WolfSSL and PCRE objects
# are merged into mysqlserver.lib by MariaDB's Windows archive target.
set(WITH_SSL "bundled" CACHE STRING "Use bundled WolfSSL on Windows" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "Use the DLL CRT" FORCE)
