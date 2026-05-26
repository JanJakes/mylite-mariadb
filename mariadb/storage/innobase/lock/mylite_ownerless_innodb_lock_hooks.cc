#define LOCK_MODULE_IMPLEMENTATION

#include "mylite_ownerless_innodb_lock_hooks.h"

#include "buf0flu.h"
#include "buf0buf.h"
#include "buf0lru.h"
#include "dict0mem.h"
#include "lock0lock.h"
#include "lock0priv.h"
#include "log0log.h"
#include "page0page.h"

#include <atomic>

namespace {

constexpr trx_id_t k_transient_lock_trx_id_flag =
    trx_id_t{1} << ((sizeof(trx_id_t) * 8) - 1);

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
std::atomic<mylite_ownerless_innodb_lock_wait_record_callback>
    wait_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_wait_until_record_callback>
    wait_until_record_callback{nullptr};
std::atomic<mylite_ownerless_innodb_lock_clear_wait_callback>
    clear_wait_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_enter_callback>
    redo_enter_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_reserve_callback>
    redo_reserve_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_written_callback>
    redo_written_callback{nullptr};
std::atomic<mylite_ownerless_innodb_redo_leave_callback>
    redo_leave_callback{nullptr};
std::atomic<mylite_ownerless_innodb_pages_visible_callback>
    pages_visible_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_publish_callback>
    page_publish_callback{nullptr};
std::atomic<mylite_ownerless_innodb_page_read_callback>
    page_read_callback{nullptr};
std::atomic<void *> callback_context{nullptr};
std::atomic<trx_id_t> next_transient_lock_trx_id{1};
thread_local uint64_t page_visible_lsn= 0;
thread_local unsigned redo_depth= 0;
thread_local uint64_t redo_latest_lsn= 0;

void handle_hook_result(const char *operation, int result);
bool lock_publishable(const ib_lock_t *lock);
bool table_lock_publishable(const ib_lock_t *lock);
bool record_lock_publishable(const ib_lock_t *lock);
bool wait_lock_publishable(const ib_lock_t *lock);
bool blocker_lock_publishable(const ib_lock_t *lock);
void advance_external_lsn(uint64_t latest_lsn);
void refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no);
void refresh_replaceable_buffer_pool_pages();
bool record_bit_set(const ib_lock_t *lock, uint32_t heap_no);
trx_id_t lock_transaction_id(const ib_lock_t *lock, bool create_transient);
trx_id_t transaction_lock_id(trx_t *trx, bool create_transient);
trx_id_t transaction_lock_id(const trx_t *trx);
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
    mylite_ownerless_innodb_lock_wait_record_callback wait_record_hook,
    mylite_ownerless_innodb_lock_wait_until_table_callback wait_until_table_hook,
    mylite_ownerless_innodb_lock_wait_until_record_callback wait_until_record_hook,
    mylite_ownerless_innodb_lock_clear_wait_callback clear_wait_hook,
    mylite_ownerless_innodb_redo_enter_callback redo_enter_hook,
    mylite_ownerless_innodb_redo_reserve_callback redo_reserve_hook,
    mylite_ownerless_innodb_redo_written_callback redo_written_hook,
    mylite_ownerless_innodb_redo_leave_callback redo_leave_hook,
    mylite_ownerless_innodb_pages_visible_callback pages_visible_hook,
    mylite_ownerless_innodb_page_publish_callback page_publish_hook,
    mylite_ownerless_innodb_page_read_callback page_read_hook,
    void *context)
{
  if (acquire_table_hook == nullptr || release_table_hook == nullptr ||
      wait_table_hook == nullptr || acquire_record_hook == nullptr ||
      release_record_hook == nullptr || wait_record_hook == nullptr ||
      wait_until_table_hook == nullptr || wait_until_record_hook == nullptr ||
      clear_wait_hook == nullptr || redo_enter_hook == nullptr ||
      redo_reserve_hook == nullptr || redo_written_hook == nullptr ||
      redo_leave_hook == nullptr || pages_visible_hook == nullptr ||
      page_publish_hook == nullptr || page_read_hook == nullptr ||
      context == nullptr)
  {
    mylite_ownerless_innodb_lock_reset_hooks();
    return;
  }

  callback_context.store(context, std::memory_order_release);
  page_read_callback.store(page_read_hook, std::memory_order_release);
  page_publish_callback.store(page_publish_hook, std::memory_order_release);
  pages_visible_callback.store(pages_visible_hook, std::memory_order_release);
  redo_leave_callback.store(redo_leave_hook, std::memory_order_release);
  redo_written_callback.store(redo_written_hook, std::memory_order_release);
  redo_reserve_callback.store(redo_reserve_hook, std::memory_order_release);
  redo_enter_callback.store(redo_enter_hook, std::memory_order_release);
  clear_wait_callback.store(clear_wait_hook, std::memory_order_release);
  wait_until_record_callback.store(wait_until_record_hook, std::memory_order_release);
  wait_record_callback.store(wait_record_hook, std::memory_order_release);
  release_record_callback.store(release_record_hook, std::memory_order_release);
  acquire_record_callback.store(acquire_record_hook, std::memory_order_release);
  wait_until_table_callback.store(wait_until_table_hook, std::memory_order_release);
  wait_table_callback.store(wait_table_hook, std::memory_order_release);
  release_table_callback.store(release_table_hook, std::memory_order_release);
  acquire_table_callback.store(acquire_table_hook, std::memory_order_release);
}

