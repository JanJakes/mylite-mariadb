#include <mylite/mylite.h>
#include <mylite/storage.h>

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

typedef enum benchmark_phase {
    BENCHMARK_PHASE_ALL,
    BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS,
    BENCHMARK_PHASE_POINT_SELECTS,
    BENCHMARK_PHASE_DIRECT_PK_SELECTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS,
    BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW,
    BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS,
    BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
    BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS,
    BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
    BENCHMARK_PHASE_STORAGE_READ_STATEMENTS,
    BENCHMARK_PHASE_STORAGE_ROW_UPDATES,
    BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS,
    BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS,
    BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS,
    BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS,
    BENCHMARK_PHASE_UPDATES,
    BENCHMARK_PHASE_DIRECT_UPDATES,
    BENCHMARK_PHASE_PREPARED_UPDATES,
    BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS,
} benchmark_phase;

typedef enum benchmark_metric {
    BENCHMARK_METRIC_OPEN_SETUP,
    BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS,
    BENCHMARK_METRIC_DIRECT_INSERTS,
    BENCHMARK_METRIC_PREPARED_INSERTS,
    BENCHMARK_METRIC_DIRECT_PK_SELECTS,
    BENCHMARK_METRIC_PREPARED_PK_SELECTS,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET,
    BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW,
    BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS,
    BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
    BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS,
    BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
    BENCHMARK_METRIC_STORAGE_READ_STATEMENTS,
    BENCHMARK_METRIC_STORAGE_ROW_UPDATES,
    BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARE_LEAF_ROWS,
    BENCHMARK_METRIC_PUBLISH_LEAF_INDEX,
    BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS,
    BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS,
    BENCHMARK_METRIC_DIRECT_UPDATES,
    BENCHMARK_METRIC_PREPARED_UPDATES,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP,
    BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET,
    BENCHMARK_METRIC_ORDERED_SCAN,
    BENCHMARK_METRIC_COUNT,
} benchmark_metric;

typedef struct benchmark_metric_definition {
    benchmark_metric metric;
    const char *name;
} benchmark_metric_definition;

typedef struct benchmark_config {
    size_t rows;
    size_t iterations;
    benchmark_phase phase;
    double max_us[BENCHMARK_METRIC_COUNT];
} benchmark_config;

typedef struct benchmark_context {
    const benchmark_config *config;
    const char *root;
    char *filename;
    mylite_db *db;
    int published_leaf_secondary_index;
} benchmark_context;

typedef struct scalar_result {
    unsigned long long value;
    int rows;
} scalar_result;

typedef struct scan_result {
    size_t rows;
    uint64_t checksum;
} scan_result;

typedef struct secondary_result {
    size_t rows;
    uint64_t checksum;
} secondary_result;

static int parse_config(int argc, char **argv, benchmark_config *config);
static int parse_phase_argument(const char *argument, benchmark_config *config);
static int parse_threshold_argument(const char *argument, benchmark_config *config);
static int find_metric_by_name(const char *name, size_t name_size, benchmark_metric *out_metric);
static const char *benchmark_phase_name(benchmark_phase phase);
static const char *benchmark_metric_name(benchmark_metric metric);
static int run_benchmark(const benchmark_config *config);
static void print_usage(const char *program);
static int print_result(
    const benchmark_config *config,
    benchmark_metric metric,
    const char *operation,
    size_t count,
    uint64_t elapsed_ns
);
static int setup_database(benchmark_context *ctx);
static int benchmark_prepared_scalar_selects(benchmark_context *ctx);
static int benchmark_insert_rows(benchmark_context *ctx);
static int benchmark_prepared_insert_rows(benchmark_context *ctx);
static int benchmark_point_selects(benchmark_context *ctx);
static int benchmark_prepared_point_selects(benchmark_context *ctx);
static int benchmark_prepared_point_select_components(benchmark_context *ctx);
static int benchmark_prepared_point_select_reset_after_row(benchmark_context *ctx);
static int benchmark_storage_entry_lookups(benchmark_context *ctx);
static int benchmark_storage_entry_lookups_in_one_read_statement(benchmark_context *ctx);
static int benchmark_storage_entry_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
);
static int benchmark_storage_point_lookups(benchmark_context *ctx);
static int benchmark_storage_point_lookups_in_one_read_statement(benchmark_context *ctx);
static int benchmark_storage_point_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
);
static int benchmark_storage_read_statements(benchmark_context *ctx);
static int benchmark_storage_row_updates(benchmark_context *ctx);
static int benchmark_secondary_selects(benchmark_context *ctx);
static int benchmark_prepared_secondary_selects(benchmark_context *ctx);
static int publish_secondary_leaf_index(benchmark_context *ctx);
static int benchmark_leaf_secondary_selects(benchmark_context *ctx);
static int benchmark_prepared_leaf_secondary_selects(benchmark_context *ctx);
static int benchmark_updates(benchmark_context *ctx);
static int benchmark_prepared_updates(benchmark_context *ctx);
static int benchmark_prepared_update_components(benchmark_context *ctx);
static int benchmark_ordered_scan(benchmark_context *ctx);
static int benchmark_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int benchmark_prepared_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
);
static int prepare_secondary_leaf_table(benchmark_context *ctx);
static int verify_secondary_leaf_index_root(benchmark_context *ctx);
static size_t secondary_value_for_row(benchmark_context *ctx, size_t row_number);
static size_t secondary_value_for_iteration(benchmark_context *ctx, size_t iteration);
static uint64_t secondary_expected_checksum(benchmark_context *ctx, size_t value, size_t *out_rows);
static size_t secondary_bucket_count(benchmark_context *ctx);
static int verify_row_count(benchmark_context *ctx, size_t expected);
static int exec_sql(benchmark_context *ctx, const char *sql);
static int query_uint64(benchmark_context *ctx, const char *sql, unsigned long long *out_value);
static int scalar_callback(void *data, int column_count, char **values, char **column_names);
static int secondary_callback(void *data, int column_count, char **values, char **column_names);
static int scan_callback(void *data, int column_count, char **values, char **column_names);
static void report_database_error(benchmark_context *ctx, const char *operation);
static uint64_t monotonic_ns(void);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static void remove_tree_entry(const char *path);

static const benchmark_metric_definition k_metric_definitions[] = {
    {BENCHMARK_METRIC_OPEN_SETUP, "open-setup"},
    {BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS, "prepared-scalar-selects"},
    {BENCHMARK_METRIC_DIRECT_INSERTS, "direct-inserts"},
    {BENCHMARK_METRIC_PREPARED_INSERTS, "prepared-inserts"},
    {BENCHMARK_METRIC_DIRECT_PK_SELECTS, "direct-pk-selects"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECTS, "prepared-pk-selects"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND, "prepared-pk-select-bind"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW, "prepared-pk-select-row"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE, "prepared-pk-select-done"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET, "prepared-pk-select-reset"},
    {BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW, "prepared-pk-select-reset-after-row"},
    {BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS, "storage-pk-entry-lookups"},
    {BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ, "storage-pk-entry-lookups-one-read"},
    {BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS, "storage-pk-row-lookups"},
    {BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ, "storage-pk-row-lookups-one-read"},
    {BENCHMARK_METRIC_STORAGE_READ_STATEMENTS, "storage-read-statements"},
    {BENCHMARK_METRIC_STORAGE_ROW_UPDATES, "storage-row-updates"},
    {BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS, "direct-secondary-selects"},
    {BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS, "prepared-secondary-selects"},
    {BENCHMARK_METRIC_PREPARE_LEAF_ROWS, "prepare-leaf-rows"},
    {BENCHMARK_METRIC_PUBLISH_LEAF_INDEX, "publish-leaf-index"},
    {BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS, "direct-leaf-secondary-selects"},
    {BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS, "prepared-leaf-secondary-selects"},
    {BENCHMARK_METRIC_DIRECT_UPDATES, "direct-updates"},
    {BENCHMARK_METRIC_PREPARED_UPDATES, "prepared-updates"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND, "prepared-update-bind"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP, "prepared-update-step"},
    {BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET, "prepared-update-reset"},
    {BENCHMARK_METRIC_ORDERED_SCAN, "ordered-scan"},
};

int main(int argc, char **argv) {
    benchmark_config config = {
        .rows = 100U,
        .iterations = 100U,
        .phase = BENCHMARK_PHASE_ALL,
    };

    if (parse_config(argc, argv, &config) != 0) {
        return 2;
    }

    return run_benchmark(&config);
}

static int parse_config(int argc, char **argv, benchmark_config *config) {
    int numeric_argument = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (strncmp(argv[i], "--phase=", 8U) == 0) {
            if (parse_phase_argument(argv[i] + 8U, config) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strncmp(argv[i], "--max-us=", 9U) == 0) {
            if (parse_threshold_argument(argv[i] + 9U, config) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (strncmp(argv[i], "--", 2U) == 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }

        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(argv[i], &end, 10);
        if (errno != 0 || end == argv[i] || *end != '\0' || value == 0U || value > 1000000U) {
            fprintf(stderr, "Expected a positive integer up to 1000000, got: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }

        ++numeric_argument;
        if (numeric_argument == 1) {
            config->rows = (size_t)value;
        } else if (numeric_argument == 2) {
            config->iterations = (size_t)value;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    return 0;
}

static int parse_phase_argument(const char *argument, benchmark_config *config) {
    if (strcmp(argument, "all") == 0) {
        config->phase = BENCHMARK_PHASE_ALL;
        return 0;
    }
    if (strcmp(argument, "prepared-scalar-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS;
        return 0;
    }
    if (strcmp(argument, "point-selects") == 0) {
        config->phase = BENCHMARK_PHASE_POINT_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-pk-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_PK_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-select-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS;
        return 0;
    }
    if (strcmp(argument, "prepared-pk-select-reset-after-row") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW;
        return 0;
    }
    if (strcmp(argument, "storage-pk-entry-lookups") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS;
        return 0;
    }
    if (strcmp(argument, "storage-pk-entry-lookups-one-read") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ;
        return 0;
    }
    if (strcmp(argument, "storage-pk-row-lookups") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS;
        return 0;
    }
    if (strcmp(argument, "storage-pk-row-lookups-one-read") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ;
        return 0;
    }
    if (strcmp(argument, "storage-read-statements") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_READ_STATEMENTS;
        return 0;
    }
    if (strcmp(argument, "storage-row-updates") == 0) {
        config->phase = BENCHMARK_PHASE_STORAGE_ROW_UPDATES;
        return 0;
    }
    if (strcmp(argument, "direct-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "direct-leaf-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "prepared-leaf-secondary-selects") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS;
        return 0;
    }
    if (strcmp(argument, "updates") == 0) {
        config->phase = BENCHMARK_PHASE_UPDATES;
        return 0;
    }
    if (strcmp(argument, "direct-updates") == 0) {
        config->phase = BENCHMARK_PHASE_DIRECT_UPDATES;
        return 0;
    }
    if (strcmp(argument, "prepared-updates") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_UPDATES;
        return 0;
    }
    if (strcmp(argument, "prepared-update-components") == 0) {
        config->phase = BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS;
        return 0;
    }

    fprintf(
        stderr,
        "Expected phase `all`, `prepared-scalar-selects`, `point-selects`, "
        "`direct-pk-selects`, `prepared-pk-selects`, "
        "`prepared-pk-select-components`, `prepared-pk-select-reset-after-row`, "
        "`storage-pk-entry-lookups`, "
        "`storage-pk-entry-lookups-one-read`, `storage-pk-row-lookups`, "
        "`storage-pk-row-lookups-one-read`, `storage-read-statements`, "
        "`storage-row-updates`, "
        "`direct-secondary-selects`, `prepared-secondary-selects`, "
        "`direct-leaf-secondary-selects`, `prepared-leaf-secondary-selects`, "
        "`updates`, `direct-updates`, `prepared-updates`, or "
        "`prepared-update-components`, got: %s\n",
        argument
    );
    return 1;
}

static int parse_threshold_argument(const char *argument, benchmark_config *config) {
    const char *separator = strchr(argument, ':');
    if (separator == NULL || separator == argument || separator[1] == '\0') {
        fprintf(stderr, "Expected threshold as <metric>:<max_us>, got: %s\n", argument);
        return 1;
    }

    benchmark_metric metric = BENCHMARK_METRIC_COUNT;
    if (find_metric_by_name(argument, (size_t)(separator - argument), &metric) != 0) {
        fprintf(
            stderr,
            "Unknown benchmark metric in threshold: %.*s\n",
            (int)(separator - argument),
            argument
        );
        return 1;
    }

    char *end = NULL;
    errno = 0;
    const double max_us = strtod(separator + 1, &end);
    if (errno != 0 || end == separator + 1 || *end != '\0' || max_us <= 0.0 || max_us != max_us ||
        max_us > 1000000000.0) {
        fprintf(stderr, "Expected positive threshold microseconds, got: %s\n", separator + 1);
        return 1;
    }

    config->max_us[metric] = max_us;
    return 0;
}

static int find_metric_by_name(const char *name, size_t name_size, benchmark_metric *out_metric) {
    for (size_t i = 0U; i < sizeof(k_metric_definitions) / sizeof(k_metric_definitions[0]); ++i) {
        const char *candidate = k_metric_definitions[i].name;
        if (strlen(candidate) == name_size && strncmp(candidate, name, name_size) == 0) {
            *out_metric = k_metric_definitions[i].metric;
            return 0;
        }
    }
    return 1;
}

static const char *benchmark_phase_name(benchmark_phase phase) {
    switch (phase) {
    case BENCHMARK_PHASE_ALL:
        return "all";
    case BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS:
        return "prepared-scalar-selects";
    case BENCHMARK_PHASE_POINT_SELECTS:
        return "point-selects";
    case BENCHMARK_PHASE_DIRECT_PK_SELECTS:
        return "direct-pk-selects";
    case BENCHMARK_PHASE_PREPARED_PK_SELECTS:
        return "prepared-pk-selects";
    case BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS:
        return "prepared-pk-select-components";
    case BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW:
        return "prepared-pk-select-reset-after-row";
    case BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS:
        return "storage-pk-entry-lookups";
    case BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ:
        return "storage-pk-entry-lookups-one-read";
    case BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS:
        return "storage-pk-row-lookups";
    case BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ:
        return "storage-pk-row-lookups-one-read";
    case BENCHMARK_PHASE_STORAGE_READ_STATEMENTS:
        return "storage-read-statements";
    case BENCHMARK_PHASE_STORAGE_ROW_UPDATES:
        return "storage-row-updates";
    case BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS:
        return "direct-secondary-selects";
    case BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS:
        return "prepared-secondary-selects";
    case BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS:
        return "direct-leaf-secondary-selects";
    case BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS:
        return "prepared-leaf-secondary-selects";
    case BENCHMARK_PHASE_UPDATES:
        return "updates";
    case BENCHMARK_PHASE_DIRECT_UPDATES:
        return "direct-updates";
    case BENCHMARK_PHASE_PREPARED_UPDATES:
        return "prepared-updates";
    case BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS:
        return "prepared-update-components";
    }
    return "unknown";
}

static int run_benchmark(const benchmark_config *config) {
    benchmark_context ctx = {
        .config = config,
        .root = NULL,
        .filename = NULL,
        .db = NULL,
        .published_leaf_secondary_index = 0,
    };
    int result = 1;
    uint64_t start_ns;
    const int scalar_phase = config->phase == BENCHMARK_PHASE_PREPARED_SCALAR_SELECTS;
    const int point_select_phase =
        config->phase == BENCHMARK_PHASE_POINT_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_PK_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS ||
        config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ ||
        config->phase == BENCHMARK_PHASE_STORAGE_READ_STATEMENTS;
    const int secondary_select_phase =
        config->phase == BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS ||
        config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS;
    const int storage_update_phase = config->phase == BENCHMARK_PHASE_STORAGE_ROW_UPDATES;

    ctx.root = make_temp_root();
    if (ctx.root == NULL) {
        return 1;
    }

    printf("# MyLite Performance Baseline\n\n");
    printf("Rows: %zu\n", config->rows);
    printf("Iterations: %zu\n", config->iterations);
    printf("Phase: %s\n", benchmark_phase_name(config->phase));
    printf("Storage route: `ENGINE=InnoDB` through the MyLite storage engine\n\n");
    printf("| Operation | Count | Total ms | us/op |\n");
    printf("| --- | ---: | ---: | ---: |\n");

    start_ns = monotonic_ns();
    if (setup_database(&ctx) != 0) {
        goto cleanup;
    }
    if (print_result(
            config,
            BENCHMARK_METRIC_OPEN_SETUP,
            "open and schema setup",
            1U,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }

    if (config->phase == BENCHMARK_PHASE_ALL || scalar_phase) {
        if (benchmark_prepared_scalar_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (scalar_phase) {
            result = 0;
            goto cleanup;
        }
    }

    if (benchmark_insert_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (verify_row_count(&ctx, config->rows) != 0) {
        goto cleanup;
    }
    if (storage_update_phase) {
        if (benchmark_storage_row_updates(&ctx) != 0) {
            goto cleanup;
        }
        if (verify_row_count(&ctx, config->rows) != 0) {
            goto cleanup;
        }
        if (benchmark_ordered_scan(&ctx) != 0) {
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (point_select_phase) {
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS) {
            if (benchmark_storage_entry_lookups(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ) {
            if (benchmark_storage_entry_lookups_in_one_read_statement(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS) {
            if (benchmark_storage_point_lookups(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_PK_ROW_LOOKUPS_ONE_READ) {
            if (benchmark_storage_point_lookups_in_one_read_statement(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_STORAGE_READ_STATEMENTS) {
            if (benchmark_storage_read_statements(&ctx) != 0) {
                goto cleanup;
            }
            result = 0;
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW &&
            benchmark_point_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_DIRECT_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_RESET_AFTER_ROW &&
            benchmark_prepared_point_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            benchmark_prepared_point_select_components(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase != BENCHMARK_PHASE_DIRECT_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECTS &&
            config->phase != BENCHMARK_PHASE_PREPARED_PK_SELECT_COMPONENTS &&
            benchmark_prepared_point_select_reset_after_row(&ctx) != 0) {
            goto cleanup;
        }
        result = 0;
        goto cleanup;
    }
    if (secondary_select_phase) {
        if (config->phase == BENCHMARK_PHASE_DIRECT_SECONDARY_SELECTS &&
            benchmark_secondary_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_PREPARED_SECONDARY_SELECTS &&
            benchmark_prepared_secondary_selects(&ctx) != 0) {
            goto cleanup;
        }
        if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS ||
            config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS) {
            if (publish_secondary_leaf_index(&ctx) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_DIRECT_LEAF_SECONDARY_SELECTS &&
                benchmark_leaf_secondary_selects(&ctx) != 0) {
                goto cleanup;
            }
            if (config->phase == BENCHMARK_PHASE_PREPARED_LEAF_SECONDARY_SELECTS &&
                benchmark_prepared_leaf_secondary_selects(&ctx) != 0) {
                goto cleanup;
            }
        }
        result = 0;
        goto cleanup;
    }
    if (benchmark_prepared_insert_rows(&ctx) != 0) {
        goto cleanup;
    }
    if (config->phase != BENCHMARK_PHASE_ALL) {
        goto updates;
    }
    if (benchmark_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_point_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_point_select_reset_after_row(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (publish_secondary_leaf_index(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_leaf_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
    if (benchmark_prepared_leaf_secondary_selects(&ctx) != 0) {
        goto cleanup;
    }
updates:
    if (config->phase != BENCHMARK_PHASE_PREPARED_UPDATES &&
        config->phase != BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS) {
        if (benchmark_updates(&ctx) != 0) {
            goto cleanup;
        }
    }
    if (config->phase == BENCHMARK_PHASE_PREPARED_UPDATE_COMPONENTS) {
        if (benchmark_prepared_update_components(&ctx) != 0) {
            goto cleanup;
        }
    } else if (config->phase != BENCHMARK_PHASE_DIRECT_UPDATES) {
        if (benchmark_prepared_updates(&ctx) != 0) {
            goto cleanup;
        }
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
    if (getenv("MYLITE_PERF_KEEP_ROOT") != NULL) {
        fprintf(stderr, "Keeping benchmark root: %s\n", ctx.root);
    } else {
        remove_tree(ctx.root);
    }
    free((void *)ctx.root);
    return result;
}

static void print_usage(const char *program) {
    fprintf(
        stderr,
        "Usage: %s [--phase=all|prepared-scalar-selects|point-selects|direct-pk-selects|"
        "prepared-pk-selects|prepared-pk-select-components|"
        "prepared-pk-select-reset-after-row|updates|direct-updates|"
        "prepared-updates|prepared-update-components|direct-secondary-selects|"
        "prepared-secondary-selects|"
        "direct-leaf-secondary-selects|prepared-leaf-secondary-selects|"
        "storage-pk-entry-lookups|storage-pk-entry-lookups-one-read|storage-pk-row-lookups|"
        "storage-pk-row-lookups-one-read|storage-read-statements|storage-row-updates] "
        "[--max-us=<metric>:<value>] [rows] [iterations]\n"
        "\n"
        "Defaults: phase=all rows=100 iterations=100.\n"
        "Focused scalar, point-select, secondary-select, and update phases skip unrelated "
        "timings after setup.\n"
        "Thresholds are opt-in and may be supplied more than once. Metrics: "
        "open-setup, prepared-scalar-selects, direct-inserts, prepared-inserts, "
        "direct-pk-selects, prepared-pk-selects, prepared-pk-select-bind, "
        "prepared-pk-select-row, prepared-pk-select-done, prepared-pk-select-reset, "
        "prepared-pk-select-reset-after-row, "
        "storage-pk-entry-lookups, storage-pk-entry-lookups-one-read, "
        "storage-pk-row-lookups, storage-pk-row-lookups-one-read, "
        "storage-read-statements, storage-row-updates, "
        "direct-secondary-selects, prepared-secondary-selects, "
        "prepare-leaf-rows, publish-leaf-index, "
        "direct-leaf-secondary-selects, "
        "prepared-leaf-secondary-selects, direct-updates, prepared-updates, "
        "prepared-update-bind, prepared-update-step, prepared-update-reset, ordered-scan.\n"
        "Set MYLITE_PERF_KEEP_ROOT=1 to keep the temporary benchmark directory.\n",
        program
    );
}

static int print_result(
    const benchmark_config *config,
    benchmark_metric metric,
    const char *operation,
    size_t count,
    uint64_t elapsed_ns
) {
    const double total_ms = (double)elapsed_ns / 1000000.0;
    const double micros_per_operation =
        count == 0U ? 0.0 : (double)elapsed_ns / (double)count / 1000.0;

    printf("| %s | %zu | %.3f | %.3f |\n", operation, count, total_ms, micros_per_operation);
    if (config->max_us[metric] > 0.0 && micros_per_operation > config->max_us[metric]) {
        fprintf(
            stderr,
            "Benchmark metric %s exceeded %.3f us/op: %.3f us/op\n",
            benchmark_metric_name(metric),
            config->max_us[metric],
            micros_per_operation
        );
        return 1;
    }
    return 0;
}

static const char *benchmark_metric_name(benchmark_metric metric) {
    for (size_t i = 0U; i < sizeof(k_metric_definitions) / sizeof(k_metric_definitions[0]); ++i) {
        if (k_metric_definitions[i].metric == metric) {
            return k_metric_definitions[i].name;
        }
    }
    return "unknown";
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

static int benchmark_prepared_scalar_selects(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT CAST(? AS SIGNED) + 1",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare scalar select");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const unsigned long long input = (unsigned long long)(i % 1000000U);
        const unsigned long long expected = input + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)input) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared scalar select");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(stderr, "Prepared scalar select returned no row\n");
            report_database_error(ctx, "prepared scalar select");
            goto cleanup;
        }

        unsigned long long value = 0U;
        const mylite_value_type value_type = mylite_column_type(stmt, 0U);
        if (value_type == MYLITE_TYPE_INT64) {
            value = (unsigned long long)mylite_column_int64(stmt, 0U);
        } else if (value_type == MYLITE_TYPE_UINT64) {
            value = mylite_column_uint64(stmt, 0U);
        } else {
            fprintf(stderr, "Prepared scalar select returned a non-integer value\n");
            goto cleanup;
        }
        if (value != expected) {
            fprintf(
                stderr,
                "Prepared scalar select returned %llu; expected %llu\n",
                value,
                expected
            );
            goto cleanup;
        }
        checksum += value;

        const int done_result = mylite_step(stmt);
        if (done_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared scalar select returned extra rows\n");
            report_database_error(ctx, "prepared scalar select completion");
            goto cleanup;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared scalar select");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_SCALAR_SELECTS,
            "prepared scalar selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared scalar checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared scalar select");
        return 1;
    }
    return result;
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
            secondary_value_for_row(ctx, i + 1U),
            i + 1U
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_INSERTS,
            "direct inserts in one transaction",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

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

static int benchmark_prepared_insert_rows(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_prepared_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "value INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL,"
            "KEY value_key (value)"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }
    if (mylite_prepare(
            ctx->db,
            "INSERT INTO perf_prepared_rows (id, value, pad) VALUES (?, ?, ?)",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare row insert");
        goto rollback;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char pad[32];
        const size_t row_id = i + 1U;
        const int written = snprintf(pad, sizeof(pad), "row-%zu", row_id);
        if (written < 0 || (size_t)written >= sizeof(pad)) {
            goto rollback;
        }
        if (mylite_bind_int64(stmt, 1U, (long long)row_id) != MYLITE_OK ||
            mylite_bind_int64(stmt, 2U, (long long)secondary_value_for_row(ctx, row_id)) !=
                MYLITE_OK ||
            mylite_bind_text(stmt, 3U, pad, MYLITE_NUL_TERMINATED, MYLITE_TRANSIENT) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared row insert");
            goto rollback;
        }

        const int step_result = mylite_step(stmt);
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared row insert failed for id %zu\n", row_id);
            report_database_error(ctx, "prepared row insert");
            goto rollback;
        }
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared row insert");
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_INSERTS,
            "prepared inserts in one transaction",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared row insert");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    unsigned long long row_count = 0U;
    if (query_uint64(ctx, "SELECT COUNT(*) FROM perf_prepared_rows", &row_count) != 0) {
        return 1;
    }
    if (row_count != (unsigned long long)ctx->config->rows) {
        fprintf(stderr, "Expected %zu prepared rows, got %llu\n", ctx->config->rows, row_count);
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared row insert");
        result = 1;
    }
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

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_PK_SELECTS,
            "direct primary-key point selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
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

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECTS,
            "prepared primary-key point selects",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared point-select checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select");
        return 1;
    }
    return result;
}

static int benchmark_prepared_point_select_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t checksum = 0U;
    uint64_t bind_ns = 0U;
    uint64_t row_ns = 0U;
    uint64_t done_ns = 0U;
    uint64_t reset_ns = 0U;
    int result = 1;

    if (mylite_prepare(
            ctx->db,
            "SELECT value FROM perf_rows WHERE id = ?",
            MYLITE_NUL_TERMINATED,
            &stmt,
            NULL
        ) != MYLITE_OK) {
        report_database_error(ctx, "prepare primary-key point select components");
        return 1;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select components");
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int row_result = mylite_step(stmt);
        const int row_is_int =
            row_result == MYLITE_ROW && mylite_column_type(stmt, 0U) == MYLITE_TYPE_INT64;
        if (row_is_int) {
            checksum += (uint64_t)mylite_column_int64(stmt, 0U);
        }
        row_ns += monotonic_ns() - start_ns;
        if (row_result != MYLITE_ROW) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned no row for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select components");
            goto cleanup;
        }
        if (!row_is_int) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned a non-integer value\n"
            );
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int done_result = mylite_step(stmt);
        done_ns += monotonic_ns() - start_ns;
        if (done_result != MYLITE_DONE) {
            fprintf(
                stderr,
                "Prepared primary-key point select component phase returned extra rows for id "
                "%zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select component drain");
            goto cleanup;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select components");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_BIND,
            "prepared primary-key bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_ROW,
            "prepared primary-key row component",
            ctx->config->iterations,
            row_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_DONE,
            "prepared primary-key done component",
            ctx->config->iterations,
            done_ns
        ) != 0) {
        goto cleanup;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_COMPONENT_RESET,
            "prepared primary-key reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared point-select component checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select components");
        return 1;
    }
    return result;
}

static int benchmark_prepared_point_select_reset_after_row(benchmark_context *ctx) {
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
        report_database_error(ctx, "prepare primary-key point select reset-after-row");
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        if (mylite_bind_int64(stmt, 1U, (long long)id) != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key point select reset-after-row");
            goto cleanup;
        }

        const int row_result = mylite_step(stmt);
        if (row_result != MYLITE_ROW) {
            fprintf(
                stderr,
                "Prepared primary-key reset-after-row point select returned no row for id %zu\n",
                id
            );
            report_database_error(ctx, "prepared primary-key point select reset-after-row");
            goto cleanup;
        }
        if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64) {
            fprintf(
                stderr,
                "Prepared primary-key reset-after-row point select returned a non-integer value\n"
            );
            goto cleanup;
        }
        checksum += (uint64_t)mylite_column_int64(stmt, 0U);

        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key point select after row");
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_PK_SELECT_RESET_AFTER_ROW,
            "prepared primary-key point selects reset after row",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("Prepared reset-after-row point-select checksum: %" PRIu64 "\n", checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key point select reset-after-row");
        return 1;
    }
    return result;
}

static int benchmark_storage_entry_lookups(benchmark_context *ctx) {
    return benchmark_storage_entry_lookups_with_scope(
        ctx,
        0,
        BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS,
        "storage primary-key entry lookups"
    );
}

static int benchmark_storage_entry_lookups_in_one_read_statement(benchmark_context *ctx) {
    return benchmark_storage_entry_lookups_with_scope(
        ctx,
        1,
        BENCHMARK_METRIC_STORAGE_PK_ENTRY_LOOKUPS_ONE_READ,
        "storage primary-key entry lookups in one read statement"
    );
}

static int benchmark_storage_entry_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *shared_statement = NULL;
    uint64_t row_id_checksum = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary-key entries, got %zu\n",
            ctx->config->rows,
            entries.entry_count
        );
        goto cleanup;
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    if (one_read_statement) {
        storage_result = mylite_storage_begin_read_statement(ctx->filename, &shared_statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scopes;
        }
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % entries.entry_count;
        if (entry_index >= entries.entry_count ||
            entries.key_offsets[entry_index] > entries.key_bytes ||
            entries.key_sizes[entry_index] > entries.key_bytes - entries.key_offsets[entry_index]) {
            fprintf(stderr, "Primary-key index entry %zu is corrupt\n", entry_index);
            goto end_scopes;
        }

        mylite_storage_statement *statement = NULL;
        if (!one_read_statement) {
            storage_result = mylite_storage_begin_read_statement(ctx->filename, &statement);
            if (storage_result != MYLITE_STORAGE_OK) {
                fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
                goto end_read_statement;
            }
        }

        unsigned long long row_id = 0ULL;
        storage_result = mylite_storage_find_index_entry(
            ctx->filename,
            "perf",
            "perf_rows",
            0U,
            entries.keys + entries.key_offsets[entry_index],
            entries.key_sizes[entry_index],
            &row_id
        );
        mylite_storage_result end_result = MYLITE_STORAGE_OK;
        if (!one_read_statement) {
            end_result = mylite_storage_end_read_statement(statement);
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Storage entry lookup failed: %d\n", storage_result);
            goto end_read_statement;
        }
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            goto end_read_statement;
        }
        if (row_id != entries.row_ids[entry_index]) {
            fprintf(
                stderr,
                "Storage entry lookup returned row_id=%llu for entry %zu; expected row_id=%llu\n",
                row_id,
                entry_index,
                entries.row_ids[entry_index]
            );
            goto end_read_statement;
        }

        row_id_checksum += row_id;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_read_statement;
    }
    printf("Storage entry-lookup row-id checksum: %" PRIu64 "\n", row_id_checksum);
    result = 0;

end_read_statement:
    if (shared_statement != NULL) {
        const mylite_storage_result end_result =
            mylite_storage_end_read_statement(shared_statement);
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            result = 1;
        }
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    mylite_storage_free_index_entryset(&entries);
    return result;
}

static int benchmark_storage_point_lookups(benchmark_context *ctx) {
    return benchmark_storage_point_lookups_with_scope(
        ctx,
        0,
        BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS,
        "storage primary-key row lookups"
    );
}

static int benchmark_storage_point_lookups_in_one_read_statement(benchmark_context *ctx) {
    return benchmark_storage_point_lookups_with_scope(
        ctx,
        1,
        BENCHMARK_METRIC_STORAGE_PK_ROW_LOOKUPS_ONE_READ,
        "storage primary-key row lookups in one read statement"
    );
}

static int benchmark_storage_point_lookups_with_scope(
    benchmark_context *ctx,
    int one_read_statement,
    benchmark_metric metric,
    const char *operation
) {
    mylite_storage_index_entryset entries = {
        .size = sizeof(entries),
    };
    mylite_storage_statement *shared_statement = NULL;
    unsigned char row[1024];
    uint64_t row_id_checksum = 0U;
    uint64_t row_size_checksum = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary-key entries, got %zu\n",
            ctx->config->rows,
            entries.entry_count
        );
        goto cleanup;
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    if (one_read_statement) {
        storage_result = mylite_storage_begin_read_statement(ctx->filename, &shared_statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scopes;
        }
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % entries.entry_count;
        if (entry_index >= entries.entry_count ||
            entries.key_offsets[entry_index] > entries.key_bytes ||
            entries.key_sizes[entry_index] > entries.key_bytes - entries.key_offsets[entry_index]) {
            fprintf(stderr, "Primary-key index entry %zu is corrupt\n", entry_index);
            goto end_scopes;
        }

        mylite_storage_statement *statement = NULL;
        if (!one_read_statement) {
            storage_result = mylite_storage_begin_read_statement(ctx->filename, &statement);
            if (storage_result != MYLITE_STORAGE_OK) {
                fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
                goto end_read_statement;
            }
        }

        unsigned long long row_id = 0ULL;
        size_t row_size = 0U;
        storage_result = mylite_storage_find_indexed_row_into(
            ctx->filename,
            "perf",
            "perf_rows",
            0U,
            entries.keys + entries.key_offsets[entry_index],
            entries.key_sizes[entry_index],
            &row_id,
            row,
            sizeof(row),
            &row_size
        );
        mylite_storage_result end_result = MYLITE_STORAGE_OK;
        if (!one_read_statement) {
            end_result = mylite_storage_end_read_statement(statement);
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Storage point lookup failed: %d\n", storage_result);
            goto end_read_statement;
        }
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            goto end_read_statement;
        }
        if (row_id != entries.row_ids[entry_index] || row_size == 0U || row_size > sizeof(row)) {
            fprintf(
                stderr,
                "Storage point lookup returned row_id=%llu row_size=%zu for entry %zu; "
                "expected row_id=%llu\n",
                row_id,
                row_size,
                entry_index,
                entries.row_ids[entry_index]
            );
            goto end_read_statement;
        }

        row_id_checksum += row_id;
        row_size_checksum += row_size;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_read_statement;
    }
    printf("Storage point-lookup row-id checksum: %" PRIu64 "\n", row_id_checksum);
    printf("Storage point-lookup row-size checksum: %" PRIu64 "\n", row_size_checksum);
    result = 0;

end_read_statement:
    if (shared_statement != NULL) {
        const mylite_storage_result end_result =
            mylite_storage_end_read_statement(shared_statement);
        if (end_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", end_result);
            result = 1;
        }
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    mylite_storage_free_index_entryset(&entries);
    return result;
}

static int benchmark_storage_read_statements(benchmark_context *ctx) {
    uint64_t opened_statement_count = 0U;
    int result = 1;

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        mylite_storage_statement *statement = NULL;
        mylite_storage_result storage_result =
            mylite_storage_begin_read_statement(ctx->filename, &statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage read statement: %d\n", storage_result);
            goto end_scope;
        }
        if (statement != NULL) {
            ++opened_statement_count;
        }

        storage_result = mylite_storage_end_read_statement(statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to end storage read statement: %d\n", storage_result);
            goto end_scope;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_READ_STATEMENTS,
            "storage read statement begin/end pairs",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto end_scope;
    }
    printf("Storage read-statement opened count: %" PRIu64 "\n", opened_statement_count);
    result = 0;

end_scope:
    mylite_storage_end_filename_identity_scope(&filename_scope);
    return result;
}

static int benchmark_storage_row_updates(benchmark_context *ctx) {
    mylite_storage_index_entryset primary_entries = {
        .size = sizeof(primary_entries),
    };
    mylite_storage_rowset rows = {
        .size = sizeof(rows),
    };
    mylite_storage_statement *transaction = NULL;
    unsigned long long *row_ids = NULL;
    uint64_t row_id_checksum = 0U;
    int result = 1;

    mylite_storage_result storage_result =
        mylite_storage_read_index_entries(ctx->filename, "perf", "perf_rows", 0U, &primary_entries);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read primary-key index entries: %d\n", storage_result);
        return 1;
    }
    if (primary_entries.entry_count != ctx->config->rows) {
        fprintf(
            stderr,
            "Expected %zu primary entries, got %zu\n",
            ctx->config->rows,
            primary_entries.entry_count
        );
        goto cleanup;
    }

    row_ids = malloc(ctx->config->rows * sizeof(*row_ids));
    if (row_ids == NULL) {
        fprintf(stderr, "Failed to allocate storage update row-id map\n");
        goto cleanup;
    }
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        row_ids[i] = primary_entries.row_ids[i];
    }

    storage_result = mylite_storage_read_indexed_rows(
        ctx->filename,
        "perf",
        "perf_rows",
        primary_entries.row_ids,
        primary_entries.entry_count,
        &rows
    );
    if (storage_result != MYLITE_STORAGE_OK || rows.row_count != primary_entries.entry_count) {
        fprintf(stderr, "Failed to load storage update row payloads: %d\n", storage_result);
        goto cleanup;
    }
    for (size_t i = 0; i < rows.row_count; ++i) {
        if (rows.row_offsets[i] > rows.row_bytes ||
            rows.row_sizes[i] > rows.row_bytes - rows.row_offsets[i]) {
            fprintf(stderr, "Storage update row payload %zu is corrupt\n", i);
            goto cleanup;
        }
    }

    mylite_storage_filename_identity_scope filename_scope = {0};
    mylite_storage_table_name_identity_scope table_scope = {0};
    mylite_storage_begin_filename_identity_scope(ctx->filename, &filename_scope);
    mylite_storage_begin_table_name_identity_scope("perf", "perf_rows", &table_scope);

    storage_result = mylite_storage_begin_transaction(ctx->filename, &transaction);
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to begin storage update transaction: %d\n", storage_result);
        goto end_scopes;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t entry_index = i % primary_entries.entry_count;
        const unsigned char *row = rows.rows + rows.row_offsets[entry_index];
        const size_t row_size = rows.row_sizes[entry_index];

        mylite_storage_statement *statement = NULL;
        storage_result = mylite_storage_begin_nested_statement(transaction, &statement);
        if (storage_result != MYLITE_STORAGE_OK) {
            fprintf(stderr, "Failed to begin storage update statement: %d\n", storage_result);
            goto rollback;
        }

        unsigned long long new_row_id = 0ULL;
        storage_result = mylite_storage_update_row_preserving_index_entries(
            ctx->filename,
            "perf",
            "perf_rows",
            row_ids[entry_index],
            row,
            row_size,
            &new_row_id
        );
        if (storage_result == MYLITE_STORAGE_OK) {
            storage_result = mylite_storage_commit_statement(statement);
            statement = NULL;
        }
        if (storage_result != MYLITE_STORAGE_OK) {
            if (statement != NULL) {
                (void)mylite_storage_rollback_statement(statement);
            }
            fprintf(stderr, "Storage row update failed: %d\n", storage_result);
            goto rollback;
        }

        row_ids[entry_index] = new_row_id;
        row_id_checksum += new_row_id;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_STORAGE_ROW_UPDATES,
            "storage row updates in one transaction",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }
    printf("Storage row-update row-id checksum: %" PRIu64 "\n", row_id_checksum);

    storage_result = mylite_storage_commit_statement(transaction);
    transaction = NULL;
    if (storage_result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to commit storage update transaction: %d\n", storage_result);
        goto end_scopes;
    }
    result = 0;
    goto end_scopes;

rollback:
    if (transaction != NULL) {
        (void)mylite_storage_rollback_statement(transaction);
        transaction = NULL;
    }
end_scopes:
    mylite_storage_end_table_name_identity_scope(&table_scope);
    mylite_storage_end_filename_identity_scope(&filename_scope);
cleanup:
    free(row_ids);
    mylite_storage_free_rowset(&rows);
    mylite_storage_free_index_entryset(&primary_entries);
    return result;
}

static int benchmark_secondary_selects(benchmark_context *ctx) {
    return benchmark_secondary_selects_for_index(
        ctx,
        "perf_rows",
        "value_key",
        BENCHMARK_METRIC_DIRECT_SECONDARY_SELECTS,
        "direct secondary-index exact selects",
        "Secondary exact-select rows",
        "Secondary exact-select checksum"
    );
}

static int benchmark_prepared_secondary_selects(benchmark_context *ctx) {
    return benchmark_prepared_secondary_selects_for_index(
        ctx,
        "perf_rows",
        "value_key",
        BENCHMARK_METRIC_PREPARED_SECONDARY_SELECTS,
        "prepared secondary-index exact selects",
        "Prepared secondary exact-select rows",
        "Prepared secondary exact-select checksum"
    );
}

static int publish_secondary_leaf_index(benchmark_context *ctx) {
    if (prepare_secondary_leaf_table(ctx) != 0) {
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    if (exec_sql(ctx, "CREATE INDEX value_leaf_key ON perf_leaf_rows (value) ALGORITHM=COPY") !=
        0) {
        return 1;
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PUBLISH_LEAF_INDEX,
            "publish secondary leaf index",
            1U,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    return verify_secondary_leaf_index_root(ctx);
}

static int benchmark_leaf_secondary_selects(benchmark_context *ctx) {
    if (!ctx->published_leaf_secondary_index) {
        printf("Published leaf secondary exact selects: skipped; no leaf root\n");
        return 0;
    }

    return benchmark_secondary_selects_for_index(
        ctx,
        "perf_leaf_rows",
        "value_leaf_key",
        BENCHMARK_METRIC_DIRECT_LEAF_SECONDARY_SELECTS,
        "direct published-leaf secondary-index exact selects",
        "Published leaf secondary exact-select rows",
        "Published leaf secondary exact-select checksum"
    );
}

static int benchmark_prepared_leaf_secondary_selects(benchmark_context *ctx) {
    if (!ctx->published_leaf_secondary_index) {
        printf("Prepared published leaf secondary exact selects: skipped; no leaf root\n");
        return 0;
    }

    return benchmark_prepared_secondary_selects_for_index(
        ctx,
        "perf_leaf_rows",
        "value_leaf_key",
        BENCHMARK_METRIC_PREPARED_LEAF_SECONDARY_SELECTS,
        "prepared published-leaf secondary-index exact selects",
        "Prepared published leaf secondary exact-select rows",
        "Prepared published leaf secondary exact-select checksum"
    );
}

static int benchmark_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    const uint64_t start_ns = monotonic_ns();

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        char sql[128];
        secondary_result result = {
            .rows = 0U,
            .checksum = 0U,
        };
        size_t expected_rows = 0U;
        const uint64_t expected_checksum = secondary_expected_checksum(ctx, value, &expected_rows);
        const int written = snprintf(
            sql,
            sizeof(sql),
            "SELECT id, value FROM %s FORCE INDEX (%s) "
            "WHERE value = %zu ORDER BY id",
            table_name,
            index_name,
            value
        );
        if (written < 0 || (size_t)written >= sizeof(sql)) {
            return 1;
        }
        if (mylite_exec(ctx->db, sql, secondary_callback, &result, NULL) != MYLITE_OK) {
            report_database_error(ctx, operation);
            return 1;
        }
        if (result.rows != expected_rows || result.checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned %zu rows/%" PRIu64 " checksum; expected %zu/%" PRIu64
                "\n",
                operation,
                value,
                result.rows,
                result.checksum,
                expected_rows,
                expected_checksum
            );
            return 1;
        }
        total_rows += result.rows;
        checksum += result.checksum;
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    return 0;
}

static int benchmark_prepared_secondary_selects_for_index(
    benchmark_context *ctx,
    const char *table_name,
    const char *index_name,
    benchmark_metric metric,
    const char *operation,
    const char *rows_label,
    const char *checksum_label
) {
    mylite_stmt *stmt = NULL;
    size_t total_rows = 0U;
    uint64_t checksum = 0U;
    int result = 1;
    char sql[160];
    const int written = snprintf(
        sql,
        sizeof(sql),
        "SELECT id, value FROM %s FORCE INDEX (%s) "
        "WHERE value = ? ORDER BY id",
        table_name,
        index_name
    );
    if (written < 0 || (size_t)written >= sizeof(sql)) {
        return 1;
    }

    if (mylite_prepare(ctx->db, sql, MYLITE_NUL_TERMINATED, &stmt, NULL) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }

    const uint64_t start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t value = secondary_value_for_iteration(ctx, i);
        secondary_result selected = {
            .rows = 0U,
            .checksum = 0U,
        };
        size_t expected_rows = 0U;
        const uint64_t expected_checksum = secondary_expected_checksum(ctx, value, &expected_rows);
        if (mylite_bind_int64(stmt, 1U, (long long)value) != MYLITE_OK) {
            report_database_error(ctx, operation);
            goto cleanup;
        }

        for (;;) {
            const int step_result = mylite_step(stmt);
            if (step_result == MYLITE_DONE) {
                break;
            }
            if (step_result != MYLITE_ROW) {
                fprintf(stderr, "%s failed for value %zu\n", operation, value);
                report_database_error(ctx, operation);
                goto cleanup;
            }
            if (mylite_column_type(stmt, 0U) != MYLITE_TYPE_INT64 ||
                mylite_column_type(stmt, 1U) != MYLITE_TYPE_INT64) {
                fprintf(stderr, "%s returned non-integer values\n", operation);
                goto cleanup;
            }
            selected.checksum +=
                (uint64_t)mylite_column_int64(stmt, 0U) + (uint64_t)mylite_column_int64(stmt, 1U);
            ++selected.rows;
        }

        if (selected.rows != expected_rows || selected.checksum != expected_checksum) {
            fprintf(
                stderr,
                "%s for value %zu returned %zu rows/%" PRIu64 " checksum; expected %zu/%" PRIu64
                "\n",
                operation,
                value,
                selected.rows,
                selected.checksum,
                expected_rows,
                expected_checksum
            );
            goto cleanup;
        }
        total_rows += selected.rows;
        checksum += selected.checksum;
        if (mylite_reset(stmt) != MYLITE_OK) {
            report_database_error(ctx, operation);
            goto cleanup;
        }
    }

    if (print_result(
            ctx->config,
            metric,
            operation,
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto cleanup;
    }
    printf("%s: %zu\n", rows_label, total_rows);
    printf("%s: %" PRIu64 "\n", checksum_label, checksum);
    result = 0;

cleanup:
    if (mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, operation);
        return 1;
    }
    return result;
}

static int prepare_secondary_leaf_table(benchmark_context *ctx) {
    uint64_t start_ns;
    int result = 1;

    if (exec_sql(
            ctx,
            "CREATE TABLE perf_leaf_rows ("
            "id INT NOT NULL PRIMARY KEY,"
            "value INT NOT NULL,"
            "pad VARCHAR(64) NOT NULL"
            ") ENGINE=InnoDB"
        ) != 0) {
        return 1;
    }
    if (exec_sql(ctx, "BEGIN") != 0) {
        return 1;
    }

    start_ns = monotonic_ns();
    for (size_t i = 0; i < ctx->config->rows; ++i) {
        char sql[176];
        const int written = snprintf(
            sql,
            sizeof(sql),
            "INSERT INTO perf_leaf_rows (id, value, pad) VALUES (%zu, %zu, 'row-%zu')",
            i + 1U,
            secondary_value_for_row(ctx, i + 1U),
            i + 1U
        );
        if (written < 0 || (size_t)written >= sizeof(sql) || exec_sql(ctx, sql) != 0) {
            goto rollback;
        }
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARE_LEAF_ROWS,
            "prepare secondary leaf benchmark rows",
            ctx->config->rows,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

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

static int verify_secondary_leaf_index_root(benchmark_context *ctx) {
    enum { value_leaf_key_index_number = 1U };

    mylite_storage_index_root_metadata metadata = {
        .size = sizeof(metadata),
    };
    const mylite_storage_result result = mylite_storage_read_index_root(
        ctx->filename,
        "perf",
        "perf_leaf_rows",
        value_leaf_key_index_number,
        &metadata
    );

    if (result == MYLITE_STORAGE_NOTFOUND) {
        printf("Published leaf root: skipped; not available under current single-page limits\n");
        return 0;
    }
    if (result != MYLITE_STORAGE_OK) {
        fprintf(stderr, "Failed to read published leaf root metadata: %d\n", result);
        return 1;
    }
    if (metadata.entry_count != (unsigned long long)ctx->config->rows) {
        fprintf(
            stderr,
            "Published leaf root has %llu entries; expected %zu\n",
            metadata.entry_count,
            ctx->config->rows
        );
        return 1;
    }

    ctx->published_leaf_secondary_index = 1;
    printf(
        "Published leaf root: table perf_leaf_rows index %u, entries %llu\n",
        value_leaf_key_index_number,
        metadata.entry_count
    );
    return 0;
}

static size_t secondary_value_for_row(benchmark_context *ctx, size_t row_number) {
    return ((row_number - 1U) % secondary_bucket_count(ctx)) + 1U;
}

static size_t secondary_value_for_iteration(benchmark_context *ctx, size_t iteration) {
    return (iteration % secondary_bucket_count(ctx)) + 1U;
}

static uint64_t secondary_expected_checksum(
    benchmark_context *ctx,
    size_t value,
    size_t *out_rows
) {
    const size_t bucket_count = secondary_bucket_count(ctx);
    size_t rows = 0U;
    uint64_t checksum = 0U;
    if (value <= bucket_count && value <= ctx->config->rows) {
        const size_t first_id = value;
        const size_t last_id = value + ((ctx->config->rows - value) / bucket_count) * bucket_count;
        rows = ((last_id - first_id) / bucket_count) + 1U;
        checksum = ((uint64_t)rows * ((uint64_t)first_id + (uint64_t)last_id)) / 2U +
                   (uint64_t)rows * (uint64_t)value;
    }
    *out_rows = rows;
    return checksum;
}

static size_t secondary_bucket_count(benchmark_context *ctx) {
    return ctx->config->rows < 10U ? 1U : 10U;
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
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_DIRECT_UPDATES,
            "direct primary-key updates in one transaction",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

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
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATES,
            "prepared primary-key updates in one transaction",
            ctx->config->iterations,
            monotonic_ns() - start_ns
        ) != 0) {
        goto rollback;
    }

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

static int benchmark_prepared_update_components(benchmark_context *ctx) {
    mylite_stmt *stmt = NULL;
    uint64_t bind_ns = 0U;
    uint64_t step_ns = 0U;
    uint64_t reset_ns = 0U;
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
        report_database_error(ctx, "prepare primary-key update components");
        goto rollback;
    }

    for (size_t i = 0; i < ctx->config->iterations; ++i) {
        const size_t id = (i % ctx->config->rows) + 1U;
        uint64_t start_ns = monotonic_ns();
        const int bind_result = mylite_bind_int64(stmt, 1U, (long long)id);
        bind_ns += monotonic_ns() - start_ns;
        if (bind_result != MYLITE_OK) {
            report_database_error(ctx, "bind prepared primary-key update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int step_result = mylite_step(stmt);
        step_ns += monotonic_ns() - start_ns;
        if (step_result != MYLITE_DONE) {
            fprintf(stderr, "Prepared primary-key update failed for id %zu\n", id);
            report_database_error(ctx, "prepared primary-key update component");
            goto rollback;
        }

        start_ns = monotonic_ns();
        const int reset_result = mylite_reset(stmt);
        reset_ns += monotonic_ns() - start_ns;
        if (reset_result != MYLITE_OK) {
            report_database_error(ctx, "reset prepared primary-key update component");
            goto rollback;
        }
    }

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_BIND,
            "prepared primary-key update bind component",
            ctx->config->iterations,
            bind_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_STEP,
            "prepared primary-key update step component",
            ctx->config->iterations,
            step_ns
        ) != 0) {
        goto rollback;
    }
    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_PREPARED_UPDATE_COMPONENT_RESET,
            "prepared primary-key update reset component",
            ctx->config->iterations,
            reset_ns
        ) != 0) {
        goto rollback;
    }

    if (mylite_finalize(stmt) != MYLITE_OK) {
        stmt = NULL;
        report_database_error(ctx, "finalize prepared primary-key update components");
        goto rollback;
    }
    stmt = NULL;
    if (exec_sql(ctx, "COMMIT") != 0) {
        return 1;
    }
    result = 0;

rollback:
    if (stmt != NULL && mylite_finalize(stmt) != MYLITE_OK) {
        report_database_error(ctx, "finalize prepared primary-key update components");
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

    if (print_result(
            ctx->config,
            BENCHMARK_METRIC_ORDERED_SCAN,
            "direct ordered full scan",
            scan.rows,
            monotonic_ns() - start_ns
        ) != 0) {
        return 1;
    }
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

static int secondary_callback(void *data, int column_count, char **values, char **column_names) {
    (void)column_names;
    secondary_result *result = data;
    if (column_count != 2 || values[0] == NULL || values[1] == NULL) {
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
