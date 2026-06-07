#include <mylite/mylite.h>

#include <errno.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MYLITE_PERF_REMOVE_TREE_MAX_FDS 32
#define MYLITE_PERF_DEFAULT_OPEN_CLOSE_ITERATIONS 5U
#define MYLITE_PERF_DEFAULT_SELECT_ITERATIONS 1000U
#define MYLITE_PERF_DEFAULT_INSERT_ITERATIONS 200U

typedef struct performance_paths {
    char *root;
    char *runtime_root;
    char *database_path;
} performance_paths;

static performance_paths make_performance_paths(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *statbuf,
    int typeflag,
    struct FTW *ftwbuf
);
static mylite_open_config open_config(const char *temp_directory);
static unsigned env_unsigned(const char *name, unsigned fallback);
static int env_double(const char *name, double *out_value);
static uint64_t monotonic_ns(void);
static double elapsed_seconds(uint64_t start_ns, uint64_t end_ns);
static void emit_ms(const char *name, double seconds, unsigned iterations);
static void emit_rate(const char *name, unsigned iterations, double seconds);
static void check_max_ms(const char *env_name, double seconds, unsigned iterations);
static void check_min_rate(const char *env_name, double rate);
static mylite_db *open_database(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config
);
static void close_database(mylite_db *db);
static void exec_ok(mylite_db *db, const char *sql);
static double measure_open_close(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
);
static double measure_direct_select(mylite_db *db, unsigned iterations);
static double measure_prepared_select(mylite_db *db, unsigned iterations);
static double measure_transactional_insert(mylite_db *db, const char *table_name, unsigned rows);

int main(void) {
    performance_paths paths = make_performance_paths();
    mylite_open_config config = open_config(paths.runtime_root);
    const unsigned open_close_iterations = env_unsigned(
        "MYLITE_PERF_OPEN_CLOSE_ITERATIONS",
        MYLITE_PERF_DEFAULT_OPEN_CLOSE_ITERATIONS
    );
    const unsigned select_iterations =
        env_unsigned("MYLITE_PERF_SELECT_ITERATIONS", MYLITE_PERF_DEFAULT_SELECT_ITERATIONS);
    const unsigned insert_iterations =
        env_unsigned("MYLITE_PERF_INSERT_ITERATIONS", MYLITE_PERF_DEFAULT_INSERT_ITERATIONS);
    const unsigned ordinary_flags = MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE;
    const unsigned ownerless_flags =
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE | MYLITE_OPEN_OWNERLESS_RW;
    mylite_db *db;
    uint64_t start_ns;
    uint64_t end_ns;
    double seconds;
    double rate;

    printf("mylite_perf_open_close_iterations=%u\n", open_close_iterations);
    printf("mylite_perf_select_iterations=%u\n", select_iterations);
    printf("mylite_perf_insert_iterations=%u\n", insert_iterations);
    printf("mylite_perf_database_path=%s\n", paths.database_path);

    start_ns = monotonic_ns();
    db = open_database(&paths, ordinary_flags, &config);
    exec_ok(db, "CREATE DATABASE IF NOT EXISTS app");
    exec_ok(db, "CREATE TABLE app.mylite_perf_warmup (id INT PRIMARY KEY) ENGINE=InnoDB");
    close_database(db);
    end_ns = monotonic_ns();
    seconds = elapsed_seconds(start_ns, end_ns);
    emit_ms("mylite_perf_ordinary_cold_create_open_close", seconds, 1U);

    seconds = measure_open_close(&paths, ordinary_flags, &config, open_close_iterations);
    emit_ms("mylite_perf_ordinary_warm_open_close", seconds, open_close_iterations);
    check_max_ms("MYLITE_PERF_MAX_ORDINARY_WARM_OPEN_CLOSE_MS", seconds, open_close_iterations);

    seconds = measure_open_close(&paths, ownerless_flags, &config, open_close_iterations);
    emit_ms("mylite_perf_ownerless_warm_open_close", seconds, open_close_iterations);
    check_max_ms("MYLITE_PERF_MAX_OWNERLESS_WARM_OPEN_CLOSE_MS", seconds, open_close_iterations);

    db = open_database(&paths, ordinary_flags, &config);
    seconds = measure_direct_select(db, select_iterations);
    emit_rate("mylite_perf_ordinary_direct_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_DIRECT_SELECT1_OPS", rate);

    seconds = measure_prepared_select(db, select_iterations);
    emit_rate("mylite_perf_ordinary_prepared_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_PREPARED_SELECT1_OPS", rate);

    seconds = measure_transactional_insert(db, "mylite_perf_ordinary_insert", insert_iterations);
    emit_rate("mylite_perf_ordinary_insert_txn", insert_iterations, seconds);
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_ORDINARY_INSERT_TXN_OPS", rate);
    close_database(db);

    db = open_database(&paths, ownerless_flags, &config);
    seconds = measure_direct_select(db, select_iterations);
    emit_rate("mylite_perf_ownerless_direct_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_DIRECT_SELECT1_OPS", rate);

    seconds = measure_prepared_select(db, select_iterations);
    emit_rate("mylite_perf_ownerless_prepared_select1", select_iterations, seconds);
    rate = (double)select_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_PREPARED_SELECT1_OPS", rate);

    seconds = measure_transactional_insert(db, "mylite_perf_ownerless_insert", insert_iterations);
    emit_rate("mylite_perf_ownerless_insert_txn", insert_iterations, seconds);
    rate = (double)insert_iterations / (seconds > 0.000001 ? seconds : 0.000001);
    check_min_rate("MYLITE_PERF_MIN_OWNERLESS_INSERT_TXN_OPS", rate);
    close_database(db);

    remove_tree(paths.root);
    free(paths.database_path);
    free(paths.runtime_root);
    free(paths.root);
    return 0;
}

