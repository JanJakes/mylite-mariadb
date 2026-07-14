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
    uint64_t last_leave_lsn;
    uint64_t last_written_leave_latest_lsn;
    uint64_t last_batch_latest_lsn;
    uint64_t observed_lsn;
    uint64_t last_table_wait_trx_id;
    uint64_t last_table_wait_table_id;
    uint32_t last_table_wait_mode;
    unsigned int last_table_wait_timeout_ms;
    unsigned read_count;
    unsigned reserve_count;
    unsigned written_count;
    unsigned written_leave_count;
    unsigned written_leave_batch_count;
    unsigned last_batch_range_count;
    unsigned last_batch_completed_count;
    unsigned leave_count;
    unsigned observe_count;
    unsigned table_wait_count;
    int table_wait_result;
} page_visibility_state;

static void test_page_visibility_is_thread_local(void);
static void test_checkpoint_suppression_and_file_op_flags_reset(void);
static void test_file_op_redo_relative_path_normalizes_datadir_prefix(void);
static void test_external_table_wait_dispatch_uses_table_hook(void);
static void test_redo_written_leave_uses_fused_top_level_hook(void);
static void test_redo_written_leave_falls_back_to_separate_hooks(void);
static void test_redo_deferred_flush_uses_batch_hook(void);
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
static int before_record_wait_hook(
    uint64_t trx_id,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t mode,
    uint32_t flags,
    void *context
);
static int clear_wait_hook(uint64_t trx_id, void *context);
static int redo_enter_hook(uint64_t *out_latest_lsn, void *context);
static int redo_observe_hook(uint64_t *out_latest_lsn, void *context);
static int redo_observe_visible_hook(uint64_t *out_visible_lsn, void *context);
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
static int redo_written_leave_hook(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    void *context
);
static int redo_written_leave_batch_hook(
    const mylite_ownerless_innodb_redo_range *ranges,
    size_t range_count,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    size_t *out_completed_count,
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
    uint32_t publish_flags,
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
    uint32_t *out_record_flags,
    uint32_t read_options,
    void *context
);
static int page_write_active_hook(
    uint32_t space_id,
    uint32_t page_no,
    int *out_active,
    void *context
);
static int skip_external_page_refresh_hook(void *context);

int main(void) {
    test_checkpoint_suppression_and_file_op_flags_reset();
    test_file_op_redo_relative_path_normalizes_datadir_prefix();
    test_external_table_wait_dispatch_uses_table_hook();
    test_redo_written_leave_uses_fused_top_level_hook();
    test_redo_written_leave_falls_back_to_separate_hooks();
    test_redo_deferred_flush_uses_batch_hook();
    test_page_visibility_is_thread_local();
    return 0;
}

static void test_checkpoint_suppression_and_file_op_flags_reset(void) {
    assert(!mylite_ownerless_innodb_checkpoint_suppressed());
    assert(!mylite_ownerless_innodb_relative_file_op_redo_paths());
    assert(!mylite_ownerless_innodb_take_file_op_redo());
    assert(!mylite_ownerless_innodb_take_file_rename_redo());

    mylite_ownerless_innodb_set_checkpoint_suppression(1);
    mylite_ownerless_innodb_set_relative_file_op_redo_paths(1);
    mylite_ownerless_innodb_note_file_op_redo();
    assert(mylite_ownerless_innodb_take_file_op_redo());
    assert(!mylite_ownerless_innodb_take_file_op_redo());
    mylite_ownerless_innodb_note_file_rename_redo();
    assert(mylite_ownerless_innodb_checkpoint_suppressed());
    assert(mylite_ownerless_innodb_relative_file_op_redo_paths());
    assert(mylite_ownerless_innodb_take_file_rename_redo());
    assert(!mylite_ownerless_innodb_take_file_rename_redo());

    mylite_ownerless_innodb_set_checkpoint_suppression(0);
    mylite_ownerless_innodb_set_relative_file_op_redo_paths(0);
    assert(!mylite_ownerless_innodb_checkpoint_suppressed());
    assert(!mylite_ownerless_innodb_relative_file_op_redo_paths());

    mylite_ownerless_innodb_set_checkpoint_suppression(1);
    mylite_ownerless_innodb_set_relative_file_op_redo_paths(1);
    mylite_ownerless_innodb_note_file_rename_redo();
    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_checkpoint_suppressed());
    assert(!mylite_ownerless_innodb_relative_file_op_redo_paths());
    assert(!mylite_ownerless_innodb_take_file_op_redo());
    assert(!mylite_ownerless_innodb_take_file_rename_redo());
}

