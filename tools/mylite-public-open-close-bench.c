#define _POSIX_C_SOURCE 200809L

#include <mylite/mylite.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static double monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void ensure_directory(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s): %s\n", path, strerror(errno));
        exit(2);
    }
}

static void fail_open(const char *phase, int result, mylite_db *db) {
    fprintf(
        stderr,
        "%s failed: result=%d errmsg=%s\n",
        phase,
        result,
        db != NULL ? mylite_errmsg(db) : "database handle unavailable"
    );
    if (db != NULL) {
        (void)mylite_close(db);
    }
    exit(1);
}

static void fail_exec(const char *sql, int result, mylite_db *db, char *errmsg) {
    fprintf(
        stderr,
        "exec failed: result=%d sql=%s errmsg=%s db_errmsg=%s\n",
        result,
        sql,
        errmsg != NULL ? errmsg : "none",
        db != NULL ? mylite_errmsg(db) : "database handle unavailable"
    );
    free(errmsg);
    if (db != NULL) {
        (void)mylite_close(db);
    }
    exit(1);
}

static void run_sql(mylite_db *db, const char *sql) {
    char *errmsg = NULL;
    const int result = mylite_exec(db, sql, NULL, NULL, &errmsg);
    if (result != MYLITE_OK) {
        fail_exec(sql, result, db, errmsg);
    }
    free(errmsg);
}

static mylite_open_config open_config(const char *runtime_root) {
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    return config;
}

static void prepare_database(const char *database_path, const mylite_open_config *config) {
    mylite_db *db = NULL;
    int result =
        mylite_open(database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, config);
    if (result != MYLITE_OK) {
        fail_open("prepare open", result, db);
    }

    run_sql(db, "CREATE DATABASE IF NOT EXISTS app");
    run_sql(
        db,
        "CREATE TABLE IF NOT EXISTS app.public_open_close_probe "
        "(id INT PRIMARY KEY, value INT NOT NULL) ENGINE=InnoDB"
    );
    run_sql(db, "INSERT IGNORE INTO app.public_open_close_probe VALUES (1, 1)");

    result = mylite_close(db);
    if (result != MYLITE_OK) {
        fail_open("prepare close", result, db);
    }
}

static double run_warm_open_close(
    const char *database_path,
    const mylite_open_config *config,
    unsigned iterations
) {
    const double started_ms = monotonic_ms();
    for (unsigned i = 0; i < iterations; ++i) {
        mylite_db *db = NULL;
        int result = mylite_open(database_path, &db, MYLITE_OPEN_READWRITE, config);
        if (result != MYLITE_OK) {
            fail_open("warm open", result, db);
        }
        result = mylite_close(db);
        if (result != MYLITE_OK) {
            fail_open("warm close", result, db);
        }
    }
    return monotonic_ms() - started_ms;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <database-path> <runtime-root> <iterations>\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    const unsigned long parsed_iterations = strtoul(argv[3], &end, 10);
    if (argv[3][0] == '\0' || end == NULL || *end != '\0' || parsed_iterations == 0UL ||
        parsed_iterations > 1000000UL) {
        fprintf(stderr, "invalid iteration count: %s\n", argv[3]);
        return 2;
    }

    ensure_directory(argv[2]);
    const mylite_open_config config = open_config(argv[2]);

    const double prepare_started_ms = monotonic_ms();
    prepare_database(argv[1], &config);
    const double prepare_ms = monotonic_ms() - prepare_started_ms;

    const double warm_total_ms = run_warm_open_close(argv[1], &config, (unsigned)parsed_iterations);

    printf("mylite_public_open_close_prepare_ms=%.3f\n", prepare_ms);
    printf("mylite_public_open_close_iterations=%lu\n", parsed_iterations);
    printf("mylite_public_open_close_total_ms=%.3f\n", warm_total_ms);
    printf("mylite_public_open_close_avg_ms=%.3f\n", warm_total_ms / (double)parsed_iterations);
    return 0;
}
