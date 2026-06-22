#define LOCK_MODULE_IMPLEMENTATION
#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include "mylite_ownerless_innodb_lock_hooks.h"

#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0lru.h"
#include "dict0dict.h"
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
#include "page0page.h"
#include "srv0srv.h"
#include "sql_class.h" // THD
#include "trx0sys.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

std::atomic<bool> mylite_ownerless_innodb_lock_hooks_enabled{false};
std::atomic<bool> mylite_ownerless_innodb_autoinc_hooks_enabled{false};
std::atomic<bool> mylite_ownerless_innodb_test_faults_enabled{false};
thread_local bool mylite_ownerless_statement_visible_fast_path= false;
thread_local bool mylite_ownerless_statement_deferred_page_publish= false;
thread_local bool mylite_ownerless_statement_deferred_redo_admission_ready=
    false;
thread_local bool mylite_ownerless_statement_plain_read= false;
thread_local bool mylite_ownerless_statement_plain_read_preserve_local_pages=
    false;
thread_local bool mylite_ownerless_statement_plain_read_pages_refreshed= false;
thread_local bool mylite_ownerless_statement_dictionary_ddl= false;
thread_local bool mylite_ownerless_statement_suppress_native_lifecycle_refresh=
    false;

namespace {

constexpr trx_id_t k_transient_lock_trx_id_flag =
    trx_id_t{1} << ((sizeof(trx_id_t) * 8) - 1);
constexpr size_t k_page_write_refresh_negative_cache_entries= 64;
constexpr size_t k_external_page_observation_entries= 128;
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
std::atomic<mylite_ownerless_innodb_skip_external_page_refresh_callback>
    skip_external_page_refresh_callback{nullptr};
std::atomic<mylite_ownerless_innodb_autoinc_read_callback>
    autoinc_read_callback{nullptr};
std::atomic<mylite_ownerless_innodb_autoinc_publish_callback>
    autoinc_publish_callback{nullptr};
std::atomic<void *> callback_context{nullptr};
std::atomic<void *> autoinc_callback_context{nullptr};
std::atomic<trx_id_t> next_transient_lock_trx_id{1};
std::atomic<bool> checkpoint_suppressed{false};
std::atomic<bool> relative_file_op_redo_paths{false};
std::atomic<bool> uncheckpointed_file_rename_recovery{false};
std::atomic<bool> file_op_redo_logged{false};
std::atomic<uint64_t> test_fault_match_count{0};
thread_local uint64_t page_visible_lsn= 0;
thread_local bool page_visible_lsn_is_current= false;
thread_local bool page_visible_lsn_is_retained= false;
thread_local unsigned redo_depth= 0;
thread_local uint64_t redo_latest_lsn= 0;
thread_local trx_id_t page_write_lock_trx_id= 0;
thread_local bool checkpoint_suppression_bypass= false;

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
};

std::atomic<bool> ownerless_page_write_refresh_stats_enabled{false};
std::atomic<uint64_t> ownerless_page_write_refresh_stats
    [OWNERLESS_PAGE_WRITE_REFRESH_STAT_COUNT];
std::atomic<uint64_t> ownerless_page_write_refresh_cache_epoch{1};
thread_local ownerless_page_write_refresh_negative_cache_entry
    ownerless_page_write_refresh_negative_cache
        [k_page_write_refresh_negative_cache_entries];
thread_local uint64_t ownerless_external_page_observation_token= 0;
thread_local ownerless_external_page_observation_entry
    ownerless_external_page_observations[k_external_page_observation_entries];

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
    uint32_t space_id, uint32_t page_no) noexcept
{
  return static_cast<size_t>(
      ownerless_page_key_hash(space_id, page_no) &
      (k_external_page_observation_entries - 1));
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

void handle_hook_result(const char *operation, int result);
bool ownerless_lock_hooks_enabled();
bool ownerless_autoinc_hooks_enabled();
bool lock_publishable(const ib_lock_t *lock);
bool table_lock_publishable(const ib_lock_t *lock);
bool record_lock_publishable(const ib_lock_t *lock);
bool wait_lock_publishable(const ib_lock_t *lock);
bool blocker_lock_publishable(const ib_lock_t *lock);
void advance_external_lsn(uint64_t latest_lsn);
int push_latest_external_page_visibility(uint64_t *previous_lsn);
void refresh_external_space_header(uint32_t space_id);
void refresh_external_space_allocation_pages(uint32_t space_id);
void refresh_external_space_headers();
bool refresh_external_space_header(fil_space_t &space);
bool table_can_be_evicted_from_dictionary(dict_table_t *table);
bool foreign_table_can_be_reloaded_from_dictionary(dict_table_t *table);
int refresh_page_for_write(const buf_block_t &block,
                           bool use_current_visibility= false,
                           bool force_page_version= false,
                           bool allow_boundary_newer= false,
                           bool allow_visible_boundary= false,
                           bool preserve_retained_user_page= false,
                           bool skip_page_version= false);
fil_node_t *find_file_node_for_page(fil_space_t &space, uint32_t *page_no);
void refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no,
                              bool load_if_missing,
                              bool force_page_version= false,
                              bool evict_clean_page= true,
                              bool allow_boundary_newer= false,
                              bool allow_visible_boundary= false,
                              bool preserve_retained_user_page= false,
                              bool skip_page_version= false);
void refresh_buffer_pool_pages(bool force_page_version= false,
                               bool evict_clean_pages= true,
                               bool allow_boundary_newer= false,
                               bool allow_visible_boundary= false,
                               bool preserve_retained_user_page= false,
                               bool skip_page_version= false);
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
bool transaction_should_track_page_write(trx_t *trx,
                                         uint32_t space_id,
                                         uint32_t page_no);
void collect_buffer_pool_file_pages(std::vector<uint64_t> &pages);
int flush_deferred_redo_batch();
void retain_deferred_redo_batch_tail(size_t first, size_t count);
void publish_pages_visible_lsn(uint64_t visible_lsn);
void clear_transaction_wait(trx_id_t trx_id);
void release_transaction_page_writes(trx_id_t trx_id);
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
    mylite_ownerless_innodb_redo_reserve_callback redo_reserve_hook,
    mylite_ownerless_innodb_redo_written_callback redo_written_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    mylite_ownerless_innodb_pages_visible_callback pages_visible_hook,
    mylite_ownerless_innodb_page_publish_callback page_publish_hook,
    mylite_ownerless_innodb_page_read_callback page_read_hook,
    mylite_ownerless_innodb_skip_external_page_refresh_callback skip_external_page_refresh_hook,
    void *context)
{
  if (acquire_table_hook == nullptr || release_table_hook == nullptr ||
      wait_table_hook == nullptr || acquire_record_hook == nullptr ||
      release_record_hook == nullptr || acquire_page_write_hook == nullptr ||
      release_page_write_hook == nullptr || wait_record_hook == nullptr ||
      release_page_writes_hook == nullptr ||
      wait_until_table_hook == nullptr || wait_until_record_hook == nullptr ||
      before_record_wait_hook == nullptr || clear_wait_hook == nullptr ||
      redo_enter_hook == nullptr || redo_observe_hook == nullptr ||
      redo_reserve_hook == nullptr || redo_written_hook == nullptr ||
      redo_leave_hook == nullptr || pages_visible_hook == nullptr ||
      page_publish_hook == nullptr || page_read_hook == nullptr ||
      skip_external_page_refresh_hook == nullptr ||
      context == nullptr)
  {
    mylite_ownerless_innodb_lock_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  skip_external_page_refresh_callback.store(
      skip_external_page_refresh_hook, std::memory_order_release);
  page_read_callback.store(page_read_hook, std::memory_order_release);
  page_publish_callback.store(page_publish_hook, std::memory_order_release);
  pages_visible_callback.store(pages_visible_hook, std::memory_order_release);
  redo_leave_callback.store(redo_leave_hook, std::memory_order_release);
  redo_written_callback.store(redo_written_hook, std::memory_order_release);
  redo_reserve_callback.store(redo_reserve_hook, std::memory_order_release);
  redo_observe_callback.store(redo_observe_hook, std::memory_order_release);
  redo_enter_callback.store(redo_enter_hook, std::memory_order_release);
  clear_wait_callback.store(clear_wait_hook, std::memory_order_release);
  before_record_wait_callback.store(before_record_wait_hook, std::memory_order_release);
  wait_until_record_callback.store(wait_until_record_hook, std::memory_order_release);
  wait_record_callback.store(wait_record_hook, std::memory_order_release);
  release_page_writes_callback.store(release_page_writes_hook, std::memory_order_release);
  release_page_write_callback.store(release_page_write_hook, std::memory_order_release);
  acquire_page_write_callback.store(acquire_page_write_hook, std::memory_order_release);
  release_record_callback.store(release_record_hook, std::memory_order_release);
  acquire_record_callback.store(acquire_record_hook, std::memory_order_release);
  wait_until_table_callback.store(wait_until_table_hook, std::memory_order_release);
  wait_table_callback.store(wait_table_hook, std::memory_order_release);
  release_table_callback.store(release_table_hook, std::memory_order_release);
  acquire_table_callback.store(acquire_table_hook, std::memory_order_release);
  ownerless_page_write_refresh_cache_epoch.fetch_add(
      1, std::memory_order_acq_rel);
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

extern "C" void mylite_ownerless_innodb_lock_reset_hooks(void)
{
  const int flush_result= mylite_ownerless_innodb_redo_flush_deferred();
  if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    ut_error;
  ownerless_page_write_refresh_cache_epoch.fetch_add(
      1, std::memory_order_acq_rel);
  mylite_ownerless_innodb_lock_hooks_enabled.store(false, std::memory_order_release);
  acquire_table_callback.store(nullptr, std::memory_order_release);
  release_table_callback.store(nullptr, std::memory_order_release);
  wait_table_callback.store(nullptr, std::memory_order_release);
  wait_until_table_callback.store(nullptr, std::memory_order_release);
  acquire_record_callback.store(nullptr, std::memory_order_release);
  release_record_callback.store(nullptr, std::memory_order_release);
  acquire_page_write_callback.store(nullptr, std::memory_order_release);
  release_page_write_callback.store(nullptr, std::memory_order_release);
  release_page_writes_callback.store(nullptr, std::memory_order_release);
  wait_record_callback.store(nullptr, std::memory_order_release);
  wait_until_record_callback.store(nullptr, std::memory_order_release);
  before_record_wait_callback.store(nullptr, std::memory_order_release);
  clear_wait_callback.store(nullptr, std::memory_order_release);
  redo_enter_callback.store(nullptr, std::memory_order_release);
  redo_observe_callback.store(nullptr, std::memory_order_release);
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
  skip_external_page_refresh_callback.store(nullptr, std::memory_order_release);
  mylite_ownerless_innodb_autoinc_reset_hooks();
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

extern "C" void mylite_ownerless_innodb_set_checkpoint_suppression(int suppressed)
{
  checkpoint_suppressed.store(suppressed != 0, std::memory_order_release);
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

extern "C" void mylite_ownerless_innodb_test_fault(const char *fault_name)
{
  if (!mylite_ownerless_innodb_test_faults_enabled.load(
          std::memory_order_acquire) ||
      fault_name == nullptr)
    return;

  const char *configured_fault= std::getenv("MYLITE_OWNERLESS_TEST_FAULT");
  if (configured_fault == nullptr || std::strcmp(configured_fault, fault_name))
    return;

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
    pause();
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              table->id,
              normalized_lock_mode(mode),
              timeout_ms,
              context);
}

extern "C" int mylite_ownerless_innodb_lock_acquire_autoinc(
    trx_t *trx,
    const dict_table_t *table,
    unsigned int timeout_ms)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              table->id,
              MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC,
              timeout_ms,
              context);
}

extern "C" void mylite_ownerless_innodb_lock_release_autoinc(
    trx_t *trx,
    const dict_table_t *table)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  mylite_ownerless_innodb_lock_release_table_callback hook=
      release_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= transaction_lock_id(trx, false);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id,
                         table->id,
                         MYLITE_OWNERLESS_INNODB_LOCK_MODE_AUTO_INC,
                         context);
  handle_hook_result("release autoinc", result);
}

