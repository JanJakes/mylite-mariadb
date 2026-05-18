#include <mylite/mylite.h>

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct benchmark_config {
    size_t rows;
    size_t iterations;
} benchmark_config;

typedef struct benchmark_context {
    const benchmark_config *config;
    const char *root;
    char *filename;
    mylite_db *db;
} benchmark_context;

typedef struct scalar_result {
    unsigned long long value;
    int rows;
} scalar_result;

typedef struct scan_result {
    size_t rows;
    uint64_t checksum;
} scan_result;

static int parse_config(int argc, char **argv, benchmark_config *config);
static int run_benchmark(const benchmark_config *config);
static void print_usage(const char *program);
static void print_result(const char *operation, size_t count, uint64_t elapsed_ns);
static int setup_database(benchmark_context *ctx);
static int benchmark_insert_rows(benchmark_context *ctx);
static int benchmark_point_selects(benchmark_context *ctx);
static int benchmark_prepared_point_selects(benchmark_context *ctx);
static int benchmark_updates(benchmark_context *ctx);
static int benchmark_prepared_updates(benchmark_context *ctx);
static int benchmark_ordered_scan(benchmark_context *ctx);
static int verify_row_count(benchmark_context *ctx, size_t expected);
static int exec_sql(benchmark_context *ctx, const char *sql);
static int query_uint64(benchmark_context *ctx, const char *sql, unsigned long long *out_value);
static int scalar_callback(void *data, int column_count, char **values, char **column_names);
static int scan_callback(void *data, int column_count, char **values, char **column_names);
static void report_database_error(benchmark_context *ctx, const char *operation);
static uint64_t monotonic_ns(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

int main(int argc, char **argv) {
    benchmark_config config = {
        .rows = 100U,
        .iterations = 100U,
    };

    if (parse_config(argc, argv, &config) != 0) {
        return 2;
    }

    return run_benchmark(&config);
}

static int parse_config(int argc, char **argv, benchmark_config *config) {
    if (argc > 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        exit(0);
    }

    for (int i = 1; i < argc; ++i) {
        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(argv[i], &end, 10);
        if (errno != 0 || end == argv[i] || *end != '\0' || value == 0U || value > 1000000U) {
            fprintf(stderr, "Expected a positive integer up to 1000000, got: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }

        if (i == 1) {
            config->rows = (size_t)value;
        } else {
            config->iterations = (size_t)value;
        }
    }

    return 0;
}

static int run_benchmark(const benchmark_config *config) {
    benchmark_context ctx = {
        .config = config,
        .root = NULL,
        .filename = NULL,
        .db = NULL,
    };
    int result = 1;
    uint64_t start_ns;

    ctx.root = make_temp_root();
    if (ctx.root == NULL) {
        return 1;
    }

    printf("# MyLite Performance Baseline\n\n");
    printf("Rows: %zu\n", config->rows);
    printf("Iterations: %zu\n", config->iterations);
    printf("Storage route: `ENGINE=InnoDB` through the MyLite storage engine\n\n");
    printf("| Operation | Count | Total ms | us/op |\n");
    printf("| --- | ---: | ---: | ---: |\n");

    start_ns = monotonic_ns();
    if (setup_database(&ctx) != 0) {
        goto cleanup;
    }
    print_result("open and schema setup", 1U, monotonic_ns() - start_ns);

    if (benchmark_insert_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (verify_row_count(&ctx, config->rows) != 0) {
        goto cleanup;
    }
    if (benchmark_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_updates(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_updates(&ctx) != 0) {
        goto cleanup;
    }
    if (verify_row_count(&ctx, config->rows) != 0) {
        goto cleanup;
    }
    if (benchmark_ordered_scan(&ctx) != 0) {
        goto cleanup;
    }

    result = 0;

cleanup:
    if (ctx.db != NULL && mylite_close(ctx.db) != MYLITE_OK) {
        fprintf(stderr, "Failed to close database: %s\n", mylite_errmsg(ctx.db));
        result = 1;
    }
    free(ctx.filename);
    remove_tree(ctx.root);
    free((void *)ctx.root);
    return result;
}

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [rows] [iterations]\n"
        "\n"
        "Defaults: rows=100 iterations=100.\n",
        program
    );
}

static void print_result(const char *operation, size_t count, uint64_t elapsed_ns) {
    const double total_ms = (double)elapsed_ns / 1000000.0;
    const double micros_per_operation =
        count == 0U ? 0.0 : (double)elapsed_ns / (double)count / 1000.0;

    printf("| %s | %zu | %.3f | %.3f |\n", operation, count, total_ms, micros_per_operation);
}

static int setup_database(benchmark_context *ctx) {
    char *runtime_root = path_join(ctx->root, "runtime");
    if (runtime_root == NULL) {
        return 1;
    }
    if (mkdir(runtime_root, 0700) != 0) {
        fprintf(
            stderr,
            "Failed to create runtime directory %s: %s\n",
            runtime_root,
            strerror(errno)
        );
        free(runtime_root);
        return 1;
    }

    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    ctx->filename = path_join(ctx->root, "perf.mylite");
    if (ctx->filename == NULL) {
        free(runtime_root);
        return 1;
    }

    const int open_result = mylite_open_v2(
        ctx->filename,
        &ctx->db,
        MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
        &config
    );
    free(runtime_root);
    if (open_result != MYLITE_OK) {
        report_database_error(ctx, "open database");
        return 1;
    }

    if (exec_sql(ctx, "CREATE DATABASE perf") != 0 || exec_sql(ctx, "USE perf") != 0) {
        return 1;
    }

    return exec_sql(
        ctx,
        "CREATE TABLE perf_rows ("
        "id INT NOT NULL PRIMARY KEY,"
        "value INT NOT NULL,"
        "pad VARCHAR(64) NOT NULL,"
        "KEY value_key (value)"
        ") ENGINE=InnoDB"
    );
}

static int benchmark_insert_rows(benchmark_context *ctx) {
    uint64_t start_ns;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char sql[160];
        const int written = snprintf(
            sql,
            sizeof(sql),
            "INSERT INTO perf_rows (id, value, pad) VALUES (%zu, %zu, 'row-%zu')",
            i + 1U,
            i + 1U,
            i + 1U
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    print_result("direct inserts in one transaction", ctx->config->rows, monotonic_ns() - start_ns);

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_point_selects(benchmark_context *ctx) {
    uint64_t checksum = 0U;
    const uint64_t start_ns = monotonic_ns();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        char sql[80];
        unsigned long long value = 0U;
        const int written =
            snprintf(sql, sizeof(sql), "SELECT value FROM perf_rows WHERE id = %zu", id);
        if (written < 0 || (size_t)written >= sizeof(sql) || query_uint64(ctx, sql, &value) != 0) {
            return 1;
        }
        checksum += (uint64_t)value;
    }

    print_result(
        "direct primary-key point selects",
        ctx->config->iterations,
        monotonic_ns() - start_ns
    );
    printf("Point-select checksum: %" PRIu64 "\n", checksum);
    return 0;
}

static int benchmark_prepared_point_selects(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point select");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(stderr, "Prepared primary-key point select returned no row for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key point select");
            goto cleanup;
        }
        if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64) {
            fprintf(stderr, "Prepared primary-key point select returned a non-integer value\n");
            goto cleanup;
        }
        checksum += (uint64_t)mylite_column_int64(stmt, 0U);

        const int done_result = mylite_step(stmt);
        if (done_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared primary-key point select returned extra rows for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select completion");
            goto cleanup;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select");
            goto cleanup;
        }
    }

    print_result(
        "prepared primary-key point selects",
        ctx->config->iterations,
        monotonic_ns() - start_ns
    );
    printf("Prepared point-select checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select");
        return 1;
    }
    return result;
}

static int benchmark_updates(benchmark_context *ctx) {
    const uint64_t start_ns = monotonic_ns();
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        char sql[96];
        const int written =
            snprintf(sql, sizeof(sql), "UPDATE perf_rows SET value = value + 1 WHERE id = %zu", id);
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    print_result(
        "direct primary-key updates in one transaction",
        ctx->config->iterations,
        monotonic_ns() - start_ns
    );

    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_prepared_updates(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "UPDATE perf_rows SET value = value + 1 WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key update");
        goto rollback;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key update");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared primary-key update failed for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key update");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key update");
            goto rollback;
        }
    }
    print_result(
        "prepared primary-key updates in one transaction",
        ctx->config->iterations,
        monotonic_ns() - start_ns
    );

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared primary-key update");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key update");
        result = 1;
    }
    if (result != 0) {
        (void)mylite_exec(ctx->db, "ROLLBACK", NULL, NULL, NULL);
    }
    return result;
}

static int benchmark_ordered_scan(benchmark_context *ctx) {
    scan_result scan = {
        .rows = 0U,
        .checksum = 0U,
    };
    const uint64_t start_ns = monotonic_ns();

    if (mylite_exec(
            ctx->db,
            "SELECT id, value, pad FROM perf_rows ORDER BY id",
            scan_callback,
            &scan,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "ordered scan");
        return 1;
    }

    print_result("direct ordered full scan", scan.rows, monotonic_ns() - start_ns);
    printf("Scan checksum: %" PRIu64 "\n", scan.checksum);
    if (scan.rows != ctx->config->rows) {
        fprintf(stderr, "Expected %zu scan rows, got %zu\n", ctx->config->rows, scan.rows);
        return 1;
    }
    return 0;
}

static int verify_row_count(benchmark_context *ctx, size_t expected) {
    unsigned long long count = 0U;
    if (query_uint64(ctx, "SELECT COUNT(*) FROM perf_rows", &count) != 0) {
        return 1;
    }
    if (count != (unsigned long long)expected) {
        fprintf(stderr, "Expected %zu rows, got %llu\n", expected, count);
        return 1;
    }
    return 0;
}

static int exec_sql(benchmark_context *ctx, const char *sql) {
    const int result = mylite_exec(ctx->db, sql, NULL, NULL, NULL);
    if (result != MYLITE_OK) {
        report_database_error(ctx, sql);
        return 1;
    }
    return 0;
}

static int query_uint64(benchmark_context *ctx, const char *sql, unsigned long long *out_value) {
    scalar_result result = {
        .value = 0U,
        .rows = 0,
    };

    if (mylite_exec(ctx->db, sql, scalar_callback, &result, NULL) != MYLITE_OK) {
        report_database_error(ctx, sql);
        return 1;
    }
    if (result.rows != 1) {
        fprintf(stderr, "Expected one scalar row from %s, got %d\n", sql, result.rows);
        return 1;
    }
    *out_value = result.value;
    return 0;
}

static int scalar_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    scalar_result *result = data;
    if (column_count != 1 || values[0] == NULL) {
        return 1;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(values[0], &end, 10);
    if (errno != 0 || end == values[0] || *end != '\0') {
        return 1;
    }
    result->value = value;
    ++result->rows;
    return 0;
}

static int scan_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    scan_result *result = data;
    if (column_count != 3 || values[0] == NULL || values[1] == NULL || values[2] == NULL) {
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(values[i], &end, 10);
        if (errno != 0 || end == values[i] || *end != '\0') {
            return 1;
        }
        result->checksum += (uint64_t)value;
    }
    result->checksum += (uint64_t)strlen(values[2]);
    ++result->rows;
    return 0;
}

static void report_database_error(benchmark_context *ctx, const char *operation) {
    if (ctx->db == NULL) {
        fprintf(stderr, "%s failed before database handle was available\n", operation);
        return;
    }
    fprintf(stderr, "%s failed: %s\n", operation, mylite_errmsg(ctx->db));
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-perf-baseline.XXXXXX";
    char *root = mkdtemp(template_path);
    if (root == NULL) {
        fprintf(stderr, "Failed to create temporary directory: %s\n", strerror(errno));
        return NULL;
    }

    char *copy = malloc(strlen(root) + 1U);
    if (copy == NULL) {
        fprintf(stderr, "Out of memory\n");
        remove_tree(root);
        return NULL;
    }
    strcpy(copy, root);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = malloc(directory_len + 1U + name_len + 1U);
    if (path == NULL) {
        fprintf(stderr, "Out of memory\n");
        return NULL;
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

    DIR *dir = opendir(path);
    if (dir == NULL) {
        unlink(path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char *child = path_join(path, entry->d_name);
        if (child != NULL) {
            remove_tree_entry(child);
            free(child);
        }
    }
    closedir(dir);
    rmdir(path);
}

static void remove_tree_entry(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        remove_tree(path);
        return;
    }
    unlink(path);
}
