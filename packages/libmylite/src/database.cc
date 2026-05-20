#include <mylite/mylite.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#if MYLITE_WITH_MARIADB_EMBEDDED
#  include <mysql.h>
#endif

#ifndef MYLITE_MARIADB_MESSAGES_DIR
#  define MYLITE_MARIADB_MESSAGES_DIR ""
#endif

#ifndef MYLITE_MARIADB_CHARSETS_DIR
#  define MYLITE_MARIADB_CHARSETS_DIR ""
#endif

namespace {

constexpr unsigned k_known_open_flags = MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE |
                                        MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE |
                                        MYLITE_OPEN_URI;
constexpr const char *k_sqlstate_ok = "00000";
constexpr const char *k_sqlstate_general = "HY000";
constexpr const char *k_not_an_error = "not an error";
constexpr const char *k_bad_db_handle = "bad database handle";

#if MYLITE_WITH_MARIADB_EMBEDDED
constexpr const char *k_memory_database_path = ":memory:";
constexpr const char *k_meta_filename = "mylite.meta";
constexpr const char *k_datadir_name = "datadir";
constexpr const char *k_tmpdir_name = "tmp";
constexpr const char *k_rundir_name = "run";
constexpr const char *k_plugin_directory_name = "plugins";
constexpr const char *k_mariadb_base_ref = "mariadb-11.8.6";
constexpr int k_runtime_directory_attempts = 100;

struct RuntimeLayout {
    std::filesystem::path cleanup_directory;
    std::filesystem::path persistent_tmp_directory;
    std::filesystem::path data_directory;
    std::filesystem::path tmp_directory;
    std::filesystem::path plugin_directory;
};
#endif

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path cleanup_directory;
    std::filesystem::path persistent_tmp_directory;
    std::string database_path;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
};

RuntimeState g_runtime;

} // namespace

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
    std::string database_path;
    int errcode = MYLITE_OK;
    int extended_errcode = MYLITE_OK;
    unsigned mariadb_errno = 0;
    std::string sqlstate = k_sqlstate_ok;
    std::string errmsg = k_not_an_error;
    long long changes = 0;
    unsigned long long last_insert_id = 0;
    bool connected = false;
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
#if MYLITE_WITH_MARIADB_EMBEDDED
int validate_runtime_database_path(mylite_db &db);
int prepare_database_directory(const std::filesystem::path &database_path, unsigned flags);
int start_runtime(mylite_db &db, const mylite_open_config *config);
int connect_runtime(mylite_db &db);
#endif
void close_connection(mylite_db &db);
void release_runtime(void);
int exec_impl(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &db,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
);
std::filesystem::path normalize_database_path(const char *path);
bool is_memory_database_path(const std::filesystem::path &database_path);
void initialize_database_layout(const std::filesystem::path &database_path);
void create_layout_directory(const std::filesystem::path &directory, const char *message);
void write_database_metadata(const std::filesystem::path &metadata_path);
RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config
);
RuntimeLayout create_memory_runtime_layout(const mylite_open_config *config);
RuntimeLayout create_persistent_runtime_layout(const std::filesystem::path &database_path);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message);
std::vector<std::string> runtime_arguments(const RuntimeLayout &layout);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
void remove_directory_contents_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &db, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &db);
#endif
void set_error(mylite_db &db, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &db);
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

int mylite_close(mylite_db *db) {
    if (db == nullptr) {
        return MYLITE_OK;
    }

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

        const int runtime_path_result = validate_runtime_database_path(*db);
        if (runtime_path_result != MYLITE_OK) {
            return runtime_path_result;
        }

        const int directory_result = prepare_database_directory(db->database_path, flags);
        if (directory_result != MYLITE_OK) {
            return directory_result;
        }

        const int runtime_result = start_runtime(*db, config);
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        const int connect_result = connect_runtime(*db);
        if (connect_result != MYLITE_OK) {
            close_connection(*db);
            release_runtime();
            return connect_result;
        }

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

    if (readonly) {
        return MYLITE_MISUSE;
    }

    if ((flags & MYLITE_OPEN_URI) != 0U) {
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
    if (mysql_query(&db->mysql, sql) != 0) {
        set_mariadb_error(*db);
        return copy_error_message(*db, errmsg);
    }

    bool has_result = false;
    const int result = store_and_emit_result(*db, callback, ctx, &has_result);
    if (result != MYLITE_OK) {
        return copy_error_message(*db, errmsg);
    }

    const my_ulonglong affected_rows = mysql_affected_rows(&db->mysql);
    db->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    db->last_insert_id = static_cast<unsigned long long>(mysql_insert_id(&db->mysql));
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
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
        return MYLITE_OK;
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
            set_error(db, MYLITE_ERROR, "query callback requested abort");
            return MYLITE_ERROR;
        }
    }

    mysql_free_result(result);
    return MYLITE_OK;
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
        initialize_database_layout(database_path);
        return MYLITE_OK;
    }

    std::filesystem::create_directories(database_path, error);
    if (error) {
        return MYLITE_IOERR;
    }
    initialize_database_layout(database_path);

    return MYLITE_OK;
}

int start_runtime(mylite_db &db, const mylite_open_config *config) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.database_path != db.database_path) {
            set_error(db, MYLITE_BUSY, "embedded runtime is already open for another database");
            return MYLITE_BUSY;
        }
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

    const RuntimeLayout layout = create_runtime_layout(db.database_path, config);
    g_runtime.arguments = runtime_arguments(layout);
    g_runtime.argv = mutable_arguments(g_runtime.arguments);
    char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};

    const int init_result =
        mysql_server_init(static_cast<int>(g_runtime.argv.size()), g_runtime.argv.data(), groups);
    if (init_result != 0) {
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
        remove_directory_if_present(layout.cleanup_directory);
        set_error(db, MYLITE_ERROR, "MariaDB embedded runtime initialization failed");
        return MYLITE_ERROR;
    }

    g_runtime.cleanup_directory = layout.cleanup_directory;
    g_runtime.persistent_tmp_directory = layout.persistent_tmp_directory;
    g_runtime.database_path = db.database_path;
    g_runtime.ref_count = 1;
    return MYLITE_OK;
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
    std::filesystem::path cleanup_dir;
    std::filesystem::path tmp_dir;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.ref_count == 0U) {
            return;
        }

        --g_runtime.ref_count;
        if (g_runtime.ref_count > 0U) {
            return;
        }

        cleanup_dir = g_runtime.cleanup_directory;
        tmp_dir = g_runtime.persistent_tmp_directory;
#if MYLITE_WITH_MARIADB_EMBEDDED
        mysql_server_end();
#endif
        g_runtime.cleanup_directory.clear();
        g_runtime.persistent_tmp_directory.clear();
        g_runtime.database_path.clear();
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
    }

    remove_directory_if_present(cleanup_dir);
    remove_directory_contents_if_present(tmp_dir);
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
    create_layout_directory(database_path / k_rundir_name, "create database runtime directory");
    create_layout_directory(
        database_path / k_rundir_name / k_plugin_directory_name,
        "create database plugin directory"
    );

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

    metadata << "format=1\n";
    metadata << "mariadb_base=" << k_mariadb_base_ref << "\n";
    if (!metadata) {
        throw std::filesystem::filesystem_error(
            "write database metadata",
            metadata_path,
            std::make_error_code(std::errc::io_error)
        );
    }
}

RuntimeLayout create_runtime_layout(
    const std::filesystem::path &database_path,
    const mylite_open_config *config
) {
    if (is_memory_database_path(database_path)) {
        return create_memory_runtime_layout(config);
    }
    return create_persistent_runtime_layout(database_path);
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

RuntimeLayout create_persistent_runtime_layout(const std::filesystem::path &database_path) {
    RuntimeLayout layout = {};
    layout.cleanup_directory = database_path / k_rundir_name;
    layout.persistent_tmp_directory = database_path / k_tmpdir_name;
    layout.data_directory = database_path / k_datadir_name;
    layout.tmp_directory = layout.persistent_tmp_directory;
    layout.plugin_directory = layout.cleanup_directory / k_plugin_directory_name;

    create_runtime_subdirectory(layout.cleanup_directory, "create database runtime directory");
    create_runtime_subdirectory(layout.tmp_directory, "create database temporary directory");
    create_runtime_subdirectory(layout.plugin_directory, "create database plugin directory");
    return layout;
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

void create_runtime_subdirectory(const std::filesystem::path &directory, const char *message) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::filesystem::filesystem_error(message, directory, error);
    }
}

std::vector<std::string> runtime_arguments(const RuntimeLayout &layout) {
    return {
        "mylite",
        "--no-defaults",
        "--datadir=" + layout.data_directory.string(),
        "--tmpdir=" + layout.tmp_directory.string(),
        "--plugin-dir=" + layout.plugin_directory.string(),
        "--aria-log-dir-path=" + layout.data_directory.string(),
        "--skip-grant-tables",
        "--skip-networking",
        "--default-storage-engine=MyISAM",
        "--innodb=OFF",
        std::string("--lc-messages-dir=") + MYLITE_MARIADB_MESSAGES_DIR,
        std::string("--character-sets-dir=") + MYLITE_MARIADB_CHARSETS_DIR,
    };
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
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace
