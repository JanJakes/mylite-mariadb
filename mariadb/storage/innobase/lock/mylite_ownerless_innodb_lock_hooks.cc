#define LOCK_MODULE_IMPLEMENTATION
#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include "mylite_ownerless_innodb_lock_hooks.h"

#include "btr0btr.h"
#include "btr0sea.h"
#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0lru.h"
#include "dict0dict.h"
#include "dict0load.h"
#include "dict0mem.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fut0lst.h"
#include "lock0lock.h"
#include "lock0priv.h"
#include "log0log.h"
#include "mtr0mtr.h"
#include "mylite_ownerless_innodb_deep_perf.h"
#include "os0file.h"
#include "pars0pars.h"
#include "page0page.h"
#include "que0que.h"
#include "row0mysql.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sql_class.h" // THD
#include "trx0roll.h"
#include "trx0sys.h"
#include "trx0trx.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

std::atomic<bool> mylite_ownerless_innodb_lock_hooks_enabled{false};
std::atomic<bool> mylite_ownerless_innodb_lock_hooks_ever_enabled{false};
std::atomic<bool> mylite_ownerless_innodb_autoinc_hooks_enabled{false};
std::atomic<bool> mylite_ownerless_innodb_test_faults_enabled{false};
std::atomic<uint64_t> mylite_ownerless_innodb_startup_lsn_advance_limit{0};
std::atomic<bool> mylite_ownerless_innodb_startup_lsn_advance_enabled{false};
std::atomic<uint64_t> mylite_ownerless_innodb_startup_lsn_limit_rejections{0};
thread_local bool mylite_ownerless_statement_execution_active= false;
thread_local bool mylite_ownerless_statement_visible_fast_path= false;
thread_local bool mylite_ownerless_statement_explicit_transaction= false;
thread_local bool mylite_ownerless_statement_deferred_page_publish= false;
thread_local bool mylite_ownerless_pages_visible_force= false;
thread_local bool mylite_ownerless_page_write_refresh_bypass= false;
thread_local bool mylite_ownerless_statement_deferred_redo_admission_ready=
    false;
thread_local bool mylite_ownerless_statement_plain_read= false;
thread_local bool mylite_ownerless_statement_plain_read_preserve_local_pages=
    false;
thread_local bool mylite_ownerless_statement_plain_read_pages_refreshed= false;
thread_local bool mylite_ownerless_statement_dictionary_ddl= false;
thread_local bool mylite_ownerless_statement_suppress_native_lifecycle_refresh=
    false;
thread_local bool mylite_ownerless_retained_startup_native_write_scrub= false;
thread_local unsigned mylite_ownerless_internal_lock_wait_depth= 0;

