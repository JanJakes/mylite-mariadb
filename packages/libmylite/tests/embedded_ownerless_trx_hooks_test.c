#include <mylite/mylite.h>

#include "mylite_ownerless_trx_hooks.h"

#include <assert.h>
#include <ftw.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MYLITE_TEST_REMOVE_TREE_MAX_FDS 32
#define MYLITE_TEST_MAX_ACTIVE_TRX 128
#define MYLITE_TEST_UNASSIGNED_TRX_NO UINT64_MAX

typedef enum snapshot_shape {
    SNAPSHOT_SHAPE_NORMAL,
    SNAPSHOT_SHAPE_OVERLAP,
    SNAPSHOT_SHAPE_LOCAL_ONLY,
} snapshot_shape;

typedef struct active_trx {
    uint64_t id;
    uint64_t no;
    uint32_t rollback_state;
} active_trx;

typedef struct trx_hook_state {
    uint64_t next_id;
    active_trx active[MYLITE_TEST_MAX_ACTIVE_TRX];
    unsigned active_count;
    unsigned max_active_count;
    unsigned allocate_count;
    unsigned register_count;
    unsigned assign_no_count;
    unsigned deregister_count;
    unsigned rollback_state_count;
    unsigned savepoint_read_safe_count;
    unsigned deregistered_rollback_in_progress_count;
    unsigned snapshot_count;
    unsigned full_snapshot_count;
    unsigned snapshot_errors_remaining;
    unsigned injected_snapshot_error_count;
    snapshot_shape shape;
} trx_hook_state;

int mylite_test_ownerless_snapshot_ids(
    uint64_t *out_trx_ids,
    unsigned trx_id_capacity,
    unsigned *out_trx_id_count,
    uint64_t *out_low_limit_id,
    uint64_t *out_low_limit_no
);

static void test_trx_hooks_cover_innodb_sql(void);
static int allocate_trx_hook(uint64_t *out_trx_id, void *context);
static int register_trx_hook(uint64_t *out_trx_id, void *context);
static int assign_trx_no_hook(uint64_t trx_id, uint64_t *out_trx_no, void *context);
static int deregister_trx_hook(uint64_t trx_id, void *context);
static int snapshot_trx_hook(
    uint64_t *out_trx_ids,
    unsigned trx_id_capacity,
    unsigned *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no,
    void *context
);
static uint64_t next_trx_id_hook(void *context);
static int rollback_state_hook(uint64_t trx_id, uint32_t rollback_state, void *context);
static int recovery_state_hook(uint64_t trx_id, int *out_state, void *context);
static uint64_t allocate_next_id(trx_hook_state *state);
static int find_active_trx(const trx_hook_state *state, uint64_t trx_id);
static uint64_t min_active_trx_no(const trx_hook_state *state);
static void install_test_hooks(trx_hook_state *state);
static void assert_snapshot_shape(const trx_hook_state *state);
static mylite_db *open_database(const char *root, char **database_path);
static void exec_ok(mylite_db *db, const char *sql);
static char *make_temp_root(void);
static char *path_join(const char *directory, const char *name);
static void remove_tree(const char *path);
static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
);

int main(void) {
    test_trx_hooks_cover_innodb_sql();
    return 0;
}

