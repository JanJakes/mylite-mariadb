# Initial cache for the first MyLite MariaDB embedded-library baseline.
#
# Keep this file limited to reproducibility settings and options required by
# the intentionally partial MariaDB submodule import. Size trimming belongs in
# later measured slices.

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "MariaDB baseline build type" FORCE)

set(UPDATE_SUBMODULES OFF CACHE BOOL "Do not fetch MariaDB submodules during configure" FORCE)
set(WITH_EMBEDDED_SERVER ON CACHE BOOL "Build MariaDB's embedded server library" FORCE)
set(WITH_UNIT_TESTS OFF CACHE BOOL "Skip MariaDB upstream unit-test targets in the baseline" FORCE)
set(WITH_SSL "system" CACHE STRING "Use the host OpenSSL installation" FORCE)

# These components require optional submodule payloads that are intentionally not
# included in the initial import.
set(WITH_WSREP OFF CACHE BOOL "Disable Galera/wsrep support for the embedded baseline" FORCE)
set(PLUGIN_S3 "NO" CACHE STRING "Disable Aria S3 support for the embedded baseline" FORCE)
