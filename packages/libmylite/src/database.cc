#include <mylite/mylite.h>
#include <mylite/storage.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

struct RuntimeState {
    std::mutex mutex;
    unsigned ref_count = 0;
    std::filesystem::path directory;
    std::string filename;
    std::vector<std::string> arguments;
    std::vector<char *> argv;
};

RuntimeState g_runtime;

} // namespace

struct mylite_db {
#if MYLITE_WITH_MARIADB_EMBEDDED
    MYSQL mysql = {};
#endif
    std::string filename;
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

int open_v2_impl(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
int validate_open_args(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int prepare_primary_file(const std::filesystem::path &filename, unsigned flags);
int map_storage_result(mylite_storage_result result);
int start_runtime(mylite_db &database, const mylite_open_config *config);
int connect_runtime(mylite_db &database);
#endif
void close_connection(mylite_db &database);
void release_runtime(void);
int exec_impl(
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &database,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
);
std::filesystem::path normalize_filename(const char *filename);
std::filesystem::path create_runtime_directory(const mylite_open_config *config);
std::filesystem::path runtime_root(const mylite_open_config *config);
std::string unique_runtime_name(void);
std::vector<std::string> runtime_arguments(const std::filesystem::path &runtime_dir);
std::vector<char *> mutable_arguments(std::vector<std::string> &arguments);
#endif
void remove_directory_if_present(const std::filesystem::path &directory);
int copy_error_message(mylite_db &database, char **errmsg);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &database);
#endif
void set_error(mylite_db &database, int code, const char *message);
#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &database);
#endif
const char *safe_c_str(const std::string &value);
bool has_config_field(const mylite_open_config *config, std::size_t field_end);

} // namespace

int mylite_open(const char *filename, mylite_db **out_db) {
    return mylite_open_v2(filename, out_db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, nullptr);
}

int mylite_open_v2(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    return open_v2_impl(filename, out_db, flags, config);
}

int mylite_close(mylite_db *database) {
    if (database == nullptr) {
        return MYLITE_OK;
    }

    close_connection(*database);
    release_runtime();
    delete database;
    return MYLITE_OK;
}

int mylite_exec(
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    return exec_impl(database, sql, callback, ctx, errmsg);
}

int mylite_errcode(mylite_db *database) {
    return database != nullptr ? database->errcode : MYLITE_MISUSE;
}

int mylite_extended_errcode(mylite_db *database) {
    return database != nullptr ? database->extended_errcode : MYLITE_MISUSE;
}

unsigned mylite_mariadb_errno(mylite_db *database) {
    return database != nullptr ? database->mariadb_errno : 0;
}

const char *mylite_sqlstate(mylite_db *database) {
    return database != nullptr ? safe_c_str(database->sqlstate) : k_sqlstate_general;
}

const char *mylite_errmsg(mylite_db *database) {
    return database != nullptr ? safe_c_str(database->errmsg) : k_bad_db_handle;
}

long long mylite_changes(mylite_db *database) {
    return database != nullptr ? database->changes : 0;
}

unsigned long long mylite_last_insert_id(mylite_db *database) {
    return database != nullptr ? database->last_insert_id : 0;
}

void mylite_free(void *ptr) {
    std::free(ptr);
}

namespace {

int open_v2_impl(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    const int validation_result = validate_open_args(filename, out_db, flags, config);
    if (validation_result != MYLITE_OK) {
        return validation_result;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)config;
    return MYLITE_ERROR;
#else
    try {
        std::unique_ptr<mylite_db> database(new mylite_db());
        database->filename = normalize_filename(filename).string();

        const int file_result = prepare_primary_file(database->filename, flags);
        if (file_result != MYLITE_OK) {
            return file_result;
        }

        const int runtime_result = start_runtime(*database, config);
        if (runtime_result != MYLITE_OK) {
            return runtime_result;
        }

        const int connect_result = connect_runtime(*database);
        if (connect_result != MYLITE_OK) {
            close_connection(*database);
            release_runtime();
            return connect_result;
        }

        *out_db = database.release();
        return MYLITE_OK;
    } catch (const std::bad_alloc &) {
        return MYLITE_NOMEM;
    } catch (const std::filesystem::filesystem_error &) {
        return MYLITE_IOERR;
    }
#endif
}

int validate_open_args(
    const char *filename,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
) {
    if (out_db == nullptr) {
        return MYLITE_MISUSE;
    }
    *out_db = nullptr;

    if (filename == nullptr || filename[0] == '\0') {
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

    if (readonly && (flags & (MYLITE_OPEN_CREATE | MYLITE_OPEN_EXCLUSIVE)) != 0U) {
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
    mylite_db *database,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
) {
    if (errmsg != nullptr) {
        *errmsg = nullptr;
    }

    if (database == nullptr || sql == nullptr) {
        return MYLITE_MISUSE;
    }

#if !MYLITE_WITH_MARIADB_EMBEDDED
    (void)callback;
    (void)ctx;
    set_error(*database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return copy_error_message(*database, errmsg);
#else
    set_ok(*database);
    if (mysql_query(&database->mysql, sql) != 0) {
        set_mariadb_error(*database);
        return copy_error_message(*database, errmsg);
    }

    bool has_result = false;
    const int result = store_and_emit_result(*database, callback, ctx, &has_result);
    if (result != MYLITE_OK) {
        return copy_error_message(*database, errmsg);
    }

    const my_ulonglong affected_rows = mysql_affected_rows(&database->mysql);
    database->changes =
        has_result || affected_rows == static_cast<my_ulonglong>(-1)
            ? 0
            : static_cast<long long>(
                  std::min<my_ulonglong>(affected_rows, static_cast<my_ulonglong>(LLONG_MAX))
              );
    database->last_insert_id = static_cast<unsigned long long>(mysql_insert_id(&database->mysql));
    return MYLITE_OK;
#endif
}

#if MYLITE_WITH_MARIADB_EMBEDDED
int store_and_emit_result(
    mylite_db &database,
    mylite_exec_callback callback,
    void *ctx,
    bool *has_result
) {
    MYSQL_RES *result = mysql_store_result(&database.mysql);
    if (result == nullptr) {
        if (mysql_field_count(&database.mysql) != 0U) {
            set_mariadb_error(database);
            return MYLITE_ERROR;
        }
        return MYLITE_OK;
    }
    *has_result = true;

    const unsigned field_count = mysql_num_fields(result);
    if (field_count > static_cast<unsigned>(INT_MAX)) {
        mysql_free_result(result);
        set_error(database, MYLITE_ERROR, "result has too many columns");
        return MYLITE_ERROR;
    }

    std::vector<char *> column_names;
    column_names.reserve(field_count);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    for (unsigned i = 0; i < field_count; ++i) {
        column_names.push_back(fields[i].name);
    }

    for (MYSQL_ROW row = mysql_fetch_row(result); row != nullptr; row = mysql_fetch_row(result)) {
        if (callback != nullptr &&
            callback(ctx, static_cast<int>(field_count), row, column_names.data()) != 0) {
            mysql_free_result(result);
            set_error(database, MYLITE_ERROR, "query callback requested abort");
            return MYLITE_ERROR;
        }
    }

    mysql_free_result(result);
    return MYLITE_OK;
}

int prepare_primary_file(const std::filesystem::path &filename, unsigned flags) {
    if (filename == ":memory:") {
        return MYLITE_OK;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(filename, error);
    if (error) {
        return MYLITE_IOERR;
    }

    if ((flags & MYLITE_OPEN_EXCLUSIVE) != 0U && exists) {
        return MYLITE_ERROR;
    }

    if (!exists && (flags & MYLITE_OPEN_CREATE) == 0U) {
        return MYLITE_NOTFOUND;
    }

    const std::string path = filename.string();
    if (exists) {
        mylite_storage_header header = {};
        return map_storage_result(mylite_storage_open_header(path.c_str(), &header));
    }

    if (!exists) {
        const std::filesystem::path parent = filename.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return MYLITE_IOERR;
            }
        }
        return map_storage_result(mylite_storage_create_empty(path.c_str()));
    }

    return MYLITE_OK;
}

int map_storage_result(mylite_storage_result result) {
    switch (result) {
    case MYLITE_STORAGE_OK:
        return MYLITE_OK;
    case MYLITE_STORAGE_NOMEM:
        return MYLITE_NOMEM;
    case MYLITE_STORAGE_READONLY:
        return MYLITE_READONLY;
    case MYLITE_STORAGE_IOERR:
        return MYLITE_IOERR;
    case MYLITE_STORAGE_CORRUPT:
    case MYLITE_STORAGE_UNSUPPORTED:
        return MYLITE_CORRUPT;
    case MYLITE_STORAGE_NOTFOUND:
        return MYLITE_NOTFOUND;
    case MYLITE_STORAGE_MISUSE:
        return MYLITE_MISUSE;
    case MYLITE_STORAGE_ERROR:
        return MYLITE_ERROR;
    }

    return MYLITE_ERROR;
}

int start_runtime(mylite_db &database, const mylite_open_config *config) {
    const std::lock_guard<std::mutex> guard(g_runtime.mutex);
    if (g_runtime.ref_count > 0U) {
        if (g_runtime.filename != database.filename) {
            set_error(
                database,
                MYLITE_BUSY,
                "embedded runtime is already open for another database"
            );
            return MYLITE_BUSY;
        }
        ++g_runtime.ref_count;
        return MYLITE_OK;
    }

#  if MYLITE_WITH_MARIADB_EMBEDDED
    const std::filesystem::path runtime_dir = create_runtime_directory(config);
    g_runtime.arguments = runtime_arguments(runtime_dir);
    g_runtime.argv = mutable_arguments(g_runtime.arguments);
    char *groups[] = {const_cast<char *>("server"), const_cast<char *>("embedded"), nullptr};

    const int init_result =
        mysql_server_init(static_cast<int>(g_runtime.argv.size()), g_runtime.argv.data(), groups);
    if (init_result != 0) {
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
        remove_directory_if_present(runtime_dir);
        set_error(database, MYLITE_ERROR, "MariaDB embedded runtime initialization failed");
        return MYLITE_ERROR;
    }

    g_runtime.directory = runtime_dir;
    g_runtime.filename = database.filename;
    g_runtime.ref_count = 1;
    return MYLITE_OK;
#  else
    (void)config;
    set_error(database, MYLITE_ERROR, "MariaDB embedded backend is not enabled");
    return MYLITE_ERROR;
#  endif
}

int connect_runtime(mylite_db &database) {
#  if MYLITE_WITH_MARIADB_EMBEDDED
    if (mysql_init(&database.mysql) == nullptr) {
        set_error(database, MYLITE_NOMEM, "MariaDB connection allocation failed");
        return MYLITE_NOMEM;
    }

    MYSQL *connection =
        mysql_real_connect(&database.mysql, nullptr, nullptr, nullptr, nullptr, 0, nullptr, 0);
    if (connection == nullptr) {
        set_mariadb_error(database);
        return MYLITE_ERROR;
    }

    database.connected = true;
    return MYLITE_OK;
#  endif
}
#endif

void close_connection(mylite_db &database) {
#if MYLITE_WITH_MARIADB_EMBEDDED
    if (database.connected) {
        mysql_close(&database.mysql);
        database.connected = false;
    }
#else
    (void)database;
#endif
}

void release_runtime(void) {
    std::filesystem::path runtime_dir;
    {
        const std::lock_guard<std::mutex> guard(g_runtime.mutex);
        if (g_runtime.ref_count == 0U) {
            return;
        }

        --g_runtime.ref_count;
        if (g_runtime.ref_count > 0U) {
            return;
        }

        runtime_dir = g_runtime.directory;
#if MYLITE_WITH_MARIADB_EMBEDDED
        mysql_server_end();
#endif
        g_runtime.directory.clear();
        g_runtime.filename.clear();
        g_runtime.argv.clear();
        g_runtime.arguments.clear();
    }

    remove_directory_if_present(runtime_dir);
}

#if MYLITE_WITH_MARIADB_EMBEDDED
std::filesystem::path normalize_filename(const char *filename) {
    if (std::strcmp(filename, ":memory:") == 0) {
        return std::filesystem::path(":memory:");
    }
    return std::filesystem::absolute(std::filesystem::path(filename));
}

std::filesystem::path create_runtime_directory(const mylite_open_config *config) {
    std::filesystem::path root = runtime_root(config);
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        throw std::filesystem::filesystem_error("create runtime root", root, error);
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        std::filesystem::path candidate = root / unique_runtime_name();
        if (std::filesystem::create_directory(candidate, error)) {
            std::filesystem::create_directories(candidate / "data");
            std::filesystem::create_directories(candidate / "tmp");
            std::filesystem::create_directories(candidate / "plugins");
            return candidate;
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

std::vector<std::string> runtime_arguments(const std::filesystem::path &runtime_dir) {
    const std::filesystem::path data_dir = runtime_dir / "data";
    const std::filesystem::path tmp_dir = runtime_dir / "tmp";
    const std::filesystem::path plugin_dir = runtime_dir / "plugins";

    return {
        "mylite",
        "--no-defaults",
        "--datadir=" + data_dir.string(),
        "--tmpdir=" + tmp_dir.string(),
        "--plugin-dir=" + plugin_dir.string(),
        "--skip-grant-tables",
        "--skip-networking",
        "--default-storage-engine=MyISAM",
        "--innodb=OFF",
        "--lc-messages-dir=" MYLITE_MARIADB_MESSAGES_DIR,
        "--character-sets-dir=" MYLITE_MARIADB_CHARSETS_DIR,
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

int copy_error_message(mylite_db &database, char **errmsg) {
    if (errmsg == nullptr) {
        return database.errcode;
    }

    const std::size_t length = database.errmsg.size();
    char *copy = static_cast<char *>(std::malloc(length + 1U));
    if (copy == nullptr) {
        return MYLITE_NOMEM;
    }

    std::memcpy(copy, database.errmsg.c_str(), length + 1U);
    *errmsg = copy;
    return database.errcode;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_ok(mylite_db &database) {
    database.errcode = MYLITE_OK;
    database.extended_errcode = MYLITE_OK;
    database.mariadb_errno = 0;
    database.sqlstate = k_sqlstate_ok;
    database.errmsg = k_not_an_error;
}
#endif

void set_error(mylite_db &database, int code, const char *message) {
    database.errcode = code;
    database.extended_errcode = code;
    database.mariadb_errno = 0;
    database.sqlstate = k_sqlstate_general;
    database.errmsg = message;
}

#if MYLITE_WITH_MARIADB_EMBEDDED
void set_mariadb_error(mylite_db &database) {
    database.errcode = MYLITE_ERROR;
    database.extended_errcode = MYLITE_ERROR;
    database.mariadb_errno = mysql_errno(&database.mysql);
    database.sqlstate = mysql_sqlstate(&database.mysql);
    database.errmsg = mysql_error(&database.mysql);
}
#endif

const char *safe_c_str(const std::string &value) {
    return value.empty() ? "" : value.c_str();
}

bool has_config_field(const mylite_open_config *config, std::size_t field_end) {
    return config != nullptr && config->size >= field_end;
}

} // namespace
