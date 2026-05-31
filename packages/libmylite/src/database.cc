#include <mylite/mylite.h>

#include "ownerless_autoinc_registry.h"
#include "ownerless_dictionary_state.h"
#include "ownerless_innodb_lock_registry.h"
#include "ownerless_latch.h"
#include "ownerless_lock_table.h"
#include "ownerless_mdl.h"
#include "ownerless_page_index.h"
#include "ownerless_page_log.h"
#include "ownerless_page_pin_registry.h"
#include "ownerless_process_registry.h"
#include "ownerless_read_view_registry.h"
#include "ownerless_redo_state.h"
#include "ownerless_tablespace_replay.h"
#include "ownerless_trx_registry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if MYLITE_WITH_MARIADB_EMBEDDED
#  include "mylite_ownerless_innodb_lock_hooks.h"
#  include "mylite_ownerless_mdl_hooks.h"
#  include "mylite_ownerless_read_view_hooks.h"
#  include "mylite_ownerless_runtime_hooks.h"
#  include "mylite_ownerless_trx_hooks.h"
#  include "ownerless_wait.h"
#  include <mysql.h>
#endif

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

#ifndef MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
#  define MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS 0
#endif

#ifndef MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES
#  define MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES 65536
#endif

namespace {

constexpr unsigned k_known_open_flags =
    MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE |
    MYLITE_OPEN_URI | MYLITE_OPEN_SHARED_READONLY | MYLITE_OPEN_OWNERLESS_RW;
constexpr const char *k_sqlstate_ok = "00000";
constexpr const char *k_sqlstate_general = "HY000";
constexpr const char *k_not_an_error = "not an error";
constexpr const char *k_bad_db_handle = "bad database handle";
constexpr const char *k_memory_database_path = ":memory:";
constexpr int k_decimal_base = 10;

#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr unsigned k_mariadb_lock_deadlock_errno = 1213;
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED == MYLITE_OWNERLESS_LOCK_TABLE_SHARED);
static_assert(MYLITE_OWNERLESS_MDL_MODE_EXCLUSIVE == MYLITE_OWNERLESS_LOCK_TABLE_EXCLUSIVE);
static_assert(MYLITE_OWNERLESS_MDL_MODE_UPGRADABLE == MYLITE_OWNERLESS_LOCK_TABLE_UPGRADABLE);
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED_READ == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ);
static_assert(MYLITE_OWNERLESS_MDL_MODE_SHARED_WRITE == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_WRITE);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_READ_ONLY == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_READ_ONLY
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_WRITE == MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_WRITE
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SHARED_NO_READ_WRITE ==
    MYLITE_OWNERLESS_LOCK_TABLE_SHARED_NO_READ_WRITE
);
static_assert(
    MYLITE_OWNERLESS_MDL_MODE_SCOPED_INTENTION_EXCLUSIVE ==
    MYLITE_OWNERLESS_LOCK_TABLE_SCOPED_INTENTION_EXCLUSIVE
);
constexpr std::size_t k_sql_policy_token_count = 256;
constexpr const char *k_meta_filename = "mylite.meta";
constexpr const char *k_lock_filename = "mylite.lock";
constexpr const char *k_concurrency_dir_name = "concurrency";
constexpr const char *k_concurrency_meta_filename = "mylite-concurrency.meta";
constexpr const char *k_concurrency_lock_filename = "mylite-concurrency.lock";
constexpr const char *k_concurrency_shm_filename = "mylite-concurrency.shm";
constexpr const char *k_concurrency_wal_filename = "mylite-concurrency.wal";
constexpr const char *k_concurrency_checkpoint_filename = "mylite-concurrency.ckpt";
constexpr const char *k_datadir_name = "datadir";
constexpr const char *k_tmpdir_name = "tmp";
constexpr const char *k_rundir_name = "run";
constexpr const char *k_plugin_directory_name = "plugins";
constexpr const char *k_innodb_temp_tablespace_filename = "ibtmp1";
constexpr const char *k_statement_lock_filename = "mylite-statements.lock";
constexpr const char *k_mariadb_base_ref = "mariadb-11.8.6";
constexpr const char *k_metadata_format_line = "format=1";
constexpr const char *k_concurrency_mode_line = "mode=exclusive";
constexpr const char *k_innodb_temp_data_file_path = "ibtmp1:12M:autoextend";
constexpr const char *k_create_mysql_database_sql = "CREATE DATABASE IF NOT EXISTS mysql";
constexpr const char *k_create_proc_table_sql =
    "CREATE TABLE IF NOT EXISTS mysql.proc ("
    "db char(64) collate utf8mb3_bin DEFAULT '' NOT NULL, "
    "name char(64) DEFAULT '' NOT NULL, "
    "type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, "
    "specific_name char(64) DEFAULT '' NOT NULL, "
    "language enum('SQL') DEFAULT 'SQL' NOT NULL, "
    "sql_data_access enum('CONTAINS_SQL','NO_SQL','READS_SQL_DATA','MODIFIES_SQL_DATA') "
    "DEFAULT 'CONTAINS_SQL' NOT NULL, "
    "is_deterministic enum('YES','NO') DEFAULT 'NO' NOT NULL, "
    "security_type enum('INVOKER','DEFINER') DEFAULT 'DEFINER' NOT NULL, "
    "param_list blob DEFAULT '' NOT NULL, "
    "returns longblob NOT NULL, "
    "body longblob NOT NULL, "
    "definer varchar(384) collate utf8mb3_bin DEFAULT '' NOT NULL, "
    "created timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
    "modified timestamp NOT NULL DEFAULT '0000-00-00 00:00:00', "
    "sql_mode set('REAL_AS_FLOAT','PIPES_AS_CONCAT','ANSI_QUOTES','IGNORE_SPACE',"
    "'IGNORE_BAD_TABLE_OPTIONS','ONLY_FULL_GROUP_BY','NO_UNSIGNED_SUBTRACTION',"
    "'NO_DIR_IN_CREATE','POSTGRESQL','ORACLE','MSSQL','DB2','MAXDB','NO_KEY_OPTIONS',"
    "'NO_TABLE_OPTIONS','NO_FIELD_OPTIONS','MYSQL323','MYSQL40','ANSI',"
    "'NO_AUTO_VALUE_ON_ZERO','NO_BACKSLASH_ESCAPES','STRICT_TRANS_TABLES',"
    "'STRICT_ALL_TABLES','NO_ZERO_IN_DATE','NO_ZERO_DATE','INVALID_DATES',"
    "'ERROR_FOR_DIVISION_BY_ZERO','TRADITIONAL','NO_AUTO_CREATE_USER',"
    "'HIGH_NOT_PRECEDENCE','NO_ENGINE_SUBSTITUTION','PAD_CHAR_TO_FULL_LENGTH',"
    "'EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT','TIME_ROUND_FRACTIONAL') "
    "DEFAULT '' NOT NULL, "
    "comment text collate utf8mb3_bin NOT NULL, "
    "character_set_client char(32) collate utf8mb3_bin, "
    "collation_connection char(64) collate utf8mb3_bin, "
    "db_collation char(64) collate utf8mb3_bin, "
    "body_utf8 longblob, "
    "aggregate enum('NONE','GROUP') DEFAULT 'NONE' NOT NULL, "
    "PRIMARY KEY (db,name,type)) "
    "engine=Aria transactional=1 character set utf8mb3 COLLATE utf8mb3_general_ci "
    "comment='Stored Procedures'";
constexpr const char *k_create_procs_priv_table_sql =
    "CREATE TABLE IF NOT EXISTS mysql.procs_priv ("
    "Host char(255) binary DEFAULT '' NOT NULL, "
    "Db char(64) binary DEFAULT '' NOT NULL, "
    "User char(128) binary DEFAULT '' NOT NULL, "
    "Routine_name char(64) COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL, "
    "Routine_type enum('FUNCTION','PROCEDURE','PACKAGE','PACKAGE BODY') NOT NULL, "
    "Grantor varchar(384) DEFAULT '' NOT NULL, "
    "Proc_priv set('Execute','Alter Routine','Grant','Show Create Routine') "
    "COLLATE utf8mb3_general_ci DEFAULT '' NOT NULL, "
    "Timestamp timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
    "PRIMARY KEY (Host,Db,User,Routine_name,Routine_type), "
    "KEY Grantor (Grantor)) "
    "engine=Aria transactional=1 CHARACTER SET utf8mb3 COLLATE utf8mb3_bin "
    "comment='Procedure privileges'";
constexpr int k_runtime_directory_attempts = 100;
constexpr unsigned k_lock_poll_interval_ms = 10;
constexpr unsigned k_concurrency_lock_wait_timeout_ms = 5000;
constexpr unsigned k_statement_lock_wait_timeout_ms = 60000;
constexpr unsigned k_system_tables_lock_wait_timeout_ms = 60000;
constexpr unsigned long k_initial_result_buffer_size = 4096;
constexpr off_t k_persisted_config_lock_start = 0;
constexpr off_t k_persisted_config_lock_length = 1;
constexpr off_t k_recovery_lock_start = 1;
constexpr off_t k_recovery_lock_length = 1;
constexpr off_t k_shm_resize_lock_start = 2;
constexpr off_t k_shm_resize_lock_length = 1;
constexpr off_t k_system_tables_lock_start = 3;
constexpr off_t k_system_tables_lock_length = 1;
constexpr off_t k_dictionary_statement_lock_start = 4;
constexpr off_t k_dictionary_statement_lock_length = 1;
constexpr off_t k_global_write_statement_lock_start = 6;
constexpr off_t k_global_write_statement_lock_length = 1;
constexpr off_t k_table_statement_lock_start = 4096;
constexpr off_t k_table_statement_lock_length = 1;
constexpr std::uint64_t k_table_statement_lock_slot_count = 65536;
constexpr std::uint32_t k_innodb_page_size_max = 65536;
constexpr off_t k_minimum_concurrency_shm_size = 2097152;
constexpr std::array<unsigned char, 8> k_concurrency_shm_magic = {
    'M',
    'Y',
    'L',
    'S',
    'H',
    'M',
    '0',
    '1',
};
constexpr std::array<unsigned char, 8> k_concurrency_wal_magic = {
    'M',
    'Y',
    'L',
    'W',
    'A',
    'L',
    '0',
    '1',
};
constexpr std::array<unsigned char, 8> k_concurrency_checkpoint_magic = {
    'M',
    'Y',
    'L',
    'C',
    'K',
    'P',
    '0',
    '1',
};
constexpr std::size_t k_concurrency_shm_header_size = 128;
constexpr std::size_t k_concurrency_recovery_header_size = 128;
constexpr std::size_t k_concurrency_checkpoint_latest_lsn_offset =
    k_concurrency_recovery_header_size;
constexpr std::size_t k_concurrency_checkpoint_visible_lsn_offset =
    k_concurrency_checkpoint_latest_lsn_offset + sizeof(std::uint64_t);
constexpr off_t k_concurrency_checkpoint_lsn_payload_end =
    static_cast<off_t>(k_concurrency_checkpoint_visible_lsn_offset + sizeof(std::uint64_t));
constexpr off_t k_concurrency_checkpoint_lock_start = 0;
constexpr off_t k_concurrency_checkpoint_lock_length = 1;
constexpr std::size_t k_database_uuid_size = 36;
constexpr std::uint32_t k_concurrency_shm_format_version = 9;
constexpr std::uint32_t k_concurrency_recovery_format_version = 1;
constexpr std::uint32_t k_concurrency_shm_header_version_min = 1;
constexpr std::uint32_t k_concurrency_shm_byte_order = 0x01020304U;
constexpr std::uint32_t k_concurrency_shm_state_clean = 1;
constexpr std::uint32_t k_concurrency_shm_state_dirty = 2;
constexpr std::uint32_t k_concurrency_shm_state_rebuilding = 3;
constexpr std::size_t k_concurrency_shm_magic_offset = 0;
constexpr std::size_t k_concurrency_shm_format_offset = 8;
constexpr std::size_t k_concurrency_shm_min_format_offset = 12;
constexpr std::size_t k_concurrency_shm_header_size_offset = 16;
constexpr std::size_t k_concurrency_shm_byte_order_offset = 20;
constexpr std::size_t k_concurrency_shm_flags_offset = 24;
constexpr std::size_t k_concurrency_shm_state_offset = 28;
constexpr std::size_t k_concurrency_shm_mapping_size_offset = 32;
constexpr std::size_t k_concurrency_shm_generation_offset = 40;
constexpr std::size_t k_concurrency_shm_recovery_generation_offset = 48;
constexpr std::size_t k_concurrency_shm_segment_table_offset = 56;
constexpr std::size_t k_concurrency_shm_segment_count_offset = 60;
constexpr std::size_t k_concurrency_shm_database_uuid_offset = 64;
constexpr std::size_t k_concurrency_recovery_magic_offset = 0;
constexpr std::size_t k_concurrency_recovery_format_offset = 8;
constexpr std::size_t k_concurrency_recovery_header_size_offset = 12;
constexpr std::size_t k_concurrency_recovery_byte_order_offset = 16;
constexpr std::size_t k_concurrency_recovery_flags_offset = 20;
constexpr std::size_t k_concurrency_recovery_generation_offset = 24;
constexpr std::size_t k_concurrency_recovery_database_uuid_offset = 64;
constexpr std::size_t k_concurrency_shm_segment_table_start = 128;
constexpr std::size_t k_concurrency_shm_segment_descriptor_size = 32;
constexpr std::uint32_t k_concurrency_shm_segment_count = 12;
constexpr std::size_t k_concurrency_shm_segment_type_offset = 0;
constexpr std::size_t k_concurrency_shm_segment_version_offset = 4;
constexpr std::size_t k_concurrency_shm_segment_data_offset = 8;
constexpr std::size_t k_concurrency_shm_segment_length_offset = 16;
constexpr std::size_t k_concurrency_shm_segment_generation_offset = 24;
constexpr std::uint32_t k_concurrency_process_registry_segment_type = 1;
constexpr std::uint32_t k_concurrency_process_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_wait_channel_segment_type = 2;
constexpr std::uint32_t k_concurrency_wait_channel_segment_version = 1;
constexpr std::uint32_t k_concurrency_mdl_lock_table_segment_type = 3;
constexpr std::uint32_t k_concurrency_mdl_lock_table_segment_version = 2;
constexpr std::uint32_t k_concurrency_trx_registry_segment_type = 4;
constexpr std::uint32_t k_concurrency_trx_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_read_view_registry_segment_type = 5;
constexpr std::uint32_t k_concurrency_read_view_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_innodb_lock_registry_segment_type = 6;
constexpr std::uint32_t k_concurrency_innodb_lock_registry_segment_version = 5;
constexpr std::uint32_t k_concurrency_redo_state_segment_type = 7;
constexpr std::uint32_t k_concurrency_redo_state_segment_version = 7;
constexpr std::uint32_t k_concurrency_page_index_segment_type = 8;
constexpr std::uint32_t k_concurrency_page_index_segment_version = 3;
constexpr std::uint32_t k_concurrency_dictionary_state_segment_type = 9;
constexpr std::uint32_t k_concurrency_dictionary_state_segment_version = 1;
constexpr std::uint32_t k_concurrency_page_write_lock_registry_segment_type = 10;
constexpr std::uint32_t k_concurrency_page_write_lock_registry_segment_version = 2;
constexpr std::uint32_t k_concurrency_autoinc_registry_segment_type = 11;
constexpr std::uint32_t k_concurrency_autoinc_registry_segment_version = 1;
constexpr std::uint32_t k_concurrency_page_pin_registry_segment_type = 12;
constexpr std::uint32_t k_concurrency_page_pin_registry_segment_version = 1;
constexpr std::size_t k_concurrency_process_registry_offset = 512;
constexpr std::size_t k_concurrency_process_registry_header_size =
    MYLITE_OWNERLESS_PROCESS_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_process_slot_count = 16;
constexpr std::size_t k_concurrency_process_slot_size = MYLITE_OWNERLESS_PROCESS_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_process_registry_size =
    k_concurrency_process_registry_header_size +
    (k_concurrency_process_slot_count * k_concurrency_process_slot_size);
constexpr std::size_t k_concurrency_wait_channel_offset =
    k_concurrency_process_registry_offset + k_concurrency_process_registry_size;
constexpr std::size_t k_concurrency_wait_channel_header_size = 64;
constexpr std::uint32_t k_concurrency_wait_channel_count = 16;
constexpr std::size_t k_concurrency_wait_channel_size = 64;
constexpr std::size_t k_concurrency_wait_channel_segment_size =
    k_concurrency_wait_channel_header_size +
    (k_concurrency_wait_channel_count * k_concurrency_wait_channel_size);
constexpr std::size_t k_concurrency_mdl_lock_table_offset =
    k_concurrency_wait_channel_offset + k_concurrency_wait_channel_segment_size;
constexpr std::size_t k_concurrency_mdl_lock_table_header_size =
    MYLITE_OWNERLESS_LOCK_TABLE_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_mdl_lock_table_entry_count = 128;
constexpr std::size_t k_concurrency_mdl_lock_table_entry_size =
    MYLITE_OWNERLESS_LOCK_TABLE_ENTRY_SIZE;
constexpr std::size_t k_concurrency_mdl_lock_table_segment_size =
    k_concurrency_mdl_lock_table_header_size +
    (k_concurrency_mdl_lock_table_entry_count * k_concurrency_mdl_lock_table_entry_size);
constexpr std::size_t k_concurrency_trx_registry_offset =
    k_concurrency_mdl_lock_table_offset + k_concurrency_mdl_lock_table_segment_size;
constexpr std::size_t k_concurrency_trx_registry_header_size =
    MYLITE_OWNERLESS_TRX_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_trx_slot_count = 64;
constexpr std::size_t k_concurrency_trx_slot_size = MYLITE_OWNERLESS_TRX_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_trx_registry_segment_size =
    k_concurrency_trx_registry_header_size +
    (k_concurrency_trx_slot_count * k_concurrency_trx_slot_size);
constexpr std::size_t k_concurrency_read_view_registry_offset =
    k_concurrency_trx_registry_offset + k_concurrency_trx_registry_segment_size;
constexpr std::size_t k_concurrency_read_view_registry_header_size =
    MYLITE_OWNERLESS_READ_VIEW_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_read_view_slot_count = 64;
constexpr std::size_t k_concurrency_read_view_slot_size =
    MYLITE_OWNERLESS_READ_VIEW_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_read_view_registry_segment_size =
    k_concurrency_read_view_registry_header_size +
    (k_concurrency_read_view_slot_count * k_concurrency_read_view_slot_size);
constexpr std::size_t k_concurrency_innodb_lock_registry_offset =
    k_concurrency_read_view_registry_offset + k_concurrency_read_view_registry_segment_size;
constexpr std::size_t k_concurrency_innodb_lock_registry_header_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_HEADER_SIZE;
constexpr std::uint32_t k_concurrency_innodb_lock_slot_count = 4096;
constexpr std::size_t k_concurrency_innodb_lock_slot_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_innodb_lock_registry_segment_size =
    k_concurrency_innodb_lock_registry_header_size +
    (k_concurrency_innodb_lock_slot_count * k_concurrency_innodb_lock_slot_size);
constexpr std::size_t k_concurrency_redo_state_offset =
    ((k_concurrency_innodb_lock_registry_offset + k_concurrency_innodb_lock_registry_segment_size +
      63U) /
     64U) *
    64U;
constexpr std::size_t k_concurrency_redo_state_segment_size = MYLITE_OWNERLESS_REDO_STATE_SIZE;
constexpr std::size_t k_concurrency_redo_state_latch_offset = 0;
constexpr std::size_t k_concurrency_redo_state_latest_lsn_offset = 32;
constexpr std::size_t k_concurrency_redo_state_visible_lsn_offset =
    MYLITE_OWNERLESS_REDO_STATE_VISIBLE_LSN_OFFSET;
constexpr std::size_t k_concurrency_redo_state_refcount_offset = 48;
constexpr std::size_t k_concurrency_page_index_offset =
    k_concurrency_redo_state_offset + k_concurrency_redo_state_segment_size;
constexpr std::uint32_t k_concurrency_page_index_entry_count = 16384;
constexpr std::size_t k_concurrency_page_index_segment_size =
    MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE +
    (k_concurrency_page_index_entry_count * MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE);
constexpr std::size_t k_concurrency_dictionary_state_offset =
    ((k_concurrency_page_index_offset + k_concurrency_page_index_segment_size + 63U) / 64U) * 64U;
constexpr std::size_t k_concurrency_dictionary_state_segment_size =
    MYLITE_OWNERLESS_DICTIONARY_STATE_SIZE;
constexpr std::size_t k_concurrency_page_write_lock_registry_offset =
    ((k_concurrency_dictionary_state_offset + k_concurrency_dictionary_state_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_page_write_lock_slot_count = 2048;
constexpr std::size_t k_concurrency_page_write_lock_slot_size =
    MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_SLOT_SIZE;
constexpr std::size_t k_concurrency_page_write_lock_registry_segment_size =
    k_concurrency_innodb_lock_registry_header_size +
    (k_concurrency_page_write_lock_slot_count * k_concurrency_page_write_lock_slot_size);
constexpr std::size_t k_concurrency_autoinc_registry_offset =
    ((k_concurrency_page_write_lock_registry_offset +
      k_concurrency_page_write_lock_registry_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_autoinc_slot_count = 2048;
constexpr std::size_t k_concurrency_autoinc_registry_segment_size =
    MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE +
    (k_concurrency_autoinc_slot_count * MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE);
constexpr std::size_t k_concurrency_page_pin_registry_offset =
    ((k_concurrency_autoinc_registry_offset + k_concurrency_autoinc_registry_segment_size + 63U) /
     64U) *
    64U;
constexpr std::uint32_t k_concurrency_page_pin_slot_count = 128;
constexpr std::size_t k_concurrency_page_pin_registry_segment_size =
    MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE +
    (k_concurrency_page_pin_slot_count * MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE);
constexpr std::uint64_t k_empty_ownerless_page_log_size = static_cast<std::uint64_t>(
    k_concurrency_recovery_header_size + MYLITE_OWNERLESS_PAGE_LOG_HEADER_SIZE
);
constexpr std::uint32_t k_ownerless_innodb_record_page_heap_no =
    std::numeric_limits<std::uint32_t>::max() - 1U;
constexpr std::uint64_t k_concurrency_initial_trx_id = 1;
constexpr std::uint32_t k_concurrency_process_open_mode_exclusive = 1;
constexpr std::uint32_t k_concurrency_bootstrap_latch_owner_id =
    std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t k_concurrency_registry_slot_count_offset = 0;
constexpr std::size_t k_concurrency_registry_slot_size_offset = 4;
constexpr std::size_t k_concurrency_registry_active_count_offset = 16;
constexpr std::size_t k_concurrency_process_slot_wait_channel_offset = 64;
constexpr std::size_t k_concurrency_process_slot_wait_channel_count_offset = 72;
constexpr std::size_t k_concurrency_wait_header_channel_count_offset = 0;
constexpr std::size_t k_concurrency_wait_header_channel_size_offset = 4;
constexpr std::size_t k_concurrency_wait_header_generation_offset = 8;
constexpr std::size_t k_concurrency_mdl_lock_header_entry_count_offset = 0;
constexpr std::size_t k_concurrency_mdl_lock_header_entry_size_offset = 4;
constexpr std::size_t k_concurrency_mdl_lock_header_generation_offset = 8;
constexpr std::size_t k_concurrency_mdl_lock_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_trx_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_trx_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_trx_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_trx_header_next_id_offset = 24;
constexpr std::size_t k_concurrency_trx_header_oldest_active_offset = 64;
constexpr std::size_t k_concurrency_read_view_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_read_view_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_read_view_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_innodb_lock_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_innodb_lock_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_innodb_lock_header_active_count_offset = 16;
constexpr std::size_t k_concurrency_innodb_lock_header_waiting_count_offset = 64;
constexpr std::size_t k_concurrency_innodb_lock_header_occupied_limit_offset = 72;
constexpr std::size_t k_concurrency_page_index_header_entry_count_offset = 32;
constexpr std::size_t k_concurrency_page_index_header_entry_size_offset = 36;
constexpr std::size_t k_concurrency_page_index_header_active_count_offset = 40;
constexpr std::size_t k_concurrency_page_pin_header_slot_count_offset = 0;
constexpr std::size_t k_concurrency_page_pin_header_slot_size_offset = 4;
constexpr std::size_t k_concurrency_page_pin_header_active_count_offset = 16;
static_assert(
    k_concurrency_shm_segment_table_start +
            (k_concurrency_shm_segment_count * k_concurrency_shm_segment_descriptor_size) <=
        k_concurrency_process_registry_offset,
    "concurrency shared-memory segment table overlaps process registry"
);
static_assert(
    k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size <=
        static_cast<std::size_t>(k_minimum_concurrency_shm_size),
    "concurrency shared-memory segments exceed the minimum mapping size"
);
static_assert(
    k_concurrency_redo_state_latch_offset + MYLITE_OWNERLESS_LATCH_SIZE <=
        k_concurrency_redo_state_latest_lsn_offset,
    "redo state latch overlaps redo visibility state"
);
static_assert(
    k_concurrency_checkpoint_visible_lsn_offset ==
        k_concurrency_checkpoint_latest_lsn_offset + sizeof(std::uint64_t),
    "checkpoint visible LSN must follow checkpoint latest LSN"
);
static_assert(
    k_concurrency_redo_state_refcount_offset + sizeof(std::uint32_t) <=
        k_concurrency_redo_state_segment_size,
    "redo state refcount exceeds redo state segment"
);

struct RuntimeLayout {
    std::filesystem::path cleanup_directory;
    std::filesystem::path cleanup_tmp_directory;
    std::filesystem::path runtime_parent_directory;
    std::filesystem::path data_directory;
    std::filesystem::path tmp_directory;
    std::filesystem::path plugin_directory;
};

struct DatabaseLockWait {
    int lock_fd = -1;
    unsigned busy_timeout_ms = 0;
};

void release_fd_lock(int fd, off_t start, off_t length);
void release_concurrency_lock(int lock_fd, off_t start, off_t length);

struct OwnerlessStatementByteLock {
    int fd = -1;
    off_t start = 0;
    off_t length = 0;
};

struct OwnerlessStatementLockRequest {
    off_t start = 0;
    off_t length = 0;
    short lock_type = F_UNLCK;
};

struct OwnerlessStatementLocks {
    std::vector<OwnerlessStatementByteLock> locks;

    OwnerlessStatementLocks() = default;
    OwnerlessStatementLocks(const OwnerlessStatementLocks &) = delete;
    OwnerlessStatementLocks &operator=(const OwnerlessStatementLocks &) = delete;

    ~OwnerlessStatementLocks() {
        release();
    }

    void add(int fd, off_t start, off_t length) {
        locks.push_back({fd, start, length});
    }

    void release() {
        for (auto iter = locks.rbegin(); iter != locks.rend(); ++iter) {
            if (iter->fd >= 0) {
                release_fd_lock(iter->fd, iter->start, iter->length);
                iter->fd = -1;
            }
        }
        locks.clear();
    }
};

struct ParameterBinding {
    MYSQL_BIND bind = {};
    std::vector<unsigned char> bytes;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
    long long int64_value = 0;
    unsigned long long uint64_value = 0;
    double double_value = 0.0;
};

struct ResultColumn {
    MYSQL_BIND bind = {};
    enum enum_field_types field_type = MYSQL_TYPE_NULL;
    unsigned int flags = 0;
    std::string name;
    std::string org_name;
    std::string table;
    std::string org_table;
    std::vector<unsigned char> buffer;
    std::vector<unsigned char> retired_buffer;
    unsigned long length = 0;
    my_bool is_null = 0;
    my_bool error = 0;
};

struct SqlPolicyTokens {
    std::array<std::string_view, k_sql_policy_token_count> values = {};
    std::size_t count = 0;
};
#endif

#if MYLITE_WITH_MARIADB_EMBEDDED
struct OwnerlessMdlHookContext {
    void *lock_table = nullptr;
    std::size_t lock_table_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessTrxHookContext {
    void *trx_registry = nullptr;
    std::size_t trx_registry_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessReadViewHookContext {
    void *read_view_registry = nullptr;
    std::size_t read_view_registry_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessInnoDBLockHookContext {
    void *lock_registry = nullptr;
    std::size_t lock_registry_size = 0;
    void *autoinc_registry = nullptr;
    std::size_t autoinc_registry_size = 0;
    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0;
    void *page_pin_registry = nullptr;
    std::size_t page_pin_registry_size = 0;
    void *redo_state = nullptr;
    std::size_t redo_state_size = 0;
    void *page_index = nullptr;
    std::size_t page_index_size = 0;
    int page_log_fd = -1;
    std::uint64_t page_log_offset = 0;
    int checkpoint_fd = -1;
    const char *database_path = nullptr;
    bool page_log_reads_enabled = false;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessProcessCleanupContext {
    void *lock_table = nullptr;
    std::size_t lock_table_size = 0;
    void *trx_registry = nullptr;
    std::size_t trx_registry_size = 0;
    void *read_view_registry = nullptr;
    std::size_t read_view_registry_size = 0;
    void *page_pin_registry = nullptr;
    std::size_t page_pin_registry_size = 0;
    void *innodb_lock_registry = nullptr;
    std::size_t innodb_lock_registry_size = 0;
    void *page_write_lock_registry = nullptr;
    std::size_t page_write_lock_registry_size = 0;
    void *redo_state = nullptr;
    std::size_t redo_state_size = 0;
    void *dictionary_state = nullptr;
    std::size_t dictionary_state_size = 0;
    std::uint32_t latch_owner_id = 0;
    std::uint64_t latch_owner_generation = 0;
};

struct RuntimeState;

struct OwnerlessPageIndexRebuildContext {
    void *page_index = nullptr;
    std::size_t page_index_size = 0;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
};

struct OwnerlessPageLogReclaimContext {
    RuntimeState *runtime = nullptr;
    std::uint64_t visible_lsn = 0;
    std::vector<mylite_ownerless_page_index_record> retained_records;
};

struct OwnerlessPageVisibilityScope {
    ~OwnerlessPageVisibilityScope() {
        mylite_ownerless_innodb_clear_external_page_visibility();
    }
};

struct OwnerlessPressureState {
    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    std::uint64_t page_log_bytes = 0;
    std::uint64_t page_log_limit_bytes = 0;
    bool page_log_limit_reached = false;
};
#endif

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path cleanup_directory;
    std::filesystem::path cleanup_tmp_directory;
    std::filesystem::path runtime_parent_directory;
    std::string database_path;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
    int lock_fd = -1;
    bool ownerless_rw_mode = false;
    bool readonly_mode = false;
#if MYLITE_WITH_MARIADB_EMBEDDED
    int concurrency_shm_fd = -1;
    int concurrency_wal_fd = -1;
    int concurrency_checkpoint_fd = -1;
    int ownerless_statement_lock_fd = -1;
    void *concurrency_shm_mapping = nullptr;
    std::size_t concurrency_shm_mapping_size = 0;
    std::uint32_t concurrency_process_slot_index = 0;
    std::uint64_t concurrency_process_slot_generation = 0;
    OwnerlessMdlHookContext ownerless_mdl_hook = {};
    OwnerlessTrxHookContext ownerless_trx_hook = {};
    OwnerlessReadViewHookContext ownerless_read_view_hook = {};
    OwnerlessInnoDBLockHookContext ownerless_innodb_lock_hook = {};
#endif
};

RuntimeState g_runtime;
#if MYLITE_WITH_MARIADB_EMBEDDED
std::mutex g_system_table_mutex;
#endif

} // namespace

struct ErrorSnapshot {
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate;
    std::string errmsg;
};

enum class OwnerlessTransactionIsolation {
    ReadUncommitted,
    ReadCommitted,
    RepeatableRead,
    Serializable,
};

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
    std::string database_path;
    std::string current_schema;
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate = k_sqlstate_ok;
    std::string errmsg = k_not_an_error;
    std::string warning_message;
    long long changes = 0;
    unsigned long long last_insert_id = 0;
    unsigned active_statement_count = 0;
    std::uint64_t ownerless_observed_lsn = 0;
    std::uint64_t ownerless_observed_visible_lsn = 0;
    std::uint64_t ownerless_clean_pages_evicted_lsn = 0;
    std::uint64_t ownerless_transaction_snapshot_visible_lsn = 0;
    std::uint64_t ownerless_transaction_snapshot_pin_lsn = 0;
    std::uint64_t ownerless_transaction_snapshot_pin_generation = 0;
    std::uint64_t ownerless_observed_dictionary_generation = 0;
    std::uint64_t ownerless_page_log_limit_bytes = 0;
    std::vector<std::string> ownerless_temporary_table_names;
    OwnerlessTransactionIsolation ownerless_session_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    OwnerlessTransactionIsolation ownerless_next_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    OwnerlessTransactionIsolation ownerless_active_transaction_isolation =
        OwnerlessTransactionIsolation::RepeatableRead;
    bool ownerless_explicit_transaction_active = false;
    bool ownerless_transaction_has_local_write_or_locking_read = false;
    bool ownerless_transaction_snapshot_visibility_pinned = false;
    bool ownerless_transaction_snapshot_pin_registered = false;
    bool ownerless_next_transaction_isolation_set = false;
    bool ownerless_rw_open = false;
    bool readonly_open = false;
    bool connected = false;
    std::uint32_t ownerless_transaction_snapshot_pin_slot = 0;
};

struct mylite_stmt {
    mylite_db *db = nullptr;
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL_STMT *stmt = nullptr;
    MYSQL_RES *metadata = nullptr;
    std::vector<ParameterBinding> parameters;
    std::vector<MYSQL_BIND> parameter_binds;
    std::vector<ResultColumn> columns;
    std::vector<MYSQL_BIND> result_binds;
    std::string sql_text;
    bool result_binds_dirty = false;
    bool ownerless_page_visibility_enabled = false;
#endif
    bool executed = false;
    bool has_result = false;
    bool has_row = false;
};

namespace {

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
bool shared_readonly_open_available(void);
bool ownerless_rw_open_available(void);
#if MYLITE_WITH_MARIADB_EMBEDDED
int validate_runtime_database_path(mylite_db &db);
int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags);
int prepare_existing_database_directory(const std::filesystem::path &database_path, unsigned flags);
int validate_database_layout(const std::filesystem::path &database_path);
int validate_layout_directory(const std::filesystem::path &directory);
int validate_database_metadata(const std::filesystem::path &metadata_path);
int prepare_concurrency_metadata(const std::filesystem::path &database_path);
int prepare_concurrency_shared_memory(
    const std::filesystem::path &database_path,
    bool allow_recovery_rebuild
);
int read_concurrency_database_uuid(
    const std::filesystem::path &metadata_path,
    std::string &database_uuid
);
int prepare_concurrency_recovery_files(
    const std::filesystem::path &concurrency_directory,
    std::string_view database_uuid
);
int prepare_concurrency_recovery_file(
    const std::filesystem::path &file_path,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
int prepare_concurrency_checkpoint_file(
    const std::filesystem::path &file_path,
    std::string_view database_uuid
);
bool concurrency_recovery_header_matches(
    const std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
void build_concurrency_recovery_header(
    std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
);
int prepare_concurrency_shm_layout(
    const std::filesystem::path &database_path,
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    off_t shm_size,
    std::string_view database_uuid,
    bool allow_recovery_rebuild,
    bool initial_shared_memory
);
int replay_concurrency_tablespaces(
    const std::filesystem::path &database_path,
    int page_log_fd,
    int checkpoint_fd
);
bool concurrency_shm_header_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid
);
bool concurrency_shm_header_layout_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid
);
bool concurrency_shm_segments_match(int shm_fd, off_t shm_size);
bool concurrency_shm_rebuild_requires_recovery(int shm_fd, off_t shm_size);
bool concurrency_shm_header_identity_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    std::string_view database_uuid
);
void build_concurrency_shm_header(
    std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid,
    std::uint64_t recovery_generation
);
int initialize_concurrency_shm_segments(int shm_fd, int page_log_fd, int checkpoint_fd);
bool write_concurrency_segment_descriptor(
    int shm_fd,
    std::uint32_t index,
    std::uint32_t type,
    std::uint32_t version,
    std::uint64_t offset,
    std::uint64_t length
);
int initialize_concurrency_process_registry(int shm_fd);
int initialize_concurrency_wait_channels(int shm_fd);
int initialize_concurrency_mdl_lock_table(int shm_fd);
int initialize_concurrency_trx_registry(int shm_fd);
int initialize_concurrency_read_view_registry(int shm_fd);
int initialize_concurrency_page_pin_registry(int shm_fd);
int initialize_concurrency_innodb_lock_registry(int shm_fd);
int initialize_concurrency_page_write_lock_registry(int shm_fd);
int initialize_concurrency_redo_state(int shm_fd, int checkpoint_fd);
int initialize_concurrency_page_index(int shm_fd, int page_log_fd);
int initialize_concurrency_dictionary_state(int shm_fd);
int initialize_concurrency_autoinc_registry(int shm_fd);
void reclaim_ownerless_page_log_after_native_checkpoint(RuntimeState &runtime);
bool prepare_ownerless_page_log_native_checkpoint_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t visible_lsn
);
int prepare_ownerless_page_log_active_pin_reclaim(void *context);
void publish_ownerless_snapshot_boundary_if_needed(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t visible_lsn,
    std::uint32_t page_size
);
bool ownerless_runtime_has_no_live_peers(RuntimeState &runtime);
int snapshot_ownerless_page_version_pins(
    RuntimeState &runtime,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
);
int replay_concurrency_page_index(void *page_index, std::size_t page_index_size, int page_log_fd);
int replay_concurrency_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
int collect_ownerless_reclaimed_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
);
int replace_ownerless_page_index_after_reclaim(void *context);
int page_log_result_from_page_index_result(int result);
int read_concurrency_process_active_count(int shm_fd, std::uint64_t *out_active_count);
int read_concurrency_process_live_count(int shm_fd, std::uint64_t *out_live_count);
int validate_concurrency_shm_mapping(int shm_fd, off_t shm_size, std::string_view database_uuid);
int map_concurrency_shared_memory_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int open_concurrency_page_log_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int open_concurrency_checkpoint_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
);
int allocate_concurrency_process_slot(RuntimeState &runtime);
int install_ownerless_runtime_lifecycle_hooks(RuntimeState &runtime);
int install_ownerless_innodb_lock_hooks(RuntimeState &runtime);
int install_ownerless_runtime_hooks(RuntimeState &runtime);
int refresh_ownerless_external_pages_before_statement(
    mylite_db &db,
    bool allow_page_version_reads,
    bool allow_global_refresh
);
int read_ownerless_pressure_state(mylite_db &db, OwnerlessPressureState &state);
int enforce_ownerless_page_log_limit_policy(mylite_db &db, const SqlPolicyTokens &tokens);
int refresh_ownerless_dictionary_before_statement(mylite_db &db, bool allow_global_refresh);
int flush_ownerless_dictionary_cache(mylite_db &db);
void initialize_ownerless_dictionary_generation(mylite_db &db);
bool ownerless_dictionary_ddl_statement(std::string_view sql);
bool ownerless_dictionary_ddl_statement(const SqlPolicyTokens &tokens);
bool ownerless_temporary_table_ddl_statement(const SqlPolicyTokens &tokens);
bool ownerless_statement_uses_tracked_temporary_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool ownerless_statement_uses_temporary_table(const mylite_db &db, const SqlPolicyTokens &tokens);
std::string ownerless_temporary_table_name_from_ddl(const SqlPolicyTokens &tokens);
void update_ownerless_temporary_table_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
std::vector<OwnerlessStatementLockRequest> ownerless_autocommit_write_statement_lock_requests(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
);
int ownerless_begin_dictionary_ddl(mylite_db &db, std::string_view sql, bool *out_ddl_started);
int ownerless_finish_dictionary_ddl(mylite_db &db, bool ddl_started);
int ownerless_dictionary_result_from_state_result(int state_result);
bool ownerless_connection_is_in_explicit_transaction(const mylite_db &db);
bool ownerless_connection_allows_global_refresh(const mylite_db &db, bool allow_page_version_reads);
bool statement_allows_ownerless_page_version_reads(std::string_view sql);
int update_ownerless_transaction_state_after_successful_sql(mylite_db &db, std::string_view sql);
bool ownerless_transaction_pins_consistent_reads(const mylite_db &db);
int ensure_ownerless_consistent_snapshot_start_pin(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_pin_registered
);
int ensure_ownerless_transaction_page_version_pin(mylite_db &db, std::uint64_t read_lsn);
void release_ownerless_transaction_page_version_pin(mylite_db &db);
void update_ownerless_transaction_isolation_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
);
bool sql_sets_transaction_isolation(
    const SqlPolicyTokens &tokens,
    OwnerlessTransactionIsolation *out_isolation,
    bool *out_session_scope
);
bool sql_starts_consistent_snapshot_transaction(const SqlPolicyTokens &tokens);
bool sql_starts_explicit_transaction(const SqlPolicyTokens &tokens);
bool sql_ends_explicit_transaction(const SqlPolicyTokens &tokens);
bool sql_chains_transaction(const SqlPolicyTokens &tokens);
int acquire_ownerless_statement_locks(
    mylite_db &db,
    std::string_view sql,
    OwnerlessStatementLocks &lock
);
int ownerless_statement_lock_fd(mylite_db &db);
void unmap_concurrency_shared_memory_for_runtime(RuntimeState &runtime);
void reset_ownerless_runtime_hooks(RuntimeState &runtime);
void release_concurrency_owner_state(RuntimeState &runtime);
void release_concurrency_process_slot(RuntimeState &runtime);
int ownerless_mdl_acquire_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
);
void ownerless_mdl_release_hook(const mylite_ownerless_mdl_key_view *key, void *ctx);
unsigned ownerless_mdl_timeout_ms(double lock_wait_timeout);
int ownerless_mdl_result_from_lock_table_result(int lock_table_result);
int ownerless_trx_allocate_hook(std::uint64_t *out_trx_id, void *ctx);
int ownerless_trx_register_hook(std::uint64_t *out_trx_id, void *ctx);
int ownerless_trx_assign_no_hook(std::uint64_t trx_id, std::uint64_t *out_trx_no, void *ctx);
int ownerless_trx_deregister_hook(std::uint64_t trx_id, void *ctx);
int ownerless_trx_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no,
    void *ctx
);
int ownerless_trx_result_from_registry_result(int registry_result);
int ownerless_trx_deregister_result_from_registry_result(int registry_result);
int ownerless_read_view_register_hook(
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    unsigned int trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation,
    void *ctx
);
int ownerless_read_view_deregister_hook(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    void *ctx
);
int ownerless_read_view_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no,
    void *ctx
);
int ownerless_read_view_result_from_registry_result(int registry_result);
int ownerless_innodb_lock_acquire_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_release_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    void *ctx
);
int ownerless_innodb_lock_wait_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint64_t blocker_trx_id,
    void *ctx
);
int ownerless_innodb_lock_wait_until_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_acquire_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_release_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
int ownerless_innodb_lock_acquire_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    std::uint32_t *out_acquire_flags,
    void *ctx
);
int ownerless_innodb_lock_release_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
int ownerless_innodb_lock_release_page_writes_hook(std::uint64_t trx_id, void *ctx);
int ownerless_innodb_lock_wait_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint64_t blocker_trx_id,
    void *ctx
);
int ownerless_innodb_lock_wait_until_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
);
int ownerless_innodb_lock_before_record_wait_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
);
void normalize_ownerless_record_lock_resource(
    std::uint32_t mode,
    std::uint32_t *heap_no,
    std::uint32_t *flags
);
bool ownerless_record_lock_uses_page_resource(std::uint32_t mode, std::uint32_t flags);
int ownerless_innodb_lock_clear_wait_hook(std::uint64_t trx_id, void *ctx);
int ownerless_innodb_autoinc_read_hook(
    std::uint64_t table_id,
    std::uint64_t seed_next_value,
    std::uint64_t *out_next_value,
    void *ctx
);
int ownerless_innodb_autoinc_publish_hook(
    std::uint64_t table_id,
    std::uint64_t next_value,
    void *ctx
);
int ownerless_innodb_redo_enter_hook(std::uint64_t *out_latest_lsn, void *ctx);
int ownerless_innodb_redo_observe_hook(std::uint64_t *out_latest_lsn, void *ctx);
int ownerless_innodb_redo_reserve_hook(
    std::uint64_t current_lsn,
    std::uint64_t length,
    std::uint64_t *out_start_lsn,
    std::uint64_t *out_end_lsn,
    void *ctx
);
int ownerless_innodb_redo_written_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
);
void ownerless_innodb_redo_leave_hook(std::uint64_t latest_lsn, void *ctx);
void ownerless_innodb_pages_visible_hook(std::uint64_t visible_lsn, void *ctx);
void ownerless_persist_redo_checkpoint(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
);
void pause_for_ownerless_test_fault(const char *fault_name);
int ownerless_innodb_page_publish_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    void *ctx
);
int ownerless_innodb_page_read_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    void *ctx
);
int ownerless_innodb_page_read_locked(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
);
int ownerless_innodb_lock_result_from_registry_result(int registry_result);
int ownerless_innodb_lock_result_from_page_index_result(int index_result);
int ownerless_runtime_may_delete_shared_file_hook(void *ctx);
unsigned char *runtime_process_registry(RuntimeState &runtime);
unsigned char *runtime_trx_registry(RuntimeState &runtime);
unsigned char *runtime_read_view_registry(RuntimeState &runtime);
unsigned char *runtime_page_pin_registry(RuntimeState &runtime);
unsigned char *runtime_innodb_lock_registry(RuntimeState &runtime);
unsigned char *runtime_autoinc_registry(RuntimeState &runtime);
unsigned char *runtime_page_write_lock_registry(RuntimeState &runtime);
unsigned char *runtime_redo_state(RuntimeState &runtime);
unsigned char *runtime_page_index(RuntimeState &runtime);
unsigned char *runtime_dictionary_state(RuntimeState &runtime);
std::uint32_t ownerless_owner_id_from_slot_index(std::uint32_t slot_index);
int ownerless_process_is_alive(std::uint64_t pid, void *ctx);
int ownerless_process_cleanup_dead_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
);
int ownerless_process_cleanup_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
);
int ownerless_process_release_owner_page_write_locks(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
);
bool ownerless_process_owner_state_requires_recovery(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
);
int mylite_result_from_process_registry_result(int registry_result);
bool update_concurrency_shm_state(int shm_fd, std::uint32_t state);
bool update_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
);
bool read_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
);
bool acquire_fd_write_lock(int fd, off_t start, off_t length);
bool acquire_fd_range_lock(int fd, off_t start, off_t length, short lock_type, unsigned timeout_ms);
void release_fd_lock(int fd, off_t start, off_t length);
std::uint64_t current_time_milliseconds(void);
bool read_exact_at(int fd, unsigned char *data, std::size_t length, off_t offset);
bool write_exact_at(int fd, const unsigned char *data, std::size_t length, off_t offset);
std::uint32_t load_le32(const unsigned char *bytes, std::size_t offset);
std::uint64_t load_le64(const unsigned char *bytes, std::size_t offset);
void store_le32(unsigned char *bytes, std::size_t offset, std::uint32_t value);
void store_le64(unsigned char *bytes, std::size_t offset, std::uint64_t value);
std::uint64_t load_shared64(const unsigned char *bytes, std::size_t offset);
int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type
);
int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
);
int acquire_concurrency_lock(const std::filesystem::path &lock_path, off_t start, off_t length);
void release_concurrency_lock(int lock_fd, off_t start, off_t length);
int validate_concurrency_metadata(const std::filesystem::path &metadata_path);
bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
);
int start_runtime(mylite_db &db, unsigned flags, const mylite_open_config *config);
int connect_runtime(mylite_db &db);
int ensure_core_system_tables(mylite_db &db);
int execute_core_system_table_statements(mylite_db &db);
int execute_system_table_statement(mylite_db &db, const char *sql);
int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
);
int wait_for_database_lock(DatabaseLockWait wait);
void release_database_lock(int lock_fd);
unsigned configured_busy_timeout_ms(const mylite_open_config *config);
bool unsafe_disable_database_lock_for_tests(void);
void cleanup_runtime_layout(const RuntimeLayout &layout);
#endif
void cleanup_runtime_state(RuntimeState &runtime);
void clear_runtime_state(RuntimeState &runtime);
void remove_directory_if_empty(const std::filesystem::path &directory);
void close_connection(mylite_db &db);
void release_runtime(void);
int exec_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &db,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
);
int drain_remaining_query_results(mylite_db &db);
void rollback_active_transaction_after_deadlock(mylite_db &db);
int rollback_active_transaction(mylite_db &db);
ErrorSnapshot capture_error(const mylite_db &db);
void restore_error(mylite_db &db, const ErrorSnapshot &snapshot);
int initialize_statement_results(mylite_stmt &stmt, bool release_existing_results);
int fetch_statement_row(mylite_stmt &stmt);
int drain_remaining_statement_results(mylite_stmt &stmt);
int refresh_dirty_result_binds(mylite_stmt &stmt);
int fetch_truncated_statement_columns(mylite_stmt &stmt);
int configure_column_buffer(ResultColumn &column, unsigned long buffer_length);
int allocate_column_buffer(std::vector<unsigned char> &buffer, unsigned long buffer_length);
void release_statement_results(mylite_stmt &stmt);
void clear_statement_ownerless_page_visibility(mylite_stmt &stmt);
ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index);
int bind_null_value(mylite_stmt &stmt, unsigned index);
int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
);
void bind_parameter_buffer(ParameterBinding &parameter);
int bind_parameters(mylite_stmt &stmt);
mylite_value_type column_type(const ResultColumn &column);
const ResultColumn *metadata_column_at(const mylite_stmt *stmt, unsigned column);
const ResultColumn *value_column_at(const mylite_stmt *stmt, unsigned column);
void set_mariadb_statement_error(mylite_stmt &stmt);
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql);
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql);
bool is_unsupported_server_surface_sql(std::string_view sql, const std::string &current_schema);
bool is_readonly_rejected_sql_statement(const mylite_db &db, std::string_view sql);
bool sql_statement_requires_write(const SqlPolicyTokens &tokens);
bool sql_statement_requests_write_transaction(const SqlPolicyTokens &tokens);
bool sql_statement_uses_locking_read(const SqlPolicyTokens &tokens);
bool is_unsupported_oracle_sql_mode_statement(std::string_view sql);
bool is_unsupported_procedure_analyse_statement(std::string_view sql);
bool is_unsupported_vector_runtime_statement(std::string_view sql);
bool is_unsupported_xml_sql_function_statement(std::string_view sql);
bool is_unsupported_dynamic_column_statement(std::string_view sql);
bool is_unsupported_table_directory_option_statement(std::string_view sql);
bool is_unsupported_ownerless_engine_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_routine_ddl_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_sequence_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_table_admin_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_lock_tables_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_flush_table_lock_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_isolation_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_special_index_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_partition_statement(const mylite_db &db, std::string_view sql);
bool is_unsupported_ownerless_tablespace_management_statement(
    const mylite_db &db,
    std::string_view sql
);
bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_xa_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_replication_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_vector_sql_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_vector_index_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_xml_sql_function_call(const SqlPolicyTokens &tokens);
bool is_unsupported_dynamic_column_function_call(const SqlPolicyTokens &tokens);
bool is_unsupported_server_utility_function_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_sql_handler_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_select_file_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_load_file_import_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_help_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_processlist_metadata_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_thread_control_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_foreign_server_metadata_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_backup_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_userstat_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_user_variable_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_statement_profiling_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool is_unsupported_persistent_statistics_statement(const SqlPolicyTokens &tokens);
bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens);
bool sql_sets_non_innodb_storage_engine_variable(const SqlPolicyTokens &tokens);
bool sql_uses_non_innodb_table_engine(const SqlPolicyTokens &tokens);
bool sql_statement_can_use_table_engine_option(const SqlPolicyTokens &tokens);
bool sql_assigns_transaction_isolation_variable(const SqlPolicyTokens &tokens);
bool is_ownerless_storage_engine_variable(std::string_view token);
bool is_ownerless_supported_default_engine(std::string_view engine);
bool is_ownerless_supported_table_engine(std::string_view engine);
SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql);
bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token);
void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset);
bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset);
bool skip_block_sql_comment(std::string_view sql, std::size_t &offset);
void skip_quoted_sql_token(std::string_view sql, std::size_t &offset);
bool is_sql_space(char value);
bool is_sql_identifier_char(char value);
bool is_sql_identifier_token(std::string_view token);
std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index);
std::string_view unquoted_identifier_token(std::string_view token);
bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
);
bool has_information_schema_userstat_statistics_table(const SqlPolicyTokens &tokens);
bool has_current_schema_userstat_statistics_table_reference(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
);
bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name);
bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
);
bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name);
bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index);
bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index);
bool token_contains_sql_mode_name(std::string_view token, const char *mode_name);
bool token_equals(std::string_view token, const char *keyword);
bool identifier_token_equals(std::string_view token, const char *keyword);
bool table_reference_keyword(std::string_view token);
bool token_in(std::string_view token, const char *first, const char *second);
bool token_in(std::string_view token, const char *first, const char *second, const char *third);
bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
);
bool is_userstat_statistics_table_token(std::string_view token);
bool is_server_variable_token(std::string_view token);
bool is_query_log_variable_token(std::string_view token);
bool is_persistent_statistics_variable_token(std::string_view token);
bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index);
bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index);
std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens);
std::filesystem::path normalize_database_path(const char *path);
bool is_memory_database_path(const std::filesystem::path &database_path);
void initialize_database_layout(const std::filesystem::path &database_path);
void create_layout_directory(const std::filesystem::path &directory, const char *message);
void write_database_metadata(const std::filesystem::path &metadata_path);
void write_concurrency_metadata(const std::filesystem::path &metadata_path);
std::string generate_database_uuid(void);
void fill_database_uuid_bytes(std::array<unsigned char, 16> &bytes);
void fill_database_uuid_bytes_from_fallback(std::array<unsigned char, 16> &bytes);
bool is_database_uuid(std::string_view value);
bool is_unsigned_decimal(std::string_view value);
RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config,
    bool allow_stale_cleanup
);
RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config);
RuntimeLayout create_persistent_runtime_layout(
    const std::filesystem::path &database_path,
    bool allow_stale_cleanup
);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message);
std::vector<std::string> runtime_arguments(
    const RuntimeLayout &layout,
    bool ownerless_rw_open,
    bool readonly_open
);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
void remove_directory_contents_if_present(const std::filesystem::path &directory);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &db, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db);
#endif
void set_error(mylite_db &db, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db);
int parse_warning_level(const char *level);
#endif
const char *safe_c_str(const std::string &value);
bool has_config_field(const mylite_open_config *config, std::size_t field_end);

} // namespace

int mylite_open(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    return open_impl(path, out_db, flags, config);
}

unsigned long long mylite_capabilities(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    unsigned long long capabilities = MYLITE_CAP_SAME_PROCESS_CONCURRENCY;
    if (shared_readonly_open_available()) {
        capabilities |= MYLITE_CAP_SHARED_READONLY;
    }
    if (ownerless_rw_open_available()) {
        capabilities |= MYLITE_CAP_OWNERLESS_RW;
    }
    return capabilities;
#else
    return 0U;
#endif
}

int mylite_ownerless_pressure_status(mylite_db *db, mylite_ownerless_pressure_info *out_info) {
    if (db == nullptr || out_info == nullptr ||
        out_info->size < sizeof(mylite_ownerless_pressure_info)) {
        return MYLITE_MISUSE;
    }

    const std::size_t caller_size = out_info->size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->size = caller_size;

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*db);
    OwnerlessPressureState state;
    const int result = read_ownerless_pressure_state(*db, state);
    if (result != MYLITE_OK) {
        return result;
    }

    out_info->active_page_version_pin_count = state.active_pin_count;
    out_info->page_version_wal_limit_reached = state.page_log_limit_reached ? 1 : 0;
    out_info->oldest_page_version_pin_lsn = state.oldest_pin_lsn;
    out_info->page_version_wal_bytes = state.page_log_bytes;
    out_info->page_version_wal_limit_bytes = state.page_log_limit_bytes;
    return MYLITE_OK;
#endif
}

int mylite_close(mylite_db *db) {
    if (db == nullptr) {
        return MYLITE_OK;
    }
    if (db->active_statement_count > 0U) {
        set_error(*db, MYLITE_BUSY, "database has active statements");
        return MYLITE_BUSY;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    if (rollback_active_transaction(*db) != MYLITE_OK) {
        return MYLITE_ERROR;
    }
#endif
    close_connection(*db);
    release_runtime();
    delete db;
    return MYLITE_OK;
}

int mylite_exec(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    return exec_impl(db, sql, callback, ctx, errmsg);
}

int mylite_prepare(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    return prepare_impl(db, sql, sql_len, out_stmt, tail);
}

int mylite_step(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    if (!stmt->executed) {
        const SqlPolicyTokens policy_tokens = collect_sql_policy_tokens(stmt->sql_text);
        const int pressure_result =
            enforce_ownerless_page_log_limit_policy(*stmt->db, policy_tokens);
        if (pressure_result != MYLITE_OK) {
            return pressure_result;
        }
        const bool statement_uses_temporary_table =
            ownerless_statement_uses_temporary_table(*stmt->db, policy_tokens);
        OwnerlessStatementLocks statement_locks;
        const int statement_lock_result =
            acquire_ownerless_statement_locks(*stmt->db, stmt->sql_text, statement_locks);
        if (statement_lock_result != MYLITE_OK) {
            return statement_lock_result;
        }
        const bool allow_page_version_reads =
            !statement_uses_temporary_table &&
            statement_allows_ownerless_page_version_reads(stmt->sql_text);
        const int refresh_result = refresh_ownerless_external_pages_before_statement(
            *stmt->db,
            allow_page_version_reads,
            !statement_uses_temporary_table &&
                ownerless_connection_allows_global_refresh(*stmt->db, allow_page_version_reads)
        );
        if (refresh_result != MYLITE_OK) {
            return refresh_result;
        }
        stmt->ownerless_page_visibility_enabled = allow_page_version_reads;
        const int bind_result = bind_parameters(*stmt);
        if (bind_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return bind_result;
        }
        const int initial_result_setup = initialize_statement_results(*stmt, true);
        if (initial_result_setup != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return initial_result_setup;
        }
        bool dictionary_ddl_started = false;
        const int dictionary_ddl_result =
            ownerless_begin_dictionary_ddl(*stmt->db, stmt->sql_text, &dictionary_ddl_started);
        if (dictionary_ddl_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return dictionary_ddl_result;
        }
        bool consistent_snapshot_start_pin_registered = false;
        const int consistent_snapshot_pin_result = ensure_ownerless_consistent_snapshot_start_pin(
            *stmt->db,
            policy_tokens,
            &consistent_snapshot_start_pin_registered
        );
        if (consistent_snapshot_pin_result != MYLITE_OK) {
            const int dictionary_finish_result =
                ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
            if (dictionary_finish_result != MYLITE_OK) {
                set_error(
                    *stmt->db,
                    dictionary_finish_result,
                    "ownerless dictionary change could not finish after failed statement"
                );
                clear_statement_ownerless_page_visibility(*stmt);
                return dictionary_finish_result;
            }
            clear_statement_ownerless_page_visibility(*stmt);
            return consistent_snapshot_pin_result;
        }
        if (mysql_stmt_execute(stmt->stmt) != 0) {
            set_mariadb_statement_error(*stmt);
            const int dictionary_finish_result =
                ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
            if (dictionary_finish_result != MYLITE_OK) {
                set_error(
                    *stmt->db,
                    dictionary_finish_result,
                    "ownerless dictionary change could not finish after failed statement"
                );
                clear_statement_ownerless_page_visibility(*stmt);
                return dictionary_finish_result;
            }
            if (consistent_snapshot_start_pin_registered) {
                release_ownerless_transaction_page_version_pin(*stmt->db);
            }
            rollback_active_transaction_after_deadlock(*stmt->db);
            clear_statement_ownerless_page_visibility(*stmt);
            return MYLITE_ERROR;
        }
        stmt->db->changes = 0;
        stmt->db->last_insert_id =
            static_cast<unsigned long long>(mysql_stmt_insert_id(stmt->stmt));
        stmt->executed = true;
        update_ownerless_temporary_table_state_after_successful_sql(*stmt->db, policy_tokens);
        const int transaction_state_result =
            update_ownerless_transaction_state_after_successful_sql(*stmt->db, stmt->sql_text);
        if (transaction_state_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(*stmt);
            return transaction_state_result;
        }
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*stmt->db, dictionary_ddl_started);
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *stmt->db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish"
            );
            clear_statement_ownerless_page_visibility(*stmt);
            return dictionary_finish_result;
        }

        if (!stmt->has_result && mysql_stmt_field_count(stmt->stmt) != 0U) {
            const int result_setup = initialize_statement_results(*stmt, false);
            if (result_setup != MYLITE_OK) {
                clear_statement_ownerless_page_visibility(*stmt);
                return result_setup;
            }
        }
        if (!stmt->has_result) {
            const my_ulonglong affected_rows = mysql_stmt_affected_rows(stmt->stmt);
            stmt->db->changes = affected_rows == static_cast<my_ulonglong>(-1)
                                    ? 0
                                    : static_cast<long long>(std::min<my_ulonglong>(
                                          affected_rows,
                                          static_cast<my_ulonglong>(LLONG_MAX)
                                      ));
            clear_statement_ownerless_page_visibility(*stmt);
            return MYLITE_DONE;
        }
    }

    return stmt->has_result ? fetch_statement_row(*stmt) : MYLITE_DONE;
#endif
}

int mylite_reset(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    set_ok(*stmt->db);
    release_statement_results(*stmt);
    clear_statement_ownerless_page_visibility(*stmt);
    if (mysql_stmt_reset(stmt->stmt) != 0) {
        set_mariadb_statement_error(*stmt);
        return MYLITE_ERROR;
    }
    stmt->executed = false;
    stmt->has_result = false;
    stmt->has_row = false;
    return MYLITE_OK;
#endif
}

int mylite_finalize(mylite_stmt *stmt) {
    if (stmt == nullptr) {
        return MYLITE_OK;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    release_statement_results(*stmt);
    clear_statement_ownerless_page_visibility(*stmt);
    if (stmt->stmt != nullptr) {
        static_cast<void>(mysql_stmt_close(stmt->stmt));
    }
#endif
    if (stmt->db != nullptr && stmt->db->active_statement_count > 0U) {
        --stmt->db->active_statement_count;
    }
    delete stmt;
    return MYLITE_OK;
}

unsigned mylite_bind_parameter_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    return stmt != nullptr ? static_cast<unsigned>(stmt->parameters.size()) : 0U;
#endif
}

int mylite_clear_bindings(mylite_stmt *stmt) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    if (stmt->executed) {
        return MYLITE_MISUSE;
    }
    for (unsigned index = 1; index <= stmt->parameters.size(); ++index) {
        const int result = bind_null_value(*stmt, index);
        if (result != MYLITE_OK) {
            return result;
        }
    }
    return MYLITE_OK;
#endif
}

int mylite_bind_null(mylite_stmt *stmt, unsigned index) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_null_value(*stmt, index);
#endif
}

// Public binding APIs use SQLite-style index, value order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
int mylite_bind_int64(mylite_stmt *stmt, unsigned index, long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->int64_value = value;
    parameter->length = sizeof(parameter->int64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->int64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_uint64(mylite_stmt *stmt, unsigned index, unsigned long long value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->uint64_value = value;
    parameter->length = sizeof(parameter->uint64_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_LONGLONG;
    parameter->bind.buffer = &parameter->uint64_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    parameter->bind.is_unsigned = 1;
    return MYLITE_OK;
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): public binding API uses SQLite-style index,
// value order.
int mylite_bind_double(mylite_stmt *stmt, unsigned index, double value) {
    if (stmt == nullptr || stmt->db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    ParameterBinding *parameter = parameter_at(*stmt, index);
    if (parameter == nullptr || stmt->executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->double_value = value;
    parameter->length = sizeof(parameter->double_value);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_DOUBLE;
    parameter->bind.buffer = &parameter->double_value;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
#endif
}

// NOLINTEND(bugprone-easily-swappable-parameters)

int mylite_bind_text(
    mylite_stmt *stmt,
    unsigned index,
    const char *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U)) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len =
        value_len == MYLITE_NUL_TERMINATED ? std::strlen(value) : value_len;
    return bind_bytes(*stmt, index, value, resolved_len, MYSQL_TYPE_STRING, destructor);
#endif
}

int mylite_bind_blob(
    mylite_stmt *stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    mylite_destructor destructor
) {
    if (stmt == nullptr || stmt->db == nullptr || (value == nullptr && value_len != 0U) ||
        value_len == MYLITE_NUL_TERMINATED) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)value_len;
    (void)destructor;
    set_error(*stmt->db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    return bind_bytes(*stmt, index, value, value_len, MYSQL_TYPE_BLOB, destructor);
#endif
}

unsigned mylite_column_count(mylite_stmt *stmt) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    return 0U;
#else
    if (stmt == nullptr || stmt->stmt == nullptr) {
        return 0U;
    }
    if (!stmt->columns.empty()) {
        return static_cast<unsigned>(stmt->columns.size());
    }
    return mysql_stmt_field_count(stmt->stmt);
#endif
}

const char *mylite_column_name(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->name.c_str() : nullptr;
#endif
}

const char *mylite_column_org_name(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->org_name.c_str() : nullptr;
#endif
}

const char *mylite_column_table(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->table.c_str() : nullptr;
#endif
}

const char *mylite_column_org_table(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? result_column->org_table.c_str() : nullptr;
#endif
}

mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return MYLITE_TYPE_NULL;
#else
    const ResultColumn *result_column = metadata_column_at(stmt, column);
    return result_column != nullptr ? column_type(*result_column) : MYLITE_TYPE_NULL;
#endif
}

long long mylite_column_int64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoll(text, nullptr, k_decimal_base) : 0;
}

unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtoull(text, nullptr, k_decimal_base) : 0U;
}

double mylite_column_double(mylite_stmt *stmt, unsigned column) {
    const char *text = mylite_column_text(stmt, column);
    return text != nullptr ? std::strtod(text, nullptr) : 0.0;
}

const char *mylite_column_text(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return reinterpret_cast<const char *>(result_column->buffer.data());
#endif
}

const void *mylite_column_blob(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return nullptr;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return nullptr;
    }
    return result_column->buffer.data();
#endif
}

std::size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)stmt;
    (void)column;
    return 0U;
#else
    const ResultColumn *result_column = value_column_at(stmt, column);
    if (result_column == nullptr || result_column->is_null != 0) {
        return 0U;
    }
    return result_column->length;
#endif
}

int mylite_errcode(mylite_db *db) {
    return db != nullptr ? db->errcode : MYLITE_MISUSE;
}

int mylite_extended_errcode(mylite_db *db) {
    return db != nullptr ? db->extended_errcode : MYLITE_MISUSE;
}

unsigned mylite_mariadb_errno(mylite_db *db) {
    return db != nullptr ? db->mariadb_errno : 0;
}

const char *mylite_sqlstate(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->sqlstate) : k_sqlstate_general;
}

const char *mylite_errmsg(mylite_db *db) {
    return db != nullptr ? safe_c_str(db->errmsg) : k_bad_db_handle;
}

unsigned mylite_warning_count(mylite_db *db) {
#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)db;
    return 0U;
#else
    return db != nullptr ? mysql_warning_count(&db->mysql) : 0U;
#endif
}

// NOLINTBEGIN(readability-non-const-parameter): output parameters are part of the public C API.
int mylite_warning(
    mylite_db *db,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message
) {
    if (db == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)index;
    (void)level;
    (void)code;
    (void)message;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const unsigned warning_count = mysql_warning_count(&db->mysql);
    if (index >= warning_count) {
        return MYLITE_NOTFOUND;
    }

    const std::string sql = "SHOW WARNINGS LIMIT " + std::to_string(index) + ", 1";
    if (mysql_query(&db->mysql, sql.c_str()) != 0) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_RES *result = mysql_store_result(&db->mysql);
    if (result == nullptr) {
        set_mariadb_error(*db);
        return MYLITE_ERROR;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (row == nullptr) {
        mysql_free_result(result);
        return MYLITE_NOTFOUND;
    }

    if (level != nullptr) {
        *level = static_cast<mylite_warning_level>(parse_warning_level(row[0]));
    }
    if (code != nullptr) {
        *code = static_cast<unsigned>(
            std::strtoul(row[1] != nullptr ? row[1] : "0", nullptr, k_decimal_base)
        );
    }
    db->warning_message = row[2] != nullptr ? row[2] : "";
    if (message != nullptr) {
        *message = db->warning_message.c_str();
    }

    mysql_free_result(result);
    set_ok(*db);
    return MYLITE_OK;
#endif
}

// NOLINTEND(readability-non-const-parameter)

long long mylite_changes(mylite_db *db) {
    return db != nullptr ? db->changes : 0;
}

unsigned long long mylite_last_insert_id(mylite_db *db) {
    return db != nullptr ? db->last_insert_id : 0;
}

void mylite_free(void *ptr) {
    std::free(ptr);
}

namespace {

int open_impl(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    const int validation_result = validate_open_args(path, out_db, flags, config);
    if (validation_result != MYLITE_OK) {
        return validation_result;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)config;
    return MYLITE_ERROR;
#else
    try {
        std::unique_ptr<mylite_db> db(new mylite_db());
        db->database_path = normalize_database_path(path).string();
        db->ownerless_rw_open =
            (flags & (MYLITE_OPEN_OWNERLESS_RW | MYLITE_OPEN_SHARED_READONLY)) != 0U;
        db->readonly_open = (flags & MYLITE_OPEN_READONLY) != 0U;
        if (has_config_field(
                config,
                offsetof(mylite_open_config, ownerless_page_log_limit_bytes) +
                    sizeof(config->ownerless_page_log_limit_bytes)
            )) {
            db->ownerless_page_log_limit_bytes =
                static_cast<std::uint64_t>(config->ownerless_page_log_limit_bytes);
        }

        const int runtime_path_result = validate_runtime_database_path(*db);
        if (runtime_path_result != MYLITE_OK) {
            return runtime_path_result;
        }

        const int directory_result = prepare_database_directory(db->database_path, flags);
        if (directory_result != MYLITE_OK) {
            return directory_result;
        }

        const int runtime_result = start_runtime(*db, flags, config);
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        const int connect_result = connect_runtime(*db);
        if (connect_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return connect_result;
        }

        const int system_tables_result = ensure_core_system_tables(*db);
        if (system_tables_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return system_tables_result;
        }
        initialize_ownerless_dictionary_generation(*db);

        *out_db = db.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return MYLITE_IOERR;
    }
#endif
}

int validate_open_args(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    if (out_db == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_db = nullptr;

    if (path == nullptr || path[0] == '\0') {
        return MYLITE_MISUSE;
    }

    if ((flags & ~k_known_open_flags) != 0U) {
        return MYLITE_MISUSE;
    }

    const bool readonly = (flags & MYLITE_OPEN_READONLY) != 0U;
    const bool readwrite = (flags & MYLITE_OPEN_READWRITE) != 0U;
    if (readonly == readwrite) {
        return MYLITE_MISUSE;
    }

    const bool shared_readonly = (flags & MYLITE_OPEN_SHARED_READONLY) != 0U;
    const bool ownerless_rw = (flags & MYLITE_OPEN_OWNERLESS_RW) != 0U;

    if (readonly && ((flags & (MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE)) != 0U)) {
        return MYLITE_MISUSE;
    }

    if (readonly && (!shared_readonly || ownerless_rw)) {
        return MYLITE_MISUSE;
    }

    if (readonly && std::strcmp(path, k_memory_database_path) == 0) {
        return MYLITE_MISUSE;
    }

    if (shared_readonly && !readonly) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_URI) != 0U) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_SHARED_READONLY) != 0U && !shared_readonly_open_available()) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_OWNERLESS_RW) != 0U && !ownerless_rw_open_available()) {
        return MYLITE_MISUSE;
    }

    if (config != nullptr && config->size > 0U) {
        if (has_config_field(
                config,
                offsetof(mylite_open_config, profile) + sizeof(config->profile)
            ) &&
            config->profile != MYLITE_PROFILE_DEFAULT && config->profile != MYLITE_PROFILE_STRICT &&
            config->profile != MYLITE_PROFILE_COMPAT) {
            return MYLITE_MISUSE;
        }

        if (has_config_field(
                config,
                offsetof(mylite_open_config, durability) + sizeof(config->durability)
            ) &&
            config->durability != MYLITE_DURABILITY_OFF &&
            config->durability != MYLITE_DURABILITY_NORMAL &&
            config->durability != MYLITE_DURABILITY_FULL) {
            return MYLITE_MISUSE;
        }
    }

    return MYLITE_OK;
}

bool shared_readonly_open_available(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    return true;
#else
    return false;
#endif
}

bool ownerless_rw_open_available(void) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    return true;
#else
    return false;
#endif
}

int exec_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)callback;
    (void)ctx;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return copy_error_message(*db, errmsg);
#else
    set_ok(*db);
    const OwnerlessPageVisibilityScope page_visibility_scope;
    if (reject_unsupported_sql_policy(*db, sql) != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    const SqlPolicyTokens policy_tokens = collect_sql_policy_tokens(sql);
    const int pressure_result = enforce_ownerless_page_log_limit_policy(*db, policy_tokens);
    if (pressure_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    const bool statement_uses_temporary_table =
        ownerless_statement_uses_temporary_table(*db, policy_tokens);
    OwnerlessStatementLocks statement_locks;
    const int statement_lock_result = acquire_ownerless_statement_locks(*db, sql, statement_locks);
    if (statement_lock_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    const bool allow_page_version_reads =
        !statement_uses_temporary_table && statement_allows_ownerless_page_version_reads(sql);
    const int refresh_result = refresh_ownerless_external_pages_before_statement(
        *db,
        allow_page_version_reads,
        !statement_uses_temporary_table &&
            ownerless_connection_allows_global_refresh(*db, allow_page_version_reads)
    );
    if (refresh_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    bool dictionary_ddl_started = false;
    const int dictionary_ddl_result =
        ownerless_begin_dictionary_ddl(*db, sql, &dictionary_ddl_started);
    if (dictionary_ddl_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    bool consistent_snapshot_start_pin_registered = false;
    const int consistent_snapshot_pin_result = ensure_ownerless_consistent_snapshot_start_pin(
        *db,
        policy_tokens,
        &consistent_snapshot_start_pin_registered
    );
    if (consistent_snapshot_pin_result != MYLITE_OK) {
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish after failed statement"
            );
        }
        return copy_error_message(*db, errmsg);
    }
    if (mysql_query(&db->mysql, sql) != 0) {
        set_mariadb_error(*db);
        const int dictionary_finish_result =
            ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
        if (dictionary_finish_result != MYLITE_OK) {
            set_error(
                *db,
                dictionary_finish_result,
                "ownerless dictionary change could not finish after failed statement"
            );
        }
        if (consistent_snapshot_start_pin_registered) {
            release_ownerless_transaction_page_version_pin(*db);
        }
        rollback_active_transaction_after_deadlock(*db);
        return copy_error_message(*db, errmsg);
    }
    const my_ulonglong affected_rows = mysql_affected_rows(&db->mysql);
    const unsigned long long insert_id =
        static_cast<unsigned long long>(mysql_insert_id(&db->mysql));

    bool has_result = false;
    const int result = store_and_emit_result(*db, callback, ctx, &has_result);
    if (result != MYLITE_OK) {
        if (ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started) != MYLITE_OK) {
            set_error(*db, MYLITE_IOERR, "ownerless dictionary change could not finish");
        }
        return copy_error_message(*db, errmsg);
    }
    update_current_schema_after_successful_sql(*db, sql);
    update_ownerless_temporary_table_state_after_successful_sql(*db, policy_tokens);
    const int transaction_state_result =
        update_ownerless_transaction_state_after_successful_sql(*db, sql);
    if (transaction_state_result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }
    const int dictionary_finish_result =
        ownerless_finish_dictionary_ddl(*db, dictionary_ddl_started);
    if (dictionary_finish_result != MYLITE_OK) {
        set_error(*db, dictionary_finish_result, "ownerless dictionary change could not finish");
        return copy_error_message(*db, errmsg);
    }

    db->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    db->last_insert_id = insert_id;
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void update_current_schema_after_successful_sql(mylite_db &db, std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "USE") || tokens.count < 2U) {
        return;
    }
    db.current_schema = std::string(unquoted_identifier_token(tokens.values[1]));
}

#endif

int prepare_impl(
    mylite_db *db,
    const char *sql,
    std::size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
) {
    if (out_stmt == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_stmt = nullptr;
    if (tail != nullptr) {
        *tail = nullptr;
    }
    if (db == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)sql_len;
    set_error(*db, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#else
    const std::size_t resolved_len = sql_len == MYLITE_NUL_TERMINATED ? std::strlen(sql) : sql_len;
    if (resolved_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }
    set_ok(*db);
    const int policy_result =
        reject_unsupported_sql_policy(*db, std::string_view(sql, resolved_len));
    if (policy_result != MYLITE_OK) {
        return policy_result;
    }
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(std::string_view(sql, resolved_len));
    if (token_equals(identifier_token_at(tokens, 0), "CALL")) {
        set_error(*db, MYLITE_ERROR, "prepared CALL statements are not supported by MyLite");
        return MYLITE_ERROR;
    }
    const bool statement_uses_temporary_table =
        ownerless_statement_uses_temporary_table(*db, tokens);
    const int refresh_result = refresh_ownerless_external_pages_before_statement(
        *db,
        false,
        !statement_uses_temporary_table && ownerless_connection_allows_global_refresh(*db, false)
    );
    if (refresh_result != MYLITE_OK) {
        return refresh_result;
    }

    std::unique_ptr<mylite_stmt> statement(new mylite_stmt());
    statement->db = db;
    statement->sql_text = std::string(sql, resolved_len);
    statement->stmt = mysql_stmt_init(&db->mysql);
    if (statement->stmt == nullptr) {
        set_error(*db, MYLITE_NOMEM, "statement could not be allocated");
        return MYLITE_NOMEM;
    }

    my_bool update_max_length = 1;
    static_cast<void>(
        mysql_stmt_attr_set(statement->stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length)
    );

    if (mysql_stmt_prepare(statement->stmt, sql, static_cast<unsigned long>(resolved_len)) != 0) {
        set_mariadb_statement_error(*statement);
        static_cast<void>(mysql_stmt_close(statement->stmt));
        statement->stmt = nullptr;
        return MYLITE_ERROR;
    }

    statement->parameters.resize(mysql_stmt_param_count(statement->stmt));
    for (unsigned index = 1; index <= statement->parameters.size(); ++index) {
        const int result = bind_null_value(*statement, index);
        if (result != MYLITE_OK) {
            static_cast<void>(mysql_stmt_close(statement->stmt));
            statement->stmt = nullptr;
            return result;
        }
    }

    if (tail != nullptr) {
        *tail = sql + resolved_len;
    }
    ++db->active_statement_count;
    *out_stmt = statement.release();
    set_ok(*db);
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int reject_unsupported_sql_policy(mylite_db &db, std::string_view sql) {
    if (is_readonly_rejected_sql_statement(db, sql)) {
        set_error(db, MYLITE_READONLY, "database is open read-only");
        return MYLITE_READONLY;
    }

    if (is_unsupported_oracle_sql_mode_statement(sql)) {
        set_error(db, MYLITE_ERROR, "Oracle SQL mode is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_procedure_analyse_statement(sql)) {
        set_error(db, MYLITE_ERROR, "PROCEDURE ANALYSE is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_vector_runtime_statement(sql)) {
        set_error(db, MYLITE_ERROR, "vector SQL runtime is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_xml_sql_function_statement(sql)) {
        set_error(db, MYLITE_ERROR, "XML SQL functions are not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_dynamic_column_statement(sql)) {
        set_error(db, MYLITE_ERROR, "dynamic columns are not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_table_directory_option_statement(sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "table DATA DIRECTORY and INDEX DIRECTORY options are not supported by MyLite"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_engine_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode currently supports InnoDB tables only"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_server_surface_sql(sql, db.current_schema)) {
        set_error(db, MYLITE_ERROR, "server-owned SQL surface is not supported by MyLite");
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_routine_ddl_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode does not support stored routine DDL"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_sequence_statement(db, sql)) {
        set_error(db, MYLITE_ERROR, "ownerless read/write mode does not support sequence SQL");
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_table_admin_statement(db, sql)) {
        set_error(db, MYLITE_ERROR, "ownerless read/write mode does not support table admin SQL");
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_lock_tables_statement(db, sql)) {
        set_error(db, MYLITE_ERROR, "ownerless read/write mode does not support LOCK TABLES");
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_flush_table_lock_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode does not support FLUSH TABLES locks or export"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_isolation_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless mode does not support READ UNCOMMITTED or isolation variable assignments"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_partition_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode does not support partitioned table DDL"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_tablespace_management_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode does not support DISCARD/IMPORT TABLESPACE"
        );
        return MYLITE_ERROR;
    }

    if (is_unsupported_ownerless_special_index_statement(db, sql)) {
        set_error(
            db,
            MYLITE_ERROR,
            "ownerless read/write mode does not support FULLTEXT or SPATIAL index DDL"
        );
        return MYLITE_ERROR;
    }

    return MYLITE_OK;
}

bool is_readonly_rejected_sql_statement(const mylite_db &db, std::string_view sql) {
    if (!db.readonly_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return sql_statement_requires_write(tokens) ||
           sql_statement_requests_write_transaction(tokens) ||
           sql_statement_uses_locking_read(tokens);
}

bool sql_statement_requires_write(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (first.empty()) {
        return false;
    }

    if (token_in(first, "ALTER", "ANALYZE", "CALL", "CREATE") ||
        token_in(first, "DELETE", "DO", "DROP", "EXECUTE") ||
        token_in(first, "GRANT", "HANDLER", "IMPORT", "INSERT") ||
        token_in(first, "INSTALL", "LOAD", "LOCK", "OPTIMIZE") ||
        token_in(first, "PREPARE", "RENAME", "REPAIR", "REPLACE") ||
        token_in(first, "REVOKE", "TRUNCATE", "UNINSTALL", "UPDATE")) {
        return true;
    }

    if (token_equals(first, "SET")) {
        for (std::size_t index = 1; index < tokens.count; ++index) {
            if (token_in(identifier_token_at(tokens, index), "GLOBAL", "PERSIST", "PERSIST_ONLY")) {
                return true;
            }
        }
    }

    if (token_in(first, "SELECT", "SHOW", "DESCRIBE", "DESC") ||
        token_in(first, "EXPLAIN", "USE", "SET", "START") ||
        token_in(first, "BEGIN", "COMMIT", "ROLLBACK", "VALUES") || token_equals(first, "TABLE")) {
        return false;
    }

    if (!token_equals(first, "WITH")) {
        return true;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(identifier_token_at(tokens, index), "DELETE", "INSERT", "REPLACE", "UPDATE")) {
            return true;
        }
    }
    return false;
}

bool sql_statement_requests_write_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "SET", "START")) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "READ") &&
            token_equals(identifier_token_at(tokens, index + 1U), "WRITE")) {
            return true;
        }
    }
    return false;
}

bool sql_statement_uses_locking_read(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "FOR") &&
            token_equals(identifier_token_at(tokens, index + 1U), "UPDATE")) {
            return true;
        }
        if (index + 3U < tokens.count && token_equals(identifier_token_at(tokens, index), "LOCK") &&
            token_equals(identifier_token_at(tokens, index + 1U), "IN") &&
            token_equals(identifier_token_at(tokens, index + 2U), "SHARE") &&
            token_equals(identifier_token_at(tokens, index + 3U), "MODE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_oracle_sql_mode_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "SQL_MODE") &&
            is_sql_mode_assignment_target(tokens, index) &&
            sql_mode_assignment_mentions_oracle(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_procedure_analyse_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "SELECT")) {
        return false;
    }

    for (std::size_t index = 1; index + 2U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PROCEDURE") &&
            token_equals(tokens.values[index + 1U], "ANALYSE") &&
            token_equals(tokens.values[index + 2U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_runtime_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return is_unsupported_vector_sql_function_statement(tokens) ||
           is_unsupported_vector_index_statement(tokens);
}

bool is_unsupported_xml_sql_function_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return is_unsupported_xml_sql_function_call(tokens);
}

bool is_unsupported_dynamic_column_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return is_unsupported_dynamic_column_function_call(tokens);
}

bool is_unsupported_table_directory_option_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            continue;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "DATA", "INDEX") &&
            token_equals(identifier_token_at(tokens, index + 1U), "DIRECTORY")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_engine_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return sql_sets_non_innodb_storage_engine_variable(tokens) ||
           sql_uses_non_innodb_table_engine(tokens);
}

bool is_unsupported_ownerless_routine_ddl_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE", "DROP")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count && index < 12U; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token_in(token, "FUNCTION", "PROCEDURE")) {
            return true;
        }
        if (token_in(token, "DATABASE", "EVENT", "INDEX", "ROLE") ||
            token_in(token, "SCHEMA", "SEQUENCE", "SERVER", "TABLE") ||
            token_in(token, "TRIGGER", "USER", "VIEW")) {
            return false;
        }
    }
    return false;
}

bool is_unsupported_ownerless_sequence_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_in(first, "ALTER", "CREATE", "DROP")) {
        for (std::size_t index = 1U; index < tokens.count && index < 12U; ++index) {
            const std::string_view token = identifier_token_at(tokens, index);
            if (token_equals(token, "SEQUENCE")) {
                return true;
            }
            if (token_in(token, "DATABASE", "EVENT", "FUNCTION", "INDEX") ||
                token_in(token, "PROCEDURE", "ROLE", "SCHEMA", "SERVER") ||
                token_in(token, "TABLE", "TRIGGER", "USER", "VIEW")) {
                return false;
            }
        }
    }

    for (std::size_t index = 0; index < tokens.count; ++index) {
        if (index + 2U < tokens.count && token_in(tokens.values[index], "NEXT", "PREVIOUS") &&
            token_equals(tokens.values[index + 1U], "VALUE") &&
            token_equals(tokens.values[index + 2U], "FOR")) {
            return true;
        }
        if (index + 1U < tokens.count &&
            token_in(tokens.values[index], "LASTVAL", "NEXTVAL", "SETVAL") &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_table_admin_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ANALYZE", "CHECK", "CHECKSUM", "OPTIMIZE") &&
        !token_equals(first, "REPAIR")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "TABLE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_lock_tables_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    if (token_in(first, "LOCK", "UNLOCK")) {
        return token_in(second, "TABLE", "TABLES");
    }
    return false;
}

bool is_unsupported_ownerless_flush_table_lock_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "FLUSH")) {
        return false;
    }

    std::size_t table_index = 1U;
    if (token_in(identifier_token_at(tokens, table_index), "LOCAL", "NO_WRITE_TO_BINLOG")) {
        ++table_index;
    }
    if (!token_in(identifier_token_at(tokens, table_index), "TABLE", "TABLES")) {
        return false;
    }

    for (std::size_t index = table_index + 1U; index < tokens.count; ++index) {
        if (index + 2U < tokens.count && token_equals(identifier_token_at(tokens, index), "WITH") &&
            token_equals(identifier_token_at(tokens, index + 1U), "READ") &&
            token_equals(identifier_token_at(tokens, index + 2U), "LOCK")) {
            return true;
        }
        if (index + 1U < tokens.count && token_equals(identifier_token_at(tokens, index), "FOR") &&
            token_equals(identifier_token_at(tokens, index + 1U), "EXPORT")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_isolation_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    OwnerlessTransactionIsolation isolation = OwnerlessTransactionIsolation::RepeatableRead;
    bool session_scope = false;
    return (sql_sets_transaction_isolation(tokens, &isolation, &session_scope) &&
            isolation == OwnerlessTransactionIsolation::ReadUncommitted) ||
           sql_assigns_transaction_isolation_variable(tokens);
}

bool is_unsupported_ownerless_special_index_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    for (std::size_t index = 1U; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "FULLTEXT", "SPATIAL")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_partition_statement(const mylite_db &db, std::string_view sql) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "ALTER", "CREATE")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            break;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "PARTITION", "PARTITIONING", "PARTITIONS") ||
            token_in(token, "SUBPARTITION", "SUBPARTITIONING", "SUBPARTITIONS")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_ownerless_tablespace_management_statement(
    const mylite_db &db,
    std::string_view sql
) {
    if (!db.ownerless_rw_open) {
        return false;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (!token_equals(identifier_token_at(tokens, 0), "ALTER")) {
        return false;
    }

    bool found_table = false;
    for (std::size_t index = 1U; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token.empty()) {
            break;
        }
        if (!found_table) {
            found_table = token_equals(token, "TABLE");
            continue;
        }
        if (token_in(token, "DISCARD", "IMPORT") &&
            token_equals(identifier_token_at(tokens, index + 1U), "TABLESPACE")) {
            return true;
        }
    }
    return false;
}

bool sql_sets_non_innodb_storage_engine_variable(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index + 2U < tokens.count; ++index) {
        if (is_ownerless_storage_engine_variable(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index) &&
            !is_ownerless_supported_default_engine(tokens.values[index + 2U])) {
            return true;
        }
    }
    return false;
}

bool sql_uses_non_innodb_table_engine(const SqlPolicyTokens &tokens) {
    if (!sql_statement_can_use_table_engine_option(tokens)) {
        return false;
    }

    int paren_depth = 0;
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(tokens.values[index], ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth != 0 || !identifier_token_equals(tokens.values[index], "ENGINE")) {
            continue;
        }

        std::size_t engine_index = index + 1U;
        if (token_equals(tokens.values[engine_index], "=")) {
            ++engine_index;
        }
        if (engine_index >= tokens.count) {
            continue;
        }
        if (!is_ownerless_supported_table_engine(tokens.values[engine_index])) {
            return true;
        }
    }
    return false;
}

bool sql_statement_can_use_table_engine_option(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "CREATE", "ALTER")) {
        return false;
    }

    for (std::size_t index = 1U; index < 6U; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "TABLE")) {
            return true;
        }
    }
    return false;
}

bool sql_assigns_transaction_isolation_variable(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1U; index + 2U < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "TX_ISOLATION") ||
            identifier_token_equals(tokens.values[index], "TRANSACTION_ISOLATION")) {
            if (is_system_variable_qualified_token(tokens, index)) {
                return true;
            }
        }
    }
    return false;
}

bool is_ownerless_storage_engine_variable(std::string_view token) {
    return identifier_token_equals(token, "DEFAULT_STORAGE_ENGINE") ||
           identifier_token_equals(token, "STORAGE_ENGINE") ||
           identifier_token_equals(token, "DEFAULT_TMP_STORAGE_ENGINE") ||
           identifier_token_equals(token, "ENFORCE_STORAGE_ENGINE");
}

bool is_ownerless_supported_default_engine(std::string_view engine) {
    return identifier_token_equals(engine, "INNODB") || identifier_token_equals(engine, "DEFAULT");
}

bool is_ownerless_supported_table_engine(std::string_view engine) {
    return identifier_token_equals(engine, "INNODB");
}

bool is_unsupported_server_surface_sql(std::string_view sql, const std::string &current_schema) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    if (identifier_token_at(tokens, 0).empty()) {
        return false;
    }

    return is_unsupported_account_or_event_statement(tokens) ||
           is_unsupported_plugin_statement(tokens) || is_unsupported_udf_statement(tokens) ||
           is_unsupported_replication_statement(tokens) ||
           is_unsupported_binlog_statement(tokens) || is_unsupported_xa_statement(tokens) ||
           is_unsupported_replication_function_statement(tokens) ||
           is_unsupported_server_utility_function_statement(tokens) ||
           is_unsupported_sql_handler_statement(tokens) ||
           is_unsupported_select_file_statement(tokens) ||
           is_unsupported_load_file_import_statement(tokens) ||
           is_unsupported_help_statement(tokens) ||
           is_unsupported_static_show_info_statement(tokens) ||
           is_unsupported_processlist_metadata_statement(tokens) ||
           is_unsupported_thread_control_statement(tokens) ||
           is_unsupported_foreign_server_metadata_statement(tokens) ||
           is_unsupported_backup_statement(tokens) ||
           is_unsupported_userstat_diagnostics_statement(tokens, current_schema) ||
           is_unsupported_user_variable_diagnostics_statement(tokens, current_schema) ||
           is_unsupported_statement_profiling_statement(tokens, current_schema) ||
           is_unsupported_query_cache_statement(tokens) ||
           is_unsupported_query_log_statement(tokens) ||
           is_unsupported_optimizer_trace_statement(tokens, current_schema) ||
           is_unsupported_persistent_statistics_statement(tokens) ||
           is_unsupported_server_set_statement(tokens);
}

bool is_unsupported_account_or_event_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    const std::string_view fourth = identifier_token_at(tokens, 3);

    if (token_in(first, "GRANT", "REVOKE")) {
        return true;
    }
    if (token_equals(first, "CREATE")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
            return token_equals(fourth, "DEFINER")
                       ? has_identifier_token(tokens, "EVENT", 4)
                       : token_in(fourth, "USER", "ROLE", "EVENT", "SERVER");
        }
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    if (token_equals(first, "ALTER")) {
        if (token_equals(second, "DEFINER")) {
            return has_identifier_token(tokens, "EVENT", 2);
        }
        return token_in(second, "USER", "EVENT", "SERVER");
    }
    if (token_equals(first, "DROP")) {
        return token_in(second, "USER", "ROLE", "EVENT", "SERVER");
    }
    if (token_equals(first, "SHOW")) {
        return token_equals(second, "EVENTS") ||
               (token_equals(second, "CREATE") && token_equals(third, "EVENT"));
    }
    return token_equals(first, "RENAME") && token_equals(second, "USER");
}

bool is_unsupported_plugin_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    return (token_equals(first, "INSTALL") || token_equals(first, "UNINSTALL")) &&
           token_in(second, "PLUGIN", "SONAME");
}

bool is_unsupported_udf_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);
    std::size_t function_index = 1;

    if (!token_equals(first, "CREATE")) {
        return false;
    }
    if (token_equals(second, "OR") && token_equals(third, "REPLACE")) {
        function_index = 3;
    }
    if (token_equals(identifier_token_at(tokens, function_index), "AGGREGATE")) {
        ++function_index;
    }
    return token_equals(identifier_token_at(tokens, function_index), "FUNCTION") &&
           has_identifier_token(tokens, "SONAME", function_index + 1U);
}

bool is_unsupported_replication_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "CHANGE") && token_in(second, "MASTER", "REPLICATION")) {
        return true;
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_in(first, "START", "STOP", "RESET") && token_in(second, "SLAVE", "REPLICA");
}

bool is_unsupported_binlog_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    if (token_equals(first, "BINLOG")) {
        return true;
    }
    if (token_equals(first, "SHOW") && token_equals(second, "BINARY")) {
        return token_in(third, "LOGS", "STATUS");
    }
    if (token_equals(first, "SHOW") && token_equals(second, "BINLOG")) {
        return token_equals(third, "EVENTS");
    }
    if (token_equals(first, "SHOW") && token_in(second, "MASTER", "SLAVE", "REPLICA")) {
        return token_equals(third, "STATUS");
    }
    if (token_equals(first, "FLUSH") && token_equals(second, "BINARY")) {
        return token_equals(third, "LOGS");
    }
    if (token_equals(first, "RESET") && token_equals(second, "MASTER")) {
        return true;
    }
    return token_equals(first, "PURGE") && token_in(second, "BINARY", "MASTER");
}

bool is_unsupported_xa_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "XA");
}

bool is_unsupported_replication_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "MASTER_GTID_WAIT") ||
             identifier_token_equals(tokens.values[index], "MASTER_POS_WAIT") ||
             identifier_token_equals(tokens.values[index], "BINLOG_GTID_POS") ||
             identifier_token_equals(tokens.values[index], "WSREP_SYNC_WAIT_UPTO_GTID")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_sql_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "VEC_DISTANCE") ||
             identifier_token_equals(tokens.values[index], "VEC_DISTANCE_COSINE") ||
             identifier_token_equals(tokens.values[index], "VEC_DISTANCE_EUCLIDEAN") ||
             identifier_token_equals(tokens.values[index], "VEC_FROMTEXT") ||
             identifier_token_equals(tokens.values[index], "VEC_TOTEXT")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_vector_index_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (!identifier_token_equals(tokens.values[index], "VECTOR")) {
            continue;
        }
        if (identifier_token_equals(tokens.values[index + 1U], "KEY") ||
            identifier_token_equals(tokens.values[index + 1U], "INDEX")) {
            return true;
        }
        if (token_equals(tokens.values[index + 1U], "(") && index > 0U &&
            token_in(tokens.values[index - 1U], "(", ",")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_xml_sql_function_call(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "EXTRACTVALUE") ||
             identifier_token_equals(tokens.values[index], "UPDATEXML")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_dynamic_column_function_call(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "COLUMN_ADD") ||
             identifier_token_equals(tokens.values[index], "COLUMN_CHECK") ||
             identifier_token_equals(tokens.values[index], "COLUMN_CREATE") ||
             identifier_token_equals(tokens.values[index], "COLUMN_DELETE") ||
             identifier_token_equals(tokens.values[index], "COLUMN_EXISTS") ||
             identifier_token_equals(tokens.values[index], "COLUMN_GET") ||
             identifier_token_equals(tokens.values[index], "COLUMN_JSON") ||
             identifier_token_equals(tokens.values[index], "COLUMN_LIST")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_utility_function_statement(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if ((identifier_token_equals(tokens.values[index], "BENCHMARK") ||
             identifier_token_equals(tokens.values[index], "GET_LOCK") ||
             identifier_token_equals(tokens.values[index], "IS_FREE_LOCK") ||
             identifier_token_equals(tokens.values[index], "IS_USED_LOCK") ||
             identifier_token_equals(tokens.values[index], "LOAD_FILE") ||
             identifier_token_equals(tokens.values[index], "RELEASE_ALL_LOCKS") ||
             identifier_token_equals(tokens.values[index], "RELEASE_LOCK") ||
             identifier_token_equals(tokens.values[index], "SLEEP") ||
             identifier_token_equals(tokens.values[index], "UUID_SHORT")) &&
            token_equals(tokens.values[index + 1U], "(")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_sql_handler_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "HANDLER");
}

bool is_unsupported_select_file_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);

    if (!token_in(first, "SELECT", "WITH")) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "INTO") &&
            token_in(tokens.values[index + 1U], "OUTFILE", "DUMPFILE")) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_load_file_import_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "LOAD") &&
           token_in(identifier_token_at(tokens, 1), "DATA", "XML");
}

bool is_unsupported_help_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "HELP");
}

bool is_unsupported_static_show_info_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "SHOW") &&
           token_in(identifier_token_at(tokens, 1), "AUTHORS", "CONTRIBUTORS", "PRIVILEGES");
}

bool is_unsupported_processlist_metadata_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return token_equals(first, "SHOW") &&
           (token_equals(second, "PROCESSLIST") ||
            (token_equals(second, "FULL") && token_equals(third, "PROCESSLIST")));
}

bool is_unsupported_thread_control_statement(const SqlPolicyTokens &tokens) {
    return token_in(identifier_token_at(tokens, 0), "KILL", "SHUTDOWN");
}

bool is_unsupported_foreign_server_metadata_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return token_equals(first, "SHOW") && token_equals(second, "CREATE") &&
           token_equals(third, "SERVER");
}

bool is_unsupported_backup_statement(const SqlPolicyTokens &tokens) {
    return token_equals(identifier_token_at(tokens, 0), "BACKUP");
}

bool is_unsupported_userstat_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_userstat_statistics_table(tokens) ||
        has_current_schema_userstat_statistics_table_reference(tokens, current_schema)) {
        return true;
    }
    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        return is_userstat_statistics_table_token(identifier_token_at(tokens, flush_target_index));
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "USERSTAT") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_user_variable_diagnostics_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_table(tokens, "USER_VARIABLES") ||
        has_current_schema_table_reference(tokens, "USER_VARIABLES", current_schema)) {
        return true;
    }
    if (token_equals(first, "SHOW")) {
        return token_equals(second, "USER_VARIABLES");
    }
    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        return identifier_token_equals(
            identifier_token_at(tokens, flush_target_index),
            "USER_VARIABLES"
        );
    }
    return false;
}

bool is_unsupported_statement_profiling_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (has_information_schema_table(tokens, "PROFILING") ||
        has_current_schema_table_reference(tokens, "PROFILING", current_schema)) {
        return true;
    }
    if (token_equals(first, "SHOW") && token_in(second, "PROFILE", "PROFILES")) {
        return true;
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "PROFILING", "PROFILING_HISTORY_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_query_cache_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);
    const std::string_view third = identifier_token_at(tokens, 2);

    return (token_equals(first, "FLUSH") || token_equals(first, "RESET")) &&
           token_equals(second, "QUERY") && token_equals(third, "CACHE");
}

bool is_unsupported_query_log_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "FLUSH")) {
        const std::size_t flush_target_index =
            token_in(second, "LOCAL", "NO_WRITE_TO_BINLOG") ? 2U : 1U;
        const std::string_view flush_target = identifier_token_at(tokens, flush_target_index);
        const std::string_view flush_target_tail =
            identifier_token_at(tokens, flush_target_index + 1U);

        return token_equals(flush_target, "LOGS") || (token_in(flush_target, "GENERAL", "SLOW") &&
                                                      token_equals(flush_target_tail, "LOGS"));
    }
    if (!token_equals(first, "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (is_query_log_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_optimizer_trace_statement(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    if (has_information_schema_table(tokens, "OPTIMIZER_TRACE") ||
        has_current_schema_table_reference(tokens, "OPTIMIZER_TRACE", current_schema)) {
        return true;
    }
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_in(tokens.values[index], "OPTIMIZER_TRACE", "OPTIMIZER_TRACE_MAX_MEM_SIZE") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_persistent_statistics_statement(const SqlPolicyTokens &tokens) {
    if (token_equals(identifier_token_at(tokens, 0), "ANALYZE") &&
        has_identifier_token(tokens, "PERSISTENT", 1)) {
        return true;
    }
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (is_persistent_statistics_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

bool is_unsupported_server_set_statement(const SqlPolicyTokens &tokens) {
    if (!token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (token_equals(tokens.values[index], "PASSWORD") &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
        if (is_server_variable_token(tokens.values[index]) &&
            is_system_variable_qualified_token(tokens, index)) {
            return true;
        }
    }
    return false;
}

SqlPolicyTokens collect_sql_policy_tokens(std::string_view sql) {
    std::size_t offset = 0;
    SqlPolicyTokens tokens = {};
    while (tokens.count < tokens.values.size() &&
           next_sql_token(sql, offset, tokens.values[tokens.count])) {
        ++tokens.count;
    }
    return tokens;
}

bool next_sql_token(std::string_view sql, std::size_t &offset, std::string_view &token) {
    skip_sql_spacing_and_comments(sql, offset);
    if (offset >= sql.size()) {
        token = {};
        return false;
    }

    const std::size_t start = offset;
    if (sql[offset] == '\'' || sql[offset] == '"' || sql[offset] == '`') {
        skip_quoted_sql_token(sql, offset);
        token = sql.substr(start, offset - start);
        return true;
    }

    if (is_sql_identifier_char(sql[offset])) {
        while (offset < sql.size() && is_sql_identifier_char(sql[offset])) {
            ++offset;
        }
        token = sql.substr(start, offset - start);
        return true;
    }

    ++offset;
    token = sql.substr(start, 1);
    return true;
}

void skip_sql_spacing_and_comments(std::string_view sql, std::size_t &offset) {
    for (;;) {
        while (offset < sql.size() && is_sql_space(sql[offset])) {
            ++offset;
        }
        if (enter_executable_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_dash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_hash_sql_comment(sql, offset)) {
            continue;
        }
        if (skip_block_sql_comment(sql, offset)) {
            continue;
        }
        return;
    }
}

bool enter_executable_sql_comment(std::string_view sql, std::size_t &offset) {
    const bool is_mysql_comment = offset + 2U < sql.size() && sql[offset] == '/' &&
                                  sql[offset + 1U] == '*' && sql[offset + 2U] == '!';
    const bool is_mariadb_comment =
        offset + 3U < sql.size() && sql[offset] == '/' && sql[offset + 1U] == '*' &&
        (sql[offset + 2U] == 'M' || sql[offset + 2U] == 'm') && sql[offset + 3U] == '!';

    if (is_mysql_comment) {
        offset += 3U;
    } else if (is_mariadb_comment) {
        offset += 4U;
    } else {
        return false;
    }

    while (offset < sql.size() && sql[offset] >= '0' && sql[offset] <= '9') {
        ++offset;
    }
    return true;
}

bool skip_dash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '-' || sql[offset + 1U] != '-') {
        return false;
    }
    offset += 2U;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_hash_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset >= sql.size() || sql[offset] != '#') {
        return false;
    }
    ++offset;
    while (offset < sql.size() && sql[offset] != '\n') {
        ++offset;
    }
    return true;
}

bool skip_block_sql_comment(std::string_view sql, std::size_t &offset) {
    if (offset + 1U >= sql.size() || sql[offset] != '/' || sql[offset + 1U] != '*') {
        return false;
    }
    offset += 2U;
    while (offset + 1U < sql.size() && (sql[offset] != '*' || sql[offset + 1U] != '/')) {
        ++offset;
    }
    if (offset + 1U < sql.size()) {
        offset += 2U;
    }
    return true;
}

void skip_quoted_sql_token(std::string_view sql, std::size_t &offset) {
    const char quote = sql[offset++];
    while (offset < sql.size()) {
        if (sql[offset] == '\\' && offset + 1U < sql.size()) {
            offset += 2U;
            continue;
        }
        if (sql[offset++] == quote) {
            return;
        }
    }
}

bool is_sql_space(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

bool is_sql_identifier_char(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

bool is_sql_identifier_token(std::string_view token) {
    return !token.empty() && is_sql_identifier_char(token[0]);
}

std::string_view identifier_token_at(const SqlPolicyTokens &tokens, std::size_t index) {
    std::size_t identifier_index = 0;
    for (std::size_t raw_index = 0; raw_index < tokens.count; ++raw_index) {
        if (!is_sql_identifier_token(tokens.values[raw_index])) {
            continue;
        }
        if (identifier_index == index) {
            return tokens.values[raw_index];
        }
        ++identifier_index;
    }
    return {};
}

std::string_view unquoted_identifier_token(std::string_view token) {
    if (token.size() >= 2U && token.front() == '`' && token.back() == '`') {
        token.remove_prefix(1U);
        token.remove_suffix(1U);
    }
    return token;
}

bool has_identifier_token(
    const SqlPolicyTokens &tokens,
    const char *keyword,
    std::size_t start_index
) {
    for (std::size_t index = start_index; !identifier_token_at(tokens, index).empty(); ++index) {
        if (token_equals(identifier_token_at(tokens, index), keyword)) {
            return true;
        }
    }
    return false;
}

bool has_information_schema_userstat_statistics_table(const SqlPolicyTokens &tokens) {
    return has_information_schema_table(tokens, "CLIENT_STATISTICS") ||
           has_information_schema_table(tokens, "INDEX_STATISTICS") ||
           has_information_schema_table(tokens, "TABLE_STATISTICS") ||
           has_information_schema_table(tokens, "USER_STATISTICS");
}

bool has_current_schema_userstat_statistics_table_reference(
    const SqlPolicyTokens &tokens,
    std::string_view current_schema
) {
    return has_current_schema_table_reference(tokens, "CLIENT_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "INDEX_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "TABLE_STATISTICS", current_schema) ||
           has_current_schema_table_reference(tokens, "USER_STATISTICS", current_schema);
}

bool has_information_schema_table(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 0; index + 2U < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], "INFORMATION_SCHEMA") &&
            token_equals(tokens.values[index + 1U], ".") &&
            identifier_token_equals(tokens.values[index + 2U], table_name)) {
            return true;
        }
    }
    return false;
}

bool has_current_schema_table_reference(
    const SqlPolicyTokens &tokens,
    const char *table_name,
    std::string_view current_schema
) {
    return identifier_token_equals(current_schema, "INFORMATION_SCHEMA") &&
           has_unqualified_table_reference(tokens, table_name);
}

bool has_unqualified_table_reference(const SqlPolicyTokens &tokens, const char *table_name) {
    for (std::size_t index = 1; index < tokens.count; ++index) {
        if (identifier_token_equals(tokens.values[index], table_name) &&
            table_reference_keyword(tokens.values[index - 1U])) {
            return true;
        }
    }
    return false;
}

bool is_sql_mode_assignment_target(const SqlPolicyTokens &tokens, std::size_t index) {
    return is_system_variable_qualified_token(tokens, index);
}

bool sql_mode_assignment_mentions_oracle(const SqlPolicyTokens &tokens, std::size_t index) {
    int paren_depth = 0;
    for (std::size_t value_index = index + 2U; value_index < tokens.count; ++value_index) {
        const std::string_view token = tokens.values[value_index];
        if (token_equals(token, "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(token, ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && token_equals(token, ",")) {
            return false;
        }
        if (token_contains_sql_mode_name(token, "ORACLE")) {
            return true;
        }
    }
    return false;
}

bool token_contains_sql_mode_name(std::string_view token, const char *mode_name) {
    const std::size_t mode_length = std::strlen(mode_name);
    if (mode_length == 0U || token.size() < mode_length) {
        return false;
    }

    for (std::size_t start = 0; start + mode_length <= token.size(); ++start) {
        bool matches = true;
        for (std::size_t offset = 0; offset < mode_length; ++offset) {
            char left = token[start + offset];
            char right = mode_name[offset];
            if (left >= 'a' && left <= 'z') {
                left = static_cast<char>(left - ('a' - 'A'));
            }
            if (right >= 'a' && right <= 'z') {
                right = static_cast<char>(right - ('a' - 'A'));
            }
            if (left != right) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        const bool left_boundary = start == 0U || !is_sql_identifier_char(token[start - 1U]);
        const std::size_t end = start + mode_length;
        const bool right_boundary = end == token.size() || !is_sql_identifier_char(token[end]);
        if (left_boundary && right_boundary) {
            return true;
        }
    }
    return false;
}

bool token_equals(std::string_view token, const char *keyword) {
    const std::size_t keyword_length = std::strlen(keyword);
    if (token.size() != keyword_length) {
        return false;
    }
    for (std::size_t index = 0; index < token.size(); ++index) {
        char left = token[index];
        char right = keyword[index];
        if (left >= 'a' && left <= 'z') {
            left = static_cast<char>(left - ('a' - 'A'));
        }
        if (right >= 'a' && right <= 'z') {
            right = static_cast<char>(right - ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool identifier_token_equals(std::string_view token, const char *keyword) {
    return token_equals(unquoted_identifier_token(token), keyword);
}

bool table_reference_keyword(std::string_view token) {
    return token_in(token, "FROM", "JOIN", "UPDATE") ||
           token_in(token, "INTO", "TABLE", "DESC", "DESCRIBE");
}

bool token_in(std::string_view token, const char *first, const char *second) {
    return token_equals(token, first) || token_equals(token, second);
}

bool token_in(std::string_view token, const char *first, const char *second, const char *third) {
    return token_equals(token, first) || token_equals(token, second) || token_equals(token, third);
}

bool token_in(
    std::string_view token,
    const char *first,
    const char *second,
    const char *third,
    const char *fourth
) {
    return token_equals(token, first) || token_equals(token, second) ||
           token_equals(token, third) || token_equals(token, fourth);
}

bool is_userstat_statistics_table_token(std::string_view token) {
    return identifier_token_equals(token, "CLIENT_STATISTICS") ||
           identifier_token_equals(token, "INDEX_STATISTICS") ||
           identifier_token_equals(token, "TABLE_STATISTICS") ||
           identifier_token_equals(token, "USER_STATISTICS");
}

bool is_server_variable_token(std::string_view token) {
    return token_in(token, "EVENT_SCHEDULER", "SQL_LOG_BIN", "LOG_BIN", "BINLOG_FORMAT") ||
           token_in(token, "QUERY_CACHE_SIZE", "QUERY_CACHE_TYPE", "QUERY_CACHE_LIMIT") ||
           token_in(
               token,
               "QUERY_CACHE_MIN_RES_UNIT",
               "QUERY_CACHE_WLOCK_INVALIDATE",
               "QUERY_CACHE_STRIP_COMMENTS"
           ) ||
           token_in(token, "GTID_BINLOG_STATE", "GTID_SLAVE_POS", "GTID_STRICT_MODE") ||
           token_in(token, "GTID_DOMAIN_ID", "GTID_SEQ_NO", "GTID_CLEANUP_BATCH_SIZE") ||
           token_in(
               token,
               "GTID_IGNORE_DUPLICATES",
               "GTID_POS_AUTO_ENGINES",
               "BINLOG_GTID_INDEX"
           ) ||
           token_in(
               token,
               "BINLOG_GTID_INDEX_PAGE_SIZE",
               "BINLOG_GTID_INDEX_SPAN_MIN",
               "WSREP_GTID_DOMAIN_ID"
           ) ||
           token_in(token, "WSREP_GTID_SEQ_NO", "WSREP_GTID_MODE") ||
           token_in(
               token,
               "INNODB_BUFFER_POOL_DUMP_NOW",
               "INNODB_BUFFER_POOL_DUMP_AT_SHUTDOWN",
               "INNODB_BUFFER_POOL_DUMP_PCT"
           ) ||
           token_in(
               token,
               "INNODB_BUFFER_POOL_LOAD_NOW",
               "INNODB_BUFFER_POOL_LOAD_ABORT",
               "INNODB_BUFFER_POOL_LOAD_AT_STARTUP"
           ) ||
           token_equals(token, "INNODB_BUFFER_POOL_LOAD_PAGES_ABORT");
}

bool is_query_log_variable_token(std::string_view token) {
    return token_in(token, "GENERAL_LOG", "GENERAL_LOG_FILE", "LOG_OUTPUT") ||
           token_in(token, "SLOW_QUERY_LOG", "SLOW_QUERY_LOG_FILE", "LOG_SLOW_QUERY") ||
           token_in(token, "LOG_SLOW_QUERY_FILE", "SQL_LOG_OFF", "LONG_QUERY_TIME") ||
           token_in(
               token,
               "MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_MIN_EXAMINED_ROW_LIMIT",
               "LOG_SLOW_RATE_LIMIT"
           ) ||
           token_in(
               token,
               "LOG_SLOW_FILTER",
               "LOG_SLOW_VERBOSITY",
               "LOG_SLOW_DISABLED_STATEMENTS"
           ) ||
           token_in(
               token,
               "LOG_SLOW_ADMIN_STATEMENTS",
               "LOG_SLOW_SLAVE_STATEMENTS",
               "LOG_SLOW_MAX_WARNINGS"
           );
}

bool is_persistent_statistics_variable_token(std::string_view token) {
    return token_in(token, "USE_STAT_TABLES", "HISTOGRAM_SIZE", "HISTOGRAM_TYPE");
}

bool is_system_variable_qualified_token(const SqlPolicyTokens &tokens, std::size_t index) {
    if (index + 1U >= tokens.count || !token_equals(tokens.values[index + 1U], "=")) {
        return false;
    }
    if (is_system_variable_assignment_start(tokens, index)) {
        return true;
    }
    if (token_in(tokens.values[index - 1U], "GLOBAL", "SESSION", "LOCAL")) {
        return is_system_variable_assignment_start(tokens, index - 1U);
    }
    if (index >= 2U && token_equals(tokens.values[index - 1U], "@") &&
        token_equals(tokens.values[index - 2U], "@")) {
        return true;
    }
    return index >= 4U && token_equals(tokens.values[index - 1U], ".") &&
           token_in(tokens.values[index - 2U], "GLOBAL", "SESSION", "LOCAL") &&
           token_equals(tokens.values[index - 3U], "@") &&
           token_equals(tokens.values[index - 4U], "@");
}

bool is_system_variable_assignment_start(const SqlPolicyTokens &tokens, std::size_t index) {
    const std::size_t first_assignment = first_set_assignment_token_index(tokens);
    int paren_depth = 0;

    if (index == first_assignment) {
        return true;
    }
    for (std::size_t token_index = first_assignment; token_index < index; ++token_index) {
        if (token_equals(tokens.values[token_index], "(")) {
            ++paren_depth;
            continue;
        }
        if (token_equals(tokens.values[token_index], ")")) {
            if (paren_depth > 0) {
                --paren_depth;
            }
            continue;
        }
        if (paren_depth == 0 && first_assignment == 2U &&
            token_equals(tokens.values[token_index], "FOR")) {
            return false;
        }
    }
    return index > 0U && paren_depth == 0 && token_equals(tokens.values[index - 1U], ",");
}

std::size_t first_set_assignment_token_index(const SqlPolicyTokens &tokens) {
    if (tokens.count > 2U && token_equals(tokens.values[1], "STATEMENT") &&
        !token_equals(tokens.values[2], "=")) {
        return 2U;
    }
    return 1U;
}

int validate_runtime_database_path(mylite_db &db) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U && g_runtime.database_path != db.database_path) {
        set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
        return MYLITE_BUSY;
    }
    return MYLITE_OK;
}

int store_and_emit_result(
    mylite_db &db,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
) {
    MYSQL_RES *result = mysql_store_result(&db.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&db.mysql) != 0U) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
        return drain_remaining_query_results(db);
    }
    *has_result = true;

    const unsigned field_count = mysql_num_fields(result);
    if (field_count > static_cast<unsigned>(INT_MAX)) {
        mysql_free_result(result);
        set_error(db, MYLITE_ERROR, "result has too many columns");
        return MYLITE_ERROR;
    }

    std::vector<char *> column_names;
    column_names.reserve(field_count);
    const MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned i = 0; i < field_count; ++i) {
        column_names.push_back(fields[i].name);
    }

    for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr; row = mysql_fetch_row(result)) {
        if (callback != nullptr &&
            callback(ctx, static_cast<int>(field_count), row, column_names.data()) != 0) {
            mysql_free_result(result);
            static_cast<void>(drain_remaining_query_results(db));
            set_error(db, MYLITE_ERROR, "query callback requested abort");
            return MYLITE_ERROR;
        }
    }

    mysql_free_result(result);
    return drain_remaining_query_results(db);
}

int drain_remaining_query_results(mylite_db &db) {
    for (;;) {
        const int next_result = mysql_next_result(&db.mysql);
        if (next_result < 0) {
            return MYLITE_OK;
        }
        if (next_result > 0) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }

        MYSQL_RES *result = mysql_store_result(&db.mysql);
        if (result != nullptr) {
            mysql_free_result(result);
            continue;
        }
        if (mysql_field_count(&db.mysql) != 0U) {
            set_mariadb_error(db);
            return MYLITE_ERROR;
        }
    }
    return MYLITE_OK;
}

void rollback_active_transaction_after_deadlock(mylite_db &db) {
    if (db.mariadb_errno != k_mariadb_lock_deadlock_errno) {
        return;
    }

    const ErrorSnapshot snapshot = capture_error(db);
    static_cast<void>(rollback_active_transaction(db));
    restore_error(db, snapshot);
}

int rollback_active_transaction(mylite_db &db) {
    if (!db.connected) {
        return MYLITE_OK;
    }
    if (mysql_query(&db.mysql, "ROLLBACK") != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    const int drain_result = drain_remaining_query_results(db);
    if (drain_result == MYLITE_OK) {
        release_ownerless_transaction_page_version_pin(db);
        db.ownerless_explicit_transaction_active = false;
        db.ownerless_transaction_has_local_write_or_locking_read = false;
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
    }
    return drain_result;
}

ErrorSnapshot capture_error(const mylite_db &db) {
    return {db.errcode, db.extended_errcode, db.mariadb_errno, db.sqlstate, db.errmsg};
}

void restore_error(mylite_db &db, const ErrorSnapshot &snapshot) {
    db.errcode = snapshot.errcode;
    db.extended_errcode = snapshot.extended_errcode;
    db.mariadb_errno = snapshot.mariadb_errno;
    db.sqlstate = snapshot.sqlstate;
    db.errmsg = snapshot.errmsg;
}

int initialize_statement_results(mylite_stmt &stmt, bool release_existing_results) {
    if (release_existing_results) {
        release_statement_results(stmt);
    }

    stmt.metadata = mysql_stmt_result_metadata(stmt.stmt);
    if (stmt.metadata == nullptr) {
        if (mysql_stmt_field_count(stmt.stmt) != 0U) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
        stmt.has_result = false;
        return MYLITE_OK;
    }

    const unsigned field_count = mysql_num_fields(stmt.metadata);
    const MYSQL_FIELD *fields = mysql_fetch_fields(stmt.metadata);
    stmt.columns.resize(field_count);
    stmt.result_binds.resize(field_count);
    for (unsigned index = 0; index < field_count; ++index) {
        ResultColumn &column = stmt.columns[index];
        column.field_type = fields[index].type;
        column.flags = fields[index].flags;
        column.name = fields[index].name != nullptr ? fields[index].name : "";
        column.org_name = fields[index].org_name != nullptr ? fields[index].org_name : "";
        column.table = fields[index].table != nullptr ? fields[index].table : "";
        column.org_table = fields[index].org_table != nullptr ? fields[index].org_table : "";
        column.length = 0;
        column.is_null = 0;
        column.error = 0;
        column.bind = {};
        column.bind.buffer_type = MYSQL_TYPE_STRING;
        column.bind.length = &column.length;
        column.bind.is_null = &column.is_null;
        column.bind.error = &column.error;
        const unsigned long buffer_length =
            std::min(std::max(fields[index].length, 1UL), k_initial_result_buffer_size);
        if (configure_column_buffer(column, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }
        stmt.result_binds[index] = column.bind;
    }

    if (mysql_stmt_bind_result(stmt.stmt, stmt.result_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    stmt.has_result = true;
    return MYLITE_OK;
}

int fetch_statement_row(mylite_stmt &stmt) {
    const int bind_result = refresh_dirty_result_binds(stmt);
    if (bind_result != MYLITE_OK) {
        clear_statement_ownerless_page_visibility(stmt);
        return bind_result;
    }

    const int fetch_result = mysql_stmt_fetch(stmt.stmt);
    if (fetch_result == MYSQL_NO_DATA) {
        stmt.has_row = false;
        const int drain_result = drain_remaining_statement_results(stmt);
        if (drain_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(stmt);
            return drain_result;
        }
        stmt.has_result = false;
        clear_statement_ownerless_page_visibility(stmt);
        return MYLITE_DONE;
    }
    if (fetch_result != 0 && fetch_result != MYSQL_DATA_TRUNCATED) {
        set_mariadb_statement_error(stmt);
        clear_statement_ownerless_page_visibility(stmt);
        return MYLITE_ERROR;
    }
    if (fetch_result == MYSQL_DATA_TRUNCATED) {
        const int truncated_result = fetch_truncated_statement_columns(stmt);
        if (truncated_result != MYLITE_OK) {
            clear_statement_ownerless_page_visibility(stmt);
            return truncated_result;
        }
    }

    for (ResultColumn &column : stmt.columns) {
        if (column.length < column.buffer.size()) {
            column.buffer[column.length] = 0U;
        } else if (!column.buffer.empty()) {
            column.buffer.back() = 0U;
        }
    }
    stmt.has_row = true;
    return MYLITE_ROW;
}

int drain_remaining_statement_results(mylite_stmt &stmt) {
    for (;;) {
        const int next_result = mysql_stmt_next_result(stmt.stmt);
        if (next_result < 0) {
            return MYLITE_OK;
        }
        if (next_result > 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }

        if (mysql_stmt_field_count(stmt.stmt) == 0U) {
            continue;
        }
        if (mysql_stmt_store_result(stmt.stmt) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
        if (mysql_stmt_free_result(stmt.stmt) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }
    }
    return MYLITE_OK;
}

int refresh_dirty_result_binds(mylite_stmt &stmt) {
    if (!stmt.result_binds_dirty) {
        return MYLITE_OK;
    }

    if (mysql_stmt_bind_result(stmt.stmt, stmt.result_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    for (ResultColumn &column : stmt.columns) {
        std::vector<unsigned char>().swap(column.retired_buffer);
    }
    stmt.result_binds_dirty = false;
    return MYLITE_OK;
}

int fetch_truncated_statement_columns(mylite_stmt &stmt) {
    for (unsigned index = 0; index < stmt.columns.size(); ++index) {
        ResultColumn &column = stmt.columns[index];
        if (column.is_null != 0 || column.error == 0) {
            continue;
        }
        if (column.length == ULONG_MAX) {
            set_error(*stmt.db, MYLITE_ERROR, "result column is too large");
            return MYLITE_ERROR;
        }

        const unsigned long buffer_length = std::max(column.length, 1UL);
        std::vector<unsigned char> buffer;
        if (allocate_column_buffer(buffer, buffer_length) != MYLITE_OK) {
            set_error(*stmt.db, MYLITE_NOMEM, "result column buffer could not be allocated");
            return MYLITE_NOMEM;
        }

        MYSQL_BIND fetch_bind = column.bind;
        fetch_bind.buffer = buffer.data();
        fetch_bind.buffer_length = buffer_length;
        if (mysql_stmt_fetch_column(stmt.stmt, &fetch_bind, index, 0) != 0) {
            set_mariadb_statement_error(stmt);
            return MYLITE_ERROR;
        }

        column.retired_buffer = std::move(column.buffer);
        column.buffer = std::move(buffer);
        column.bind = fetch_bind;
        column.bind.buffer = column.buffer.data();
        stmt.result_binds[index] = column.bind;
        stmt.result_binds_dirty = true;
    }
    return MYLITE_OK;
}

int configure_column_buffer(ResultColumn &column, unsigned long buffer_length) {
    const int result = allocate_column_buffer(column.buffer, buffer_length);
    if (result != MYLITE_OK) {
        return result;
    }
    column.bind.buffer = column.buffer.data();
    column.bind.buffer_length = buffer_length;
    return MYLITE_OK;
}

int allocate_column_buffer(std::vector<unsigned char> &buffer, unsigned long buffer_length) {
    if (buffer_length == ULONG_MAX) {
        return MYLITE_NOMEM;
    }

    try {
        buffer.assign(buffer_length + 1UL, 0U);
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    }
    return MYLITE_OK;
}

void release_statement_results(mylite_stmt &stmt) {
    if (stmt.stmt != nullptr && stmt.has_result) {
        static_cast<void>(mysql_stmt_free_result(stmt.stmt));
    }
    if (stmt.metadata != nullptr) {
        mysql_free_result(stmt.metadata);
        stmt.metadata = nullptr;
    }
    stmt.columns.clear();
    stmt.result_binds.clear();
    stmt.result_binds_dirty = false;
    stmt.has_result = false;
    stmt.has_row = false;
}

void clear_statement_ownerless_page_visibility(mylite_stmt &stmt) {
    if (!stmt.ownerless_page_visibility_enabled) {
        return;
    }
    mylite_ownerless_innodb_clear_external_page_visibility();
    stmt.ownerless_page_visibility_enabled = false;
}

ParameterBinding *parameter_at(mylite_stmt &stmt, unsigned index) {
    if (index == 0U || index > stmt.parameters.size()) {
        return nullptr;
    }
    return &stmt.parameters[index - 1U];
}

int bind_null_value(mylite_stmt &stmt, unsigned index) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed) {
        return MYLITE_MISUSE;
    }

    parameter->bytes.clear();
    parameter->length = 0;
    parameter->is_null = 1;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = MYSQL_TYPE_NULL;
    parameter->bind.length = &parameter->length;
    parameter->bind.is_null = &parameter->is_null;
    parameter->bind.error = &parameter->error;
    return MYLITE_OK;
}

int bind_bytes(
    mylite_stmt &stmt,
    unsigned index,
    const void *value,
    std::size_t value_len,
    enum enum_field_types buffer_type,
    mylite_destructor destructor
) {
    ParameterBinding *parameter = parameter_at(stmt, index);
    if (parameter == nullptr || stmt.executed || value_len > ULONG_MAX) {
        return MYLITE_MISUSE;
    }

    const auto *bytes = static_cast<const unsigned char *>(value);
    parameter->bytes.clear();
    if (value_len > 0U) {
        parameter->bytes.assign(bytes, bytes + value_len);
    }
    if (parameter->bytes.empty()) {
        parameter->bytes.push_back(0U);
    }
    parameter->length = static_cast<unsigned long>(value_len);
    parameter->is_null = 0;
    parameter->error = 0;
    parameter->bind = {};
    parameter->bind.buffer_type = buffer_type;
    bind_parameter_buffer(*parameter);

    // MYLITE_TRANSIENT mirrors SQLite's public -1 destructor sentinel.
    // NOLINTBEGIN(performance-no-int-to-ptr)
    if (value != nullptr && destructor != MYLITE_STATIC && destructor != MYLITE_TRANSIENT) {
        destructor(const_cast<void *>(value));
    }
    // NOLINTEND(performance-no-int-to-ptr)
    return MYLITE_OK;
}

void bind_parameter_buffer(ParameterBinding &parameter) {
    parameter.bind.buffer =
        parameter.bytes.empty() ? nullptr : static_cast<void *>(parameter.bytes.data());
    parameter.bind.buffer_length = parameter.length;
    parameter.bind.length = &parameter.length;
    parameter.bind.is_null = &parameter.is_null;
    parameter.bind.error = &parameter.error;
}

int bind_parameters(mylite_stmt &stmt) {
    if (stmt.parameters.empty()) {
        return MYLITE_OK;
    }
    stmt.parameter_binds.resize(stmt.parameters.size());
    for (std::size_t index = 0; index < stmt.parameters.size(); ++index) {
        stmt.parameter_binds[index] = stmt.parameters[index].bind;
    }
    if (mysql_stmt_bind_param(stmt.stmt, stmt.parameter_binds.data()) != 0) {
        set_mariadb_statement_error(stmt);
        return MYLITE_ERROR;
    }
    return MYLITE_OK;
}

mylite_value_type column_type(const ResultColumn &column) {
    if (column.is_null != 0) {
        return MYLITE_TYPE_NULL;
    }

    switch (column.field_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
        return (column.flags & UNSIGNED_FLAG) != 0U ? MYLITE_TYPE_UINT64 : MYLITE_TYPE_INT64;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
        return MYLITE_TYPE_DOUBLE;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_VARCHAR:
        return (column.flags & BINARY_FLAG) != 0U ? MYLITE_TYPE_BLOB : MYLITE_TYPE_TEXT;
    default:
        return MYLITE_TYPE_TEXT;
    }
}

const ResultColumn *metadata_column_at(const mylite_stmt *stmt, unsigned column) {
    if (stmt == nullptr || column >= stmt->columns.size()) {
        return nullptr;
    }
    return &stmt->columns[column];
}

const ResultColumn *value_column_at(const mylite_stmt *stmt, unsigned column) {
    if (stmt == nullptr || !stmt->has_row || column >= stmt->columns.size()) {
        return nullptr;
    }
    return &stmt->columns[column];
}

int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags) {
    if (is_memory_database_path(database_path)) {
        return MYLITE_OK;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if ((flags & MYLITE_OPEN_EXCLUSIVE) != 0U && exists) {
        return MYLITE_ERROR;
    }

    if (!exists && (flags & MYLITE_OPEN_CREATE) == 0U) {
        return MYLITE_NOTFOUND;
    }

    if (exists) {
        const bool is_directory = std::filesystem::is_directory(database_path, error);
        if (error || !is_directory) {
            return MYLITE_IOERR;
        }
        return prepare_existing_database_directory(database_path, flags);
    }

    std::filesystem::create_directories(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }
    initialize_database_layout(database_path);

    return MYLITE_OK;
}

int prepare_existing_database_directory(
    const std::filesystem::path &database_path,
    unsigned flags
) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    const bool metadata_exists = std::filesystem::exists(metadata_path, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if (!metadata_exists) {
        const bool empty = database_directory_is_empty(database_path, error);
        if (error) {
            return MYLITE_IOERR;
        }
        if (empty && (flags & MYLITE_OPEN_CREATE) != 0U) {
            initialize_database_layout(database_path);
            return MYLITE_OK;
        }
        return empty ? MYLITE_NOTFOUND : MYLITE_CORRUPT;
    }

    const int layout_result = validate_database_layout(database_path);
    if (layout_result != MYLITE_OK) {
        return layout_result;
    }
    return MYLITE_OK;
}

int validate_database_layout(const std::filesystem::path &database_path) {
    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;

    const bool metadata_is_file = std::filesystem::is_regular_file(metadata_path, error);
    if (error || !metadata_is_file) {
        return error ? MYLITE_IOERR : MYLITE_CORRUPT;
    }

    const int metadata_result = validate_database_metadata(metadata_path);
    if (metadata_result != MYLITE_OK) {
        return metadata_result;
    }

    const int data_result = validate_layout_directory(database_path / k_datadir_name);
    if (data_result != MYLITE_OK) {
        return data_result;
    }

    const int tmp_result = validate_layout_directory(database_path / k_tmpdir_name);
    if (tmp_result != MYLITE_OK) {
        return tmp_result;
    }

    return MYLITE_OK;
}

int validate_layout_directory(const std::filesystem::path &directory) {
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    if (!exists) {
        return MYLITE_CORRUPT;
    }

    const bool is_directory = std::filesystem::is_directory(directory, error);
    if (error) {
        return MYLITE_IOERR;
    }
    return is_directory ? MYLITE_OK : MYLITE_CORRUPT;
}

int validate_database_metadata(const std::filesystem::path &metadata_path) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    bool has_format = false;
    bool has_mariadb_base = false;
    const std::string mariadb_base_line = std::string("mariadb_base=") + k_mariadb_base_ref;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == mariadb_base_line) {
            has_mariadb_base = true;
        }
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }

    return has_format && has_mariadb_base ? MYLITE_OK : MYLITE_CORRUPT;
}

int prepare_concurrency_metadata(const std::filesystem::path &database_path) {
    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    const std::filesystem::path metadata_path = concurrency_directory / k_concurrency_meta_filename;
    const std::filesystem::path lock_path = concurrency_directory / k_concurrency_lock_filename;
    std::error_code error;

    std::filesystem::create_directories(concurrency_directory, error);
    if (error) {
        return MYLITE_IOERR;
    }

    const int lock_fd = acquire_concurrency_lock(
        lock_path,
        k_persisted_config_lock_start,
        k_persisted_config_lock_length
    );
    if (lock_fd < 0) {
        return MYLITE_IOERR;
    }

    const bool metadata_exists = std::filesystem::exists(metadata_path, error);
    if (error) {
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return MYLITE_IOERR;
    }
    if (!metadata_exists) {
        try {
            write_concurrency_metadata(metadata_path);
        } catch (...) {
            release_concurrency_lock(
                lock_fd,
                k_persisted_config_lock_start,
                k_persisted_config_lock_length
            );
            throw;
        }
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return MYLITE_OK;
    }

    const bool metadata_is_file = std::filesystem::is_regular_file(metadata_path, error);
    if (error || !metadata_is_file) {
        release_concurrency_lock(
            lock_fd,
            k_persisted_config_lock_start,
            k_persisted_config_lock_length
        );
        return error ? MYLITE_IOERR : MYLITE_CORRUPT;
    }
    const int metadata_result = validate_concurrency_metadata(metadata_path);
    release_concurrency_lock(
        lock_fd,
        k_persisted_config_lock_start,
        k_persisted_config_lock_length
    );
    return metadata_result;
}

int acquire_concurrency_lock(const std::filesystem::path &lock_path, off_t start, off_t length) {
    return acquire_concurrency_lock(lock_path, start, length, F_WRLCK);
}

int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type
) {
    return acquire_concurrency_lock(
        lock_path,
        start,
        length,
        lock_type,
        k_concurrency_lock_wait_timeout_ms
    );
}

int acquire_concurrency_lock(
    const std::filesystem::path &lock_path,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
) {
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        return -1;
    }

    if (acquire_fd_range_lock(lock_fd, start, length, lock_type, timeout_ms)) {
        return lock_fd;
    }
    static_cast<void>(::close(lock_fd));
    return -1;
}

bool acquire_fd_range_lock(
    int fd,
    off_t start,
    off_t length,
    short lock_type,
    unsigned timeout_ms
) {
    if (fd < 0) {
        return false;
    }

    struct flock lock = {};
    lock.l_type = lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (::fcntl(fd, F_SETLK, &lock) == 0) {
            return true;
        }
        if (errno != EACCES && errno != EAGAIN) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(k_lock_poll_interval_ms));
    }
}

void release_concurrency_lock(int lock_fd, off_t start, off_t length) {
    if (lock_fd < 0) {
        return;
    }

    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    static_cast<void>(::fcntl(lock_fd, F_SETLK, &lock));
    static_cast<void>(::close(lock_fd));
}

int prepare_concurrency_shared_memory(
    const std::filesystem::path &database_path,
    bool allow_recovery_rebuild
) {
    const std::filesystem::path concurrency_directory = database_path / k_concurrency_dir_name;
    const std::filesystem::path metadata_path = concurrency_directory / k_concurrency_meta_filename;
    const std::filesystem::path lock_path = concurrency_directory / k_concurrency_lock_filename;
    const std::filesystem::path shm_path = concurrency_directory / k_concurrency_shm_filename;
    const std::filesystem::path wal_path = concurrency_directory / k_concurrency_wal_filename;
    const std::filesystem::path checkpoint_path =
        concurrency_directory / k_concurrency_checkpoint_filename;
    std::string database_uuid;
    const int uuid_result = read_concurrency_database_uuid(metadata_path, database_uuid);
    if (uuid_result != MYLITE_OK) {
        return uuid_result;
    }

    const int recovery_lock_fd =
        acquire_concurrency_lock(lock_path, k_recovery_lock_start, k_recovery_lock_length);
    if (recovery_lock_fd < 0) {
        return MYLITE_IOERR;
    }

    const int recovery_files_result =
        prepare_concurrency_recovery_files(concurrency_directory, database_uuid);
    if (recovery_files_result != MYLITE_OK) {
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
        return recovery_files_result;
    }

    const int resize_lock_fd =
        acquire_concurrency_lock(lock_path, k_shm_resize_lock_start, k_shm_resize_lock_length);
    if (resize_lock_fd < 0) {
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
        return MYLITE_IOERR;
    }
    const auto release_layout_locks = [&]() {
        release_concurrency_lock(resize_lock_fd, k_shm_resize_lock_start, k_shm_resize_lock_length);
        release_concurrency_lock(recovery_lock_fd, k_recovery_lock_start, k_recovery_lock_length);
    };

    const std::string shm_name = shm_path.string();
    const int shm_fd = ::open(shm_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (shm_fd < 0) {
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const std::string wal_name = wal_path.string();
    const int wal_fd = ::open(wal_name.c_str(), O_RDWR | O_CLOEXEC);
    if (wal_fd < 0) {
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const std::string checkpoint_name = checkpoint_path.string();
    const int checkpoint_fd = ::open(checkpoint_name.c_str(), O_RDWR | O_CLOEXEC);
    if (checkpoint_fd < 0) {
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const bool initial_shared_memory = shm_stat.st_size == 0;
    if (shm_stat.st_size < k_minimum_concurrency_shm_size &&
        ::ftruncate(shm_fd, k_minimum_concurrency_shm_size) != 0) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return MYLITE_IOERR;
    }
    const off_t shm_size =
        std::max(shm_stat.st_size, static_cast<off_t>(k_minimum_concurrency_shm_size));
    const int layout_result = prepare_concurrency_shm_layout(
        database_path,
        shm_fd,
        wal_fd,
        checkpoint_fd,
        shm_size,
        database_uuid,
        allow_recovery_rebuild,
        initial_shared_memory
    );
    if (layout_result != MYLITE_OK) {
        static_cast<void>(::close(checkpoint_fd));
        static_cast<void>(::close(wal_fd));
        static_cast<void>(::close(shm_fd));
        release_layout_locks();
        return layout_result;
    }

    static_cast<void>(::close(checkpoint_fd));
    static_cast<void>(::close(wal_fd));
    static_cast<void>(::close(shm_fd));
    release_layout_locks();
    return MYLITE_OK;
}

int read_concurrency_database_uuid(
    const std::filesystem::path &metadata_path,
    std::string &database_uuid
) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    for (std::string line; std::getline(metadata, line);) {
        if (line.rfind("database_uuid=", 0) != 0) {
            continue;
        }
        const std::string_view uuid = std::string_view(line).substr(14U);
        if (!is_database_uuid(uuid)) {
            return MYLITE_CORRUPT;
        }
        database_uuid.assign(uuid);
        return MYLITE_OK;
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }
    return MYLITE_CORRUPT;
}

int prepare_concurrency_recovery_files(
    const std::filesystem::path &concurrency_directory,
    std::string_view database_uuid
) {
    const int wal_result = prepare_concurrency_recovery_file(
        concurrency_directory / k_concurrency_wal_filename,
        k_concurrency_wal_magic,
        database_uuid
    );
    if (wal_result != MYLITE_OK) {
        return wal_result;
    }
    return prepare_concurrency_checkpoint_file(
        concurrency_directory / k_concurrency_checkpoint_filename,
        database_uuid
    );
}

int prepare_concurrency_checkpoint_file(
    const std::filesystem::path &file_path,
    std::string_view database_uuid
) {
    const int header_result =
        prepare_concurrency_recovery_file(file_path, k_concurrency_checkpoint_magic, database_uuid);
    if (header_result != MYLITE_OK) {
        return header_result;
    }

    const std::string file_name = file_path.string();
    const int file_fd = ::open(file_name.c_str(), O_RDWR | O_CLOEXEC);
    if (file_fd < 0) {
        return MYLITE_IOERR;
    }
    struct stat file_stat = {};
    if (::fstat(file_fd, &file_stat) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    if (file_stat.st_size < k_concurrency_checkpoint_lsn_payload_end &&
        (::ftruncate(file_fd, k_concurrency_checkpoint_lsn_payload_end) != 0 ||
         ::fsync(file_fd) != 0)) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    static_cast<void>(::close(file_fd));
    return MYLITE_OK;
}

int prepare_concurrency_recovery_file(
    const std::filesystem::path &file_path,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    const std::string file_name = file_path.string();
    const int file_fd = ::open(file_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (file_fd < 0) {
        return MYLITE_IOERR;
    }

    struct stat file_stat = {};
    if (::fstat(file_fd, &file_stat) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    if (file_stat.st_size < static_cast<off_t>(k_concurrency_recovery_header_size) &&
        ::ftruncate(file_fd, static_cast<off_t>(k_concurrency_recovery_header_size)) != 0) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_recovery_header_size> header = {};
    if (!read_exact_at(file_fd, header.data(), header.size(), 0)) {
        static_cast<void>(::close(file_fd));
        return MYLITE_IOERR;
    }
    if (!concurrency_recovery_header_matches(header, magic, database_uuid)) {
        build_concurrency_recovery_header(header, magic, database_uuid);
        if (!write_exact_at(file_fd, header.data(), header.size(), 0) || ::fsync(file_fd) != 0) {
            static_cast<void>(::close(file_fd));
            return MYLITE_IOERR;
        }
    }

    static_cast<void>(::close(file_fd));
    return MYLITE_OK;
}

bool concurrency_recovery_header_matches(
    const std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    if (database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    if (std::memcmp(
            header.data() + k_concurrency_recovery_magic_offset,
            magic.data(),
            magic.size()
        ) != 0) {
        return false;
    }
    return load_le32(header.data(), k_concurrency_recovery_format_offset) ==
               k_concurrency_recovery_format_version &&
           load_le32(header.data(), k_concurrency_recovery_header_size_offset) ==
               k_concurrency_recovery_header_size &&
           load_le32(header.data(), k_concurrency_recovery_byte_order_offset) ==
               k_concurrency_shm_byte_order &&
           load_le32(header.data(), k_concurrency_recovery_flags_offset) == 0U &&
           load_le64(header.data(), k_concurrency_recovery_generation_offset) == 0U &&
           std::memcmp(
               header.data() + k_concurrency_recovery_database_uuid_offset,
               database_uuid.data(),
               database_uuid.size()
           ) == 0;
}

void build_concurrency_recovery_header(
    std::array<unsigned char, k_concurrency_recovery_header_size> &header,
    const std::array<unsigned char, 8> &magic,
    std::string_view database_uuid
) {
    header.fill(0U);
    std::memcpy(header.data() + k_concurrency_recovery_magic_offset, magic.data(), magic.size());
    store_le32(
        header.data(),
        k_concurrency_recovery_format_offset,
        k_concurrency_recovery_format_version
    );
    store_le32(
        header.data(),
        k_concurrency_recovery_header_size_offset,
        static_cast<std::uint32_t>(k_concurrency_recovery_header_size)
    );
    store_le32(
        header.data(),
        k_concurrency_recovery_byte_order_offset,
        k_concurrency_shm_byte_order
    );
    store_le32(header.data(), k_concurrency_recovery_flags_offset, 0U);
    store_le64(header.data(), k_concurrency_recovery_generation_offset, 0U);
    std::memcpy(
        header.data() + k_concurrency_recovery_database_uuid_offset,
        database_uuid.data(),
        database_uuid.size()
    );
}

int prepare_concurrency_shm_layout(
    const std::filesystem::path &database_path,
    int shm_fd,
    int page_log_fd,
    int checkpoint_fd,
    off_t shm_size,
    std::string_view database_uuid,
    bool allow_recovery_rebuild,
    bool initial_shared_memory
) {
    std::array<unsigned char, k_concurrency_shm_header_size> header = {};
    if (!read_exact_at(shm_fd, header.data(), header.size(), 0)) {
        return MYLITE_IOERR;
    }

    std::uint64_t recovery_generation = 0;
    const bool identity_matches = concurrency_shm_header_identity_matches(header, database_uuid);
    const bool layout_matches =
        concurrency_shm_header_layout_matches(header, shm_size, database_uuid);
    if (identity_matches) {
        recovery_generation =
            load_le64(header.data(), k_concurrency_shm_recovery_generation_offset);
        const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
        if (state == k_concurrency_shm_state_rebuilding) {
            ++recovery_generation;
        }
    }

    bool rebuild_segments = !layout_matches;
    const bool segments_match =
        !rebuild_segments && concurrency_shm_segments_match(shm_fd, shm_size);
    if (!rebuild_segments && !segments_match) {
        std::uint64_t active_count = 0;
        const int active_count_result =
            read_concurrency_process_active_count(shm_fd, &active_count);
        if (active_count_result != MYLITE_OK) {
            return MYLITE_BUSY;
        }
        std::uint64_t live_count = active_count;
        if (active_count > 0U) {
            const int live_count_result = read_concurrency_process_live_count(shm_fd, &live_count);
            if (live_count_result != MYLITE_OK) {
                return MYLITE_BUSY;
            }
        }
        if (live_count > 0U) {
            return MYLITE_BUSY;
        }
        ++recovery_generation;
        rebuild_segments = true;
    }
    if (!rebuild_segments) {
        const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
        if (state == k_concurrency_shm_state_clean || state == k_concurrency_shm_state_dirty) {
            std::uint64_t active_count = 0;
            const int active_count_result =
                read_concurrency_process_active_count(shm_fd, &active_count);
            if (active_count_result != MYLITE_OK) {
                return active_count_result;
            }
            std::uint64_t live_count = active_count;
            if (active_count > 0U) {
                const int live_count_result =
                    read_concurrency_process_live_count(shm_fd, &live_count);
                if (live_count_result != MYLITE_OK) {
                    return live_count_result;
                }
            }
            const bool no_live_processes = active_count == 0U || live_count == 0U;
            const bool dirty_shm_has_no_live_process =
                state == k_concurrency_shm_state_dirty && no_live_processes;
            const bool clean_shm_has_only_dead_processes =
                state == k_concurrency_shm_state_clean && active_count > 0U && live_count == 0U;
            if (dirty_shm_has_no_live_process) {
                ++recovery_generation;
                rebuild_segments = true;
            } else if (clean_shm_has_only_dead_processes) {
                ++recovery_generation;
                rebuild_segments = true;
            }
        } else if (state != k_concurrency_shm_state_clean) {
            rebuild_segments = true;
        }
    }

    if (rebuild_segments) {
        if (!allow_recovery_rebuild && !initial_shared_memory &&
            concurrency_shm_rebuild_requires_recovery(shm_fd, shm_size)) {
            return MYLITE_BUSY;
        }
        if (allow_recovery_rebuild) {
            const int replay_result =
                replay_concurrency_tablespaces(database_path, page_log_fd, checkpoint_fd);
            if (replay_result != MYLITE_OK) {
                return replay_result;
            }
        }
        build_concurrency_shm_header(header, shm_size, database_uuid, recovery_generation);
        store_le32(
            header.data(),
            k_concurrency_shm_state_offset,
            k_concurrency_shm_state_rebuilding
        );
        if (!write_exact_at(shm_fd, header.data(), header.size(), 0)) {
            return MYLITE_IOERR;
        }
        const int segment_result =
            initialize_concurrency_shm_segments(shm_fd, page_log_fd, checkpoint_fd);
        if (segment_result != MYLITE_OK) {
            return segment_result;
        }
        if (!update_concurrency_shm_state(shm_fd, k_concurrency_shm_state_clean)) {
            return MYLITE_IOERR;
        }
    }
    return validate_concurrency_shm_mapping(shm_fd, shm_size, database_uuid);
}

int replay_concurrency_tablespaces(
    const std::filesystem::path &database_path,
    int page_log_fd,
    int checkpoint_fd
) {
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        return MYLITE_IOERR;
    }
    if (visible_lsn == 0U) {
        return MYLITE_OK;
    }

    const std::filesystem::path datadir = database_path / k_datadir_name;
    const std::string datadir_name = datadir.string();
    const int replay_result = mylite_ownerless_tablespace_replay_apply_with_flags(
        datadir_name.c_str(),
        page_log_fd,
        k_concurrency_recovery_header_size,
        visible_lsn,
        MYLITE_OWNERLESS_TABLESPACE_REPLAY_IGNORE_MISSING_TABLESPACES
    );
    if (replay_result != MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK) {
        return MYLITE_IOERR;
    }

    /*
     * Keep complete page-version records after materializing them into native
     * tablespaces. Native InnoDB startup can still replay an older local redo
     * view before the exclusive runtime has reconciled its checkpoint, so the
     * page-version WAL remains the authoritative ownerless recovery view.
     */
    const int checkpoint_result = mylite_ownerless_page_log_checkpoint_at(
        page_log_fd,
        k_concurrency_recovery_header_size,
        0U,
        nullptr,
        nullptr
    );
    return checkpoint_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? MYLITE_OK : MYLITE_IOERR;
}

bool concurrency_shm_header_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid
) {
    const std::uint32_t state = load_le32(header.data(), k_concurrency_shm_state_offset);
    return concurrency_shm_header_layout_matches(header, shm_size, database_uuid) &&
           (state == k_concurrency_shm_state_clean || state == k_concurrency_shm_state_dirty);
}

bool concurrency_shm_header_layout_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid
) {
    if (shm_size < static_cast<off_t>(k_concurrency_shm_header_size) ||
        database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    return concurrency_shm_header_identity_matches(header, database_uuid) &&
           load_le64(header.data(), k_concurrency_shm_mapping_size_offset) ==
               static_cast<std::uint64_t>(shm_size) &&
           load_le64(header.data(), k_concurrency_shm_generation_offset) == 0U &&
           load_le32(header.data(), k_concurrency_shm_segment_table_offset) ==
               k_concurrency_shm_segment_table_start &&
           load_le32(header.data(), k_concurrency_shm_segment_count_offset) ==
               k_concurrency_shm_segment_count;
}

bool concurrency_shm_segments_match(int shm_fd, off_t shm_size) {
    if (shm_size <
        static_cast<off_t>(
            k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size
        )) {
        return false;
    }

    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> process_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> wait_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> mdl_lock_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> trx_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> read_view_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> innodb_lock_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> redo_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_index_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> dictionary_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_write_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> autoinc_segment = {};
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> page_pin_segment = {};
    std::array<unsigned char, k_concurrency_process_registry_header_size> registry = {};
    std::array<unsigned char, k_concurrency_wait_channel_header_size> wait_channels = {};
    std::array<unsigned char, k_concurrency_mdl_lock_table_header_size> mdl_lock_table = {};
    std::array<unsigned char, k_concurrency_trx_registry_header_size> trx_registry = {};
    std::array<unsigned char, k_concurrency_read_view_registry_header_size> read_view_registry = {};
    std::array<unsigned char, k_concurrency_innodb_lock_registry_header_size> innodb_lock_registry =
        {};
    std::array<unsigned char, k_concurrency_innodb_lock_registry_header_size>
        page_write_lock_registry = {};
    std::array<unsigned char, MYLITE_OWNERLESS_AUTOINC_REGISTRY_HEADER_SIZE> autoinc_registry = {};
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_INDEX_HEADER_SIZE> page_index = {};
    std::array<unsigned char, MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_HEADER_SIZE> page_pin_registry =
        {};

    if (!read_exact_at(
            shm_fd,
            process_segment.data(),
            process_segment.size(),
            static_cast<off_t>(k_concurrency_shm_segment_table_start)
        ) ||
        !read_exact_at(
            shm_fd,
            wait_segment.data(),
            wait_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start + k_concurrency_shm_segment_descriptor_size
            )
        ) ||
        !read_exact_at(
            shm_fd,
            mdl_lock_segment.data(),
            mdl_lock_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (2U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            trx_segment.data(),
            trx_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (3U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            read_view_segment.data(),
            read_view_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (4U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            innodb_lock_segment.data(),
            innodb_lock_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (5U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            redo_segment.data(),
            redo_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (6U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_index_segment.data(),
            page_index_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (7U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            dictionary_segment.data(),
            dictionary_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (8U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_write_segment.data(),
            page_write_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (9U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            autoinc_segment.data(),
            autoinc_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (10U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            page_pin_segment.data(),
            page_pin_segment.size(),
            static_cast<off_t>(
                k_concurrency_shm_segment_table_start +
                (11U * k_concurrency_shm_segment_descriptor_size)
            )
        ) ||
        !read_exact_at(
            shm_fd,
            registry.data(),
            registry.size(),
            static_cast<off_t>(k_concurrency_process_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            wait_channels.data(),
            wait_channels.size(),
            static_cast<off_t>(k_concurrency_wait_channel_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            mdl_lock_table.data(),
            mdl_lock_table.size(),
            static_cast<off_t>(k_concurrency_mdl_lock_table_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            trx_registry.data(),
            trx_registry.size(),
            static_cast<off_t>(k_concurrency_trx_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            read_view_registry.data(),
            read_view_registry.size(),
            static_cast<off_t>(k_concurrency_read_view_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            innodb_lock_registry.data(),
            innodb_lock_registry.size(),
            static_cast<off_t>(k_concurrency_innodb_lock_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_index.data(),
            page_index.size(),
            static_cast<off_t>(k_concurrency_page_index_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_write_lock_registry.data(),
            page_write_lock_registry.size(),
            static_cast<off_t>(k_concurrency_page_write_lock_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            autoinc_registry.data(),
            autoinc_registry.size(),
            static_cast<off_t>(k_concurrency_autoinc_registry_offset)
        ) ||
        !read_exact_at(
            shm_fd,
            page_pin_registry.data(),
            page_pin_registry.size(),
            static_cast<off_t>(k_concurrency_page_pin_registry_offset)
        )) {
        return false;
    }

    const std::uint64_t process_active_count =
        load_le64(registry.data(), k_concurrency_registry_active_count_offset);
    const std::uint64_t mdl_lock_active_count =
        load_le64(mdl_lock_table.data(), k_concurrency_mdl_lock_header_active_count_offset);
    const std::uint64_t trx_active_count =
        load_le64(trx_registry.data(), k_concurrency_trx_header_active_count_offset);
    const std::uint64_t trx_next_id =
        load_le64(trx_registry.data(), k_concurrency_trx_header_next_id_offset);
    const std::uint64_t trx_oldest_active =
        load_le64(trx_registry.data(), k_concurrency_trx_header_oldest_active_offset);
    const std::uint64_t read_view_active_count =
        load_le64(read_view_registry.data(), k_concurrency_read_view_header_active_count_offset);
    const std::uint64_t innodb_lock_active_count = load_le64(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t innodb_lock_waiting_count = load_le64(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    const std::uint32_t innodb_lock_occupied_limit = load_le32(
        innodb_lock_registry.data(),
        k_concurrency_innodb_lock_header_occupied_limit_offset
    );
    const std::uint64_t page_write_lock_active_count = load_le64(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_active_count_offset
    );
    const std::uint64_t page_write_lock_waiting_count = load_le64(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_waiting_count_offset
    );
    const std::uint32_t page_write_lock_occupied_limit = load_le32(
        page_write_lock_registry.data(),
        k_concurrency_innodb_lock_header_occupied_limit_offset
    );
    const std::uint64_t page_index_active_count =
        load_le64(page_index.data(), k_concurrency_page_index_header_active_count_offset);
    const std::uint64_t page_pin_active_count =
        load_le64(page_pin_registry.data(), k_concurrency_page_pin_header_active_count_offset);

    return load_le32(process_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_process_registry_segment_type &&
           load_le32(process_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_process_registry_segment_version &&
           load_le64(process_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_process_registry_offset &&
           load_le64(process_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_process_registry_size &&
           load_le32(wait_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_wait_channel_segment_type &&
           load_le32(wait_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_wait_channel_segment_version &&
           load_le64(wait_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_wait_channel_offset &&
           load_le64(wait_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_wait_channel_segment_size &&
           load_le32(mdl_lock_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_mdl_lock_table_segment_type &&
           load_le32(mdl_lock_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_mdl_lock_table_segment_version &&
           load_le64(mdl_lock_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_mdl_lock_table_offset &&
           load_le64(mdl_lock_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_mdl_lock_table_segment_size &&
           load_le32(trx_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_trx_registry_segment_type &&
           load_le32(trx_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_trx_registry_segment_version &&
           load_le64(trx_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_trx_registry_offset &&
           load_le64(trx_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_trx_registry_segment_size &&
           load_le32(read_view_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_read_view_registry_segment_type &&
           load_le32(read_view_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_read_view_registry_segment_version &&
           load_le64(read_view_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_read_view_registry_offset &&
           load_le64(read_view_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_read_view_registry_segment_size &&
           load_le32(innodb_lock_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_innodb_lock_registry_segment_type &&
           load_le32(innodb_lock_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_innodb_lock_registry_segment_version &&
           load_le64(innodb_lock_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_innodb_lock_registry_offset &&
           load_le64(innodb_lock_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_innodb_lock_registry_segment_size &&
           load_le32(redo_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_redo_state_segment_type &&
           load_le32(redo_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_redo_state_segment_version &&
           load_le64(redo_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_redo_state_offset &&
           load_le64(redo_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_redo_state_segment_size &&
           load_le32(page_index_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_index_segment_type &&
           load_le32(page_index_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_index_segment_version &&
           load_le64(page_index_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_index_offset &&
           load_le64(page_index_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_index_segment_size &&
           load_le32(dictionary_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_dictionary_state_segment_type &&
           load_le32(dictionary_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_dictionary_state_segment_version &&
           load_le64(dictionary_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_dictionary_state_offset &&
           load_le64(dictionary_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_dictionary_state_segment_size &&
           load_le32(page_write_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_write_lock_registry_segment_type &&
           load_le32(page_write_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_write_lock_registry_segment_version &&
           load_le64(page_write_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_write_lock_registry_offset &&
           load_le64(page_write_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_write_lock_registry_segment_size &&
           load_le32(autoinc_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_autoinc_registry_segment_type &&
           load_le32(autoinc_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_autoinc_registry_segment_version &&
           load_le64(autoinc_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_autoinc_registry_offset &&
           load_le64(autoinc_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_autoinc_registry_segment_size &&
           load_le32(page_pin_segment.data(), k_concurrency_shm_segment_type_offset) ==
               k_concurrency_page_pin_registry_segment_type &&
           load_le32(page_pin_segment.data(), k_concurrency_shm_segment_version_offset) ==
               k_concurrency_page_pin_registry_segment_version &&
           load_le64(page_pin_segment.data(), k_concurrency_shm_segment_data_offset) ==
               k_concurrency_page_pin_registry_offset &&
           load_le64(page_pin_segment.data(), k_concurrency_shm_segment_length_offset) ==
               k_concurrency_page_pin_registry_segment_size &&
           load_le32(registry.data(), k_concurrency_registry_slot_count_offset) ==
               k_concurrency_process_slot_count &&
           load_le32(registry.data(), k_concurrency_registry_slot_size_offset) ==
               k_concurrency_process_slot_size &&
           process_active_count <= k_concurrency_process_slot_count &&
           load_le32(wait_channels.data(), k_concurrency_wait_header_channel_count_offset) ==
               k_concurrency_wait_channel_count &&
           load_le32(wait_channels.data(), k_concurrency_wait_header_channel_size_offset) ==
               k_concurrency_wait_channel_size &&
           load_le32(mdl_lock_table.data(), k_concurrency_mdl_lock_header_entry_count_offset) ==
               k_concurrency_mdl_lock_table_entry_count &&
           load_le32(mdl_lock_table.data(), k_concurrency_mdl_lock_header_entry_size_offset) ==
               k_concurrency_mdl_lock_table_entry_size &&
           mdl_lock_active_count <= k_concurrency_mdl_lock_table_entry_count &&
           load_le32(trx_registry.data(), k_concurrency_trx_header_slot_count_offset) ==
               k_concurrency_trx_slot_count &&
           load_le32(trx_registry.data(), k_concurrency_trx_header_slot_size_offset) ==
               k_concurrency_trx_slot_size &&
           trx_active_count <= k_concurrency_trx_slot_count &&
           trx_next_id >= k_concurrency_initial_trx_id &&
           (trx_active_count == 0U || process_active_count > 0U) &&
           ((trx_active_count == 0U && trx_oldest_active == 0U) ||
            (trx_active_count > 0U && trx_oldest_active >= k_concurrency_initial_trx_id &&
             trx_oldest_active < trx_next_id)) &&
           load_le32(read_view_registry.data(), k_concurrency_read_view_header_slot_count_offset) ==
               k_concurrency_read_view_slot_count &&
           load_le32(read_view_registry.data(), k_concurrency_read_view_header_slot_size_offset) ==
               k_concurrency_read_view_slot_size &&
           read_view_active_count <= k_concurrency_read_view_slot_count &&
           (read_view_active_count == 0U || process_active_count > 0U) &&
           load_le32(
               innodb_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_count_offset
           ) == k_concurrency_innodb_lock_slot_count &&
           load_le32(
               innodb_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_size_offset
           ) == k_concurrency_innodb_lock_slot_size &&
           innodb_lock_active_count <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_waiting_count <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_occupied_limit <= k_concurrency_innodb_lock_slot_count &&
           innodb_lock_active_count + innodb_lock_waiting_count <= innodb_lock_occupied_limit &&
           (innodb_lock_active_count == 0U || process_active_count > 0U) &&
           (innodb_lock_waiting_count == 0U || process_active_count > 0U) &&
           load_le32(page_index.data(), k_concurrency_page_index_header_entry_count_offset) ==
               k_concurrency_page_index_entry_count &&
           load_le32(page_index.data(), k_concurrency_page_index_header_entry_size_offset) ==
               MYLITE_OWNERLESS_PAGE_INDEX_ENTRY_SIZE &&
           page_index_active_count <= k_concurrency_page_index_entry_count &&
           load_le32(
               page_write_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_count_offset
           ) == k_concurrency_page_write_lock_slot_count &&
           load_le32(
               page_write_lock_registry.data(),
               k_concurrency_innodb_lock_header_slot_size_offset
           ) == k_concurrency_page_write_lock_slot_size &&
           page_write_lock_active_count <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_waiting_count <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_occupied_limit <= k_concurrency_page_write_lock_slot_count &&
           page_write_lock_active_count + page_write_lock_waiting_count <=
               page_write_lock_occupied_limit &&
           (page_write_lock_active_count == 0U || process_active_count > 0U) &&
           (page_write_lock_waiting_count == 0U || process_active_count > 0U) &&
           load_le32(autoinc_registry.data(), k_concurrency_registry_slot_count_offset) ==
               k_concurrency_autoinc_slot_count &&
           load_le32(autoinc_registry.data(), k_concurrency_registry_slot_size_offset) ==
               MYLITE_OWNERLESS_AUTOINC_REGISTRY_SLOT_SIZE &&
           load_le32(page_pin_registry.data(), k_concurrency_page_pin_header_slot_count_offset) ==
               k_concurrency_page_pin_slot_count &&
           load_le32(page_pin_registry.data(), k_concurrency_page_pin_header_slot_size_offset) ==
               MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_SLOT_SIZE &&
           page_pin_active_count <= k_concurrency_page_pin_slot_count &&
           (page_pin_active_count == 0U || process_active_count > 0U);
}

bool concurrency_shm_rebuild_requires_recovery(int shm_fd, off_t shm_size) {
    if (shm_size < static_cast<off_t>(
                       k_concurrency_page_pin_registry_offset +
                       k_concurrency_page_pin_registry_segment_size
                   ) ||
        static_cast<std::uintmax_t>(shm_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return true;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        return true;
    }

    const auto *base = static_cast<const unsigned char *>(mapping);
    const auto active_count_at = [&](std::size_t segment_offset, std::size_t count_offset) {
        return load_le64(base + segment_offset, count_offset);
    };
    const bool has_recovery_sensitive_entries =
        active_count_at(
            k_concurrency_mdl_lock_table_offset,
            k_concurrency_mdl_lock_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_trx_registry_offset,
            k_concurrency_trx_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_innodb_lock_registry_offset,
            k_concurrency_innodb_lock_header_active_count_offset
        ) > 0U ||
        active_count_at(
            k_concurrency_page_write_lock_registry_offset,
            k_concurrency_innodb_lock_header_active_count_offset
        ) > 0U;

    bool requires_recovery = has_recovery_sensitive_entries;
    if (!requires_recovery) {
        mylite_ownerless_dictionary_state_snapshot dictionary_snapshot = {};
        requires_recovery = mylite_ownerless_dictionary_state_read_snapshot(
                                base + k_concurrency_dictionary_state_offset,
                                k_concurrency_dictionary_state_segment_size,
                                &dictionary_snapshot
                            ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK ||
                            (dictionary_snapshot.generation & 1U) != 0U ||
                            dictionary_snapshot.active_owner_id != 0U;
    }
    if (!requires_recovery) {
        mylite_ownerless_redo_state_snapshot redo_snapshot = {};
        requires_recovery =
            mylite_ownerless_redo_state_read_snapshot(
                base + k_concurrency_redo_state_offset,
                k_concurrency_redo_state_segment_size,
                &redo_snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK ||
            redo_snapshot.refcount != 0U || redo_snapshot.active_reservation_count != 0U ||
            redo_snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED ||
            redo_snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED;
    }

    if (::munmap(mapping, mapping_size) != 0) {
        return true;
    }
    return requires_recovery;
}

bool concurrency_shm_header_identity_matches(
    const std::array<unsigned char, k_concurrency_shm_header_size> &header,
    std::string_view database_uuid
) {
    if (database_uuid.size() != k_database_uuid_size) {
        return false;
    }
    if (std::memcmp(
            header.data() + k_concurrency_shm_magic_offset,
            k_concurrency_shm_magic.data(),
            k_concurrency_shm_magic.size()
        ) != 0) {
        return false;
    }
    return load_le32(header.data(), k_concurrency_shm_format_offset) ==
               k_concurrency_shm_format_version &&
           load_le32(header.data(), k_concurrency_shm_min_format_offset) ==
               k_concurrency_shm_header_version_min &&
           load_le32(header.data(), k_concurrency_shm_header_size_offset) ==
               k_concurrency_shm_header_size &&
           load_le32(header.data(), k_concurrency_shm_byte_order_offset) ==
               k_concurrency_shm_byte_order &&
           load_le32(header.data(), k_concurrency_shm_flags_offset) == 0U &&
           std::memcmp(
               header.data() + k_concurrency_shm_database_uuid_offset,
               database_uuid.data(),
               database_uuid.size()
           ) == 0;
}

void build_concurrency_shm_header(
    std::array<unsigned char, k_concurrency_shm_header_size> &header,
    off_t shm_size,
    std::string_view database_uuid,
    std::uint64_t recovery_generation
) {
    header.fill(0U);
    std::memcpy(
        header.data() + k_concurrency_shm_magic_offset,
        k_concurrency_shm_magic.data(),
        k_concurrency_shm_magic.size()
    );
    store_le32(header.data(), k_concurrency_shm_format_offset, k_concurrency_shm_format_version);
    store_le32(
        header.data(),
        k_concurrency_shm_min_format_offset,
        k_concurrency_shm_header_version_min
    );
    store_le32(
        header.data(),
        k_concurrency_shm_header_size_offset,
        static_cast<std::uint32_t>(k_concurrency_shm_header_size)
    );
    store_le32(header.data(), k_concurrency_shm_byte_order_offset, k_concurrency_shm_byte_order);
    store_le32(header.data(), k_concurrency_shm_flags_offset, 0U);
    store_le32(header.data(), k_concurrency_shm_state_offset, k_concurrency_shm_state_clean);
    store_le64(
        header.data(),
        k_concurrency_shm_mapping_size_offset,
        static_cast<std::uint64_t>(shm_size)
    );
    store_le64(header.data(), k_concurrency_shm_generation_offset, 0U);
    store_le64(header.data(), k_concurrency_shm_recovery_generation_offset, recovery_generation);
    store_le32(
        header.data(),
        k_concurrency_shm_segment_table_offset,
        static_cast<std::uint32_t>(k_concurrency_shm_segment_table_start)
    );
    store_le32(
        header.data(),
        k_concurrency_shm_segment_count_offset,
        k_concurrency_shm_segment_count
    );
    std::memcpy(
        header.data() + k_concurrency_shm_database_uuid_offset,
        database_uuid.data(),
        database_uuid.size()
    );
}

int initialize_concurrency_shm_segments(int shm_fd, int page_log_fd, int checkpoint_fd) {
    if (!write_concurrency_segment_descriptor(
            shm_fd,
            0U,
            k_concurrency_process_registry_segment_type,
            k_concurrency_process_registry_segment_version,
            k_concurrency_process_registry_offset,
            k_concurrency_process_registry_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            1U,
            k_concurrency_wait_channel_segment_type,
            k_concurrency_wait_channel_segment_version,
            k_concurrency_wait_channel_offset,
            k_concurrency_wait_channel_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            2U,
            k_concurrency_mdl_lock_table_segment_type,
            k_concurrency_mdl_lock_table_segment_version,
            k_concurrency_mdl_lock_table_offset,
            k_concurrency_mdl_lock_table_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            3U,
            k_concurrency_trx_registry_segment_type,
            k_concurrency_trx_registry_segment_version,
            k_concurrency_trx_registry_offset,
            k_concurrency_trx_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            4U,
            k_concurrency_read_view_registry_segment_type,
            k_concurrency_read_view_registry_segment_version,
            k_concurrency_read_view_registry_offset,
            k_concurrency_read_view_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            5U,
            k_concurrency_innodb_lock_registry_segment_type,
            k_concurrency_innodb_lock_registry_segment_version,
            k_concurrency_innodb_lock_registry_offset,
            k_concurrency_innodb_lock_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            6U,
            k_concurrency_redo_state_segment_type,
            k_concurrency_redo_state_segment_version,
            k_concurrency_redo_state_offset,
            k_concurrency_redo_state_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            7U,
            k_concurrency_page_index_segment_type,
            k_concurrency_page_index_segment_version,
            k_concurrency_page_index_offset,
            k_concurrency_page_index_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            8U,
            k_concurrency_dictionary_state_segment_type,
            k_concurrency_dictionary_state_segment_version,
            k_concurrency_dictionary_state_offset,
            k_concurrency_dictionary_state_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            9U,
            k_concurrency_page_write_lock_registry_segment_type,
            k_concurrency_page_write_lock_registry_segment_version,
            k_concurrency_page_write_lock_registry_offset,
            k_concurrency_page_write_lock_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            10U,
            k_concurrency_autoinc_registry_segment_type,
            k_concurrency_autoinc_registry_segment_version,
            k_concurrency_autoinc_registry_offset,
            k_concurrency_autoinc_registry_segment_size
        ) ||
        !write_concurrency_segment_descriptor(
            shm_fd,
            11U,
            k_concurrency_page_pin_registry_segment_type,
            k_concurrency_page_pin_registry_segment_version,
            k_concurrency_page_pin_registry_offset,
            k_concurrency_page_pin_registry_segment_size
        )) {
        return MYLITE_IOERR;
    }

    const int registry_result = initialize_concurrency_process_registry(shm_fd);
    if (registry_result != MYLITE_OK) {
        return registry_result;
    }
    const int wait_channel_result = initialize_concurrency_wait_channels(shm_fd);
    if (wait_channel_result != MYLITE_OK) {
        return wait_channel_result;
    }
    const int mdl_lock_result = initialize_concurrency_mdl_lock_table(shm_fd);
    if (mdl_lock_result != MYLITE_OK) {
        return mdl_lock_result;
    }
    const int trx_result = initialize_concurrency_trx_registry(shm_fd);
    if (trx_result != MYLITE_OK) {
        return trx_result;
    }
    const int read_view_result = initialize_concurrency_read_view_registry(shm_fd);
    if (read_view_result != MYLITE_OK) {
        return read_view_result;
    }
    const int innodb_lock_result = initialize_concurrency_innodb_lock_registry(shm_fd);
    if (innodb_lock_result != MYLITE_OK) {
        return innodb_lock_result;
    }
    const int redo_result = initialize_concurrency_redo_state(shm_fd, checkpoint_fd);
    if (redo_result != MYLITE_OK) {
        return redo_result;
    }
    const int page_index_result = initialize_concurrency_page_index(shm_fd, page_log_fd);
    if (page_index_result != MYLITE_OK) {
        return page_index_result;
    }
    const int dictionary_result = initialize_concurrency_dictionary_state(shm_fd);
    if (dictionary_result != MYLITE_OK) {
        return dictionary_result;
    }
    const int page_write_result = initialize_concurrency_page_write_lock_registry(shm_fd);
    if (page_write_result != MYLITE_OK) {
        return page_write_result;
    }
    const int autoinc_result = initialize_concurrency_autoinc_registry(shm_fd);
    if (autoinc_result != MYLITE_OK) {
        return autoinc_result;
    }
    return initialize_concurrency_page_pin_registry(shm_fd);
}

bool write_concurrency_segment_descriptor(
    int shm_fd,
    std::uint32_t index,
    std::uint32_t type,
    std::uint32_t version,
    std::uint64_t offset,
    std::uint64_t length
) {
    std::array<unsigned char, k_concurrency_shm_segment_descriptor_size> segment = {};
    store_le32(segment.data(), k_concurrency_shm_segment_type_offset, type);
    store_le32(segment.data(), k_concurrency_shm_segment_version_offset, version);
    store_le64(segment.data(), k_concurrency_shm_segment_data_offset, offset);
    store_le64(segment.data(), k_concurrency_shm_segment_length_offset, length);
    store_le64(segment.data(), k_concurrency_shm_segment_generation_offset, 0U);
    return write_exact_at(
        shm_fd,
        segment.data(),
        segment.size(),
        static_cast<off_t>(
            k_concurrency_shm_segment_table_start +
            (index * k_concurrency_shm_segment_descriptor_size)
        )
    );
}

int initialize_concurrency_process_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_process_registry_size> registry = {};
    if (mylite_ownerless_process_registry_initialize(
            registry.data(),
            registry.size(),
            k_concurrency_process_slot_count
        ) != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               registry.data(),
               registry.size(),
               static_cast<off_t>(k_concurrency_process_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_wait_channels(int shm_fd) {
    std::array<unsigned char, k_concurrency_wait_channel_segment_size> wait_channels = {};
    store_le32(
        wait_channels.data(),
        k_concurrency_wait_header_channel_count_offset,
        k_concurrency_wait_channel_count
    );
    store_le32(
        wait_channels.data(),
        k_concurrency_wait_header_channel_size_offset,
        static_cast<std::uint32_t>(k_concurrency_wait_channel_size)
    );
    store_le64(wait_channels.data(), k_concurrency_wait_header_generation_offset, 0U);
    return write_exact_at(
               shm_fd,
               wait_channels.data(),
               wait_channels.size(),
               static_cast<off_t>(k_concurrency_wait_channel_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_mdl_lock_table(int shm_fd) {
    std::array<unsigned char, k_concurrency_mdl_lock_table_segment_size> lock_table = {};
    store_le32(
        lock_table.data(),
        k_concurrency_mdl_lock_header_entry_count_offset,
        k_concurrency_mdl_lock_table_entry_count
    );
    store_le32(
        lock_table.data(),
        k_concurrency_mdl_lock_header_entry_size_offset,
        static_cast<std::uint32_t>(k_concurrency_mdl_lock_table_entry_size)
    );
    store_le64(lock_table.data(), k_concurrency_mdl_lock_header_generation_offset, 0U);
    store_le64(lock_table.data(), k_concurrency_mdl_lock_header_active_count_offset, 0U);
    return write_exact_at(
               shm_fd,
               lock_table.data(),
               lock_table.size(),
               static_cast<off_t>(k_concurrency_mdl_lock_table_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_trx_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_trx_registry_segment_size> trx_registry = {};
    if (mylite_ownerless_trx_registry_initialize(
            trx_registry.data(),
            trx_registry.size(),
            k_concurrency_trx_slot_count,
            k_concurrency_initial_trx_id
        ) != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               trx_registry.data(),
               trx_registry.size(),
               static_cast<off_t>(k_concurrency_trx_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_read_view_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_read_view_registry_segment_size> read_view_registry{};
    if (mylite_ownerless_read_view_registry_initialize(
            read_view_registry.data(),
            read_view_registry.size(),
            k_concurrency_read_view_slot_count
        ) != MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               read_view_registry.data(),
               read_view_registry.size(),
               static_cast<off_t>(k_concurrency_read_view_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_pin_registry(int shm_fd) {
    std::array<unsigned char, k_concurrency_page_pin_registry_segment_size> page_pin_registry{};
    if (mylite_ownerless_page_pin_registry_initialize(
            page_pin_registry.data(),
            page_pin_registry.size(),
            k_concurrency_page_pin_slot_count
        ) != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               page_pin_registry.data(),
               page_pin_registry.size(),
               static_cast<off_t>(k_concurrency_page_pin_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_innodb_lock_registry(int shm_fd) {
    const std::size_t registry_size = k_concurrency_innodb_lock_registry_segment_size;
    std::vector<unsigned char> innodb_lock_registry(registry_size);
    if (mylite_ownerless_innodb_lock_registry_initialize(
            innodb_lock_registry.data(),
            innodb_lock_registry.size(),
            k_concurrency_innodb_lock_slot_count
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               innodb_lock_registry.data(),
               innodb_lock_registry.size(),
               static_cast<off_t>(k_concurrency_innodb_lock_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_write_lock_registry(int shm_fd) {
    std::vector<unsigned char> page_write_lock_registry(
        k_concurrency_page_write_lock_registry_segment_size
    );
    if (mylite_ownerless_innodb_lock_registry_initialize(
            page_write_lock_registry.data(),
            page_write_lock_registry.size(),
            k_concurrency_page_write_lock_slot_count
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               page_write_lock_registry.data(),
               page_write_lock_registry.size(),
               static_cast<off_t>(k_concurrency_page_write_lock_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_redo_state(int shm_fd, int checkpoint_fd) {
    std::array<unsigned char, k_concurrency_redo_state_segment_size> redo_state = {};
    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        return MYLITE_IOERR;
    }
    if (visible_lsn > latest_lsn) {
        latest_lsn = visible_lsn;
    }
    if (mylite_ownerless_redo_state_initialize(
            redo_state.data(),
            redo_state.size(),
            latest_lsn,
            visible_lsn
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               redo_state.data(),
               redo_state.size(),
               static_cast<off_t>(k_concurrency_redo_state_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_page_index(int shm_fd, int page_log_fd) {
    std::array<unsigned char, k_concurrency_page_index_segment_size> page_index = {};
    if (mylite_ownerless_page_index_initialize(
            page_index.data(),
            page_index.size(),
            k_concurrency_page_index_entry_count
        ) != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_IOERR;
    }
    const int replay_result =
        replay_concurrency_page_index(page_index.data(), page_index.size(), page_log_fd);
    if (replay_result != MYLITE_OK) {
        return replay_result;
    }
    return write_exact_at(
               shm_fd,
               page_index.data(),
               page_index.size(),
               static_cast<off_t>(k_concurrency_page_index_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

void reclaim_ownerless_page_log_after_native_checkpoint(RuntimeState &runtime) {
    if (runtime.readonly_mode || runtime.concurrency_wal_fd < 0 ||
        runtime.concurrency_checkpoint_fd < 0 || runtime.concurrency_shm_fd < 0 ||
        runtime.concurrency_process_slot_generation == 0U) {
        return;
    }

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(
            runtime.concurrency_checkpoint_fd,
            &latest_lsn,
            &visible_lsn
        ) ||
        visible_lsn == 0U) {
        return;
    }

    const bool no_live_peers = ownerless_runtime_has_no_live_peers(runtime);
    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    if (!no_live_peers &&
        snapshot_ownerless_page_version_pins(runtime, &active_pin_count, &oldest_pin_lsn) !=
            MYLITE_OK) {
        return;
    }
    const bool active_pin_reclaim = !no_live_peers && active_pin_count > 0U;
    if (active_pin_reclaim && (oldest_pin_lsn == 0U || oldest_pin_lsn > visible_lsn)) {
        return;
    }

    OwnerlessPageLogReclaimContext reclaim_context = {};
    reclaim_context.runtime = &runtime;
    reclaim_context.visible_lsn = visible_lsn;

    if (active_pin_reclaim) {
        if (active_pin_count == 1U) {
            static_cast<void>(mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at(
                runtime.concurrency_wal_fd,
                k_concurrency_recovery_header_size,
                visible_lsn,
                oldest_pin_lsn,
                collect_ownerless_reclaimed_page_index_record,
                prepare_ownerless_page_log_active_pin_reclaim,
                replace_ownerless_page_index_after_reclaim,
                &reclaim_context
            ));
        } else {
            static_cast<void>(mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at(
                runtime.concurrency_wal_fd,
                k_concurrency_recovery_header_size,
                visible_lsn,
                oldest_pin_lsn,
                collect_ownerless_reclaimed_page_index_record,
                prepare_ownerless_page_log_active_pin_reclaim,
                replace_ownerless_page_index_after_reclaim,
                &reclaim_context
            ));
        }
        return;
    }

    if (!prepare_ownerless_page_log_native_checkpoint_for_reclaim(runtime, visible_lsn)) {
        return;
    }

    const int checkpoint_result = mylite_ownerless_page_log_checkpoint_with_completion_at(
        runtime.concurrency_wal_fd,
        k_concurrency_recovery_header_size,
        visible_lsn,
        collect_ownerless_reclaimed_page_index_record,
        replace_ownerless_page_index_after_reclaim,
        &reclaim_context
    );
    if (checkpoint_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }
}

bool prepare_ownerless_page_log_native_checkpoint_for_reclaim(
    RuntimeState &runtime,
    std::uint64_t visible_lsn
) {
    if (mylite_ownerless_innodb_advance_external_lsn(visible_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }
    mylite_ownerless_innodb_refresh_external_pages(visible_lsn);
    if (mylite_ownerless_innodb_make_checkpoint() != MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }
    if (mylite_ownerless_innodb_checkpoint_covers_lsn(visible_lsn) !=
        MYLITE_OWNERLESS_INNODB_LOCK_OK) {
        return false;
    }

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("native-checkpoint-before-reclaim");
#  endif

    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    return mylite_ownerless_page_index_require_wal_scan(
               runtime_page_index(runtime),
               k_concurrency_page_index_segment_size,
               owner_id,
               runtime.concurrency_process_slot_generation
           ) == MYLITE_OWNERLESS_PAGE_INDEX_OK;
}

int prepare_ownerless_page_log_active_pin_reclaim(void *context) {
    auto *reclaim = static_cast<OwnerlessPageLogReclaimContext *>(context);
    if (reclaim == nullptr || reclaim->runtime == nullptr || reclaim->visible_lsn == 0U) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return prepare_ownerless_page_log_native_checkpoint_for_reclaim(
               *reclaim->runtime,
               reclaim->visible_lsn
           )
               ? MYLITE_OWNERLESS_PAGE_LOG_OK
               : MYLITE_OWNERLESS_PAGE_LOG_ERROR;
}

bool ownerless_runtime_has_no_live_peers(RuntimeState &runtime) {
    std::uint64_t active_count = 0;
    if (read_concurrency_process_active_count(runtime.concurrency_shm_fd, &active_count) !=
        MYLITE_OK) {
        return false;
    }
    if (active_count != 1U) {
        return false;
    }

    std::uint64_t live_count = 0;
    if (read_concurrency_process_live_count(runtime.concurrency_shm_fd, &live_count) != MYLITE_OK) {
        return false;
    }
    return live_count == 1U;
}

int snapshot_ownerless_page_version_pins(
    RuntimeState &runtime,
    std::uint32_t *out_active_count,
    std::uint64_t *out_oldest_read_lsn
) {
    void *page_pin_registry = runtime_page_pin_registry(runtime);
    if (page_pin_registry == nullptr || runtime.concurrency_process_slot_generation == 0U ||
        out_active_count == nullptr || out_oldest_read_lsn == nullptr) {
        return MYLITE_IOERR;
    }

    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    const int registry_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation,
        out_active_count,
        out_oldest_read_lsn
    );
    return registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ? MYLITE_OK : MYLITE_IOERR;
}

int initialize_concurrency_dictionary_state(int shm_fd) {
    std::array<unsigned char, k_concurrency_dictionary_state_segment_size> dictionary_state = {};
    if (mylite_ownerless_dictionary_state_initialize(
            dictionary_state.data(),
            dictionary_state.size()
        ) != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               dictionary_state.data(),
               dictionary_state.size(),
               static_cast<off_t>(k_concurrency_dictionary_state_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int initialize_concurrency_autoinc_registry(int shm_fd) {
    std::vector<unsigned char> autoinc_registry(k_concurrency_autoinc_registry_segment_size);
    if (mylite_ownerless_autoinc_registry_initialize(
            autoinc_registry.data(),
            autoinc_registry.size(),
            k_concurrency_autoinc_slot_count
        ) != MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_IOERR;
    }
    return write_exact_at(
               shm_fd,
               autoinc_registry.data(),
               autoinc_registry.size(),
               static_cast<off_t>(k_concurrency_autoinc_registry_offset)
           )
               ? MYLITE_OK
               : MYLITE_IOERR;
}

int replay_concurrency_page_index(void *page_index, std::size_t page_index_size, int page_log_fd) {
    if (page_index == nullptr || page_log_fd < 0) {
        return MYLITE_IOERR;
    }

    OwnerlessPageIndexRebuildContext context = {};
    context.page_index = page_index;
    context.page_index_size = page_index_size;
    context.owner_id = k_concurrency_bootstrap_latch_owner_id;
    context.owner_generation = static_cast<std::uint64_t>(::getpid());
    const int replay_result = mylite_ownerless_page_log_replay_at(
        page_log_fd,
        k_concurrency_recovery_header_size,
        replay_concurrency_page_index_record,
        &context
    );
    return replay_result == MYLITE_OWNERLESS_PAGE_LOG_OK ? MYLITE_OK : MYLITE_IOERR;
}

int replay_concurrency_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *rebuild = static_cast<OwnerlessPageIndexRebuildContext *>(context);
    if (rebuild == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    return page_log_result_from_page_index_result(mylite_ownerless_page_index_publish(
        rebuild->page_index,
        rebuild->page_index_size,
        rebuild->owner_id,
        rebuild->owner_generation,
        space_id,
        page_no,
        commit_lsn,
        page_lsn,
        record_offset
    ));
}

int collect_ownerless_reclaimed_page_index_record(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t commit_lsn,
    std::uint64_t record_offset,
    void *context
) {
    auto *reclaim = static_cast<OwnerlessPageLogReclaimContext *>(context);
    if (reclaim == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
    reclaim->retained_records.push_back(
        mylite_ownerless_page_index_record{
            space_id,
            page_no,
            commit_lsn,
            page_lsn,
            record_offset,
        }
    );
    return MYLITE_OWNERLESS_PAGE_LOG_OK;
}

int replace_ownerless_page_index_after_reclaim(void *context) {
    auto *reclaim = static_cast<OwnerlessPageLogReclaimContext *>(context);
    if (reclaim == nullptr || reclaim->runtime == nullptr) {
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }

    RuntimeState &runtime = *reclaim->runtime;
    const std::uint32_t owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    const mylite_ownerless_page_index_record *records =
        reclaim->retained_records.empty() ? nullptr : reclaim->retained_records.data();
    const int replace_result = mylite_ownerless_page_index_replace(
        runtime_page_index(runtime),
        k_concurrency_page_index_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation,
        records,
        reclaim->retained_records.size()
    );
    if (replace_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    }

    static_cast<void>(mylite_ownerless_page_index_require_wal_scan(
        runtime_page_index(runtime),
        k_concurrency_page_index_segment_size,
        owner_id,
        runtime.concurrency_process_slot_generation
    ));
    return page_log_result_from_page_index_result(replace_result);
}

int page_log_result_from_page_index_result(int result) {
    switch (result) {
    case MYLITE_OWNERLESS_PAGE_INDEX_OK:
        return MYLITE_OWNERLESS_PAGE_LOG_OK;
    case MYLITE_OWNERLESS_PAGE_INDEX_FULL:
        return MYLITE_OWNERLESS_PAGE_LOG_FULL;
    default:
        return MYLITE_OWNERLESS_PAGE_LOG_ERROR;
    }
}

int read_concurrency_process_active_count(int shm_fd, std::uint64_t *out_active_count) {
    if (out_active_count == nullptr) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size <
            static_cast<off_t>(
                k_concurrency_process_registry_offset + k_concurrency_process_registry_size
            ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    void *mapping = ::mmap(
        nullptr,
        static_cast<std::size_t>(shm_stat.st_size),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    const auto *registry =
        static_cast<const unsigned char *>(mapping) + k_concurrency_process_registry_offset;
    *out_active_count = mylite_ownerless_process_registry_active_count(registry);
    if (::munmap(mapping, static_cast<std::size_t>(shm_stat.st_size)) != 0) {
        return MYLITE_IOERR;
    }
    return MYLITE_OK;
}

int read_concurrency_process_live_count(int shm_fd, std::uint64_t *out_live_count) {
    if (out_live_count == nullptr) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size <
            static_cast<off_t>(
                k_concurrency_process_registry_offset + k_concurrency_process_registry_size
            ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_process_registry_header_size> registry_header = {};
    if (!read_exact_at(
            shm_fd,
            registry_header.data(),
            registry_header.size(),
            static_cast<off_t>(k_concurrency_process_registry_offset)
        )) {
        return MYLITE_IOERR;
    }
    if (load_le32(registry_header.data(), k_concurrency_registry_slot_count_offset) !=
            k_concurrency_process_slot_count ||
        load_le32(registry_header.data(), k_concurrency_registry_slot_size_offset) !=
            k_concurrency_process_slot_size) {
        return MYLITE_IOERR;
    }

    void *mapping = ::mmap(
        nullptr,
        static_cast<std::size_t>(shm_stat.st_size),
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0
    );
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    auto *registry = static_cast<unsigned char *>(mapping) + k_concurrency_process_registry_offset;
    std::uint64_t live_count = 0;
    const int registry_result = mylite_ownerless_process_registry_live_count(
        registry,
        k_concurrency_process_registry_size,
        ownerless_process_is_alive,
        nullptr,
        &live_count
    );
    const int unmap_result = ::munmap(mapping, static_cast<std::size_t>(shm_stat.st_size));
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK || unmap_result != 0) {
        return MYLITE_IOERR;
    }

    *out_live_count = live_count;
    return MYLITE_OK;
}

int validate_concurrency_shm_mapping(int shm_fd, off_t shm_size, std::string_view database_uuid) {
    if (shm_size < static_cast<off_t>(k_minimum_concurrency_shm_size) ||
        static_cast<std::uintmax_t>(shm_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return MYLITE_IOERR;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        return MYLITE_IOERR;
    }

    std::array<unsigned char, k_concurrency_shm_header_size> header = {};
    std::memcpy(header.data(), mapping, header.size());
    const bool valid = concurrency_shm_header_matches(header, shm_size, database_uuid) &&
                       concurrency_shm_segments_match(shm_fd, shm_size);
    const int unmap_result = ::munmap(mapping, mapping_size);
    return valid && unmap_result == 0 ? MYLITE_OK : MYLITE_IOERR;
}

int map_concurrency_shared_memory_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path shm_path =
        database_path / k_concurrency_dir_name / k_concurrency_shm_filename;
    const std::string shm_name = shm_path.string();
    const int shm_fd = ::open(shm_name.c_str(), O_RDWR | O_CLOEXEC);
    if (shm_fd < 0) {
        return MYLITE_IOERR;
    }

    struct stat shm_stat = {};
    if (::fstat(shm_fd, &shm_stat) != 0 ||
        shm_stat.st_size < static_cast<off_t>(
                               k_concurrency_page_pin_registry_offset +
                               k_concurrency_page_pin_registry_segment_size
                           ) ||
        static_cast<std::uintmax_t>(shm_stat.st_size) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        static_cast<void>(::close(shm_fd));
        return MYLITE_IOERR;
    }

    const std::size_t mapping_size = static_cast<std::size_t>(shm_stat.st_size);
    void *mapping = ::mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (mapping == MAP_FAILED) {
        static_cast<void>(::close(shm_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_shm_fd = shm_fd;
    runtime.concurrency_shm_mapping = mapping;
    runtime.concurrency_shm_mapping_size = mapping_size;
    runtime.ownerless_mdl_hook.lock_table =
        static_cast<unsigned char *>(mapping) + k_concurrency_mdl_lock_table_offset;
    runtime.ownerless_mdl_hook.lock_table_size = k_concurrency_mdl_lock_table_segment_size;
    runtime.ownerless_trx_hook.trx_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_trx_registry_offset;
    runtime.ownerless_trx_hook.trx_registry_size = k_concurrency_trx_registry_segment_size;
    runtime.ownerless_read_view_hook.read_view_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_read_view_registry_offset;
    runtime.ownerless_read_view_hook.read_view_registry_size =
        k_concurrency_read_view_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.lock_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_innodb_lock_registry_offset;
    runtime.ownerless_innodb_lock_hook.lock_registry_size =
        k_concurrency_innodb_lock_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.autoinc_registry = runtime_autoinc_registry(runtime);
    runtime.ownerless_innodb_lock_hook.autoinc_registry_size =
        k_concurrency_autoinc_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.page_write_lock_registry =
        static_cast<unsigned char *>(mapping) + k_concurrency_page_write_lock_registry_offset;
    runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.page_pin_registry = runtime_page_pin_registry(runtime);
    runtime.ownerless_innodb_lock_hook.page_pin_registry_size =
        k_concurrency_page_pin_registry_segment_size;
    runtime.ownerless_innodb_lock_hook.redo_state =
        static_cast<unsigned char *>(mapping) + k_concurrency_redo_state_offset;
    runtime.ownerless_innodb_lock_hook.redo_state_size = k_concurrency_redo_state_segment_size;
    runtime.ownerless_innodb_lock_hook.page_index = runtime_page_index(runtime);
    runtime.ownerless_innodb_lock_hook.page_index_size = k_concurrency_page_index_segment_size;
    runtime.ownerless_innodb_lock_hook.database_path = runtime.database_path.c_str();
    const int slot_result = allocate_concurrency_process_slot(runtime);
    if (slot_result != MYLITE_OK) {
        runtime.ownerless_mdl_hook = {};
        runtime.ownerless_trx_hook = {};
        runtime.ownerless_read_view_hook = {};
        runtime.ownerless_innodb_lock_hook = {};
        runtime.concurrency_shm_mapping = nullptr;
        runtime.concurrency_shm_mapping_size = 0;
        runtime.concurrency_shm_fd = -1;
        static_cast<void>(::munmap(mapping, mapping_size));
        static_cast<void>(::close(shm_fd));
        return slot_result;
    }

    return MYLITE_OK;
}

int open_concurrency_page_log_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path wal_path =
        database_path / k_concurrency_dir_name / k_concurrency_wal_filename;
    const std::string wal_name = wal_path.string();
    const int wal_fd = ::open(wal_name.c_str(), O_RDWR | O_CLOEXEC);
    if (wal_fd < 0) {
        return MYLITE_IOERR;
    }

    const int log_result =
        mylite_ownerless_page_log_initialize_at(wal_fd, k_concurrency_recovery_header_size);
    if (log_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        static_cast<void>(::close(wal_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_wal_fd = wal_fd;
    runtime.ownerless_innodb_lock_hook.page_log_fd = wal_fd;
    runtime.ownerless_innodb_lock_hook.page_log_offset = k_concurrency_recovery_header_size;
    return MYLITE_OK;
}

int open_concurrency_checkpoint_for_runtime(
    const std::filesystem::path &database_path,
    RuntimeState &runtime
) {
    const std::filesystem::path checkpoint_path =
        database_path / k_concurrency_dir_name / k_concurrency_checkpoint_filename;
    const std::string checkpoint_name = checkpoint_path.string();
    const int checkpoint_fd = ::open(checkpoint_name.c_str(), O_RDWR | O_CLOEXEC);
    if (checkpoint_fd < 0) {
        return MYLITE_IOERR;
    }

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    if (!read_concurrency_checkpoint_lsn(checkpoint_fd, &latest_lsn, &visible_lsn)) {
        static_cast<void>(::close(checkpoint_fd));
        return MYLITE_IOERR;
    }
    if (mylite_ownerless_redo_state_seed_checkpoint(
            runtime.ownerless_innodb_lock_hook.redo_state,
            runtime.ownerless_innodb_lock_hook.redo_state_size,
            latest_lsn,
            visible_lsn
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        static_cast<void>(::close(checkpoint_fd));
        return MYLITE_IOERR;
    }

    runtime.concurrency_checkpoint_fd = checkpoint_fd;
    runtime.ownerless_innodb_lock_hook.checkpoint_fd = checkpoint_fd;
    return MYLITE_OK;
}

int allocate_concurrency_process_slot(RuntimeState &runtime) {
    unsigned char *registry = runtime_process_registry(runtime);
    if (registry == nullptr || runtime.concurrency_shm_fd < 0) {
        return MYLITE_IOERR;
    }
    if (!update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_dirty)) {
        return MYLITE_IOERR;
    }

    OwnerlessProcessCleanupContext cleanup_context = {};
    cleanup_context.lock_table = runtime.ownerless_mdl_hook.lock_table;
    cleanup_context.lock_table_size = runtime.ownerless_mdl_hook.lock_table_size;
    cleanup_context.trx_registry = runtime_trx_registry(runtime);
    cleanup_context.trx_registry_size = k_concurrency_trx_registry_segment_size;
    cleanup_context.read_view_registry = runtime_read_view_registry(runtime);
    cleanup_context.read_view_registry_size = k_concurrency_read_view_registry_segment_size;
    cleanup_context.page_pin_registry = runtime_page_pin_registry(runtime);
    cleanup_context.page_pin_registry_size = k_concurrency_page_pin_registry_segment_size;
    cleanup_context.innodb_lock_registry = runtime_innodb_lock_registry(runtime);
    cleanup_context.innodb_lock_registry_size = k_concurrency_innodb_lock_registry_segment_size;
    cleanup_context.page_write_lock_registry = runtime_page_write_lock_registry(runtime);
    cleanup_context.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    cleanup_context.redo_state = runtime_redo_state(runtime);
    cleanup_context.redo_state_size = k_concurrency_redo_state_segment_size;
    cleanup_context.dictionary_state = runtime_dictionary_state(runtime);
    cleanup_context.dictionary_state_size = k_concurrency_dictionary_state_segment_size;
    cleanup_context.latch_owner_id = k_concurrency_bootstrap_latch_owner_id;
    cleanup_context.latch_owner_generation = static_cast<std::uint64_t>(::getpid());
    std::uint32_t cleaned_slots = 0;
    int registry_result = mylite_ownerless_process_registry_cleanup_dead_with_callback(
        registry,
        k_concurrency_process_registry_size,
        ownerless_process_is_alive,
        nullptr,
        ownerless_process_cleanup_dead_owner_state,
        &cleanup_context,
        &cleaned_slots
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    registry_result = mylite_ownerless_process_registry_allocate(
        registry,
        k_concurrency_process_registry_size,
        static_cast<std::uint64_t>(::getpid()),
        k_concurrency_process_open_mode_exclusive,
        load_le64(
            static_cast<unsigned char *>(runtime.concurrency_shm_mapping),
            k_concurrency_shm_generation_offset
        ),
        &slot_index,
        &slot_generation
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    registry_result = mylite_ownerless_process_registry_heartbeat(
        registry,
        k_concurrency_process_registry_size,
        slot_index,
        slot_generation,
        current_time_milliseconds()
    );
    if (registry_result != MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        static_cast<void>(mylite_ownerless_process_registry_release(
            registry,
            k_concurrency_process_registry_size,
            slot_index,
            slot_generation
        ));
        static_cast<void>(
            update_concurrency_shm_state(runtime.concurrency_shm_fd, k_concurrency_shm_state_clean)
        );
        return mylite_result_from_process_registry_result(registry_result);
    }

    unsigned char *slot = registry + k_concurrency_process_registry_header_size +
                          (slot_index * k_concurrency_process_slot_size);
    store_le64(
        slot,
        k_concurrency_process_slot_wait_channel_offset,
        k_concurrency_wait_channel_offset + k_concurrency_wait_channel_header_size
    );
    store_le64(
        slot,
        k_concurrency_process_slot_wait_channel_count_offset,
        k_concurrency_wait_channel_count
    );

    runtime.concurrency_process_slot_index = slot_index;
    runtime.concurrency_process_slot_generation = slot_generation;
    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    runtime.ownerless_mdl_hook.owner_id = owner_id;
    runtime.ownerless_mdl_hook.owner_generation = slot_generation;
    runtime.ownerless_trx_hook.owner_id = owner_id;
    runtime.ownerless_trx_hook.owner_generation = slot_generation;
    runtime.ownerless_read_view_hook.owner_id = owner_id;
    runtime.ownerless_read_view_hook.owner_generation = slot_generation;
    runtime.ownerless_innodb_lock_hook.owner_id = owner_id;
    runtime.ownerless_innodb_lock_hook.owner_generation = slot_generation;
    return MYLITE_OK;
}

int install_ownerless_runtime_lifecycle_hooks(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_process_slot_generation == 0U) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_runtime_set_hooks(ownerless_runtime_may_delete_shared_file_hook, &runtime);
    return MYLITE_OK;
}

int install_ownerless_innodb_lock_hooks(RuntimeState &runtime) {
    if (runtime.ownerless_innodb_lock_hook.lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_index == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_index_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_log_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.page_log_offset == 0U ||
        runtime.ownerless_innodb_lock_hook.checkpoint_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.database_path == nullptr ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_innodb_lock_set_hooks(
        ownerless_innodb_lock_acquire_table_hook,
        ownerless_innodb_lock_release_table_hook,
        ownerless_innodb_lock_wait_table_hook,
        ownerless_innodb_lock_acquire_record_hook,
        ownerless_innodb_lock_release_record_hook,
        ownerless_innodb_lock_acquire_page_write_hook,
        ownerless_innodb_lock_release_page_write_hook,
        ownerless_innodb_lock_release_page_writes_hook,
        ownerless_innodb_lock_wait_record_hook,
        ownerless_innodb_lock_wait_until_table_hook,
        ownerless_innodb_lock_wait_until_record_hook,
        ownerless_innodb_lock_before_record_wait_hook,
        ownerless_innodb_lock_clear_wait_hook,
        ownerless_innodb_redo_enter_hook,
        ownerless_innodb_redo_observe_hook,
        ownerless_innodb_redo_reserve_hook,
        ownerless_innodb_redo_written_hook,
        ownerless_innodb_redo_leave_hook,
        ownerless_innodb_pages_visible_hook,
        ownerless_innodb_page_publish_hook,
        ownerless_innodb_page_read_hook,
        &runtime.ownerless_innodb_lock_hook
    );
    mylite_ownerless_innodb_autoinc_set_hooks(
        ownerless_innodb_autoinc_read_hook,
        ownerless_innodb_autoinc_publish_hook,
        &runtime.ownerless_innodb_lock_hook
    );
    return MYLITE_OK;
}

int install_ownerless_runtime_hooks(RuntimeState &runtime) {
    if (runtime.ownerless_mdl_hook.lock_table == nullptr ||
        runtime.ownerless_mdl_hook.lock_table_size == 0U ||
        runtime.ownerless_mdl_hook.owner_id == 0U ||
        runtime.ownerless_mdl_hook.owner_generation == 0U ||
        runtime.ownerless_trx_hook.trx_registry == nullptr ||
        runtime.ownerless_trx_hook.trx_registry_size == 0U ||
        runtime.ownerless_trx_hook.owner_id == 0U ||
        runtime.ownerless_trx_hook.owner_generation == 0U ||
        runtime.ownerless_read_view_hook.read_view_registry == nullptr ||
        runtime.ownerless_read_view_hook.read_view_registry_size == 0U ||
        runtime.ownerless_read_view_hook.owner_id == 0U ||
        runtime.ownerless_read_view_hook.owner_generation == 0U ||
        runtime.ownerless_innodb_lock_hook.lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.autoinc_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_write_lock_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_pin_registry_size == 0U ||
        runtime.ownerless_innodb_lock_hook.redo_state == nullptr ||
        runtime.ownerless_innodb_lock_hook.redo_state_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_index == nullptr ||
        runtime.ownerless_innodb_lock_hook.page_index_size == 0U ||
        runtime.ownerless_innodb_lock_hook.page_log_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.page_log_offset == 0U ||
        runtime.ownerless_innodb_lock_hook.checkpoint_fd < 0 ||
        runtime.ownerless_innodb_lock_hook.database_path == nullptr ||
        runtime.ownerless_innodb_lock_hook.owner_id == 0U ||
        runtime.ownerless_innodb_lock_hook.owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    const std::uint64_t local_max_trx_id =
        std::max<std::uint64_t>(mylite_ownerless_trx_local_max_id(), k_concurrency_initial_trx_id);
    const int seed_result = mylite_ownerless_trx_registry_ensure_next_id_at_least(
        runtime.ownerless_trx_hook.trx_registry,
        runtime.ownerless_trx_hook.trx_registry_size,
        runtime.ownerless_trx_hook.owner_id,
        runtime.ownerless_trx_hook.owner_generation,
        local_max_trx_id
    );
    if (seed_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_IOERR;
    }

    mylite_ownerless_mdl_set_hooks(
        ownerless_mdl_acquire_hook,
        ownerless_mdl_release_hook,
        &runtime.ownerless_mdl_hook
    );
    mylite_ownerless_trx_set_hooks(
        ownerless_trx_allocate_hook,
        ownerless_trx_register_hook,
        ownerless_trx_assign_no_hook,
        ownerless_trx_deregister_hook,
        ownerless_trx_snapshot_hook,
        &runtime.ownerless_trx_hook
    );
    mylite_ownerless_read_view_set_hooks(
        ownerless_read_view_register_hook,
        ownerless_read_view_deregister_hook,
        ownerless_read_view_snapshot_hook,
        &runtime.ownerless_read_view_hook
    );
    return install_ownerless_innodb_lock_hooks(runtime);
}

int refresh_ownerless_external_pages_before_statement(
    mylite_db &db,
    bool allow_page_version_reads,
    bool allow_global_refresh
) {
    mylite_ownerless_innodb_clear_external_page_visibility();

    std::uint64_t latest_lsn = 0;
    std::uint64_t visible_lsn = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.concurrency_shm_mapping == nullptr ||
            g_runtime.ownerless_innodb_lock_hook.redo_state == nullptr) {
            return MYLITE_OK;
        }
        latest_lsn = load_shared64(
            static_cast<unsigned char *>(g_runtime.ownerless_innodb_lock_hook.redo_state),
            k_concurrency_redo_state_latest_lsn_offset
        );
        visible_lsn = load_shared64(
            static_cast<unsigned char *>(g_runtime.ownerless_innodb_lock_hook.redo_state),
            k_concurrency_redo_state_visible_lsn_offset
        );
    }

    const std::uint64_t live_read_lsn = std::max(latest_lsn, visible_lsn);
    std::uint64_t refresh_lsn = visible_lsn;
    std::uint64_t page_version_read_lsn = allow_page_version_reads ? live_read_lsn : 0U;
    if (ownerless_connection_is_in_explicit_transaction(db) && allow_page_version_reads) {
        if (db.ownerless_transaction_snapshot_visibility_pinned) {
            page_version_read_lsn = db.ownerless_transaction_snapshot_visible_lsn;
            const int pin_result =
                ensure_ownerless_transaction_page_version_pin(db, page_version_read_lsn);
            if (pin_result != MYLITE_OK) {
                return pin_result;
            }
        } else if (ownerless_transaction_pins_consistent_reads(db)) {
            page_version_read_lsn = live_read_lsn;
            const int pin_result =
                ensure_ownerless_transaction_page_version_pin(db, page_version_read_lsn);
            if (pin_result != MYLITE_OK) {
                return pin_result;
            }
            db.ownerless_transaction_snapshot_visible_lsn = page_version_read_lsn;
            db.ownerless_transaction_snapshot_visibility_pinned = true;
        }
    }
    if (refresh_lsn == 0U && page_version_read_lsn == 0U) {
        return refresh_ownerless_dictionary_before_statement(db, allow_global_refresh);
    }

    if (allow_global_refresh && refresh_lsn > db.ownerless_observed_lsn) {
        mylite_ownerless_innodb_refresh_external_pages(refresh_lsn);
        db.ownerless_observed_lsn = refresh_lsn;
    }
    if (allow_page_version_reads && page_version_read_lsn > db.ownerless_clean_pages_evicted_lsn) {
        mylite_ownerless_innodb_refresh_buffer_pool_pages(page_version_read_lsn);
        db.ownerless_clean_pages_evicted_lsn = page_version_read_lsn;
    }

    if (allow_global_refresh && refresh_lsn > db.ownerless_observed_visible_lsn) {
        const std::uint64_t previous_visible_lsn =
            mylite_ownerless_innodb_push_external_page_visibility(refresh_lsn);
        mylite_ownerless_innodb_refresh_external_space_headers();
        mylite_ownerless_innodb_restore_external_page_visibility(previous_visible_lsn);
        db.ownerless_observed_visible_lsn = refresh_lsn;
    }

    if (allow_page_version_reads && page_version_read_lsn != 0U) {
        mylite_ownerless_innodb_enable_external_page_visibility(page_version_read_lsn);
    }
    return refresh_ownerless_dictionary_before_statement(db, allow_global_refresh);
}

int read_ownerless_pressure_state(mylite_db &db, OwnerlessPressureState &state) {
    state = OwnerlessPressureState{};
    if (!db.ownerless_rw_open) {
        return MYLITE_OK;
    }
    state.page_log_limit_bytes = db.ownerless_page_log_limit_bytes;

    void *page_pin_registry = nullptr;
    int page_log_fd = -1;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        page_log_fd = g_runtime.concurrency_wal_fd;
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }
    if (page_pin_registry == nullptr || page_log_fd < 0 || owner_id == 0U ||
        owner_generation == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless page-version pressure state is unavailable");
        return MYLITE_IOERR;
    }

    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    const int pin_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_generation,
        &active_pin_count,
        &oldest_pin_lsn
    );
    if (pin_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        set_error(db, MYLITE_IOERR, "ownerless page-version pressure state could not be read");
        return MYLITE_IOERR;
    }

    struct stat page_log_stat = {};
    if (::fstat(page_log_fd, &page_log_stat) != 0 || page_log_stat.st_size < 0) {
        set_error(db, MYLITE_IOERR, "ownerless page-version WAL size could not be read");
        return MYLITE_IOERR;
    }

    state.active_pin_count = active_pin_count;
    state.oldest_pin_lsn = active_pin_count != 0U ? oldest_pin_lsn : 0U;
    state.page_log_bytes = static_cast<std::uint64_t>(page_log_stat.st_size);
    state.page_log_limit_reached = state.active_pin_count != 0U && state.oldest_pin_lsn != 0U &&
                                   state.page_log_limit_bytes != 0U &&
                                   state.page_log_bytes > k_empty_ownerless_page_log_size &&
                                   state.page_log_bytes >= state.page_log_limit_bytes;
    return MYLITE_OK;
}

int enforce_ownerless_page_log_limit_policy(mylite_db &db, const SqlPolicyTokens &tokens) {
    if (!db.ownerless_rw_open || db.ownerless_page_log_limit_bytes == 0U ||
        !sql_statement_requires_write(tokens)) {
        return MYLITE_OK;
    }

    OwnerlessPressureState state;
    const int pressure_result = read_ownerless_pressure_state(db, state);
    if (pressure_result != MYLITE_OK) {
        return pressure_result;
    }
    if (!state.page_log_limit_reached) {
        return MYLITE_OK;
    }

    set_error(db, MYLITE_BUSY, "ownerless page-version WAL pressure limit reached");
    return MYLITE_BUSY;
}

int refresh_ownerless_dictionary_before_statement(mylite_db &db, bool allow_global_refresh) {
    if (!db.ownerless_rw_open || !allow_global_refresh) {
        return MYLITE_OK;
    }

    void *dictionary_state = nullptr;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
    }
    if (dictionary_state == nullptr) {
        return MYLITE_OK;
    }

    std::uint64_t generation = 0;
    const int wait_result = mylite_ownerless_dictionary_state_wait_ready(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        ownerless_process_is_alive,
        nullptr,
        k_concurrency_lock_wait_timeout_ms,
        &generation
    );
    if (wait_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary change is still active or needs recovery");
        return ownerless_dictionary_result_from_state_result(wait_result);
    }

    if (generation == db.ownerless_observed_dictionary_generation) {
        return MYLITE_OK;
    }
    const int flush_result = flush_ownerless_dictionary_cache(db);
    if (flush_result == MYLITE_OK) {
        db.ownerless_observed_dictionary_generation = generation;
    }
    return flush_result;
}

int flush_ownerless_dictionary_cache(mylite_db &db) {
    if (mysql_query(&db.mysql, "FLUSH TABLES") != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    const int drain_result = drain_remaining_query_results(db);
    if (drain_result != MYLITE_OK) {
        return drain_result;
    }
    mylite_ownerless_innodb_evict_dictionary_cache();
    return MYLITE_OK;
}

void initialize_ownerless_dictionary_generation(mylite_db &db) {
    if (!db.ownerless_rw_open) {
        return;
    }

    void *dictionary_state = nullptr;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
    }
    if (dictionary_state == nullptr) {
        return;
    }

    std::uint64_t generation = 0;
    if (mylite_ownerless_dictionary_state_wait_ready(
            dictionary_state,
            k_concurrency_dictionary_state_segment_size,
            ownerless_process_is_alive,
            nullptr,
            k_concurrency_lock_wait_timeout_ms,
            &generation
        ) == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        db.ownerless_observed_dictionary_generation = generation;
    }
}

bool ownerless_dictionary_ddl_statement(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    return ownerless_dictionary_ddl_statement(tokens);
}

bool ownerless_temporary_table_ddl_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_equals(first, "CREATE")) {
        for (std::size_t index = 1; index < tokens.count; ++index) {
            const std::string_view token = identifier_token_at(tokens, index);
            if (token_equals(token, "TEMPORARY")) {
                return token_equals(identifier_token_at(tokens, index + 1U), "TABLE");
            }
            if (token_equals(token, "TABLE")) {
                return false;
            }
        }
        return false;
    }

    if (!token_equals(first, "DROP")) {
        return false;
    }
    for (std::size_t index = 1; index < tokens.count; ++index) {
        const std::string_view token = identifier_token_at(tokens, index);
        if (token_equals(token, "TEMPORARY")) {
            return token_equals(identifier_token_at(tokens, index + 1U), "TABLE");
        }
        if (token_equals(token, "TABLE")) {
            return false;
        }
    }
    return false;
}

bool ownerless_dictionary_ddl_statement(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    return token_in(first, "ALTER", "CREATE", "DROP", "RENAME") || token_equals(first, "TRUNCATE");
}

bool ownerless_table_identifier_token(std::string_view token) {
    return is_sql_identifier_token(token) ||
           (token.size() >= 2U && token.front() == '`' && token.back() == '`');
}

bool ownerless_statement_uses_tracked_temporary_table(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (db.ownerless_temporary_table_names.empty()) {
        return false;
    }

    for (std::size_t index = 0; index < tokens.count; ++index) {
        if (!ownerless_table_identifier_token(tokens.values[index])) {
            continue;
        }
        const std::string_view token = unquoted_identifier_token(tokens.values[index]);
        for (const std::string &table_name : db.ownerless_temporary_table_names) {
            if (token_equals(token, table_name.c_str())) {
                return true;
            }
        }
    }
    return false;
}

bool ownerless_statement_uses_temporary_table(const mylite_db &db, const SqlPolicyTokens &tokens) {
    return ownerless_temporary_table_ddl_statement(tokens) ||
           ownerless_statement_uses_tracked_temporary_table(db, tokens);
}

std::string ownerless_temporary_table_name_from_ddl(const SqlPolicyTokens &tokens) {
    bool table_keyword_seen = false;
    std::string table_name;
    for (std::size_t index = 0; index < tokens.count; ++index) {
        const std::string_view token = tokens.values[index];
        if (!table_keyword_seen) {
            table_keyword_seen = ownerless_table_identifier_token(token) &&
                                 token_equals(unquoted_identifier_token(token), "TABLE");
            continue;
        }
        if (token == "(" || token == "," || token == ";") {
            break;
        }
        if (!ownerless_table_identifier_token(token)) {
            continue;
        }
        const std::string_view identifier = unquoted_identifier_token(token);
        if (token_in(identifier, "IF", "NOT", "EXISTS")) {
            continue;
        }
        table_name.assign(identifier.data(), identifier.size());
    }
    return table_name;
}

void update_ownerless_temporary_table_state_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    if (!db.ownerless_rw_open || !ownerless_temporary_table_ddl_statement(tokens)) {
        return;
    }

    const std::string table_name = ownerless_temporary_table_name_from_ddl(tokens);
    if (table_name.empty()) {
        return;
    }

    const std::string_view first = identifier_token_at(tokens, 0);
    if (token_equals(first, "CREATE")) {
        const bool already_tracked = std::any_of(
            db.ownerless_temporary_table_names.begin(),
            db.ownerless_temporary_table_names.end(),
            [&](const std::string &tracked_name) {
                return token_equals(table_name, tracked_name.c_str());
            }
        );
        if (!already_tracked) {
            db.ownerless_temporary_table_names.push_back(table_name);
        }
        return;
    }

    if (!token_equals(first, "DROP")) {
        return;
    }
    db.ownerless_temporary_table_names.erase(
        std::remove_if(
            db.ownerless_temporary_table_names.begin(),
            db.ownerless_temporary_table_names.end(),
            [&](const std::string &tracked_name) {
                return token_equals(table_name, tracked_name.c_str());
            }
        ),
        db.ownerless_temporary_table_names.end()
    );
}

char ownerless_ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

std::string ownerless_normalized_identifier(std::string_view token) {
    token = unquoted_identifier_token(token);
    std::string normalized;
    normalized.reserve(token.size());
    for (char value : token) {
        normalized.push_back(ownerless_ascii_lower(value));
    }
    return normalized;
}

bool ownerless_tracked_temporary_table_name(const mylite_db &db, std::string_view table_name) {
    for (const std::string &tracked_name : db.ownerless_temporary_table_names) {
        if (token_equals(table_name, tracked_name.c_str())) {
            return true;
        }
    }
    return false;
}

bool ownerless_token_in_any(std::string_view token, std::initializer_list<const char *> keywords) {
    for (const char *keyword : keywords) {
        if (token_equals(token, keyword)) {
            return true;
        }
    }
    return false;
}

bool ownerless_table_reference_stop_token(std::string_view token) {
    return ownerless_token_in_any(
        token,
        {"WHERE",
         "SET",
         "VALUES",
         "VALUE",
         "ON",
         "ORDER",
         "GROUP",
         "HAVING",
         "LIMIT",
         "RETURNING",
         "PROCEDURE",
         "FOR",
         "LOCK",
         "UNION",
         "EXCEPT",
         "INTERSECT",
         "WINDOW",
         "PARTITION"}
    );
}

bool ownerless_table_reference_skip_token(std::string_view token) {
    return ownerless_token_in_any(
        token,
        {"LOW_PRIORITY",
         "DELAYED",
         "HIGH_PRIORITY",
         "IGNORE",
         "QUICK",
         "FROM",
         "INTO",
         "TABLE",
         "ONLY",
         "AS"}
    );
}

std::string_view ownerless_raw_identifier_token_at(
    const SqlPolicyTokens &tokens,
    std::size_t index
) {
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return {};
    }
    return unquoted_identifier_token(tokens.values[index]);
}

std::size_t ownerless_add_table_statement_lock_key(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t index,
    std::vector<std::string> &keys
) {
    if (index >= tokens.count || !ownerless_table_identifier_token(tokens.values[index])) {
        return index + 1U;
    }

    std::string schema_name;
    std::string table_name;
    std::size_t next_index = index + 1U;
    if (index + 2U < tokens.count && tokens.values[index + 1U] == "." &&
        ownerless_table_identifier_token(tokens.values[index + 2U])) {
        schema_name = ownerless_normalized_identifier(tokens.values[index]);
        table_name = ownerless_normalized_identifier(tokens.values[index + 2U]);
        next_index = index + 3U;
    } else {
        schema_name = ownerless_normalized_identifier(db.current_schema);
        table_name = ownerless_normalized_identifier(tokens.values[index]);
    }

    if (table_name.empty() || ownerless_table_reference_stop_token(table_name) ||
        ownerless_table_reference_skip_token(table_name) ||
        ownerless_tracked_temporary_table_name(db, table_name)) {
        return next_index;
    }

    std::string key;
    if (!schema_name.empty()) {
        key = schema_name + ".";
    }
    key += table_name;
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(std::move(key));
    }
    return next_index;
}

std::size_t ownerless_collect_table_references_until_clause(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t index,
    std::vector<std::string> &keys
) {
    while (index < tokens.count) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (ownerless_table_reference_stop_token(token)) {
            break;
        }
        if (ownerless_table_reference_skip_token(token) || ownerless_token_in_any(
                                                               token,
                                                               {"JOIN",
                                                                "INNER",
                                                                "LEFT",
                                                                "RIGHT",
                                                                "FULL",
                                                                "CROSS",
                                                                "OUTER",
                                                                "STRAIGHT_JOIN",
                                                                "NATURAL",
                                                                "USE",
                                                                "FORCE",
                                                                "IGNORE",
                                                                "KEY",
                                                                "INDEX"}
                                                           )) {
            ++index;
            continue;
        }
        if (tokens.values[index] == "," || tokens.values[index] == "(" ||
            tokens.values[index] == ")") {
            ++index;
            continue;
        }
        if (ownerless_table_identifier_token(tokens.values[index])) {
            index = ownerless_add_table_statement_lock_key(db, tokens, index, keys);
            continue;
        }
        ++index;
    }
    return index;
}

std::size_t ownerless_first_write_keyword_index(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 0; index < tokens.count; ++index) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_in(token, "DELETE", "INSERT", "LOAD", "REPLACE") ||
            token_equals(token, "UPDATE")) {
            return index;
        }
    }
    return tokens.count;
}

void ownerless_collect_update_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t update_index,
    std::vector<std::string> &keys
) {
    bool direct_table_seen = false;
    for (std::size_t index = update_index + 1U; index < tokens.count;) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_equals(token, "SET")) {
            return;
        }
        if (ownerless_table_reference_skip_token(token)) {
            ++index;
            continue;
        }
        if (ownerless_token_in_any(
                token,
                {"JOIN",
                 "INNER",
                 "LEFT",
                 "RIGHT",
                 "FULL",
                 "CROSS",
                 "OUTER",
                 "STRAIGHT_JOIN",
                 "NATURAL"}
            )) {
            index = ownerless_collect_table_references_until_clause(db, tokens, index + 1U, keys);
            continue;
        }
        if (!direct_table_seen && ownerless_table_identifier_token(tokens.values[index])) {
            index = ownerless_add_table_statement_lock_key(db, tokens, index, keys);
            direct_table_seen = true;
            continue;
        }
        if (tokens.values[index] == ",") {
            direct_table_seen = false;
        }
        ++index;
    }
}

void ownerless_collect_insert_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t insert_index,
    std::vector<std::string> &keys
) {
    std::size_t index = insert_index + 1U;
    while (index < tokens.count &&
           ownerless_table_reference_skip_token(ownerless_raw_identifier_token_at(tokens, index))) {
        ++index;
    }
    if (index < tokens.count) {
        ownerless_add_table_statement_lock_key(db, tokens, index, keys);
    }
}

void ownerless_collect_delete_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t delete_index,
    std::vector<std::string> &keys
) {
    for (std::size_t index = delete_index + 1U; index < tokens.count; ++index) {
        const std::string_view token = ownerless_raw_identifier_token_at(tokens, index);
        if (token_in(token, "FROM", "USING")) {
            ownerless_collect_table_references_until_clause(db, tokens, index + 1U, keys);
            return;
        }
    }
}

void ownerless_collect_load_statement_lock_keys(
    const mylite_db &db,
    const SqlPolicyTokens &tokens,
    std::size_t load_index,
    std::vector<std::string> &keys
) {
    for (std::size_t index = load_index + 1U; index + 1U < tokens.count; ++index) {
        if (token_equals(ownerless_raw_identifier_token_at(tokens, index), "INTO") &&
            token_equals(ownerless_raw_identifier_token_at(tokens, index + 1U), "TABLE")) {
            ownerless_add_table_statement_lock_key(db, tokens, index + 2U, keys);
            return;
        }
    }
}

std::uint64_t ownerless_statement_lock_hash(std::string_view key) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (char value : key) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<OwnerlessStatementLockRequest> ownerless_autocommit_write_statement_lock_requests(
    const mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    std::vector<OwnerlessStatementLockRequest> requests;
    if (ownerless_connection_is_in_explicit_transaction(db) ||
        ownerless_dictionary_ddl_statement(tokens) || !sql_statement_requires_write(tokens)) {
        return requests;
    }

    const bool temporary_statement = ownerless_temporary_table_ddl_statement(tokens) ||
                                     ownerless_statement_uses_tracked_temporary_table(db, tokens);
    std::vector<std::string> keys;
    const std::size_t write_index = ownerless_first_write_keyword_index(tokens);
    if (write_index < tokens.count) {
        const std::string_view write_keyword =
            ownerless_raw_identifier_token_at(tokens, write_index);
        if (token_equals(write_keyword, "UPDATE")) {
            ownerless_collect_update_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_in(write_keyword, "INSERT", "REPLACE")) {
            ownerless_collect_insert_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_equals(write_keyword, "DELETE")) {
            ownerless_collect_delete_statement_lock_keys(db, tokens, write_index, keys);
        } else if (token_equals(write_keyword, "LOAD")) {
            ownerless_collect_load_statement_lock_keys(db, tokens, write_index, keys);
        }
    }

    if (keys.empty()) {
        if (!temporary_statement) {
            requests.push_back(
                {k_global_write_statement_lock_start, k_global_write_statement_lock_length, F_WRLCK}
            );
        }
        return requests;
    }

    std::vector<off_t> table_offsets;
    table_offsets.reserve(keys.size());
    for (const std::string &key : keys) {
        const std::uint64_t slot =
            ownerless_statement_lock_hash(key) % k_table_statement_lock_slot_count;
        table_offsets.push_back(k_table_statement_lock_start + static_cast<off_t>(slot));
    }
    std::sort(table_offsets.begin(), table_offsets.end());
    table_offsets.erase(
        std::unique(table_offsets.begin(), table_offsets.end()),
        table_offsets.end()
    );

    requests.push_back(
        {k_global_write_statement_lock_start, k_global_write_statement_lock_length, F_RDLCK}
    );
    for (const off_t table_offset : table_offsets) {
        requests.push_back({table_offset, k_table_statement_lock_length, F_WRLCK});
    }
    return requests;
}

int ownerless_statement_lock_fd(mylite_db &db) {
    const std::filesystem::path lock_path = std::filesystem::path(db.database_path) /
                                            k_concurrency_dir_name / k_statement_lock_filename;
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ownerless_statement_lock_fd >= 0) {
        return g_runtime.ownerless_statement_lock_fd;
    }

    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error(db, MYLITE_IOERR, "ownerless statement lock file could not be opened");
        return -1;
    }

    g_runtime.ownerless_statement_lock_fd = lock_fd;
    return g_runtime.ownerless_statement_lock_fd;
}

int acquire_ownerless_statement_locks(
    mylite_db &db,
    std::string_view sql,
    OwnerlessStatementLocks &lock
) {
    lock.release();
    if (!db.ownerless_rw_open) {
        return MYLITE_OK;
    }

    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const bool dictionary_ddl = ownerless_dictionary_ddl_statement(tokens);
    short lock_type = F_UNLCK;
    if (dictionary_ddl) {
        lock_type = F_WRLCK;
    } else if (sql_statement_requires_write(tokens) || sql_statement_uses_locking_read(tokens)) {
        lock_type = F_RDLCK;
    } else {
        return MYLITE_OK;
    }

    const int lock_fd = ownerless_statement_lock_fd(db);
    if (lock_fd < 0) {
        return MYLITE_IOERR;
    }
    if (!acquire_fd_range_lock(
            lock_fd,
            k_dictionary_statement_lock_start,
            k_dictionary_statement_lock_length,
            lock_type,
            k_statement_lock_wait_timeout_ms
        )) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary statement lock is busy");
        return MYLITE_BUSY;
    }

    lock.add(lock_fd, k_dictionary_statement_lock_start, k_dictionary_statement_lock_length);
    const std::vector<OwnerlessStatementLockRequest> table_write_locks =
        ownerless_autocommit_write_statement_lock_requests(db, tokens);
    for (const OwnerlessStatementLockRequest &request : table_write_locks) {
        if (!acquire_fd_range_lock(
                lock_fd,
                request.start,
                request.length,
                request.lock_type,
                k_statement_lock_wait_timeout_ms
            )) {
            set_error(db, MYLITE_BUSY, "ownerless table write statement lock is busy");
            return MYLITE_BUSY;
        }
        lock.add(lock_fd, request.start, request.length);
    }
    return MYLITE_OK;
}

int ownerless_begin_dictionary_ddl(mylite_db &db, std::string_view sql, bool *out_ddl_started) {
    if (out_ddl_started == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_ddl_started = false;
    if (!db.ownerless_rw_open || !ownerless_dictionary_ddl_statement(sql)) {
        return MYLITE_OK;
    }

    void *dictionary_state = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
        owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
        owner_generation = g_runtime.concurrency_process_slot_generation;
    }
    if (dictionary_state == nullptr || owner_id == 0U || owner_generation == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless dictionary state is unavailable");
        return MYLITE_IOERR;
    }

    std::uint64_t generation = 0;
    const int begin_result = mylite_ownerless_dictionary_state_begin_ddl(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        owner_id,
        owner_generation,
        static_cast<std::uint64_t>(::getpid()),
        k_concurrency_lock_wait_timeout_ms,
        &generation
    );
    if (begin_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        set_error(db, MYLITE_BUSY, "ownerless dictionary change could not start");
        return ownerless_dictionary_result_from_state_result(begin_result);
    }

    db.ownerless_observed_dictionary_generation = generation;
    *out_ddl_started = true;
    pause_for_ownerless_test_fault("dictionary-after-begin");
    return MYLITE_OK;
}

int ownerless_finish_dictionary_ddl(mylite_db &db, bool ddl_started) {
    if (!ddl_started) {
        return MYLITE_OK;
    }

    pause_for_ownerless_test_fault("dictionary-before-finish");

    void *dictionary_state = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        dictionary_state = runtime_dictionary_state(g_runtime);
        owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
        owner_generation = g_runtime.concurrency_process_slot_generation;
    }
    if (dictionary_state == nullptr || owner_id == 0U || owner_generation == 0U) {
        return MYLITE_IOERR;
    }

    std::uint64_t generation = 0;
    const int finish_result = mylite_ownerless_dictionary_state_finish_ddl(
        dictionary_state,
        k_concurrency_dictionary_state_segment_size,
        owner_id,
        owner_generation,
        &generation
    );
    if (finish_result == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        db.ownerless_observed_dictionary_generation = generation;
        pause_for_ownerless_test_fault("dictionary-after-finish");
        return MYLITE_OK;
    }
    return ownerless_dictionary_result_from_state_result(finish_result);
}

int ownerless_dictionary_result_from_state_result(int state_result) {
    if (state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_OK) {
        return MYLITE_OK;
    }
    if (state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_BUSY ||
        state_result == MYLITE_OWNERLESS_DICTIONARY_STATE_TIMEOUT) {
        return MYLITE_BUSY;
    }
    return MYLITE_IOERR;
}

bool statement_allows_ownerless_page_version_reads(std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    const std::string_view first = identifier_token_at(tokens, 0);
    if (!token_in(first, "SELECT", "WITH")) {
        return false;
    }

    return !has_identifier_token(tokens, "UPDATE", 1) &&
           !has_identifier_token(tokens, "SHARE", 1) && !has_identifier_token(tokens, "LOCK", 1);
}

bool ownerless_connection_is_in_explicit_transaction(const mylite_db &db) {
    if (db.ownerless_explicit_transaction_active) {
        return true;
    }

    const bool server_in_transaction = (db.mysql.server_status & SERVER_STATUS_IN_TRANS) != 0U;
    const bool server_autocommit = (db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT) != 0U;
    return server_in_transaction && !server_autocommit;
}

bool ownerless_connection_allows_global_refresh(
    const mylite_db &db,
    bool allow_page_version_reads
) {
    return !ownerless_connection_is_in_explicit_transaction(db) ||
           (allow_page_version_reads && !db.ownerless_transaction_has_local_write_or_locking_read);
}

int update_ownerless_transaction_state_after_successful_sql(mylite_db &db, std::string_view sql) {
    const SqlPolicyTokens tokens = collect_sql_policy_tokens(sql);
    update_ownerless_transaction_isolation_after_successful_sql(db, tokens);
    const bool statement_writes = sql_statement_requires_write(tokens);
    const bool statement_uses_locking_read = sql_statement_uses_locking_read(tokens);

    if (sql_starts_explicit_transaction(tokens)) {
        const bool consistent_snapshot = sql_starts_consistent_snapshot_transaction(tokens);
        db.ownerless_active_transaction_isolation =
            db.ownerless_next_transaction_isolation_set
                ? db.ownerless_next_transaction_isolation
                : db.ownerless_session_transaction_isolation;
        db.ownerless_next_transaction_isolation_set = false;
        db.ownerless_explicit_transaction_active = true;
        db.ownerless_transaction_has_local_write_or_locking_read = false;
        db.ownerless_transaction_snapshot_visible_lsn =
            consistent_snapshot ? db.ownerless_observed_visible_lsn : 0U;
        db.ownerless_transaction_snapshot_visibility_pinned = consistent_snapshot;
        if (consistent_snapshot) {
            const int pin_result = ensure_ownerless_transaction_page_version_pin(
                db,
                db.ownerless_transaction_snapshot_visible_lsn
            );
            if (pin_result != MYLITE_OK) {
                static_cast<void>(rollback_active_transaction(db));
                return pin_result;
            }
        }
        return MYLITE_OK;
    }
    if (sql_ends_explicit_transaction(tokens)) {
        release_ownerless_transaction_page_version_pin(db);
        db.ownerless_explicit_transaction_active = sql_chains_transaction(tokens);
        db.ownerless_active_transaction_isolation = db.ownerless_session_transaction_isolation;
        db.ownerless_transaction_has_local_write_or_locking_read = false;
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
        return MYLITE_OK;
    }
    if (ownerless_connection_is_in_explicit_transaction(db) &&
        (statement_writes || statement_uses_locking_read)) {
        db.ownerless_transaction_has_local_write_or_locking_read = true;
    }
    if ((db.mysql.server_status & SERVER_STATUS_IN_TRANS) == 0U &&
        (db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT) != 0U) {
        release_ownerless_transaction_page_version_pin(db);
        db.ownerless_explicit_transaction_active = false;
        db.ownerless_transaction_has_local_write_or_locking_read = false;
        db.ownerless_transaction_snapshot_visible_lsn = 0;
        db.ownerless_transaction_snapshot_visibility_pinned = false;
        db.ownerless_active_transaction_isolation = db.ownerless_session_transaction_isolation;
    }
    return MYLITE_OK;
}

bool ownerless_transaction_pins_consistent_reads(const mylite_db &db) {
    return db.ownerless_active_transaction_isolation ==
               OwnerlessTransactionIsolation::RepeatableRead ||
           db.ownerless_active_transaction_isolation == OwnerlessTransactionIsolation::Serializable;
}

int ensure_ownerless_consistent_snapshot_start_pin(
    mylite_db &db,
    const SqlPolicyTokens &tokens,
    bool *out_pin_registered
) {
    if (out_pin_registered == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_pin_registered = false;
    if (!db.ownerless_rw_open || !sql_starts_consistent_snapshot_transaction(tokens) ||
        db.ownerless_transaction_snapshot_pin_registered) {
        return MYLITE_OK;
    }

    const int pin_result =
        ensure_ownerless_transaction_page_version_pin(db, db.ownerless_observed_visible_lsn);
    if (pin_result != MYLITE_OK) {
        return pin_result;
    }
    if (db.ownerless_transaction_snapshot_pin_registered) {
        *out_pin_registered = true;
        pause_for_ownerless_test_fault("consistent-snapshot-after-pin");
    }
    return MYLITE_OK;
}

int ensure_ownerless_transaction_page_version_pin(mylite_db &db, std::uint64_t read_lsn) {
    if (!db.ownerless_rw_open || read_lsn == 0U) {
        return MYLITE_OK;
    }
    if (db.ownerless_transaction_snapshot_pin_registered) {
        if (db.ownerless_transaction_snapshot_pin_lsn == read_lsn) {
            return MYLITE_OK;
        }
        release_ownerless_transaction_page_version_pin(db);
        if (db.ownerless_transaction_snapshot_pin_registered) {
            set_error(db, MYLITE_BUSY, "ownerless page-version snapshot pin is still active");
            return MYLITE_BUSY;
        }
    }

    void *page_pin_registry = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }
    if (page_pin_registry == nullptr || owner_id == 0U || owner_generation == 0U) {
        set_error(db, MYLITE_IOERR, "ownerless page-version snapshot registry is unavailable");
        return MYLITE_IOERR;
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    const int registry_result = mylite_ownerless_page_pin_registry_open(
        page_pin_registry,
        k_concurrency_page_pin_registry_segment_size,
        owner_id,
        owner_generation,
        read_lsn,
        &slot_index,
        &slot_generation
    );
    if (registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        db.ownerless_transaction_snapshot_pin_registered = true;
        db.ownerless_transaction_snapshot_pin_slot = slot_index;
        db.ownerless_transaction_snapshot_pin_generation = slot_generation;
        db.ownerless_transaction_snapshot_pin_lsn = read_lsn;
        return MYLITE_OK;
    }

    const int result = registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_FULL ||
                               registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_TIMEOUT
                           ? MYLITE_BUSY
                           : MYLITE_IOERR;
    set_error(
        db,
        result,
        result == MYLITE_BUSY ? "ownerless page-version snapshot registry is busy"
                              : "ownerless page-version snapshot registry failed"
    );
    return result;
}

void release_ownerless_transaction_page_version_pin(mylite_db &db) {
    if (!db.ownerless_transaction_snapshot_pin_registered) {
        return;
    }

    void *page_pin_registry = nullptr;
    std::uint32_t owner_id = 0;
    std::uint64_t owner_generation = 0;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        page_pin_registry = runtime_page_pin_registry(g_runtime);
        if (g_runtime.concurrency_process_slot_generation != 0U) {
            owner_id = ownerless_owner_id_from_slot_index(g_runtime.concurrency_process_slot_index);
            owner_generation = g_runtime.concurrency_process_slot_generation;
        }
    }

    int registry_result = MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND;
    if (page_pin_registry != nullptr && owner_id != 0U && owner_generation != 0U) {
        registry_result = mylite_ownerless_page_pin_registry_close(
            page_pin_registry,
            k_concurrency_page_pin_registry_segment_size,
            owner_id,
            owner_generation,
            db.ownerless_transaction_snapshot_pin_slot,
            db.ownerless_transaction_snapshot_pin_generation
        );
    }
    if (registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK ||
        registry_result == MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_NOT_FOUND) {
        db.ownerless_transaction_snapshot_pin_registered = false;
        db.ownerless_transaction_snapshot_pin_slot = 0;
        db.ownerless_transaction_snapshot_pin_generation = 0;
        db.ownerless_transaction_snapshot_pin_lsn = 0;
    }
}

void update_ownerless_transaction_isolation_after_successful_sql(
    mylite_db &db,
    const SqlPolicyTokens &tokens
) {
    OwnerlessTransactionIsolation isolation = OwnerlessTransactionIsolation::RepeatableRead;
    bool session_scope = false;
    if (!sql_sets_transaction_isolation(tokens, &isolation, &session_scope)) {
        return;
    }

    if (session_scope) {
        db.ownerless_session_transaction_isolation = isolation;
        if (!ownerless_connection_is_in_explicit_transaction(db)) {
            db.ownerless_active_transaction_isolation = isolation;
        }
        return;
    }

    db.ownerless_next_transaction_isolation = isolation;
    db.ownerless_next_transaction_isolation_set = true;
}

bool sql_sets_transaction_isolation(
    const SqlPolicyTokens &tokens,
    OwnerlessTransactionIsolation *out_isolation,
    bool *out_session_scope
) {
    if (out_isolation == nullptr || out_session_scope == nullptr ||
        !token_equals(identifier_token_at(tokens, 0), "SET")) {
        return false;
    }

    std::size_t index = 1;
    bool session_scope = false;
    const std::string_view scope = identifier_token_at(tokens, index);
    if (token_in(scope, "SESSION", "LOCAL")) {
        session_scope = true;
        ++index;
    } else if (token_equals(scope, "GLOBAL")) {
        return false;
    }

    if (!token_equals(identifier_token_at(tokens, index), "TRANSACTION") ||
        !token_equals(identifier_token_at(tokens, index + 1U), "ISOLATION") ||
        !token_equals(identifier_token_at(tokens, index + 2U), "LEVEL")) {
        return false;
    }

    const std::string_view first = identifier_token_at(tokens, index + 3U);
    const std::string_view second = identifier_token_at(tokens, index + 4U);
    if (token_equals(first, "READ") && token_equals(second, "UNCOMMITTED")) {
        *out_isolation = OwnerlessTransactionIsolation::ReadUncommitted;
    } else if (token_equals(first, "READ") && token_equals(second, "COMMITTED")) {
        *out_isolation = OwnerlessTransactionIsolation::ReadCommitted;
    } else if (token_equals(first, "REPEATABLE") && token_equals(second, "READ")) {
        *out_isolation = OwnerlessTransactionIsolation::RepeatableRead;
    } else if (token_equals(first, "SERIALIZABLE")) {
        *out_isolation = OwnerlessTransactionIsolation::Serializable;
    } else {
        return false;
    }

    *out_session_scope = session_scope;
    return true;
}

bool sql_starts_consistent_snapshot_transaction(const SqlPolicyTokens &tokens) {
    if (!sql_starts_explicit_transaction(tokens)) {
        return false;
    }

    for (std::size_t index = 0; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "CONSISTENT") &&
            token_equals(identifier_token_at(tokens, index + 1U), "SNAPSHOT")) {
            return true;
        }
    }
    return false;
}

bool sql_starts_explicit_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "START")) {
        return token_equals(second, "TRANSACTION");
    }
    if (!token_equals(first, "BEGIN")) {
        return false;
    }
    return tokens.count == 1U || token_equals(second, "WORK");
}

bool sql_ends_explicit_transaction(const SqlPolicyTokens &tokens) {
    const std::string_view first = identifier_token_at(tokens, 0);
    const std::string_view second = identifier_token_at(tokens, 1);

    if (token_equals(first, "COMMIT")) {
        return true;
    }
    return token_equals(first, "ROLLBACK") && !token_equals(second, "TO");
}

bool sql_chains_transaction(const SqlPolicyTokens &tokens) {
    for (std::size_t index = 1; index + 1U < tokens.count; ++index) {
        if (token_equals(identifier_token_at(tokens, index), "AND") &&
            token_equals(identifier_token_at(tokens, index + 1U), "CHAIN")) {
            return true;
        }
    }
    return false;
}

void unmap_concurrency_shared_memory_for_runtime(RuntimeState &runtime) {
    reset_ownerless_runtime_hooks(runtime);
    release_concurrency_owner_state(runtime);
    release_concurrency_process_slot(runtime);

    if (runtime.concurrency_shm_mapping != nullptr) {
        static_cast<void>(
            ::munmap(runtime.concurrency_shm_mapping, runtime.concurrency_shm_mapping_size)
        );
        runtime.concurrency_shm_mapping = nullptr;
        runtime.concurrency_shm_mapping_size = 0;
    }
    if (runtime.concurrency_shm_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_shm_fd));
        runtime.concurrency_shm_fd = -1;
    }
    if (runtime.concurrency_wal_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_wal_fd));
        runtime.concurrency_wal_fd = -1;
    }
    if (runtime.concurrency_checkpoint_fd >= 0) {
        static_cast<void>(::close(runtime.concurrency_checkpoint_fd));
        runtime.concurrency_checkpoint_fd = -1;
    }
    if (runtime.ownerless_statement_lock_fd >= 0) {
        static_cast<void>(::close(runtime.ownerless_statement_lock_fd));
        runtime.ownerless_statement_lock_fd = -1;
    }
}

void reset_ownerless_runtime_hooks(RuntimeState &runtime) {
    mylite_ownerless_innodb_clear_external_page_visibility();
    mylite_ownerless_runtime_reset_hooks();
    mylite_ownerless_innodb_lock_reset_hooks();
    mylite_ownerless_read_view_reset_hooks();
    mylite_ownerless_trx_reset_hooks();
    mylite_ownerless_mdl_reset_hooks();
    runtime.ownerless_innodb_lock_hook = {};
    runtime.ownerless_read_view_hook = {};
    runtime.ownerless_trx_hook = {};
    runtime.ownerless_mdl_hook = {};
}

void release_concurrency_owner_state(RuntimeState &runtime) {
    if (runtime.concurrency_process_slot_generation == 0U) {
        return;
    }

    OwnerlessProcessCleanupContext cleanup_context = {};
    if (runtime.concurrency_shm_mapping != nullptr) {
        auto *mapping = static_cast<unsigned char *>(runtime.concurrency_shm_mapping);
        cleanup_context.lock_table = mapping + k_concurrency_mdl_lock_table_offset;
    }
    cleanup_context.lock_table_size = k_concurrency_mdl_lock_table_segment_size;
    cleanup_context.trx_registry = runtime_trx_registry(runtime);
    cleanup_context.trx_registry_size = k_concurrency_trx_registry_segment_size;
    cleanup_context.read_view_registry = runtime_read_view_registry(runtime);
    cleanup_context.read_view_registry_size = k_concurrency_read_view_registry_segment_size;
    cleanup_context.page_pin_registry = runtime_page_pin_registry(runtime);
    cleanup_context.page_pin_registry_size = k_concurrency_page_pin_registry_segment_size;
    cleanup_context.innodb_lock_registry = runtime_innodb_lock_registry(runtime);
    cleanup_context.innodb_lock_registry_size = k_concurrency_innodb_lock_registry_segment_size;
    cleanup_context.page_write_lock_registry = runtime_page_write_lock_registry(runtime);
    cleanup_context.page_write_lock_registry_size =
        k_concurrency_page_write_lock_registry_segment_size;
    cleanup_context.redo_state = runtime_redo_state(runtime);
    cleanup_context.redo_state_size = k_concurrency_redo_state_segment_size;
    cleanup_context.dictionary_state = runtime_dictionary_state(runtime);
    cleanup_context.dictionary_state_size = k_concurrency_dictionary_state_segment_size;
    cleanup_context.latch_owner_id =
        ownerless_owner_id_from_slot_index(runtime.concurrency_process_slot_index);
    cleanup_context.latch_owner_generation = runtime.concurrency_process_slot_generation;
    static_cast<void>(ownerless_process_cleanup_owner_state(
        runtime.concurrency_process_slot_index,
        runtime.concurrency_process_slot_generation,
        static_cast<std::uint64_t>(::getpid()),
        &cleanup_context
    ));
}

void release_concurrency_process_slot(RuntimeState &runtime) {
    if (runtime.concurrency_process_slot_generation == 0U) {
        return;
    }

    unsigned char *registry = runtime_process_registry(runtime);
    if (registry != nullptr) {
        const int registry_result = mylite_ownerless_process_registry_release(
            registry,
            k_concurrency_process_registry_size,
            runtime.concurrency_process_slot_index,
            runtime.concurrency_process_slot_generation
        );
        if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK &&
            runtime.concurrency_shm_fd >= 0 &&
            mylite_ownerless_process_registry_active_count(registry) == 0U) {
            static_cast<void>(update_concurrency_shm_state(
                runtime.concurrency_shm_fd,
                k_concurrency_shm_state_clean
            ));
        }
    }

    runtime.concurrency_process_slot_index = 0;
    runtime.concurrency_process_slot_generation = 0;
}

int ownerless_mdl_acquire_hook(
    const mylite_ownerless_mdl_key_view *key,
    double lock_wait_timeout,
    void *ctx
) {
    if (key == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_MDL_ERROR;
    }

    auto *hook = static_cast<OwnerlessMdlHookContext *>(ctx);
    if (hook->lock_table == nullptr || hook->lock_table_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_MDL_ERROR;
    }

    if (key->ownerless_mode != MYLITE_OWNERLESS_MDL_MODE_NONE) {
        return ownerless_mdl_result_from_lock_table_result(mylite_ownerless_mdl_acquire_mode(
            hook->lock_table,
            hook->lock_table_size,
            hook->owner_id,
            hook->owner_generation,
            key->namespace_id,
            key->database_name,
            key->object_name,
            key->ownerless_mode,
            ownerless_mdl_timeout_ms(lock_wait_timeout)
        ));
    }
    return MYLITE_OWNERLESS_MDL_OK;
}

void ownerless_mdl_release_hook(const mylite_ownerless_mdl_key_view *key, void *ctx) {
    if (key == nullptr || ctx == nullptr) {
        return;
    }

    auto *hook = static_cast<OwnerlessMdlHookContext *>(ctx);
    if (hook->lock_table == nullptr || hook->lock_table_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return;
    }

    if (key->ownerless_mode != MYLITE_OWNERLESS_MDL_MODE_NONE) {
        static_cast<void>(mylite_ownerless_mdl_release_mode(
            hook->lock_table,
            hook->lock_table_size,
            hook->owner_id,
            hook->owner_generation,
            key->namespace_id,
            key->database_name,
            key->object_name,
            key->ownerless_mode
        ));
    }
}

unsigned ownerless_mdl_timeout_ms(double lock_wait_timeout) {
    if (lock_wait_timeout <= 0.0) {
        return 0U;
    }

    constexpr double k_milliseconds_per_second = 1000.0;
    constexpr double k_max_timeout_ms = static_cast<double>(std::numeric_limits<unsigned>::max());
    const double timeout_ms = lock_wait_timeout * k_milliseconds_per_second;
    if (timeout_ms >= k_max_timeout_ms) {
        return std::numeric_limits<unsigned>::max();
    }
    return static_cast<unsigned>(std::max(timeout_ms, 1.0));
}

int ownerless_mdl_result_from_lock_table_result(int lock_table_result) {
    if (lock_table_result == MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return MYLITE_OWNERLESS_MDL_OK;
    }
    if (lock_table_result == MYLITE_OWNERLESS_LOCK_TABLE_TIMEOUT) {
        return MYLITE_OWNERLESS_MDL_TIMEOUT;
    }
    return MYLITE_OWNERLESS_MDL_ERROR;
}

int ownerless_trx_allocate_hook(std::uint64_t *out_trx_id, void *ctx) {
    if (out_trx_id == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_result_from_registry_result(mylite_ownerless_trx_registry_allocate_id(
        hook->trx_registry,
        hook->trx_registry_size,
        hook->owner_id,
        hook->owner_generation,
        out_trx_id
    ));
}

int ownerless_trx_register_hook(std::uint64_t *out_trx_id, void *ctx) {
    if (out_trx_id == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    std::uint32_t slot_index = 0;
    std::uint64_t slot_generation = 0;
    const int result =
        ownerless_trx_result_from_registry_result(mylite_ownerless_trx_registry_begin(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id,
            &slot_index,
            &slot_generation
        ));
    if (result == MYLITE_OWNERLESS_TRX_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("trx-after-register");
#  endif
    }
    return result;
}

int ownerless_trx_assign_no_hook(std::uint64_t trx_id, std::uint64_t *out_trx_no, void *ctx) {
    if (out_trx_no == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    int registry_result = mylite_ownerless_trx_registry_assign_new_no(
        hook->trx_registry,
        hook->trx_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        out_trx_no
    );
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND) {
        registry_result = mylite_ownerless_trx_registry_allocate_id(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            out_trx_no
        );
    }
    return ownerless_trx_result_from_registry_result(registry_result);
}

int ownerless_trx_deregister_hook(std::uint64_t trx_id, void *ctx) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_deregister_result_from_registry_result(
        mylite_ownerless_trx_registry_end_by_id(
            hook->trx_registry,
            hook->trx_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id
        )
    );
}

int ownerless_trx_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_next_trx_id,
    std::uint64_t *out_min_trx_no,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    auto *hook = static_cast<OwnerlessTrxHookContext *>(ctx);
    if (hook->trx_registry == nullptr || hook->trx_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_TRX_ERROR;
    }

    return ownerless_trx_result_from_registry_result(
        mylite_ownerless_trx_registry_snapshot_read_view(
            hook->trx_registry,
            hook->trx_registry_size,
            out_trx_ids,
            trx_id_capacity,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id_count,
            out_next_trx_id,
            out_min_trx_no
        )
    );
}

int ownerless_trx_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_OWNERLESS_TRX_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_TRX_FULL;
    }
    return MYLITE_OWNERLESS_TRX_ERROR;
}

int ownerless_trx_deregister_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_TRX_REGISTRY_NOT_FOUND) {
        return MYLITE_OWNERLESS_TRX_OK;
    }
    return ownerless_trx_result_from_registry_result(registry_result);
}

int ownerless_read_view_register_hook(
    std::uint64_t low_limit_id,
    std::uint64_t low_limit_no,
    const std::uint64_t *trx_ids,
    unsigned int trx_id_count,
    std::uint32_t *out_slot_index,
    std::uint64_t *out_slot_generation,
    void *ctx
) {
    if (out_slot_index == nullptr || out_slot_generation == nullptr || ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(mylite_ownerless_read_view_registry_open(
        hook->read_view_registry,
        hook->read_view_registry_size,
        hook->owner_id,
        hook->owner_generation,
        low_limit_id,
        low_limit_no,
        trx_ids,
        trx_id_count,
        out_slot_index,
        out_slot_generation
    ));
}

int ownerless_read_view_deregister_hook(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(
        mylite_ownerless_read_view_registry_close(
            hook->read_view_registry,
            hook->read_view_registry_size,
            hook->owner_id,
            hook->owner_generation,
            slot_index,
            slot_generation
        )
    );
}

int ownerless_read_view_snapshot_hook(
    std::uint64_t *out_trx_ids,
    unsigned int trx_id_capacity,
    unsigned int *out_trx_id_count,
    std::uint64_t *out_low_limit_id,
    std::uint64_t *out_low_limit_no,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    auto *hook = static_cast<OwnerlessReadViewHookContext *>(ctx);
    if (hook->read_view_registry == nullptr || hook->read_view_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_READ_VIEW_ERROR;
    }

    return ownerless_read_view_result_from_registry_result(
        mylite_ownerless_read_view_registry_snapshot_oldest(
            hook->read_view_registry,
            hook->read_view_registry_size,
            out_trx_ids,
            trx_id_capacity,
            hook->owner_id,
            hook->owner_generation,
            out_trx_id_count,
            out_low_limit_id,
            out_low_limit_no
        )
    );
}

int ownerless_read_view_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK) {
        return MYLITE_OWNERLESS_READ_VIEW_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_READ_VIEW_FULL;
    }
    return MYLITE_OWNERLESS_READ_VIEW_ERROR;
}

int ownerless_innodb_lock_acquire_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_reserve_table(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            table_id,
            mode,
            timeout_ms
        )
    );
}

int ownerless_innodb_lock_release_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_release_table(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            table_id,
            mode
        )
    );
}

int ownerless_innodb_lock_wait_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    std::uint64_t blocker_trx_id,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("table-lock-wait");
#  endif
    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_wait_for_table(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            table_id,
            mode,
            hook->owner_id,
            blocker_trx_id
        )
    );
}

int ownerless_innodb_lock_wait_until_table_hook(
    std::uint64_t trx_id,
    std::uint64_t table_id,
    std::uint32_t mode,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_wait_until_table_available(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            table_id,
            mode,
            timeout_ms
        )
    );
}

int ownerless_innodb_lock_acquire_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result = mylite_ownerless_innodb_lock_registry_reserve_record(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        normalized_heap_no,
        mode,
        normalized_flags,
        timeout_ms
    );
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("record-lock-after-acquire");
#  endif
    }
    return result;
}

int ownerless_innodb_lock_release_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result = mylite_ownerless_innodb_lock_registry_release_record(
        hook->lock_registry,
        hook->lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        normalized_heap_no,
        mode,
        normalized_flags
    );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_acquire_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    std::uint32_t *out_acquire_flags,
    void *ctx
) {
    if (out_acquire_flags != nullptr) {
        *out_acquire_flags = 0U;
    }
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    int registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
        hook->page_write_lock_registry,
        hook->page_write_lock_registry_size,
        hook->owner_id,
        hook->owner_generation,
        trx_id,
        index_id,
        space_id,
        page_no,
        heap_no,
        mode,
        flags,
        0U,
        nullptr
    );
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT &&
        hook->lock_registry != nullptr && hook->lock_registry_size != 0U) {
        registry_result =
            mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
                hook->page_write_lock_registry,
                hook->page_write_lock_registry_size,
                hook->lock_registry,
                hook->lock_registry_size,
                hook->owner_id,
                hook->owner_generation,
                trx_id,
                index_id,
                space_id,
                page_no,
                heap_no,
                mode,
                flags,
                timeout_ms
            );
        if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
            registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
                hook->page_write_lock_registry,
                hook->page_write_lock_registry_size,
                hook->owner_id,
                hook->owner_generation,
                trx_id,
                index_id,
                space_id,
                page_no,
                heap_no,
                mode,
                flags,
                0U,
                nullptr
            );
            if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
                out_acquire_flags != nullptr) {
                *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED;
            }
        }
    } else if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT) {
        std::uint32_t registry_acquire_flags = 0U;
        registry_result = mylite_ownerless_innodb_lock_registry_acquire_record_with_flags(
            hook->page_write_lock_registry,
            hook->page_write_lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            index_id,
            space_id,
            page_no,
            heap_no,
            mode,
            flags,
            timeout_ms,
            &registry_acquire_flags
        );
        if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK &&
            out_acquire_flags != nullptr &&
            (registry_acquire_flags & MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_ACQUIRE_WAITED) != 0U) {
            *out_acquire_flags |= MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED;
        }
    }
    const int result = ownerless_innodb_lock_result_from_registry_result(registry_result);
    return result;
}

int ownerless_innodb_lock_release_page_write_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_release_record(
            hook->page_write_lock_registry,
            hook->page_write_lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            index_id,
            space_id,
            page_no,
            heap_no,
            mode,
            flags
        )
    );
}

int ownerless_innodb_lock_release_page_writes_hook(std::uint64_t trx_id, void *ctx) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_write_lock_registry == nullptr || hook->page_write_lock_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t released_locks = 0;
    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_release_transaction_records(
            hook->page_write_lock_registry,
            hook->page_write_lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
            MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
            MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
            0U,
            &released_locks
        )
    );
}

int ownerless_innodb_lock_wait_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    std::uint64_t blocker_trx_id,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_wait_for_record(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            index_id,
            space_id,
            page_no,
            normalized_heap_no,
            mode,
            normalized_flags,
            hook->owner_id,
            blocker_trx_id
        )
    );
}

int ownerless_innodb_lock_wait_until_record_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    unsigned int timeout_ms,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    const int registry_result =
        hook->page_write_lock_registry != nullptr && hook->page_write_lock_registry_size != 0U
            ? mylite_ownerless_innodb_lock_registry_wait_until_record_available_with_cycle_registry(
                  hook->lock_registry,
                  hook->lock_registry_size,
                  hook->page_write_lock_registry,
                  hook->page_write_lock_registry_size,
                  hook->owner_id,
                  hook->owner_generation,
                  trx_id,
                  index_id,
                  space_id,
                  page_no,
                  normalized_heap_no,
                  mode,
                  normalized_flags,
                  timeout_ms
              )
            : mylite_ownerless_innodb_lock_registry_wait_until_record_available(
                  hook->lock_registry,
                  hook->lock_registry_size,
                  hook->owner_id,
                  hook->owner_generation,
                  trx_id,
                  index_id,
                  space_id,
                  page_no,
                  normalized_heap_no,
                  mode,
                  normalized_flags,
                  timeout_ms
              );
    return ownerless_innodb_lock_result_from_registry_result(registry_result);
}

int ownerless_innodb_lock_before_record_wait_hook(
    std::uint64_t trx_id,
    std::uint64_t index_id,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint32_t heap_no,
    std::uint32_t mode,
    std::uint32_t flags,
    void *ctx
) {
    if (ctx == nullptr || trx_id == 0U || index_id == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t normalized_heap_no = heap_no;
    std::uint32_t normalized_flags = flags;
    normalize_ownerless_record_lock_resource(mode, &normalized_heap_no, &normalized_flags);
    (void)space_id;
    (void)page_no;
    (void)normalized_heap_no;
    (void)normalized_flags;

#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("record-lock-before-grant");
#  endif
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

void normalize_ownerless_record_lock_resource(
    std::uint32_t mode,
    std::uint32_t *heap_no,
    std::uint32_t *flags
) {
    if (heap_no == nullptr || flags == nullptr ||
        !ownerless_record_lock_uses_page_resource(mode, *flags)) {
        return;
    }

    *heap_no = k_ownerless_innodb_record_page_heap_no;
    *flags = 0U;
}

bool ownerless_record_lock_uses_page_resource(std::uint32_t mode, std::uint32_t flags) {
    if (mode != MYLITE_OWNERLESS_INNODB_LOCK_MODE_X) {
        return false;
    }
    return flags == 0U;
}

int ownerless_innodb_lock_clear_wait_hook(std::uint64_t trx_id, void *ctx) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->lock_registry == nullptr || hook->lock_registry_size == 0U || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint32_t cleared_waits = 0;
    return ownerless_innodb_lock_result_from_registry_result(
        mylite_ownerless_innodb_lock_registry_clear_wait(
            hook->lock_registry,
            hook->lock_registry_size,
            hook->owner_id,
            hook->owner_generation,
            trx_id,
            &cleared_waits
        )
    );
}

int ownerless_innodb_autoinc_read_hook(
    std::uint64_t table_id,
    std::uint64_t seed_next_value,
    std::uint64_t *out_next_value,
    void *ctx
) {
    if (ctx == nullptr || out_next_value == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->autoinc_registry == nullptr || hook->autoinc_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_autoinc_registry_read_or_seed(
        hook->autoinc_registry,
        hook->autoinc_registry_size,
        hook->owner_id,
        hook->owner_generation,
        table_id,
        seed_next_value,
        out_next_value
    );
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_autoinc_publish_hook(
    std::uint64_t table_id,
    std::uint64_t next_value,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->autoinc_registry == nullptr || hook->autoinc_registry_size == 0U ||
        hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_autoinc_registry_publish(
        hook->autoinc_registry,
        hook->autoinc_registry_size,
        hook->owner_id,
        hook->owner_generation,
        table_id,
        next_value
    );
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_AUTOINC_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_enter_hook(std::uint64_t *out_latest_lsn, void *ctx) {
    if (ctx == nullptr || out_latest_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_enter(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        30000U,
        out_latest_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_observe_hook(std::uint64_t *out_latest_lsn, void *ctx) {
    if (ctx == nullptr || out_latest_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    mylite_ownerless_redo_state_snapshot snapshot = {};
    if (mylite_ownerless_redo_state_read_snapshot(
            hook->redo_state,
            hook->redo_state_size,
            &snapshot
        ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    *out_latest_lsn = snapshot.latest_lsn;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

int ownerless_innodb_redo_reserve_hook(
    std::uint64_t current_lsn,
    std::uint64_t length,
    std::uint64_t *out_start_lsn,
    std::uint64_t *out_end_lsn,
    void *ctx
) {
    if (ctx == nullptr || length == 0U || out_start_lsn == nullptr || out_end_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_reserve(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        current_lsn,
        length,
        out_start_lsn,
        out_end_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
        pause_for_ownerless_test_fault("redo-after-reserve");
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_redo_written_hook(
    std::uint64_t start_lsn,
    std::uint64_t end_lsn,
    std::uint64_t *out_written_lsn,
    void *ctx
) {
    if (ctx == nullptr || start_lsn == 0U || end_lsn <= start_lsn) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    const int result = mylite_ownerless_redo_state_complete_write(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        start_lsn,
        end_lsn,
        out_written_lsn
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-after-written");
#  endif
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_REDO_STATE_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

void ownerless_innodb_redo_leave_hook(std::uint64_t latest_lsn, void *ctx) {
    if (ctx == nullptr) {
        return;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size || hook->owner_id == 0U ||
        hook->owner_generation == 0U) {
        return;
    }

    std::uint64_t advanced_latest_lsn = 0U;
    const int result = mylite_ownerless_redo_state_leave(
        hook->redo_state,
        hook->redo_state_size,
        hook->owner_id,
        hook->owner_generation,
        latest_lsn,
        &advanced_latest_lsn,
        nullptr
    );
    if (result == MYLITE_OWNERLESS_REDO_STATE_OK && advanced_latest_lsn != 0U) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-before-checkpoint");
#  endif
        ownerless_persist_redo_checkpoint(hook, advanced_latest_lsn, 0U, false);
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
        pause_for_ownerless_test_fault("redo-after-checkpoint");
#  endif
    }
}

void ownerless_innodb_pages_visible_hook(std::uint64_t visible_lsn, void *ctx) {
    if (ctx == nullptr || visible_lsn == 0U) {
        return;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->redo_state == nullptr ||
        hook->redo_state_size < k_concurrency_redo_state_segment_size) {
        return;
    }
    if (hook->page_log_fd < 0 || hook->page_log_offset == 0U ||
        mylite_ownerless_page_log_sync_at(hook->page_log_fd, hook->page_log_offset) !=
            MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }

    std::uint64_t latest_lsn = 0U;
    std::uint64_t published_visible_lsn = 0U;
    const int publish_result = mylite_ownerless_redo_state_publish_visible(
        hook->redo_state,
        hook->redo_state_size,
        visible_lsn,
        &latest_lsn,
        &published_visible_lsn
    );
    if (publish_result != MYLITE_OWNERLESS_REDO_STATE_OK) {
        return;
    }
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("pages-visible-before-checkpoint");
#  endif
    ownerless_persist_redo_checkpoint(hook, latest_lsn, published_visible_lsn, true);
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    pause_for_ownerless_test_fault("pages-visible-after-checkpoint");
#  endif
}

void ownerless_persist_redo_checkpoint(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    if (hook == nullptr || hook->checkpoint_fd < 0) {
        return;
    }
    static_cast<void>(
        update_concurrency_checkpoint_lsn(hook->checkpoint_fd, latest_lsn, visible_lsn, durable)
    );
}

void publish_ownerless_snapshot_boundary_if_needed(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t visible_lsn,
    std::uint32_t page_size
) {
    if (hook == nullptr || hook->page_pin_registry == nullptr ||
        hook->page_pin_registry_size == 0U || hook->page_index == nullptr ||
        hook->page_index_size == 0U || hook->page_log_fd < 0 || hook->page_log_offset == 0U ||
        hook->database_path == nullptr || hook->owner_id == 0U || hook->owner_generation == 0U ||
        visible_lsn == 0U || page_size == 0U || page_size > k_innodb_page_size_max) {
        return;
    }

    std::uint32_t active_pin_count = 0;
    std::uint64_t oldest_pin_lsn = 0;
    const int pin_result = mylite_ownerless_page_pin_registry_snapshot_oldest(
        hook->page_pin_registry,
        hook->page_pin_registry_size,
        hook->owner_id,
        hook->owner_generation,
        &active_pin_count,
        &oldest_pin_lsn
    );
    if (pin_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK || active_pin_count == 0U ||
        oldest_pin_lsn == 0U || oldest_pin_lsn >= visible_lsn) {
        return;
    }

    std::unique_ptr<unsigned char[]> page(new (std::nothrow) unsigned char[page_size]);
    if (page == nullptr) {
        return;
    }

    std::uint32_t existing_page_size = 0;
    std::uint64_t existing_page_lsn = 0;
    std::uint64_t existing_commit_lsn = 0;
    const int existing_result = mylite_ownerless_page_log_find_latest_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        oldest_pin_lsn,
        page.get(),
        page_size,
        &existing_page_size,
        &existing_page_lsn,
        &existing_commit_lsn
    );
    if (existing_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }
    if (existing_result != MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND) {
        return;
    }

    const std::filesystem::path datadir =
        std::filesystem::path(hook->database_path) / k_datadir_name;
    const std::string datadir_name = datadir.string();
    std::uint32_t boundary_page_size = 0;
    std::uint64_t boundary_page_lsn = 0;
    const int read_result = mylite_ownerless_tablespace_read_page_at_or_before(
        datadir_name.c_str(),
        space_id,
        page_no,
        page_size,
        oldest_pin_lsn,
        page.get(),
        page_size,
        &boundary_page_size,
        &boundary_page_lsn
    );
    if (read_result != MYLITE_OWNERLESS_TABLESPACE_REPLAY_OK || boundary_page_size == 0U ||
        boundary_page_lsn == 0U || boundary_page_lsn > oldest_pin_lsn) {
        return;
    }

    std::uint64_t record_offset = 0;
    const int append_result = mylite_ownerless_page_log_append_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        boundary_page_lsn,
        oldest_pin_lsn,
        page.get(),
        boundary_page_size,
        &record_offset
    );
    if (append_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return;
    }

    const int publish_result = mylite_ownerless_page_index_publish(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        oldest_pin_lsn,
        boundary_page_lsn,
        record_offset
    );
    if (publish_result != MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        static_cast<void>(mylite_ownerless_page_index_require_wal_scan(
            hook->page_index,
            hook->page_index_size,
            hook->owner_id,
            hook->owner_generation
        ));
    }
}

int ownerless_innodb_page_publish_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t page_lsn,
    std::uint64_t visible_lsn,
    const void *page,
    std::uint32_t page_size,
    void *ctx
) {
    if (ctx == nullptr || page == nullptr || page_size == 0U || visible_lsn == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_index == nullptr || hook->page_index_size == 0U || hook->page_log_fd < 0 ||
        hook->page_log_offset == 0U || hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    std::uint64_t record_offset = 0;
    publish_ownerless_snapshot_boundary_if_needed(hook, space_id, page_no, visible_lsn, page_size);
    pause_for_ownerless_test_fault("page-publish-before-append");

    const int append_result = mylite_ownerless_page_log_append_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        page_lsn,
        visible_lsn,
        page,
        page_size,
        &record_offset
    );
    if (append_result != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    pause_for_ownerless_test_fault("page-publish-after-append");

    return ownerless_innodb_lock_result_from_page_index_result(mylite_ownerless_page_index_publish(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        visible_lsn,
        page_lsn,
        record_offset
    ));
}

int ownerless_innodb_page_read_hook(
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn,
    void *ctx
) {
    if (ctx == nullptr || page == nullptr || page_capacity == 0U || max_commit_lsn == 0U ||
        out_page_size == nullptr || out_page_lsn == nullptr || out_commit_lsn == nullptr) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }

    auto *hook = static_cast<OwnerlessInnoDBLockHookContext *>(ctx);
    if (hook->page_index == nullptr || hook->page_index_size == 0U || hook->page_log_fd < 0 ||
        hook->page_log_offset == 0U || hook->owner_id == 0U || hook->owner_generation == 0U) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (!hook->page_log_reads_enabled) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }

    if (mylite_ownerless_page_log_begin_read(hook->page_log_fd) != MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    const int result = ownerless_innodb_page_read_locked(
        hook,
        space_id,
        page_no,
        max_commit_lsn,
        page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
    mylite_ownerless_page_log_end_read(hook->page_log_fd);
    return result;
}

int ownerless_innodb_page_read_locked(
    OwnerlessInnoDBLockHookContext *hook,
    std::uint32_t space_id,
    std::uint32_t page_no,
    std::uint64_t max_commit_lsn,
    void *page,
    std::uint32_t page_capacity,
    std::uint32_t *out_page_size,
    std::uint64_t *out_page_lsn,
    std::uint64_t *out_commit_lsn
) {
    std::uint64_t record_offset = 0;
    std::uint64_t index_page_lsn = 0;
    std::uint64_t index_commit_lsn = 0;
    const int index_result = mylite_ownerless_page_index_find(
        hook->page_index,
        hook->page_index_size,
        hook->owner_id,
        hook->owner_generation,
        space_id,
        page_no,
        max_commit_lsn,
        &record_offset,
        &index_page_lsn,
        &index_commit_lsn
    );
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        const int read_result = mylite_ownerless_page_log_read_page_at(
            hook->page_log_fd,
            hook->page_log_offset,
            record_offset,
            space_id,
            page_no,
            page,
            page_capacity,
            out_page_size,
            out_page_lsn,
            out_commit_lsn
        );
        if (read_result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
            if (*out_page_lsn == index_page_lsn && *out_commit_lsn == index_commit_lsn) {
                return MYLITE_OWNERLESS_INNODB_LOCK_OK;
            }
        } else if (read_result == MYLITE_OWNERLESS_PAGE_LOG_FULL) {
            return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
        }
    } else if (index_result != MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND) {
        return ownerless_innodb_lock_result_from_page_index_result(index_result);
    }

    // The page index is rebuildable; the WAL scan is authoritative if an indexed
    // offset is stale after checkpoint movement.
    const int result = mylite_ownerless_page_log_find_latest_at(
        hook->page_log_fd,
        hook->page_log_offset,
        space_id,
        page_no,
        max_commit_lsn,
        page,
        page_capacity,
        out_page_size,
        out_page_lsn,
        out_commit_lsn
    );
    if (result == MYLITE_OWNERLESS_PAGE_LOG_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (result == MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_lock_result_from_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_NOT_FOUND) {
        return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_TIMEOUT) {
        return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    }
    if (registry_result == MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_DEADLOCK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_innodb_lock_result_from_page_index_result(int index_result) {
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_OK) {
        return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }
    if (index_result == MYLITE_OWNERLESS_PAGE_INDEX_FULL) {
        return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

int ownerless_runtime_may_delete_shared_file_hook(void *ctx) {
    if (ctx == nullptr) {
        return 1;
    }

    auto *runtime = static_cast<RuntimeState *>(ctx);
    unsigned char *registry = runtime_process_registry(*runtime);
    if (registry == nullptr) {
        return 1;
    }

    return mylite_ownerless_process_registry_active_count(registry) <= 1U ? 1 : 0;
}

unsigned char *runtime_process_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_process_registry_offset + k_concurrency_process_registry_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_process_registry_offset;
}

unsigned char *runtime_trx_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_trx_registry_offset + k_concurrency_trx_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_trx_registry_offset;
}

unsigned char *runtime_read_view_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size < k_concurrency_read_view_registry_offset +
                                                   k_concurrency_read_view_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_read_view_registry_offset;
}

unsigned char *runtime_page_pin_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_pin_registry_offset + k_concurrency_page_pin_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_pin_registry_offset;
}

unsigned char *runtime_innodb_lock_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_innodb_lock_registry_offset +
                k_concurrency_innodb_lock_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_innodb_lock_registry_offset;
}

unsigned char *runtime_autoinc_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_autoinc_registry_offset + k_concurrency_autoinc_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_autoinc_registry_offset;
}

unsigned char *runtime_page_write_lock_registry(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_write_lock_registry_offset +
                k_concurrency_page_write_lock_registry_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_write_lock_registry_offset;
}

unsigned char *runtime_redo_state(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_redo_state_offset + k_concurrency_redo_state_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_redo_state_offset;
}

unsigned char *runtime_page_index(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_page_index_offset + k_concurrency_page_index_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_page_index_offset;
}

unsigned char *runtime_dictionary_state(RuntimeState &runtime) {
    if (runtime.concurrency_shm_mapping == nullptr ||
        runtime.concurrency_shm_mapping_size <
            k_concurrency_dictionary_state_offset + k_concurrency_dictionary_state_segment_size) {
        return nullptr;
    }
    return static_cast<unsigned char *>(runtime.concurrency_shm_mapping) +
           k_concurrency_dictionary_state_offset;
}

std::uint32_t ownerless_owner_id_from_slot_index(std::uint32_t slot_index) {
    return slot_index + 1U;
}

int ownerless_process_is_alive(std::uint64_t pid, void *ctx) {
    (void)ctx;
    if (pid == 0U || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        return 0;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return 1;
    }
    return errno == EPERM ? 1 : 0;
}

int ownerless_process_cleanup_dead_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
) {
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    auto *cleanup = static_cast<OwnerlessProcessCleanupContext *>(ctx);
    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    const int page_write_release_result =
        ownerless_process_release_owner_page_write_locks(*cleanup, owner_id);
    if (page_write_release_result != MYLITE_OWNERLESS_PROCESS_CLEANUP_OK) {
        return page_write_release_result;
    }
    if (ownerless_process_owner_state_requires_recovery(*cleanup, owner_id)) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_BLOCKED;
    }
    return ownerless_process_cleanup_owner_state(slot_index, slot_generation, pid, ctx);
}

int ownerless_process_cleanup_owner_state(
    std::uint32_t slot_index,
    std::uint64_t slot_generation,
    std::uint64_t pid,
    void *ctx
) {
    (void)pid;
    if (ctx == nullptr) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    auto *cleanup = static_cast<OwnerlessProcessCleanupContext *>(ctx);
    if (cleanup->lock_table == nullptr || cleanup->lock_table_size == 0U ||
        cleanup->trx_registry == nullptr || cleanup->trx_registry_size == 0U ||
        cleanup->read_view_registry == nullptr || cleanup->read_view_registry_size == 0U ||
        cleanup->page_pin_registry == nullptr || cleanup->page_pin_registry_size == 0U ||
        cleanup->page_write_lock_registry == nullptr ||
        cleanup->page_write_lock_registry_size == 0U) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    const std::uint32_t owner_id = ownerless_owner_id_from_slot_index(slot_index);
    std::uint32_t released_entries = 0;
    const int release_result = mylite_ownerless_lock_table_release_owner(
        cleanup->lock_table,
        cleanup->lock_table_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_entries
    );
    if (release_result != MYLITE_OWNERLESS_LOCK_TABLE_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_innodb_locks = 0;
    if (cleanup->innodb_lock_registry == nullptr || cleanup->innodb_lock_registry_size == 0U ||
        mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup->innodb_lock_registry,
            cleanup->innodb_lock_registry_size,
            owner_id,
            cleanup->latch_owner_id,
            cleanup->latch_owner_generation,
            &released_innodb_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_page_write_locks = 0;
    if (mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup->page_write_lock_registry,
            cleanup->page_write_lock_registry_size,
            owner_id,
            cleanup->latch_owner_id,
            cleanup->latch_owner_generation,
            &released_page_write_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_transactions = 0;
    const int trx_release_result = mylite_ownerless_trx_registry_release_owner(
        cleanup->trx_registry,
        cleanup->trx_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_transactions
    );
    if (trx_release_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_pins = 0;
    const int page_pin_release_result = mylite_ownerless_page_pin_registry_release_owner(
        cleanup->page_pin_registry,
        cleanup->page_pin_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_pins
    );
    if (page_pin_release_result != MYLITE_OWNERLESS_PAGE_PIN_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    if (cleanup->redo_state != nullptr &&
        cleanup->redo_state_size >= k_concurrency_redo_state_segment_size) {
        std::uint32_t released_redo = 0U;
        if (mylite_ownerless_redo_state_cleanup_owner(
                cleanup->redo_state,
                cleanup->redo_state_size,
                owner_id,
                slot_generation,
                &released_redo
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
        }
    }

    std::uint32_t released_views = 0;
    const int read_view_release_result = mylite_ownerless_read_view_registry_release_owner(
        cleanup->read_view_registry,
        cleanup->read_view_registry_size,
        owner_id,
        cleanup->latch_owner_id,
        cleanup->latch_owner_generation,
        &released_views
    );
    return read_view_release_result == MYLITE_OWNERLESS_READ_VIEW_REGISTRY_OK
               ? MYLITE_OWNERLESS_PROCESS_CLEANUP_OK
               : MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
}

int ownerless_process_release_owner_page_write_locks(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
) {
    if (cleanup.page_write_lock_registry == nullptr ||
        cleanup.page_write_lock_registry_size == 0U) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }

    std::uint32_t released_page_write_locks = 0;
    if (mylite_ownerless_innodb_lock_registry_release_owner(
            cleanup.page_write_lock_registry,
            cleanup.page_write_lock_registry_size,
            owner_id,
            cleanup.latch_owner_id,
            cleanup.latch_owner_generation,
            &released_page_write_locks
        ) != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK) {
        return MYLITE_OWNERLESS_PROCESS_CLEANUP_ERROR;
    }
    return MYLITE_OWNERLESS_PROCESS_CLEANUP_OK;
}

bool ownerless_process_owner_state_requires_recovery(
    OwnerlessProcessCleanupContext &cleanup,
    std::uint32_t owner_id
) {
    if (cleanup.lock_table == nullptr || cleanup.lock_table_size == 0U ||
        cleanup.trx_registry == nullptr || cleanup.trx_registry_size == 0U ||
        cleanup.read_view_registry == nullptr || cleanup.read_view_registry_size == 0U ||
        cleanup.page_pin_registry == nullptr || cleanup.page_pin_registry_size == 0U ||
        cleanup.innodb_lock_registry == nullptr || cleanup.innodb_lock_registry_size == 0U ||
        cleanup.page_write_lock_registry == nullptr ||
        cleanup.page_write_lock_registry_size == 0U || cleanup.dictionary_state == nullptr ||
        cleanup.dictionary_state_size == 0U) {
        return true;
    }

    std::uint32_t active_count = 0;
    const int trx_count_result = mylite_ownerless_trx_registry_owner_active_count(
        cleanup.trx_registry,
        cleanup.trx_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (trx_count_result != MYLITE_OWNERLESS_TRX_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    /*
      Dead read-only snapshot readers publish MDL, read-view, and page-version
      pin state, but no transaction/redo/InnoDB lock/dictionary state that
      requires no-live recovery. Let normal dead-owner cleanup release those
      entries so they do not indefinitely block live-peer page-log reclamation.
    */
    const int innodb_count_result = mylite_ownerless_innodb_lock_registry_owner_active_count(
        cleanup.innodb_lock_registry,
        cleanup.innodb_lock_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (innodb_count_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    const int page_write_count_result = mylite_ownerless_innodb_lock_registry_owner_active_count(
        cleanup.page_write_lock_registry,
        cleanup.page_write_lock_registry_size,
        owner_id,
        cleanup.latch_owner_id,
        cleanup.latch_owner_generation,
        &active_count
    );
    if (page_write_count_result != MYLITE_OWNERLESS_INNODB_LOCK_REGISTRY_OK || active_count > 0U) {
        return true;
    }
    if (cleanup.redo_state != nullptr &&
        cleanup.redo_state_size >= k_concurrency_redo_state_segment_size) {
        if (mylite_ownerless_redo_state_owner_active_count(
                cleanup.redo_state,
                cleanup.redo_state_size,
                owner_id,
                &active_count
            ) != MYLITE_OWNERLESS_REDO_STATE_OK ||
            active_count > 0U) {
            return true;
        }
        mylite_ownerless_redo_state_snapshot snapshot = {};
        if (mylite_ownerless_redo_state_read_snapshot(
                cleanup.redo_state,
                cleanup.redo_state_size,
                &snapshot
            ) != MYLITE_OWNERLESS_REDO_STATE_OK) {
            return true;
        }
        if (snapshot.latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED &&
            snapshot.latch_owner_id == owner_id) {
            return true;
        }
        if (snapshot.progress_latch_state == MYLITE_OWNERLESS_LATCH_STATE_LOCKED &&
            snapshot.progress_latch_owner_id == owner_id) {
            return true;
        }
    }
    const int dictionary_count_result = mylite_ownerless_dictionary_state_owner_active_count(
        cleanup.dictionary_state,
        cleanup.dictionary_state_size,
        owner_id,
        &active_count
    );
    if (dictionary_count_result != MYLITE_OWNERLESS_DICTIONARY_STATE_OK || active_count > 0U) {
        return true;
    }
    return false;
}

int mylite_result_from_process_registry_result(int registry_result) {
    if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_OK) {
        return MYLITE_OK;
    }
    if (registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_FULL ||
        registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_TIMEOUT ||
        registry_result == MYLITE_OWNERLESS_PROCESS_REGISTRY_BUSY) {
        return MYLITE_BUSY;
    }
    return MYLITE_IOERR;
}

bool update_concurrency_shm_state(int shm_fd, std::uint32_t state) {
    std::array<unsigned char, 4> state_bytes = {};
    store_le32(state_bytes.data(), 0, state);
    return write_exact_at(
        shm_fd,
        state_bytes.data(),
        state_bytes.size(),
        static_cast<off_t>(k_concurrency_shm_state_offset)
    );
}

bool update_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t latest_lsn,
    std::uint64_t visible_lsn,
    bool durable
) {
    if (checkpoint_fd < 0 || (latest_lsn == 0U && visible_lsn == 0U)) {
        return false;
    }
    if (!acquire_fd_write_lock(
            checkpoint_fd,
            k_concurrency_checkpoint_lock_start,
            k_concurrency_checkpoint_lock_length
        )) {
        return false;
    }

    std::uint64_t current_latest_lsn = 0;
    std::uint64_t current_visible_lsn = 0;
    bool ok =
        read_concurrency_checkpoint_lsn(checkpoint_fd, &current_latest_lsn, &current_visible_lsn);
    if (ok) {
        latest_lsn = std::max(latest_lsn, current_latest_lsn);
        visible_lsn = std::max(visible_lsn, current_visible_lsn);
        if (visible_lsn > latest_lsn) {
            latest_lsn = visible_lsn;
        }
        std::array<unsigned char, 16> payload = {};
        store_le64(payload.data(), 0U, latest_lsn);
        store_le64(payload.data(), sizeof(std::uint64_t), visible_lsn);
        ok = write_exact_at(
                 checkpoint_fd,
                 payload.data(),
                 payload.size(),
                 static_cast<off_t>(k_concurrency_checkpoint_latest_lsn_offset)
             ) &&
             (!durable || ::fsync(checkpoint_fd) == 0);
    }

    release_fd_lock(
        checkpoint_fd,
        k_concurrency_checkpoint_lock_start,
        k_concurrency_checkpoint_lock_length
    );
    return ok;
}

bool read_concurrency_checkpoint_lsn(
    int checkpoint_fd,
    std::uint64_t *out_latest_lsn,
    std::uint64_t *out_visible_lsn
) {
    if (checkpoint_fd < 0 || out_latest_lsn == nullptr || out_visible_lsn == nullptr) {
        return false;
    }

    std::array<unsigned char, 16> payload = {};
    ssize_t bytes_read = 0;
    do {
        bytes_read = ::pread(
            checkpoint_fd,
            payload.data(),
            payload.size(),
            static_cast<off_t>(k_concurrency_checkpoint_latest_lsn_offset)
        );
    } while (bytes_read < 0 && errno == EINTR);

    if (bytes_read == 0) {
        *out_latest_lsn = 0U;
        *out_visible_lsn = 0U;
        return true;
    }
    if (bytes_read != static_cast<ssize_t>(payload.size())) {
        return false;
    }

    *out_latest_lsn = load_le64(payload.data(), 0U);
    *out_visible_lsn = load_le64(payload.data(), sizeof(std::uint64_t));
    if (*out_visible_lsn > *out_latest_lsn) {
        *out_latest_lsn = *out_visible_lsn;
    }
    return true;
}

bool acquire_fd_write_lock(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    while (::fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

void release_fd_lock(int fd, off_t start, off_t length) {
    struct flock lock = {};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    static_cast<void>(::fcntl(fd, F_SETLK, &lock));
}

std::uint64_t current_time_milliseconds(void) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
    );
}

bool read_exact_at(int fd, unsigned char *data, std::size_t length, off_t offset) {
    while (length > 0U) {
        const ssize_t bytes_read = ::pread(fd, data, length, offset);
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_read == 0) {
            return false;
        }
        data += bytes_read;
        length -= static_cast<std::size_t>(bytes_read);
        offset += bytes_read;
    }
    return true;
}

bool write_exact_at(int fd, const unsigned char *data, std::size_t length, off_t offset) {
    while (length > 0U) {
        const ssize_t bytes_written = ::pwrite(fd, data, length, offset);
        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes_written == 0) {
            return false;
        }
        data += bytes_written;
        length -= static_cast<std::size_t>(bytes_written);
        offset += bytes_written;
    }
    return true;
}

std::uint32_t load_le32(const unsigned char *bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t load_le64(const unsigned char *bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void store_le32(unsigned char *bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
}

void store_le64(unsigned char *bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU);
    }
}

std::uint64_t load_shared64(const unsigned char *bytes, std::size_t offset) {
    const auto *value = reinterpret_cast<const std::uint64_t *>(bytes + offset);
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

int validate_concurrency_metadata(const std::filesystem::path &metadata_path) {
    std::ifstream metadata(metadata_path, std::ios::binary);
    if (!metadata) {
        return MYLITE_IOERR;
    }

    bool has_format = false;
    bool has_mariadb_base = false;
    bool has_database_uuid = false;
    bool has_concurrency_generation = false;
    bool has_mode = false;
    const std::string mariadb_base_line = std::string("mariadb_base=") + k_mariadb_base_ref;
    for (std::string line; std::getline(metadata, line);) {
        if (line == k_metadata_format_line) {
            has_format = true;
            continue;
        }
        if (line == mariadb_base_line) {
            has_mariadb_base = true;
            continue;
        }
        if (line.rfind("database_uuid=", 0) == 0) {
            has_database_uuid = is_database_uuid(std::string_view(line).substr(14U));
            continue;
        }
        if (line.rfind("concurrency_generation=", 0) == 0) {
            has_concurrency_generation = is_unsigned_decimal(std::string_view(line).substr(23U));
            continue;
        }
        if (line == k_concurrency_mode_line) {
            has_mode = true;
        }
    }
    if (!metadata.eof()) {
        return MYLITE_IOERR;
    }

    return has_format && has_mariadb_base && has_database_uuid && has_concurrency_generation &&
                   has_mode
               ? MYLITE_OK
               : MYLITE_CORRUPT;
}

bool database_directory_is_empty(
    const std::filesystem::path &database_path,
    std::error_code &error
) {
    const std::filesystem::directory_iterator entry(database_path, error);
    if (error) {
        return false;
    }
    return entry == std::filesystem::directory_iterator();
}

int start_runtime(mylite_db &db, unsigned flags, const mylite_open_config *config) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    const bool ownerless_rw_open = (flags & MYLITE_OPEN_OWNERLESS_RW) != 0U;
    const bool ownerless_runtime_open =
        ownerless_rw_open || (flags & MYLITE_OPEN_SHARED_READONLY) != 0U;
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.database_path != db.database_path) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
            return MYLITE_BUSY;
        }
        if (g_runtime.ownerless_rw_mode != ownerless_runtime_open) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open with a different mode");
            return MYLITE_BUSY;
        }
        if (g_runtime.readonly_mode != db.readonly_open) {
            set_error(
                db,
                MYLITE_BUSY,
                "embedded runtime is already open with a different access mode"
            );
            return MYLITE_BUSY;
        }
        db.ownerless_rw_open = g_runtime.ownerless_rw_mode;
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

    const bool memory_database = is_memory_database_path(db.database_path);
    const bool skip_database_lock =
        ownerless_runtime_open || unsafe_disable_database_lock_for_tests();
    int lock_fd = -1;
    if (!memory_database && !skip_database_lock) {
        lock_fd = acquire_database_lock(db, db.database_path, config);
        if (lock_fd < 0) {
            return db.errcode;
        }
    }

    bool concurrency_mapped = false;
    bool server_initialized = false;
    RuntimeLayout layout = {};
    try {
        if (!memory_database) {
            const int concurrency_result = prepare_concurrency_metadata(db.database_path);
            if (concurrency_result != MYLITE_OK) {
                release_database_lock(lock_fd);
                set_error(db, concurrency_result, "database concurrency metadata is invalid");
                return concurrency_result;
            }
            const int shared_memory_result =
                prepare_concurrency_shared_memory(db.database_path, !db.readonly_open);
            if (shared_memory_result != MYLITE_OK) {
                release_database_lock(lock_fd);
                set_error(
                    db,
                    shared_memory_result,
                    "database concurrency shared memory is invalid"
                );
                return shared_memory_result;
            }
        }

        layout = create_runtime_layout(db.database_path, config, !skip_database_lock);
        g_runtime.arguments = runtime_arguments(layout, ownerless_runtime_open, db.readonly_open);
        g_runtime.argv = mutable_arguments(g_runtime.arguments);
        g_runtime.cleanup_directory = layout.cleanup_directory;
        g_runtime.cleanup_tmp_directory = layout.cleanup_tmp_directory;
        g_runtime.runtime_parent_directory = layout.runtime_parent_directory;
        g_runtime.database_path = db.database_path;
        char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};

        if (!memory_database) {
            const int concurrency_runtime_result =
                map_concurrency_shared_memory_for_runtime(db.database_path, g_runtime);
            if (concurrency_runtime_result != MYLITE_OK) {
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(
                    db,
                    concurrency_runtime_result,
                    "database ownerless concurrency runtime is invalid"
                );
                return concurrency_runtime_result;
            }
            concurrency_mapped = true;
            g_runtime.ownerless_innodb_lock_hook.page_log_reads_enabled =
                ownerless_runtime_open || !db.readonly_open;

            const int page_log_result =
                open_concurrency_page_log_for_runtime(db.database_path, g_runtime);
            if (page_log_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, page_log_result, "database ownerless page log is invalid");
                return page_log_result;
            }
            const int checkpoint_result =
                open_concurrency_checkpoint_for_runtime(db.database_path, g_runtime);
            if (checkpoint_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, checkpoint_result, "database ownerless checkpoint is invalid");
                return checkpoint_result;
            }

            const int lifecycle_hook_result = install_ownerless_runtime_lifecycle_hooks(g_runtime);
            if (lifecycle_hook_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(
                    db,
                    lifecycle_hook_result,
                    "database ownerless runtime hooks are invalid"
                );
                return lifecycle_hook_result;
            }

            const int lock_hook_result = install_ownerless_innodb_lock_hooks(g_runtime);
            if (lock_hook_result != MYLITE_OK) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, lock_hook_result, "database ownerless lock hooks are invalid");
                return lock_hook_result;
            }
            mylite_ownerless_innodb_clear_external_page_visibility();
        }

        int bootstrap_lock_fd = -1;
        if (!memory_database && skip_database_lock) {
            const std::filesystem::path lock_path = std::filesystem::path(db.database_path) /
                                                    k_concurrency_dir_name /
                                                    k_concurrency_lock_filename;
            bootstrap_lock_fd = acquire_concurrency_lock(
                lock_path,
                k_system_tables_lock_start,
                k_system_tables_lock_length,
                F_WRLCK,
                k_system_tables_lock_wait_timeout_ms
            );
            if (bootstrap_lock_fd < 0) {
                if (concurrency_mapped) {
                    unmap_concurrency_shared_memory_for_runtime(g_runtime);
                    concurrency_mapped = false;
                }
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, MYLITE_BUSY, "database runtime bootstrap is busy");
                return MYLITE_BUSY;
            }
        }

        const int init_result = mysql_server_init(
            static_cast<int>(g_runtime.argv.size()),
            g_runtime.argv.data(),
            groups
        );
        if (init_result != 0) {
            release_concurrency_lock(
                bootstrap_lock_fd,
                k_system_tables_lock_start,
                k_system_tables_lock_length
            );
            if (concurrency_mapped) {
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
            }
            clear_runtime_state(g_runtime);
            cleanup_runtime_layout(layout);
            release_database_lock(lock_fd);
            set_error(db, MYLITE_ERROR, "MariaDB embedded runtime initialization failed");
            return MYLITE_ERROR;
        }
        server_initialized = true;

        if (!memory_database) {
            const int hook_result = install_ownerless_runtime_hooks(g_runtime);
            if (hook_result != MYLITE_OK) {
                mysql_server_end();
                server_initialized = false;
                release_concurrency_lock(
                    bootstrap_lock_fd,
                    k_system_tables_lock_start,
                    k_system_tables_lock_length
                );
                unmap_concurrency_shared_memory_for_runtime(g_runtime);
                concurrency_mapped = false;
                clear_runtime_state(g_runtime);
                cleanup_runtime_layout(layout);
                release_database_lock(lock_fd);
                set_error(db, hook_result, "database ownerless concurrency runtime is invalid");
                return hook_result;
            }
        }
        release_concurrency_lock(
            bootstrap_lock_fd,
            k_system_tables_lock_start,
            k_system_tables_lock_length
        );

        g_runtime.ref_count = 1;
        g_runtime.lock_fd = lock_fd;
        g_runtime.ownerless_rw_mode = ownerless_runtime_open;
        g_runtime.readonly_mode = db.readonly_open;
        return MYLITE_OK;
    } catch (...) {
        clear_runtime_state(g_runtime);
        if (server_initialized) {
            mysql_server_end();
        }
        if (concurrency_mapped) {
            unmap_concurrency_shared_memory_for_runtime(g_runtime);
        }
        cleanup_runtime_layout(layout);
        release_database_lock(lock_fd);
        throw;
    }
}

int connect_runtime(mylite_db &db) {
    if (mysql_init(&db.mysql) == nullptr) {
        set_error(db, MYLITE_NOMEM, "MariaDB connection allocation failed");
        return MYLITE_NOMEM;
    }

    const MYSQL *connection =
        mysql_real_connect(&db.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0);
    if (connection == nullptr) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }

    db.connected = true;
    return MYLITE_OK;
}

int ensure_core_system_tables(mylite_db &db) {
    if (is_memory_database_path(db.database_path)) {
        return execute_core_system_table_statements(db);
    }
    if (db.readonly_open) {
        return MYLITE_OK;
    }

    const std::lock_guard<std::mutex> guard(g_system_table_mutex);
    const std::filesystem::path lock_path = std::filesystem::path(db.database_path) /
                                            k_concurrency_dir_name / k_concurrency_lock_filename;
    const int lock_fd = acquire_concurrency_lock(
        lock_path,
        k_system_tables_lock_start,
        k_system_tables_lock_length,
        F_WRLCK,
        k_system_tables_lock_wait_timeout_ms
    );
    if (lock_fd < 0) {
        set_error(db, MYLITE_BUSY, "database system table initialization is busy");
        return MYLITE_BUSY;
    }

    const int result = execute_core_system_table_statements(db);
    release_concurrency_lock(lock_fd, k_system_tables_lock_start, k_system_tables_lock_length);
    return result;
}

int execute_core_system_table_statements(mylite_db &db) {
    int result = execute_system_table_statement(db, k_create_mysql_database_sql);
    if (result != MYLITE_OK) {
        return result;
    }

    result = execute_system_table_statement(db, k_create_proc_table_sql);
    if (result != MYLITE_OK) {
        return result;
    }

    return execute_system_table_statement(db, k_create_procs_priv_table_sql);
}

int execute_system_table_statement(mylite_db &db, const char *sql) {
    if (mysql_query(&db.mysql, sql) != 0) {
        set_mariadb_error(db);
        return MYLITE_ERROR;
    }
    if (drain_remaining_query_results(db) != MYLITE_OK) {
        return MYLITE_ERROR;
    }

    set_ok(db);
    return MYLITE_OK;
}
#endif

void close_connection(mylite_db &db) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (db.connected) {
        mysql_close(&db.mysql);
        db.connected = false;
    }
#else
    (void)db;
#endif
}

void release_runtime(void) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count == 0U) {
        return;
    }

    --g_runtime.ref_count;
    if (g_runtime.ref_count > 0U) {
        return;
    }

#if MYLITE_WITH_MARIADB_EMBEDDED
    reclaim_ownerless_page_log_after_native_checkpoint(g_runtime);
    mysql_thread_end();
    mysql_server_end();
    unmap_concurrency_shared_memory_for_runtime(g_runtime);
#endif
    cleanup_runtime_state(g_runtime);
#if MYLITE_WITH_MARIADB_EMBEDDED
    release_database_lock(g_runtime.lock_fd);
    g_runtime.lock_fd = -1;
#endif

    clear_runtime_state(g_runtime);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void cleanup_runtime_layout(const RuntimeLayout &layout) {
    remove_directory_if_present(layout.cleanup_tmp_directory);
    remove_directory_if_present(layout.cleanup_directory);
    remove_directory_if_empty(layout.runtime_parent_directory);
}
#endif

void cleanup_runtime_state(RuntimeState &runtime) {
    remove_directory_if_present(runtime.cleanup_tmp_directory);
    remove_directory_if_present(runtime.cleanup_directory);
    remove_directory_if_empty(runtime.runtime_parent_directory);
}

void clear_runtime_state(RuntimeState &runtime) {
    runtime.cleanup_directory.clear();
    runtime.cleanup_tmp_directory.clear();
    runtime.runtime_parent_directory.clear();
    runtime.database_path.clear();
    runtime.argv.clear();
    runtime.arguments.clear();
    runtime.ownerless_rw_mode = false;
    runtime.readonly_mode = false;
}

void remove_directory_if_empty(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code error;
    static_cast<void>(std::filesystem::remove(directory, error));
}

#if MYLITE_WITH_MARIADB_EMBEDDED
std::filesystem::path normalize_database_path(const char *path) {
    if (std::strcmp(path, k_memory_database_path) == 0) {
        return std::filesystem::path(k_memory_database_path);
    }
    return std::filesystem::absolute(std::filesystem::path(path));
}

bool is_memory_database_path(const std::filesystem::path &database_path) {
    return database_path == std::filesystem::path(k_memory_database_path);
}

void initialize_database_layout(const std::filesystem::path &database_path) {
    create_layout_directory(database_path / k_datadir_name, "create database data directory");
    create_layout_directory(database_path / k_tmpdir_name, "create database temporary directory");

    const std::filesystem::path metadata_path = database_path / k_meta_filename;
    std::error_code error;
    if (std::filesystem::exists(metadata_path, error)) {
        if (error || !std::filesystem::is_regular_file(metadata_path, error) || error) {
            throw std::filesystem::filesystem_error(
                "validate database metadata",
                metadata_path,
                error ? error : std::make_error_code(std::errc::invalid_argument)
            );
        }
        return;
    }
    if (error) {
        throw std::filesystem::filesystem_error("validate database metadata", metadata_path, error);
    }

    write_database_metadata(metadata_path);
}

void create_layout_directory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

void write_database_metadata(const std::filesystem::path &metadata_path) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "create database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

void write_concurrency_metadata(const std::filesystem::path &metadata_path) {
    std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "create concurrency metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }

    metadata << k_metadata_format_line << "\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    metadata << "database_uuid=" << generate_database_uuid() << "\n";
    metadata << "concurrency_generation=0\n";
    metadata << k_concurrency_mode_line << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write concurrency metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

std::string generate_database_uuid(void) {
    constexpr char k_hex_digits[] = "0123456789abcdef";
    std::array<unsigned char, 16> bytes = {};
    fill_database_uuid_bytes(bytes);
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

    std::string uuid;
    uuid.reserve(36U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            uuid.push_back('-');
        }
        uuid.push_back(k_hex_digits[bytes[index] >> 4U]);
        uuid.push_back(k_hex_digits[bytes[index] & 0x0FU]);
    }
    return uuid;
}

void fill_database_uuid_bytes(std::array<unsigned char, 16> &bytes) {
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (random.read(
            reinterpret_cast<char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        )) {
        return;
    }
    fill_database_uuid_bytes_from_fallback(bytes);
}

void fill_database_uuid_bytes_from_fallback(std::array<unsigned char, 16> &bytes) {
    std::uint64_t state = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    state ^= static_cast<std::uint64_t>(::getpid()) << 32U;
    state ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&bytes));

    for (unsigned char &byte : bytes) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        byte = static_cast<unsigned char>(state & 0xFFU);
    }
}

bool is_database_uuid(std::string_view value) {
    if (value.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_unsigned_decimal(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config,
    bool allow_stale_cleanup
) {
    if (is_memory_database_path(database_path)) {
        return create_memory_runtime_layout(config);
    }
    return create_persistent_runtime_layout(database_path, allow_stale_cleanup);
}

RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config) {
    const std::filesystem::path root = runtime_root(config);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("create runtime root", root, error);
    }

    for (int attempt = 0; attempt < k_runtime_directory_attempts; ++attempt) {
        const std::filesystem::path candidate = root / unique_runtime_name();
        if (std::filesystem::create_directory(candidate, error)) {
            RuntimeLayout layout = {};
            layout.cleanup_directory = candidate;
            layout.data_directory = candidate / "data";
            layout.tmp_directory = candidate / "tmp";
            layout.plugin_directory = candidate / "plugins";
            create_runtime_subdirectory(layout.data_directory, "create runtime data directory");
            create_runtime_subdirectory(layout.tmp_directory, "create runtime temporary directory");
            create_runtime_subdirectory(layout.plugin_directory, "create runtime plugin directory");
            return layout;
        }
        if (error) {
            throw std::filesystem::filesystem_error("create runtime directory", candidate, error);
        }
    }

    throw std::filesystem::filesystem_error(
        "create runtime directory",
        root,
        std::make_error_code(std::errc::file_exists)
    );
}

RuntimeLayout create_persistent_runtime_layout(
    const std::filesystem::path &database_path,
    bool allow_stale_cleanup
) {
    const std::filesystem::path run_root = database_path / k_rundir_name;
    const std::filesystem::path tmp_root = database_path / k_tmpdir_name;

    if (allow_stale_cleanup) {
        remove_directory_if_present(run_root);
        remove_directory_contents_if_present(tmp_root);
    }
    create_runtime_subdirectory(run_root, "create database runtime root directory");
    create_runtime_subdirectory(tmp_root, "create database temporary root directory");

    std::error_code error;
    for (int attempt = 0; attempt < k_runtime_directory_attempts; ++attempt) {
        const std::filesystem::path runtime_name = unique_runtime_name();
        const std::filesystem::path run_candidate = run_root / runtime_name;
        if (!std::filesystem::create_directory(run_candidate, error)) {
            if (error) {
                throw std::filesystem::filesystem_error(
                    "create database runtime directory",
                    run_candidate,
                    error
                );
            }
            continue;
        }

        RuntimeLayout layout = {};
        layout.cleanup_directory = run_candidate;
        layout.cleanup_tmp_directory = tmp_root / runtime_name;
        layout.runtime_parent_directory = run_root;
        layout.data_directory = database_path / k_datadir_name;
        layout.tmp_directory = layout.cleanup_tmp_directory;
        layout.plugin_directory = layout.cleanup_directory / k_plugin_directory_name;
        try {
            create_runtime_subdirectory(
                layout.tmp_directory,
                "create database temporary directory"
            );
            create_runtime_subdirectory(
                layout.plugin_directory,
                "create database plugin directory"
            );
        } catch (...) {
            remove_directory_if_present(layout.cleanup_tmp_directory);
            remove_directory_if_present(layout.cleanup_directory);
            throw;
        }
        return layout;
    }

    throw std::filesystem::filesystem_error(
        "create database runtime directory",
        run_root,
        std::make_error_code(std::errc::file_exists)
    );
}

int acquire_database_lock(
    mylite_db &db,
    const std::filesystem::path &database_path,
    const mylite_open_config *config
) {
    const std::filesystem::path lock_path = database_path / k_lock_filename;
    const std::string lock_name = lock_path.string();
    const int lock_fd = ::open(lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error(db, MYLITE_IOERR, "database lock file could not be opened");
        return -1;
    }

    DatabaseLockWait wait = {};
    wait.lock_fd = lock_fd;
    wait.busy_timeout_ms = configured_busy_timeout_ms(config);
    const int lock_result = wait_for_database_lock(wait);
    if (lock_result == MYLITE_OK) {
        return lock_fd;
    }

    release_database_lock(lock_fd);
    if (lock_result == MYLITE_BUSY) {
        set_error(db, MYLITE_BUSY, "database directory is locked by another process");
    } else {
        set_error(db, MYLITE_IOERR, "database lock could not be acquired");
    }
    return -1;
}

int wait_for_database_lock(DatabaseLockWait wait) {
    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(wait.busy_timeout_ms);
    for (;;) {
        if (::flock(wait.lock_fd, LOCK_EX | LOCK_NB) == 0) {
            return MYLITE_OK;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            return MYLITE_IOERR;
        }
        if (wait.busy_timeout_ms == 0U || std::chrono::steady_clock::now() - start >= timeout) {
            return MYLITE_BUSY;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(k_lock_poll_interval_ms));
    }
}

void release_database_lock(int lock_fd) {
    if (lock_fd >= 0) {
        static_cast<void>(::flock(lock_fd, LOCK_UN));
        static_cast<void>(::close(lock_fd));
    }
}

unsigned configured_busy_timeout_ms(const mylite_open_config *config) {
    if (has_config_field(
            config,
            offsetof(mylite_open_config, busy_timeout_ms) + sizeof(config->busy_timeout_ms)
        )) {
        return config->busy_timeout_ms;
    }
    return 0U;
}

bool unsafe_disable_database_lock_for_tests(void) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *value = std::getenv("MYLITE_UNSAFE_DISABLE_DIRECTORY_LOCK_FOR_TESTS");
    return value != nullptr && std::strcmp(value, "1") == 0;
#  else
    return false;
#  endif
}

void pause_for_ownerless_test_fault(const char *fault_name) {
#  if MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS
    const char *configured_fault = std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
    if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name) != 0) {
        return;
    }

    const char *ready_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD");
    if (ready_fd_value != nullptr) {
        char *end = nullptr;
        const long ready_fd = std::strtol(ready_fd_value, &end, k_decimal_base);
        if (end != ready_fd_value && *end == '\0' && ready_fd >= 0 &&
            ready_fd <= std::numeric_limits<int>::max()) {
            const char value = 'x';
            static_cast<void>(::write(static_cast<int>(ready_fd), &value, sizeof(value)));
            static_cast<void>(::close(static_cast<int>(ready_fd)));
        }
    }

    const char *release_fd_value = std::getenv("MYLITE_OWNERLESS_TEST_FAULT_RELEASE_FD");
    if (release_fd_value != nullptr) {
        char *end = nullptr;
        const long release_fd = std::strtol(release_fd_value, &end, k_decimal_base);
        if (end != release_fd_value && *end == '\0' && release_fd >= 0 &&
            release_fd <= std::numeric_limits<int>::max()) {
            char value = '\0';
            ssize_t bytes_read = -1;
            do {
                bytes_read = ::read(static_cast<int>(release_fd), &value, sizeof(value));
            } while (bytes_read < 0 && errno == EINTR);
            static_cast<void>(::close(static_cast<int>(release_fd)));
            if (bytes_read == sizeof(value)) {
                return;
            }
        }
    }

    for (;;) {
        ::pause();
    }
#  else
    (void)fault_name;
#  endif
}

std::filesystem::path runtime_root(const mylite_open_config *config) {
    if (config != nullptr &&
        has_config_field(
            config,
            offsetof(mylite_open_config, temp_directory) + sizeof(config->temp_directory)
        ) &&
        config->temp_directory != nullptr && config->temp_directory[0] != '\0') {
        return std::filesystem::path(config->temp_directory);
    }
    return std::filesystem::temp_directory_path();
}

std::string unique_runtime_name(void) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    static unsigned counter = 0;
    return "mylite-runtime-" + std::to_string(now) + "-" + std::to_string(++counter);
}

std::string innodb_temp_data_file_path_argument(
    const RuntimeLayout &layout,
    bool ownerless_rw_open
) {
    if (!ownerless_rw_open) {
        return k_innodb_temp_data_file_path;
    }

    const std::filesystem::path temp_file =
        layout.tmp_directory / k_innodb_temp_tablespace_filename;
    const std::filesystem::path relative_temp_file =
        temp_file.lexically_relative(layout.data_directory);
    if (relative_temp_file.empty()) {
        return k_innodb_temp_data_file_path;
    }
    return relative_temp_file.generic_string() + ":12M:autoextend";
}

void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

std::vector<std::string> runtime_arguments(
    const RuntimeLayout &layout,
    bool ownerless_rw_open,
    bool readonly_open
) {
    std::vector<std::string> arguments = {
        "mylite",
        "--no-defaults",
        "--datadir=" + layout.data_directory.string(),
        "--tmpdir=" + layout.tmp_directory.string(),
        "--plugin-dir=" + layout.plugin_directory.string(),
        "--aria-log-dir-path=" + layout.data_directory.string(),
        "--innodb-data-home-dir=" + layout.data_directory.string(),
        "--innodb-log-group-home-dir=" + layout.data_directory.string(),
        "--innodb-undo-directory=" + layout.data_directory.string(),
        "--innodb-tmpdir=" + layout.tmp_directory.string(),
        "--innodb-temp-data-file-path=" +
            innodb_temp_data_file_path_argument(layout, ownerless_rw_open),
        "--innodb-flush-log-at-trx-commit=1",
        "--innodb-fast-shutdown=1",
        "--innodb-buffer-pool-dump-at-shutdown=OFF",
        "--innodb-buffer-pool-load-at-startup=OFF",
        "--log-output=NONE",
        "--max-digest-length=0",
        "--use-stat-tables=never",
        "--histogram-size=0",
        "--skip-log-bin",
        "--skip-slave-start",
        "--skip-grant-tables",
        "--skip-networking",
#  ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
        "--performance-schema=OFF",
#  endif
        std::string("--lc-messages-dir=") + MYLITE_MARIADB_MESSAGES_DIR,
        std::string("--character-sets-dir=") + MYLITE_MARIADB_CHARSETS_DIR,
    };
    if (readonly_open) {
        arguments.emplace_back("--read-only=ON");
    }
    if (ownerless_rw_open) {
        arguments.emplace_back("--mylite-ownerless-managed-file-locks");
    } else {
        arguments.emplace_back("--skip-mylite-ownerless-managed-file-locks");
    }
    return arguments;
}

std::vector<char *> mutable_arguments(std::vector<std::string> &arguments) {
    std::vector<char *> argv;
    argv.reserve(arguments.size());
    std::transform(
        arguments.begin(),
        arguments.end(),
        std::back_inserter(argv),
        [](std::string &argument) { return argument.data(); }
    );
    return argv;
}
#endif

void remove_directory_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void remove_directory_contents_if_present(const std::filesystem::path &directory) {
    if (directory.empty()) {
        return;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return;
    }

    std::filesystem::directory_iterator entry(directory, error);
    const std::filesystem::directory_iterator end;
    for (; entry != end && !error; entry.increment(error)) {
        std::error_code ignored;
        std::filesystem::remove_all(entry->path(), ignored);
    }
}
#endif

int copy_error_message(mylite_db &db, char **errmsg) {
    if (errmsg == nullptr) {
        return db.errcode;
    }

    const std::size_t length = db.errmsg.size();
    char *copy = static_cast<char *>(std::malloc(length + 1U));
    if (copy == nullptr) {
        return MYLITE_NOMEM;
    }

    std::memcpy(copy, db.errmsg.c_str(), length + 1U);
    *errmsg = copy;
    return db.errcode;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db) {
    db.errcode = MYLITE_OK;
    db.extended_errcode = MYLITE_OK;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_ok;
    db.errmsg = k_not_an_error;
}
#endif

void set_error(mylite_db &db, int code, const char *message) {
    db.errcode = code;
    db.extended_errcode = code;
    db.mariadb_errno = 0;
    db.sqlstate = k_sqlstate_general;
    db.errmsg = message;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db) {
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_errno(&db.mysql);
    db.sqlstate = mysql_sqlstate(&db.mysql);
    db.errmsg = mysql_error(&db.mysql);
}

void set_mariadb_statement_error(mylite_stmt &stmt) {
    mylite_db &db = *stmt.db;
    db.errcode = MYLITE_ERROR;
    db.extended_errcode = MYLITE_ERROR;
    db.mariadb_errno = mysql_stmt_errno(stmt.stmt);
    db.sqlstate = mysql_stmt_sqlstate(stmt.stmt);
    db.errmsg = mysql_stmt_error(stmt.stmt);
}

int parse_warning_level(const char *level) {
    if (level != nullptr && std::strcmp(level, "Note") == 0) {
        return MYLITE_WARNING_NOTE;
    }
    if (level != nullptr && std::strcmp(level, "Error") == 0) {
        return MYLITE_WARNING_ERROR;
    }
    return MYLITE_WARNING_WARNING;
}
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace
