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

MYLITE_API int mylite_errcode(mylite_db *db);
MYLITE_API int mylite_extended_errcode(mylite_db *db);
MYLITE_API unsigned mylite_mariadb_errno(mylite_db *db);
MYLITE_API const char *mylite_sqlstate(mylite_db *db);
MYLITE_API const char *mylite_errmsg(mylite_db *db);
MYLITE_API long long mylite_changes(mylite_db *db);
MYLITE_API unsigned long long mylite_last_insert_id(mylite_db *db);
MYLITE_API void mylite_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