static performance_paths make_performance_paths(void) {
    const char *tmp = getenv("TMPDIR");
    char *template_path;
    performance_paths paths;
    size_t tmp_len;
    const char suffix[] = "/mylite-perf-probe.XXXXXX";

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    tmp_len = strlen(tmp);
    template_path = (char *)malloc(tmp_len + sizeof(suffix));
    if (template_path == NULL) {
        fprintf(stderr, "failed to allocate performance path\n");
        exit(1);
    }
    memcpy(template_path, tmp, tmp_len);
    memcpy(template_path + tmp_len, suffix, sizeof(suffix));
    if (mkdtemp(template_path) == NULL) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        free(template_path);
        exit(1);
    }

    paths.root = template_path;
    paths.runtime_root = path_join(paths.root, "runtime");
    paths.database_path = path_join(paths.root, "startup-performance.mylite");
    if (mkdir(paths.runtime_root, 0700) != 0) {
        fprintf(stderr, "mkdir %s failed: %s\n", paths.runtime_root, strerror(errno));
        remove_tree(paths.root);
        free(paths.database_path);
        free(paths.runtime_root);
        free(paths.root);
        exit(1);
    }
    return paths;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = (char *)malloc(directory_len + name_len + 2U);
    if (path == NULL) {
        fprintf(stderr, "failed to allocate path\n");
        exit(1);
    }
    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static void remove_tree(const char *path) {
    if (path == NULL) {
        return;
    }
    if (nftw(path, remove_tree_entry, MYLITE_PERF_REMOVE_TREE_MAX_FDS, FTW_DEPTH | FTW_PHYS) != 0) {
        fprintf(stderr, "warning: failed to remove %s: %s\n", path, strerror(errno));
    }
}