static void test_trx_hooks_cover_innodb_sql(void) {
    char *root = make_temp_root();
    char *database_path = NULL;
    trx_hook_state state = {.next_id = 1000};
    mylite_db *db;

    db = open_database(root, &database_path);
    install_test_hooks(&state);

    exec_ok(db, "CREATE DATABASE app");
    exec_ok(
        db,
        "CREATE TABLE app.posts ("
        "id INT NOT NULL PRIMARY KEY, "
        "title VARCHAR(64) NOT NULL"
        ") ENGINE=InnoDB"
    );
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "INSERT INTO app.posts VALUES (1, 'draft')");
    assert(state.active_count == 1U);
    state.shape = SNAPSHOT_SHAPE_OVERLAP;
    state.snapshot_errors_remaining = 1U;
    assert_snapshot_shape(&state);
    assert(state.injected_snapshot_error_count == 1U);
    state.shape = SNAPSHOT_SHAPE_NORMAL;
    exec_ok(db, "SELECT title FROM app.posts WHERE id = 1");
    exec_ok(db, "COMMIT");
    exec_ok(db, "START TRANSACTION");
    exec_ok(db, "UPDATE app.posts SET title = 'published' WHERE id = 1");
    exec_ok(db, "SAVEPOINT before_second_update");
    exec_ok(db, "UPDATE app.posts SET title = 'review' WHERE id = 1");
    exec_ok(db, "ROLLBACK TO SAVEPOINT before_second_update");
    assert(state.savepoint_read_safe_count > 0U);
    {
        const unsigned safe_count = state.savepoint_read_safe_count;
        exec_ok(db, "INSERT IGNORE INTO app.posts VALUES (1, 'duplicate'), (2, 'second')");
        assert(state.savepoint_read_safe_count == safe_count);
    }
    assert(state.active_count == 1U);
    state.shape = SNAPSHOT_SHAPE_LOCAL_ONLY;
    assert_snapshot_shape(&state);
    state.shape = SNAPSHOT_SHAPE_NORMAL;
    exec_ok(db, "SELECT title FROM app.posts WHERE id = 1");
    exec_ok(db, "ROLLBACK");
    exec_ok(db, "SELECT title FROM app.posts WHERE id = 1");
    exec_ok(db, "DROP TABLE app.posts");
    exec_ok(db, "DROP DATABASE app");
    assert(mylite_close(db) == MYLITE_OK);

    assert(state.register_count > 0U);
    assert(state.assign_no_count > 0U);
    assert(state.snapshot_count > 0U);
    assert(state.full_snapshot_count > 0U);
    assert(state.allocate_count >= state.assign_no_count);
    assert(state.register_count == state.deregister_count);
    assert(state.rollback_state_count > 0U);
    assert(state.deregistered_rollback_in_progress_count > 0U);
    assert(state.active_count == 0U);
    assert(state.max_active_count > 0U);

    mylite_ownerless_trx_reset_hooks();
    assert(!mylite_ownerless_trx_has_hooks());

    free(database_path);
    remove_tree(root);
    free(root);
}

static int allocate_trx_hook(uint64_t *out_trx_id, void *context) {
    trx_hook_state *state = (trx_hook_state *)context;

    assert(out_trx_id != NULL);
    *out_trx_id = allocate_next_id(state);
    ++state->allocate_count;
    return MYLITE_OWNERLESS_TRX_OK;
}

static int register_trx_hook(uint64_t *out_trx_id, void *context) {
    trx_hook_state *state = (trx_hook_state *)context;

    assert(out_trx_id != NULL);
    assert(state->active_count < MYLITE_TEST_MAX_ACTIVE_TRX);
    *out_trx_id = allocate_next_id(state);
    state->active[state->active_count].id = *out_trx_id;
    state->active[state->active_count].no = MYLITE_TEST_UNASSIGNED_TRX_NO;
    state->active[state->active_count].rollback_state = MYLITE_OWNERLESS_TRX_ROLLBACK_NONE;
    ++state->active_count;
    ++state->register_count;
    if (state->active_count > state->max_active_count) {
        state->max_active_count = state->active_count;
    }
    return MYLITE_OWNERLESS_TRX_OK;
}

static int assign_trx_no_hook(uint64_t trx_id, uint64_t *out_trx_no, void *context) {
    trx_hook_state *state = (trx_hook_state *)context;
    const int index = find_active_trx(state, trx_id);

    assert(out_trx_no != NULL);
    assert(index >= 0);
    *out_trx_no = allocate_next_id(state);
    state->active[index].no = *out_trx_no;
    ++state->allocate_count;
    ++state->assign_no_count;
    return MYLITE_OWNERLESS_TRX_OK;
}

