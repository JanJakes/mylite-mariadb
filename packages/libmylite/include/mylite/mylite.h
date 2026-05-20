#ifndef MYLITE_MYLITE_H
#define MYLITE_MYLITE_H

#include <mylite/version.h>

#include <stddef.h>

#ifndef MYLITE_API
#  ifdef _WIN32
#    if defined(MYLITE_BUILDING_SHARED_LIBRARY)
#      define MYLITE_API __declspec(dllexport)
#    elif defined(MYLITE_USING_SHARED_LIBRARY)
#      define MYLITE_API __declspec(dllimport)
#    else
#      define MYLITE_API
#    endif
#  elif defined(__GNUC__) || defined(__clang__)
#    define MYLITE_API __attribute__((visibility("default")))
#  else
#    define MYLITE_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mylite_db mylite_db;
typedef struct mylite_stmt mylite_stmt;

typedef enum mylite_result {
    MYLITE_OK = 0,
    MYLITE_ERROR = 1,
    MYLITE_BUSY = 5,
    MYLITE_NOMEM = 7,
    MYLITE_READONLY = 8,
    MYLITE_IOERR = 10,
    MYLITE_CORRUPT = 11,
    MYLITE_NOTFOUND = 12,
    MYLITE_FULL = 13,
    MYLITE_CONSTRAINT = 19,
    MYLITE_MISUSE = 21,
    MYLITE_ROW = 100,
    MYLITE_DONE = 101
} mylite_result;

typedef struct mylite_open_config {
    size_t size;
    int profile;
    unsigned busy_timeout_ms;
    int durability;
    const char *temp_directory;
} mylite_open_config;

typedef void (*mylite_destructor)(void *);

typedef enum mylite_value_type {
    MYLITE_TYPE_NULL = 0,
    MYLITE_TYPE_INT64 = 1,
    MYLITE_TYPE_UINT64 = 2,
    MYLITE_TYPE_DOUBLE = 3,
    MYLITE_TYPE_TEXT = 4,
    MYLITE_TYPE_BLOB = 5
} mylite_value_type;

typedef enum mylite_warning_level {
    MYLITE_WARNING_NOTE = 1,
    MYLITE_WARNING_WARNING = 2,
    MYLITE_WARNING_ERROR = 3
} mylite_warning_level;

#define MYLITE_OPEN_READONLY 0x00000001U
#define MYLITE_OPEN_READWRITE 0x00000002U
#define MYLITE_OPEN_CREATE 0x00000004U
#define MYLITE_OPEN_EXCLUSIVE 0x00000008U
#define MYLITE_OPEN_URI 0x00000010U

#define MYLITE_PROFILE_DEFAULT 0
#define MYLITE_PROFILE_STRICT 1
#define MYLITE_PROFILE_COMPAT 2

#define MYLITE_DURABILITY_OFF 0
#define MYLITE_DURABILITY_NORMAL 1
#define MYLITE_DURABILITY_FULL 2

#define MYLITE_NUL_TERMINATED ((size_t)-1)
#define MYLITE_STATIC ((mylite_destructor)0)
#define MYLITE_TRANSIENT ((mylite_destructor)(-1))

typedef int (*mylite_exec_callback)(
    void *ctx,
    int column_count,
    char **values,
    char **column_names
);

MYLITE_API const char *mylite_version(void);
MYLITE_API int mylite_open(
    const char *path,
    mylite_db **out_db,
    unsigned flags,
    const mylite_open_config *config
);
MYLITE_API int mylite_close(mylite_db *db);
MYLITE_API int mylite_exec(
    mylite_db *db,
    const char *sql,
    mylite_exec_callback callback,
    void *ctx,
    char **errmsg
);
MYLITE_API int mylite_prepare(
    mylite_db *db,
    const char *sql,
    size_t sql_len,
    mylite_stmt **out_stmt,
    const char **tail
);
MYLITE_API int mylite_step(mylite_stmt *stmt);
MYLITE_API int mylite_reset(mylite_stmt *stmt);
MYLITE_API int mylite_finalize(mylite_stmt *stmt);
MYLITE_API unsigned mylite_bind_parameter_count(mylite_stmt *stmt);
MYLITE_API int mylite_clear_bindings(mylite_stmt *stmt);
MYLITE_API int mylite_bind_null(mylite_stmt *stmt, unsigned index);
MYLITE_API int mylite_bind_int64(mylite_stmt *stmt, unsigned index, long long value);
MYLITE_API int mylite_bind_uint64(mylite_stmt *stmt, unsigned index, unsigned long long value);
MYLITE_API int mylite_bind_double(mylite_stmt *stmt, unsigned index, double value);
MYLITE_API int mylite_bind_text(
    mylite_stmt *stmt,
    unsigned index,
    const char *value,
    size_t value_len,
    mylite_destructor destructor
);
MYLITE_API int mylite_bind_blob(
    mylite_stmt *stmt,
    unsigned index,
    const void *value,
    size_t value_len,
    mylite_destructor destructor
);
MYLITE_API unsigned mylite_column_count(mylite_stmt *stmt);
MYLITE_API const char *mylite_column_name(mylite_stmt *stmt, unsigned column);
MYLITE_API mylite_value_type mylite_column_type(mylite_stmt *stmt, unsigned column);
MYLITE_API long long mylite_column_int64(mylite_stmt *stmt, unsigned column);
MYLITE_API unsigned long long mylite_column_uint64(mylite_stmt *stmt, unsigned column);
MYLITE_API double mylite_column_double(mylite_stmt *stmt, unsigned column);
MYLITE_API const char *mylite_column_text(mylite_stmt *stmt, unsigned column);
MYLITE_API const void *mylite_column_blob(mylite_stmt *stmt, unsigned column);
MYLITE_API size_t mylite_column_bytes(mylite_stmt *stmt, unsigned column);

MYLITE_API int mylite_errcode(mylite_db *db);
MYLITE_API int mylite_extended_errcode(mylite_db *db);
MYLITE_API unsigned mylite_mariadb_errno(mylite_db *db);
MYLITE_API const char *mylite_sqlstate(mylite_db *db);
MYLITE_API const char *mylite_errmsg(mylite_db *db);
MYLITE_API unsigned mylite_warning_count(mylite_db *db);
MYLITE_API int mylite_warning(
    mylite_db *db,
    unsigned index,
    mylite_warning_level *level,
    unsigned *code,
    const char **message
);
MYLITE_API long long mylite_changes(mylite_db *db);
MYLITE_API unsigned long long mylite_last_insert_id(mylite_db *db);
MYLITE_API void mylite_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
