# Initial cache for the MyLite MariaDB embedded-library profile.
#
# Keep this file limited to reproducibility settings and options required by
# the intentionally partial MariaDB submodule import plus measured trims for
# server-oriented surfaces that are outside the embedded MyLite runtime.

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "MariaDB embedded profile build type" FORCE)

if(APPLE)
  set(
    CMAKE_C_FLAGS
    "-Wno-nullability-completeness"
    CACHE STRING
    "Suppress Apple SDK nullability warnings in MariaDB embedded builds"
    FORCE
  )
  set(
    CMAKE_CXX_FLAGS
    "-Wno-nullability-completeness"
    CACHE STRING
    "Suppress Apple SDK nullability warnings in MariaDB embedded builds"
    FORCE
  )
endif()

set(UPDATE_SUBMODULES OFF CACHE BOOL "Do not fetch MariaDB submodules during configure" FORCE)
set(WITH_EMBEDDED_SERVER ON CACHE BOOL "Build MariaDB's embedded server library" FORCE)
set(WITH_UNIT_TESTS OFF CACHE BOOL "Skip MariaDB upstream unit-test targets in the profile" FORCE)
set(WITH_SSL "system" CACHE STRING "Use the host OpenSSL installation" FORCE)
set(WITHOUT_DYNAMIC_PLUGINS ON CACHE BOOL "Disable host dynamic plugin support" FORCE)

# These components require optional submodule payloads that are intentionally not
# included in the initial import.
set(WITH_WSREP OFF CACHE BOOL "Disable Galera/wsrep support for the embedded profile" FORCE)
set(PLUGIN_S3 "NO" CACHE STRING "Disable Aria S3 support for the embedded profile" FORCE)

# MyLite rejects server-oriented plugin management and runs without grants,
# feedback, and Performance Schema instrumentation.
set(PLUGIN_AUTH_SOCKET "NO" CACHE STRING "Disable server socket authentication plugin" FORCE)
set(PLUGIN_FEEDBACK "NO" CACHE STRING "Disable server feedback plugin" FORCE)
set(PLUGIN_PERFSCHEMA "NO" CACHE STRING "Disable Performance Schema storage engine" FORCE)
set(PLUGIN_THREAD_POOL_INFO "NO" CACHE STRING "Disable server thread-pool info plugin" FORCE)

# MyLite rejects LOAD DATA and LOAD XML before MariaDB execution.
set(MYLITE_WITH_LOAD_DATA OFF CACHE BOOL "Omit LOAD DATA/XML execution support" FORCE)

# MyLite rejects host-file SQL I/O before MariaDB execution.
set(MYLITE_WITH_SQL_FILE_IO OFF CACHE BOOL "Omit host-file SQL I/O support" FORCE)

# MyLite rejects server utility functions before MariaDB execution.
set(MYLITE_WITH_SERVER_UTILITY_FUNCTIONS OFF CACHE BOOL "Omit server utility SQL functions" FORCE)

# MyLite rejects Oracle SQL mode before MariaDB execution.
set(MYLITE_WITH_ORACLE_SQL_MODE OFF CACHE BOOL "Omit Oracle SQL mode parser" FORCE)

# MyLite rejects XML SQL functions before MariaDB execution.
set(MYLITE_WITH_XML_SQL_FUNCTIONS OFF CACHE BOOL "Omit XML SQL functions" FORCE)

# MyLite rejects GIS SQL functions before MariaDB execution.
set(MYLITE_WITH_GIS_SQL_FUNCTIONS OFF CACHE BOOL "Omit GIS SQL functions" FORCE)

# MyLite rejects SFORMAT() before MariaDB execution.
set(MYLITE_WITH_SFORMAT_SQL_FUNCTION OFF CACHE BOOL "Omit SFORMAT SQL function" FORCE)

# MyLite rejects HELP before MariaDB execution.
set(MYLITE_WITH_HELP_COMMAND OFF CACHE BOOL "Omit SQL HELP command" FORCE)

# MyLite rejects PROCEDURE ANALYSE() before MariaDB execution.
set(MYLITE_WITH_PROCEDURE_ANALYSE OFF CACHE BOOL "Omit PROCEDURE ANALYSE implementation" FORCE)

# MyLite's embedded SQL profile has no retained C++ exception users.
set(MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS OFF CACHE BOOL "Enable embedded SQL C++ exceptions" FORCE)