namespace {

constexpr trx_id_t k_transient_lock_trx_id_flag =
    trx_id_t{1} << ((sizeof(trx_id_t) * 8) - 1);

constexpr size_t k_page_write_refresh_negative_cache_entries= 64;
constexpr size_t k_external_page_observation_initial_entries= 128;
constexpr size_t k_external_page_observation_max_entries= 8192;
constexpr size_t k_deferred_redo_batch_capacity=
    MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES;
constexpr size_t k_shared_redo_state_slot_count= 64;
constexpr size_t k_deferred_redo_batch_active_owner_slots= 1;
constexpr size_t k_deferred_redo_batch_peer_headroom_slots= 2;

static_assert(
    k_deferred_redo_batch_capacity <=
        k_shared_redo_state_slot_count -
            k_deferred_redo_batch_active_owner_slots -
            k_deferred_redo_batch_peer_headroom_slots,
    "deferred redo batches must leave peer entry and reservation headroom");

struct deferred_redo_range
{
  uint64_t start_lsn= 0;
  uint64_t end_lsn= 0;
  uint64_t latest_lsn= 0;
};

struct ownerless_active_recovered_trx_scan
{
  bool found= false;
};

thread_local deferred_redo_range deferred_redo_batch
    [k_deferred_redo_batch_capacity];
thread_local size_t deferred_redo_batch_count= 0;
thread_local uint64_t deferred_redo_batch_latest_lsn= 0;

bool ownerless_path_separator(char c) noexcept
{
  return c == '/'
#ifdef _WIN32
         || c == '\\'
#endif
      ;
}

bool ownerless_valid_file_op_relative_path(const char *relative,
                                           size_t relative_path_size) noexcept
{
  constexpr const char suffix[]= ".ibd";
  constexpr size_t suffix_len= sizeof(suffix) - 1;
  const size_t relative_len= strlen(relative);
  return relative_len >= suffix_len && relative_len < relative_path_size &&
         strchr(relative, '/') != nullptr &&
         strcmp(&relative[relative_len - suffix_len], suffix) == 0;
}

std::atomic<mylite_ownerless_innodb_lock_acquire_table_callback>
    acquire_table_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_release_table_callback>
    release_table_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_wait_table_callback>
    wait_table_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_wait_until_table_callback>
    wait_until_table_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_acquire_record_callback>
    acquire_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_release_record_callback>
    release_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_release_page_writes_callback>
    release_records_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_acquire_page_write_callback>
    acquire_page_write_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_release_record_callback>
    release_page_write_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_release_page_writes_callback>
    release_page_writes_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_wait_record_callback>
    wait_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_wait_until_record_callback>
    wait_until_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_before_record_wait_callback>
    before_record_wait_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_clear_wait_callback>
    clear_wait_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_enter_callback>
    redo_enter_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_observe_callback>
    redo_observe_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_observe_visible_callback>
    redo_observe_visible_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_observe_written_callback>
    redo_observe_written_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_reserve_callback>
    redo_reserve_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_written_callback>
    redo_written_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_leave_callback>
    redo_leave_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_written_leave_callback>
    redo_written_leave_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_written_leave_batch_callback>
    redo_written_leave_batch_callback{nullptr};
std::atomic<mylite_ownerless_innodb_pages_visible_callback>
    pages_visible_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_publish_callback>
    page_publish_callback{nullptr};
std::atomic<mylite_ownerless_innodb_history_proof_publish_pair_callback>
    history_proof_publish_pair_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_publish_batch_callback>
    page_publish_batch_begin_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_publish_batch_callback>
    page_publish_batch_end_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_read_callback>
    page_read_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_write_active_callback>
    page_write_active_callback{nullptr};
std::atomic<mylite_ownerless_innodb_skip_external_page_refresh_callback>
    skip_external_page_refresh_callback{nullptr};
std::atomic<mylite_ownerless_innodb_file_delete_guard_acquire_callback>
    file_delete_guard_acquire_callback{nullptr};
std::atomic<mylite_ownerless_innodb_file_delete_guard_release_callback>
    file_delete_guard_release_callback{nullptr};
std::atomic<void *> file_delete_guard_context{nullptr};
std::atomic<mylite_ownerless_innodb_autoinc_read_callback>
    autoinc_read_callback{nullptr};
std::atomic<mylite_ownerless_innodb_autoinc_publish_callback>
    autoinc_publish_callback{nullptr};
std::atomic<void *> callback_context{nullptr};
std::atomic<void *> autoinc_callback_context{nullptr};
std::atomic<trx_id_t> next_transient_lock_trx_id{1};
std::atomic<bool> checkpoint_suppressed{false};
std::atomic<bool> ownerless_write_coordination_enabled{false};
std::atomic<bool> ownerless_coordination_error{false};
std::atomic<bool> relative_file_op_redo_paths{false};
std::atomic<bool> uncheckpointed_file_rename_recovery{false};
std::atomic<bool> file_op_redo_logged{false};
std::atomic<uint64_t> test_fault_match_count{0};
std::atomic<uint64_t> startup_page_visible_lsn{0};
std::atomic<uint64_t> startup_native_support_page_visible_lsn{0};
thread_local uint64_t page_visible_lsn= 0;
thread_local bool page_visible_lsn_is_current= false;
thread_local bool page_visible_lsn_is_retained= false;
thread_local unsigned redo_depth= 0;
thread_local uint64_t redo_latest_lsn= 0;
thread_local trx_id_t page_write_lock_trx_id= 0;
thread_local bool checkpoint_suppression_bypass= false;

trx_id_t allocate_transient_lock_trx_id()
{
  trx_id_t next= next_transient_lock_trx_id.load(std::memory_order_relaxed);
  for (;;)
  {
    if (next == 0 || next >= k_transient_lock_trx_id_flag)
    {
      ownerless_coordination_error.store(true, std::memory_order_release);
      return 0;
    }
    if (next_transient_lock_trx_id.compare_exchange_weak(
            next, next + 1, std::memory_order_acq_rel,
            std::memory_order_relaxed))
      return k_transient_lock_trx_id_flag | next;
  }
}

enum ownerless_page_write_refresh_stat_index {
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_CALLS= 0,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_FORCE_CALLS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_CURRENT_VISIBILITY_CALLS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_ALLOC_FAILURES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_CALLS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_ERRORS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_MISSES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_NODE_MISSES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_CALLS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_NS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_HITS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_MISSES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_ERRORS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_IDENTITY_MISMATCH,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_OVERLAYS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_CALLS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_NS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_FAILURES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_IDENTITY_MISMATCH,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_NOT_NEWER,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_OVERLAYS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_HITS,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_MISSES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_STORES,
  OWNERLESS_PAGE_WRITE_REFRESH_STAT_COUNT
};

struct ownerless_page_write_refresh_negative_cache_entry {
  uint64_t epoch;
  void *context;
  uint32_t space_id;
  uint32_t page_no;
  uint64_t local_lsn;
  uint64_t covered_visible_lsn;
};

struct ownerless_external_page_observation_entry {
  uint64_t token;
  void *context;
  uint32_t space_id;
  uint32_t page_no;
  uint64_t commit_lsn;
  bool rollback_barrier;
};

std::atomic<bool> ownerless_page_write_refresh_stats_enabled{false};
std::atomic<uint64_t> ownerless_page_write_refresh_stats
    [OWNERLESS_PAGE_WRITE_REFRESH_STAT_COUNT];
std::atomic<uint64_t> ownerless_page_write_refresh_cache_epoch{1};
thread_local ownerless_page_write_refresh_negative_cache_entry
    ownerless_page_write_refresh_negative_cache
        [k_page_write_refresh_negative_cache_entries];
thread_local uint64_t ownerless_external_page_observation_token= 0;
thread_local ownerless_external_page_observation_entry *
    ownerless_external_page_observations= nullptr;
thread_local size_t ownerless_external_page_observation_capacity= 0;

bool ownerless_page_write_refresh_stats_on() noexcept
{
  return ownerless_page_write_refresh_stats_enabled.load(
      std::memory_order_relaxed);
}

uint64_t ownerless_page_write_refresh_now_ns() noexcept
{
  const auto now= std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void ownerless_page_write_refresh_add(
    ownerless_page_write_refresh_stat_index index, uint64_t value) noexcept
{
  if (ownerless_page_write_refresh_stats_on())
    ownerless_page_write_refresh_stats[index].fetch_add(
        value, std::memory_order_relaxed);
}

void ownerless_page_write_refresh_count(
    ownerless_page_write_refresh_stat_index index) noexcept
{
  ownerless_page_write_refresh_add(index, 1);
}

void ownerless_page_write_refresh_add_elapsed(
    ownerless_page_write_refresh_stat_index index, uint64_t start_ns) noexcept
{
  if (start_ns != 0)
    ownerless_page_write_refresh_add(
        index, ownerless_page_write_refresh_now_ns() - start_ns);
}

uint64_t ownerless_page_key_hash(uint32_t space_id, uint32_t page_no) noexcept
{
  uint64_t hash= (static_cast<uint64_t>(space_id) << 32) | page_no;
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdULL;
  hash ^= hash >> 33;
  return hash;
}

size_t ownerless_page_write_refresh_negative_cache_slot(
    uint32_t space_id, uint32_t page_no) noexcept
{
  return static_cast<size_t>(
      ownerless_page_key_hash(space_id, page_no) &
      (k_page_write_refresh_negative_cache_entries - 1));
}

size_t ownerless_external_page_observation_slot(
    uint32_t space_id, uint32_t page_no, size_t capacity) noexcept
{
  return static_cast<size_t>(
      ownerless_page_key_hash(space_id, page_no) &
      (capacity - 1));
}

bool ownerless_external_page_observation_matches(
    const ownerless_external_page_observation_entry &entry, uint64_t token,
    void *context, uint32_t space_id, uint32_t page_no) noexcept
{
  return entry.token == token && entry.context == context &&
         entry.space_id == space_id && entry.page_no == page_no;
}

void ownerless_external_page_observations_clear() noexcept
{
  if (ownerless_external_page_observations != nullptr &&
      ownerless_external_page_observation_capacity != 0)
  {
    memset(
        ownerless_external_page_observations, 0,
        ownerless_external_page_observation_capacity *
            sizeof(ownerless_external_page_observation_entry));
  }
}

bool ownerless_external_page_observations_resize(size_t new_capacity) noexcept
{
  if (new_capacity <= ownerless_external_page_observation_capacity)
    return true;
  if (new_capacity > k_external_page_observation_max_entries)
    return false;

  ownerless_external_page_observation_entry *new_entries=
      static_cast<ownerless_external_page_observation_entry*>(
          std::calloc(new_capacity,
                      sizeof(ownerless_external_page_observation_entry)));
  if (new_entries == nullptr)
    return false;

  if (ownerless_external_page_observations != nullptr)
  {
    for (size_t old_slot= 0;
         old_slot < ownerless_external_page_observation_capacity; ++old_slot)
    {
      const ownerless_external_page_observation_entry entry=
          ownerless_external_page_observations[old_slot];
      if (entry.token == 0 || entry.context == nullptr)
        continue;

      const size_t start_slot= ownerless_external_page_observation_slot(
          entry.space_id, entry.page_no, new_capacity);
      for (size_t probe= 0; probe < new_capacity; ++probe)
      {
        const size_t slot= (start_slot + probe) & (new_capacity - 1);
        ownerless_external_page_observation_entry &new_entry=
            new_entries[slot];
        if (new_entry.token == 0 || new_entry.context == nullptr)
        {
          new_entry= entry;
          break;
        }
      }
    }
    std::free(ownerless_external_page_observations);
  }

  ownerless_external_page_observations= new_entries;
  ownerless_external_page_observation_capacity= new_capacity;
  return true;
}

bool ownerless_external_page_observations_ensure() noexcept
{
  if (ownerless_external_page_observations != nullptr &&
      ownerless_external_page_observation_capacity != 0)
  {
    return true;
  }
  return ownerless_external_page_observations_resize(
      k_external_page_observation_initial_entries);
}

bool ownerless_page_write_refresh_negative_cache_hit(
    uint32_t space_id, uint32_t page_no, uint64_t local_lsn,
    uint64_t visible_lsn) noexcept
{
  if (visible_lsn == 0)
    return false;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return false;

  const size_t slot=
      ownerless_page_write_refresh_negative_cache_slot(space_id, page_no);
  const ownerless_page_write_refresh_negative_cache_entry &entry=
      ownerless_page_write_refresh_negative_cache[slot];
  const uint64_t epoch= ownerless_page_write_refresh_cache_epoch.load(
      std::memory_order_acquire);
  const bool hit= entry.epoch == epoch && entry.context == context &&
      entry.space_id == space_id && entry.page_no == page_no &&
      entry.local_lsn == local_lsn &&
      entry.covered_visible_lsn >= visible_lsn;
  ownerless_page_write_refresh_count(
      hit ? OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_HITS :
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_MISSES);
  return hit;
}

void ownerless_page_write_refresh_negative_cache_store(
    uint32_t space_id, uint32_t page_no, uint64_t local_lsn,
    uint64_t visible_lsn) noexcept
{
  if (visible_lsn == 0)
    return;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return;

  const size_t slot=
      ownerless_page_write_refresh_negative_cache_slot(space_id, page_no);
  ownerless_page_write_refresh_negative_cache_entry &entry=
      ownerless_page_write_refresh_negative_cache[slot];
  entry.epoch= ownerless_page_write_refresh_cache_epoch.load(
      std::memory_order_acquire);
  entry.context= context;
  entry.space_id= space_id;
  entry.page_no= page_no;
  entry.local_lsn= local_lsn;
  entry.covered_visible_lsn= visible_lsn;
  ownerless_page_write_refresh_count(
      OWNERLESS_PAGE_WRITE_REFRESH_STAT_NEGATIVE_CACHE_STORES);
}

bool ownerless_skip_external_page_refresh() noexcept
{
  mylite_ownerless_innodb_skip_external_page_refresh_callback skip_hook=
      skip_external_page_refresh_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  return skip_hook != nullptr && context != nullptr && skip_hook(context) != 0;
}

bool ownerless_lock_result_is_coordination_failure(int result) noexcept
{
  switch (result) {
  case MYLITE_OWNERLESS_INNODB_LOCK_OK:
  case MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE:
  case MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT:
  case MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK:
  case MYLITE_OWNERLESS_INNODB_LOCK_FULL:
    return false;
  default:
    return true;
  }
}

int ownerless_lock_result_from_dberr(dberr_t error) noexcept
{
  switch (error) {
  case DB_SUCCESS:
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  case DB_LOCK_WAIT_TIMEOUT:
    return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;
  case DB_DEADLOCK:
    return MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK;
  case DB_LOCK_TABLE_FULL:
    return MYLITE_OWNERLESS_INNODB_LOCK_FULL;
  default:
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
}

void note_ownerless_coordination_fault(trx_t *trx);
int normalize_required_hook_result(trx_t *trx, int result);
int missing_required_hook_result(trx_t *trx);
void handle_hook_result(const char *operation, int result, trx_t *trx= nullptr);
bool ownerless_lock_hooks_enabled();
bool ownerless_autoinc_hooks_enabled();
bool replay_persistent_autoinc_root_value(
    trx_t *trx,
    dict_index_t *index,
    uint64_t autoinc);
bool lock_publishable(const ib_lock_t *lock);
bool table_lock_publishable(const ib_lock_t *lock);
bool record_lock_publishable(const ib_lock_t *lock);
bool wait_lock_publishable(const ib_lock_t *lock);
bool blocker_lock_publishable(const ib_lock_t *lock);
void advance_external_lsn(uint64_t latest_lsn);
int push_latest_external_page_visibility(uint64_t *previous_lsn);
void refresh_external_space_header(uint32_t space_id);
void refresh_external_space_allocation_pages(uint32_t space_id);
void refresh_external_space_allocation_pages_native_current(uint32_t space_id);
void refresh_external_space_headers();
bool refresh_external_space_header(fil_space_t &space,
                                   bool native_current= false);
bool table_can_be_evicted_from_dictionary(dict_table_t *table);
bool foreign_table_can_be_reloaded_from_dictionary(dict_table_t *table);
void mark_retained_native_write_scrub_page_dirty(const buf_block_t &block);
extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_preserve_clean_no_skip(
    uint64_t visible_lsn);
static bool active_transaction_rollback_page(
    const trx_t *trx, const buf_page_t &page, uint16_t page_type) noexcept
{
  if (trx == nullptr ||
      (trx->rsegs.m_redo.undo == nullptr && trx->rsegs.m_noredo.undo == nullptr))
    return false;

  const page_id_t id{page.id()};
  if (page_type == FIL_PAGE_UNDO_LOG || srv_is_undo_tablespace(id.space()) ||
      (id.space() > TRX_SYS_SPACE && id.space() <= 3))
    return true;

  const auto matches_rseg= [&id](const trx_rseg_t *rseg) noexcept {
    return rseg != nullptr && rseg->space != nullptr &&
           rseg->space->id == id.space() && rseg->page_no == id.page_no();
  };
  return matches_rseg(trx->rsegs.m_redo.rseg) ||
         matches_rseg(trx->rsegs.m_noredo.rseg);
}

int refresh_page_for_write(const buf_block_t &block,
                           bool use_current_visibility= false,
                           bool force_page_version= false,
                           bool allow_boundary_newer= false,
                           bool allow_visible_boundary= false,
                           bool preserve_retained_user_page= false,
                           bool skip_page_version= false,
                           bool preserve_local_transaction_page= true,
                           bool allow_native_disk_regression= false,
                           bool allow_dirty_committed_page_refresh= false,
                           bool allow_current_logical_page_version= false);
fil_node_t *find_file_node_for_page(fil_space_t &space, uint32_t *page_no);
int refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no,
                             bool load_if_missing,
                             bool force_page_version= false,
                             bool evict_clean_page= true,
                             bool allow_boundary_newer= false,
                             bool allow_visible_boundary= false,
                             bool preserve_retained_user_page= false,
                             bool skip_page_version= false,
                             bool preserve_local_transaction_page= true,
                             bool allow_native_disk_regression= false,
                             bool allow_dirty_committed_page_refresh= false);
void refresh_buffer_pool_pages(bool force_page_version= false,
                               bool evict_clean_pages= true,
                               bool allow_boundary_newer= false,
                               bool allow_visible_boundary= false,
                               bool preserve_retained_user_page= false,
                               bool skip_page_version= false,
                               bool preserve_local_transaction_page= true,
                               bool allow_native_disk_regression= false,
                               bool allow_dirty_committed_page_refresh= false,
                               bool user_tablespaces_only= false);
void refresh_replaceable_buffer_pool_pages();
bool record_bit_set(const ib_lock_t *lock, uint32_t heap_no);
trx_id_t lock_transaction_id(const ib_lock_t *lock, bool create_transient);
trx_id_t transaction_lock_id(trx_t *trx, bool create_transient);
trx_id_t transaction_lock_id(const trx_t *trx);
trx_id_t page_write_transaction_id(trx_t *trx);
uint64_t page_write_pack(uint32_t space_id, uint32_t page_no);
uint64_t page_write_transaction_gate_for_space(const trx_t *trx,
                                               uint32_t space_id);
bool transaction_has_page_write_gate(const trx_t *trx, uint64_t gate_page);
bool transaction_has_page_write_entry(const trx_t *trx, uint64_t packed_page);
void note_transaction_page_write_gate(trx_t *trx, uint64_t gate_page);
void note_transaction_page_write_page(trx_t *trx, uint64_t packed_page);
bool packed_page_write_transaction_gate(uint64_t packed_page);
bool packed_page_write_synthetic_gate(uint64_t packed_page);
bool transaction_has_page_write_image(const trx_t *trx, uint64_t packed_page);
bool transaction_should_keep_page_write_gate(const trx_t *trx,
                                             uint64_t gate_page);
bool transaction_should_keep_statement_page_write(const trx_t *trx,
                                                 uint64_t packed_page);
bool transaction_keeps_page_writes_to_end(const trx_t *trx);
bool transaction_should_track_page_write(trx_t *trx,
                                         uint32_t space_id,
                                         uint32_t page_no);
void collect_buffer_pool_file_pages(std::vector<uint64_t> &pages);
int flush_deferred_redo_batch();
void retain_deferred_redo_batch_tail(size_t first, size_t count);
int publish_pages_visible_lsn(uint64_t visible_lsn);
void clear_transaction_wait(trx_id_t trx_id, trx_t *trx);
int release_transaction_page_writes(trx_id_t trx_id);
int release_transaction_records(trx_id_t trx_id);
uint32_t normalized_lock_mode(const ib_lock_t *lock);
uint32_t normalized_lock_mode(uint32_t type_mode);
uint32_t record_lock_flags(const ib_lock_t *lock, uint32_t heap_no);
uint32_t record_lock_flags(uint32_t type_mode, uint32_t heap_no);

} // namespace

extern "C" void mylite_ownerless_innodb_lock_set_hooks(
    mylite_ownerless_innodb_lock_acquire_table_callback acquire_table_hook,
    mylite_ownerless_innodb_lock_release_table_callback release_table_hook,
    mylite_ownerless_innodb_lock_wait_table_callback wait_table_hook,
    mylite_ownerless_innodb_lock_acquire_record_callback acquire_record_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_record_hook,
    mylite_ownerless_innodb_lock_release_page_writes_callback release_records_hook,
    mylite_ownerless_innodb_lock_acquire_page_write_callback acquire_page_write_hook,
    mylite_ownerless_innodb_lock_release_record_callback release_page_write_hook,
    mylite_ownerless_innodb_lock_release_page_writes_callback release_page_writes_hook,
    mylite_ownerless_innodb_lock_wait_record_callback wait_record_hook,
    mylite_ownerless_innodb_lock_wait_until_table_callback wait_until_table_hook,
    mylite_ownerless_innodb_lock_wait_until_record_callback wait_until_record_hook,
    mylite_ownerless_innodb_lock_before_record_wait_callback before_record_wait_hook,
    mylite_ownerless_innodb_lock_clear_wait_callback clear_wait_hook,
    mylite_ownerless_innodb_redo_enter_callback redo_enter_hook,
    mylite_ownerless_innodb_redo_observe_callback redo_observe_hook,
    mylite_ownerless_innodb_redo_observe_visible_callback redo_observe_visible_hook,
    mylite_ownerless_innodb_redo_reserve_callback redo_reserve_hook,
    mylite_ownerless_innodb_redo_written_callback redo_written_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    mylite_ownerless_innodb_pages_visible_callback pages_visible_hook,
    mylite_ownerless_innodb_page_publish_callback page_publish_hook,
    mylite_ownerless_innodb_page_read_callback page_read_hook,
    mylite_ownerless_innodb_page_write_active_callback page_write_active_hook,
    mylite_ownerless_innodb_skip_external_page_refresh_callback skip_external_page_refresh_hook,
    int write_coordination_enabled,
    void *context)
{
  if (acquire_table_hook == nullptr || release_table_hook == nullptr ||
      wait_table_hook == nullptr || acquire_record_hook == nullptr ||
      release_record_hook == nullptr || release_records_hook == nullptr ||
      acquire_page_write_hook == nullptr ||
      release_page_write_hook == nullptr || wait_record_hook == nullptr ||
      release_page_writes_hook == nullptr ||
      wait_until_table_hook == nullptr || wait_until_record_hook == nullptr ||
      before_record_wait_hook == nullptr || clear_wait_hook == nullptr ||
      redo_enter_hook == nullptr || redo_observe_hook == nullptr ||
      redo_observe_visible_hook == nullptr ||
      redo_reserve_hook == nullptr || redo_written_hook == nullptr ||
      redo_leave_hook == nullptr || pages_visible_hook == nullptr ||
      page_publish_hook == nullptr || page_read_hook == nullptr ||
      page_write_active_hook == nullptr ||
      skip_external_page_refresh_hook == nullptr ||
      context == nullptr)
  {
    mylite_ownerless_innodb_lock_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  skip_external_page_refresh_callback.store(
      skip_external_page_refresh_hook, std::memory_order_release);
  page_write_active_callback.store(page_write_active_hook,
                                   std::memory_order_release);
  page_read_callback.store(page_read_hook, std::memory_order_release);
  page_publish_callback.store(page_publish_hook, std::memory_order_release);
  pages_visible_callback.store(pages_visible_hook, std::memory_order_release);
  redo_leave_callback.store(redo_leave_hook, std::memory_order_release);
  redo_written_callback.store(redo_written_hook, std::memory_order_release);
  redo_reserve_callback.store(redo_reserve_hook, std::memory_order_release);
  redo_observe_callback.store(redo_observe_hook, std::memory_order_release);
  redo_observe_visible_callback.store(redo_observe_visible_hook,
                                      std::memory_order_release);
  redo_enter_callback.store(redo_enter_hook, std::memory_order_release);
  clear_wait_callback.store(clear_wait_hook, std::memory_order_release);
  before_record_wait_callback.store(before_record_wait_hook, std::memory_order_release);
  wait_until_record_callback.store(wait_until_record_hook, std::memory_order_release);
  wait_record_callback.store(wait_record_hook, std::memory_order_release);
  release_page_writes_callback.store(release_page_writes_hook, std::memory_order_release);
  release_page_write_callback.store(release_page_write_hook, std::memory_order_release);
  acquire_page_write_callback.store(acquire_page_write_hook, std::memory_order_release);
  release_records_callback.store(release_records_hook, std::memory_order_release);
  release_record_callback.store(release_record_hook, std::memory_order_release);
  acquire_record_callback.store(acquire_record_hook, std::memory_order_release);
  wait_until_table_callback.store(wait_until_table_hook, std::memory_order_release);
  wait_table_callback.store(wait_table_hook, std::memory_order_release);
  release_table_callback.store(release_table_hook, std::memory_order_release);
  acquire_table_callback.store(acquire_table_hook, std::memory_order_release);
  ownerless_coordination_error.store(false, std::memory_order_release);
  ownerless_write_coordination_enabled.store(
      write_coordination_enabled != 0, std::memory_order_release);
  ownerless_page_write_refresh_cache_epoch.fetch_add(
      1, std::memory_order_acq_rel);
  mylite_ownerless_innodb_lock_hooks_ever_enabled.store(
      true, std::memory_order_release);
  mylite_ownerless_innodb_lock_hooks_enabled.store(true, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_page_publish_batch_hooks(
    mylite_ownerless_innodb_page_publish_batch_callback begin_hook,
    mylite_ownerless_innodb_page_publish_batch_callback end_hook)
{
  page_publish_batch_begin_callback.store(begin_hook,
                                          std::memory_order_release);
  page_publish_batch_end_callback.store(end_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_history_proof_publish_pair_hook(
    mylite_ownerless_innodb_history_proof_publish_pair_callback pair_hook)
{
  history_proof_publish_pair_callback.store(pair_hook,
                                            std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_redo_written_leave_hook(
    mylite_ownerless_innodb_redo_written_leave_callback written_leave_hook)
{
  redo_written_leave_callback.store(written_leave_hook,
                                    std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_redo_written_leave_batch_hook(
    mylite_ownerless_innodb_redo_written_leave_batch_callback
        written_leave_batch_hook)
{
  redo_written_leave_batch_callback.store(written_leave_batch_hook,
                                          std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_redo_observe_written_hook(
    mylite_ownerless_innodb_redo_observe_written_callback
        observe_written_hook)
{
  redo_observe_written_callback.store(observe_written_hook,
                                      std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_set_file_delete_guard_hooks(
    mylite_ownerless_innodb_file_delete_guard_acquire_callback acquire_hook,
    mylite_ownerless_innodb_file_delete_guard_release_callback release_hook,
    void *context)
{
  file_delete_guard_context.store(context, std::memory_order_release);
  file_delete_guard_acquire_callback.store(acquire_hook,
                                           std::memory_order_release);
  file_delete_guard_release_callback.store(release_hook,
                                           std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_reset_file_delete_guard_hooks(void)
{
  file_delete_guard_acquire_callback.store(nullptr, std::memory_order_release);
  file_delete_guard_release_callback.store(nullptr, std::memory_order_release);
  file_delete_guard_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_lock_reset_hooks(void)
{
  mylite_ownerless_innodb_clear_coordination_error_for_recovery();
  if (trx_ownerless_retry_quarantined())
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return 0;
  }

  const int flush_result= mylite_ownerless_innodb_redo_flush_deferred();
  if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return 0;
  }
  ownerless_page_write_refresh_cache_epoch.fetch_add(
      1, std::memory_order_acq_rel);
  mylite_ownerless_innodb_lock_hooks_enabled.store(false, std::memory_order_release);
  ownerless_write_coordination_enabled.store(false, std::memory_order_release);
  acquire_table_callback.store(nullptr, std::memory_order_release);
  release_table_callback.store(nullptr, std::memory_order_release);
  wait_table_callback.store(nullptr, std::memory_order_release);
  wait_until_table_callback.store(nullptr, std::memory_order_release);
  acquire_record_callback.store(nullptr, std::memory_order_release);
  release_record_callback.store(nullptr, std::memory_order_release);
  release_records_callback.store(nullptr, std::memory_order_release);
  acquire_page_write_callback.store(nullptr, std::memory_order_release);
  release_page_write_callback.store(nullptr, std::memory_order_release);
  release_page_writes_callback.store(nullptr, std::memory_order_release);
  wait_record_callback.store(nullptr, std::memory_order_release);
  wait_until_record_callback.store(nullptr, std::memory_order_release);
  before_record_wait_callback.store(nullptr, std::memory_order_release);
  clear_wait_callback.store(nullptr, std::memory_order_release);
  redo_enter_callback.store(nullptr, std::memory_order_release);
  redo_observe_callback.store(nullptr, std::memory_order_release);
  redo_observe_visible_callback.store(nullptr, std::memory_order_release);
  redo_observe_written_callback.store(nullptr, std::memory_order_release);
  redo_reserve_callback.store(nullptr, std::memory_order_release);
  redo_written_callback.store(nullptr, std::memory_order_release);
  redo_leave_callback.store(nullptr, std::memory_order_release);
  redo_written_leave_callback.store(nullptr, std::memory_order_release);
  redo_written_leave_batch_callback.store(nullptr, std::memory_order_release);
  pages_visible_callback.store(nullptr, std::memory_order_release);
  page_publish_callback.store(nullptr, std::memory_order_release);
  history_proof_publish_pair_callback.store(nullptr, std::memory_order_release);
  page_publish_batch_begin_callback.store(nullptr, std::memory_order_release);
  page_publish_batch_end_callback.store(nullptr, std::memory_order_release);
  page_read_callback.store(nullptr, std::memory_order_release);
  page_write_active_callback.store(nullptr, std::memory_order_release);
  skip_external_page_refresh_callback.store(nullptr, std::memory_order_release);
  mylite_ownerless_innodb_autoinc_reset_hooks();
  startup_page_visible_lsn.store(0, std::memory_order_release);
  startup_native_support_page_visible_lsn.store(0, std::memory_order_release);
  mylite_ownerless_statement_execution_active= false;
  mylite_ownerless_statement_explicit_transaction= false;
  page_visible_lsn= 0;
  redo_depth= 0;
  redo_latest_lsn= 0;
  page_write_lock_trx_id= 0;
  mylite_ownerless_statement_deferred_redo_admission_ready= false;
  checkpoint_suppression_bypass= false;
  mylite_ownerless_innodb_reset_thread_redo_latch_depth();
  callback_context.store(nullptr, std::memory_order_release);
  checkpoint_suppressed.store(false, std::memory_order_release);
  relative_file_op_redo_paths.store(false, std::memory_order_release);
  uncheckpointed_file_rename_recovery.store(false, std::memory_order_release);
  file_op_redo_logged.store(false, std::memory_order_release);
  mylite_ownerless_innodb_set_test_faults_enabled(0);
  mylite_ownerless_innodb_startup_lsn_advance_limit.store(
      0, std::memory_order_release);
  mylite_ownerless_innodb_startup_lsn_advance_enabled.store(
      false, std::memory_order_release);
  return 1;
}

extern "C" int mylite_ownerless_innodb_lock_has_hooks(void)
{
  if (!ownerless_lock_hooks_enabled())
    return 0;

  return acquire_table_callback.load(std::memory_order_acquire) != nullptr &&
         release_table_callback.load(std::memory_order_acquire) != nullptr &&
         wait_table_callback.load(std::memory_order_acquire) != nullptr &&
         wait_until_table_callback.load(std::memory_order_acquire) != nullptr &&
         acquire_record_callback.load(std::memory_order_acquire) != nullptr &&
         release_record_callback.load(std::memory_order_acquire) != nullptr &&
         release_records_callback.load(std::memory_order_acquire) != nullptr &&
         acquire_page_write_callback.load(std::memory_order_acquire) != nullptr &&
         release_page_write_callback.load(std::memory_order_acquire) != nullptr &&
         release_page_writes_callback.load(std::memory_order_acquire) != nullptr &&
         wait_record_callback.load(std::memory_order_acquire) != nullptr &&
         wait_until_record_callback.load(std::memory_order_acquire) != nullptr &&
         before_record_wait_callback.load(std::memory_order_acquire) != nullptr &&
         clear_wait_callback.load(std::memory_order_acquire) != nullptr &&
         redo_enter_callback.load(std::memory_order_acquire) != nullptr &&
         redo_observe_callback.load(std::memory_order_acquire) != nullptr &&
         redo_reserve_callback.load(std::memory_order_acquire) != nullptr &&
         redo_written_callback.load(std::memory_order_acquire) != nullptr &&
         redo_leave_callback.load(std::memory_order_acquire) != nullptr &&
         pages_visible_callback.load(std::memory_order_acquire) != nullptr &&
         page_publish_callback.load(std::memory_order_acquire) != nullptr &&
         page_read_callback.load(std::memory_order_acquire) != nullptr &&
         skip_external_page_refresh_callback.load(std::memory_order_acquire) != nullptr &&
         callback_context.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_innodb_file_delete_guard_acquire(void)
{
  mylite_ownerless_innodb_file_delete_guard_acquire_callback hook=
      file_delete_guard_acquire_callback.load(std::memory_order_acquire);
  void *context= file_delete_guard_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(context);
}

extern "C" void mylite_ownerless_innodb_file_delete_guard_release(void)
{
  mylite_ownerless_innodb_file_delete_guard_release_callback hook=
      file_delete_guard_release_callback.load(std::memory_order_acquire);
  void *context= file_delete_guard_context.load(std::memory_order_acquire);
  if (hook != nullptr && context != nullptr)
    hook(context);
}

extern "C" int mylite_ownerless_innodb_coordination_error(void)
{
  return ownerless_coordination_error.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_write_coordination_enabled(void)
{
  return ownerless_write_coordination_enabled.load(
      std::memory_order_acquire) ? 1 : 0;
}

extern "C" void mylite_ownerless_innodb_note_coordination_error(void)
{
  ownerless_coordination_error.store(true, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_clear_coordination_error_for_recovery(void)
{
  ownerless_coordination_error.store(false, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_set_checkpoint_suppression(int suppressed)
{
  checkpoint_suppressed.store(suppressed != 0, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_set_startup_lsn_advance_limit(
    uint64_t max_lsn)
{
  mylite_ownerless_innodb_startup_lsn_advance_limit.store(
      max_lsn, std::memory_order_release);
  mylite_ownerless_innodb_startup_lsn_advance_enabled.store(
      true, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_clear_startup_lsn_advance_limit(void)
{
  mylite_ownerless_innodb_startup_lsn_advance_enabled.store(
      false, std::memory_order_release);
  mylite_ownerless_innodb_startup_lsn_advance_limit.store(
      0, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_checkpoint_suppressed(void)
{
  if (checkpoint_suppression_bypass)
    return 0;

  return checkpoint_suppressed.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" void mylite_ownerless_innodb_set_relative_file_op_redo_paths(int enabled)
{
  relative_file_op_redo_paths.store(enabled != 0, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_relative_file_op_redo_paths(void)
{
  return relative_file_op_redo_paths.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_file_op_redo_relative_path(
    const char *datadir,
    const char *path,
    char *relative_path,
    size_t relative_path_size)
{
  if (datadir == nullptr || path == nullptr || relative_path == nullptr ||
      relative_path_size == 0 || !*datadir || !*path)
    return 0;

  const size_t path_len= strlen(path);
  size_t datadir_len= strlen(datadir);
  while (datadir_len > 0 && ownerless_path_separator(datadir[datadir_len - 1]))
    --datadir_len;
  if (datadir_len == 0)
    return 0;

  const char *relative= nullptr;
  if (path_len > datadir_len && strncmp(path, datadir, datadir_len) == 0 &&
      ownerless_path_separator(path[datadir_len]))
    relative= path + datadir_len;
  else
  {
    const char *datadir_without_root= datadir;
    while (ownerless_path_separator(*datadir_without_root))
      ++datadir_without_root;
    const size_t stripped_len=
        datadir_len - size_t(datadir_without_root - datadir);
    if (stripped_len == 0 || path_len <= stripped_len ||
        strncmp(path, datadir_without_root, stripped_len) != 0 ||
        !ownerless_path_separator(path[stripped_len]))
      return 0;
    relative= path + stripped_len;
  }

  while (ownerless_path_separator(*relative))
    ++relative;
  if (!ownerless_valid_file_op_relative_path(relative, relative_path_size))
    return 0;

  strcpy(relative_path, relative);
  return 1;
}

extern "C" void mylite_ownerless_innodb_set_uncheckpointed_file_rename_recovery(
    int enabled)
{
  uncheckpointed_file_rename_recovery.store(enabled != 0,
                                            std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_uncheckpointed_file_rename_recovery(void)
{
  return uncheckpointed_file_rename_recovery.load(std::memory_order_acquire)
             ? 1
             : 0;
}

extern "C" void mylite_ownerless_innodb_note_file_op_redo(void)
{
  file_op_redo_logged.store(true, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_take_file_op_redo(void)
{
  const bool logged= file_op_redo_logged.exchange(false,
                                                  std::memory_order_acq_rel);
  return logged ? 1 : 0;
}

extern "C" void mylite_ownerless_innodb_note_file_rename_redo(void)
{
  mylite_ownerless_innodb_note_file_op_redo();
}

extern "C" int mylite_ownerless_innodb_take_file_rename_redo(void)
{
  return mylite_ownerless_innodb_take_file_op_redo();
}

extern "C" void mylite_ownerless_innodb_set_test_faults_enabled(int enabled)
{
  if (enabled != 0)
  {
    test_fault_match_count.store(0, std::memory_order_release);
    mylite_ownerless_statement_deferred_redo_admission_ready= false;
  }
  mylite_ownerless_innodb_test_faults_enabled.store(enabled != 0,
                                                    std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_test_set_next_transient_lock_trx_id(
    uint64_t next_id)
{
  if (!mylite_ownerless_innodb_test_faults_enabled_fast() || next_id == 0 ||
      next_id >= k_transient_lock_trx_id_flag)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  next_transient_lock_trx_id.store(next_id, std::memory_order_release);
  page_write_lock_trx_id= 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" uint64_t
mylite_ownerless_innodb_test_allocate_transient_lock_trx_id()
{
  if (!mylite_ownerless_innodb_test_faults_enabled_fast())
    return 0;
  return allocate_transient_lock_trx_id();
}

static uint64_t mylite_ownerless_innodb_test_fault_skip_count()
{
  uint64_t skip_count= 0;
  const char *skip_value= std::getenv("MYLITE_OWNERLESS_TEST_FAULT_SKIP");
  if (skip_value != nullptr)
  {
    char *end= nullptr;
    errno= 0;
    const unsigned long long parsed_skip= std::strtoull(skip_value, &end, 10);
    if (end != skip_value && *end == '\0' && errno != ERANGE)
      skip_count= static_cast<uint64_t>(parsed_skip);
  }
  return skip_count;
}

extern "C" int mylite_ownerless_innodb_test_fault_will_pause(
    const char *fault_name)
{
  if (!mylite_ownerless_innodb_test_faults_enabled.load(
          std::memory_order_acquire) ||
      fault_name == nullptr)
    return 0;

  const char *configured_fault= std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
  if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name))
    return 0;

  return test_fault_match_count.load(std::memory_order_acquire) >=
         mylite_ownerless_innodb_test_fault_skip_count();
}

extern "C" int mylite_ownerless_innodb_test_fault_is_configured(
    const char *fault_name)
{
  if (!mylite_ownerless_innodb_test_faults_enabled.load(
          std::memory_order_acquire))
    return 0;

  const char *configured_fault= std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
  if (configured_fault == nullptr || configured_fault[0] == '\0')
    return 0;

  return fault_name == nullptr || !std::strcmp(configured_fault, fault_name);
}

extern "C" void mylite_ownerless_innodb_test_fault(const char *fault_name)
{
  if (!mylite_ownerless_innodb_test_faults_enabled.load(
          std::memory_order_acquire) ||
      fault_name == nullptr)
    return;

  const char *configured_fault= std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
  if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name))
    return;

  const uint64_t skip_count= mylite_ownerless_innodb_test_fault_skip_count();
  if (test_fault_match_count.fetch_add(1, std::memory_order_acq_rel) <
      skip_count)
    return;

  const char *ready_fd_value=
      std::getenv("MYLITE_OWNERLESS_TEST_FAULT_READY_FD");
  if (ready_fd_value != nullptr)
  {
    char *end= nullptr;
    const long ready_fd= std::strtol(ready_fd_value, &end, 10);
    if (end != ready_fd_value && *end == '\0' && ready_fd >= 0 &&
        ready_fd <= std::numeric_limits<int>::max())
    {
      const char value= 'x';
      static_cast<void>(write(static_cast<int>(ready_fd), &value,
                              sizeof(value)));
      static_cast<void>(close(static_cast<int>(ready_fd)));
    }
  }

  const char *release_fd_value=
      std::getenv("MYLITE_OWNERLESS_TEST_FAULT_RELEASE_FD");
  if (release_fd_value != nullptr)
  {
    char *end= nullptr;
    const long release_fd= std::strtol(release_fd_value, &end, 10);
    if (end != release_fd_value && *end == '\0' && release_fd >= 0 &&
        release_fd <= std::numeric_limits<int>::max())
    {
      char value= '\0';
      ssize_t bytes_read= -1;
      do
      {
        bytes_read= read(static_cast<int>(release_fd), &value, sizeof(value));
      } while (bytes_read < 0 && errno == EINTR);
      static_cast<void>(close(static_cast<int>(release_fd)));
      if (bytes_read == sizeof(value))
        return;
    }
  }

  for (;;)
    std::this_thread::sleep_for(std::chrono::hours(24));
}

extern "C" void mylite_ownerless_innodb_set_page_write_refresh_stats_enabled(
    int enabled)
{
  ownerless_page_write_refresh_stats_enabled.store(
      enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_reset_page_write_refresh_stats(void)
{
  for (size_t i= 0; i < OWNERLESS_PAGE_WRITE_REFRESH_STAT_COUNT; ++i)
    ownerless_page_write_refresh_stats[i].store(0,
                                                std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_read_page_write_refresh_stats(
    uint64_t *out_values, size_t value_count)
{
  if (out_values == nullptr || value_count == 0)
    return;

  const size_t copy_count= std::min(
      value_count,
      static_cast<size_t>(OWNERLESS_PAGE_WRITE_REFRESH_STAT_COUNT));
  for (size_t i= 0; i < copy_count; ++i)
    out_values[i]= ownerless_page_write_refresh_stats[i].load(
        std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_autoinc_set_hooks(
    mylite_ownerless_innodb_autoinc_read_callback read_hook,
    mylite_ownerless_innodb_autoinc_publish_callback publish_hook,
    void *context)
{
  if (read_hook == nullptr || publish_hook == nullptr || context == nullptr)
  {
    mylite_ownerless_innodb_autoinc_reset_hooks();
    return;
  }

  autoinc_callback_context.store(context, std::memory_order_release);
  autoinc_publish_callback.store(publish_hook, std::memory_order_release);
  autoinc_read_callback.store(read_hook, std::memory_order_release);
  mylite_ownerless_innodb_autoinc_hooks_enabled.store(true, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_autoinc_reset_hooks(void)
{
  mylite_ownerless_innodb_autoinc_hooks_enabled.store(false, std::memory_order_release);
  autoinc_read_callback.store(nullptr, std::memory_order_release);
  autoinc_publish_callback.store(nullptr, std::memory_order_release);
  autoinc_callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_autoinc_has_hooks(void)
{
  if (!ownerless_autoinc_hooks_enabled())
    return 0;

  return autoinc_read_callback.load(std::memory_order_acquire) != nullptr &&
         autoinc_publish_callback.load(std::memory_order_acquire) != nullptr &&
         autoinc_callback_context.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_innodb_lock_reserve_table(
    trx_t *trx,
    const dict_table_t *table,
    uint32_t mode,
    unsigned int timeout_ms)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(trx);

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return missing_required_hook_result(trx);

  return normalize_required_hook_result(
      trx, hook(trx_id, table->id, normalized_lock_mode(mode), timeout_ms,
                context));
}

extern "C" int mylite_ownerless_innodb_lock_acquire_autoinc(
    trx_t *trx,
    const dict_table_t *table,
    unsigned int timeout_ms)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(trx);

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return missing_required_hook_result(trx);

  return normalize_required_hook_result(
      trx, hook(trx_id, table->id,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC, timeout_ms,
                context));
}

extern "C" void mylite_ownerless_innodb_lock_release_autoinc(
    trx_t *trx,
    const dict_table_t *table)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    static_cast<void>(missing_required_hook_result(trx));
    return;
  }

  mylite_ownerless_innodb_lock_release_table_callback hook=
      release_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(trx));
    return;
  }

  const trx_id_t trx_id= transaction_lock_id(trx, false);
  if (trx_id == 0)
  {
    static_cast<void>(missing_required_hook_result(trx));
    return;
  }

  const int result= hook(trx_id,
                         table->id,
                         MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC,
                         context);
  handle_hook_result("release autoinc", result, trx);
}

extern "C" void mylite_ownerless_innodb_lock_publish_table(
    const ib_lock_t *lock)
{
  if (!table_lock_publishable(lock))
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const trx_id_t trx_id= lock_transaction_id(lock, true);
  if (trx_id == 0)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const int result= hook(trx_id,
                         lock->un_member.tab_lock.table->id,
                         normalized_lock_mode(lock),
                         0U,
                         context);
  handle_hook_result("acquire table", result, lock->trx);
}

extern "C" void mylite_ownerless_innodb_lock_release_table(
    const ib_lock_t *lock)
{
  if (!table_lock_publishable(lock))
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  mylite_ownerless_innodb_lock_release_table_callback hook=
      release_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const trx_id_t trx_id= lock_transaction_id(lock, false);
  if (trx_id == 0)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const int result= hook(trx_id,
                         lock->un_member.tab_lock.table->id,
                         normalized_lock_mode(lock),
                         context);
  handle_hook_result("release table", result, lock->trx);
}

extern "C" int mylite_ownerless_innodb_lock_publish_table_wait(
    const ib_lock_t *wait_lock,
    const ib_lock_t *blocker_lock)
{
  if (!wait_lock_publishable(wait_lock) ||
      !blocker_lock_publishable(blocker_lock) ||
      !wait_lock->is_table() ||
      !blocker_lock->is_table() ||
      wait_lock->un_member.tab_lock.table == nullptr ||
      wait_lock->un_member.tab_lock.table->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(wait_lock->trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    wait_lock->trx->mylite_ownerless_coordination_fault= true;
    wait_lock->trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_wait_table_callback hook=
      wait_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(wait_lock->trx);

  const trx_id_t trx_id= lock_transaction_id(wait_lock, true);
  const trx_id_t blocker_trx_id= lock_transaction_id(blocker_lock, true);
  if (trx_id == 0 || blocker_trx_id == 0)
    return missing_required_hook_result(wait_lock->trx);

  return normalize_required_hook_result(
      wait_lock->trx,
      hook(trx_id, wait_lock->un_member.tab_lock.table->id,
           normalized_lock_mode(wait_lock), blocker_trx_id, context));
}

extern "C" int mylite_ownerless_innodb_lock_snapshot_external_wait(
    const ib_lock_t *wait_lock,
    mylite_ownerless_innodb_lock_external_wait *snapshot)
{
  if (snapshot == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  snapshot->kind= MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_NONE;
  snapshot->trx_id= 0;
  snapshot->table_id= 0;
  snapshot->index_id= 0;
  snapshot->space_id= 0;
  snapshot->page_no= 0;
  snapshot->heap_no= 0;
  snapshot->mode= 0;
  snapshot->flags= 0;

  if (!wait_lock_publishable(wait_lock))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const trx_id_t trx_id= lock_transaction_id(wait_lock, true);
  if (trx_id == 0)
    return missing_required_hook_result(wait_lock->trx);

  if (wait_lock->is_table())
  {
    if (wait_lock->un_member.tab_lock.table == nullptr ||
        wait_lock->un_member.tab_lock.table->id == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;

    snapshot->kind= MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE;
    snapshot->trx_id= trx_id;
    snapshot->table_id= wait_lock->un_member.tab_lock.table->id;
    snapshot->mode= normalized_lock_mode(wait_lock);
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  if (wait_lock->is_table() ||
      wait_lock->index == nullptr ||
      wait_lock->index->id == 0 ||
      wait_lock->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const ulint heap_no= lock_rec_find_set_bit(wait_lock);
  if (heap_no == ULINT_UNDEFINED)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  snapshot->kind= MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD;
  snapshot->trx_id= trx_id;
  snapshot->index_id= wait_lock->index->id;
  snapshot->space_id= wait_lock->un_member.rec_lock.page_id.space();
  snapshot->page_no= wait_lock->un_member.rec_lock.page_id.page_no();
  snapshot->heap_no= static_cast<uint32_t>(heap_no);
  snapshot->mode= normalized_lock_mode(wait_lock);
  snapshot->flags= record_lock_flags(wait_lock, static_cast<uint32_t>(heap_no));
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_lock_wait_for_external(
    const mylite_ownerless_innodb_lock_external_wait *snapshot,
    unsigned int timeout_ms)
{
  if (snapshot == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (snapshot->kind == MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_NONE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(nullptr);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return missing_required_hook_result(nullptr);

  if (snapshot->kind == MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE)
  {
    mylite_ownerless_innodb_lock_wait_until_table_callback hook=
        wait_until_table_callback.load(std::memory_order_acquire);
    if (hook == nullptr)
      return missing_required_hook_result(nullptr);

    return normalize_required_hook_result(
        nullptr, hook(snapshot->trx_id, snapshot->table_id, snapshot->mode,
                      timeout_ms, context));
  }

  if (snapshot->kind != MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_lock_wait_until_record_callback hook=
      wait_until_record_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return missing_required_hook_result(nullptr);

  return normalize_required_hook_result(
      nullptr,
      hook(snapshot->trx_id, snapshot->index_id, snapshot->space_id,
           snapshot->page_no, snapshot->heap_no, snapshot->mode,
           snapshot->flags, timeout_ms, context));
}

extern "C" int mylite_ownerless_innodb_lock_reserve_record(
    trx_t *trx,
    const dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode,
    unsigned int timeout_ms)
{
  if (trx == nullptr ||
      index == nullptr ||
      index->id == 0 ||
      type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(trx);

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return missing_required_hook_result(trx);
  return normalize_required_hook_result(
      trx, hook(trx_id, index->id, space_id, page_no, heap_no,
                normalized_lock_mode(type_mode),
                record_lock_flags(type_mode, heap_no), timeout_ms, context));
}

extern "C" int mylite_ownerless_innodb_lock_reserve_insert_record(
    trx_t *trx,
    const dict_index_t *index,
    unsigned int timeout_ms)
{
  if (trx == nullptr || index == nullptr || index->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (!ownerless_lock_hooks_enabled())
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  return normalize_required_hook_result(
      trx, hook(trx_id, index->id, 0U, 0U, 0U,
                MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION,
                timeout_ms, context));
}

extern "C" int mylite_ownerless_innodb_lock_cancel_insert_record(
    trx_t *trx,
    const dict_index_t *index)
{
  if (trx == nullptr || index == nullptr || index->id == 0 ||
      !ownerless_lock_hooks_enabled())
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  const trx_id_t trx_id= transaction_lock_id(trx, false);
  if (hook == nullptr || context == nullptr || trx_id == 0)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const int result= normalize_required_hook_result(trx, hook(
      trx_id,
      index->id,
      0U,
      0U,
      0U,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
      MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_RESERVATION,
      context));
  return result;
}

extern "C" int mylite_ownerless_innodb_lock_finalize_insert_record(
    trx_t *trx,
    const dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no)
{
  if (trx == nullptr || index == nullptr || index->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (!ownerless_lock_hooks_enabled())
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const trx_id_t trx_id= transaction_lock_id(trx, false);
  if (trx_id == 0)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const int result= normalize_required_hook_result(trx, hook(
      trx_id,
      index->id,
      space_id,
      page_no,
      heap_no,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
      MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP |
          MYLITE_OWNERLESS_INNODB_RECORD_LOCK_FINALIZE_INSERT_RESERVATION,
      0U,
      context));
  return result;
}

extern "C" int mylite_ownerless_innodb_lock_release_rollback_insert_record(
    trx_t *trx,
    uint64_t index_id,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no)
{
  if (trx == nullptr || index_id == 0 || !ownerless_lock_hooks_enabled())
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  const trx_id_t trx_id= transaction_lock_id(trx, false);
  if (hook == nullptr || context == nullptr || trx_id == 0)
  {
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const int result= hook(
      trx_id,
      index_id,
      space_id,
      page_no,
      heap_no,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
      MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP,
      context);
  if (UNIV_UNLIKELY(result != MYLITE_OWNERLESS_INNODB_LOCK_OK))
    note_ownerless_coordination_fault(trx);
  return result;
}

extern "C" uint64_t mylite_ownerless_innodb_lock_transaction_id(trx_t *trx)
{
  return transaction_lock_id(trx, true);
}

extern "C" int mylite_ownerless_innodb_remote_trx_active(
    uint64_t trx_id,
    int *out_active)
{
  if (out_active == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_active= 0;
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (!mylite_ownerless_trx_hooks_enabled_fast())
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  unsigned int trx_count= 0;
  uint64_t next_trx_id= 0;
  uint64_t min_trx_no= 0;
  int result= mylite_ownerless_trx_snapshot_retry(
      nullptr, 0U, &trx_count, &next_trx_id, &min_trx_no);
  if (result == MYLITE_OWNERLESS_TRX_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  if (result != MYLITE_OWNERLESS_TRX_OK &&
      result != MYLITE_OWNERLESS_TRX_FULL)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  std::vector<uint64_t> trx_ids;
  for (;;)
  {
    trx_ids.resize(trx_count);
    result= mylite_ownerless_trx_snapshot_retry(
        trx_count ? trx_ids.data() : nullptr,
        trx_count,
        &trx_count,
        &next_trx_id,
        &min_trx_no);
    if (result == MYLITE_OWNERLESS_TRX_FULL)
      continue;
    if (result != MYLITE_OWNERLESS_TRX_OK)
    {
      ownerless_coordination_error.store(true, std::memory_order_release);
      return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    break;
  }

  *out_active= std::find(trx_ids.begin(), trx_ids.end(), trx_id) !=
                      trx_ids.end()
      ? 1
      : 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_lock_wait_until_record_available(
    trx_t *trx,
    const dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode,
    unsigned int timeout_ms)
{
  if (trx == nullptr ||
      index == nullptr ||
      index->id == 0 ||
      type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_wait_until_record_callback hook=
      wait_until_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(trx);

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return missing_required_hook_result(trx);
  return normalize_required_hook_result(
      trx, hook(trx_id, index->id, space_id, page_no, heap_no,
                normalized_lock_mode(type_mode),
                record_lock_flags(type_mode, heap_no), timeout_ms, context));
}

extern "C" int mylite_ownerless_innodb_lock_before_external_record_wait(
    trx_t *trx,
    const dict_index_t *index,
    uint32_t space_id,
    uint32_t page_no,
    uint32_t heap_no,
    uint32_t type_mode)
{
  if (trx == nullptr ||
      index == nullptr ||
      index->id == 0 ||
      type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_before_record_wait_callback hook=
      before_record_wait_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(trx);

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return missing_required_hook_result(trx);

  return normalize_required_hook_result(
      trx, hook(trx_id, index->id, space_id, page_no, heap_no,
                normalized_lock_mode(type_mode),
                record_lock_flags(type_mode, heap_no), context));
}

extern "C" void mylite_ownerless_innodb_lock_publish_record_bit(
  const ib_lock_t *lock,
  uint32_t heap_no)
{
  if (!ownerless_lock_hooks_enabled())
  {
    if (record_lock_publishable(lock))
      static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }
  if (!record_lock_publishable(lock) || !record_bit_set(lock, heap_no))
    return;

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const trx_id_t trx_id= lock_transaction_id(lock, true);
  if (trx_id == 0)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }
  const int result= hook(trx_id,
                         lock->index->id,
                         lock->un_member.rec_lock.page_id.space(),
                         lock->un_member.rec_lock.page_id.page_no(),
                         heap_no,
                         normalized_lock_mode(lock),
                         record_lock_flags(lock, heap_no),
                         0U,
                         context);
  handle_hook_result("acquire record", result, lock->trx);
}

extern "C" void mylite_ownerless_innodb_lock_publish_record_bits(
    const ib_lock_t *lock)
{
  if (!ownerless_lock_hooks_enabled())
  {
    if (record_lock_publishable(lock))
      static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }
  if (!record_lock_publishable(lock))
    return;

  const uint32_t n_bits= static_cast<uint32_t>(lock_rec_get_n_bits(lock));
  for (uint32_t heap_no= 0; heap_no < n_bits; heap_no++)
  {
    if (record_bit_set(lock, heap_no))
      mylite_ownerless_innodb_lock_publish_record_bit(lock, heap_no);
  }
}

extern "C" void mylite_ownerless_innodb_lock_release_record_bit(
    const ib_lock_t *lock,
    uint32_t heap_no)
{
  if (!ownerless_lock_hooks_enabled())
  {
    if (record_lock_publishable(lock))
      static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }
  if (!record_lock_publishable(lock))
    return;

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const trx_id_t trx_id= lock_transaction_id(lock, false);
  if (trx_id == 0)
  {
    static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }

  const int result= hook(trx_id,
                         lock->index->id,
                         lock->un_member.rec_lock.page_id.space(),
                         lock->un_member.rec_lock.page_id.page_no(),
                         heap_no,
                         normalized_lock_mode(lock),
                         record_lock_flags(lock, heap_no),
                         context);
  static_cast<void>(normalize_required_hook_result(lock->trx, result));
}

extern "C" void mylite_ownerless_innodb_lock_release_record_bits(
    const ib_lock_t *lock)
{
  if (!ownerless_lock_hooks_enabled())
  {
    if (record_lock_publishable(lock))
      static_cast<void>(missing_required_hook_result(lock->trx));
    return;
  }
  if (!record_lock_publishable(lock))
    return;

  const uint32_t n_bits= static_cast<uint32_t>(lock_rec_get_n_bits(lock));
  for (uint32_t heap_no= 0; heap_no < n_bits; heap_no++)
  {
    if (record_bit_set(lock, heap_no))
      mylite_ownerless_innodb_lock_release_record_bit(lock, heap_no);
  }
}

static int mylite_ownerless_innodb_lock_acquire_page_write_low(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags,
    uint32_t mode,
    bool track_transaction_page)
{
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0U;
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_lock_acquire_page_write_callback hook=
      acquire_page_write_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= page_write_transaction_id(trx);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const bool internal_rollback_wait= trx != nullptr && trx->in_rollback;
  if (internal_rollback_wait)
    mylite_ownerless_innodb_begin_internal_lock_wait();
  const int result= normalize_required_hook_result(
      trx, hook(trx_id,
                MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
                space_id,
                page_no,
                MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
                mode,
                0U,
                timeout_ms,
                out_acquire_flags,
                context));
  if (internal_rollback_wait)
    mylite_ownerless_innodb_end_internal_lock_wait();
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      track_transaction_page &&
      transaction_should_track_page_write(trx, space_id, page_no))
    note_transaction_page_write_page(trx, page_write_pack(space_id, page_no));
  return result;
}

extern "C" int mylite_ownerless_innodb_lock_acquire_page_write(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  return mylite_ownerless_innodb_lock_acquire_page_write_low(
      trx, space_id, page_no, timeout_ms, out_acquire_flags,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X, true);
}

extern "C" int mylite_ownerless_innodb_lock_acquire_page_write_untracked(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  return mylite_ownerless_innodb_lock_acquire_page_write_low(
      trx, space_id, page_no, timeout_ms, out_acquire_flags,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X, false);
}

extern "C" int mylite_ownerless_innodb_lock_reserve_record_page_write(
    trx_t *trx,
    const buf_block_t *block,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0U;
  if (trx == nullptr || block == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const page_id_t id{block->page.id()};
  if (id.space() >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const uint64_t packed_page= page_write_pack(id.space(), id.page_no());
  if (transaction_has_page_write_entry(trx, packed_page))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint32_t acquire_flags= 0U;
  const int acquire_result= mylite_ownerless_innodb_lock_acquire_page_write_low(
      trx, id.space(), id.page_no(), timeout_ms, &acquire_flags,
      MYLITE_OWNERLESS_INNODB_LOCK_MODE_X, true);
  if (acquire_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    if (out_acquire_flags != nullptr)
      *out_acquire_flags= acquire_flags;
    return acquire_result;
  }

  /* Record-grant reservations are transaction-owned even when the MTR fast
  path would normally publish and release the page itself. */
  note_transaction_page_write_page(trx, packed_page);
  if (out_acquire_flags != nullptr)
    *out_acquire_flags=
        acquire_flags | MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_NEW_RECORD_PAGE;

  const int refresh_result=
      mylite_ownerless_innodb_refresh_page_for_write_after_wait(block);
  if (refresh_result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
      refresh_result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const int cancel_result= mylite_ownerless_innodb_lock_cancel_record_page_write(
      trx, id.space(), id.page_no());
  if (cancel_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      cancel_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return cancel_result;
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0U;
  return refresh_result;
}

extern "C" int mylite_ownerless_innodb_lock_cancel_record_page_write(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no)
{
  if (trx == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const uint64_t packed_page= page_write_pack(space_id, page_no);
  trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages;
  if (pages == nullptr ||
      std::find(pages->begin(), pages->end(), packed_page) == pages->end())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const int result=
      mylite_ownerless_innodb_lock_release_page_write(trx, space_id, page_no);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return result;

  pages->erase(std::remove(pages->begin(), pages->end(), packed_page),
               pages->end());
  trx->mylite_ownerless_rebuild_modified_page_set();
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_lock_release_clean_record_page_write(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no)
{
  if (trx == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const uint64_t packed_page= page_write_pack(space_id, page_no);
  if (!transaction_has_page_write_entry(trx, packed_page) ||
      trx->mylite_ownerless_dirty_page_contains(packed_page) ||
      transaction_has_page_write_image(trx, packed_page))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return mylite_ownerless_innodb_lock_cancel_record_page_write(
      trx, space_id, page_no);
}

extern "C" int
mylite_ownerless_innodb_lock_acquire_transaction_page_write_gate(
    trx_t *trx,
    uint32_t space_id,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0;
  if (trx == nullptr || space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const uint64_t gate_page=
      page_write_transaction_gate_for_space(trx, space_id);
  if (transaction_has_page_write_gate(trx, gate_page))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint32_t acquire_flags= 0;
  const int result= mylite_ownerless_innodb_lock_acquire_page_write(
      trx,
      static_cast<uint32_t>(gate_page >> 32),
      static_cast<uint32_t>(gate_page),
      timeout_ms,
      &acquire_flags);
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= acquire_flags;
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    note_transaction_page_write_gate(trx, gate_page);
  return result;
}

extern "C" int
mylite_ownerless_innodb_lock_acquire_transaction_page_read_gate(
    trx_t *trx,
    uint32_t space_id,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0;
  if (trx == nullptr || space_id >= SRV_TMP_SPACE_ID ||
      space_id == TRX_SYS_SPACE || srv_is_undo_tablespace(space_id))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const uint64_t gate_page= page_write_pack(
      space_id, MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO);
  if (transaction_has_page_write_entry(trx, gate_page))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint32_t acquire_flags= 0;
  const int result= mylite_ownerless_innodb_lock_acquire_page_write_low(
      trx, space_id, MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO,
      timeout_ms, &acquire_flags, MYLITE_OWNERLESS_INNODB_LOCK_MODE_S, false);
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= acquire_flags;
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    note_transaction_page_write_gate(trx, gate_page);
  return result;
}

extern "C" int mylite_ownerless_innodb_lock_release_page_write(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no)
{
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  const bool known_token=
      trx != nullptr && transaction_has_page_write_entry(
                            trx, page_write_pack(space_id, page_no));
  if (!ownerless_lock_hooks_enabled())
  {
    if (known_token)
    {
      ownerless_coordination_error.store(true, std::memory_order_release);
      return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  }

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_page_write_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    if (known_token)
    {
      ownerless_coordination_error.store(true, std::memory_order_release);
      return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  }

  const trx_id_t trx_id= page_write_transaction_id(trx);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const uint32_t mode=
      page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO
          ? MYLITE_OWNERLESS_INNODB_LOCK_MODE_S
          : MYLITE_OWNERLESS_INNODB_LOCK_MODE_X;

  const int result= hook(trx_id,
                         MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
                         space_id,
                         page_no,
                         MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
                         mode,
                         0U,
                         context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE || known_token))
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE && known_token
             ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
             : result;
}

extern "C" int mylite_ownerless_innodb_page_write_active(
    uint32_t space_id,
    uint32_t page_no,
    int *out_active)
{
  if (out_active != nullptr)
    *out_active= 0;
  if (out_active == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_page_write_active_callback hook=
      page_write_active_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  return hook(space_id, page_no, out_active, context);
}

extern "C" void mylite_ownerless_innodb_lock_release_transaction_page_writes(
    trx_t *trx)
{
  if (trx == nullptr)
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    if (!trx->mylite_ownerless_modified_pages_empty() ||
        !trx->mylite_ownerless_native_support_page_write_pages_empty())
      ownerless_coordination_error.store(true, std::memory_order_release);
    return;
  }

  bool released= true;
  const trx_id_t page_write_trx_id= trx->mylite_ownerless_page_write_trx_id;
  if (page_write_trx_id != 0)
    released= release_transaction_page_writes(page_write_trx_id) ==
              MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const trx_id_t trx_id= transaction_lock_id(trx);
  if (trx_id != 0 && trx_id != page_write_trx_id)
    released= release_transaction_page_writes(trx_id) ==
                  MYLITE_OWNERLESS_INNODB_LOCK_OK &&
              released;
  if (released)
    trx->mylite_ownerless_modified_pages_clear();
}

extern "C" void
mylite_ownerless_innodb_lock_release_transaction_page_write_gates(trx_t *trx)
{
  if (trx == nullptr)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages;
  if (pages == nullptr)
    return;

  for (auto page= pages->begin(); page != pages->end();)
  {
    const uint64_t packed_page= *page;
    if (!packed_page_write_transaction_gate(packed_page))
    {
      ++page;
      continue;
    }
    if (transaction_should_keep_page_write_gate(trx, packed_page))
    {
      ++page;
      continue;
    }
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    const int result= mylite_ownerless_innodb_lock_release_page_write(
        trx, space_id, page_no);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      page= pages->erase(page);
    else
    {
      handle_hook_result("release page-write gate", result);
      ++page;
    }
  }
  trx->mylite_ownerless_rebuild_modified_page_set();
}

extern "C" void
mylite_ownerless_innodb_lock_release_transaction_clean_page_writes(trx_t *trx)
{
  if (trx == nullptr)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages;
  if (pages == nullptr)
    return;

  for (auto page= pages->begin(); page != pages->end();)
  {
    const uint64_t packed_page= *page;
    const bool keep=
        transaction_should_keep_statement_page_write(trx, packed_page);
    if (keep)
    {
      ++page;
      continue;
    }
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    const int result= mylite_ownerless_innodb_lock_release_page_write(
        trx, space_id, page_no);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      page= pages->erase(page);
    else
    {
      handle_hook_result("release clean page write", result);
      ++page;
    }
  }
  trx->mylite_ownerless_rebuild_modified_page_set();
}

extern "C" int mylite_ownerless_innodb_lock_publish_record_wait(
    const ib_lock_t *wait_lock,
    const ib_lock_t *blocker_lock)
{
  if (!wait_lock_publishable(wait_lock) ||
      !blocker_lock_publishable(blocker_lock) ||
      wait_lock->is_table() ||
      wait_lock->index == nullptr ||
      wait_lock->index->id == 0 ||
      wait_lock->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE))
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return missing_required_hook_result(wait_lock->trx);
  if (ownerless_coordination_error.load(std::memory_order_acquire))
  {
    wait_lock->trx->mylite_ownerless_coordination_fault= true;
    wait_lock->trx->error_state= DB_ERROR;
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const ulint heap_no= lock_rec_find_set_bit(wait_lock);
  if (heap_no == ULINT_UNDEFINED)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  mylite_ownerless_innodb_lock_wait_record_callback hook=
      wait_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return missing_required_hook_result(wait_lock->trx);

  const trx_id_t trx_id= lock_transaction_id(wait_lock, true);
  const trx_id_t blocker_trx_id= lock_transaction_id(blocker_lock, true);
  if (trx_id == 0 || blocker_trx_id == 0)
    return missing_required_hook_result(wait_lock->trx);
  return normalize_required_hook_result(
      wait_lock->trx,
      hook(trx_id, wait_lock->index->id,
           wait_lock->un_member.rec_lock.page_id.space(),
           wait_lock->un_member.rec_lock.page_id.page_no(),
           static_cast<uint32_t>(heap_no), normalized_lock_mode(wait_lock),
           record_lock_flags(wait_lock, static_cast<uint32_t>(heap_no)),
           blocker_trx_id, context));
}

extern "C" void mylite_ownerless_innodb_lock_clear_transaction_wait(trx_t *trx)
{
  if (trx == nullptr)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  const trx_id_t page_write_trx_id= trx->mylite_ownerless_page_write_trx_id;
  if (page_write_trx_id != 0)
    clear_transaction_wait(page_write_trx_id, trx);

  const trx_id_t trx_id= transaction_lock_id(trx);
  if (trx_id != 0 && trx_id != page_write_trx_id)
    clear_transaction_wait(trx_id, trx);
}

extern "C" void mylite_ownerless_innodb_lock_forget_transaction(trx_t *trx)
{
  if (trx != nullptr)
  {
    if (!ownerless_lock_hooks_enabled())
    {
      trx->mylite_ownerless_page_write_trx_id= 0;
      trx->mylite_ownerless_lock_trx_id= 0;
      return;
    }
    mylite_ownerless_innodb_lock_clear_transaction_wait(trx);
    bool records_released= true;
    const trx_id_t lock_trx_id= trx->mylite_ownerless_lock_trx_id;
    if (lock_trx_id != 0)
      records_released= release_transaction_records(lock_trx_id) ==
                        MYLITE_OWNERLESS_INNODB_LOCK_OK;
    if (trx->id != 0 && trx->id != lock_trx_id)
      records_released= release_transaction_records(trx->id) ==
                            MYLITE_OWNERLESS_INNODB_LOCK_OK &&
                        records_released;
    if (!records_released)
      ownerless_coordination_error.store(true, std::memory_order_release);
    mylite_ownerless_innodb_lock_release_transaction_page_writes(trx);
    if (!ownerless_coordination_error.load(std::memory_order_acquire))
    {
      trx->mylite_ownerless_page_write_trx_id= 0;
      trx->mylite_ownerless_lock_trx_id= 0;
    }
  }
}

extern "C" int
mylite_ownerless_innodb_set_statement_execution_active(int enabled)
{
  const bool previous= mylite_ownerless_statement_execution_active;
  mylite_ownerless_statement_execution_active= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_statement_execution_active(void)
{
  return mylite_ownerless_statement_execution_active ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_visible_fast_path(int enabled)
{
  const bool previous= mylite_ownerless_statement_visible_fast_path;
  mylite_ownerless_statement_visible_fast_path= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_statement_visible_fast_path(void)
{
  return mylite_ownerless_statement_visible_fast_path ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_explicit_transaction(int enabled)
{
  const bool previous= mylite_ownerless_statement_explicit_transaction;
  mylite_ownerless_statement_explicit_transaction= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_statement_explicit_transaction(void)
{
  return mylite_ownerless_statement_explicit_transaction ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_deferred_page_publish(int enabled)
{
  if (enabled == 0 && mylite_ownerless_statement_deferred_page_publish)
  {
    const int flush_result= mylite_ownerless_innodb_redo_flush_deferred();
    if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    {
      ownerless_coordination_error.store(true, std::memory_order_release);
      return mylite_ownerless_statement_deferred_page_publish ? 1 : 0;
    }
  }
  const bool previous= mylite_ownerless_statement_deferred_page_publish;
  mylite_ownerless_statement_deferred_page_publish= enabled != 0;
  if (enabled != 0 && !previous)
  {
    mylite_ownerless_statement_deferred_redo_admission_ready=
        ownerless_lock_hooks_enabled() &&
        !mylite_ownerless_innodb_test_faults_enabled_fast() &&
        redo_written_leave_callback.load(std::memory_order_acquire) !=
            nullptr &&
        callback_context.load(std::memory_order_acquire) != nullptr;
  }
  else if (enabled == 0)
    mylite_ownerless_statement_deferred_redo_admission_ready= false;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_statement_deferred_page_publish(void)
{
  return mylite_ownerless_statement_deferred_page_publish ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_set_pages_visible_force(int enabled)
{
  const bool previous= mylite_ownerless_pages_visible_force;
  mylite_ownerless_pages_visible_force= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_pages_visible_force(void)
{
  return mylite_ownerless_pages_visible_force ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_page_write_refresh_bypass(int enabled)
{
  const bool previous= mylite_ownerless_page_write_refresh_bypass;
  mylite_ownerless_page_write_refresh_bypass= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_page_write_refresh_bypass(void)
{
  return mylite_ownerless_page_write_refresh_bypass ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_plain_read(int enabled)
{
  const bool previous= mylite_ownerless_statement_plain_read;
  mylite_ownerless_statement_plain_read= enabled != 0;
  mylite_ownerless_statement_plain_read_pages_refreshed= false;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_statement_plain_read(void)
{
  return mylite_ownerless_statement_plain_read ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_plain_read_preserve_local_pages(
    int enabled)
{
  const bool previous=
      mylite_ownerless_statement_plain_read_preserve_local_pages;
  mylite_ownerless_statement_plain_read_preserve_local_pages= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_statement_plain_read_preserves_local_pages(void)
{
  return mylite_ownerless_statement_plain_read_preserve_local_pages ? 1 : 0;
}

extern "C" void
mylite_ownerless_innodb_refresh_statement_plain_read_pages_once(void)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() ||
      !mylite_ownerless_statement_plain_read ||
      mylite_ownerless_statement_plain_read_pages_refreshed)
    return;

  const uint64_t visible_lsn= mylite_ownerless_innodb_external_page_visibility();
  if (visible_lsn == 0)
    return;
  mylite_ownerless_statement_plain_read_pages_refreshed= true;
  if (page_visible_lsn_is_retained)
    mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_preserve_clean_no_skip(
        visible_lsn);
  else
    mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_preserve_clean_no_skip(
        visible_lsn);
}

extern "C" int mylite_ownerless_innodb_set_statement_dictionary_ddl(
    int enabled)
{
  const bool previous= mylite_ownerless_statement_dictionary_ddl;
  mylite_ownerless_statement_dictionary_ddl= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int mylite_ownerless_innodb_statement_dictionary_ddl(void)
{
  return mylite_ownerless_statement_dictionary_ddl ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_statement_suppress_native_lifecycle_refresh(
    int enabled)
{
  const bool previous=
      mylite_ownerless_statement_suppress_native_lifecycle_refresh;
  mylite_ownerless_statement_suppress_native_lifecycle_refresh= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_statement_suppress_native_lifecycle_refresh(void)
{
  return mylite_ownerless_statement_suppress_native_lifecycle_refresh ? 1 : 0;
}

struct ownerless_page_publish_batch_scope
{
  explicit ownerless_page_publish_batch_scope(bool enabled_arg) noexcept
      : enabled(enabled_arg)
  {
    if (enabled)
      mylite_ownerless_innodb_begin_page_publish_batch();
  }

  ~ownerless_page_publish_batch_scope()
  {
    if (enabled)
      mylite_ownerless_innodb_end_page_publish_batch();
  }

  bool enabled;
};

static void collect_transaction_page_write_pages(
    const trx_t *trx, std::vector<uint64_t> &pages, bool include_dirty_pages)
{
  if (trx == nullptr)
    return;

  auto append_pages=
      [&pages](const trx_t::mylite_ownerless_page_vector *trx_pages)
  {
    if (trx_pages == nullptr)
      return;

    for (const uint64_t packed_page : *trx_pages)
    {
      if (packed_page_write_synthetic_gate(packed_page))
        continue;
      const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
      if (space_id >= SRV_TMP_SPACE_ID)
        continue;
      pages.push_back(packed_page);
    }
  };

  if (include_dirty_pages)
    append_pages(trx->mylite_ownerless_dirty_pages_for_read());
  append_pages(trx->mylite_ownerless_modified_pages_for_read());

  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
}

static void collect_transaction_dirty_page_write_pages(
    const trx_t *trx, std::vector<uint64_t> &pages)
{
  if (trx == nullptr)
    return;

  const trx_t::mylite_ownerless_page_vector *trx_pages=
      trx->mylite_ownerless_dirty_pages_for_read();
  if (trx_pages == nullptr)
    return;

  for (const uint64_t packed_page : *trx_pages)
  {
    if (packed_page_write_synthetic_gate(packed_page))
      continue;
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    if (space_id >= SRV_TMP_SPACE_ID)
      continue;
    pages.push_back(packed_page);
  }

  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
}

static lsn_t ownerless_flush_wait_lsn(uint64_t flush_lsn)
{
  const lsn_t lsn= static_cast<lsn_t>(flush_lsn);
  return lsn >= LSN_MAX - 1 ? LSN_MAX - 1 : lsn + 1;
}

enum class transaction_page_image_buffer_state
{
  missing,
  matches,
  same_lsn_mismatch,
  different_lsn
};

struct transaction_page_image_source
{
  const byte *page;
  uint32_t page_size;
  bool compressed;
};

static bool transaction_page_image_compressed_frame_page_type_stored_uncompressed(
    uint16_t page_type) noexcept
{
  switch (page_type) {
  case FIL_PAGE_TYPE_ALLOCATED:
  case FIL_PAGE_INODE:
  case FIL_PAGE_IBUF_BITMAP:
  case FIL_PAGE_TYPE_FSP_HDR:
  case FIL_PAGE_TYPE_XDES:
    return true;
  default:
    return false;
  }
}

static bool transaction_page_image_has_native_support(
    const byte *page) noexcept
{
  if (page == nullptr)
    return false;

  switch (fil_page_get_type(page))
  {
  case FIL_PAGE_TYPE_ALLOCATED:
  case FIL_PAGE_UNDO_LOG:
  case FIL_PAGE_INODE:
  case FIL_PAGE_IBUF_FREE_LIST:
  case FIL_PAGE_IBUF_BITMAP:
  case FIL_PAGE_TYPE_SYS:
  case FIL_PAGE_TYPE_TRX_SYS:
  case FIL_PAGE_TYPE_FSP_HDR:
  case FIL_PAGE_TYPE_XDES:
    return true;
  default:
    return false;
  }
}

static bool transaction_page_image_source_for_publish(
    const buf_page_t &bpage,
    transaction_page_image_source *out_source) noexcept
{
  ut_ad(out_source != nullptr);

  const ulint zip_size= bpage.zip_size();
  if (zip_size != 0)
  {
    if (bpage.frame != nullptr &&
        transaction_page_image_compressed_frame_page_type_stored_uncompressed(
            fil_page_get_type(bpage.frame)))
    {
      out_source->page= bpage.frame;
      out_source->page_size= static_cast<uint32_t>(zip_size);
      out_source->compressed= true;
      return true;
    }
    if (bpage.zip.data == nullptr)
      return false;
    out_source->page= bpage.zip.data;
    out_source->page_size= static_cast<uint32_t>(zip_size);
    out_source->compressed= true;
    return true;
  }

  if (bpage.frame == nullptr)
    return false;
  out_source->page= bpage.frame;
  out_source->page_size= static_cast<uint32_t>(bpage.physical_size());
  out_source->compressed= false;
  return true;
}

static transaction_page_image_buffer_state transaction_page_image_buffer(
    const trx_t::mylite_ownerless_page_image &image)
{
  const uint32_t space_id= static_cast<uint32_t>(image.packed_page >> 32);
  const uint32_t page_no= static_cast<uint32_t>(image.packed_page);
  fil_space_t *space= fil_space_t::get(space_id);
  const bool full_crc32= space != nullptr && space->full_crc32();
  if (space != nullptr)
    space->release();
  transaction_page_image_buffer_state state=
      transaction_page_image_buffer_state::missing;

  mtr_t mtr(nullptr);
  mtr.start();
  dberr_t err= DB_SUCCESS;
  if (buf_block_t *block= buf_page_get_gen(
          page_id_t(space_id, page_no), 0, RW_S_LATCH, nullptr,
          BUF_GET_IF_IN_POOL, &mtr, &err))
  {
    state= transaction_page_image_buffer_state::same_lsn_mismatch;
    const buf_page_t &bpage= block->page;
    transaction_page_image_source page_source;
    if (bpage.in_file() &&
        transaction_page_image_source_for_publish(bpage, &page_source) &&
        page_source.compressed == image.compressed &&
        page_source.page_size == image.page_size)
    {
      const byte *source= page_source.page;
      const lsn_t page_lsn= mach_read_from_8(source + FIL_PAGE_LSN);
      if (page_lsn == image.page_lsn &&
          memcmp(source, image.page.data(), image.page_size) == 0)
        state= transaction_page_image_buffer_state::matches;
      else if (page_lsn == image.page_lsn)
      {
        std::vector<byte, ut_allocator<byte> > normalized_page= image.page;
        byte *page= normalized_page.data();
        if (page_source.compressed)
          buf_flush_update_zip_checksum(page, image.page_size);
        else
          buf_flush_init_for_writing(nullptr, page, nullptr, full_crc32);
        if (memcmp(source, page, image.page_size) == 0)
          state= transaction_page_image_buffer_state::matches;
      }
      else
        state= transaction_page_image_buffer_state::different_lsn;
    }
  }
  mtr.commit();

  return state;
}

static uint64_t publish_transaction_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn, bool publish_rollback_images,
    bool publish_terminal_rollback_barriers)
{
  if (trx != nullptr && !publish_rollback_images)
    trx->mylite_ownerless_page_write_deferred_pages_published= false;
  if (trx == nullptr || visible_lsn == 0 ||
      !mylite_ownerless_innodb_lock_has_hooks())
    return visible_lsn;
  std::vector<uint64_t> pages;
  collect_transaction_page_write_pages(trx, pages, true);
  std::vector<uint64_t> dirty_pages;
  collect_transaction_dirty_page_write_pages(trx, dirty_pages);
  trx_t::mylite_ownerless_page_image_vector *images=
      trx->mylite_ownerless_page_images;
  if (pages.empty() && (images == nullptr || images->empty()))
    return visible_lsn;

  ownerless_page_publish_batch_scope page_publish_batch(true);
  uint64_t maximum_observed_lsn= visible_lsn;
  std::vector<uint64_t> successful_image_pages;
  if ((!trx->in_rollback || publish_rollback_images) && images != nullptr)
  {
    successful_image_pages.reserve(images->size());
    for (trx_t::mylite_ownerless_page_image &image : *images)
    {
      if (image.page_lsn == 0 || image.page_size == 0 ||
          image.page.size() != image.page_size)
        continue;
      const uint32_t space_id= static_cast<uint32_t>(image.packed_page >> 32);
      const uint32_t page_no= static_cast<uint32_t>(image.packed_page);
      const transaction_page_image_buffer_state buffer_state=
          transaction_page_image_buffer(image);
      if (buffer_state ==
          transaction_page_image_buffer_state::same_lsn_mismatch)
      {
        continue;
      }
      /*
      The transaction image is the commit-time proof for its own visible LSN.
      A resident buffer page with another LSN may already reflect a later local
      or peer attempt, especially around record-lock grant crash faults.  Do
      not let that invalidate a bounded committed image; normal commit images
      can publish at their own page LSN. Rollback publication stays bounded by
      the caller's visible boundary.
      */
      const uint64_t image_visible_lsn= publish_rollback_images
          ? visible_lsn
          : std::max<uint64_t>(visible_lsn, image.page_lsn);
      if (image.page_lsn > image_visible_lsn)
      {
        continue;
      }

      fil_space_t *space= fil_space_t::get(space_id);
      const bool full_crc32= space != nullptr && space->full_crc32();
      if (space != nullptr)
        space->release();

      /*
      The captured image is private to this transaction and will be cleared
      during transaction cleanup. Prepare it in place instead of allocating a
	      second page-sized buffer for the publish call. Savepoint rollback
	      crash-boundary publication is different: the transaction may continue
	      and commit, so preserve the transaction image cache byte-for-byte.
      */
      std::vector<byte, ut_allocator<byte> > rollback_page;
      byte *page= image.page.data();
      if (publish_rollback_images)
      {
        rollback_page= image.page;
        page= rollback_page.data();
      }
      if (image.compressed)
        buf_flush_update_zip_checksum(page, image.page_size);
      else
        buf_flush_init_for_writing(nullptr, page, nullptr, full_crc32);

      const uint64_t publish_lsn= image_visible_lsn;
      mylite_ownerless_innodb_deep_perf_count(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_IMAGE_ATTEMPTS);
      const bool rollback_proof_publish= publish_rollback_images;
      const uint32_t publish_flags=
          rollback_proof_publish
              ? MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY |
                    (publish_terminal_rollback_barriers
                         ? MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_ROLLBACK_BARRIER
                         : 0U)
              : 0U;
      const int result=
          rollback_proof_publish || transaction_page_image_has_native_support(page)
              ? mylite_ownerless_innodb_try_publish_page_version_with_flags(
                    space_id, page_no, image.page_lsn, publish_lsn, page,
                    image.page_size, publish_flags)
              : mylite_ownerless_innodb_publish_page_version_with_flags(
                    space_id, page_no, image.page_lsn, publish_lsn, page,
                    image.page_size, publish_flags);
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      {
        mylite_ownerless_innodb_deep_perf_count(
            MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_IMAGE_PUBLISHED);
        trx->mylite_ownerless_page_write_published_page= true;
        if (publish_lsn > maximum_observed_lsn)
          maximum_observed_lsn= publish_lsn;
        successful_image_pages.push_back(image.packed_page);
      }
    }
  }
  if (!successful_image_pages.empty())
  {
    std::sort(successful_image_pages.begin(), successful_image_pages.end());
    successful_image_pages.erase(std::unique(successful_image_pages.begin(),
                                             successful_image_pages.end()),
                                 successful_image_pages.end());
  }
  const bool rollback_proof_publish=
      trx->in_rollback && publish_rollback_images;
  for (uint64_t packed_page : pages)
  {
    if (!successful_image_pages.empty() &&
        std::binary_search(successful_image_pages.begin(),
                           successful_image_pages.end(), packed_page))
      continue;

    /*
    User payload records for transaction-deferred pages must come from the
    transaction-private image cache.  A later buffer-pool snapshot can include
    an uncommitted or rolled-back local image for the same page; if the private
    image is missing, the commit path falls back to native flush visibility.
    Rollback images are durability proof only and are skipped by normal readers
    and tablespace replay. A restored predecessor page must not become a
    replayable committed version: native redo could otherwise apply the
    already-committed predecessor changes over it again.
    */
    if (!publish_rollback_images)
      continue;

    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    uint64_t published_pages= 0;
    mylite_ownerless_innodb_deep_perf_count(
        MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_ATTEMPTS);
    const uint32_t publish_flags=
        rollback_proof_publish
            ? MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_PROOF_ONLY |
                  (publish_terminal_rollback_barriers
                       ? MYLITE_OWNERLESS_INNODB_PAGE_PUBLISH_ROLLBACK_BARRIER
                       : 0U)
            : 0U;
    const lsn_t observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
        space_id, page_no, static_cast<lsn_t>(visible_lsn), false,
        &published_pages, false, publish_flags);
    if (published_pages != 0)
    {
      mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_PUBLISHED,
          published_pages);
      trx->mylite_ownerless_page_write_published_page= true;
      successful_image_pages.push_back(packed_page);
    }
    if (observed_lsn > maximum_observed_lsn)
      maximum_observed_lsn= observed_lsn;
    if (observed_lsn > visible_lsn)
    {
      published_pages= 0;
      mylite_ownerless_innodb_deep_perf_count(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_RETRY_ATTEMPTS);
      const lsn_t second_observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
          space_id, page_no, observed_lsn, false, &published_pages, false,
          publish_flags);
      if (published_pages != 0)
      {
        mylite_ownerless_innodb_deep_perf_add(
            MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_RETRY_PUBLISHED,
            published_pages);
        trx->mylite_ownerless_page_write_published_page= true;
        successful_image_pages.push_back(packed_page);
      }
      if (second_observed_lsn > maximum_observed_lsn)
        maximum_observed_lsn= second_observed_lsn;
    }
  }
  if (trx->in_rollback)
  {
    if (publish_rollback_images && successful_image_pages.empty())
      return 0;
    return maximum_observed_lsn;
  }
  if (dirty_pages.empty())
    trx->mylite_ownerless_page_write_deferred_pages_published= true;
  else if (!successful_image_pages.empty())
  {
    std::sort(successful_image_pages.begin(), successful_image_pages.end());
    successful_image_pages.erase(std::unique(successful_image_pages.begin(),
                                             successful_image_pages.end()),
                                 successful_image_pages.end());
    trx->mylite_ownerless_page_write_deferred_pages_published=
        std::all_of(dirty_pages.begin(), dirty_pages.end(),
                    [&successful_image_pages](uint64_t packed_page) {
                      return std::binary_search(successful_image_pages.begin(),
                                                successful_image_pages.end(),
                                                packed_page);
                    });
  }
  const bool history_proof_published=
      !trx->mylite_ownerless_history_proof_active ||
      (trx->mylite_ownerless_history_proof_rseg_published &&
       trx->mylite_ownerless_history_proof_undo_published);
  if (trx->mylite_ownerless_page_write_deferred_pages_published &&
      history_proof_published &&
      trx->mylite_ownerless_page_write_published_page)
    trx->mylite_ownerless_page_write_publish_failed= false;

  return maximum_observed_lsn;
}

extern "C" uint64_t mylite_ownerless_innodb_publish_transaction_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn)
{
  return publish_transaction_pages_to_lsn(trx, visible_lsn, false, false);
}

extern "C" uint64_t mylite_ownerless_innodb_publish_rollback_proof_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn)
{
  if (visible_lsn == 0)
    visible_lsn= log_get_lsn();
  return publish_transaction_pages_to_lsn(trx, visible_lsn, true, false);
}

extern "C" uint64_t mylite_ownerless_innodb_publish_rollback_barrier_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn)
{
  if (visible_lsn == 0)
    visible_lsn= log_get_lsn();
  return publish_transaction_pages_to_lsn(trx, visible_lsn, true, true);
}

extern "C" uint64_t
mylite_ownerless_innodb_publish_transaction_buffer_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn)
{
  if (trx == nullptr || visible_lsn == 0 ||
      !mylite_ownerless_innodb_lock_has_hooks())
    return visible_lsn;

  std::vector<uint64_t> pages;
  collect_transaction_page_write_pages(trx, pages, true);
  if (pages.empty())
    return visible_lsn;

  ownerless_page_publish_batch_scope page_publish_batch(true);
  uint64_t maximum_observed_lsn= visible_lsn;
  for (uint64_t packed_page : pages)
  {
    if (trx->mylite_ownerless_page_write_deferred_pages_published &&
        transaction_has_page_write_image(trx, packed_page))
      continue;

    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    uint64_t published_pages= 0;
    mylite_ownerless_innodb_deep_perf_count(
        MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_ATTEMPTS);
    const lsn_t observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
        space_id, page_no, static_cast<lsn_t>(visible_lsn), false,
        &published_pages, false);
    if (published_pages != 0)
    {
      mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_PUBLISHED,
          published_pages);
      trx->mylite_ownerless_page_write_published_page= true;
    }
    if (observed_lsn > maximum_observed_lsn)
      maximum_observed_lsn= observed_lsn;
    if (observed_lsn > visible_lsn)
    {
      published_pages= 0;
      mylite_ownerless_innodb_deep_perf_count(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_RETRY_ATTEMPTS);
      const lsn_t second_observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
          space_id, page_no, observed_lsn, false, &published_pages, false);
      if (published_pages != 0)
      {
        mylite_ownerless_innodb_deep_perf_add(
            MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_RETRY_PUBLISHED,
            published_pages);
        trx->mylite_ownerless_page_write_published_page= true;
      }
      if (second_observed_lsn > maximum_observed_lsn)
        maximum_observed_lsn= second_observed_lsn;
    }
  }

  trx->mylite_ownerless_page_write_deferred_pages_published= true;
  if (trx->mylite_ownerless_page_write_published_page)
    trx->mylite_ownerless_page_write_publish_failed= false;

  return maximum_observed_lsn;
}

extern "C" void mylite_ownerless_innodb_flush_dirty_pages_to_lsn(uint64_t visible_lsn)
{
  if (visible_lsn == 0)
  {
    buf_flush_sync();
    return;
  }

  const lsn_t flush_lsn= std::min<lsn_t>(
      static_cast<lsn_t>(visible_lsn) + 1, LSN_MAX - 1);
  buf_flush_wait_flushed(flush_lsn);

  if (ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    publish_pages_visible_lsn(visible_lsn);
}

extern "C" int mylite_ownerless_innodb_publish_pages_visible_lsn(
    uint64_t visible_lsn)
{
  return publish_pages_visible_lsn(visible_lsn);
}

extern "C" void mylite_ownerless_innodb_publish_dirty_pages_to_lsn(
    uint64_t visible_lsn)
{
  if (visible_lsn == 0 || !mylite_ownerless_innodb_lock_has_hooks())
    return;

  ownerless_page_publish_batch_scope page_publish_batch(true);
  buf_flush_publish_ownerless_pages_to_lsn(static_cast<lsn_t>(visible_lsn));
}

extern "C" void mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn(
    uint64_t visible_lsn)
{
  if (visible_lsn == 0 || !mylite_ownerless_innodb_lock_has_hooks())
    return;

  std::vector<uint64_t> pages;
  collect_buffer_pool_file_pages(pages);

  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  ownerless_page_publish_batch_scope page_publish_batch(!pages.empty());
  for (uint64_t packed_page : pages)
  {
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    uint64_t published_pages= 0;
    mylite_ownerless_innodb_deep_perf_count(
        MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_BUFFER_POOL_SCAN_ATTEMPTS);
    buf_flush_publish_ownerless_page_to_lsn(
        space_id, page_no, static_cast<lsn_t>(visible_lsn), true,
        &published_pages);
    if (published_pages != 0)
      mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_BUFFER_POOL_SCAN_PUBLISHED,
          published_pages);
  }
}

extern "C" void mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(
    uint64_t flush_lsn)
{
  if (flush_lsn == 0)
    return;

  buf_flush_wait_flushed(ownerless_flush_wait_lsn(flush_lsn));
  mylite_ownerless_os_aio_wait_until_no_pending_writes_profiled(false);
}

extern "C" uint64_t
mylite_ownerless_innodb_flush_transaction_pages_for_page_writes(
    trx_t *trx,
    uint64_t flush_lsn,
    uint64_t *exact_flushed_pages,
    uint64_t *fallback_rounds)
{
  if (exact_flushed_pages != nullptr)
    *exact_flushed_pages= 0;
  if (fallback_rounds != nullptr)
    *fallback_rounds= 0;
  if (trx == nullptr || flush_lsn == 0)
    return 0;

  std::vector<uint64_t> pages;
  collect_transaction_page_write_pages(trx, pages, true);
  if (pages.empty())
    return 0;

  const lsn_t sync_lsn= ownerless_flush_wait_lsn(flush_lsn);
  uint64_t total_flushed_pages= 0;
  uint64_t total_exact_pages= 0;
  uint64_t total_fallback_rounds= 0;
  for (auto it= pages.begin(); it != pages.end(); )
  {
    const uint32_t space_id= static_cast<uint32_t>(*it >> 32);
    std::vector<uint32_t> page_nos;
    for (; it != pages.end() &&
           static_cast<uint32_t>(*it >> 32) == space_id; ++it)
    {
      page_nos.push_back(static_cast<uint32_t>(*it));
    }

    ulint exact_pages= 0;
    ulint fallback_count= 0;
    const ulint flushed_pages= buf_flush_wait_space_pages_flushed(
        space_id,
        page_nos.data(),
        page_nos.size(),
        sync_lsn,
        &exact_pages,
        &fallback_count);
    total_flushed_pages+= flushed_pages;
    total_exact_pages+= exact_pages;
    total_fallback_rounds+= fallback_count;
  }

  if (exact_flushed_pages != nullptr)
    *exact_flushed_pages= total_exact_pages;
  if (fallback_rounds != nullptr)
    *fallback_rounds= total_fallback_rounds;
  return total_flushed_pages;
}

extern "C" uint64_t mylite_ownerless_innodb_flush_space_dirty_pages_to_lsn(
    uint32_t space_id,
    uint64_t flush_lsn)
{
  if (flush_lsn == 0)
    return 0;

  return buf_flush_wait_space_flushed(space_id,
                                      static_cast<lsn_t>(flush_lsn));
}

extern "C" uint64_t mylite_ownerless_innodb_flush_history_pages_to_lsn(
    uint32_t space_id,
    uint32_t rseg_page_no,
    uint32_t undo_page_no,
    uint64_t flush_lsn,
    uint64_t *exact_flushed_pages,
    uint64_t *fallback_rounds)
{
  if (exact_flushed_pages != nullptr)
    *exact_flushed_pages= 0;
  if (fallback_rounds != nullptr)
    *fallback_rounds= 0;

  if (flush_lsn == 0)
    return 0;

  const uint32_t page_nos[]= {rseg_page_no, undo_page_no};
  ulint exact_pages= 0;
  ulint fallback_count= 0;
  const ulint flushed_pages= buf_flush_wait_space_pages_flushed(
      space_id,
      page_nos,
      UT_ARR_SIZE(page_nos),
      static_cast<lsn_t>(flush_lsn),
      &exact_pages,
      &fallback_count);

  if (exact_flushed_pages != nullptr)
    *exact_flushed_pages= exact_pages;
  if (fallback_rounds != nullptr)
    *fallback_rounds= fallback_count;

  return flushed_pages;
}

extern "C" void mylite_ownerless_innodb_flush_space_dirty_pages(uint32_t space_id)
{
  fil_space_t *space= fil_space_t::get(space_id);
  if (space == nullptr)
    return;

  for (unsigned attempts= 0; attempts < 4; ++attempts)
  {
    if (!buf_flush_list_space(space))
      break;
  }
  space->release();
}

extern "C" void mylite_ownerless_innodb_refresh_external_pages(uint64_t latest_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || latest_lsn == 0)
    return;

  advance_external_lsn(latest_lsn);
  buf_flush_sync_batch(static_cast<lsn_t>(latest_lsn));
  refresh_replaceable_buffer_pool_pages();
}

extern "C" void
mylite_ownerless_innodb_refresh_external_pages_retained(uint64_t latest_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || latest_lsn == 0)
    return;

  advance_external_lsn(latest_lsn);
  buf_flush_sync_batch(static_cast<lsn_t>(latest_lsn));
  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= true;
  refresh_buffer_pool_pages(true, true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
}

extern "C" void mylite_ownerless_innodb_refresh_buffer_pool_pages(uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_pages(false);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" void mylite_ownerless_innodb_refresh_buffer_pool_pages_force(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_pages(true);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, false);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_native_current_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
  refresh_buffer_pool_pages(true, true, true, false, false, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_dirty_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true, false, false, false,
                            false, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_dirty_user_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true, false, false, false,
                            false, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= true;
  refresh_buffer_pool_pages(true, true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_preserve_clean_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= true;
  refresh_buffer_pool_pages(true, false, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_preserve_clean_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
  refresh_buffer_pool_pages(true, false, true, true, false, false, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
}

extern "C" void
mylite_ownerless_innodb_materialize_retained_page_for_native_write(
    uint32_t space_id, uint32_t page_no, uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  const bool previous_scrub= mylite_ownerless_retained_startup_native_write_scrub;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= true;
  mylite_ownerless_retained_startup_native_write_scrub= true;
  refresh_buffer_pool_page(space_id, page_no, true, true, false, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
  mylite_ownerless_retained_startup_native_write_scrub= previous_scrub;
}

extern "C" void mylite_ownerless_innodb_refresh_buffer_pool_pages_preserve(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_pages(false, false);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" void
mylite_ownerless_innodb_refresh_buffer_pool_pages_native_visible_boundary(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_pages(true, true, false, true, false, true);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" void mylite_ownerless_innodb_refresh_buffer_pool_pages_force_preserve(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_pages(true, false);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" int
mylite_ownerless_innodb_refresh_transaction_pages_from_native(trx_t *trx)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (trx == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  std::vector<uint64_t> pages;
  collect_transaction_page_write_pages(trx, pages, true);
  if (pages.empty())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint64_t latest_lsn= 0;
  const int observe_result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (observe_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  const bool use_page_version=
      latest_lsn != 0;
  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  if (use_page_version)
  {
    advance_external_lsn(latest_lsn);
    page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  }
  else
    page_visible_lsn= 0;
  page_visible_lsn_is_current= false;
  page_visible_lsn_is_retained= false;

  int refresh_result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  for (uint64_t packed_page : pages)
  {
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    refresh_result= refresh_buffer_pool_page(
        space_id, page_no, false, true, true, false, true, false,
        !use_page_version, false);
    if (refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      break;
  }

  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
  if (refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_read(
    uint32_t space_id, uint32_t page_no, uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (visible_lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  const int refresh_result=
      refresh_buffer_pool_page(space_id, page_no, false);
  page_visible_lsn= previous_visible_lsn;
  return refresh_result;
}

extern "C" void mylite_ownerless_innodb_evict_clean_external_pages(void)
{
  if (!srv_was_started)
    return;

  refresh_replaceable_buffer_pool_pages();
}

extern "C" int mylite_ownerless_innodb_advance_external_lsn(uint64_t latest_lsn)
{
  const bool startup_advance_enabled=
      mylite_ownerless_innodb_startup_lsn_advance_enabled.load(
          std::memory_order_acquire);
  const uint64_t startup_limit=
      mylite_ownerless_innodb_startup_lsn_advance_limit.load(
          std::memory_order_acquire);
  if (!startup_advance_enabled && !mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (startup_advance_enabled && latest_lsn > startup_limit)
  {
    mylite_ownerless_innodb_startup_lsn_limit_rejections.fetch_add(
        1, std::memory_order_relaxed);
    sql_print_error("InnoDB: ownerless startup external LSN " LSN_PF
                    " exceeds proven limit " LSN_PF,
                    latest_lsn, startup_limit);
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  }

  if (startup_advance_enabled)
  {
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    log_sys.mylite_advance_external_recovered_lsn(latest_lsn);
    log_sys.latch.wr_unlock();
  }
  else
    advance_external_lsn(latest_lsn);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int
mylite_ownerless_innodb_advance_startup_page_lsn(uint64_t latest_lsn)
{
  const bool startup_advance_enabled=
      mylite_ownerless_innodb_startup_lsn_advance_enabled.load(
          std::memory_order_acquire);
  const uint64_t startup_limit=
      mylite_ownerless_innodb_startup_lsn_advance_limit.load(
          std::memory_order_acquire);
  if (!startup_advance_enabled)
    return mylite_ownerless_innodb_advance_external_lsn(latest_lsn);
  if (latest_lsn > startup_limit)
  {
    uint64_t observed_lsn= 0;
    const int observe_result=
        mylite_ownerless_innodb_redo_observe_written(&observed_lsn);
    if (observe_result != MYLITE_OWNERLESS_INNODB_LOCK_OK ||
        latest_lsn > observed_lsn)
    {
      mylite_ownerless_innodb_startup_lsn_limit_rejections.fetch_add(
          1, std::memory_order_relaxed);
      sql_print_error("InnoDB: ownerless startup page LSN " LSN_PF
                      " exceeds proven limit " LSN_PF,
                      latest_lsn, std::max(startup_limit, observed_lsn));
      return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    }

    /*
      A live peer may write more redo after the pre-init snapshot. The startup
      observe hook exposes the contiguous written frontier, so it can extend
      the proof without admitting a merely reserved or incomplete range.
    */
    uint64_t current_limit= startup_limit;
    while (current_limit < observed_lsn &&
           !mylite_ownerless_innodb_startup_lsn_advance_limit
                .compare_exchange_weak(current_limit, observed_lsn,
                                       std::memory_order_release,
                                       std::memory_order_relaxed))
      ;
  }

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  log_sys.mylite_advance_external_recovered_lsn(latest_lsn);
  log_sys.latch.wr_unlock();
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" uint64_t
mylite_ownerless_innodb_test_startup_lsn_limit_rejections(void)
{
  return mylite_ownerless_innodb_startup_lsn_limit_rejections.load(
      std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_refresh_external_space_header(
    uint32_t space_id)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  uint64_t previous_visible_lsn= 0;
  const int visibility_result=
    push_latest_external_page_visibility(&previous_visible_lsn);
  if (visibility_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return;
  refresh_external_space_header(space_id);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" int mylite_ownerless_innodb_refresh_external_space_allocation(
    uint32_t space_id)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (mylite_ownerless_statement_dictionary_ddl ||
      mylite_ownerless_statement_suppress_native_lifecycle_refresh)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (ownerless_skip_external_page_refresh())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  uint64_t previous_visible_lsn= page_visible_lsn;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  advance_external_lsn(latest_lsn);
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);

  refresh_external_space_header(space_id);
  refresh_external_space_allocation_pages(space_id);
  page_visible_lsn= previous_visible_lsn;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int
mylite_ownerless_innodb_refresh_external_space_allocation_native_current(
    uint32_t space_id)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  advance_external_lsn(latest_lsn);
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);

  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
  bool header_refreshed= false;
  mysql_mutex_lock(&fil_system.mutex);
  if (fil_space_t *space= fil_space_get_by_id(space_id))
    header_refreshed= refresh_external_space_header(*space, true);
  mysql_mutex_unlock(&fil_system.mutex);
  if (header_refreshed)
    refresh_external_space_allocation_pages_native_current(space_id);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" void mylite_ownerless_innodb_refresh_external_space_headers(void)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  refresh_external_space_headers();
}

extern "C" void
mylite_ownerless_innodb_refresh_external_space_headers_no_skip(void)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;

  refresh_external_space_headers();
}

extern "C" void mylite_ownerless_innodb_evict_dictionary_cache(void)
{
  if (!dict_sys.is_initialised())
    return;

  uint64_t latest_lsn= 0;
  if (mylite_ownerless_innodb_redo_observe(&latest_lsn) !=
      MYLITE_OWNERLESS_INNODB_LOCK_OK)
    latest_lsn= 0;
  latest_lsn= std::max(latest_lsn, mylite_ownerless_innodb_current_lsn());
  if (latest_lsn != 0)
    mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(latest_lsn);

#ifdef BTR_CUR_HASH_ADAPT
  const bool btr_search_was_enabled= btr_search.disable();
#endif

  dict_sys.lock(SRW_LOCK_CALL);
  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    table->mylite_ownerless_referenced_foreigns_loaded= false;
    const bool can_evict= table_can_be_evicted_from_dictionary(table);
    if (can_evict)
      dict_sys.remove(table, true);
    table= prev;
  }
  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_non_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    table->mylite_ownerless_referenced_foreigns_loaded= false;
    const bool can_evict= foreign_table_can_be_reloaded_from_dictionary(table);
    if (can_evict)
      dict_sys.remove(table, false);
    table= prev;
  }
  dict_sys.unlock();

#ifdef BTR_CUR_HASH_ADAPT
  if (btr_search_was_enabled)
    btr_search.enable();
#endif
}

void reload_ownerless_foreign_key_cache_from_dictionary()
{
  if (!dict_sys.is_initialised())
    return;

  std::vector<std::string> table_names;

  dict_sys.lock(SRW_LOCK_CALL);
  auto remember_table = [&table_names](dict_table_t *table) {
    if (table == nullptr || table->is_system_db || table->is_temporary())
      return;
    const char *name= table->name.m_name;
    if (name == nullptr || !*name)
      return;
    for (const std::string &existing : table_names)
      if (existing == name)
        return;
    table_names.emplace_back(name);
  };

  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    remember_table(table);
    table= prev;
  }
  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_non_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    remember_table(table);
    table= prev;
  }

  for (const std::string &name : table_names)
  {
    dict_table_t *table= dict_sys.find_table(
        span<const char>{name.c_str(), name.size()});
    if (table == nullptr)
      continue;

    table->mylite_ownerless_referenced_foreigns_loaded= false;
    while (!table->foreign_set.empty())
      dict_foreign_remove_from_cache(*table->foreign_set.begin());
    while (!table->referenced_set.empty())
      dict_foreign_remove_from_cache(*table->referenced_set.begin());
  }

  for (const std::string &name : table_names)
  {
    if (dict_sys.find_table(span<const char>{name.c_str(), name.size()}) ==
        nullptr)
      continue;

    dict_names_t fk_tables;
    mtr_t mtr{nullptr};
    static_cast<void>(dict_load_foreigns(
        mtr, name.c_str(), nullptr, 1, true, DICT_ERR_IGNORE_FK_NOKEY,
        fk_tables));
  }

  dict_sys.unlock();
}

static void dispose_or_quarantine_repair_start(trx_t *trx)
{
  ut_ad(trx != nullptr);
  if (!trx->dispose_failed_start())
    trx_ownerless_quarantine_detached(trx);
}

static dberr_t finish_ownerless_dictionary_repair(
    trx_t *trx, dberr_t operation_error)
{
  ut_ad(trx != nullptr);
  dberr_t cleanup_error= DB_SUCCESS;
  if (operation_error == DB_SUCCESS)
  {
    cleanup_error= trx_commit_for_mysql(trx);
    operation_error= cleanup_error;
  }
  else
  {
    trx->error_state= DB_SUCCESS;
    cleanup_error= trx->rollback();
  }

  row_mysql_unlock_data_dictionary(trx);
  if (cleanup_error != DB_SUCCESS ||
      trx->mylite_ownerless_coordination_fault ||
      trx->state != TRX_STATE_NOT_STARTED)
  {
    trx_ownerless_quarantine_detached(trx);
    return DB_ERROR;
  }

  trx->free();
  return operation_error;
}

extern "C" int mylite_ownerless_innodb_repair_dictionary_rename(
    const char *old_name,
    const char *new_name)
{
  if (old_name == nullptr || new_name == nullptr || !*old_name ||
      !*new_name || strcmp(old_name, new_name) == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!mylite_ownerless_innodb_uncheckpointed_file_rename_recovery() ||
      srv_read_only_mode || !srv_was_started || recv_recovery_is_on() ||
      !dict_sys.is_initialised())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  trx_t *trx= trx_create();
  dberr_t err= trx_start_for_ddl(trx);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
    dispose_or_quarantine_repair_start(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  trx->op_info= "repairing ownerless dictionary rename";

  err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);
  if (err == DB_SUCCESS)
  {
    dict_table_t *cached_old= dict_sys.find_table(
        span<const char>{old_name, strlen(old_name)});
    if (cached_old != nullptr)
    {
      const bool removed=
          cached_old->can_be_evicted
              ? table_can_be_evicted_from_dictionary(cached_old)
              : foreign_table_can_be_reloaded_from_dictionary(cached_old);
      if (removed)
        dict_sys.remove(cached_old, cached_old->can_be_evicted);
      else
        err= DB_LOCK_WAIT_TIMEOUT;
    }
  }
  if (err == DB_SUCCESS)
  {
    dict_table_t *cached_new= dict_sys.find_table(
        span<const char>{new_name, strlen(new_name)});
    if (cached_new != nullptr)
    {
      const bool removed=
          cached_new->can_be_evicted
              ? table_can_be_evicted_from_dictionary(cached_new)
              : foreign_table_can_be_reloaded_from_dictionary(cached_new);
      if (removed)
        dict_sys.remove(cached_new, cached_new->can_be_evicted);
      else
        err= DB_LOCK_WAIT_TIMEOUT;
    }
  }
  if (err == DB_SUCCESS)
    err= row_rename_table_for_mysql(old_name, new_name, trx, RENAME_IGNORE_FK);

  err= finish_ownerless_dictionary_repair(trx, err);
  return err == DB_SUCCESS ? MYLITE_OWNERLESS_INNODB_LOCK_OK
                           : MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

extern "C" int mylite_ownerless_innodb_repair_dictionary_name_only(
    const char *old_name,
    const char *new_name)
{
  if (old_name == nullptr || new_name == nullptr || !*old_name ||
      !*new_name || strcmp(old_name, new_name) == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!mylite_ownerless_innodb_uncheckpointed_file_rename_recovery() ||
      srv_read_only_mode || !srv_was_started || recv_recovery_is_on() ||
      !dict_sys.is_initialised())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  trx_t *trx= trx_create();
  dberr_t err= trx_start_for_ddl(trx);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
    dispose_or_quarantine_repair_start(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  trx->op_info= "repairing ownerless dictionary name";

  err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);
  if (err == DB_SUCCESS)
  {
    dict_table_t *cached_new= dict_sys.find_table(
        span<const char>{new_name, strlen(new_name)});
    if (cached_new != nullptr)
    {
      const bool removed=
          cached_new->can_be_evicted
              ? table_can_be_evicted_from_dictionary(cached_new)
              : foreign_table_can_be_reloaded_from_dictionary(cached_new);
      if (removed)
        dict_sys.remove(cached_new, cached_new->can_be_evicted);
      else
        err= DB_LOCK_WAIT_TIMEOUT;
    }
  }
  if (err == DB_SUCCESS)
  {
    char new_table_name[MAX_TABLE_NAME_LEN + 1];
    char old_table_utf8[MAX_TABLE_NAME_LEN + 1];
    uint errors= 0;

    strncpy(old_table_utf8, old_name, MAX_TABLE_NAME_LEN);
    old_table_utf8[MAX_TABLE_NAME_LEN]= '\0';
    char *old_utf8_table= strchr(old_table_utf8, '/');
    const char *old_name_table= strchr(old_name, '/');
    if (old_utf8_table != nullptr && old_name_table != nullptr)
    {
      innobase_convert_to_system_charset(
          old_utf8_table + 1, old_name_table + 1, MAX_TABLE_NAME_LEN,
          &errors);
      if (errors)
      {
        strncpy(old_table_utf8, old_name, MAX_TABLE_NAME_LEN);
        old_table_utf8[MAX_TABLE_NAME_LEN]= '\0';
      }
    }

    errors= 0;
    strncpy(new_table_name, new_name, MAX_TABLE_NAME_LEN);
    new_table_name[MAX_TABLE_NAME_LEN]= '\0';
    char *new_utf8_table= strchr(new_table_name, '/');
    const char *new_name_table= strchr(new_name, '/');
    if (new_utf8_table != nullptr && new_name_table != nullptr)
    {
      innobase_convert_to_system_charset(
          new_utf8_table + 1, new_name_table + 1, MAX_TABLE_NAME_LEN,
          &errors);
      if (errors)
      {
        strncpy(new_table_name, new_name, MAX_TABLE_NAME_LEN);
        new_table_name[MAX_TABLE_NAME_LEN]= '\0';
      }
    }

    pars_info_t *info= pars_info_create();
    pars_info_add_str_literal(info, "new_table_name", new_name);
    pars_info_add_str_literal(info, "old_table_name", old_name);
    pars_info_add_str_literal(info, "old_table_name_utf8", old_table_utf8);
    pars_info_add_str_literal(info, "new_table_utf8", new_table_name);
    err= que_eval_sql(
        info,
        "PROCEDURE MYLITE_RENAME_TABLE_NAME_ONLY () IS\n"
        "gen_constr_prefix CHAR;\n"
        "new_db_name CHAR;\n"
        "foreign_id CHAR;\n"
        "new_foreign_id CHAR;\n"
        "old_db_name_len INT;\n"
        "old_t_name_len INT;\n"
        "new_db_name_len INT;\n"
        "id_len INT;\n"
        "offset INT;\n"
        "found INT;\n"
        "BEGIN\n"
        "UPDATE SYS_TABLES"
        " SET NAME = :new_table_name\n"
        " WHERE NAME = :old_table_name;\n"
        "found := 1;\n"
        "old_db_name_len := INSTR(:old_table_name, '/')-1;\n"
        "new_db_name_len := INSTR(:new_table_name, '/')-1;\n"
        "new_db_name := SUBSTR(:new_table_name, 0, new_db_name_len);\n"
        "old_t_name_len := LENGTH(:old_table_name);\n"
        "gen_constr_prefix := CONCAT(:old_table_name_utf8, '_ibfk_');\n"
        "WHILE found = 1 LOOP\n"
        " SELECT ID INTO foreign_id\n"
        " FROM SYS_FOREIGN\n"
        " WHERE FOR_NAME = :old_table_name\n"
        " AND TO_BINARY(FOR_NAME) = TO_BINARY(:old_table_name)\n"
        " LOCK IN SHARE MODE;\n"
        " IF (SQL % NOTFOUND) THEN\n"
        "  found := 0;\n"
        " ELSE\n"
        "  UPDATE SYS_FOREIGN\n"
        "  SET FOR_NAME = :new_table_name\n"
        "  WHERE ID = foreign_id;\n"
        "  id_len := LENGTH(foreign_id);\n"
        "  IF (INSTR(foreign_id, '/') > 0) THEN\n"
        "   IF (INSTR(foreign_id, gen_constr_prefix) > 0) THEN\n"
        "    offset := INSTR(foreign_id, '_ibfk_') - 1;\n"
        "    new_foreign_id := CONCAT(:new_table_utf8,\n"
        "      SUBSTR(foreign_id, offset, id_len - offset));\n"
        "   ELSE\n"
        "    new_foreign_id := CONCAT(new_db_name,\n"
        "      SUBSTR(foreign_id, old_db_name_len,\n"
        "        id_len - old_db_name_len));\n"
        "   END IF;\n"
        "   UPDATE SYS_FOREIGN\n"
        "   SET ID = new_foreign_id\n"
        "   WHERE ID = foreign_id;\n"
        "   UPDATE SYS_FOREIGN_COLS\n"
        "   SET ID = new_foreign_id\n"
        "   WHERE ID = foreign_id;\n"
        "  END IF;\n"
        " END IF;\n"
        "END LOOP;\n"
        "UPDATE SYS_FOREIGN"
        " SET REF_NAME = :new_table_name\n"
        " WHERE REF_NAME = :old_table_name\n"
        " AND TO_BINARY(REF_NAME) = TO_BINARY(:old_table_name);\n"
        "END;\n",
        trx);
  }

  err= finish_ownerless_dictionary_repair(trx, err);
  if (err == DB_SUCCESS)
  {
    mylite_ownerless_innodb_evict_dictionary_cache();
    reload_ownerless_foreign_key_cache_from_dictionary();
  }
  return err == DB_SUCCESS ? MYLITE_OWNERLESS_INNODB_LOCK_OK
                           : MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

extern "C" int mylite_ownerless_innodb_repair_foreign_key_id(
    const char *old_id,
    const char *new_id)
{
  if (old_id == nullptr || new_id == nullptr || !*old_id || !*new_id ||
      strcmp(old_id, new_id) == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!mylite_ownerless_innodb_uncheckpointed_file_rename_recovery() ||
      srv_read_only_mode || !srv_was_started || recv_recovery_is_on() ||
      !dict_sys.is_initialised())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_evict_dictionary_cache();

  trx_t *trx= trx_create();
  dberr_t err= trx_start_for_ddl(trx);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
    dispose_or_quarantine_repair_start(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  trx->op_info= "repairing ownerless foreign key id";

  err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);
  if (err == DB_SUCCESS)
  {
    pars_info_t *info= pars_info_create();
    pars_info_add_str_literal(info, "old_id", old_id);
    pars_info_add_str_literal(info, "new_id", new_id);
    err= que_eval_sql(
        info,
        "PROCEDURE MYLITE_RENAME_FOREIGN_KEY_ID () IS\n"
        "BEGIN\n"
        "UPDATE SYS_FOREIGN"
        " SET ID = :new_id\n"
        " WHERE ID = :old_id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:old_id);\n"
        "UPDATE SYS_FOREIGN_COLS"
        " SET ID = :new_id\n"
        " WHERE ID = :old_id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:old_id);\n"
        "END;\n",
        trx);
  }

  err= finish_ownerless_dictionary_repair(trx, err);
  if (err == DB_SUCCESS)
  {
    mylite_ownerless_innodb_evict_dictionary_cache();
    reload_ownerless_foreign_key_cache_from_dictionary();
  }
  return err == DB_SUCCESS ? MYLITE_OWNERLESS_INNODB_LOCK_OK
                           : MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

extern "C" int mylite_ownerless_innodb_repair_foreign_key_identity(
    const char *old_id,
    const char *new_id,
    const char *new_for_name)
{
  if (old_id == nullptr || new_id == nullptr || new_for_name == nullptr ||
      !*old_id || !*new_id || !*new_for_name)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!mylite_ownerless_innodb_uncheckpointed_file_rename_recovery() ||
      srv_read_only_mode || !srv_was_started || recv_recovery_is_on() ||
      !dict_sys.is_initialised())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_evict_dictionary_cache();

  trx_t *trx= trx_create();
  dberr_t err= trx_start_for_ddl(trx);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
    dispose_or_quarantine_repair_start(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  trx->op_info= "repairing ownerless foreign key identity";

  err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);
  if (err == DB_SUCCESS)
  {
    pars_info_t *info= pars_info_create();
    pars_info_add_str_literal(info, "old_id", old_id);
    pars_info_add_str_literal(info, "new_id", new_id);
    pars_info_add_str_literal(info, "new_for_name", new_for_name);
    err= que_eval_sql(
        info,
        "PROCEDURE MYLITE_REPAIR_FOREIGN_KEY_IDENTITY () IS\n"
        "BEGIN\n"
        "UPDATE SYS_FOREIGN"
        " SET ID = :new_id, FOR_NAME = :new_for_name\n"
        " WHERE ID = :old_id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:old_id);\n"
        "UPDATE SYS_FOREIGN_COLS"
        " SET ID = :new_id\n"
        " WHERE ID = :old_id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:old_id);\n"
        "END;\n",
        trx);
  }

  err= finish_ownerless_dictionary_repair(trx, err);
  if (err == DB_SUCCESS)
  {
    mylite_ownerless_innodb_evict_dictionary_cache();
    reload_ownerless_foreign_key_cache_from_dictionary();
  }
  return err == DB_SUCCESS ? MYLITE_OWNERLESS_INNODB_LOCK_OK
                           : MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

extern "C" int mylite_ownerless_innodb_delete_foreign_key_metadata(
    const char *id)
{
  if (id == nullptr || !*id)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!mylite_ownerless_innodb_uncheckpointed_file_rename_recovery() ||
      srv_read_only_mode || !srv_was_started || recv_recovery_is_on() ||
      !dict_sys.is_initialised())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_evict_dictionary_cache();

  trx_t *trx= trx_create();
  dberr_t err= trx_start_for_ddl(trx);
  if (UNIV_UNLIKELY(err != DB_SUCCESS))
  {
    dispose_or_quarantine_repair_start(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  trx->op_info= "deleting ownerless foreign key metadata";

  err= lock_sys_tables(trx);
  row_mysql_lock_data_dictionary(trx);
  if (err == DB_SUCCESS)
  {
    pars_info_t *info= pars_info_create();
    pars_info_add_str_literal(info, "id", id);
    err= que_eval_sql(
        info,
        "PROCEDURE MYLITE_DELETE_FOREIGN_KEY_METADATA () IS\n"
        "BEGIN\n"
        "DELETE FROM SYS_FOREIGN_COLS\n"
        " WHERE ID = :id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:id);\n"
        "DELETE FROM SYS_FOREIGN\n"
        " WHERE ID = :id\n"
        " AND TO_BINARY(ID) = TO_BINARY(:id);\n"
        "END;\n",
        trx);
  }

  err= finish_ownerless_dictionary_repair(trx, err);
  if (err == DB_SUCCESS)
  {
    mylite_ownerless_innodb_evict_dictionary_cache();
    reload_ownerless_foreign_key_cache_from_dictionary();
  }
  return err == DB_SUCCESS ? MYLITE_OWNERLESS_INNODB_LOCK_OK
                           : MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
}

extern "C" int mylite_ownerless_innodb_can_skip_external_page_refresh(void)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return ownerless_skip_external_page_refresh()
    ? MYLITE_OWNERLESS_INNODB_LOCK_OK
    : MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_write(
    const buf_block_t *block)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (block == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (ownerless_skip_external_page_refresh())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  uint64_t visible_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe_visible(&visible_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    const uint64_t effective_lsn= std::max(page_visible_lsn, visible_lsn);
    if (effective_lsn == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    advance_external_lsn(effective_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= effective_lsn;
    const int refresh_result= refresh_page_for_write(*block, true, false);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return result;
  }

  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

static int refresh_page_for_write_force(
    const buf_block_t *block, bool preserve_local_transaction_page)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (block == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (ownerless_skip_external_page_refresh())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  if (mylite_ownerless_statement_plain_read)
  {
    if (page_visible_lsn == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    const bool retained_visibility= page_visible_lsn_is_retained;
    return refresh_page_for_write(*block, true, true, true, true,
                                  retained_visibility);
  }

  if (!page_visible_lsn_is_current && page_visible_lsn != 0)
  {
    const bool retained_visibility= page_visible_lsn_is_retained;
    return refresh_page_for_write(*block, true, true, true, true,
                                  retained_visibility);
  }

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    const uint64_t effective_lsn= std::max(page_visible_lsn, latest_lsn);
    if (effective_lsn == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    if (latest_lsn != 0)
      advance_external_lsn(latest_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= effective_lsn;
    const int refresh_result=
        refresh_page_for_write(*block, true, true, true, false, false, false,
                               preserve_local_transaction_page, false, false,
                               true);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return result;
  }

  return refresh_page_for_write(*block, false, true, true, false, false,
                                false, preserve_local_transaction_page);
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_write_force(
    const buf_block_t *block)
{
  return refresh_page_for_write_force(block, true);
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_write_after_wait(
    const buf_block_t *block)
{
  return refresh_page_for_write_force(block, false);
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_current_read(
    const buf_block_t *block)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (block == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (ownerless_skip_external_page_refresh())
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  if (mylite_ownerless_statement_plain_read)
  {
    if (page_visible_lsn == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    const bool retained_visibility= page_visible_lsn_is_retained;
    return refresh_page_for_write(*block, true, true, true, true,
                                  retained_visibility);
  }

  if (!page_visible_lsn_is_current && page_visible_lsn != 0)
  {
    const bool retained_visibility= page_visible_lsn_is_retained;
    return refresh_page_for_write(*block, true, true, true, true,
                                  retained_visibility);
  }

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    const uint64_t effective_lsn= std::max(page_visible_lsn, latest_lsn);
    if (effective_lsn == 0)
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    if (latest_lsn != 0)
      advance_external_lsn(latest_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= effective_lsn;
    const bool retained_visibility= page_visible_lsn_is_retained;
    const int refresh_result=
        refresh_page_for_write(*block, true, true, true,
                               retained_visibility, true);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return result;
  }

  return refresh_page_for_write(*block, false, true, true,
                                page_visible_lsn_is_retained, true);
}

extern "C" int mylite_ownerless_innodb_refresh_external_wait_page(
    const mylite_ownerless_innodb_lock_external_wait *snapshot)
{
  if (snapshot == nullptr ||
      snapshot->kind != MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return result;
  if (latest_lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  advance_external_lsn(latest_lsn);
  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  const bool previous_retained= page_visible_lsn_is_retained;
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
  refresh_replaceable_buffer_pool_pages();
  const int refresh_result= refresh_buffer_pool_page(
      snapshot->space_id, snapshot->page_no, true, true, true, true, false,
      false, false, false);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
  page_visible_lsn_is_retained= previous_retained;
  return refresh_result;
}

extern "C" void mylite_ownerless_innodb_enable_external_page_visibility(
    uint64_t latest_lsn)
{
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= false;
  page_visible_lsn_is_retained= false;
  if (!srv_was_started)
  {
    startup_page_visible_lsn.store(0, std::memory_order_release);
    startup_native_support_page_visible_lsn.store(0, std::memory_order_release);
  }
}

extern "C" void
mylite_ownerless_innodb_enable_current_external_page_visibility(
    uint64_t latest_lsn)
{
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
  if (!srv_was_started)
  {
    startup_page_visible_lsn.store(0, std::memory_order_release);
    startup_native_support_page_visible_lsn.store(0, std::memory_order_release);
  }
}

extern "C" uint64_t mylite_ownerless_innodb_external_page_visibility(void)
{
  return page_visible_lsn;
}

extern "C" int mylite_ownerless_innodb_external_page_visibility_is_current(void)
{
  return page_visible_lsn_is_current ? 1 : 0;
}

extern "C" void mylite_ownerless_innodb_set_retained_external_page_visibility(
    int enabled)
{
  page_visible_lsn_is_retained= enabled != 0;
  if (enabled != 0 && page_visible_lsn != 0)
    startup_page_visible_lsn.store(page_visible_lsn, std::memory_order_release);
  else if (enabled == 0)
  {
    startup_page_visible_lsn.store(0, std::memory_order_release);
    startup_native_support_page_visible_lsn.store(0, std::memory_order_release);
  }
}

extern "C" int mylite_ownerless_innodb_retained_external_page_visibility(void)
{
  return page_visible_lsn_is_retained ? 1 : 0;
}

extern "C" int
mylite_ownerless_innodb_set_retained_startup_native_write_scrub(int enabled)
{
  const bool previous= mylite_ownerless_retained_startup_native_write_scrub;
  mylite_ownerless_retained_startup_native_write_scrub= enabled != 0;
  return previous ? 1 : 0;
}

extern "C" void
mylite_ownerless_innodb_set_startup_native_support_page_visibility(
    uint64_t latest_lsn)
{
  startup_native_support_page_visible_lsn.store(
      latest_lsn, std::memory_order_release);
}

extern "C" uint64_t
mylite_ownerless_innodb_startup_native_support_page_visibility(void)
{
  return startup_native_support_page_visible_lsn.load(
      std::memory_order_acquire);
}

extern "C" uint64_t
mylite_ownerless_innodb_startup_external_page_visibility(void)
{
  return startup_page_visible_lsn.load(std::memory_order_acquire);
}

extern "C" void
mylite_ownerless_innodb_clear_startup_external_page_visibility(void)
{
  startup_page_visible_lsn.store(0, std::memory_order_release);
  startup_native_support_page_visible_lsn.store(
      0, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_set_external_page_observation_token(
    uint64_t token)
{
  if (ownerless_external_page_observation_token != token)
    ownerless_external_page_observations_clear();
  ownerless_external_page_observation_token= token;
}

extern "C" void mylite_ownerless_innodb_clear_external_page_observations(void)
{
  ownerless_external_page_observations_clear();
}

static void ownerless_note_external_page_observation(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn,
    bool rollback_barrier)
{
  if (commit_lsn == 0 || ownerless_external_page_observation_token == 0)
    return;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return;

  if (!ownerless_external_page_observations_ensure())
    return;

retry:
  const size_t capacity= ownerless_external_page_observation_capacity;
  const size_t start_slot=
      ownerless_external_page_observation_slot(space_id, page_no, capacity);
  size_t reusable_slot= capacity;
  for (size_t probe= 0; probe < capacity; ++probe)
  {
    const size_t slot= (start_slot + probe) & (capacity - 1);
    ownerless_external_page_observation_entry &entry=
        ownerless_external_page_observations[slot];
    if (ownerless_external_page_observation_matches(
            entry, ownerless_external_page_observation_token, context,
            space_id, page_no))
    {
      if (commit_lsn >= entry.commit_lsn)
      {
        entry.commit_lsn= commit_lsn;
        entry.rollback_barrier= rollback_barrier;
      }
      return;
    }
    if (reusable_slot == capacity &&
        (entry.token == 0 || entry.context == nullptr ||
         entry.token != ownerless_external_page_observation_token ||
         entry.context != context))
    {
      reusable_slot= slot;
    }
    if (entry.token == 0 || entry.context == nullptr)
      break;
  }
  if (reusable_slot != capacity)
  {
    ownerless_external_page_observation_entry &entry=
        ownerless_external_page_observations[reusable_slot];
    entry.token= ownerless_external_page_observation_token;
    entry.context= context;
    entry.space_id= space_id;
    entry.page_no= page_no;
    entry.commit_lsn= commit_lsn;
    entry.rollback_barrier= rollback_barrier;
    return;
  }
  if (capacity < k_external_page_observation_max_entries &&
      ownerless_external_page_observations_resize(
          std::min(capacity * 2, k_external_page_observation_max_entries)))
      goto retry;
}

extern "C" void mylite_ownerless_innodb_note_external_page_observed(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn)
{
  ownerless_note_external_page_observation(
      space_id, page_no, commit_lsn, false);
}

extern "C" void
mylite_ownerless_innodb_note_external_page_rollback_barrier(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn)
{
  ownerless_note_external_page_observation(
      space_id, page_no, commit_lsn, true);
}

static uint64_t ownerless_external_page_observed_commit_lsn(
    uint32_t space_id, uint32_t page_no)
{
  if (ownerless_external_page_observation_token == 0)
    return 0;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr || ownerless_external_page_observations == nullptr ||
      ownerless_external_page_observation_capacity == 0)
    return 0;

  const size_t capacity= ownerless_external_page_observation_capacity;
  const size_t start_slot=
      ownerless_external_page_observation_slot(space_id, page_no, capacity);
  for (size_t probe= 0; probe < capacity; ++probe)
  {
    const size_t slot= (start_slot + probe) & (capacity - 1);
    const ownerless_external_page_observation_entry &entry=
        ownerless_external_page_observations[slot];
    if (ownerless_external_page_observation_matches(
            entry, ownerless_external_page_observation_token, context,
            space_id, page_no))
      return entry.commit_lsn;
    if (entry.token == 0 || entry.context == nullptr)
      return 0;
  }
  return 0;
}

extern "C" int mylite_ownerless_innodb_external_page_is_rollback_barrier(
    uint32_t space_id, uint32_t page_no)
{
  if (ownerless_external_page_observation_token == 0)
    return 0;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr || ownerless_external_page_observations == nullptr ||
      ownerless_external_page_observation_capacity == 0)
    return 0;

  const size_t capacity= ownerless_external_page_observation_capacity;
  const size_t start_slot=
      ownerless_external_page_observation_slot(space_id, page_no, capacity);
  for (size_t probe= 0; probe < capacity; ++probe)
  {
    const size_t slot= (start_slot + probe) & (capacity - 1);
    const ownerless_external_page_observation_entry &entry=
        ownerless_external_page_observations[slot];
    if (ownerless_external_page_observation_matches(
            entry, ownerless_external_page_observation_token, context,
            space_id, page_no))
      return entry.rollback_barrier ? 1 : 0;
    if (entry.token == 0 || entry.context == nullptr)
      return 0;
  }
  return 0;
}

extern "C" int mylite_ownerless_innodb_external_page_observed_at_or_after(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn)
{
  return commit_lsn != 0 &&
                 ownerless_external_page_observed_commit_lsn(
                     space_id, page_no) >= commit_lsn
             ? 1
             : 0;
}

extern "C" uint64_t mylite_ownerless_innodb_push_external_page_visibility(
    uint64_t latest_lsn)
{
  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  return previous_visible_lsn;
}

extern "C" void mylite_ownerless_innodb_restore_external_page_visibility(
    uint64_t previous_lsn)
{
  page_visible_lsn= previous_lsn;
}

extern "C" void mylite_ownerless_innodb_clear_external_page_visibility(void)
{
  startup_page_visible_lsn.store(0, std::memory_order_release);
  page_visible_lsn= 0;
  page_visible_lsn_is_current= false;
  page_visible_lsn_is_retained= false;
}

extern "C" void mylite_ownerless_innodb_close_current_read_view(void)
{
  trx_t *trx= current_trx();
  if (trx != nullptr)
    if (UNIV_UNLIKELY(trx->close_read_view() != DB_SUCCESS))
      return;
}

extern "C" void mylite_ownerless_innodb_begin_internal_lock_wait(void)
{
  mylite_ownerless_internal_lock_wait_depth++;
}

extern "C" void mylite_ownerless_innodb_end_internal_lock_wait(void)
{
  ut_ad(mylite_ownerless_internal_lock_wait_depth != 0);
  if (mylite_ownerless_internal_lock_wait_depth != 0)
    mylite_ownerless_internal_lock_wait_depth--;
}

extern "C" int mylite_ownerless_innodb_internal_lock_wait_active(void)
{
  return mylite_ownerless_internal_lock_wait_depth != 0;
}

extern "C" int mylite_ownerless_innodb_refresh_to_latest_external_lsn(void)
{
  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return result;

  mylite_ownerless_innodb_refresh_external_pages(latest_lsn);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" uint64_t mylite_ownerless_innodb_current_lsn(void)
{
  if (recv_recovery_is_on() || !srv_was_started)
    return 0;

  return log_get_lsn();
}

extern "C" uint64_t mylite_ownerless_innodb_checkpoint_lsn(void)
{
  if (recv_recovery_is_on() || !srv_was_started)
    return 0;

  return log_sys.last_checkpoint_lsn;
}

extern "C" uint64_t mylite_ownerless_innodb_shutdown_lsn(void)
{
  return srv_shutdown_lsn;
}

extern "C" int mylite_ownerless_innodb_make_checkpoint(void)
{
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const bool previous_checkpoint_suppression_bypass=
      checkpoint_suppression_bypass;
  checkpoint_suppression_bypass= true;
  log_make_checkpoint();
  checkpoint_suppression_bypass= previous_checkpoint_suppression_bypass;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_checkpoint_covers_lsn(uint64_t lsn)
{
  if (recv_recovery_is_on() || !srv_was_started || lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const uint64_t checkpoint_lsn= log_sys.last_checkpoint_lsn;
  const uint64_t checkpoint_record_size=
      log_sys.is_encrypted() ? SIZE_OF_FILE_CHECKPOINT + 8
                             : SIZE_OF_FILE_CHECKPOINT;
  if (checkpoint_lsn >= lsn || checkpoint_lsn + checkpoint_record_size >= lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
}

static my_bool ownerless_active_recovered_trx_callback(
    rw_trx_hash_element_t *element, ownerless_active_recovered_trx_scan *scan)
{
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    trx->mutex_lock();
    if (trx_state_eq(trx, TRX_STATE_ACTIVE) && trx->is_recovered &&
        !trx->mylite_ownerless_remote_recovered)
      scan->found= true;
    trx->mutex_unlock();
  }
  element->mutex.wr_unlock();
  return 0;
}

static bool ownerless_active_recovered_trx_exists()
{
  ownerless_active_recovered_trx_scan scan;
  trx_sys.rw_trx_hash.iterate_no_dups(
      ownerless_active_recovered_trx_callback, &scan);
  return scan.found;
}

extern "C" int mylite_ownerless_innodb_wait_recovered_rollback(
    unsigned int timeout_ms)
{
  if (recv_recovery_is_on() || !srv_was_started || srv_thread_pool == nullptr ||
      srv_read_only_mode)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const auto deadline=
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;)
  {
    if (trx_rollback_is_active &&
        !rollback_all_recovered_task.wait_until(deadline))
      return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;

    if (!ownerless_active_recovered_trx_exists())
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;

    const auto now= std::chrono::steady_clock::now();
    if (now >= deadline)
      return MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT;

    if (!trx_rollback_is_active)
    {
      trx_rollback_is_active= true;
      srv_thread_pool->submit_task(&rollback_all_recovered_task);
    }
    std::this_thread::sleep_until(
        std::min(deadline, now + std::chrono::milliseconds(1)));
  }
}

extern "C" int mylite_ownerless_innodb_has_recovered_active_transactions(
    int *out_has_recovered)
{
  if (out_has_recovered == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_has_recovered= 0;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (trx_rollback_is_active)
  {
    *out_has_recovered= 1;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  *out_has_recovered= ownerless_active_recovered_trx_exists() ? 1 : 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_rollback_history_exists(
    int *out_exists)
{
  if (out_exists == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_exists= 0;
  if (recv_recovery_is_on() || !srv_was_started)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  *out_exists= trx_sys.history_exists() ? 1 : 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_redo_is_active(void)
{
  return ownerless_lock_hooks_enabled() && redo_depth != 0;
}

extern "C" int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (redo_depth != 0)
  {
    redo_depth++;
    if (out_latest_lsn != nullptr)
      *out_latest_lsn= 0;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  mylite_ownerless_innodb_redo_enter_callback hook=
      redo_enter_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  const int result= normalize_required_hook_result(
      current_trx(), hook(out_latest_lsn, context));
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    redo_depth++;
    redo_latest_lsn= 0;
  }
  return result;
}

extern "C" int mylite_ownerless_innodb_redo_observe(uint64_t *out_latest_lsn)
{
  if (out_latest_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  *out_latest_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_redo_observe_callback hook=
      redo_observe_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  const int result= hook(out_latest_lsn, context);
  return normalize_required_hook_result(current_trx(), result);
}

extern "C" int
mylite_ownerless_innodb_redo_observe_visible(uint64_t *out_visible_lsn)
{
  if (out_visible_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  *out_visible_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_redo_observe_visible_callback hook=
      redo_observe_visible_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return normalize_required_hook_result(current_trx(),
                                        hook(out_visible_lsn, context));
}

extern "C" int
mylite_ownerless_innodb_redo_observe_written(uint64_t *out_written_lsn)
{
  if (out_written_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  *out_written_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_redo_observe_written_callback hook=
      redo_observe_written_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return normalize_required_hook_result(current_trx(),
                                        hook(out_written_lsn, context));
}

extern "C" int mylite_ownerless_innodb_redo_reserve(uint64_t current_lsn,
                                                    uint64_t length,
                                                    uint64_t *out_start_lsn,
                                                    uint64_t *out_end_lsn)
{
  if (length == 0 || out_start_lsn == nullptr || out_end_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  *out_start_lsn= 0;
  *out_end_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_redo_reserve_callback hook=
      redo_reserve_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return normalize_required_hook_result(
      current_trx(),
      hook(current_lsn, length, out_start_lsn, out_end_lsn, context));
}

extern "C" int mylite_ownerless_innodb_redo_written(uint64_t start_lsn,
                                                    uint64_t end_lsn,
                                                    uint64_t *out_written_lsn)
{
  if (start_lsn == 0 || end_lsn <= start_lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (out_written_lsn != nullptr)
    *out_written_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_redo_written_callback hook=
      redo_written_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return normalize_required_hook_result(
      current_trx(), hook(start_lsn, end_lsn, out_written_lsn, context));
}

extern "C" int mylite_ownerless_innodb_redo_leave(uint64_t latest_lsn)
{
  if (redo_depth == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  if (latest_lsn > redo_latest_lsn)
    redo_latest_lsn= latest_lsn;
  if (redo_depth > 1)
  {
    redo_depth--;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  latest_lsn= redo_latest_lsn;
  mylite_ownerless_innodb_redo_leave_callback hook=
      redo_leave_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  const int result= hook(latest_lsn, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
               ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
               : result;
  }
  redo_depth= 0;
  redo_latest_lsn= 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_redo_written_and_leave(
    uint64_t start_lsn, uint64_t end_lsn, uint64_t latest_lsn,
    uint64_t *out_written_lsn)
{
  if (start_lsn == 0 || end_lsn <= start_lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (out_written_lsn != nullptr)
    *out_written_lsn= 0;

  auto separate_written_then_leave= [start_lsn, end_lsn, latest_lsn,
                                     out_written_lsn]() -> int {
    const int result= mylite_ownerless_innodb_redo_written(start_lsn, end_lsn,
                                                           out_written_lsn);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
      return result;
    return mylite_ownerless_innodb_redo_leave(latest_lsn);
  };

  if (!ownerless_lock_hooks_enabled() || redo_depth != 1 ||
      mylite_ownerless_innodb_test_faults_enabled_fast())
    return separate_written_then_leave();

  mylite_ownerless_innodb_redo_written_leave_callback hook=
      redo_written_leave_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return separate_written_then_leave();

  if (latest_lsn > redo_latest_lsn)
    redo_latest_lsn= latest_lsn;
  const int result=
      hook(start_lsn, end_lsn, redo_latest_lsn, out_written_lsn, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
               ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
               : result;
  }

  redo_depth--;
  if (redo_depth == 0)
    redo_latest_lsn= 0;
  return result;
}

extern "C" int mylite_ownerless_innodb_redo_defer_written_and_leave(
    uint64_t start_lsn, uint64_t end_lsn, uint64_t latest_lsn,
    uint64_t *out_written_lsn)
{
  if (start_lsn == 0 || end_lsn <= start_lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (out_written_lsn != nullptr)
    *out_written_lsn= 0;

  if (redo_depth != 1 ||
      !mylite_ownerless_statement_deferred_redo_admission_ready)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  if (deferred_redo_batch_count == k_deferred_redo_batch_capacity)
  {
    const int flush_result= flush_deferred_redo_batch();
    if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      return flush_result;
  }

  if (deferred_redo_batch_count >= k_deferred_redo_batch_capacity)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  deferred_redo_batch[deferred_redo_batch_count++]= {start_lsn, end_lsn,
                                                     latest_lsn};
  if (latest_lsn > deferred_redo_batch_latest_lsn)
    deferred_redo_batch_latest_lsn= latest_lsn;
  if (latest_lsn > redo_latest_lsn)
    redo_latest_lsn= latest_lsn;

  redo_depth--;
  if (redo_depth == 0)
    redo_latest_lsn= 0;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_redo_flush_deferred(void)
{
  return flush_deferred_redo_batch();
}

extern "C" int mylite_ownerless_innodb_publish_page_version(
    uint32_t space_id, uint32_t page_no, uint64_t page_lsn,
    uint64_t visible_lsn, const void *page, uint32_t page_size)
{
  return mylite_ownerless_innodb_publish_page_version_with_flags(
      space_id, page_no, page_lsn, visible_lsn, page, page_size, 0U);
}

extern "C" int mylite_ownerless_innodb_publish_page_version_with_flags(
    uint32_t space_id, uint32_t page_no, uint64_t page_lsn,
    uint64_t visible_lsn, const void *page, uint32_t page_size,
    uint32_t publish_flags)
{
  const int result= mylite_ownerless_innodb_try_publish_page_version_with_flags(
      space_id, page_no, page_lsn, visible_lsn, page, page_size,
      publish_flags);
  return normalize_required_hook_result(current_trx(), result);
}

extern "C" int
mylite_ownerless_innodb_try_publish_page_version_with_flags(
    uint32_t space_id, uint32_t page_no, uint64_t page_lsn,
    uint64_t visible_lsn, const void *page, uint32_t page_size,
    uint32_t publish_flags)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_page_publish_callback hook=
      page_publish_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(space_id, page_no, page_lsn, visible_lsn, page, page_size,
              publish_flags, context);
}

extern "C" int mylite_ownerless_innodb_publish_history_proof_pair(
    uint32_t space_id, uint32_t rseg_page_no, uint64_t rseg_page_lsn,
    const void *rseg_page, uint32_t rseg_page_size, uint32_t undo_page_no,
    uint64_t undo_page_lsn, const void *undo_page, uint32_t undo_page_size,
    uint64_t visible_lsn)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_history_proof_publish_pair_callback hook=
      history_proof_publish_pair_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(space_id, rseg_page_no, rseg_page_lsn, rseg_page, rseg_page_size,
              undo_page_no, undo_page_lsn, undo_page, undo_page_size,
              visible_lsn, context);
}

extern "C" void mylite_ownerless_innodb_begin_page_publish_batch(void)
{
  if (!ownerless_lock_hooks_enabled() ||
      !ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return;

  mylite_ownerless_innodb_page_publish_batch_callback hook=
      page_publish_batch_begin_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;
  hook(context);
}

extern "C" void mylite_ownerless_innodb_end_page_publish_batch(void)
{
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return;
  mylite_ownerless_innodb_page_publish_batch_callback hook=
      page_publish_batch_end_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;
  hook(context);
}

static int mylite_ownerless_innodb_read_page_version_at_lsn(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags,
    uint32_t read_options)
{
  if (page == nullptr || page_capacity == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (out_page_size != nullptr)
    *out_page_size= 0;
  if (out_page_lsn != nullptr)
    *out_page_lsn= 0;
  if (out_commit_lsn != nullptr)
    *out_commit_lsn= 0;
  if (out_record_flags != nullptr)
    *out_record_flags= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (ownerless_coordination_error.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (max_commit_lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if ((read_options & ~MYLITE_OWNERLESS_INNODB_PAGE_READ_HISTORY_RSEG_DELTA) != 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_page_read_callback hook=
      page_read_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  uint32_t page_size= 0;
  uint64_t page_lsn= 0;
  uint64_t commit_lsn= 0;
  uint32_t record_flags= 0;
  const int result= hook(space_id, page_no, max_commit_lsn, page, page_capacity,
                         &page_size, &page_lsn, &commit_lsn, &record_flags,
                         read_options, context);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    if (out_page_size != nullptr)
      *out_page_size= page_size;
    if (out_page_lsn != nullptr)
      *out_page_lsn= page_lsn;
    if (out_commit_lsn != nullptr)
      *out_commit_lsn= commit_lsn;
    if (out_record_flags != nullptr)
      *out_record_flags= record_flags;
  }
  else if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE &&
           (record_flags &
            MYLITE_OWNERLESS_INNODB_PAGE_VERSION_ROLLBACK_BARRIER) != 0)
  {
    /*
    A terminal rollback barrier deliberately has no page payload. Preserve
    its page-local ordering metadata so the refresh path can replace a newer
    uncommitted buffer image with the restored native page.
    */
    if (out_page_lsn != nullptr)
      *out_page_lsn= page_lsn;
    if (out_commit_lsn != nullptr)
      *out_commit_lsn= commit_lsn;
    if (out_record_flags != nullptr)
      *out_record_flags= record_flags;
  }
  else if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
  }
  return result;
}

extern "C" int mylite_ownerless_innodb_read_page_version_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags)
{
  uint64_t latest_lsn= 0;
  const int observe_result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (observe_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    advance_external_lsn(latest_lsn);
  else if (observe_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return observe_result;

  uint64_t max_commit_lsn= page_visible_lsn;
  max_commit_lsn= std::max(
      max_commit_lsn, startup_page_visible_lsn.load(std::memory_order_acquire));
  return mylite_ownerless_innodb_read_page_version_at_lsn(
      space_id, page_no, max_commit_lsn, page, page_capacity, out_page_size,
      out_page_lsn, out_commit_lsn, out_record_flags, 0);
}

extern "C" int
mylite_ownerless_innodb_read_startup_native_support_page_version_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags)
{
  uint64_t visible_lsn= page_visible_lsn;
  visible_lsn= std::max(
      visible_lsn, startup_page_visible_lsn.load(std::memory_order_acquire));
  const uint64_t startup_native_support_lsn=
      startup_native_support_page_visible_lsn.load(std::memory_order_acquire);
  visible_lsn= std::max(
      visible_lsn, startup_native_support_lsn);
  if (max_commit_lsn > visible_lsn)
    max_commit_lsn= visible_lsn;
  return mylite_ownerless_innodb_read_page_version_at_lsn(
      space_id, page_no, max_commit_lsn, page, page_capacity, out_page_size,
      out_page_lsn, out_commit_lsn, out_record_flags, 0);
}

extern "C" int
mylite_ownerless_innodb_read_startup_native_support_page_version_with_history_rseg_delta(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags)
{
  uint64_t visible_lsn= page_visible_lsn;
  visible_lsn= std::max(
      visible_lsn, startup_page_visible_lsn.load(std::memory_order_acquire));
  const uint64_t startup_native_support_lsn=
      startup_native_support_page_visible_lsn.load(std::memory_order_acquire);
  visible_lsn= std::max(
      visible_lsn, startup_native_support_lsn);
  if (max_commit_lsn > visible_lsn)
    max_commit_lsn= visible_lsn;
  return mylite_ownerless_innodb_read_page_version_at_lsn(
      space_id, page_no, max_commit_lsn, page, page_capacity, out_page_size,
      out_page_lsn, out_commit_lsn, out_record_flags,
      MYLITE_OWNERLESS_INNODB_PAGE_READ_HISTORY_RSEG_DELTA);
}

extern "C" int mylite_ownerless_innodb_read_page_version_before_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t max_commit_lsn,
    void *page,
    uint32_t page_capacity,
    uint32_t *out_page_size,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags)
{
  uint64_t visible_lsn= page_visible_lsn;
  visible_lsn= std::max(
      visible_lsn, startup_page_visible_lsn.load(std::memory_order_acquire));
  if (max_commit_lsn > visible_lsn)
    max_commit_lsn= visible_lsn;
  return mylite_ownerless_innodb_read_page_version_at_lsn(
      space_id, page_no, max_commit_lsn, page, page_capacity, out_page_size,
      out_page_lsn, out_commit_lsn, out_record_flags, 0);
}

extern "C" int mylite_ownerless_innodb_read_page_version(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity)
{
  return mylite_ownerless_innodb_read_page_version_with_metadata(
      space_id, page_no, page, page_capacity, nullptr, nullptr, nullptr,
      nullptr);
}

extern "C" int mylite_ownerless_innodb_retained_allocation_page_in_use(
    uint32_t space_id,
    uint32_t page_no)
{
  if (!ownerless_lock_hooks_enabled())
    return 0;

  bool check_native_disk_page= false;
  if (mylite_ownerless_statement_dictionary_ddl)
  {
    /* DDL may retain historical user and SYS_* page images while rebuilding
    storage. The globally serialized, refreshed allocation bitmap is current;
    old page bodies and WAL versions can legitimately describe prior uses. */
    return 0;
  }
  else if (!page_visible_lsn_is_retained)
  {
    return 0;
  }
  else
  {
    uint64_t max_commit_lsn= page_visible_lsn;
    max_commit_lsn= std::max(
        max_commit_lsn,
        startup_page_visible_lsn.load(std::memory_order_acquire));
    if (max_commit_lsn == 0)
      return 0;

    unsigned char *page=
        static_cast<unsigned char *>(std::malloc(UNIV_PAGE_SIZE_MAX));
    if (page == nullptr)
      return 1;
    uint32_t page_size= 0;
    uint64_t page_lsn= 0;
    uint64_t commit_lsn= 0;
    uint32_t record_flags= 0;
    const int result= mylite_ownerless_innodb_read_page_version_at_lsn(
        space_id, page_no, max_commit_lsn, page, UNIV_PAGE_SIZE_MAX,
        &page_size, &page_lsn, &commit_lsn, &record_flags, 0);
    std::free(page);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK || page_size == 0 ||
        commit_lsn == 0)
    {
      check_native_disk_page= true;
    }
    else if ((record_flags &
              MYLITE_OWNERLESS_INNODB_PAGE_VERSION_NATIVE_SUPPORT_STATE) == 0)
    {
      return 1;
    }
  }

  if (!check_native_disk_page)
    return 0;

  if (space_id >= SRV_TMP_SPACE_ID)
    return 0;

  page_t *disk_page= nullptr;
  int in_use= 0;

  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(space_id);
  if (space == nullptr || space->is_temporary())
    goto disk_exit;

  {
    const uint32_t disk_page_size=
        static_cast<uint32_t>(space->physical_size());
    disk_page= static_cast<byte*>(aligned_malloc(disk_page_size,
                                                 disk_page_size));
    if (disk_page == nullptr)
    {
      in_use= 1;
      goto disk_exit;
    }

    uint32_t node_page_no= page_no;
    fil_node_t *node= find_file_node_for_page(*space, &node_page_no);
    if (node == nullptr || !node->is_open() || node->deferred)
      goto disk_exit;

    const os_offset_t offset=
        os_offset_t{node_page_no} * os_offset_t{disk_page_size};
    if (os_file_read(IORequestRead, node->handle, disk_page, offset,
                     disk_page_size, nullptr) != DB_SUCCESS)
      goto disk_exit;

    const uint32_t read_space_id=
        mach_read_from_4(disk_page + FIL_PAGE_SPACE_ID);
    const uint32_t read_page_no=
        mach_read_from_4(disk_page + FIL_PAGE_OFFSET);
    if (read_space_id != space_id || read_page_no != page_no)
      goto disk_exit;

    if (buf_page_is_corrupted(true, disk_page, space->flags) != NOT_CORRUPTED)
      goto disk_exit;

    const uint16_t page_type= mach_read_from_2(disk_page + FIL_PAGE_TYPE);
    in_use= page_type != FIL_PAGE_TYPE_ALLOCATED ? 1 : 0;
  }

disk_exit:
  mysql_mutex_unlock(&fil_system.mutex);
  if (disk_page != nullptr)
    aligned_free(disk_page);
  return in_use;
}

extern "C" int mylite_ownerless_innodb_disk_page_lsn(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t *out_page_lsn)
{
  if (out_page_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_page_lsn= 0;
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  page_t *page= nullptr;
  int result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(space_id);
  if (space == nullptr || space->is_temporary())
    goto exit;

  {
    const uint32_t page_size= static_cast<uint32_t>(space->physical_size());
    page= static_cast<byte*>(aligned_malloc(page_size, page_size));
    if (page == nullptr)
    {
      result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      goto exit;
    }

    uint32_t node_page_no= page_no;
    fil_node_t *node= find_file_node_for_page(*space, &node_page_no);
    if (node == nullptr || !node->is_open() || node->deferred)
      goto exit;

    const os_offset_t offset=
        os_offset_t{node_page_no} * os_offset_t{page_size};
    if (os_file_read(IORequestRead, node->handle, page, offset,
                     page_size, nullptr) != DB_SUCCESS)
      goto exit;

    const uint32_t read_space_id= mach_read_from_4(page + FIL_PAGE_SPACE_ID);
    const uint32_t read_page_no= mach_read_from_4(page + FIL_PAGE_OFFSET);
    if (read_space_id != space_id || read_page_no != page_no)
      goto exit;

    if (buf_page_is_corrupted(true, page, space->flags) != NOT_CORRUPTED)
      goto exit;

    *out_page_lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

exit:
  mysql_mutex_unlock(&fil_system.mutex);
  if (page != nullptr)
    aligned_free(page);
  return result;
}

extern "C" int mylite_ownerless_innodb_disk_page_matches(
    uint32_t space_id,
    uint32_t page_no,
    const void *expected_page,
    uint32_t expected_page_size,
    uint64_t *out_page_lsn,
    int *out_matches)
{
  if (expected_page == nullptr || expected_page_size == 0 ||
      out_page_lsn == nullptr || out_matches == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_page_lsn= 0;
  *out_matches= 0;
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  page_t *page= nullptr;
  int result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_get_by_id(space_id);
  if (space == nullptr || space->is_temporary())
    goto exit;

  {
    const uint32_t page_size= static_cast<uint32_t>(space->physical_size());
    if (page_size != expected_page_size)
      goto exit;

    page= static_cast<byte*>(aligned_malloc(page_size, page_size));
    if (page == nullptr)
    {
      result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      goto exit;
    }

    uint32_t node_page_no= page_no;
    fil_node_t *node= find_file_node_for_page(*space, &node_page_no);
    if (node == nullptr || !node->is_open() || node->deferred)
      goto exit;

    const os_offset_t offset=
        os_offset_t{node_page_no} * os_offset_t{page_size};
    if (os_file_read(IORequestRead, node->handle, page, offset,
                     page_size, nullptr) != DB_SUCCESS)
      goto exit;

    const uint32_t read_space_id= mach_read_from_4(page + FIL_PAGE_SPACE_ID);
    const uint32_t read_page_no= mach_read_from_4(page + FIL_PAGE_OFFSET);
    if (read_space_id != space_id || read_page_no != page_no)
      goto exit;

    if (buf_page_is_corrupted(true, page, space->flags) != NOT_CORRUPTED)
      goto exit;

    *out_page_lsn= mach_read_from_8(page + FIL_PAGE_LSN);
    *out_matches= memcmp(page, expected_page, page_size) == 0 ? 1 : 0;
    result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

exit:
  mysql_mutex_unlock(&fil_system.mutex);
  if (page != nullptr)
    aligned_free(page);
  return result;
}

extern "C" int mylite_ownerless_innodb_autoinc_read(
    uint64_t table_id,
    uint64_t seed_next_value,
    uint64_t *out_next_value)
{
  if (out_next_value == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  *out_next_value= seed_next_value;
  if (table_id == 0 || seed_next_value == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_autoinc_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_autoinc_read_callback hook=
      autoinc_read_callback.load(std::memory_order_acquire);
  void *context= autoinc_callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  return hook(table_id, seed_next_value, out_next_value, context);
}

extern "C" int mylite_ownerless_innodb_autoinc_publish(
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value)
{
  if (table_id == 0 || next_value == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_autoinc_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_autoinc_publish_callback hook=
      autoinc_publish_callback.load(std::memory_order_acquire);
  void *context= autoinc_callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  return hook(table_id, next_value, persistent_value, context);
}

extern "C" int mylite_ownerless_innodb_autoinc_replay_persistent(
    uint64_t table_id,
    uint64_t next_value,
    uint64_t persistent_value)
{
  if (table_id == 0 || next_value <= 1)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_autoinc_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  trx_t *trx= current_trx();

  dict_table_t *table= dict_table_open_on_id(
      table_id_t{table_id}, false, DICT_TABLE_OP_NORMAL);
  if (table == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  int result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  table->autoinc_mutex.wr_lock();
  dict_table_autoinc_update_if_greater(table, next_value);
  if (table->persistent_autoinc && !table->is_temporary())
  {
    const uint64_t root_value=
        persistent_value != 0 ? persistent_value : next_value - 1U;
    dict_index_t *index= dict_table_get_first_index(table);
    if (index != nullptr)
    {
      const int previous_bypass=
          mylite_ownerless_innodb_set_page_write_refresh_bypass(1);
      if (!replay_persistent_autoinc_root_value(trx, index, root_value))
        result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
      mylite_ownerless_innodb_set_page_write_refresh_bypass(previous_bypass);
    }
    else
      result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  }
  table->autoinc_mutex.wr_unlock();

  dict_table_close(table, nullptr, nullptr);
  return result;
}

namespace {

bool ownerless_lock_hooks_enabled()
{
  return mylite_ownerless_innodb_lock_hooks_enabled.load(std::memory_order_relaxed);
}

bool ownerless_autoinc_hooks_enabled()
{
  return mylite_ownerless_innodb_autoinc_hooks_enabled.load(std::memory_order_relaxed);
}

bool replay_persistent_autoinc_root_value(
    trx_t *trx,
    dict_index_t *index,
    uint64_t autoinc)
{
  ut_ad(index->is_primary());
  ut_ad(index->table->persistent_autoinc);
  ut_ad(!index->table->is_temporary());

  mtr_t mtr{trx};
  mtr.start();
  fil_space_t *space= index->table->space;
  const uint32_t space_id= space->id;
  const uint32_t page_no= index->page;
  bool wrote= false;
  if (buf_block_t *root= buf_page_get(page_id_t(space_id, page_no),
                                      space->zip_size(), RW_SX_LATCH, &mtr))
  {
#ifdef BTR_CUR_HASH_ADAPT
    ut_d(if (dict_index_t *ri= root->index)) ut_ad(ri == index);
#endif /* BTR_CUR_HASH_ADAPT */
    buf_page_make_young_if_needed(&root->page);
    mtr.set_named_space(space);
    byte *field= my_assume_aligned<8>(
        PAGE_HEADER + PAGE_ROOT_AUTO_INC + root->page.frame);
    const uint64_t old= mach_read_from_8(field);
    if (old == autoinc)
    {
      mtr.write<8>(*root, field, autoinc);
      if (UNIV_LIKELY_NULL(root->page.zip.data))
        memcpy_aligned<8>(
            PAGE_HEADER + PAGE_ROOT_AUTO_INC + root->page.zip.data, field, 8);
    }
    else
      page_set_autoinc(root, autoinc, &mtr, true);
    wrote= true;
  }

  mtr.commit();
  if (wrote && ownerless_lock_hooks_enabled())
  {
    const lsn_t visible_lsn= log_get_lsn();
    const lsn_t observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
        space_id, page_no, visible_lsn, false, nullptr, false);
    if (observed_lsn > visible_lsn)
      buf_flush_publish_ownerless_page_to_lsn(
          space_id, page_no, observed_lsn, false, nullptr, false);
  }
  return wrote;
}

int publish_pages_visible_lsn(uint64_t visible_lsn)
{
  if (visible_lsn == 0 || !ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (!ownerless_write_coordination_enabled.load(std::memory_order_acquire))
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  const int flush_result= flush_deferred_redo_batch();
  if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    note_ownerless_coordination_fault(current_trx());
    return flush_result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
        ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
        : flush_result;
  }
  mylite_ownerless_innodb_pages_visible_callback hook=
      pages_visible_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    note_ownerless_coordination_fault(current_trx());
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  const int result= hook(visible_lsn, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
      ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
      : result;
}

int flush_deferred_redo_batch()
{
  if (deferred_redo_batch_count == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  mylite_ownerless_innodb_redo_written_leave_callback hook=
      redo_written_leave_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const size_t count= deferred_redo_batch_count;
  const uint64_t latest_lsn= deferred_redo_batch_latest_lsn;
  deferred_redo_batch_count= 0;
  deferred_redo_batch_latest_lsn= 0;

  if (latest_lsn != 0)
    log_write_up_to(static_cast<lsn_t>(latest_lsn), false);

  mylite_ownerless_innodb_redo_written_leave_batch_callback batch_hook=
      redo_written_leave_batch_callback.load(std::memory_order_acquire);
  if (batch_hook != nullptr && count > 1)
  {
    mylite_ownerless_innodb_redo_range ranges[k_deferred_redo_batch_capacity];
    for (size_t i= 0; i < count; ++i)
      ranges[i]= {deferred_redo_batch[i].start_lsn,
                  deferred_redo_batch[i].end_lsn};

    uint64_t written_lsn= 0;
    size_t completed_count= 0;
    const int result= batch_hook(ranges, count, latest_lsn, &written_lsn,
                                 &completed_count, context);
    if (completed_count > count)
    {
      retain_deferred_redo_batch_tail(0, count);
      note_ownerless_coordination_fault(current_trx());
      return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        completed_count == count)
    {
      for (size_t i= 0; i < count; ++i)
        deferred_redo_batch[i]= {};
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }

    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK || completed_count != count)
    {
      retain_deferred_redo_batch_tail(completed_count, count);
      note_ownerless_coordination_fault(current_trx());
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
          ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
          : result;
    }
  }

  for (size_t i= 0; i < count; ++i)
  {
    uint64_t written_lsn= 0;
    const uint64_t range_latest_lsn= (i + 1 == count) ? latest_lsn : 0;
    const int result= hook(deferred_redo_batch[i].start_lsn,
                           deferred_redo_batch[i].end_lsn,
                           range_latest_lsn, &written_lsn, context);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      retain_deferred_redo_batch_tail(i, count);
      note_ownerless_coordination_fault(current_trx());
      return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
          ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
          : result;
    }
    deferred_redo_batch[i]= {};
  }
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

void retain_deferred_redo_batch_tail(size_t first, size_t count)
{
  size_t retained_count= 0;
  deferred_redo_batch_latest_lsn= 0;
  for (size_t tail= first; tail < count; ++tail)
  {
    const deferred_redo_range retained= deferred_redo_batch[tail];
    deferred_redo_batch[retained_count++]= retained;
    if (retained.latest_lsn > deferred_redo_batch_latest_lsn)
      deferred_redo_batch_latest_lsn= retained.latest_lsn;
  }
  for (size_t clear= retained_count; clear < count; ++clear)
    deferred_redo_batch[clear]= {};
  deferred_redo_batch_count= retained_count;
}

void note_ownerless_coordination_fault(trx_t *trx)
{
  ownerless_coordination_error.store(true, std::memory_order_release);
  if (trx != nullptr)
  {
    trx->mylite_ownerless_coordination_fault= true;
    trx->error_state= DB_ERROR;
  }
}

int normalize_required_hook_result(trx_t *trx, int result)
{
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    if (!mylite_ownerless_innodb_lock_hooks_ever_enabled_fast())
      return result;
    note_ownerless_coordination_fault(trx);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_ERROR)
  {
    note_ownerless_coordination_fault(trx);
  }
  return result;
}

int missing_required_hook_result(trx_t *trx)
{
  return normalize_required_hook_result(
      trx, MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE);
}

void handle_hook_result(const char *operation, int result, trx_t *trx)
{
  (void) operation;
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
      (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE &&
       !mylite_ownerless_innodb_lock_hooks_ever_enabled_fast()))
    return;
  note_ownerless_coordination_fault(trx);
}

bool lock_publishable(const ib_lock_t *lock)
{
  return lock != nullptr &&
         !lock->is_waiting() &&
         lock->trx != nullptr;
}

bool table_lock_publishable(const ib_lock_t *lock)
{
  return lock_publishable(lock) &&
         lock->is_table() &&
         lock->un_member.tab_lock.table != nullptr &&
         lock->un_member.tab_lock.table->id != 0;
}

bool record_lock_publishable(const ib_lock_t *lock)
{
  return lock_publishable(lock) &&
         !lock->is_table() &&
         lock->index != nullptr &&
         lock->index->id != 0 &&
         !(lock->type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE));
}

bool wait_lock_publishable(const ib_lock_t *lock)
{
  return lock != nullptr &&
         lock->is_waiting() &&
         lock->trx != nullptr;
}

bool blocker_lock_publishable(const ib_lock_t *lock)
{
  return lock != nullptr &&
         lock->trx != nullptr;
}

void refresh_external_space_headers()
{
  std::vector<uint32_t> space_ids;
  mysql_mutex_lock(&fil_system.mutex);
  for (fil_space_t &space : fil_system.space_list)
  {
    static_cast<void>(refresh_external_space_header(space));
    if (space.id < SRV_TMP_SPACE_ID && !space.is_temporary())
      space_ids.push_back(space.id);
  }
  mysql_mutex_unlock(&fil_system.mutex);

  for (uint32_t space_id : space_ids)
    refresh_external_space_allocation_pages(space_id);
}

void refresh_external_space_header(uint32_t space_id)
{
  mysql_mutex_lock(&fil_system.mutex);
  if (fil_space_t *space= fil_space_get_by_id(space_id))
    static_cast<void>(refresh_external_space_header(*space));
  mysql_mutex_unlock(&fil_system.mutex);
}

void refresh_external_space_allocation_pages(uint32_t space_id)
{
  refresh_replaceable_buffer_pool_pages();
  refresh_buffer_pool_page(space_id, 0, true);
  refresh_buffer_pool_page(space_id, FSP_FIRST_INODE_PAGE_NO, true);
}

void refresh_external_space_allocation_pages_native_current(uint32_t space_id)
{
  refresh_replaceable_buffer_pool_pages();
  refresh_buffer_pool_page(space_id, 0, true, true, true, true, false, false,
                           true, true, true);
  refresh_buffer_pool_page(space_id, FSP_FIRST_INODE_PAGE_NO, true, true, true,
                           true, false, false, true, true, true);
}

bool refresh_external_space_header(fil_space_t &space, bool native_current)
{
  if (space.id >= SRV_TMP_SPACE_ID || space.is_temporary())
    return false;

  fil_node_t *node= UT_LIST_GET_FIRST(space.chain);
  if (node == nullptr || !node->is_open() || node->deferred)
    return false;

  page_t *page= static_cast<byte*>(
      aligned_malloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX));
  if (page == nullptr)
    return false;

  bool refreshed= false;
  ulint read_bytes= 0;
  if (!native_current)
  {
    const int page_version_result= mylite_ownerless_innodb_read_page_version(
        space.id, 0, page, UNIV_PAGE_SIZE_MAX);
    if (page_version_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      refreshed= true;
      read_bytes= UNIV_PAGE_SIZE_MAX;
    }
    else if (page_version_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      goto exit;
  }

  if (!refreshed)
  {
    if (os_file_read(IORequestReadPartial, node->handle, page, 0,
                     UNIV_PAGE_SIZE_MAX, &read_bytes)
        != DB_SUCCESS)
      goto exit;
  }

  {
    const uint32_t space_id= mach_read_from_4(page + FIL_PAGE_SPACE_ID);
    const uint32_t page_no= mach_read_from_4(page + FIL_PAGE_OFFSET);
    const uint32_t flags= fsp_header_get_flags(page);
    const uint32_t size= fsp_header_get_field(page, FSP_SIZE);
    const unsigned refreshed_page_size= fil_space_t::physical_size(flags);

    if (page_no != 0 || space_id != space.id || size < 4 ||
        !fil_space_t::is_valid_flags(flags, space.id) ||
        fil_space_t::logical_size(flags) != srv_page_size ||
        refreshed_page_size == 0 ||
        refreshed_page_size > UNIV_PAGE_SIZE_MAX ||
        read_bytes < refreshed_page_size ||
        buf_page_is_corrupted(true, page, flags) != NOT_CORRUPTED)
      goto exit;

    space.flags= (space.flags & FSP_FLAGS_MEM_MASK) | flags;
    space.size_in_header= std::max(space.size_in_header, size);
    space.free_limit= std::max(
        space.free_limit,
        fsp_header_get_field(page, FSP_FREE_LIMIT));
    space.free_len= flst_get_len(FSP_HEADER_OFFSET + FSP_FREE + page);

    const os_offset_t size_bytes= os_file_get_size(node->handle);
    if (size_bytes != os_offset_t(-1))
    {
      os_offset_t rounded_size= size_bytes;
      const ulint mask= refreshed_page_size * FSP_EXTENT_SIZE - 1;
      if (rounded_size > mask)
        rounded_size&= ~os_offset_t(mask);
      const uint32_t file_pages= uint32_t(rounded_size / refreshed_page_size);
      node->size= std::max(node->size, file_pages);
      space.size= std::max(space.size, node->size);
    }

    refreshed= true;
  }

exit:
  aligned_free(page);
  return refreshed;
}

bool table_can_be_evicted_from_dictionary(dict_table_t *table)
{
  ut_ad(dict_sys.locked());

  if (!table->can_be_evicted || table->get_ref_count() != 0 ||
      lock_table_has_locks(table) || table->fts != nullptr)
    return false;

#ifdef BTR_CUR_HASH_ADAPT
  for (const dict_index_t *index= dict_table_get_first_index(table);
       index; index= dict_table_get_next_index(index))
    if (index->any_ahi_pages())
      return false;
#endif

  return true;
}

bool foreign_table_can_be_reloaded_from_dictionary(dict_table_t *table)
{
  ut_ad(dict_sys.locked());

  /* InnoDB keeps some user tables on table_non_LRU. Ownerless peer DDL
  must reload any unused non-system table there, not only FK-bearing tables. */
  if (table->can_be_evicted || table->is_system_db ||
      table->get_ref_count() != 0 || lock_table_has_locks(table) ||
      table->fts != nullptr)
    return false;

#ifdef BTR_CUR_HASH_ADAPT
  for (const dict_index_t *index= dict_table_get_first_index(table);
       index; index= dict_table_get_next_index(index))
    if (index->any_ahi_pages())
      return false;
#endif

  return true;
}

bool compressed_frame_page_type_stored_uncompressed(uint16_t page_type)
{
  switch (page_type) {
  case FIL_PAGE_TYPE_ALLOCATED:
  case FIL_PAGE_INODE:
  case FIL_PAGE_IBUF_BITMAP:
  case FIL_PAGE_TYPE_FSP_HDR:
  case FIL_PAGE_TYPE_XDES:
    return true;
  default:
    return false;
  }
}

bool allocation_metadata_page_type(uint16_t page_type)
{
  switch (page_type) {
  case FIL_PAGE_INODE:
  case FIL_PAGE_IBUF_BITMAP:
  case FIL_PAGE_IBUF_FREE_LIST:
  case FIL_PAGE_TYPE_FSP_HDR:
  case FIL_PAGE_TYPE_XDES:
    return true;
  default:
    return false;
  }
}

bool refresh_page_local_source(const buf_block_t &block, byte **out_page,
                               uint32_t *out_page_size,
                               bool *out_mirror_to_zip,
                               bool *out_decompress_frame)
{
  if (out_page == nullptr || out_page_size == nullptr ||
      out_mirror_to_zip == nullptr || out_decompress_frame == nullptr)
    return false;

  *out_page= nullptr;
  *out_page_size= 0;
  *out_mirror_to_zip= false;
  *out_decompress_frame= false;

  const buf_page_t &bpage= block.page;
  const ulint zip_size= bpage.zip_size();
  if (zip_size != 0)
  {
    *out_page_size= static_cast<uint32_t>(zip_size);
    if (bpage.frame != nullptr &&
        compressed_frame_page_type_stored_uncompressed(
            fil_page_get_type(bpage.frame)))
    {
      *out_page= bpage.frame;
      *out_mirror_to_zip= bpage.zip.data != nullptr;
      return true;
    }
    if (bpage.zip.data == nullptr)
      return false;
    *out_page= bpage.zip.data;
    *out_decompress_frame= bpage.frame != nullptr;
    return true;
  }

  if (bpage.frame == nullptr)
    return false;
  *out_page= bpage.frame;
  *out_page_size= static_cast<uint32_t>(bpage.physical_size());
  return true;
}

bool copy_refreshed_page_to_block(const buf_block_t &block, byte *local_page,
                                  const byte *external_page,
                                  uint32_t page_size, bool mirror_to_zip,
                                  bool decompress_frame)
{
  if (local_page == nullptr || external_page == nullptr || page_size == 0)
    return false;

  memcpy(local_page, external_page, page_size);
  if (mirror_to_zip && block.page.zip.data != nullptr)
    memcpy(block.page.zip.data, external_page, page_size);
  if (decompress_frame)
  {
    buf_block_t *mutable_block= const_cast<buf_block_t*>(&block);
    if (!buf_zip_decompress(mutable_block, false))
      return false;
  }
  buf_block_modify_clock_inc(const_cast<buf_block_t*>(&block));
  return true;
}

void mark_retained_native_write_scrub_page_dirty(const buf_block_t &block)
{
  buf_block_t *mutable_block= const_cast<buf_block_t*>(&block);
  buf_page_t &page= mutable_block->page;
  if (page.id().space() >= SRV_TMP_SPACE_ID || !page.in_file() ||
      page.oldest_modification_acquire() > 1)
    return;

  mylite_ownerless_mark_retained_native_write_page_dirty(mutable_block);
}

int refresh_page_for_write(const buf_block_t &block,
                           bool use_current_visibility,
                           bool force_page_version,
                           bool allow_boundary_newer,
                           bool allow_visible_boundary,
                           bool preserve_retained_user_page,
                           bool skip_page_version,
                           bool preserve_local_transaction_page,
                           bool allow_native_disk_regression,
                           bool allow_dirty_committed_page_refresh,
                           bool allow_current_logical_page_version)
{
  ownerless_page_write_refresh_count(
      OWNERLESS_PAGE_WRITE_REFRESH_STAT_CALLS);
  if (force_page_version)
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_FORCE_CALLS);
  if (use_current_visibility)
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_CURRENT_VISIBILITY_CALLS);

  const buf_page_t &bpage= block.page;
  const page_id_t id{bpage.id()};
  if (id.space() >= SRV_TMP_SPACE_ID || !bpage.in_file() ||
      (bpage.zip.data == nullptr && bpage.frame == nullptr))
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE);
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  byte *local_page= nullptr;
  uint32_t page_size= 0;
  bool mirror_to_zip= false;
  bool decompress_frame= false;
  if (!refresh_page_local_source(block, &local_page, &page_size,
                                 &mirror_to_zip, &decompress_frame))
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE);
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }
  const lsn_t local_lsn= mach_read_from_8(local_page + FIL_PAGE_LSN);
  const uint16_t local_page_type= fil_page_get_type(local_page);
  const bool local_page_dirty= bpage.oldest_modification_acquire() != 0;
  const uint64_t packed_page= page_write_pack(id.space(), id.page_no());
  trx_t *trx= current_trx();
  const bool local_transaction_page=
      trx != nullptr &&
      (trx->mylite_ownerless_dirty_page_contains(packed_page) ||
       transaction_has_page_write_image(trx, packed_page) ||
       trx->mylite_ownerless_native_support_page_write_contains(packed_page) ||
       active_transaction_rollback_page(trx, bpage, local_page_type));
  if (preserve_local_transaction_page && trx != nullptr &&
      local_transaction_page)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  const bool ownerless_user_page=
      id.space() > 3 && !srv_is_undo_tablespace(id.space()) &&
      (fil_page_type_is_index(local_page_type) ||
       local_page_type == FIL_PAGE_TYPE_BLOB ||
       local_page_type == FIL_PAGE_TYPE_ZBLOB ||
       local_page_type == FIL_PAGE_TYPE_ZBLOB2 ||
       local_page_type == FIL_PAGE_PAGE_COMPRESSED ||
       local_page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
  /* Retained direct reads may preserve user table pages across a lower
  visible boundary; native undo/system/allocation pages must still refresh. */
	  const bool retained_user_page=
	      preserve_retained_user_page && ownerless_user_page;

  uint64_t previous_visible_lsn= 0;
  if (!use_current_visibility)
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_CALLS);
    const int visibility_result=
      push_latest_external_page_visibility(&previous_visible_lsn);
    if (visibility_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_VISIBILITY_PUSH_ERRORS);
      ownerless_coordination_error.store(true, std::memory_order_release);
      return visibility_result;
    }
  }

  const bool local_page_is_observed_external=
      local_lsn != 0 &&
      mylite_ownerless_innodb_external_page_observed_at_or_after(
          id.space(), id.page_no(), local_lsn) != 0;
  const bool local_page_dominates_visible_boundary=
      local_page_dirty || local_page_is_observed_external ||
      (local_lsn != 0 && local_lsn > page_visible_lsn);

  if (!skip_page_version && !force_page_version &&
      ownerless_page_write_refresh_negative_cache_hit(
          id.space(), id.page_no(), local_lsn, page_visible_lsn))
  {
    if (!use_current_visibility)
      page_visible_lsn= previous_visible_lsn;
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  page_t *external_page= static_cast<byte*>(aligned_malloc(page_size, page_size));
  if (external_page == nullptr)
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_ALLOC_FAILURES);
    if (!use_current_visibility)
      page_visible_lsn= previous_visible_lsn;
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  int result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  uint64_t observed_current_rollback_barrier_commit_lsn= 0;
  bool fil_mutex_locked= false;
  auto unlock_fil_system= [&]() {
    if (fil_mutex_locked)
    {
      mysql_mutex_unlock(&fil_system.mutex);
      fil_mutex_locked= false;
    }
  };
  mysql_mutex_lock(&fil_system.mutex);
  fil_mutex_locked= true;
  fil_space_t *space= fil_space_get_by_id(id.space());
  if (space == nullptr)
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_MISSES);
    goto exit;
  }

  {
    uint32_t node_page_no= id.page_no();
    fil_node_t *node= find_file_node_for_page(*space, &node_page_no);
    if (node == nullptr || !node->is_open() || node->deferred)
    {
      ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_NODE_MISSES);
      goto exit;
    }
    const uint32_t space_flags= space->flags;

    uint64_t page_version_commit_lsn= 0;
    uint64_t page_version_page_lsn= 0;
    uint32_t page_version_record_flags= 0;
    int page_version_result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    bool page_version_proved_no_newer= false;
    bool page_version_same_image_observed= false;
    if (!skip_page_version)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_CALLS);
      const uint64_t page_version_read_start_ns=
          ownerless_page_write_refresh_stats_on() ?
              ownerless_page_write_refresh_now_ns() :
              0;
      page_version_result=
          mylite_ownerless_innodb_read_page_version_at_lsn(
              id.space(), id.page_no(), page_visible_lsn, external_page,
              page_size, nullptr, &page_version_page_lsn,
              &page_version_commit_lsn,
              &page_version_record_flags,
              local_page_type == FIL_PAGE_TYPE_SYS &&
                      (id.space() == TRX_SYS_SPACE ||
                       srv_is_undo_tablespace(id.space()))
                  ? MYLITE_OWNERLESS_INNODB_PAGE_READ_HISTORY_RSEG_DELTA
                  : 0);
      ownerless_page_write_refresh_add_elapsed(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_NS,
          page_version_read_start_ns);
    }
    if ((page_version_record_flags &
         MYLITE_OWNERLESS_INNODB_PAGE_VERSION_ROLLBACK_BARRIER) != 0 &&
        page_visible_lsn_is_current && page_version_commit_lsn != 0 &&
        page_version_commit_lsn <= page_visible_lsn)
    {
      observed_current_rollback_barrier_commit_lsn= page_version_commit_lsn;
    }
    if (page_version_result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    {
      if (!skip_page_version)
      {
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_MISSES);
        page_version_proved_no_newer= true;
      }
    }
    else if (page_version_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_ERRORS);
      result= page_version_result;
      goto exit;
    }

	  if (page_version_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
	  {
	      ownerless_page_write_refresh_count(
	          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_HITS);
	      const uint32_t read_space_id=
	          mach_read_from_4(external_page + FIL_PAGE_SPACE_ID);
      const uint32_t read_page_no=
          mach_read_from_4(external_page + FIL_PAGE_OFFSET);
      const lsn_t page_version_lsn=
          mach_read_from_8(external_page + FIL_PAGE_LSN);
      if (read_space_id != id.space() || read_page_no != id.page_no() ||
          page_version_lsn == 0)
      {
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_IDENTITY_MISMATCH);
        result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
        goto exit;
      }
      const bool page_version_matches_local=
          memcmp(external_page, local_page, page_size) == 0;
      const bool page_version_already_observed=
          mylite_ownerless_innodb_external_page_observed_at_or_after(
              id.space(), id.page_no(), page_version_commit_lsn) != 0;
      const bool page_version_older_than_observed=
          page_version_commit_lsn != 0 &&
          page_version_commit_lsn != UINT64_MAX &&
          mylite_ownerless_innodb_external_page_observed_at_or_after(
              id.space(), id.page_no(), page_version_commit_lsn + 1) != 0;
      const bool local_page_already_observed=
          mylite_ownerless_innodb_external_page_observed_at_or_after(
              id.space(), id.page_no(), local_lsn) != 0;
      const bool page_version_retained_observed=
          retained_user_page && page_version_already_observed;
      const bool page_version_snapshot_boundary=
          (page_version_record_flags &
           MYLITE_OWNERLESS_INNODB_PAGE_VERSION_SNAPSHOT_BOUNDARY) != 0;
      const bool page_version_rollback_barrier=
          (page_version_record_flags &
           MYLITE_OWNERLESS_INNODB_PAGE_VERSION_ROLLBACK_BARRIER) != 0;
      const uint64_t local_observed_commit_lsn=
          ownerless_external_page_observed_commit_lsn(
              id.space(), id.page_no());
      /*
      Physical page LSNs are not a total order across ownerless processes. A
      peer can commit a logically newer image whose page LSN is below a local
      image. Permit that regression only when this handle has already tied the
      local image to an observed commit and the candidate is strictly newer
      than every commit observed for the page. This excludes uncommitted local
      images and stale retry payloads while allowing a post-rollback peer
      commit to replace the restored predecessor page.
      */
      const bool current_logically_newer_user_page_version=
          ownerless_user_page &&
          (page_visible_lsn_is_current ||
           allow_current_logical_page_version) &&
          !page_visible_lsn_is_retained && !page_version_snapshot_boundary &&
          !local_transaction_page && page_version_commit_lsn != 0 &&
          (page_version_commit_lsn >= local_lsn ||
           (page_version_rollback_barrier &&
            page_version_page_lsn >= local_lsn)) &&
          (page_version_commit_lsn > local_observed_commit_lsn ||
           (allow_current_logical_page_version &&
            !page_version_older_than_observed)) &&
          page_version_commit_lsn <= page_visible_lsn;
      const bool retained_native_write_scrub_page=
          retained_user_page && page_visible_lsn_is_current &&
          page_visible_lsn_is_retained &&
          mylite_ownerless_retained_startup_native_write_scrub &&
          !local_page_dirty;
      const bool retained_current_snapshot_boundary=
          retained_user_page && page_visible_lsn_is_current &&
          page_version_snapshot_boundary && !retained_native_write_scrub_page;
      const bool page_version_boundary_newer_than_local=
          page_version_lsn > local_lsn ||
          (retained_user_page && page_version_commit_lsn > local_lsn);
      /*
      Retained ownerless user-page versions can be older than the local native
      page LSN while still carrying a committed peer image that is not present
      in the native page. Keep that allowance for unobserved local pages, but
      do not let a later retained-boundary refresh move this handle behind a
      higher-LSN image it has already exposed to SQL.
      */
      const bool retained_current_unobserved_page=
          retained_user_page && page_visible_lsn_is_current &&
          page_visible_lsn_is_retained && !local_page_already_observed;
      const bool committed_boundary_newer_than_local=
          allow_dirty_committed_page_refresh &&
          page_version_commit_lsn > local_lsn;
      const bool page_version_would_regress_dirty_local_page=
          local_page_dirty && !page_version_matches_local &&
          !retained_current_unobserved_page &&
          !allow_dirty_committed_page_refresh &&
          !current_logically_newer_user_page_version;
      const bool current_read_would_regress_physical_page=
          page_version_lsn < local_lsn &&
          !page_version_boundary_newer_than_local &&
          !committed_boundary_newer_than_local &&
          !current_logically_newer_user_page_version &&
          (!retained_user_page ||
           (local_page_already_observed && !retained_native_write_scrub_page));
      const bool visible_boundary_allowed=
          allow_visible_boundary &&
          (!page_version_retained_observed || retained_native_write_scrub_page ||
           page_version_boundary_newer_than_local) &&
          !page_version_older_than_observed &&
          !retained_current_snapshot_boundary &&
          !current_read_would_regress_physical_page &&
          !page_version_would_regress_dirty_local_page &&
          page_version_commit_lsn != 0 &&
          page_version_commit_lsn <= page_visible_lsn;
      const bool boundary_newer_than_local=
          allow_boundary_newer &&
          !page_version_older_than_observed &&
          !retained_current_snapshot_boundary &&
          !current_read_would_regress_physical_page &&
          (page_version_boundary_newer_than_local ||
           current_logically_newer_user_page_version);
      const bool observed_same_lsn_boundary=
          (page_version_already_observed ||
           retained_current_snapshot_boundary) &&
	          !visible_boundary_allowed &&
	          !boundary_newer_than_local &&
	          page_version_lsn == local_lsn &&
	          !page_version_matches_local;
	      const bool page_version_same_lsn_same_image=
		          page_version_lsn == local_lsn && page_version_matches_local;
	      if ((page_version_lsn < local_lsn ||
             page_version_older_than_observed) &&
	          !boundary_newer_than_local && !visible_boundary_allowed)
	      {
	        ownerless_page_write_refresh_count(
	            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER);
	        page_version_proved_no_newer= true;
	      }
      else if (page_version_same_lsn_same_image)
      {
        mylite_ownerless_innodb_note_external_page_observed(
            id.space(), id.page_no(), page_version_commit_lsn);
        if (retained_native_write_scrub_page)
          mark_retained_native_write_scrub_page_dirty(block);
        page_version_same_image_observed= true;
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER);
        page_version_proved_no_newer= true;
      }
	      else if (observed_same_lsn_boundary)
	      {
	        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_NOT_NEWER);
        page_version_proved_no_newer= true;
	      }
	      else
	      {
	        advance_external_lsn(page_version_lsn);
        if (buf_page_is_corrupted(true, external_page, space_flags) !=
            NOT_CORRUPTED)
        {
          ownerless_page_write_refresh_count(
              OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES);
          result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
          goto exit;
        }
        unlock_fil_system();
        if (!copy_refreshed_page_to_block(
                block, local_page, external_page, page_size, mirror_to_zip,
                decompress_frame))
        {
          ownerless_page_write_refresh_count(
              OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES);
          result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
          goto exit;
        }
        mylite_ownerless_innodb_note_external_page_observed(
            id.space(), id.page_no(), page_version_commit_lsn);
        if (retained_native_write_scrub_page)
          mark_retained_native_write_scrub_page_dirty(block);
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_OVERLAYS);
        if (id.page_no() == 0)
        {
          ownerless_page_write_refresh_count(
              OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES);
          refresh_external_space_header(id.space());
        }
		        result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
		        goto exit;
		      }
		    }

    const os_offset_t offset=
        os_offset_t{node_page_no} * os_offset_t{page_size};
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_CALLS);
    const uint64_t disk_read_start_ns=
        ownerless_page_write_refresh_stats_on() ?
            ownerless_page_write_refresh_now_ns() :
            0;
    if (os_file_read(IORequestRead, node->handle, external_page, offset,
                     page_size, nullptr) != DB_SUCCESS)
    {
      ownerless_page_write_refresh_add_elapsed(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_NS,
          disk_read_start_ns);
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_FAILURES);
      if (!local_page_dominates_visible_boundary)
        result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      goto exit;
    }
    ownerless_page_write_refresh_add_elapsed(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_NS, disk_read_start_ns);

    const uint32_t read_space_id=
        mach_read_from_4(external_page + FIL_PAGE_SPACE_ID);
    const uint32_t read_page_no=
        mach_read_from_4(external_page + FIL_PAGE_OFFSET);
    const lsn_t disk_page_lsn= mach_read_from_8(external_page + FIL_PAGE_LSN);
    const uint16_t disk_page_type= fil_page_get_type(external_page);
    const page_id_t local_page_id{
        mach_read_from_4(local_page + FIL_PAGE_SPACE_ID),
        mach_read_from_4(local_page + FIL_PAGE_OFFSET)};
    const bool local_recovered_page_is_authoritative=
        page_version_proved_no_newer && local_lsn != 0 && local_page_id == id &&
        buf_page_is_corrupted(true, local_page, space_flags) == NOT_CORRUPTED;
    if (read_space_id != id.space() || read_page_no != id.page_no())
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_IDENTITY_MISMATCH);
      /* A newly initialized or redo-reconstructed buffer block can precede
      its native sparse-file allocation. With no newer WAL version, the block
      identity and local LSN are authoritative even before its page header is
      finalized. */
      if (page_version_proved_no_newer && local_lsn != 0)
        goto exit;
      if (!local_page_dominates_visible_boundary)
        result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      goto exit;
    }

	    const bool disk_page_is_visible=
	        page_visible_lsn == 0 || disk_page_lsn <= page_visible_lsn;
    const bool page_version_covers_disk_page=
        page_version_result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        page_version_commit_lsn != 0 &&
        disk_page_lsn <= page_version_commit_lsn;
    const bool disk_page_matches_local=
        memcmp(external_page, local_page, page_size) == 0;
    const bool disk_page_older_than_observed=
        disk_page_lsn != 0 && disk_page_lsn != UINT64_MAX &&
        mylite_ownerless_innodb_external_page_observed_at_or_after(
            id.space(), id.page_no(), disk_page_lsn + 1) != 0;
    const bool disk_page_newer_and_visible=
        disk_page_lsn > local_lsn && disk_page_is_visible &&
        !page_version_covers_disk_page && !disk_page_older_than_observed;
    const bool disk_visible_boundary_allowed=
        allow_visible_boundary && disk_page_is_visible && !retained_user_page &&
        !page_version_same_image_observed && !page_version_covers_disk_page &&
        !disk_page_older_than_observed;
    const bool disk_page_same_lsn_different_image=
        disk_page_lsn == local_lsn && disk_page_is_visible &&
        !retained_user_page && !page_version_same_image_observed &&
        !page_version_covers_disk_page && !disk_page_older_than_observed &&
        !disk_page_matches_local;
    const bool disk_boundary_would_regress_dirty_local_page=
        local_page_dirty && !disk_page_matches_local &&
        !allow_dirty_committed_page_refresh;
    const bool rollback_barrier_native_exact=
        (page_version_record_flags &
         MYLITE_OWNERLESS_INNODB_PAGE_VERSION_ROLLBACK_BARRIER) != 0 &&
        page_visible_lsn_is_current && page_version_commit_lsn != 0 &&
        page_version_commit_lsn <= page_visible_lsn &&
        /*
        A barrier proves the exact restored native page, not every older disk
        image. Accepting a lower LSN can resurrect a pre-rollback clustered
        page after later peer commits have deleted or updated its records.
        */
        page_version_page_lsn != 0 && disk_page_lsn == page_version_page_lsn &&
        disk_page_is_visible;
    const bool rollback_barrier_native_fallback=
        rollback_barrier_native_exact && !disk_page_matches_local;
    const bool current_read_disk_boundary_would_regress=
        disk_page_lsn < local_lsn;
    const bool local_blob_page=
        local_page_type == FIL_PAGE_TYPE_BLOB ||
        local_page_type == FIL_PAGE_TYPE_ZBLOB ||
        local_page_type == FIL_PAGE_TYPE_ZBLOB2;
    const bool disk_blob_page=
        disk_page_type == FIL_PAGE_TYPE_BLOB ||
        disk_page_type == FIL_PAGE_TYPE_ZBLOB ||
        disk_page_type == FIL_PAGE_TYPE_ZBLOB2;
    const bool local_allocation_metadata_page=
        allocation_metadata_page_type(local_page_type);
    const bool disk_allocation_metadata_page=
        allocation_metadata_page_type(disk_page_type);
    const bool native_current_disk_regression_shape_safe=
        (local_blob_page && disk_blob_page) ||
        (local_allocation_metadata_page && disk_allocation_metadata_page);
	    const bool native_current_disk_regression_allowed=
	        allow_native_disk_regression && page_visible_lsn_is_current &&
	        !page_visible_lsn_is_retained && disk_page_is_visible &&
	        disk_page_lsn < local_lsn &&
	        (ownerless_user_page || local_allocation_metadata_page) &&
	        native_current_disk_regression_shape_safe &&
	        !page_version_same_image_observed && !page_version_covers_disk_page;
	    if (disk_page_newer_and_visible)
      advance_external_lsn(disk_page_lsn);
    if (buf_page_is_corrupted(true, external_page, space_flags) !=
        NOT_CORRUPTED)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES);
      if (local_recovered_page_is_authoritative)
        goto exit;
      if (!local_page_dominates_visible_boundary)
        result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      goto exit;
    }
    bool should_store_negative_cache= false;
    bool should_refresh_space_header= id.page_no() == 0 && disk_page_is_visible;
    if (rollback_barrier_native_fallback ||
        disk_page_newer_and_visible ||
        (disk_page_same_lsn_different_image &&
         !disk_boundary_would_regress_dirty_local_page) ||
        (disk_visible_boundary_allowed &&
         !current_read_disk_boundary_would_regress &&
	        !disk_boundary_would_regress_dirty_local_page) ||
	        native_current_disk_regression_allowed)
	    {
	      unlock_fil_system();
      if (!copy_refreshed_page_to_block(
              block, local_page, external_page, page_size, mirror_to_zip,
              decompress_frame))
      {
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES);
        result= MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
        goto exit;
      }
      if (disk_page_is_visible && disk_page_lsn != 0 &&
          !rollback_barrier_native_exact)
      {
        mylite_ownerless_innodb_note_external_page_observed(
            id.space(), id.page_no(), disk_page_lsn);
      }
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_OVERLAYS);
    }
    else
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_NOT_NEWER);
      if (page_version_proved_no_newer && !force_page_version)
        should_store_negative_cache= true;
    }
    unlock_fil_system();
    if (should_refresh_space_header)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES);
      refresh_external_space_header(id.space());
    }
    if (should_store_negative_cache)
      ownerless_page_write_refresh_negative_cache_store(
          id.space(), id.page_no(), local_lsn, page_visible_lsn);
    result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

exit:
  unlock_fil_system();
  aligned_free(external_page);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      observed_current_rollback_barrier_commit_lsn != 0)
  {
    /*
    Preserve the semantic identity of the restored native image. A later MTR
    handoff boundary must not turn this rollback proof into an ordinary
    readable page version.
    */
    mylite_ownerless_innodb_note_external_page_rollback_barrier(
        id.space(), id.page_no(),
        observed_current_rollback_barrier_commit_lsn);
  }
  if (!use_current_visibility)
    page_visible_lsn= previous_visible_lsn;
  if (ownerless_lock_result_is_coordination_failure(result))
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result;
}

fil_node_t *find_file_node_for_page(fil_space_t &space, uint32_t *page_no)
{
  if (page_no == nullptr)
    return nullptr;

  for (fil_node_t *node= UT_LIST_GET_FIRST(space.chain); node != nullptr;
       node= UT_LIST_GET_NEXT(chain, node))
  {
    if (*page_no < node->size)
      return node;
    *page_no-= node->size;
  }
  return nullptr;
}

void refresh_replaceable_buffer_pool_pages()
{
  mysql_mutex_lock(&buf_pool.mutex);
  const ulint attempts =
      UT_LIST_GET_LEN(buf_pool.LRU) + UT_LIST_GET_LEN(buf_pool.unzip_LRU);
  for (ulint i= 0; i < attempts; i++)
  {
    if (!buf_LRU_scan_and_free_block(ULINT_UNDEFINED))
      break;
  }
  mysql_mutex_unlock(&buf_pool.mutex);
}

void collect_buffer_pool_file_pages(std::vector<uint64_t> &pages)
{
  mysql_mutex_lock(&buf_pool.mutex);
  const ulint cell_count= buf_pool.page_hash.n_cells;
  for (ulint cell= 0; cell < cell_count; ++cell)
  {
    buf_pool_t::hash_chain &chain=
        buf_pool.page_hash.array[buf_pool_t::page_hash_table::pad(cell)];
    for (buf_page_t *bpage= chain.first; bpage != nullptr;
         bpage= bpage->hash)
    {
      if (!bpage->in_file())
        continue;
      const page_id_t id{bpage->id()};
      if (id.space() >= SRV_TMP_SPACE_ID)
        continue;
      pages.push_back((uint64_t{id.space()} << 32) | id.page_no());
    }
  }
  mysql_mutex_unlock(&buf_pool.mutex);
}

void refresh_buffer_pool_pages(bool force_page_version, bool evict_clean_pages,
                               bool allow_boundary_newer,
                               bool allow_visible_boundary,
                               bool preserve_retained_user_page,
                               bool skip_page_version,
                               bool preserve_local_transaction_page,
                               bool allow_native_disk_regression,
                               bool allow_dirty_committed_page_refresh,
                               bool user_tablespaces_only)
{
  std::vector<uint64_t> pages;
  collect_buffer_pool_file_pages(pages);

  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  for (uint64_t packed_page : pages)
  {
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    if (user_tablespaces_only && space_id <= 3)
      continue;
    const int result= refresh_buffer_pool_page(
        space_id, page_no, false, force_page_version, evict_clean_pages,
        allow_boundary_newer, allow_visible_boundary,
        preserve_retained_user_page, skip_page_version,
        preserve_local_transaction_page, allow_native_disk_regression,
        allow_dirty_committed_page_refresh);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      break;
  }
}

void advance_external_lsn(uint64_t latest_lsn)
{
  if (latest_lsn == 0)
    return;

  bool needs_flush= false;
  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  if (latest_lsn > log_sys.get_lsn())
    log_sys.set_recovered_lsn(latest_lsn);
  else if (latest_lsn >
           log_sys.get_flushed_lsn(std::memory_order_relaxed))
    needs_flush= true;
  log_sys.latch.wr_unlock();

  if (needs_flush)
    log_write_up_to(static_cast<lsn_t>(latest_lsn), true);
}

int push_latest_external_page_visibility(uint64_t *previous_lsn)
{
  if (previous_lsn == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  *previous_lsn= page_visible_lsn;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return result;

  advance_external_lsn(latest_lsn);
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

int refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no,
                             bool load_if_missing, bool force_page_version,
                             bool evict_clean_page, bool allow_boundary_newer,
                             bool allow_visible_boundary,
                             bool preserve_retained_user_page,
                             bool skip_page_version,
                             bool preserve_local_transaction_page,
                             bool allow_native_disk_regression,
                             bool allow_dirty_committed_page_refresh)
{
  const page_id_t id(space_id, page_no);

  mysql_mutex_lock(&buf_pool.mutex);
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  hash_lock.lock_shared();
  buf_page_t *bpage= buf_pool.page_hash.get(id, chain);
  hash_lock.unlock_shared();

  const bool evicted_clean_page=
      evict_clean_page && bpage != nullptr &&
      bpage->oldest_modification_acquire() == 0 &&
      buf_LRU_free_page(bpage, true);

  mysql_mutex_unlock(&buf_pool.mutex);

  const int previous_bypass=
      mylite_ownerless_innodb_set_page_write_refresh_bypass(1);
  mtr_t mtr(nullptr);
  mtr.start();
  dberr_t err= DB_SUCCESS;
  int result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  const ulint get_mode= (load_if_missing || evicted_clean_page)
      ? BUF_GET
      : BUF_GET_IF_IN_POOL;
  if (buf_block_t *block= buf_page_get_gen(id, 0, RW_X_LATCH, nullptr,
                                           get_mode, &mtr, &err))
  {
    if (force_page_version || block->page.oldest_modification_acquire() == 0)
      result= refresh_page_for_write(
          *block, force_page_version, force_page_version,
          allow_boundary_newer, allow_visible_boundary,
          preserve_retained_user_page, skip_page_version,
          preserve_local_transaction_page, allow_native_disk_regression,
          allow_dirty_committed_page_refresh);
  }
  else if (get_mode == BUF_GET && err != DB_SUCCESS)
  {
    result= err == DB_TABLESPACE_DELETED &&
                    mylite_ownerless_retained_startup_native_write_scrub
                ? MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
                : ownerless_lock_result_from_dberr(err);
  }
  const dberr_t mtr_error= mtr.commit();
  mylite_ownerless_innodb_set_page_write_refresh_bypass(previous_bypass);
  if (UNIV_UNLIKELY(mtr_error != DB_SUCCESS))
    ib::error() << "MyLite ownerless page refresh mini-transaction failed: "
                << id << " error=" << mtr_error;
  if (UNIV_UNLIKELY(ownerless_lock_result_is_coordination_failure(result)))
    ib::error() << "MyLite ownerless page refresh failed: " << id
                << " result=" << result;
  if (ownerless_lock_result_is_coordination_failure(result))
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result;
}

bool record_bit_set(const ib_lock_t *lock, uint32_t heap_no)
{
  return heap_no < lock_rec_get_n_bits(lock) &&
         lock_rec_get_nth_bit(lock, heap_no) != 0;
}

void clear_transaction_wait(trx_id_t trx_id, trx_t *trx)
{
  if (trx_id == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    static_cast<void>(missing_required_hook_result(trx));
    return;
  }

  mylite_ownerless_innodb_lock_clear_wait_callback hook=
      clear_wait_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    static_cast<void>(missing_required_hook_result(trx));
    return;
  }

  const int result= hook(trx_id, context);
  handle_hook_result("clear wait", result, trx);
}

int release_transaction_page_writes(trx_id_t trx_id)
{
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_release_page_writes_callback hook=
      release_page_writes_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const int result= hook(trx_id, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
             ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
             : result;
}

int release_transaction_records(trx_id_t trx_id)
{
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  mylite_ownerless_innodb_lock_release_page_writes_callback hook=
      release_records_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
  {
    ownerless_coordination_error.store(true, std::memory_order_release);
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  const int result= hook(trx_id, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    ownerless_coordination_error.store(true, std::memory_order_release);
  return result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE
             ? MYLITE_OWNERLESS_INNODB_LOCK_ERROR
             : result;
}

trx_id_t lock_transaction_id(const ib_lock_t *lock, bool create_transient)
{
  if (lock == nullptr || lock->trx == nullptr)
    return 0;

  trx_t *trx= lock->trx;
  if (trx->mylite_ownerless_lock_trx_id != 0)
    return trx->mylite_ownerless_lock_trx_id;
  if (trx->id != 0)
    return trx->id;
  if (!create_transient)
    return 0;

  const trx_id_t transient_id= allocate_transient_lock_trx_id();
  if (transient_id == 0)
    return 0;
  trx->mylite_ownerless_lock_trx_id= transient_id;
  return transient_id;
}

trx_id_t transaction_lock_id(trx_t *trx, bool create_transient)
{
  if (trx == nullptr)
    return 0;
  if (trx->mylite_ownerless_lock_trx_id != 0)
    return trx->mylite_ownerless_lock_trx_id;
  if (trx->id != 0)
    return trx->id;
  if (!create_transient)
    return 0;

  const trx_id_t transient_id= allocate_transient_lock_trx_id();
  if (transient_id == 0)
    return 0;
  trx->mylite_ownerless_lock_trx_id= transient_id;
  return transient_id;
}

trx_id_t transaction_lock_id(const trx_t *trx)
{
  if (trx == nullptr)
    return 0;
  if (trx->mylite_ownerless_lock_trx_id != 0)
    return trx->mylite_ownerless_lock_trx_id;
  return trx->id;
}

trx_id_t page_write_transaction_id(trx_t *trx)
{
  if (trx != nullptr)
  {
    if (trx->mylite_ownerless_page_write_trx_id != 0)
      return trx->mylite_ownerless_page_write_trx_id;

    const trx_id_t trx_id= transaction_lock_id(trx, true);
    if (trx_id != 0)
    {
      trx->mylite_ownerless_page_write_trx_id= trx_id;
      return trx_id;
    }
  }

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id != 0)
    return trx_id;
  if (page_write_lock_trx_id == 0)
    page_write_lock_trx_id= allocate_transient_lock_trx_id();
  return page_write_lock_trx_id;
}

uint64_t page_write_pack(uint32_t space_id, uint32_t page_no)
{
  return (uint64_t{space_id} << 32) | page_no;
}

uint64_t page_write_transaction_gate_for_space(const trx_t *trx,
                                               uint32_t space_id)
{
  const uint64_t global_gate= page_write_pack(
      MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID,
      MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO);
  const uint64_t space_gate= page_write_pack(
      space_id, MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO);

  if (trx == nullptr)
    return global_gate;

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages_for_read();
  if (pages != nullptr)
    for (uint64_t packed_page : *pages)
    {
      if (!packed_page_write_transaction_gate(packed_page))
        continue;
      if (packed_page == global_gate)
        return global_gate;
      if (packed_page == space_gate)
        return space_gate;
    }

  return space_gate;
}

bool transaction_has_page_write_gate(const trx_t *trx, uint64_t gate_page)
{
  if (trx == nullptr)
    return false;

  return trx->mylite_ownerless_modified_page_contains(gate_page) ||
         trx->mylite_ownerless_modified_page_contains(
             page_write_pack(
                 MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID,
                 MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO));
}

bool transaction_has_page_write_entry(const trx_t *trx, uint64_t packed_page)
{
  if (trx == nullptr)
    return false;

  return trx->mylite_ownerless_modified_page_contains(packed_page);
}

void note_transaction_page_write_gate(trx_t *trx, uint64_t gate_page)
{
  if (trx == nullptr || transaction_has_page_write_gate(trx, gate_page))
    return;

  trx->mylite_ownerless_note_modified_page(gate_page);
}

void note_transaction_page_write_page(trx_t *trx, uint64_t packed_page)
{
  if (trx == nullptr || packed_page_write_transaction_gate(packed_page) ||
      transaction_has_page_write_entry(trx, packed_page))
    return;

  trx->mylite_ownerless_note_modified_page(packed_page);
}

bool packed_page_write_transaction_gate(uint64_t packed_page)
{
  const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
  const uint32_t page_no= static_cast<uint32_t>(packed_page);
  return (space_id == MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID &&
          page_no == MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO) ||
         (space_id < SRV_TMP_SPACE_ID &&
          (page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO ||
           page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO));
}

bool packed_page_write_synthetic_gate(uint64_t packed_page)
{
  const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
  const uint32_t page_no= static_cast<uint32_t>(packed_page);
  return packed_page_write_transaction_gate(packed_page) ||
         (space_id < SRV_TMP_SPACE_ID &&
          page_no == MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO);
}

bool transaction_has_page_write_image(const trx_t *trx, uint64_t packed_page)
{
  if (trx == nullptr || trx->mylite_ownerless_page_images == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_image_vector *images=
      trx->mylite_ownerless_page_images;
  return std::find_if(images->begin(), images->end(),
                      [packed_page](
                          const trx_t::mylite_ownerless_page_image &image) {
                        return image.packed_page == packed_page;
                      }) != images->end();
}

bool transaction_should_keep_page_write_gate(const trx_t *trx,
                                             uint64_t gate_page)
{
  if (trx == nullptr || !packed_page_write_transaction_gate(gate_page))
    return false;

  const uint32_t gate_space= static_cast<uint32_t>(gate_page >> 32);
  const uint32_t gate_page_no= static_cast<uint32_t>(gate_page);
  if (gate_page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_READ_PAGE_NO)
    return transaction_keeps_page_writes_to_end(trx);
  const bool global_gate=
      gate_space == MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID;

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages_for_read();
  if (pages == nullptr)
    return false;

  for (uint64_t packed_page : *pages)
  {
    if (packed_page_write_transaction_gate(packed_page))
      continue;
    if (!global_gate &&
        static_cast<uint32_t>(packed_page >> 32) != gate_space)
      continue;
    if (trx->mylite_ownerless_dirty_page_contains(packed_page) ||
        transaction_has_page_write_image(trx, packed_page))
      return true;
  }

  return false;
}

bool transaction_should_keep_statement_page_write(const trx_t *trx,
                                                  uint64_t packed_page)
{
  if (packed_page_write_transaction_gate(packed_page))
    return true;
  if (trx == nullptr)
    return false;
  return trx->mylite_ownerless_dirty_page_contains(packed_page) ||
         transaction_has_page_write_image(trx, packed_page);
}

bool transaction_keeps_page_writes_to_end(const trx_t *trx)
{
  if (trx == nullptr)
    return false;
  if (trx->mysql_thd == nullptr)
    return !trx->auto_commit;
  return !trx->auto_commit ||
         (trx->mysql_thd->variables.option_bits &
          (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) != 0;
}

bool transaction_sql_is_plain_select(const trx_t *trx)
{
  THD *thd= trx != nullptr ? trx->mysql_thd : nullptr;
  if (thd == nullptr || thd->lex == nullptr ||
      thd->lex->sql_command != SQLCOM_SELECT)
    return false;
  const SELECT_LEX *select_lex= thd->lex->first_select_lex();
  return select_lex == nullptr ||
         select_lex->select_lock == st_select_lex::select_lock_type::NONE;
}

bool transaction_sql_allows_visible_fast_path(const trx_t *trx)
{
  return trx != nullptr &&
         mylite_ownerless_innodb_statement_visible_fast_path() != 0;
}

bool transaction_should_track_page_write(trx_t *trx,
                                         uint32_t space_id,
                                         uint32_t page_no)
{
  if (trx == nullptr || space_id >= SRV_TMP_SPACE_ID ||
      packed_page_write_synthetic_gate(page_write_pack(space_id, page_no)))
    return false;
  if (trx->read_only || trx->dict_operation)
    return false;
  if (trx->mysql_thd == nullptr && !trx->has_logged() && trx->undo_no == 0 &&
      trx->mod_tables.empty())
    return false;
  if (trx->mysql_thd != nullptr)
  {
    if (transaction_sql_is_plain_select(trx))
      return false;
    const bool sql_autocommit=
      !(trx->mysql_thd->variables.option_bits &
        (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
    if (sql_autocommit && transaction_sql_allows_visible_fast_path(trx))
      return false;
  }
  else if (trx->auto_commit)
    return false;
  return true;
}

uint32_t normalized_lock_mode(const ib_lock_t *lock)
{
  return normalized_lock_mode(lock->type_mode);
}

uint32_t normalized_lock_mode(uint32_t type_mode)
{
  return type_mode & LOCK_MODE_MASK;
}

uint32_t record_lock_flags(const ib_lock_t *lock, uint32_t heap_no)
{
  return record_lock_flags(lock->type_mode, heap_no);
}

uint32_t record_lock_flags(uint32_t type_mode, uint32_t heap_no)
{
  uint32_t flags= 0;
  if (type_mode & LOCK_GAP)
    flags|= MYLITE_OWNERLESS_INNODB_RECORD_LOCK_GAP;
  if (type_mode & LOCK_REC_NOT_GAP)
    flags|= MYLITE_OWNERLESS_INNODB_RECORD_LOCK_REC_NOT_GAP;
  if (type_mode & LOCK_INSERT_INTENTION)
    flags|= MYLITE_OWNERLESS_INNODB_RECORD_LOCK_INSERT_INTENTION;
  if (heap_no == PAGE_HEAP_NO_SUPREMUM)
    flags|= MYLITE_OWNERLESS_INNODB_RECORD_LOCK_SUPREMUM;
  return flags;
}

} // namespace