extern "C" void mylite_ownerless_innodb_lock_publish_table(
    const ib_lock_t *lock)
{
  if (!table_lock_publishable(lock))
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  mylite_ownerless_innodb_lock_acquire_table_callback hook=
      acquire_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= lock_transaction_id(lock, true);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id,
                         lock->un_member.tab_lock.table->id,
                         normalized_lock_mode(lock),
                         0U,
                         context);
  handle_hook_result("acquire table", result);
}

extern "C" void mylite_ownerless_innodb_lock_release_table(
    const ib_lock_t *lock)
{
  if (!table_lock_publishable(lock))
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  mylite_ownerless_innodb_lock_release_table_callback hook=
      release_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= lock_transaction_id(lock, false);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id,
                         lock->un_member.tab_lock.table->id,
                         normalized_lock_mode(lock),
                         context);
  handle_hook_result("release table", result);
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_wait_table_callback hook=
      wait_table_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= lock_transaction_id(wait_lock, true);
  const trx_id_t blocker_trx_id= lock_transaction_id(blocker_lock, true);
  if (trx_id == 0 || blocker_trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              wait_lock->un_member.tab_lock.table->id,
              normalized_lock_mode(wait_lock),
              blocker_trx_id,
              context);
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
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  if (snapshot->kind == MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE)
  {
    mylite_ownerless_innodb_lock_wait_until_table_callback hook=
        wait_until_table_callback.load(std::memory_order_acquire);
    if (hook == nullptr)
      return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

    return hook(snapshot->trx_id,
                snapshot->table_id,
                snapshot->mode,
                timeout_ms,
                context);
  }

  if (snapshot->kind != MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  mylite_ownerless_innodb_lock_wait_until_record_callback hook=
      wait_until_record_callback.load(std::memory_order_acquire);
  if (hook == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  return hook(snapshot->trx_id,
              snapshot->index_id,
              snapshot->space_id,
              snapshot->page_no,
              snapshot->heap_no,
              snapshot->mode,
              snapshot->flags,
              timeout_ms,
              context);
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              index->id,
              space_id,
              page_no,
              heap_no,
              normalized_lock_mode(type_mode),
              record_lock_flags(type_mode, heap_no),
              timeout_ms,
              context);
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_wait_until_record_callback hook=
      wait_until_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              index->id,
              space_id,
              page_no,
              heap_no,
              normalized_lock_mode(type_mode),
              record_lock_flags(type_mode, heap_no),
              timeout_ms,
              context);
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_before_record_wait_callback hook=
      before_record_wait_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= transaction_lock_id(trx, true);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              index->id,
              space_id,
              page_no,
              heap_no,
              normalized_lock_mode(type_mode),
              record_lock_flags(type_mode, heap_no),
              context);
}

extern "C" void mylite_ownerless_innodb_lock_publish_record_bit(
    const ib_lock_t *lock,
    uint32_t heap_no)
{
  if (!ownerless_lock_hooks_enabled())
    return;
  if (!record_lock_publishable(lock) || !record_bit_set(lock, heap_no))
    return;

  mylite_ownerless_innodb_lock_acquire_record_callback hook=
      acquire_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= lock_transaction_id(lock, true);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id,
                         lock->index->id,
                         lock->un_member.rec_lock.page_id.space(),
                         lock->un_member.rec_lock.page_id.page_no(),
                         heap_no,
                         normalized_lock_mode(lock),
                         record_lock_flags(lock, heap_no),
                         0U,
                         context);
  handle_hook_result("acquire record", result);
}

extern "C" void mylite_ownerless_innodb_lock_publish_record_bits(
    const ib_lock_t *lock)
{
  if (!ownerless_lock_hooks_enabled())
    return;
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
    return;
  if (!record_lock_publishable(lock))
    return;

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= lock_transaction_id(lock, false);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id,
                         lock->index->id,
                         lock->un_member.rec_lock.page_id.space(),
                         lock->un_member.rec_lock.page_id.page_no(),
                         heap_no,
                         normalized_lock_mode(lock),
                         record_lock_flags(lock, heap_no),
                         context);
  handle_hook_result("release record", result);
}

extern "C" void mylite_ownerless_innodb_lock_release_record_bits(
    const ib_lock_t *lock)
{
  if (!ownerless_lock_hooks_enabled())
    return;
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
    bool track_transaction_page)
{
  if (out_acquire_flags != nullptr)
    *out_acquire_flags= 0U;
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_acquire_page_write_callback hook=
      acquire_page_write_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= page_write_transaction_id(trx);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  const int result= hook(trx_id,
                         MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
                         space_id,
                         page_no,
                         MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
                         MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
                         0U,
                         timeout_ms,
                         out_acquire_flags,
                         context);
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
      trx, space_id, page_no, timeout_ms, out_acquire_flags, true);
}