static int deregister_trx_hook(uint64_t trx_id, void *context) {
    trx_hook_state *state = (trx_hook_state *)context;
    const int index = find_active_trx(state, trx_id);

    assert(index >= 0);
    if (state->active[index].rollback_state == MYLITE_OWNERLESS_TRX_ROLLBACK_IN_PROGRESS) {
        ++state->deregistered_rollback_in_progress_count;
    }
    state->active[index] = state->active[state->active_count - 1U];
    --state->active_count;
    ++state->deregister_count;
    return MYLITE_OWNERLESS_TRX_OK;
}

static int snapshot_trx_hook(
    uint64_t *out_trx_ids,
    unsigned trx_id_capacity,
    unsigned *out_trx_id_count,
    uint64_t *out_next_trx_id,
    uint64_t *out_min_trx_no,
    void *context
) {
    trx_hook_state *state = (trx_hook_state *)context;
    unsigned required_count = state->active_count;
    unsigned i;

    assert(out_trx_id_count != NULL);
    assert(out_next_trx_id != NULL);
    assert(out_min_trx_no != NULL);
    ++state->snapshot_count;
    if (state->snapshot_errors_remaining > 0U) {
        --state->snapshot_errors_remaining;
        ++state->injected_snapshot_error_count;
        return MYLITE_OWNERLESS_TRX_ERROR;
    }
    if (state->active_count == 1U && state->shape == SNAPSHOT_SHAPE_OVERLAP) {
        required_count = 2U;
    } else if (state->active_count == 1U && state->shape == SNAPSHOT_SHAPE_LOCAL_ONLY) {
        required_count = 1U;
    }
    *out_trx_id_count = required_count;
    *out_next_trx_id = state->next_id;
    *out_min_trx_no = min_active_trx_no(state);
    if (required_count > trx_id_capacity) {
        ++state->full_snapshot_count;
        return MYLITE_OWNERLESS_TRX_FULL;
    }

    if (state->active_count == 1U && state->shape != SNAPSHOT_SHAPE_NORMAL) {
        const uint64_t active_id = state->active[0].id;

        assert(active_id > 1U);
        assert(out_trx_ids != NULL);
        if (state->shape == SNAPSHOT_SHAPE_OVERLAP) {
            out_trx_ids[0] = active_id;
            out_trx_ids[1] = active_id - 1U;
        } else {
            out_trx_ids[0] = active_id - 1U;
        }
        return MYLITE_OWNERLESS_TRX_OK;
    }

    for (i = 0; i < state->active_count; ++i) {
        assert(out_trx_ids != NULL);
        out_trx_ids[i] = state->active[i].id;
    }
    return MYLITE_OWNERLESS_TRX_OK;
}

static uint64_t next_trx_id_hook(void *context) {
    const trx_hook_state *state = (const trx_hook_state *)context;

    assert(state != NULL);
    return state->next_id;
}

static int rollback_state_hook(uint64_t trx_id, uint32_t rollback_state, void *context) {
    trx_hook_state *state = (trx_hook_state *)context;
    const int index = find_active_trx(state, trx_id);

    assert(rollback_state <= MYLITE_OWNERLESS_TRX_ROLLBACK_SAVEPOINT_READ_SAFE);
    if (index < 0) {
        assert(rollback_state == MYLITE_OWNERLESS_TRX_ROLLBACK_NONE);
        return MYLITE_OWNERLESS_TRX_OK;
    }
    state->active[index].rollback_state = rollback_state;
    ++state->rollback_state_count;
    if (rollback_state == MYLITE_OWNERLESS_TRX_ROLLBACK_SAVEPOINT_READ_SAFE) {
        ++state->savepoint_read_safe_count;
    }
    return MYLITE_OWNERLESS_TRX_OK;
}

static int recovery_state_hook(uint64_t trx_id, int *out_state, void *context) {
    const trx_hook_state *state = (const trx_hook_state *)context;

    assert(state != NULL);
    assert(out_state != NULL);
    *out_state = find_active_trx(state, trx_id) >= 0 ? MYLITE_OWNERLESS_TRX_RECOVERY_LIVE_REMOTE
                                                     : MYLITE_OWNERLESS_TRX_RECOVERY_ABSENT;
    return MYLITE_OWNERLESS_TRX_OK;
}

