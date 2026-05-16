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
set(ENABLED_PROFILING OFF CACHE BOOL "Disable session statement profiling" FORCE)

# MyLite rejects dynamic plugin install/uninstall SQL and does not load host plugin libraries.
set(MYLITE_WITH_DYNAMIC_PLUGIN_LOADING OFF CACHE BOOL "Omit dynamic plugin loading" FORCE)

# These components require optional submodule payloads that are intentionally not
# included in the initial import.
set(WITH_WSREP OFF CACHE BOOL "Disable Galera/wsrep support for the embedded profile" FORCE)
set(PLUGIN_S3 "NO" CACHE STRING "Disable Aria S3 support for the embedded profile" FORCE)

# MyLite rejects server-oriented plugin management and runs without grants,
# feedback, and Performance Schema instrumentation.
set(PLUGIN_AUTH_SOCKET "NO" CACHE STRING "Disable server socket authentication plugin" FORCE)
set(PLUGIN_FEEDBACK "NO" CACHE STRING "Disable server feedback plugin" FORCE)
set(PLUGIN_PERFSCHEMA "NO" CACHE STRING "Disable Performance Schema storage engine" FORCE)
set(PLUGIN_SEQUENCE "NO" CACHE STRING "Disable virtual SEQUENCE storage engine" FORCE)
set(PLUGIN_THREAD_POOL_INFO "NO" CACHE STRING "Disable server thread-pool info plugin" FORCE)
set(PLUGIN_USERSTAT "NO" CACHE STRING "Disable server user-statistics plugin" FORCE)

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

# MyLite omits zlib-backed SQL, protocol, column, and binlog compression.
set(MYLITE_WITH_ZLIB_COMPRESSION OFF CACHE BOOL "Omit zlib-backed compression helpers" FORCE)

# MyLite rejects GIS SQL functions before MariaDB execution.
set(MYLITE_WITH_GIS_SQL_FUNCTIONS OFF CACHE BOOL "Omit GIS SQL functions" FORCE)

# MyLite rejects vector SQL functions and vector indexes before MariaDB execution.
set(MYLITE_WITH_VECTOR_SQL_RUNTIME OFF CACHE BOOL "Omit vector SQL and MHNSW runtime" FORCE)

# MyLite rejects SFORMAT() before MariaDB execution.
set(MYLITE_WITH_SFORMAT_SQL_FUNCTION OFF CACHE BOOL "Omit SFORMAT SQL function" FORCE)

# MyLite rejects JSON_SCHEMA_VALID() before MariaDB execution.
set(MYLITE_WITH_JSON_SCHEMA_VALID OFF CACHE BOOL "Omit JSON schema validation SQL function" FORCE)

# MyLite rejects JSON_TABLE() before MariaDB execution.
set(MYLITE_WITH_JSON_TABLE OFF CACHE BOOL "Omit JSON_TABLE table function runtime" FORCE)

# MyLite rejects dynamic-column SQL functions before MariaDB execution.
set(MYLITE_WITH_DYNAMIC_COLUMNS OFF CACHE BOOL "Omit dynamic-column packed BLOB runtime" FORCE)

# MyLite rejects top-level SQL HANDLER commands before MariaDB execution.
set(MYLITE_WITH_SQL_HANDLER_COMMAND OFF CACHE BOOL "Omit SQL HANDLER command runtime" FORCE)

# MyLite rejects sequence objects and sequence value functions before MariaDB execution.
set(MYLITE_WITH_SEQUENCE_RUNTIME OFF CACHE BOOL "Omit SQL sequence runtime" FORCE)

# MyLite rejects HELP before MariaDB execution.
set(MYLITE_WITH_HELP_COMMAND OFF CACHE BOOL "Omit SQL HELP command" FORCE)

# MyLite rejects PROCEDURE ANALYSE() before MariaDB execution.
set(MYLITE_WITH_PROCEDURE_ANALYSE OFF CACHE BOOL "Omit PROCEDURE ANALYSE implementation" FORCE)

# MyLite rejects SELECT PROCEDURE clauses before MariaDB execution.
set(MYLITE_WITH_SELECT_PROCEDURE_RUNTIME OFF CACHE BOOL "Omit SELECT PROCEDURE runtime" FORCE)

# MyLite rejects stored routine, trigger, event, and package runtime surfaces.
set(MYLITE_WITH_STORED_PROGRAM_RUNTIME OFF CACHE BOOL "Omit stored program runtime" FORCE)

# MyLite rejects event DDL and does not validate event scheduler parse data.
set(MYLITE_WITH_EVENT_PARSE_DATA_VALIDATION OFF CACHE BOOL "Omit event parse-data validation" FORCE)

# MyLite rejects trigger DDL and does not use .TRG/.TRN metadata files.
set(MYLITE_WITH_TRIGGER_RUNTIME OFF CACHE BOOL "Omit trigger runtime and sidecar metadata" FORCE)

# MyLite rejects persistent views and does not use view .frm metadata files.
set(MYLITE_WITH_VIEW_RUNTIME OFF CACHE BOOL "Omit view runtime and sidecar metadata" FORCE)

# MyLite rejects dynamic UDF registration and library loading.
set(MYLITE_WITH_UDF_RUNTIME OFF CACHE BOOL "Omit UDF lookup and execution runtime" FORCE)

# MyLite starts with --skip-log-bin and rejects replication/binlog SQL.
set(MYLITE_WITH_BINLOG_CORE OFF CACHE BOOL "Omit binary log transaction and event core" FORCE)

# MyLite rejects server-style MyISAM table maintenance and key-cache admin SQL.
set(MYLITE_WITH_MYISAM_MAINTENANCE OFF CACHE BOOL "Omit MyISAM maintenance admin runtime" FORCE)

# MyLite rejects foreign-server metadata SQL and does not use mysql.servers.
set(MYLITE_WITH_FOREIGN_SERVER_METADATA OFF CACHE BOOL "Omit mysql.servers metadata cache" FORCE)

# MyLite rejects external backup-tool SQL and uses file-owned recovery.
set(MYLITE_WITH_BACKUP_RUNTIME OFF CACHE BOOL "Omit external backup SQL runtime" FORCE)

# MyLite reports no server-global query cache and rejects cache administration.
set(MYLITE_WITH_QUERY_CACHE_RUNTIME OFF CACHE BOOL "Omit query cache runtime" FORCE)

# MyLite rejects optimizer trace diagnostics before MariaDB execution.
set(MYLITE_WITH_OPTIMIZER_TRACE OFF CACHE BOOL "Omit optimizer trace runtime" FORCE)

# MyLite rejects static server-information SHOW commands before MariaDB execution.
set(MYLITE_WITH_STATIC_SHOW_INFO OFF CACHE BOOL "Omit static SHOW information producers" FORCE)

# MyLite does not publish server-global status counters in the core embedded API.
set(MYLITE_WITH_STATUS_METADATA OFF CACHE BOOL "Omit status metadata producers" FORCE)

# MyLite rejects daemon-style process-list SHOW commands before MariaDB execution.
set(MYLITE_WITH_PROCESSLIST_METADATA OFF CACHE BOOL "Omit process-list metadata producers" FORCE)

# MyLite rejects routine objects and returns empty routine metadata until it has a routine catalog.
set(MYLITE_WITH_ROUTINE_METADATA OFF CACHE BOOL "Omit routine metadata producers" FORCE)

# MyLite's embedded SQL profile has no retained C++ exception users.
set(MYLITE_WITH_EMBEDDED_SQL_EXCEPTIONS OFF CACHE BOOL "Enable embedded SQL C++ exceptions" FORCE)