extern "C" int mylite_ownerless_innodb_lock_acquire_page_write_untracked(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no,
    unsigned int timeout_ms,
    uint32_t *out_acquire_flags)
{
  return mylite_ownerless_innodb_lock_acquire_page_write_low(
      trx, space_id, page_no, timeout_ms, out_acquire_flags, false);
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
  if (trx == nullptr || space_id >= SRV_TMP_SPACE_ID ||
      space_id == TRX_SYS_SPACE || srv_is_undo_tablespace(space_id))
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

extern "C" int mylite_ownerless_innodb_lock_release_page_write(
    trx_t *trx,
    uint32_t space_id,
    uint32_t page_no)
{
  if (space_id >= SRV_TMP_SPACE_ID)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_lock_release_record_callback hook=
      release_page_write_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= page_write_transaction_id(trx);
  if (trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  return hook(trx_id,
              MYLITE_OWNERLESS_INNODB_PAGE_WRITE_INDEX_ID,
              space_id,
              page_no,
              MYLITE_OWNERLESS_INNODB_PAGE_WRITE_HEAP_NO,
              MYLITE_OWNERLESS_INNODB_LOCK_MODE_X,
              0U,
              context);
}

extern "C" void mylite_ownerless_innodb_lock_release_transaction_page_writes(
    trx_t *trx)
{
  if (trx == nullptr)
    return;
  trx->mylite_ownerless_native_support_page_write_pages_clear();
  if (!ownerless_lock_hooks_enabled())
    return;

  const trx_id_t page_write_trx_id= trx->mylite_ownerless_page_write_trx_id;
  if (page_write_trx_id != 0)
    release_transaction_page_writes(page_write_trx_id);

  const trx_id_t trx_id= transaction_lock_id(trx);
  if (trx_id != 0 && trx_id != page_write_trx_id)
    release_transaction_page_writes(trx_id);
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

  for (uint64_t packed_page : *pages)
  {
    if (!packed_page_write_transaction_gate(packed_page))
      continue;
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    const int result= mylite_ownerless_innodb_lock_release_page_write(
        trx, space_id, page_no);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      handle_hook_result("release page-write gate", result);
  }
  pages->erase(std::remove_if(pages->begin(), pages->end(),
                              packed_page_write_transaction_gate),
               pages->end());
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
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const ulint heap_no= lock_rec_find_set_bit(wait_lock);
  if (heap_no == ULINT_UNDEFINED)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  mylite_ownerless_innodb_lock_wait_record_callback hook=
      wait_record_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  const trx_id_t trx_id= lock_transaction_id(wait_lock, true);
  const trx_id_t blocker_trx_id= lock_transaction_id(blocker_lock, true);
  if (trx_id == 0 || blocker_trx_id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  return hook(trx_id,
              wait_lock->index->id,
              wait_lock->un_member.rec_lock.page_id.space(),
              wait_lock->un_member.rec_lock.page_id.page_no(),
              static_cast<uint32_t>(heap_no),
              normalized_lock_mode(wait_lock),
              record_lock_flags(wait_lock, static_cast<uint32_t>(heap_no)),
              blocker_trx_id,
              context);
}

extern "C" void mylite_ownerless_innodb_lock_clear_transaction_wait(trx_t *trx)
{
  if (trx == nullptr)
    return;

  const trx_id_t page_write_trx_id= trx->mylite_ownerless_page_write_trx_id;
  if (page_write_trx_id != 0)
    clear_transaction_wait(page_write_trx_id);

  const trx_id_t trx_id= transaction_lock_id(trx);
  if (trx_id != 0 && trx_id != page_write_trx_id)
    clear_transaction_wait(trx_id);
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
    mylite_ownerless_innodb_lock_release_transaction_page_writes(trx);
    trx->mylite_ownerless_page_write_trx_id= 0;
    trx->mylite_ownerless_lock_trx_id= 0;
  }
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
mylite_ownerless_innodb_set_statement_deferred_page_publish(int enabled)
{
  if (enabled == 0 && mylite_ownerless_statement_deferred_page_publish)
  {
    const int flush_result= mylite_ownerless_innodb_redo_flush_deferred();
    if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      ut_error;
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
      mylite_ownerless_statement_plain_read_preserve_local_pages ||
      mylite_ownerless_statement_plain_read_pages_refreshed)
    return;

  const uint64_t visible_lsn= mylite_ownerless_innodb_external_page_visibility();
  if (visible_lsn == 0)
    return;

  mylite_ownerless_statement_plain_read_pages_refreshed= true;
  mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_visible_boundary_no_skip(
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
      if (packed_page_write_transaction_gate(packed_page))
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
    if (packed_page_write_transaction_gate(packed_page))
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

extern "C" uint64_t mylite_ownerless_innodb_publish_transaction_pages_to_lsn(
    trx_t *trx, uint64_t visible_lsn)
{
  if (trx != nullptr)
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
  if (images != nullptr)
  {
    successful_image_pages.reserve(images->size());
    for (trx_t::mylite_ownerless_page_image &image : *images)
    {
      if (image.page_lsn == 0 || image.page_size == 0 ||
          image.page.size() != image.page_size)
        continue;

      const uint32_t space_id= static_cast<uint32_t>(image.packed_page >> 32);
      const uint32_t page_no= static_cast<uint32_t>(image.packed_page);
      fil_space_t *space= fil_space_t::get(space_id);
      const bool full_crc32= space != nullptr && space->full_crc32();
      if (space != nullptr)
        space->release();

      /*
      The captured image is private to this transaction and will be cleared
      during transaction cleanup. Prepare it in place instead of allocating a
      second page-sized buffer for the publish call.
      */
      byte *page= image.page.data();
      if (image.compressed)
        buf_flush_update_zip_checksum(page, image.page_size);
      else
        buf_flush_init_for_writing(nullptr, page, nullptr, full_crc32);

      const uint64_t publish_lsn=
          std::max<uint64_t>(visible_lsn, image.page_lsn);
      mylite_ownerless_innodb_deep_perf_count(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_IMAGE_ATTEMPTS);
      const int result= mylite_ownerless_innodb_publish_page_version(
          space_id, page_no, image.page_lsn, publish_lsn, page,
          image.page_size);
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      {
        mylite_ownerless_innodb_deep_perf_count(
            MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_IMAGE_PUBLISHED);
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
  for (uint64_t packed_page : pages)
  {
    if (!successful_image_pages.empty() &&
        std::binary_search(successful_image_pages.begin(),
                           successful_image_pages.end(), packed_page))
      continue;

    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    uint64_t published_pages= 0;
    mylite_ownerless_innodb_deep_perf_count(
        MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_ATTEMPTS);
    const lsn_t observed_lsn= buf_flush_publish_ownerless_page_to_lsn(
        space_id, page_no, static_cast<lsn_t>(visible_lsn), false,
        &published_pages);
    if (published_pages != 0)
    {
      mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_PUBLISHED,
          published_pages);
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
          space_id, page_no, observed_lsn, false, &published_pages);
      if (published_pages != 0)
      {
        mylite_ownerless_innodb_deep_perf_add(
            MYLITE_OWNERLESS_INNODB_DEEP_PAGE_PUBLISH_TRANSACTION_BUFFER_RETRY_PUBLISHED,
            published_pages);
        successful_image_pages.push_back(packed_page);
      }
      if (second_observed_lsn > maximum_observed_lsn)
        maximum_observed_lsn= second_observed_lsn;
    }
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

  publish_pages_visible_lsn(visible_lsn);
}

extern "C" void mylite_ownerless_innodb_publish_pages_visible_lsn(
    uint64_t visible_lsn)
{
  publish_pages_visible_lsn(visible_lsn);
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
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
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
mylite_ownerless_innodb_refresh_buffer_pool_pages_force_current_read_retained_no_skip(
    uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || visible_lsn == 0)
    return;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  const bool previous_current= page_visible_lsn_is_current;
  page_visible_lsn= visible_lsn;
  page_visible_lsn_is_current= true;
  refresh_buffer_pool_pages(true, true, true, true, true);
  page_visible_lsn= previous_visible_lsn;
  page_visible_lsn_is_current= previous_current;
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

extern "C" int mylite_ownerless_innodb_refresh_page_for_read(
    uint32_t space_id, uint32_t page_no, uint64_t visible_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  if (visible_lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= visible_lsn;
  refresh_buffer_pool_page(space_id, page_no, false);
  page_visible_lsn= previous_visible_lsn;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" void mylite_ownerless_innodb_evict_clean_external_pages(void)
{
  if (!srv_was_started)
    return;

  refresh_replaceable_buffer_pool_pages();
}

extern "C" int mylite_ownerless_innodb_advance_external_lsn(uint64_t latest_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  advance_external_lsn(latest_lsn);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
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

extern "C" void mylite_ownerless_innodb_refresh_external_space_allocation(
    uint32_t space_id)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;
  if (mylite_ownerless_statement_dictionary_ddl ||
      mylite_ownerless_statement_suppress_native_lifecycle_refresh)
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  uint64_t previous_visible_lsn= page_visible_lsn;
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    advance_external_lsn(latest_lsn);
    page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  }
  else if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    return;
  }

  refresh_external_space_header(space_id);
  refresh_external_space_allocation_pages(space_id);
  page_visible_lsn= previous_visible_lsn;
}

extern "C" void mylite_ownerless_innodb_refresh_external_space_headers(void)
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (ownerless_skip_external_page_refresh())
    return;

  refresh_external_space_headers();
}

extern "C" void mylite_ownerless_innodb_evict_dictionary_cache(void)
{
  if (!dict_sys.is_initialised())
    return;

  dict_sys.lock(SRW_LOCK_CALL);
  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    table->mylite_ownerless_referenced_foreigns_loaded= false;
    if (table_can_be_evicted_from_dictionary(table))
      dict_sys.remove(table, true);
    table= prev;
  }
  for (dict_table_t *table= UT_LIST_GET_LAST(dict_sys.table_non_LRU); table;)
  {
    dict_table_t *prev= UT_LIST_GET_PREV(table_LRU, table);
    table->mylite_ownerless_referenced_foreigns_loaded= false;
    if (foreign_table_can_be_reloaded_from_dictionary(table))
      dict_sys.remove(table, false);
    table= prev;
  }
  dict_sys.unlock();
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
  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    advance_external_lsn(latest_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
    const int refresh_result= refresh_page_for_write(*block, true, false);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return refresh_page_for_write(*block, true, false);

  return refresh_page_for_write(*block, false, false);
}

extern "C" int mylite_ownerless_innodb_refresh_page_for_write_force(
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

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    advance_external_lsn(latest_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
    const int refresh_result= refresh_page_for_write(*block, true, true, true);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return refresh_page_for_write(*block, true, true, true);

  return refresh_page_for_write(*block, false, true, true);
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

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    advance_external_lsn(latest_lsn);
    const uint64_t previous_visible_lsn= page_visible_lsn;
    page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
    const int refresh_result=
        refresh_page_for_write(*block, true, true, true);
    page_visible_lsn= previous_visible_lsn;
    return refresh_result;
  }
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return refresh_page_for_write(*block, true, true, true);

  return refresh_page_for_write(*block, false, true, true);
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

  advance_external_lsn(latest_lsn);
  const uint64_t previous_visible_lsn= page_visible_lsn;
  page_visible_lsn= std::max(page_visible_lsn, latest_lsn);
  buf_flush_sync_batch(static_cast<lsn_t>(latest_lsn));
  refresh_replaceable_buffer_pool_pages();
  refresh_buffer_pool_page(snapshot->space_id, snapshot->page_no, true, true);
  page_visible_lsn= previous_visible_lsn;
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" void mylite_ownerless_innodb_enable_external_page_visibility(
    uint64_t latest_lsn)
{
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= false;
  page_visible_lsn_is_retained= false;
}

extern "C" void
mylite_ownerless_innodb_enable_current_external_page_visibility(
    uint64_t latest_lsn)
{
  page_visible_lsn= latest_lsn;
  page_visible_lsn_is_current= true;
  page_visible_lsn_is_retained= false;
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
}

extern "C" int mylite_ownerless_innodb_retained_external_page_visibility(void)
{
  return page_visible_lsn_is_retained ? 1 : 0;
}

extern "C" void mylite_ownerless_innodb_set_external_page_observation_token(
    uint64_t token)
{
  ownerless_external_page_observation_token= token;
}

extern "C" void mylite_ownerless_innodb_clear_external_page_observations(void)
{
  memset(ownerless_external_page_observations, 0,
         sizeof(ownerless_external_page_observations));
}

extern "C" void mylite_ownerless_innodb_note_external_page_observed(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn)
{
  if (commit_lsn == 0 || ownerless_external_page_observation_token == 0)
    return;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return;

  const size_t slot= ownerless_external_page_observation_slot(space_id, page_no);
  ownerless_external_page_observation_entry &entry=
      ownerless_external_page_observations[slot];
  if (entry.token == ownerless_external_page_observation_token &&
      entry.context == context && entry.space_id == space_id &&
      entry.page_no == page_no)
  {
    entry.commit_lsn= std::max(entry.commit_lsn, commit_lsn);
    return;
  }

  entry.token= ownerless_external_page_observation_token;
  entry.context= context;
  entry.space_id= space_id;
  entry.page_no= page_no;
  entry.commit_lsn= commit_lsn;
}

extern "C" int mylite_ownerless_innodb_external_page_observed_at_or_after(
    uint32_t space_id, uint32_t page_no, uint64_t commit_lsn)
{
  if (commit_lsn == 0 || ownerless_external_page_observation_token == 0)
    return 0;

  void *context= callback_context.load(std::memory_order_acquire);
  if (context == nullptr)
    return 0;

  const size_t slot= ownerless_external_page_observation_slot(space_id, page_no);
  const ownerless_external_page_observation_entry &entry=
      ownerless_external_page_observations[slot];
  return entry.token == ownerless_external_page_observation_token &&
                 entry.context == context && entry.space_id == space_id &&
                 entry.page_no == page_no && entry.commit_lsn >= commit_lsn ?
             1 :
             0;
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
  page_visible_lsn= 0;
  page_visible_lsn_is_current= false;
  page_visible_lsn_is_retained= false;
}

extern "C" void mylite_ownerless_innodb_close_current_read_view(void)
{
  trx_t *trx= current_trx();
  if (trx != nullptr)
    trx->read_view.close();
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

extern "C" int mylite_ownerless_innodb_redo_is_active(void)
{
  return ownerless_lock_hooks_enabled() && redo_depth != 0;
}

extern "C" int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

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
  const int result= hook(out_latest_lsn, context);
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

  mylite_ownerless_innodb_redo_observe_callback hook=
      redo_observe_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(out_latest_lsn, context);
}

extern "C" int mylite_ownerless_innodb_redo_reserve(
    uint64_t current_lsn,
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

  mylite_ownerless_innodb_redo_reserve_callback hook=
      redo_reserve_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(current_lsn, length, out_start_lsn, out_end_lsn, context);
}

extern "C" int mylite_ownerless_innodb_redo_written(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t *out_written_lsn)
{
  if (start_lsn == 0 || end_lsn <= start_lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (out_written_lsn != nullptr)
    *out_written_lsn= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_redo_written_callback hook=
      redo_written_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(start_lsn, end_lsn, out_written_lsn, context);
}

extern "C" void mylite_ownerless_innodb_redo_leave(uint64_t latest_lsn)
{
  if (redo_depth == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
  {
    redo_depth--;
    if (redo_depth == 0)
      redo_latest_lsn= 0;
    return;
  }
  if (latest_lsn > redo_latest_lsn)
    redo_latest_lsn= latest_lsn;
  redo_depth--;
  if (redo_depth != 0)
    return;

  latest_lsn= redo_latest_lsn;
  redo_latest_lsn= 0;
  mylite_ownerless_innodb_redo_leave_callback hook=
      redo_leave_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;
  hook(latest_lsn, context);
}

extern "C" int mylite_ownerless_innodb_redo_written_and_leave(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
    uint64_t *out_written_lsn)
{
  if (start_lsn == 0 || end_lsn <= start_lsn)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

  if (out_written_lsn != nullptr)
    *out_written_lsn= 0;

  auto separate_written_then_leave=
      [start_lsn, end_lsn, latest_lsn, out_written_lsn]() -> int
  {
    const int result= mylite_ownerless_innodb_redo_written(
        start_lsn, end_lsn, out_written_lsn);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
        result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      mylite_ownerless_innodb_redo_leave(latest_lsn);
    return result;
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
  const int result= hook(
      start_lsn, end_lsn, redo_latest_lsn, out_written_lsn, context);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return result;

  redo_depth--;
  if (redo_depth == 0)
    redo_latest_lsn= 0;
  return result;
}

extern "C" int mylite_ownerless_innodb_redo_defer_written_and_leave(
    uint64_t start_lsn,
    uint64_t end_lsn,
    uint64_t latest_lsn,
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

  deferred_redo_batch[deferred_redo_batch_count++]=
      {start_lsn, end_lsn, latest_lsn};
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
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size)
{
  return mylite_ownerless_innodb_publish_page_version_with_flags(
      space_id, page_no, page_lsn, visible_lsn, page, page_size, 0U);
}

extern "C" int mylite_ownerless_innodb_publish_page_version_with_flags(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size,
    uint32_t publish_flags)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_page_publish_callback hook=
      page_publish_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(space_id, page_no, page_lsn, visible_lsn, page, page_size,
              publish_flags, context);
}

extern "C" int mylite_ownerless_innodb_publish_history_proof_pair(
    uint32_t space_id,
    uint32_t rseg_page_no,
    uint64_t rseg_page_lsn,
    const void *rseg_page,
    uint32_t rseg_page_size,
    uint32_t undo_page_no,
    uint64_t undo_page_lsn,
    const void *undo_page,
    uint32_t undo_page_size,
    uint64_t visible_lsn)
{
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_history_proof_publish_pair_callback hook=
      history_proof_publish_pair_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(space_id, rseg_page_no, rseg_page_lsn, rseg_page,
              rseg_page_size, undo_page_no, undo_page_lsn, undo_page,
              undo_page_size, visible_lsn, context);
}

extern "C" void mylite_ownerless_innodb_begin_page_publish_batch(void)
{
  if (!ownerless_lock_hooks_enabled())
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
  mylite_ownerless_innodb_page_publish_batch_callback hook=
      page_publish_batch_end_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;
  hook(context);
}

extern "C" int mylite_ownerless_innodb_read_page_version_with_metadata(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity,
    uint64_t *out_page_lsn,
    uint64_t *out_commit_lsn,
    uint32_t *out_record_flags)
{
  if (page == nullptr || page_capacity == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  if (out_page_lsn != nullptr)
    *out_page_lsn= 0;
  if (out_commit_lsn != nullptr)
    *out_commit_lsn= 0;
  if (out_record_flags != nullptr)
    *out_record_flags= 0;
  if (!ownerless_lock_hooks_enabled())
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  uint64_t latest_lsn= 0;
  const int observe_result= mylite_ownerless_innodb_redo_observe(&latest_lsn);
  if (observe_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    advance_external_lsn(latest_lsn);
  else if (observe_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return observe_result;

  const uint64_t max_commit_lsn= page_visible_lsn;
  if (max_commit_lsn == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_page_read_callback hook=
      page_read_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  uint32_t page_size= 0;
  uint64_t page_lsn= 0;
  uint64_t commit_lsn= 0;
  uint32_t record_flags= 0;
  const int result= hook(space_id, page_no, max_commit_lsn, page, page_capacity,
                         &page_size, &page_lsn, &commit_lsn, &record_flags,
                         context);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    if (out_page_lsn != nullptr)
      *out_page_lsn= page_lsn;
    if (out_commit_lsn != nullptr)
      *out_commit_lsn= commit_lsn;
    if (out_record_flags != nullptr)
      *out_record_flags= record_flags;
  }
  return result;
}

extern "C" int mylite_ownerless_innodb_read_page_version(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity)
{
  return mylite_ownerless_innodb_read_page_version_with_metadata(
      space_id, page_no, page, page_capacity, nullptr, nullptr, nullptr);
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
    uint64_t next_value)
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

  return hook(table_id, next_value, context);
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

void publish_pages_visible_lsn(uint64_t visible_lsn)
{
  if (visible_lsn == 0 || !ownerless_lock_hooks_enabled())
    return;
  const int flush_result= flush_deferred_redo_batch();
  if (flush_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      flush_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return;
  mylite_ownerless_innodb_pages_visible_callback hook=
      pages_visible_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook != nullptr && context != nullptr)
    hook(visible_lsn, context);
}

int flush_deferred_redo_batch()
{
  if (deferred_redo_batch_count == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

  mylite_ownerless_innodb_redo_written_leave_callback hook=
      redo_written_leave_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;

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
      return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
    }
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        completed_count == count)
    {
      for (size_t i= 0; i < count; ++i)
        deferred_redo_batch[i]= {};
      return MYLITE_OWNERLESS_INNODB_LOCK_OK;
    }

    if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE ||
        completed_count != 0)
    {
      retain_deferred_redo_batch_tail(completed_count, count);
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
        return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
      return result;
    }
  }

  for (size_t i= 0; i < count; ++i)
  {
    uint64_t written_lsn= 0;
    const uint64_t range_latest_lsn= (i + 1 == count) ? latest_lsn : 0;
    const int result= hook(deferred_redo_batch[i].start_lsn,
                           deferred_redo_batch[i].end_lsn,
                           range_latest_lsn, &written_lsn, context);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    {
      retain_deferred_redo_batch_tail(i, count);
      return result;
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

void handle_hook_result(const char *operation, int result)
{
  (void) operation;

  switch (result) {
  case MYLITE_OWNERLESS_INNODB_LOCK_OK:
  case MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE:
  case MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT:
  case MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK:
  case MYLITE_OWNERLESS_INNODB_LOCK_FULL:
    return;
  default:
    ut_error;
  }
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

bool refresh_external_space_header(fil_space_t &space)
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

int refresh_page_for_write(const buf_block_t &block,
                           bool use_current_visibility,
                           bool force_page_version,
                           bool allow_boundary_newer,
                           bool allow_visible_boundary,
                           bool preserve_retained_user_page,
                           bool skip_page_version)
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
      bpage.zip.data != nullptr || bpage.frame == nullptr)
  {
    ownerless_page_write_refresh_count(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_SKIPPED_UNPUBLISHABLE);
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

  const uint32_t page_size= static_cast<uint32_t>(bpage.physical_size());
  byte *local_page= bpage.frame;
  const lsn_t local_lsn= mach_read_from_8(local_page + FIL_PAGE_LSN);
  const uint16_t local_page_type= fil_page_get_type(local_page);
  /* Retained direct reads may preserve user table pages across a lower
  visible boundary; native undo/system/allocation pages must still refresh. */
  const bool retained_user_page=
      preserve_retained_user_page && id.space() > 3 &&
      !srv_is_undo_tablespace(id.space()) &&
      (fil_page_type_is_index(local_page_type) ||
       local_page_type == FIL_PAGE_TYPE_BLOB ||
       local_page_type == FIL_PAGE_TYPE_ZBLOB ||
       local_page_type == FIL_PAGE_TYPE_ZBLOB2 ||
       local_page_type == FIL_PAGE_PAGE_COMPRESSED ||
       local_page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);

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
      return visibility_result;
    }
  }

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
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;
  }

  int result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  mysql_mutex_lock(&fil_system.mutex);
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

    uint64_t page_version_commit_lsn= 0;
    uint32_t page_version_record_flags= 0;
    int page_version_result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
    bool page_version_proved_no_newer= false;
    if (!skip_page_version)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_CALLS);
      const uint64_t page_version_read_start_ns=
          ownerless_page_write_refresh_stats_on() ?
              ownerless_page_write_refresh_now_ns() :
              0;
      page_version_result=
        mylite_ownerless_innodb_read_page_version_with_metadata(
            id.space(), id.page_no(), external_page, page_size,
            nullptr, &page_version_commit_lsn, &page_version_record_flags);
      ownerless_page_write_refresh_add_elapsed(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_READ_NS,
          page_version_read_start_ns);
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
      page_version_result= MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
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
      }
      const bool page_version_already_observed=
          mylite_ownerless_innodb_external_page_observed_at_or_after(
              id.space(), id.page_no(), page_version_commit_lsn) != 0;
      const bool page_version_retained_observed=
          retained_user_page && page_version_already_observed;
      const bool page_version_snapshot_boundary=
          (page_version_record_flags &
           MYLITE_OWNERLESS_INNODB_PAGE_VERSION_SNAPSHOT_BOUNDARY) != 0;
      const bool retained_current_snapshot_boundary=
          retained_user_page && page_visible_lsn_is_current &&
          page_version_snapshot_boundary;
      const bool visible_boundary_allowed=
          allow_visible_boundary && !page_version_retained_observed &&
          !retained_current_snapshot_boundary &&
          page_version_commit_lsn != 0 &&
          page_version_commit_lsn <= page_visible_lsn;
      const bool boundary_newer_than_local=
          allow_boundary_newer && !page_version_retained_observed &&
          !retained_current_snapshot_boundary &&
          page_version_commit_lsn > local_lsn;
      const bool observed_same_lsn_boundary=
          (page_version_already_observed ||
           retained_current_snapshot_boundary) &&
          page_version_lsn == local_lsn &&
          memcmp(external_page, local_page, page_size) != 0;
      if (page_version_lsn < local_lsn &&
          !boundary_newer_than_local && !visible_boundary_allowed)
      {
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
        if (buf_page_is_corrupted(true, external_page, space->flags) !=
            NOT_CORRUPTED)
        {
          ownerless_page_write_refresh_count(
              OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_CHECKSUM_FAILURES);
          goto exit;
        }
        memcpy(local_page, external_page, page_size);
        mylite_ownerless_innodb_note_external_page_observed(
            id.space(), id.page_no(), page_version_commit_lsn);
        ownerless_page_write_refresh_count(
            OWNERLESS_PAGE_WRITE_REFRESH_STAT_PAGE_VERSION_OVERLAYS);
        if (id.page_no() == 0)
        {
          ownerless_page_write_refresh_count(
              OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES);
          static_cast<void>(refresh_external_space_header(*space));
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
      goto exit;
    }
    ownerless_page_write_refresh_add_elapsed(
        OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_READ_NS, disk_read_start_ns);

    const uint32_t read_space_id=
        mach_read_from_4(external_page + FIL_PAGE_SPACE_ID);
    const uint32_t read_page_no=
        mach_read_from_4(external_page + FIL_PAGE_OFFSET);
	    const lsn_t disk_page_lsn= mach_read_from_8(external_page + FIL_PAGE_LSN);
	    if (read_space_id != id.space() || read_page_no != id.page_no())
	    {
	      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_IDENTITY_MISMATCH);
      goto exit;
    }

    const bool disk_page_is_visible=
        page_visible_lsn == 0 || disk_page_lsn <= page_visible_lsn;
    const bool disk_page_newer_and_visible=
        disk_page_lsn > local_lsn && disk_page_is_visible;
    const bool disk_visible_boundary_allowed=
        allow_visible_boundary && disk_page_is_visible && !retained_user_page;
    const bool disk_page_same_lsn_different_image=
        disk_page_lsn == local_lsn && disk_page_is_visible &&
        !retained_user_page &&
        memcmp(external_page, local_page, page_size) != 0;
    if (disk_page_newer_and_visible)
      advance_external_lsn(disk_page_lsn);
    if (buf_page_is_corrupted(true, external_page, space->flags) !=
        NOT_CORRUPTED)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_DISK_CHECKSUM_FAILURES);
      goto exit;
    }

    bool should_store_negative_cache= false;
    if (disk_page_newer_and_visible || disk_page_same_lsn_different_image ||
        disk_visible_boundary_allowed)
    {
      memcpy(local_page, external_page, page_size);
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
    if (id.page_no() == 0 && disk_page_is_visible)
    {
      ownerless_page_write_refresh_count(
          OWNERLESS_PAGE_WRITE_REFRESH_STAT_SPACE_HEADER_REFRESHES);
      static_cast<void>(refresh_external_space_header(*space));
    }
    if (should_store_negative_cache)
      ownerless_page_write_refresh_negative_cache_store(
          id.space(), id.page_no(), local_lsn, page_visible_lsn);
    result= MYLITE_OWNERLESS_INNODB_LOCK_OK;
  }

exit:
  mysql_mutex_unlock(&fil_system.mutex);
  aligned_free(external_page);
  if (!use_current_visibility)
    page_visible_lsn= previous_visible_lsn;
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
                               bool skip_page_version)
{
  std::vector<uint64_t> pages;
  collect_buffer_pool_file_pages(pages);

  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  for (uint64_t packed_page : pages)
  {
    const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
    const uint32_t page_no= static_cast<uint32_t>(packed_page);
    refresh_buffer_pool_page(space_id, page_no, false, force_page_version,
                             evict_clean_pages, allow_boundary_newer,
                             allow_visible_boundary,
                             preserve_retained_user_page,
                             skip_page_version);
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

void refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no,
                              bool load_if_missing, bool force_page_version,
                              bool evict_clean_page, bool allow_boundary_newer,
                              bool allow_visible_boundary,
                              bool preserve_retained_user_page,
                              bool skip_page_version)
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

  mtr_t mtr(nullptr);
  mtr.start();
  dberr_t err= DB_SUCCESS;
  const ulint get_mode= (load_if_missing || evicted_clean_page)
      ? BUF_GET
      : BUF_GET_IF_IN_POOL;
  if (buf_block_t *block= buf_page_get_gen(id, 0, RW_X_LATCH, nullptr,
                                           get_mode, &mtr, &err))
  {
    if (force_page_version || block->page.oldest_modification_acquire() == 0)
      static_cast<void>(refresh_page_for_write(
          *block, force_page_version, force_page_version,
          allow_boundary_newer, allow_visible_boundary,
          preserve_retained_user_page, skip_page_version));
  }
  mtr.commit();
}

bool record_bit_set(const ib_lock_t *lock, uint32_t heap_no)
{
  return heap_no < lock_rec_get_n_bits(lock) &&
         lock_rec_get_nth_bit(lock, heap_no) != 0;
}

void clear_transaction_wait(trx_id_t trx_id)
{
  if (trx_id == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  mylite_ownerless_innodb_lock_clear_wait_callback hook=
      clear_wait_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const int result= hook(trx_id, context);
  handle_hook_result("clear wait", result);
}

void release_transaction_page_writes(trx_id_t trx_id)
{
  if (trx_id == 0)
    return;
  if (!ownerless_lock_hooks_enabled())
    return;

  mylite_ownerless_innodb_lock_release_page_writes_callback hook=
      release_page_writes_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const int result= hook(trx_id, context);
  handle_hook_result("release page writes", result);
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

  const trx_id_t transient_id=
      k_transient_lock_trx_id_flag |
      next_transient_lock_trx_id.fetch_add(1, std::memory_order_relaxed);
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

  const trx_id_t transient_id=
      k_transient_lock_trx_id_flag |
      next_transient_lock_trx_id.fetch_add(1, std::memory_order_relaxed);
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
    page_write_lock_trx_id=
        k_transient_lock_trx_id_flag |
        next_transient_lock_trx_id.fetch_add(1, std::memory_order_relaxed);
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
  bool has_other_space_gate= false;
  if (pages != nullptr)
    for (uint64_t packed_page : *pages)
    {
      if (!packed_page_write_transaction_gate(packed_page))
        continue;
      if (packed_page == global_gate)
        return global_gate;
      if (packed_page == space_gate)
        return space_gate;
      has_other_space_gate= true;
    }

  if (has_other_space_gate)
    return global_gate;

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
          page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO);
}

bool transaction_sql_is_plain_select(const trx_t *trx)
{
  THD *thd= trx != nullptr ? trx->mysql_thd : nullptr;
  if (thd == nullptr || thd->lex == nullptr ||
      thd->lex->sql_command != SQLCOM_SELECT)
    return false;
  if (trx->will_lock)
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
      packed_page_write_transaction_gate(page_write_pack(space_id, page_no)))
    return false;
  if (trx->read_only || trx->dict_operation)
    return false;
  if (trx->mysql_thd == nullptr && trx->undo_no == 0 &&
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