extern "C" void mylite_ownerless_innodb_lock_reset_hooks(void)
{
  acquire_table_callback.store(nullptr, std::memory_order_release);
  release_table_callback.store(nullptr, std::memory_order_release);
  wait_table_callback.store(nullptr, std::memory_order_release);
  wait_until_table_callback.store(nullptr, std::memory_order_release);
  acquire_record_callback.store(nullptr, std::memory_order_release);
  release_record_callback.store(nullptr, std::memory_order_release);
  wait_record_callback.store(nullptr, std::memory_order_release);
  wait_until_record_callback.store(nullptr, std::memory_order_release);
  clear_wait_callback.store(nullptr, std::memory_order_release);
  redo_enter_callback.store(nullptr, std::memory_order_release);
  redo_reserve_callback.store(nullptr, std::memory_order_release);
  redo_written_callback.store(nullptr, std::memory_order_release);
  redo_leave_callback.store(nullptr, std::memory_order_release);
  pages_visible_callback.store(nullptr, std::memory_order_release);
  page_publish_callback.store(nullptr, std::memory_order_release);
  page_read_callback.store(nullptr, std::memory_order_release);
  page_visible_lsn= 0;
  callback_context.store(nullptr, std::memory_order_release);
}

extern "C" int mylite_ownerless_innodb_lock_has_hooks(void)
{
  return acquire_table_callback.load(std::memory_order_acquire) != nullptr &&
         release_table_callback.load(std::memory_order_acquire) != nullptr &&
         wait_table_callback.load(std::memory_order_acquire) != nullptr &&
         wait_until_table_callback.load(std::memory_order_acquire) != nullptr &&
         acquire_record_callback.load(std::memory_order_acquire) != nullptr &&
         release_record_callback.load(std::memory_order_acquire) != nullptr &&
         wait_record_callback.load(std::memory_order_acquire) != nullptr &&
         wait_until_record_callback.load(std::memory_order_acquire) != nullptr &&
         clear_wait_callback.load(std::memory_order_acquire) != nullptr &&
         redo_enter_callback.load(std::memory_order_acquire) != nullptr &&
         redo_reserve_callback.load(std::memory_order_acquire) != nullptr &&
         redo_written_callback.load(std::memory_order_acquire) != nullptr &&
         redo_leave_callback.load(std::memory_order_acquire) != nullptr &&
         pages_visible_callback.load(std::memory_order_acquire) != nullptr &&
         page_publish_callback.load(std::memory_order_acquire) != nullptr &&
         page_read_callback.load(std::memory_order_acquire) != nullptr &&
         callback_context.load(std::memory_order_acquire) != nullptr;
}

extern "C" int mylite_ownerless_innodb_lock_reserve_table(
    trx_t *trx,
    const dict_table_t *table,
    uint32_t mode,
    unsigned int timeout_ms)
{
  if (trx == nullptr || table == nullptr || table->id == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

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

extern "C" void mylite_ownerless_innodb_lock_publish_table(
    const ib_lock_t *lock)
{
  if (!table_lock_publishable(lock))
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

extern "C" void mylite_ownerless_innodb_lock_publish_record_bit(
    const ib_lock_t *lock,
    uint32_t heap_no)
{
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
  if (!record_lock_publishable(lock))
    return;

  const uint32_t n_bits= static_cast<uint32_t>(lock_rec_get_n_bits(lock));
  for (uint32_t heap_no= 0; heap_no < n_bits; heap_no++)
  {
    if (record_bit_set(lock, heap_no))
      mylite_ownerless_innodb_lock_release_record_bit(lock, heap_no);
  }
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

  mylite_ownerless_innodb_lock_clear_wait_callback hook=
      clear_wait_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return;

  const trx_id_t trx_id= transaction_lock_id(trx);
  if (trx_id == 0)
    return;

  const int result= hook(trx_id, context);
  handle_hook_result("clear wait", result);
}

extern "C" void mylite_ownerless_innodb_lock_forget_transaction(trx_t *trx)
{
  if (trx != nullptr)
  {
    mylite_ownerless_innodb_lock_clear_transaction_wait(trx);
    trx->mylite_ownerless_lock_trx_id= 0;
  }
}

extern "C" void mylite_ownerless_innodb_flush_dirty_pages_to_lsn(uint64_t visible_lsn)
{
  if (visible_lsn != 0)
    buf_flush_publish_ownerless_pages_to_lsn(static_cast<lsn_t>(visible_lsn));

  if (visible_lsn == 0)
    buf_flush_sync();
  else
    buf_flush_wait_flushed(static_cast<lsn_t>(visible_lsn));

  if (visible_lsn == 0)
    return;

  mylite_ownerless_innodb_pages_visible_callback hook=
      pages_visible_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook != nullptr && context != nullptr)
    hook(visible_lsn, context);
}

extern "C" void mylite_ownerless_innodb_refresh_external_pages(uint64_t latest_lsn)
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || latest_lsn == 0)
    return;

  advance_external_lsn(latest_lsn);
  buf_flush_sync_batch(static_cast<lsn_t>(latest_lsn));
  refresh_replaceable_buffer_pool_pages();
}