static uint64_t allocate_next_id(trx_hook_state *state) {
    const uint64_t trx_id = state->next_id;

    assert(trx_id < UINT64_MAX);
    ++state->next_id;
    return trx_id;
}

static int find_active_trx(const trx_hook_state *state, uint64_t trx_id) {
    unsigned i;

    for (i = 0; i < state->active_count; ++i) {
        if (state->active[i].id == trx_id) {
            return (int)i;
        }
    }
    return -1;
}

static uint64_t min_active_trx_no(const trx_hook_state *state) {
    uint64_t min_no = state->next_id;
    unsigned i;

    for (i = 0; i < state->active_count; ++i) {
        if (state->active[i].no < min_no) {
            min_no = state->active[i].no;
        }
    }
    return min_no;
}

static void install_test_hooks(trx_hook_state *state) {
    mylite_ownerless_trx_set_hooks(
        allocate_trx_hook,
        register_trx_hook,
        assign_trx_no_hook,
        deregister_trx_hook,
        snapshot_trx_hook,
        next_trx_id_hook,
        rollback_state_hook,
        recovery_state_hook,
        state
    );
    assert(mylite_ownerless_trx_has_hooks());
}

static void assert_snapshot_shape(const trx_hook_state *state) {
    uint64_t ids[4] = {0};
    uint64_t low_limit_id = 0;
    uint64_t low_limit_no = 0;
    unsigned id_count = 0;
    const uint64_t active_id = state->active[0].id;

    assert(state->active_count == 1U);
    assert(
        mylite_test_ownerless_snapshot_ids(ids, 4U, &id_count, &low_limit_id, &low_limit_no) == 0
    );
    assert(id_count == 2U);
    assert(ids[0] == active_id - 1U);
    assert(ids[1] == active_id);
    assert(ids[0] < ids[1]);
    assert(ids[1] < low_limit_id);
    assert(low_limit_id == state->next_id);
    assert(low_limit_no == min_active_trx_no(state));
}

static mylite_db *open_database(const char *root, char **database_path) {
    char *runtime_root = path_join(root, "runtime");
    mylite_open_config config = {
        .size = sizeof(config),
        .profile = MYLITE_PROFILE_DEFAULT,
        .busy_timeout_ms = 0,
        .durability = MYLITE_DURABILITY_FULL,
        .temp_directory = runtime_root,
    };
    mylite_db *db = NULL;

    assert(mkdir(runtime_root, 0700) == 0);
    *database_path = path_join(root, "trx-hooks.mylite");
    assert(
        mylite_open(*database_path, &db, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, &config) ==
        MYLITE_OK
    );
    free(runtime_root);
    return db;
}

static void exec_ok(mylite_db *db, const char *sql) {
    assert(mylite_exec(db, sql, NULL, NULL, NULL) == MYLITE_OK);
}

static char *make_temp_root(void) {
    char template_path[] = "/tmp/mylite-trx-hooks.XXXXXX";
    char *root = mkdtemp(template_path);
    char *copy;

    assert(root != NULL);
    copy = strdup(root);
    assert(copy != NULL);
    return copy;
}

static char *path_join(const char *directory, const char *name) {
    const size_t directory_len = strlen(directory);
    const size_t name_len = strlen(name);
    char *path = (char *)malloc(directory_len + name_len + 2U);

    assert(path != NULL);
    memcpy(path, directory, directory_len);
    path[directory_len] = '/';
    memcpy(path + directory_len + 1U, name, name_len + 1U);
    return path;
}

static void remove_tree(const char *path) {
    assert(
        nftw(path, remove_tree_entry, MYLITE_TEST_REMOVE_TREE_MAX_FDS, FTW_DEPTH | FTW_PHYS) == 0
    );
}

static int remove_tree_entry(
    const char *path,
    const struct stat *path_stat,
    int type_flag,
    struct FTW *walk
) {
    (void)path_stat;
    (void)walk;
    return type_flag == FTW_DP ? rmdir(path) : unlink(path);
}
