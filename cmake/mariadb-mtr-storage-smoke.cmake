# Initial cache for the MyLite MariaDB MTR storage smoke profile.
#
# This extends the baseline MTR smoke profile with the static MyLite storage
# engine so MyLite-owned MTR tests can exercise routed table DDL and DML.

include("${CMAKE_CURRENT_LIST_DIR}/mariadb-mtr-smoke.cmake")

set(PLUGIN_MYLITE_SE "STATIC" CACHE STRING "Build the MyLite storage engine for MTR storage smoke" FORCE)