extern "C" int mylite_ownerless_innodb_refresh_external_wait_page(
    const mylite_ownerless_innodb_lock_external_wait *snapshot)
{
  if (snapshot == nullptr ||
      snapshot->kind != MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_RECORD)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;

  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_enter(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return result;

  mylite_ownerless_innodb_redo_leave(latest_lsn);
  advance_external_lsn(latest_lsn);
  refresh_buffer_pool_page(snapshot->space_id, snapshot->page_no);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" void mylite_ownerless_innodb_enable_external_page_visibility(
    uint64_t latest_lsn)
{
  page_visible_lsn= latest_lsn;
}

extern "C" void mylite_ownerless_innodb_clear_external_page_visibility(void)
{
  page_visible_lsn= 0;
}

extern "C" int mylite_ownerless_innodb_refresh_to_latest_external_lsn(void)
{
  uint64_t latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_enter(&latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    return MYLITE_OWNERLESS_INNODB_LOCK_OK;
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    return result;

  mylite_ownerless_innodb_redo_leave(latest_lsn);
  mylite_ownerless_innodb_refresh_external_pages(latest_lsn);
  return MYLITE_OWNERLESS_INNODB_LOCK_OK;
}

extern "C" int mylite_ownerless_innodb_redo_is_active(void)
{
  return redo_depth != 0;
}

extern "C" int mylite_ownerless_innodb_redo_enter(uint64_t *out_latest_lsn)
{
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

extern "C" int mylite_ownerless_innodb_publish_page_version(
    uint32_t space_id,
    uint32_t page_no,
    uint64_t page_lsn,
    uint64_t visible_lsn,
    const void *page,
    uint32_t page_size)
{
  mylite_ownerless_innodb_page_publish_callback hook=
      page_publish_callback.load(std::memory_order_acquire);
  void *context= callback_context.load(std::memory_order_acquire);
  if (hook == nullptr || context == nullptr)
    return MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE;
  return hook(space_id, page_no, page_lsn, visible_lsn, page, page_size,
              context);
}

extern "C" int mylite_ownerless_innodb_read_page_version(
    uint32_t space_id,
    uint32_t page_no,
    void *page,
    uint32_t page_capacity)
{
  if (page == nullptr || page_capacity == 0)
    return MYLITE_OWNERLESS_INNODB_LOCK_ERROR;

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
  return hook(space_id, page_no, max_commit_lsn, page, page_capacity,
              &page_size, &page_lsn, &commit_lsn, context);
}

namespace {

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

void advance_external_lsn(uint64_t latest_lsn)
{
  if (latest_lsn == 0)
    return;

  log_sys.latch.wr_lock(SRW_LOCK_CALL);
  const bool behind= latest_lsn > log_sys.get_lsn();
  if (behind)
    log_sys.set_recovered_lsn(latest_lsn);
  log_sys.latch.wr_unlock();
}

void refresh_buffer_pool_page(uint32_t space_id, uint32_t page_no)
{
  const page_id_t id(space_id, page_no);

  mysql_mutex_lock(&buf_pool.mutex);
  buf_pool_t::hash_chain &chain= buf_pool.page_hash.cell_get(id.fold());
  page_hash_latch &hash_lock= buf_pool.page_hash.lock_get(chain);
  hash_lock.lock_shared();
  buf_page_t *bpage= buf_pool.page_hash.get(id, chain);
  hash_lock.unlock_shared();

  if (bpage != nullptr && bpage->oldest_modification_acquire() == 0)
    static_cast<void>(buf_LRU_free_page(bpage, true));

  mysql_mutex_unlock(&buf_pool.mutex);
}

bool record_bit_set(const ib_lock_t *lock, uint32_t heap_no)
{
  return heap_no < lock_rec_get_n_bits(lock) &&
         lock_rec_get_nth_bit(lock, heap_no) != 0;
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