static int remove_tree_entry(
    const char *path,
    const struct stat *statbuf,
    int typeflag,
    struct FTW *ftwbuf
) {
    (void)statbuf;
    (void)typeflag;
    (void)ftwbuf;
    if (remove(path) != 0) {
        fprintf(stderr, "warning: remove %s failed: %s\n", path, strerror(errno));
    }
    return 0;
}

static mylite_open_config open_config(const char *temp_directory) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = temp_directory,
    };
    return config;
}

static unsigned env_unsigned(const char *name, unsigned fallback) {
    char *end = NULL;
    const char *value = getenv(name);
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        fprintf(stderr, "%s must be a positive integer\n", name);
        exit(1);
    }
    if (parsed > 10000000UL) {
        fprintf(stderr, "%s is unreasonably large\n", name);
        exit(1);
    }
    return (unsigned)parsed;
}

static int env_double(const char *name, double *out_value) {
    char *end = NULL;
    const char *value = getenv(name);
    double parsed;

    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    errno = 0;
    parsed = strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0.0) {
        fprintf(stderr, "%s must be a positive number\n", name);
        exit(1);
    }
    *out_value = parsed;
    return 1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime failed: %s\n", strerror(errno));
        exit(1);
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static double elapsed_seconds(uint64_t start_ns, uint64_t end_ns) {
    return (double)(end_ns - start_ns) / 1000000000.0;
}

static void emit_ms(const char *name, double seconds, unsigned iterations) {
    const double average_ms = (seconds * 1000.0) / (double)iterations;
    printf("%s_ms_avg=%.3f\n", name, average_ms);
    printf("%s_seconds_total=%.6f\n", name, seconds);
}

static void emit_rate(const char *name, unsigned iterations, double seconds) {
    const double divisor = seconds > 0.000001 ? seconds : 0.000001;
    printf("%s_iterations=%u\n", name, iterations);
    printf("%s_seconds=%.6f\n", name, seconds);
    printf("%s_ops_per_second=%.2f\n", name, (double)iterations / divisor);
}

static void check_max_ms(const char *env_name, double seconds, unsigned iterations) {
    double threshold_ms;
    const double average_ms = (seconds * 1000.0) / (double)iterations;

    if (!env_double(env_name, &threshold_ms)) {
        return;
    }
    if (average_ms > threshold_ms) {
        fprintf(
            stderr,
            "%s exceeded: %.3fms average > %.3fms\n",
            env_name,
            average_ms,
            threshold_ms
        );
        exit(1);
    }
}

static void check_min_rate(const char *env_name, double rate) {
    double threshold_rate;

    if (!env_double(env_name, &threshold_rate)) {
        return;
    }
    if (rate < threshold_rate) {
        fprintf(stderr, "%s missed: %.2f ops/s < %.2f ops/s\n", env_name, rate, threshold_rate);
        exit(1);
    }
}

static mylite_db *open_database(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config
) {
    mylite_db *db = NULL;
    const int result = mylite_open(paths->database_path, &db, flags, config);
    if (result != MYLITE_OK) {
        fprintf(stderr, "mylite_open failed: result=%d\n", result);
        exit(1);
    }
    return db;
}

static void close_database(mylite_db *db) {
    const int result = mylite_close(db);
    if (result != MYLITE_OK) {
        fprintf(stderr, "mylite_close failed: result=%d\n", result);
        exit(1);
    }
}

static void exec_ok(mylite_db *db, const char *sql) {
    const int result = mylite_exec(db, sql, NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        fprintf(
            stderr,
            "SQL failed: %s\nresult=%d mylite_err=%d mariadb_errno=%u sqlstate=%s message=%s\n",
            sql,
            result,
            mylite_errcode(db),
            mylite_mariadb_errno(db),
            mylite_sqlstate(db),
            mylite_errmsg(db)
        );
        exit(1);
    }
}

