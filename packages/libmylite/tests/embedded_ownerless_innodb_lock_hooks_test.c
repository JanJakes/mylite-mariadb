#include "mylite_ownerless_innodb_lock_hooks.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

typedef struct page_visibility_state {
    uint64_t last_max_commit_lsn;
    uint64_t next_reserved_lsn;
    uint64_t last_reserved_length;
    uint64_t written_lsn;
    uint64_t last_written_start_lsn;
    uint64_t last_written_end_lsn;
    uint64_t observed_lsn;
    unsigned read_count;
    unsigned reserve_count;
    unsigned written_count;
    unsigned observe_count;
} page_visibility_state;

static void test_page_visibility_is_thread_local(void);
static void install_page_hooks(page_visibility_state *state);
static void *exercise_visibility_in_thread(void *context);
static int acquire_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
    void *context
);
static int release_table_hook(uint64_t trx_id, uint64_t table_id, uint32_t mode, void *context);
static int wait_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    uint64_t blocker_trx_id,
    void *context
);
static int wait_until_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
    void *context
);
static int acquire_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context
);
static int acquire_page_write_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags,
    void *context
);
static int release_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    void *context
);
static int release_page_writes_hook(uint64_t trx_id, void *context);
static int wait_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    uint64_t blocker_trx_id,
    void *context
);
static int wait_until_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context
);
static int clear_wait_hook(uint64_t trx_id, void *context);
static int redo_enter_hook(uint64_t *out_latest_lsn, void *context);
static int redo_observe_hook(uint64_t *out_latest_lsn, void *context);
static int redo_reserve_hook(
    uint64_t current_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn,
    void *context
);
static int redo_written_hook(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn,
    void *context
);
static void redo_leave_hook(uint64_t latest_lsn, void *context);
static void pages_visible_hook(uint64_t visible_lsn, void *context);
static int page_publish_hook(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    void *context
);
static int page_read_hook(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    void *context
);

int main(void) {
    test_page_visibility_is_thread_local();
    return 0;
}