static void test_file_op_redo_relative_path_normalizes_datadir_prefix(void) {
    char relative[128];
    char small[8];

    memset(relative, 0, sizeof(relative));
    assert(mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "/tmp/app.mylite/datadir/app/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(strcmp(relative, "app/t.ibd") == 0);

    memset(relative, 0, sizeof(relative));
    assert(mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir/",
        "/tmp/app.mylite/datadir//app/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(strcmp(relative, "app/t.ibd") == 0);

    memset(relative, 0, sizeof(relative));
    assert(mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "tmp/app.mylite/datadir/app/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(strcmp(relative, "app/t.ibd") == 0);

    assert(!mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "app/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(!mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "/tmp/other.mylite/datadir/app/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(!mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "/tmp/app.mylite/datadir/app/t.frm",
        relative,
        sizeof(relative)
    ));
    assert(!mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "/tmp/app.mylite/datadir/t.ibd",
        relative,
        sizeof(relative)
    ));
    assert(!mylite_ownerless_innodb_file_op_redo_relative_path(
        "/tmp/app.mylite/datadir",
        "/tmp/app.mylite/datadir/app/t.ibd",
        small,
        sizeof(small)
    ));
}

static void test_external_table_wait_dispatch_uses_table_hook(void) {
    page_visibility_state state = {0};
    struct mylite_ownerless_innodb_lock_external_wait wait = {0};
    struct mylite_ownerless_innodb_lock_external_wait none = {0};

    wait.kind = MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE;
    wait.trx_id = 17U;
    wait.table_id = 23U;
    wait.mode = MYLITE_OWNERLESS_INNODB_LOCK_MODE_X;

    assert(
        mylite_ownerless_innodb_lock_wait_for_external(&wait, 10U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );
    assert(
        mylite_ownerless_innodb_lock_wait_for_external(&none, 10U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(
        mylite_ownerless_innodb_lock_wait_for_external(NULL, 10U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_ERROR
    );

    install_page_hooks(&state);
    assert(mylite_ownerless_innodb_lock_has_hooks());
    assert(
        mylite_ownerless_innodb_lock_wait_for_external(&wait, 250U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(state.table_wait_count == 1U);
    assert(state.last_table_wait_trx_id == 17U);
    assert(state.last_table_wait_table_id == 23U);
    assert(state.last_table_wait_mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_X);
    assert(state.last_table_wait_timeout_ms == 250U);

    state.table_wait_result = MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
    wait.trx_id = 19U;
    wait.table_id = 29U;
    wait.mode = MYLITE_OWNERLESS_INNODB_LOCK_MODE_S;
    assert(
        mylite_ownerless_innodb_lock_wait_for_external(&wait, 7U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT
    );
    assert(state.table_wait_count == 2U);
    assert(state.last_table_wait_trx_id == 19U);
    assert(state.last_table_wait_table_id == 29U);
    assert(state.last_table_wait_mode == MYLITE_OWNERLESS_INNODB_LOCK_MODE_S);
    assert(state.last_table_wait_timeout_ms == 7U);

    wait.kind = 99U;
    assert(
        mylite_ownerless_innodb_lock_wait_for_external(&wait, 1U) ==
        MYLITE_OWNERLESS_INNODB_LOCK_ERROR
    );

    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_lock_has_hooks());
}

static void test_redo_written_leave_uses_fused_top_level_hook(void) {
    page_visibility_state state = {0};
    uint64_t latest_lsn = 0U;
    uint64_t written_lsn = 0U;

    install_page_hooks(&state);
    mylite_ownerless_innodb_lock_set_redo_written_leave_hook(redo_written_leave_hook);
    assert(mylite_ownerless_innodb_lock_has_hooks());
    assert(!mylite_ownerless_innodb_redo_is_active());
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(mylite_ownerless_innodb_redo_is_active());
    assert(
        mylite_ownerless_innodb_redo_written_and_leave(200U, 212U, 250U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(written_lsn == 212U);
    assert(!mylite_ownerless_innodb_redo_is_active());
    assert(state.written_leave_count == 1U);
    assert(state.written_count == 0U);
    assert(state.leave_count == 0U);
    assert(state.last_written_start_lsn == 200U);
    assert(state.last_written_end_lsn == 212U);
    assert(state.last_written_leave_latest_lsn == 250U);

    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_lock_has_hooks());
}

static void test_redo_written_leave_falls_back_to_separate_hooks(void) {
    page_visibility_state state = {0};
    uint64_t latest_lsn = 0U;
    uint64_t written_lsn = 0U;

    install_page_hooks(&state);
    assert(mylite_ownerless_innodb_lock_has_hooks());
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_written_and_leave(300U, 312U, 350U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(written_lsn == 312U);
    assert(!mylite_ownerless_innodb_redo_is_active());
    assert(state.written_leave_count == 0U);
    assert(state.written_count == 1U);
    assert(state.leave_count == 1U);
    assert(state.last_written_start_lsn == 300U);
    assert(state.last_written_end_lsn == 312U);
    assert(state.last_leave_lsn == 350U);

    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_lock_has_hooks());
}

static void test_redo_deferred_flush_uses_batch_hook(void) {
    page_visibility_state state = {0};
    uint64_t latest_lsn = 0U;
    uint64_t written_lsn = 0U;

    install_page_hooks(&state);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(1) == 0);
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_defer_written_and_leave(380U, 392U, 430U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
    );
    assert(
        mylite_ownerless_innodb_redo_written_and_leave(380U, 392U, 430U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(written_lsn == 392U);
    assert(!mylite_ownerless_innodb_redo_is_active());
    assert(state.written_leave_count == 0U);
    assert(state.written_count == 1U);
    assert(state.leave_count == 1U);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(0) == 1);
    mylite_ownerless_innodb_lock_reset_hooks();
    memset(&state, 0, sizeof(state));
    written_lsn = 0U;
    latest_lsn = 0U;

    install_page_hooks(&state);
    mylite_ownerless_innodb_lock_set_redo_written_leave_hook(redo_written_leave_hook);
    mylite_ownerless_innodb_lock_set_redo_written_leave_batch_hook(redo_written_leave_batch_hook);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(1) == 0);
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_defer_written_and_leave(400U, 412U, 450U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_defer_written_and_leave(412U, 424U, 460U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(state.written_leave_count == 0U);
    assert(state.written_leave_batch_count == 0U);
    assert(mylite_ownerless_innodb_redo_flush_deferred() == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(state.written_leave_batch_count == 1U);
    assert(state.last_batch_range_count == 2U);
    assert(state.last_batch_completed_count == 2U);
    assert(state.written_leave_count == 0U);
    assert(state.written_lsn == 424U);
    assert(state.last_batch_latest_lsn == 460U);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(0) == 1);

    memset(&state, 0, sizeof(state));
    install_page_hooks(&state);
    mylite_ownerless_innodb_lock_set_redo_written_leave_hook(redo_written_leave_hook);
    mylite_ownerless_innodb_lock_set_redo_written_leave_batch_hook(redo_written_leave_batch_hook);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(1) == 0);
    for (unsigned index = 0U; index < MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES; ++index) {
        const uint64_t start_lsn = 600U + ((uint64_t)index * 12U);
        assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
        assert(
            mylite_ownerless_innodb_redo_defer_written_and_leave(
                start_lsn,
                start_lsn + 12U,
                700U + index,
                &written_lsn
            ) == MYLITE_OWNERLESS_INNODB_LOCK_OK
        );
    }
    assert(state.written_leave_count == 0U);
    assert(state.written_leave_batch_count == 0U);
    assert(mylite_ownerless_innodb_redo_flush_deferred() == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(state.written_leave_batch_count == 1U);
    assert(state.last_batch_range_count == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);
    assert(state.last_batch_completed_count == MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES);
    assert(state.written_leave_count == 0U);
    assert(
        state.written_lsn == 600U + ((uint64_t)MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES * 12U)
    );
    assert(
        state.last_batch_latest_lsn == 700U + MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES - 1U
    );
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(0) == 1);

    memset(&state, 0, sizeof(state));
    install_page_hooks(&state);
    mylite_ownerless_innodb_lock_set_redo_written_leave_hook(redo_written_leave_hook);
    mylite_ownerless_innodb_lock_set_redo_written_leave_batch_hook(NULL);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(1) == 0);
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_defer_written_and_leave(500U, 512U, 550U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(mylite_ownerless_innodb_redo_enter(&latest_lsn) == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(
        mylite_ownerless_innodb_redo_defer_written_and_leave(512U, 524U, 560U, &written_lsn) ==
        MYLITE_OWNERLESS_INNODB_LOCK_OK
    );
    assert(mylite_ownerless_innodb_redo_flush_deferred() == MYLITE_OWNERLESS_INNODB_LOCK_OK);
    assert(state.written_leave_batch_count == 0U);
    assert(state.written_leave_count == 2U);
    assert(state.written_lsn == 524U);
    assert(state.last_written_leave_latest_lsn == 560U);
    assert(mylite_ownerless_innodb_set_statement_deferred_page_publish(0) == 1);

    mylite_ownerless_innodb_lock_reset_hooks();
    assert(!mylite_ownerless_innodb_lock_has_hooks());
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
        before_record_wait_hook,
        clear_wait_hook,
        redo_enter_hook,
        redo_observe_hook,
        redo_observe_visible_hook,
        redo_reserve_hook,
        redo_written_hook,
        redo_leave_hook,
        pages_visible_hook,
        page_publish_hook,
        page_read_hook,
        page_write_active_hook,
        skip_external_page_refresh_hook,
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
    page_visibility_state *state = (page_visibility_state *)context;

    assert(state != NULL);
    state->last_table_wait_trx_id = trx_id;
    state->last_table_wait_table_id = table_id;
    state->last_table_wait_mode = mode;
    state->last_table_wait_timeout_ms = timeout_ms;
    ++state->table_wait_count;
    return state->table_wait_result;
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

static int before_record_wait_hook(
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

static int redo_observe_visible_hook(uint64_t *out_visible_lsn, void *context) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(out_visible_lsn != NULL);
    *out_visible_lsn = state->written_lsn;
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

static int redo_written_leave_hook(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(start_lsn != 0U);
    assert(end_lsn > start_lsn);

    state->written_lsn = end_lsn;
    state->last_written_start_lsn = start_lsn;
    state->last_written_end_lsn = end_lsn;
    state->last_written_leave_latest_lsn = latest_lsn;
    ++state->written_leave_count;
    if (out_written_lsn != NULL) {
        *out_written_lsn = state->written_lsn;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int redo_written_leave_batch_hook(
    const mylite_ownerless_innodb_redo_range *ranges,
    size_t range_count,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn,
    size_t *out_completed_count,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(state != NULL);
    assert(ranges != NULL);
    assert(range_count != 0U);

    ++state->written_leave_batch_count;
    state->last_batch_range_count = (unsigned)range_count;
    state->last_batch_latest_lsn = latest_lsn;
    for (size_t index = 0; index < range_count; ++index) {
        assert(ranges[index].start_lsn != 0U);
        assert(ranges[index].end_lsn > ranges[index].start_lsn);
        state->last_written_start_lsn = ranges[index].start_lsn;
        state->last_written_end_lsn = ranges[index].end_lsn;
        state->written_lsn = ranges[index].end_lsn;
    }
    state->last_batch_completed_count = (unsigned)range_count;
    if (out_written_lsn != NULL) {
        *out_written_lsn = state->written_lsn;
    }
    if (out_completed_count != NULL) {
        *out_completed_count = range_count;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static void redo_leave_hook(uint64_t latest_lsn, void *context) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(state != NULL);
    state->last_leave_lsn = latest_lsn;
    ++state->leave_count;
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
    uint32_t publish_flags,
    void *context
) {
    (void)space_id;
    (void)page_no;
    (void)page_lsn;
    (void)visible_lsn;
    (void)page;
    (void)page_size;
    (void)publish_flags;
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
    uint32_t *out_record_flags,
    uint32_t read_options,
    void *context
) {
    page_visibility_state *state = (page_visibility_state *)context;

    assert(space_id == 1U);
    assert(page_no == 2U);
    assert(page != NULL);
    assert(out_page_size != NULL);
    assert(out_page_lsn != NULL);
    assert(out_commit_lsn != NULL);
    assert(out_record_flags != NULL);
    (void)read_options;
    memset(page, 0, page_capacity);
    state->last_max_commit_lsn = max_commit_lsn;
    ++state->read_count;
    *out_page_size = page_capacity;
    *out_page_lsn = max_commit_lsn;
    *out_commit_lsn = max_commit_lsn;
    *out_record_flags = 0U;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int page_write_active_hook(
    uint32_t space_id,
    uint32_t page_no,
    int *out_active,
    void *context
) {
    (void)space_id;
    (void)page_no;
    (void)context;
    assert(out_active != NULL);
    *out_active = 0;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int skip_external_page_refresh_hook(void *context) {
    (void)context;
    return 0;
}
