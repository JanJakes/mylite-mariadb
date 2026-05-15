set(MYLITE_MARIADB_BUILD_DIR
  "${PROJECT_SOURCE_DIR}/build/mariadb-embedded"
  CACHE PATH
  "MariaDB embedded build directory"
)
set(MYLITE_MARIADB_MESSAGES_DIR
  "${MYLITE_MARIADB_BUILD_DIR}/sql/share"
  CACHE PATH
  "MariaDB embedded message directory"
)
set(MYLITE_MARIADB_CHARSETS_DIR
  "${PROJECT_SOURCE_DIR}/mariadb/sql/share/charsets"
  CACHE PATH
  "MariaDB character set directory"
)

set(MYLITE_MARIADB_HAS_MYLITE_SE OFF)
set(MYLITE_MARIADB_HAS_PERFSCHEMA ON)
if(EXISTS "${MYLITE_MARIADB_BUILD_DIR}/CMakeCache.txt")
  file(STRINGS
    "${MYLITE_MARIADB_BUILD_DIR}/CMakeCache.txt"
    mylite_mariadb_mylite_se_cache
    REGEX "^PLUGIN_MYLITE_SE:.*=STATIC$"
  )
  if(mylite_mariadb_mylite_se_cache)
    set(MYLITE_MARIADB_HAS_MYLITE_SE ON)
  endif()

  file(STRINGS
    "${MYLITE_MARIADB_BUILD_DIR}/CMakeCache.txt"
    mylite_mariadb_perfschema_cache
    REGEX "^PLUGIN_PERFSCHEMA:.*=(NO|OFF|FALSE|0)$"
  )
  if(mylite_mariadb_perfschema_cache)
    set(MYLITE_MARIADB_HAS_PERFSCHEMA OFF)
  endif()
endif()

function(mylite_add_mariadb_embedded_target)
  if(NOT MYLITE_WITH_MARIADB_EMBEDDED)
    return()
  endif()

  set(mariadb_embedded_archive "${MYLITE_MARIADB_BUILD_DIR}/libmysqld/libmariadbd.a")
  if(NOT EXISTS "${mariadb_embedded_archive}")
    message(FATAL_ERROR
      "MYLITE_WITH_MARIADB_EMBEDDED requires ${mariadb_embedded_archive}. "
      "Run tools/mariadb-embedded-build all first."
    )
  endif()

  if(NOT EXISTS "${MYLITE_MARIADB_MESSAGES_DIR}")
    message(FATAL_ERROR
      "MYLITE_MARIADB_MESSAGES_DIR does not exist: ${MYLITE_MARIADB_MESSAGES_DIR}"
    )
  endif()

  if(NOT EXISTS "${MYLITE_MARIADB_CHARSETS_DIR}")
    message(FATAL_ERROR
      "MYLITE_MARIADB_CHARSETS_DIR does not exist: ${MYLITE_MARIADB_CHARSETS_DIR}"
    )
  endif()

  find_package(OpenSSL REQUIRED)
  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  find_package(ZLIB REQUIRED)
  pkg_check_modules(MYLITE_PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)

  if(NOT TARGET MyLite::MariaDBEmbedded)
    add_library(mylite_mariadb_embedded STATIC IMPORTED GLOBAL)
    add_library(MyLite::MariaDBEmbedded ALIAS mylite_mariadb_embedded)
    set_target_properties(mylite_mariadb_embedded PROPERTIES
      IMPORTED_LOCATION "${mariadb_embedded_archive}"
      INTERFACE_INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/mariadb/include;${MYLITE_MARIADB_BUILD_DIR}/include;${PROJECT_SOURCE_DIR}/mariadb/libmysqld"
    )
    target_compile_definitions(mylite_mariadb_embedded INTERFACE EMBEDDED_LIBRARY)
    target_link_libraries(mylite_mariadb_embedded INTERFACE
      OpenSSL::Crypto
      OpenSSL::SSL
      PkgConfig::MYLITE_PCRE2
      Threads::Threads
      ZLIB::ZLIB
    )
    if(NOT APPLE)
      target_link_libraries(mylite_mariadb_embedded INTERFACE dl m)
    endif()
  endif()
endfunction()

function(mylite_link_mariadb_embedded target)
  if(NOT MYLITE_WITH_MARIADB_EMBEDDED)
    target_compile_definitions("${target}" PRIVATE
      MYLITE_WITH_MARIADB_EMBEDDED=0
      MYLITE_MARIADB_HAS_MYLITE_SE=0
      MYLITE_MARIADB_HAS_PERFSCHEMA=0
    )
    return()
  endif()

  mylite_add_mariadb_embedded_target()
  if(MYLITE_MARIADB_HAS_MYLITE_SE)
    set(mylite_mariadb_has_mylite_se_define 1)
  else()
    set(mylite_mariadb_has_mylite_se_define 0)
  endif()
  if(MYLITE_MARIADB_HAS_PERFSCHEMA)
    set(mylite_mariadb_has_perfschema_define 1)
  else()
    set(mylite_mariadb_has_perfschema_define 0)
  endif()

  target_compile_definitions("${target}" PRIVATE
    MYLITE_WITH_MARIADB_EMBEDDED=1
    MYLITE_MARIADB_HAS_MYLITE_SE=${mylite_mariadb_has_mylite_se_define}
    MYLITE_MARIADB_HAS_PERFSCHEMA=${mylite_mariadb_has_perfschema_define}
    MYLITE_MARIADB_MESSAGES_DIR="${MYLITE_MARIADB_MESSAGES_DIR}"
    MYLITE_MARIADB_CHARSETS_DIR="${MYLITE_MARIADB_CHARSETS_DIR}"
  )
  target_link_libraries("${target}" PRIVATE MyLite::MariaDBEmbedded)
endfunction()