static void test_page_visibility_is_thread_local(void) {
    page_visibility_state state = {.next_reserved_lsn = 200U};
    unsigned char page[16];
    pthread_t thread;
    uint64_t start_lsn = 0U;
    uint64_t end_lsn = 0U;

    install_page_hooks(&state);
    assert(mylite_ownerless_innodb_lock_has_hooks());
    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );

    mylite_ownerless_innodb_enable_external_page_visibility(100U);
    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(state.last_max_commit_lsn == 100U);
    assert(state.read_count == 1U);
    assert(
        mylite_ownerless_innodb_redo_reserve(150U, 12U, &start_lsn, &end_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(start_lsn == 200U);
    assert(end_lsn == 212U);
    assert(state.last_reserved_length == 12U);
    assert(state.reserve_count == 1U);
    assert(
        mylite_ownerless_innodb_redo_written(200U, 212U, &end_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(end_lsn == 212U);
    assert(state.written_lsn == 212U);
    assert(state.last_written_start_lsn == 200U);
    assert(state.last_written_end_lsn == 212U);
    assert(state.written_count == 1U);
    assert(mylite_ownerless_innodb_redo_observe(&end_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(end_lsn == 212U);
    assert(state.observed_lsn == 212U);
    assert(state.observe_count == 3U);

    assert(pthread_create(&thread, NULL, exercise_visibility_in_thread, &state) == 0);
    assert(pthread_join(thread, NULL) == 0);

    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(state.last_max_commit_lsn == 100U);
    assert(state.read_count == 3U);

    mylite_ownerless_innodb_clear_external_page_visibility();
    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );
    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_lock_has_hooks());
}

static void install_page_hooks(page_visibility_state *state) {
    mylite_ownerless_innodb_lock_set_hooks(
        acquire_table_hook,
        release_table_hook,
        wait_table_hook,
        acquire_record_hook,
        release_record_hook,
        acquire_page_write_hook,
        release_record_hook,
        release_page_writes_hook,
        wait_record_hook,
        wait_until_table_hook,
        wait_until_record_hook,
        clear_wait_hook,
        redo_enter_hook,
        redo_observe_hook,
        redo_reserve_hook,
        redo_written_hook,
        redo_leave_hook,
        pages_visible_hook,
        page_publish_hook,
        page_read_hook,
        state
    );
}

static void *exercise_visibility_in_thread(void *context) {
    page_visibility_state *state = (page_visibility_state *)context;
    unsigned char page[16];

    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );
    mylite_ownerless_innodb_enable_external_page_visibility(25U);
    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(state->last_max_commit_lsn == 25U);
    assert(state->read_count == 2U);
    mylite_ownerless_innodb_clear_external_page_visibility();
    assert(
        mylite_ownerless_innodb_read_page_version(1U, 2U, page, sizeof(page)) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );
    return NULL;
}

static int acquire_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
    void *context
) {
    (void)trx_id;
    (void)table_id;
    (void)mode;
    (void)timeout_ms;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int release_table_hook(uint64_t trx_id, uint64_t table_id, uint32_t mode, void *context) {
    (void)trx_id;
    (void)table_id;
    (void)mode;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int wait_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    uint64_t blocker_trx_id,
    void *context
) {
    (void)trx_id;
    (void)table_id;
    (void)mode;
    (void)blocker_trx_id;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int wait_until_table_hook(
    uint64_t trx_id,
    uint64_t table_id,
    uint32_t mode,
    unsigned int timeout_ms,
    void *context
) {
    (void)trx_id;
    (void)table_id;
    (void)mode;
    (void)timeout_ms;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int acquire_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context
) {
    (void)trx_id;
    (void)index_id;
    (void)space_id;
    (void)page_no;
    (void)heap_no;
    (void)mode;
    (void)flags;
    (void)timeout_ms;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int acquire_page_write_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags,
    void *context
) {
    if (out_acquire_flags != NULL) {
        *out_acquire_flags = 0U;
    }
    return acquire_record_hook(
        trx_id,
        index_id,
        space_id,
        page_no,
        heap_no,
        mode,
        flags,
        timeout_ms,
        context
    );
}

static int release_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    void *context
) {
    (void)trx_id;
    (void)index_id;
    (void)space_id;
    (void)page_no;
    (void)heap_no;
    (void)mode;
    (void)flags;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int release_page_writes_hook(uint64_t trx_id, void *context) {
    (void)trx_id;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int wait_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    uint64_t blocker_trx_id,
    void *context
) {
    (void)trx_id;
    (void)index_id;
    (void)space_id;
    (void)page_no;
    (void)heap_no;
    (void)mode;
    (void)flags;
    (void)blocker_trx_id;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int wait_until_record_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    unsigned int timeout_ms,
    void *context
) {
    (void)trx_id;
    (void)index_id;
    (void)space_id;
    (void)page_no;
    (void)heap_no;
    (void)mode;
    (void)flags;
    (void)timeout_ms;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int clear_wait_hook(uint64_t trx_id, void *context) {
    (void)trx_id;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int redo_enter_hook(uint64_t *out_latest_lsn, void *context) {
    (void)context;
    if (out_latest_lsn != NULL) {
        *out_latest_lsn = 0U;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int redo_observe_hook(uint64_t *out_latest_lsn, void *context) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(out_latest_lsn != NULL);
    state->observed_lsn = state->written_lsn;
    ++state->observe_count;
    *out_latest_lsn = state->observed_lsn;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int redo_reserve_hook(
    uint64_t current_lsn,
    uint64_t length,
    uint64_t *out_start_lsn,
    uint64_t *out_end_lsn,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(length != 0U);
    assert(out_start_lsn != NULL);
    assert(out_end_lsn != NULL);

    if (state->next_reserved_lsn < current_lsn) {
        state->next_reserved_lsn = current_lsn;
    }
    *out_start_lsn = state->next_reserved_lsn;
    *out_end_lsn = state->next_reserved_lsn + length;
    state->next_reserved_lsn = *out_end_lsn;
    state->last_reserved_length = length;
    ++state->reserve_count;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int redo_written_hook(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(start_lsn != 0U);
    assert(end_lsn > start_lsn);

    state->written_lsn = end_lsn;
    state->last_written_start_lsn = start_lsn;
    state->last_written_end_lsn = end_lsn;
    ++state->written_count;
    if (out_written_lsn != NULL) {
        *out_written_lsn = state->written_lsn;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static void redo_leave_hook(uint64_t latest_lsn, void *context) {
    (void)latest_lsn;
    (void)context;
}

static void pages_visible_hook(uint64_t visible_lsn, void *context) {
    (void)visible_lsn;
    (void)context;
}

static int page_publish_hook(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    void *context
) {
    (void)space_id;
    (void)page_no;
    (void)page_lsn;
    (void)visible_lsn;
    (void)page;
    (void)page_size;
    (void)context;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int page_read_hook(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(space_id == 1U);
    assert(page_no == 2U);
    assert(page != NULL);
    assert(out_page_size != NULL);
    assert(out_page_lsn != NULL);
    assert(out_commit_lsn != NULL);
    memset(page, 0, page_capacity);
    state->last_max_commit_lsn = max_commit_lsn;
    ++state->read_count;
    *out_page_size = page_capacity;
    *out_page_lsn = max_commit_lsn;
    *out_commit_lsn = max_commit_lsn;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}