static double measure_open_close(
    const performance_paths *paths,
    unsigned flags,
    const mylite_open_config *config,
    unsigned iterations
) {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        mylite_db *db = open_database(paths, flags, config);
        close_database(db);
    }
    end_ns = monotonic_ns();
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_direct_select(mylite_db *db, unsigned iterations) {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        exec_ok(db, "SELECT 1");
    }
    end_ns = monotonic_ns();
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_prepared_select(mylite_db *db, unsigned iterations) {
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    if (mylite_prepare(db, "SELECT 1", MYLITE_NUL_TERMINATED, &stmt, &tail) != MYLITE_OK) {
        fprintf(stderr, "prepare SELECT 1 failed: %s\n", mylite_errmsg(db));
        exit(1);
    }
    if (tail == NULL || tail[0] != '\0') {
        fprintf(stderr, "prepare SELECT 1 left unexpected tail\n");
        exit(1);
    }

    start_ns = monotonic_ns();
    for (index = 0; index < iterations; ++index) {
        if (mylite_step(stmt) != MYLITE_ROW) {
            fprintf(stderr, "prepared SELECT 1 did not return a row\n");
            exit(1);
        }
        if (mylite_column_int64(stmt, 0) != 1) {
            fprintf(stderr, "prepared SELECT 1 returned an unexpected value\n");
            exit(1);
        }
        if (mylite_step(stmt) != MYLITE_DONE) {
            fprintf(stderr, "prepared SELECT 1 did not finish\n");
            exit(1);
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            fprintf(stderr, "prepared SELECT 1 reset failed\n");
            exit(1);
        }
    }
    end_ns = monotonic_ns();
    if (mylite_finalize(stmt) != MYLITE_OK) {
        fprintf(stderr, "finalize SELECT 1 failed\n");
        exit(1);
    }
    return elapsed_seconds(start_ns, end_ns);
}

static double measure_transactional_insert(mylite_db *db, const char *table_name, unsigned rows) {
    char sql[256];
    const char *tail = NULL;
    mylite_stmt *stmt = NULL;
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned index;

    (void)snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS app.%s", table_name);
    exec_ok(db, sql);
    (void)snprintf(
        sql,
        sizeof(sql),
        "CREATE TABLE app.%s (id INT PRIMARY KEY, value VARCHAR(32) NOT NULL) ENGINE=InnoDB",
        table_name
    );
    exec_ok(db, sql);
    (void)snprintf(sql, sizeof(sql), "INSERT INTO app.%s (id, value) VALUES (?, ?)", table_name);
    if (mylite_prepare(db, sql, MYLITE_NUL_TERMINATED, &stmt, &tail) != MYLITE_OK) {
        fprintf(stderr, "prepare insert failed: %s\n", mylite_errmsg(db));
        exit(1);
    }
    if (tail == NULL || tail[0] != '\0') {
        fprintf(stderr, "prepare insert left unexpected tail\n");
        exit(1);
    }
    exec_ok(db, "START TRANSACTION");
    start_ns = monotonic_ns();
    for (index = 1U; index <= rows; ++index) {
        if (mylite_bind_int64(stmt, 1U, (long long)index) != MYLITE_OK ||
            mylite_bind_text(stmt, 2U, "mylite-perf", MYLITE_NUL_TERMINATED, MYLITE_STATIC) !=
                MYLITE_OK) {
            fprintf(stderr, "insert bind failed\n");
            exit(1);
        }
        if (mylite_step(stmt) != MYLITE_DONE) {
            fprintf(stderr, "insert step failed\n");
            exit(1);
        }
        if (mylite_reset(stmt) != MYLITE_OK || mylite_clear_bindings(stmt) != MYLITE_OK) {
            fprintf(stderr, "insert reset failed\n");
            exit(1);
        }
    }
    exec_ok(db, "COMMIT");
    end_ns = monotonic_ns();
    if (mylite_finalize(stmt) != MYLITE_OK) {
        fprintf(stderr, "finalize insert failed\n");
        exit(1);
    }
    return elapsed_seconds(start_ns, end_ns);
}
