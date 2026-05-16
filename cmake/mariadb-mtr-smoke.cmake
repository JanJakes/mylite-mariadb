# Initial cache for the MyLite MariaDB MTR smoke profile.
#
# This keeps the measured embedded baseline trims, but re-enables the MariaDB
# runtimes required by mysql-test-run bootstrap SQL.

include("${CMAKE_CURRENT_LIST_DIR}/mariadb-embedded-baseline.cmake")

set(MYLITE_WITH_VIEW_RUNTIME ON CACHE BOOL "Build view runtime for MTR bootstrap" FORCE)
set(MYLITE_WITH_STORED_PROGRAM_RUNTIME ON CACHE BOOL "Build stored program runtime for MTR bootstrap" FORCE)
set(MYLITE_WITH_TRIGGER_RUNTIME ON CACHE BOOL "Build trigger runtime for MTR bootstrap" FORCE)
set(MYLITE_WITH_BINLOG_SYSVARS ON CACHE BOOL "Build binlog system variables used by MTR bootstrap" FORCE)
