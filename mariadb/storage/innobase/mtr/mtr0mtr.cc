/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file mtr/mtr0mtr.cc
Mini-transaction buffer

Created 11/26/1995 Heikki Tuuri
*******************************************************/

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include "mtr0log.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "page0types.h"
#include "log0crypt.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "btr0sea.h"
#endif
#include "btr0cur.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "sql_class.h" // THD
#include "mylite_ownerless_innodb_lock_hooks.h"
#include "log.h"
#include "my_cpu.h"

#include <algorithm>
#include <cstring>

#ifdef HAVE_PMEM
void (*mtr_t::commit_logger)(mtr_t *, std::pair<lsn_t,lsn_t>);
#endif

std::pair<lsn_t,lsn_t> (*mtr_t::finisher)(mtr_t *, size_t);

static thread_local unsigned ownerless_redo_log_latch_depth= 0;

static bool ownerless_page_write_requires_lock(const buf_page_t &page)
{
  if (!page.in_file() || page.id().space() >= SRV_TMP_SPACE_ID)
    return false;

  return page.frame != nullptr || page.zip.data != nullptr;
}

static bool ownerless_page_write_sql_autocommit(
    const trx_t *ownerless_trx) noexcept
{
  return ownerless_trx != nullptr && ownerless_trx->mysql_thd != nullptr &&
         !(ownerless_trx->mysql_thd->variables.option_bits &
           (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
}

static bool ownerless_space_path_is_undo_tablespace(const char *path)
{
  if (path == nullptr)
    return false;

  const char *base= std::strrchr(path, '/');
  const char *backslash= std::strrchr(path, '\\');
  if (backslash != nullptr && (base == nullptr || backslash > base))
    base= backslash;
  base= base == nullptr ? path : base + 1;

  if (std::strncmp(base, "undo", 4) != 0)
    return false;

  const char *digit= base + 4;
  if (*digit == '\0')
    return false;
  for (; *digit != '\0'; ++digit)
    if (*digit < '0' || *digit > '9')
      return false;
  return true;
}

static bool ownerless_space_is_undo_tablespace(uint32_t space_id)
{
  if (srv_is_undo_tablespace(space_id))
    return true;
  if (space_id > TRX_SYS_SPACE && space_id <= 3)
    return true;
  if (space_id > TRX_SYS_SPACE &&
      ((srv_undo_tablespaces_open != 0 &&
        space_id <= srv_undo_tablespaces_open) ||
       (srv_undo_tablespaces != 0 && space_id <= srv_undo_tablespaces)))
    return true;
  if (space_id == TRX_SYS_SPACE || space_id >= SRV_TMP_SPACE_ID ||
      !fil_system.is_initialised())
    return false;

  fil_space_t *space= fil_space_t::get(space_id);
  if (space == nullptr)
    return false;

  const fil_node_t *node= UT_LIST_GET_FIRST(space->chain);
  const bool result= node != nullptr &&
      ownerless_space_path_is_undo_tablespace(node->name);
  space->release();
  return result;
}

static bool ownerless_page_write_is_undo_page(const buf_page_t &page)
{
  if (ownerless_space_is_undo_tablespace(page.id().space()))
    return true;

  const byte *source= page.zip.data ? page.zip.data : page.frame;
  return source != nullptr && fil_page_get_type(source) == FIL_PAGE_UNDO_LOG;
}

static bool ownerless_page_write_defers_for_transaction(
    const buf_page_t &page)
{
  if (!ownerless_page_write_requires_lock(page))
    return false;

  const uint32_t space_id= page.id().space();
  return space_id != TRX_SYS_SPACE && !ownerless_page_write_is_undo_page(page);
}

static bool ownerless_page_write_publishes_with_transaction(
    const buf_page_t &page)
{
  return ownerless_page_write_defers_for_transaction(page);
}

static bool ownerless_page_write_holds_for_transaction(
    const buf_page_t &page)
{
  return ownerless_page_write_defers_for_transaction(page);
}

static bool ownerless_page_write_lock_only_transaction(const trx_t *trx)
{
  return trx != nullptr && trx->mysql_thd == nullptr && trx->undo_no == 0 &&
         !trx->dict_operation && trx->mod_tables.empty();
}

static bool ownerless_page_write_lock_only_transaction_page(
    const trx_t *trx, const buf_page_t &page)
{
  return ownerless_page_write_lock_only_transaction(trx) &&
         ownerless_page_write_defers_for_transaction(page);
}

static bool ownerless_page_write_in_startup_or_recovery()
{
  return recv_recovery_is_on() || !srv_was_started;
}

extern "C" void mylite_ownerless_innodb_reset_thread_redo_latch_depth(void)
{
  ownerless_redo_log_latch_depth= 0;
}

static uint64_t ownerless_page_write_pack(uint32_t space_id, uint32_t page_no)
{
  return (static_cast<uint64_t>(space_id) << 32) | page_no;
}

static void ownerless_page_write_release_lock(
    trx_t *trx, uint32_t space_id, uint32_t page_no)
{
  const int result= mylite_ownerless_innodb_lock_release_page_write(
    trx, space_id, page_no);
  if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    ut_error;
}

static bool ownerless_page_write_is_transaction_gate(uint64_t packed_page)
{
  const uint32_t space_id= static_cast<uint32_t>(packed_page >> 32);
  const uint32_t page_no= static_cast<uint32_t>(packed_page);
  return (space_id == MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_SPACE_ID &&
          page_no == MYLITE_OWNERLESS_INNODB_TRANSACTION_WRITE_PAGE_NO) ||
         (space_id < SRV_TMP_SPACE_ID &&
          page_no == MYLITE_OWNERLESS_INNODB_SPACE_TRANSACTION_WRITE_PAGE_NO);
}

static bool ownerless_page_write_transaction_has_modified_pages(
    const trx_t *trx)
{
  if (trx == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_vector &pages=
      trx->mylite_ownerless_modified_pages;
  return std::find_if(pages.begin(), pages.end(), [](uint64_t packed_page) {
    return !ownerless_page_write_is_transaction_gate(packed_page);
  }) != pages.end();
}

static void ownerless_page_write_forget_transaction_gate(trx_t *trx)
{
  if (trx == nullptr)
    return;

  trx_t::mylite_ownerless_page_vector &pages=
      trx->mylite_ownerless_modified_pages;
  pages.erase(std::remove_if(pages.begin(), pages.end(),
                             ownerless_page_write_is_transaction_gate),
              pages.end());
}

void mtr_t::finisher_update()
{
  ut_ad(log_sys.latch_have_wr());
#ifdef HAVE_PMEM
  if (log_sys.is_mmap())
  {
    commit_logger= mtr_t::commit_log<true>;
    finisher= mtr_t::finish_writer<true>;
    return;
  }
  commit_logger= mtr_t::commit_log<false>;
#endif
  finisher= mtr_t::finish_writer<false>;
}

void mtr_memo_slot_t::release() const
{
  ut_ad(object);

  switch (type) {
  case MTR_MEMO_S_LOCK:
    static_cast<index_lock*>(object)->s_unlock();
    break;
  case MTR_MEMO_X_LOCK:
  case MTR_MEMO_SX_LOCK:
    static_cast<index_lock*>(object)->
      u_or_x_unlock(type == MTR_MEMO_SX_LOCK);
    break;
  case MTR_MEMO_SPACE_X_LOCK:
    static_cast<fil_space_t*>(object)->set_committed_size();
    static_cast<fil_space_t*>(object)->x_unlock();
    break;
  default:
    buf_page_t *bpage= static_cast<buf_page_t*>(object);
    ut_d(const auto s=)
      bpage->unfix();
    ut_ad(s < buf_page_t::READ_FIX || s >= buf_page_t::WRITE_FIX);
    switch (type) {
    case MTR_MEMO_PAGE_S_FIX:
      bpage->lock.s_unlock();
      break;
    case MTR_MEMO_BUF_FIX:
      break;
    default:
      ut_ad(type == MTR_MEMO_PAGE_SX_FIX ||
            type == MTR_MEMO_PAGE_X_FIX ||
            type == MTR_MEMO_PAGE_SX_MODIFY ||
            type == MTR_MEMO_PAGE_X_MODIFY);
      bpage->lock.u_or_x_unlock(type & MTR_MEMO_PAGE_SX_FIX);
    }
  }
}

/** Prepare to insert a modified blcok into flush_list.
@param lsn start LSN of the mini-transaction
@return insert position for insert_into_flush_list() */
inline buf_page_t *buf_pool_t::prepare_insert_into_flush_list(lsn_t lsn)
  noexcept
{
  ut_ad(recv_recovery_is_on() || log_sys.latch_have_any());
  ut_ad(lsn >= log_sys.last_checkpoint_lsn);
  mysql_mutex_assert_owner(&flush_list_mutex);
  static_assert(log_t::FIRST_LSN >= 2, "compatibility");

rescan:
  buf_page_t *prev= UT_LIST_GET_FIRST(flush_list);
  if (prev)
  {
    lsn_t om= prev->oldest_modification();
    if (om == 1)
    {
      delete_from_flush_list(prev);
      goto rescan;
    }
    ut_ad(om > 2);
    if (om <= lsn)
      return nullptr;
    while (buf_page_t *next= UT_LIST_GET_NEXT(list, prev))
    {
      om= next->oldest_modification();
      if (om == 1)
      {
        delete_from_flush_list(next);
        continue;
      }
      ut_ad(om > 2);
      if (om <= lsn)
        break;
      prev= next;
    }
    flush_hp.adjust(prev);
  }
  return prev;
}

/** Insert a modified block into the flush list.
@param prev     insert position (from prepare_insert_into_flush_list())
@param block    modified block
@param lsn      start LSN of the mini-transaction that modified the block */
inline void buf_pool_t::insert_into_flush_list(buf_page_t *prev,
                                               buf_block_t *block, lsn_t lsn)
  noexcept
{
  ut_ad(!fsp_is_system_temporary(block->page.id().space()));
  mysql_mutex_assert_owner(&flush_list_mutex);

  MEM_CHECK_DEFINED(block->page.zip.data
                    ? block->page.zip.data : block->page.frame,
                    block->physical_size());

  if (const lsn_t old= block->page.oldest_modification())
  {
    if (old > 1)
      return;
    flush_hp.adjust(&block->page);
    UT_LIST_REMOVE(flush_list, &block->page);
  }
  else
    flush_list_bytes+= block->physical_size();

  ut_ad(flush_list_bytes <= size_in_bytes);

  if (prev)
    UT_LIST_INSERT_AFTER(flush_list, prev, &block->page);
  else
    UT_LIST_ADD_FIRST(flush_list, &block->page);

  block->page.set_oldest_modification(lsn);
}

mtr_t::mtr_t(trx_t *trx) : trx(trx) {}
mtr_t::~mtr_t()= default;

/** Start a mini-transaction. */
void mtr_t::start()
{
  ut_ad(m_memo.empty());
  ut_ad(m_ownerless_page_write_mtr_pages.empty());
  ut_ad(!m_freed_pages);
  ut_ad(!m_freed_space);
  MEM_CHECK_DEFINED(&trx, sizeof trx);
  MEM_UNDEFINED(this, sizeof *this);
  MEM_MAKE_DEFINED(&trx, sizeof trx);
  MEM_MAKE_DEFINED(&m_memo, sizeof m_memo);
  MEM_MAKE_DEFINED(&m_ownerless_page_write_mtr_pages,
                   sizeof m_ownerless_page_write_mtr_pages);
  MEM_MAKE_DEFINED(&m_freed_space, sizeof m_freed_space);
  MEM_MAKE_DEFINED(&m_freed_pages, sizeof m_freed_pages);

  ut_d(m_start= true);
  ut_d(m_commit= false);
  ut_d(m_freeing_tree= false);

  m_last= nullptr;
  m_last_offset= 0;

  new(&m_log) mtr_buf_t();

  m_made_dirty= false;
  m_latch_ex= false;
  m_ownerless_redo= false;
  m_ownerless_redo_borrowed_latch= false;
  m_modifications= false;
  m_log_mode= MTR_LOG_ALL;
  ut_d(m_user_space_id= TRX_SYS_SPACE);
  m_user_space= nullptr;
  m_commit_lsn= 0;
  m_ownerless_redo_start_lsn= 0;
  m_ownerless_redo_end_lsn= 0;
  m_ownerless_page_write_trx= nullptr;
  m_ownerless_page_write_mtr_pages.clear();
  m_trim_pages= false;
}

/** Release the resources */
inline void mtr_t::release_resources()
{
  ut_ad(is_active());
  ut_ad(m_memo.empty());
  ut_ad(m_ownerless_page_write_mtr_pages.empty());
  m_log.erase();
  ut_d(m_commit= true);
}

/** Handle any pages that were freed during the mini-transaction. */
void mtr_t::process_freed_pages()
{
  if (m_freed_pages)
  {
    ut_ad(!m_freed_pages->empty());
    ut_ad(m_freed_space);
    ut_ad(m_freed_space->is_owner());
    ut_ad(is_named_space(m_freed_space));

    /* Update the last freed lsn */
    m_freed_space->freed_range_mutex.lock();
    m_freed_space->update_last_freed_lsn(m_commit_lsn);
    if (!m_trim_pages)
      for (const auto &range : *m_freed_pages)
        m_freed_space->add_free_range(range);
    else
      m_freed_space->clear_freed_ranges();
    m_freed_space->freed_range_mutex.unlock();

    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
    /* mtr_t::start() will reset m_trim_pages */
  }
  else
    ut_ad(!m_freed_space);
}

ATTRIBUTE_COLD __attribute__((noinline))
/** Insert a modified block into buf_pool.flush_list on IMPORT TABLESPACE. */
static void insert_imported(buf_block_t *block)
{
  if (block->page.oldest_modification() <= 1)
  {
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    /* For unlogged mtrs (MTR_LOG_NO_REDO), we use the current system LSN. The
    mtr that generated the LSN is either already committed or in mtr_t::commit.
    Shared latch and relaxed atomics should be fine here as it is guaranteed
    that both the current mtr and the mtr that generated the LSN would have
    added the dirty pages to flush list before we access the minimum LSN during
    checkpoint. log_checkpoint_low() acquires exclusive log_sys.latch before
    commencing. */
    const lsn_t lsn= log_sys.get_lsn();
    mysql_mutex_lock(&buf_pool.flush_list_mutex);
    buf_pool.insert_into_flush_list
      (buf_pool.prepare_insert_into_flush_list(lsn), block, lsn);
    log_sys.latch.wr_unlock();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  }
}

/** Release modified pages when no log was written. */
void mtr_t::release_unlogged()
{
  ut_ad(m_log_mode == MTR_LOG_NO_REDO);
  ut_ad(m_log.empty());

  process_freed_pages();

  for (auto it= m_memo.rbegin(); it != m_memo.rend(); it++)
  {
    mtr_memo_slot_t &slot= *it;
    ut_ad(slot.object);
    switch (slot.type) {
    case MTR_MEMO_S_LOCK:
      static_cast<index_lock*>(slot.object)->s_unlock();
      break;
    case MTR_MEMO_SPACE_X_LOCK:
      static_cast<fil_space_t*>(slot.object)->set_committed_size();
      static_cast<fil_space_t*>(slot.object)->x_unlock();
      ownerless_space_write_leave(slot);
      break;
    case MTR_MEMO_X_LOCK:
    case MTR_MEMO_SX_LOCK:
      static_cast<index_lock*>(slot.object)->
        u_or_x_unlock(slot.type == MTR_MEMO_SX_LOCK);
      break;
    default:
      buf_block_t *block= static_cast<buf_block_t*>(slot.object);
      ut_d(const auto s=) block->page.unfix();
      ut_ad(s >= buf_page_t::FREED);
      ut_ad(s < buf_page_t::READ_FIX);

      if (slot.type & MTR_MEMO_MODIFY)
      {
        ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
              slot.type == MTR_MEMO_PAGE_SX_MODIFY);
        ut_ad(block->page.id() < end_page_id);
        if (ownerless_page_write_uses_transaction_release())
          ownerless_page_write_note_transaction_page(block->page);
        insert_imported(block);
      }

      ownerless_page_write_leave(slot);
      switch (slot.type) {
      case MTR_MEMO_PAGE_S_FIX:
        block->page.lock.s_unlock();
        break;
      case MTR_MEMO_BUF_FIX:
        break;
      default:
        ut_ad(slot.type == MTR_MEMO_PAGE_SX_FIX ||
              slot.type == MTR_MEMO_PAGE_X_FIX ||
              slot.type == MTR_MEMO_PAGE_SX_MODIFY ||
              slot.type == MTR_MEMO_PAGE_X_MODIFY);
        block->page.lock.u_or_x_unlock(slot.type & MTR_MEMO_PAGE_SX_FIX);
      }
    }
  }

  m_memo.clear();
}

void mtr_t::release()
{
  for (auto it= m_memo.rbegin(); it != m_memo.rend(); it++)
  {
    ownerless_page_write_leave(*it);
    it->release();
    ownerless_space_write_leave(*it);
  }
  m_memo.clear();
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_redo_enter() noexcept
{
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;
  if (ownerless_page_write_in_startup_or_recovery())
    return;

  if (mylite_ownerless_innodb_redo_is_active())
  {
    const int result= mylite_ownerless_innodb_redo_enter(nullptr);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      m_ownerless_redo= true;
      m_ownerless_redo_borrowed_latch= ownerless_redo_log_latch_depth != 0;
    }
    else if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      ut_error;
    return;
  }

  if (!m_latch_ex)
  {
    m_latch_ex= true;
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
  }

  uint64_t ownerless_latest_lsn= 0;
  const int result= mylite_ownerless_innodb_redo_enter(&ownerless_latest_lsn);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    m_ownerless_redo= true;
    ownerless_redo_log_latch_depth++;
    if (ownerless_latest_lsn > log_sys.get_lsn())
      log_sys.set_recovered_lsn(ownerless_latest_lsn);
  }
  else if (result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    ut_error;
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_redo_leave() noexcept
{
  if (!m_ownerless_redo)
    return;

  const lsn_t lsn= m_commit_lsn;
  if (lsn != 0)
    log_write_up_to(lsn, false);
  if (m_ownerless_redo_start_lsn != 0 &&
      m_ownerless_redo_end_lsn > m_ownerless_redo_start_lsn)
  {
    uint64_t written_lsn= 0;
    const int result= mylite_ownerless_innodb_redo_written(
      m_ownerless_redo_start_lsn,
      m_ownerless_redo_end_lsn,
      &written_lsn);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      ut_error;
  }
  mylite_ownerless_innodb_redo_leave(lsn);
  m_ownerless_redo= false;
  m_ownerless_redo_borrowed_latch= false;
  m_ownerless_redo_start_lsn= 0;
  m_ownerless_redo_end_lsn= 0;
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_enter(
    const buf_block_t &block) noexcept
{
  if (!ownerless_page_write_requires_lock(block.page))
  {
    ownerless_page_write_refresh(block);
    return;
  }

  const page_id_t id{block.page.id()};
  trx_t *ownerless_trx= ownerless_page_write_trx();
  const bool lock_only_page=
    ownerless_page_write_lock_only_transaction_page(ownerless_trx, block.page);
  if (lock_only_page)
  {
    ownerless_page_write_refresh(block);
    return;
  }
  uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (std::find(m_ownerless_page_write_mtr_pages.begin(),
                m_ownerless_page_write_mtr_pages.end(),
                packed_page) != m_ownerless_page_write_mtr_pages.end())
    return;

  bool page_already_modified_by_transaction= false;
  if (ownerless_page_write_uses_transaction_release() &&
      ownerless_page_write_holds_for_transaction(block.page) &&
      ownerless_trx != nullptr)
  {
    const trx_t::mylite_ownerless_page_vector &pages=
        ownerless_trx->mylite_ownerless_modified_pages;
    page_already_modified_by_transaction=
        std::find(pages.begin(), pages.end(), packed_page) != pages.end();
  }
  if (page_already_modified_by_transaction)
  {
    return;
  }
  bool page_write_waited= false;
  bool page_write_acquired= false;
  for (;;)
  {
    uint32_t acquire_flags= 0U;
    const unsigned timeout_ms=
      ownerless_page_write_in_startup_or_recovery() ? 0U : 30000U;
    const int result= mylite_ownerless_innodb_lock_acquire_page_write(
      ownerless_trx, id.space(), id.page_no(), timeout_ms, &acquire_flags);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
        result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    {
      page_write_acquired= result == MYLITE_OWNERLESS_INNODB_LOCK_OK;
      page_write_waited= page_write_waited ||
          (acquire_flags & MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED) != 0U;
      break;
    }
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
      ut_error;
    if (ownerless_page_write_in_startup_or_recovery())
    {
      ownerless_page_write_refresh(block, true);
      return;
    }
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
    {
      if (ownerless_trx != nullptr && !m_modifications &&
          !ownerless_page_write_transaction_has_modified_pages(ownerless_trx))
      {
        /* This hook has no error return path. If we have not dirtied a
        persistent page yet, break the physical-page cycle and retry. */
        mylite_ownerless_innodb_lock_release_transaction_page_writes(
            ownerless_trx);
        ownerless_page_write_forget_transaction_gate(ownerless_trx);
        ownerless_page_write_refresh(block, true);
        page_write_waited= true;
        continue;
      }
      page_write_waited= true;
      continue;
    }
    page_write_waited= true;
  }

  if (page_write_acquired)
    ownerless_page_write_note_mtr_page(block.page);

  if (page_write_waited)
  {
    ownerless_page_write_refresh(block, true);
    if (ownerless_trx != nullptr)
      ownerless_trx->mylite_ownerless_page_refreshed_after_wait= true;
    return;
  }

  if (block.page.oldest_modification_acquire() > 1)
    return;
  ownerless_page_write_refresh(block);
}

trx_t *mtr_t::ownerless_page_write_trx() const noexcept
{
  if (trx != nullptr)
    return trx;
  if (m_ownerless_page_write_trx != nullptr)
    return m_ownerless_page_write_trx;
  m_ownerless_page_write_trx= current_trx();
  return m_ownerless_page_write_trx;
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_refresh(
    const buf_block_t &block, bool force_page_version) noexcept
{
  const int refresh_result= force_page_version
      ? mylite_ownerless_innodb_refresh_page_for_write_force(&block)
      : mylite_ownerless_innodb_refresh_page_for_write(&block);
  if (refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ut_error;
  }
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_leave(
    const mtr_memo_slot_t &slot) noexcept
{
  if (!(slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX)))
    return;
  const buf_page_t *bpage= static_cast<const buf_page_t*>(slot.object);
  if (ownerless_page_write_lock_only_transaction_page(
          ownerless_page_write_trx(), *bpage))
    return;
  const bool mtr_page_acquired= ownerless_page_write_forget_mtr_page(*bpage);
  if (!mtr_page_acquired)
  {
    if (ownerless_page_write_uses_transaction_release() &&
        ownerless_page_write_holds_for_transaction(*bpage))
      static_cast<void>(ownerless_page_write_release_deferred(slot));
    return;
  }

  const page_id_t id{bpage->id()};
  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_page_write_uses_transaction_release() &&
      ownerless_page_write_holds_for_transaction(*bpage) &&
      ownerless_page_write_release_deferred(slot))
    return;
  ownerless_page_write_release_lock(ownerless_trx, id.space(), id.page_no());
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_space_write_enter(
    fil_space_t *space) noexcept
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || space == nullptr ||
      space->id >= SRV_TMP_SPACE_ID || space->is_temporary())
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  for (;;)
  {
    const unsigned timeout_ms=
      ownerless_page_write_in_startup_or_recovery() ? 0U : 30000U;
    const int result= mylite_ownerless_innodb_lock_acquire_page_write(
      ownerless_trx, space->id,
      MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO, timeout_ms, nullptr);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
        result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      break;
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
      ut_error;
    if (ownerless_page_write_in_startup_or_recovery())
      return;
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK &&
        ownerless_trx != nullptr && !m_modifications &&
        !ownerless_page_write_transaction_has_modified_pages(ownerless_trx))
    {
      mylite_ownerless_innodb_lock_release_transaction_page_writes(
          ownerless_trx);
      ownerless_page_write_forget_transaction_gate(ownerless_trx);
    }
    mylite_ownerless_innodb_refresh_external_space_allocation(space->id);
  }

  mylite_ownerless_innodb_refresh_external_space_allocation(space->id);
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_space_write_leave(
    const mtr_memo_slot_t &slot) noexcept
{
  if (slot.type != MTR_MEMO_SPACE_X_LOCK)
    return;
  if (!mylite_ownerless_innodb_lock_has_hooks())
    return;

  const fil_space_t *space= static_cast<const fil_space_t*>(slot.object);
  if (space == nullptr || space->id >= SRV_TMP_SPACE_ID || space->is_temporary())
    return;

  if (m_commit_lsn != 0 && ownerless_space_is_undo_tablespace(space->id))
    mylite_ownerless_innodb_flush_space_dirty_pages(space->id);

  ownerless_page_write_release_lock(
      ownerless_page_write_trx(), space->id,
      MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO);
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_writes_publish() noexcept
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || m_commit_lsn == 0)
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;
  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (!(slot.type & MTR_MEMO_MODIFY))
      continue;

    const buf_page_t *bpage= static_cast<const buf_page_t*>(slot.object);
    const bool uses_transaction= ownerless_page_write_uses_transaction_release();
    const bool transaction_publish=
        ownerless_page_write_publishes_with_transaction(*bpage);
    if (uses_transaction && transaction_publish)
    {
      ownerless_page_write_note_transaction_page(*bpage);
      continue;
    }

    ownerless_page_write_publish(*bpage);
  }
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_publish(
    const buf_page_t &bpage) noexcept
{
  if (!mylite_ownerless_innodb_lock_has_hooks() || m_commit_lsn == 0)
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;

  const page_id_t id{bpage.id()};
  if (id.space() >= SRV_TMP_SPACE_ID || !bpage.in_file())
    return;
  if (ownerless_page_write_lock_only_transaction_page(
          ownerless_page_write_trx(), bpage))
    return;

  const byte *source= bpage.zip.data ? bpage.zip.data : bpage.frame;
  if (source == nullptr)
    return;

  fil_space_t *space= fil_space_t::get(id.space());
  if (space == nullptr)
    return;
  const bool full_crc32= space->full_crc32();
  space->release();

  const ulint page_size= bpage.physical_size();
  byte *page= static_cast<byte*>(aligned_malloc(page_size, page_size));
  if (page == nullptr)
    return;

  ::memcpy(page, source, page_size);
  if (bpage.zip.data)
    buf_flush_update_zip_checksum(page, page_size);
  else
    buf_flush_init_for_writing(nullptr, page, nullptr, full_crc32);

  const lsn_t page_lsn= mach_read_from_8(page + FIL_PAGE_LSN);
  if (page_lsn == m_commit_lsn)
  {
    static_cast<void>(mylite_ownerless_innodb_publish_page_version(
        id.space(), id.page_no(), page_lsn, m_commit_lsn, page,
        static_cast<uint32_t>(page_size)));
  }

  aligned_free(page);
}

bool mtr_t::ownerless_page_write_release_deferred(
    const mtr_memo_slot_t &slot) const noexcept
{
  if (!(slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX)) ||
      !ownerless_page_write_uses_transaction_release())
    return false;

  const buf_page_t *bpage= static_cast<const buf_page_t*>(slot.object);
  if (!ownerless_page_write_holds_for_transaction(*bpage))
    return false;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx == nullptr)
    return false;

  const page_id_t id{bpage->id()};
  uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  const trx_t::mylite_ownerless_page_vector &pages=
      ownerless_trx->mylite_ownerless_modified_pages;
  return std::find(pages.begin(), pages.end(), packed_page) != pages.end();
}

void mtr_t::ownerless_page_write_note_transaction_page(
    const buf_page_t &bpage) const noexcept
{
  if (!ownerless_page_write_publishes_with_transaction(bpage))
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx == nullptr)
    return;

  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  trx_t::mylite_ownerless_page_vector &pages=
      ownerless_trx->mylite_ownerless_modified_pages;
  if (std::find(pages.begin(), pages.end(), packed_page) == pages.end())
    pages.push_back(packed_page);
}

void mtr_t::ownerless_page_write_note_mtr_page(
    const buf_page_t &bpage) noexcept
{
  const page_id_t id{bpage.id()};
  uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (std::find(m_ownerless_page_write_mtr_pages.begin(),
                m_ownerless_page_write_mtr_pages.end(),
                packed_page) == m_ownerless_page_write_mtr_pages.end())
    m_ownerless_page_write_mtr_pages.emplace_back(packed_page);
}

bool mtr_t::ownerless_page_write_forget_mtr_page(
    const buf_page_t &bpage) noexcept
{
  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  auto it= std::find(m_ownerless_page_write_mtr_pages.begin(),
                     m_ownerless_page_write_mtr_pages.end(),
                     packed_page);
  if (it == m_ownerless_page_write_mtr_pages.end())
    return false;
  m_ownerless_page_write_mtr_pages.erase(it, it + 1);
  return true;
}

bool mtr_t::ownerless_page_write_uses_transaction_release() const noexcept
{
  if (ownerless_page_write_in_startup_or_recovery())
    return false;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_page_write_lock_only_transaction(ownerless_trx))
    return false;
  return ownerless_trx != nullptr && ownerless_trx->id != 0 &&
         !ownerless_trx->read_only &&
         !ownerless_trx->dict_operation;
}

bool mtr_t::ownerless_page_write_should_prepare(
    const buf_page_t &bpage) const noexcept
{
  if (ownerless_page_write_in_startup_or_recovery())
    return false;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx != nullptr && ownerless_trx->read_only)
    return false;
  if (ownerless_trx != nullptr && ownerless_trx->mysql_thd != nullptr &&
      ownerless_trx->mysql_thd->lex->sql_command == SQLCOM_SELECT)
    return false;
  if (!ownerless_page_write_holds_for_transaction(bpage))
    return true;
  return ownerless_trx == nullptr || ownerless_trx->auto_commit ||
         ownerless_page_write_sql_autocommit(ownerless_trx) ||
         !ownerless_page_write_transaction_has_modified_pages(ownerless_trx);
}

ATTRIBUTE_NOINLINE void mtr_t::commit_log_release() noexcept
{
  if (m_ownerless_redo_borrowed_latch)
    return;

  if (m_latch_ex)
  {
    if (m_ownerless_redo)
    {
      ut_ad(ownerless_redo_log_latch_depth != 0);
      ownerless_redo_log_latch_depth--;
    }
    log_sys.latch.wr_unlock();
    m_latch_ex= false;
  }
  else
    log_sys.latch.rd_unlock();
}

static ATTRIBUTE_NOINLINE ATTRIBUTE_COLD
void mtr_flush_ahead(lsn_t flush_lsn) noexcept
{
  buf_flush_ahead(flush_lsn, bool(flush_lsn & 1));
}

template<bool mmap>
void mtr_t::commit_log(mtr_t *mtr, std::pair<lsn_t,lsn_t> lsns) noexcept
{
  size_t modified= 0;

  if (mtr->m_made_dirty)
  {
    auto it= mtr->m_memo.rbegin();

    mysql_mutex_lock(&buf_pool.flush_list_mutex);

    buf_page_t *const prev=
      buf_pool.prepare_insert_into_flush_list(lsns.first);

    while (it != mtr->m_memo.rend())
    {
      const mtr_memo_slot_t &slot= *it++;
      if (slot.type & MTR_MEMO_MODIFY)
      {
        ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
              slot.type == MTR_MEMO_PAGE_SX_MODIFY);
        modified++;
        buf_block_t *b= static_cast<buf_block_t*>(slot.object);
        ut_ad(b->page.id() < end_page_id);
        ut_d(const auto s= b->page.state());
        ut_ad(s > buf_page_t::FREED);
        ut_ad(s < buf_page_t::READ_FIX);
        ut_ad(mach_read_from_8(b->page.frame + FIL_PAGE_LSN) <=
              mtr->m_commit_lsn);
        mach_write_to_8(b->page.frame + FIL_PAGE_LSN, mtr->m_commit_lsn);
        if (UNIV_LIKELY_NULL(b->page.zip.data))
          memcpy_aligned<8>(FIL_PAGE_LSN + b->page.zip.data,
                            FIL_PAGE_LSN + b->page.frame, 8);
        buf_pool.insert_into_flush_list(prev, b, lsns.first);
      }
    }

    ut_ad(modified);
    buf_pool.flush_list_requests+= modified;
    buf_pool.page_cleaner_wakeup();
    mysql_mutex_unlock(&buf_pool.flush_list_mutex);

    mtr->commit_log_release();
    mtr->ownerless_redo_leave();
    mtr->ownerless_page_writes_publish();
    mtr->release();
  }
  else
  {
    mtr->commit_log_release();
    mtr->ownerless_redo_leave();

    for (auto it= mtr->m_memo.rbegin(); it != mtr->m_memo.rend(); )
    {
      const mtr_memo_slot_t &slot= *it++;
      ut_ad(slot.object);
      switch (slot.type) {
      case MTR_MEMO_S_LOCK:
        static_cast<index_lock*>(slot.object)->s_unlock();
        break;
      case MTR_MEMO_SPACE_X_LOCK:
        static_cast<fil_space_t*>(slot.object)->set_committed_size();
        static_cast<fil_space_t*>(slot.object)->x_unlock();
        mtr->ownerless_space_write_leave(slot);
        break;
      case MTR_MEMO_X_LOCK:
      case MTR_MEMO_SX_LOCK:
        static_cast<index_lock*>(slot.object)->
          u_or_x_unlock(slot.type == MTR_MEMO_SX_LOCK);
        break;
      default:
        buf_page_t *bpage= static_cast<buf_page_t*>(slot.object);
        ut_d(const auto s=)
          bpage->unfix();
        if (slot.type & MTR_MEMO_MODIFY)
        {
          ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
                slot.type == MTR_MEMO_PAGE_SX_MODIFY);
          ut_ad(bpage->oldest_modification() > 1);
          ut_ad(bpage->oldest_modification() < mtr->m_commit_lsn);
          ut_ad(bpage->id() < end_page_id);
          ut_ad(s >= buf_page_t::FREED);
          ut_ad(s < buf_page_t::READ_FIX);
          ut_ad(mach_read_from_8(bpage->frame + FIL_PAGE_LSN) <=
                mtr->m_commit_lsn);
          mach_write_to_8(bpage->frame + FIL_PAGE_LSN, mtr->m_commit_lsn);
          if (UNIV_LIKELY_NULL(bpage->zip.data))
            memcpy_aligned<8>(FIL_PAGE_LSN + bpage->zip.data,
                              FIL_PAGE_LSN + bpage->frame, 8);
          if (mtr->ownerless_page_write_uses_transaction_release() &&
              ownerless_page_write_publishes_with_transaction(*bpage))
            mtr->ownerless_page_write_note_transaction_page(*bpage);
          else
            mtr->ownerless_page_write_publish(*bpage);
          modified++;
        }
        switch (auto latch= slot.type & ~MTR_MEMO_MODIFY) {
        case MTR_MEMO_PAGE_S_FIX:
          bpage->lock.s_unlock();
          continue;
        case MTR_MEMO_PAGE_SX_FIX:
        case MTR_MEMO_PAGE_X_FIX:
          mtr->ownerless_page_write_leave(slot);
          bpage->lock.u_or_x_unlock(latch == MTR_MEMO_PAGE_SX_FIX);
          continue;
        default:
          ut_ad(latch == MTR_MEMO_BUF_FIX);
        }
      }
    }

    buf_pool.add_flush_list_requests(modified);
    mtr->m_memo.clear();
  }

  if (modified != 0 && mtr->trx)
    if (ha_handler_stats *stats= mtr->trx->active_handler_stats)
      stats->pages_updated+= modified;

  if (UNIV_UNLIKELY(lsns.second != 0))
  {
    ut_ad(lsns.second < mtr->m_commit_lsn);
    mtr_flush_ahead(lsns.second);
  }
}

/** Commit a mini-transaction. */
void mtr_t::commit()
{
  ut_ad(is_active());

  /* This is a dirty read, for debugging. */
  ut_ad(!m_modifications || !recv_no_log_write);
  ut_ad(!m_modifications || m_log_mode != MTR_LOG_NONE);
  ut_ad(!m_latch_ex);

  if (m_modifications && (m_log_mode == MTR_LOG_NO_REDO || !m_log.empty()))
  {
    if (UNIV_UNLIKELY(!is_logged()))
    {
      release_unlogged();
      goto func_exit;
    }

    ut_ad(!srv_read_only_mode);
    std::pair<lsn_t,lsn_t> lsns{do_write()};
    process_freed_pages();
#ifdef HAVE_PMEM
    commit_logger(this, lsns);
#else
    commit_log<false>(this, lsns);
#endif
  }
  else
  {
    if (m_freed_pages)
    {
      ut_ad(!m_freed_pages->empty());
      ut_ad(m_freed_space == fil_system.temp_space);
      ut_ad(!m_trim_pages);
      for (const auto &range : *m_freed_pages)
        m_freed_space->add_free_range(range);
      delete m_freed_pages;
      m_freed_pages= nullptr;
      m_freed_space= nullptr;
    }
    release();
  }

func_exit:
  release_resources();
}

void mtr_t::rollback_to_savepoint(ulint begin, ulint end)
{
  ut_ad(end <= m_memo.size());
  ut_ad(begin <= end);
  ulint s= end;

  while (s-- > begin)
  {
    const mtr_memo_slot_t &slot= m_memo[s];
    ut_ad(slot.object);
    ut_ad(!(slot.type & MTR_MEMO_MODIFY));
    ownerless_page_write_leave(slot);
    slot.release();
  }

  m_memo.erase(m_memo.begin() + begin, m_memo.begin() + end);
}

/** Set create_lsn. */
inline void fil_space_t::set_create_lsn(lsn_t lsn) noexcept
{
  /* Concurrent log_checkpoint_low() must be impossible. */
  ut_ad(latch.have_wr());
  create_lsn= lsn;
}

/** Commit a mini-transaction that is shrinking a tablespace.
@param space   tablespace that is being shrunk
@param size    new size in pages */
void mtr_t::commit_shrink(fil_space_t &space, uint32_t size)
{
  ut_ad(is_active());
  ut_ad(!high_level_read_only);
  ut_ad(m_modifications);
  ut_ad(!m_memo.empty());
  ut_ad(!recv_recovery_is_on());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(!m_freed_pages);

  log_write_and_flush_prepare();
  m_latch_ex= true;
  log_sys.latch.wr_lock(SRW_LOCK_CALL);

  const lsn_t start_lsn= do_write().first;
  ut_d(m_log.erase());

  fil_node_t *file= UT_LIST_GET_LAST(space.chain);
  mysql_mutex_lock(&fil_system.mutex);
  ut_ad(file->is_open());
  ut_ad(space.size >= size);
  ut_ad(file->size >= space.size - size);
  file->size-= space.size - size;
  space.size= space.size_in_header= size;

  if (space.id == TRX_SYS_SPACE)
    srv_sys_space.set_last_file_size(file->size);
  else
    space.set_create_lsn(m_commit_lsn);

  mysql_mutex_unlock(&fil_system.mutex);

  space.clear_freed_ranges();

  /* Durably write the reduced FSP_SIZE before truncating the data file. */
  log_write_and_flush();
  ut_ad(log_sys.latch_have_wr());

  os_file_truncate(file->name, file->handle,
                   os_offset_t{file->size} << srv_page_size_shift, true);

  space.clear_freed_ranges();

  const page_id_t high{space.id, size};
  size_t modified= 0;
  auto it= m_memo.rbegin();
  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  buf_page_t *const prev= buf_pool.prepare_insert_into_flush_list(start_lsn);

  while (it != m_memo.rend())
  {
    mtr_memo_slot_t &slot= *it++;

    ut_ad(slot.object);
    if (slot.type == MTR_MEMO_SPACE_X_LOCK)
      ut_ad(high.space() == static_cast<fil_space_t*>(slot.object)->id);
    else
    {
      ut_ad(slot.type == MTR_MEMO_PAGE_X_MODIFY ||
            slot.type == MTR_MEMO_PAGE_SX_MODIFY ||
            slot.type == MTR_MEMO_PAGE_X_FIX ||
            slot.type == MTR_MEMO_PAGE_SX_FIX);
      buf_block_t *b= static_cast<buf_block_t*>(slot.object);
      const page_id_t id{b->page.id()};
      const auto s= b->page.state();
      ut_ad(s > buf_page_t::FREED);
      ut_ad(s < buf_page_t::READ_FIX);
      ut_ad(b->page.frame);
      ut_ad(mach_read_from_8(b->page.frame + FIL_PAGE_LSN) <= m_commit_lsn);
      ut_ad(!b->page.zip.data); // we no not shrink ROW_FORMAT=COMPRESSED

      if (id < high)
      {
        ut_ad(id.space() == high.space() ||
              (id == page_id_t{0, TRX_SYS_PAGE_NO} &&
               srv_is_undo_tablespace(high.space())));
        if (slot.type & MTR_MEMO_MODIFY)
        {
          modified++;
          mach_write_to_8(b->page.frame + FIL_PAGE_LSN, m_commit_lsn);
          buf_pool.insert_into_flush_list(prev, b, start_lsn);
        }
      }
      else
      {
        ut_ad(id.space() == high.space());
        if (s >= buf_page_t::UNFIXED)
          b->page.set_freed(s);
        if (b->page.oldest_modification() > 1)
          b->page.reset_oldest_modification();
        slot.type= mtr_memo_type_t(slot.type & ~MTR_MEMO_MODIFY);
      }
    }
  }

  ut_ad(modified);
  buf_pool.flush_list_requests+= modified;
  buf_pool.page_cleaner_wakeup();
  mysql_mutex_unlock(&buf_pool.flush_list_mutex);

  if (m_ownerless_redo)
  {
    ut_ad(ownerless_redo_log_latch_depth != 0);
    ownerless_redo_log_latch_depth--;
  }
  log_sys.latch.wr_unlock();
  m_latch_ex= false;
  ownerless_redo_leave();

  release();
  release_resources();
}

/** Commit a mini-transaction that is deleting or renaming a file.
@param space   tablespace that is being renamed or deleted
@param name    new file name (nullptr=the file will be deleted)
@return whether the operation succeeded */
bool mtr_t::commit_file(fil_space_t &space, const char *name)
{
  ut_ad(is_active());
  ut_ad(!high_level_read_only);
  ut_ad(m_modifications);
  ut_ad(!m_made_dirty);
  ut_ad(!recv_recovery_is_on());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(UT_LIST_GET_LEN(space.chain) == 1);
  ut_ad(!m_latch_ex);

  const bool crypt{log_sys.is_encrypted()};
  m_commit_lsn= crypt ? log_sys.get_flushed_lsn() : 0;
  const size_t size{crypt ? 8 + encrypt() : crc32c()};

  log_write_and_flush_prepare();
  ownerless_redo_enter();
  if (!m_latch_ex)
  {
    m_latch_ex= true;
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
  }
  finish_write(size);

  if (!name && space.max_lsn)
  {
    space.max_lsn= 0;
    fil_system.named_spaces.remove(space);
  }

  /* Block log_checkpoint(). */
  mysql_mutex_lock(&buf_pool.flush_list_mutex);

  /* Durably write the log for the file system operation. */
  log_write_and_flush();

  if (m_ownerless_redo)
  {
    ut_ad(ownerless_redo_log_latch_depth != 0);
    ownerless_redo_log_latch_depth--;
  }
  log_sys.latch.wr_unlock();
  m_latch_ex= false;
  ownerless_redo_leave();

  char *old_name= space.chain.start->name;
  bool success= true;

  if (name)
  {
    char *new_name= mem_strdup(name);
    mysql_mutex_lock(&fil_system.mutex);
    success= os_file_rename(innodb_data_file_key, old_name, name);
    if (success)
      space.chain.start->name= new_name;
    else
      old_name= new_name;
    mysql_mutex_unlock(&fil_system.mutex);
    ut_free(old_name);
  }

  mysql_mutex_unlock(&buf_pool.flush_list_mutex);
  release_resources();

  return success;
}

ATTRIBUTE_NOINLINE size_t mtr_t::crc32c() noexcept
{
  m_crc= 0;
  size_t len= 5;
  for (const mtr_buf_t::block_t &b : m_log)
  {
    len+= b.used();
    m_crc= my_crc32c(m_crc, b.begin(), b.used());
  }
  return len;
}

/** Commit a mini-transaction that did not modify any pages,
but generated some redo log on a higher level, such as
FILE_MODIFY records and an optional FILE_CHECKPOINT marker.
The caller must hold exclusive log_sys.latch.
This is to be used at log_checkpoint().
@param checkpoint_lsn   the log sequence number of a checkpoint, or 0
@return current LSN */
ATTRIBUTE_COLD lsn_t mtr_t::commit_files(lsn_t checkpoint_lsn)
{
  ut_ad(log_sys.latch_have_wr());
  ut_ad(is_active());
  ut_ad(m_log_mode == MTR_LOG_ALL);
  ut_ad(!m_made_dirty);
  ut_ad(m_memo.empty());
  ut_ad(!srv_read_only_mode);
  ut_ad(!m_freed_space);
  ut_ad(!m_freed_pages);
  ut_ad(!m_user_space);
  ut_ad(!m_latch_ex);

  m_latch_ex= true;
  ownerless_redo_enter();

  if (checkpoint_lsn)
  {
    byte *ptr= m_log.push<byte*>(3 + 8);
    *ptr= FILE_CHECKPOINT | (2 + 8);
    ::memset(ptr + 1, 0, 2);
    mach_write_to_8(ptr + 3, checkpoint_lsn);
  }

  const bool crypt{log_sys.is_encrypted()};
  m_commit_lsn= crypt ? log_sys.get_flushed_lsn() : 0;
  finish_write(crypt ? 8 + encrypt() : crc32c());

  if (m_ownerless_redo)
  {
    ut_ad(ownerless_redo_log_latch_depth != 0);
    ownerless_redo_log_latch_depth--;
    log_sys.latch.wr_unlock();
    m_latch_ex= false;
    ownerless_redo_leave();
    log_sys.latch.wr_lock(SRW_LOCK_CALL);
    m_latch_ex= true;
  }

  release_resources();

  if (checkpoint_lsn)
    DBUG_PRINT("ib_log",
               ("FILE_CHECKPOINT(" LSN_PF ") written at " LSN_PF,
                checkpoint_lsn, m_commit_lsn));

  return m_commit_lsn;
}

#ifdef UNIV_DEBUG
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool
mtr_t::is_named_space(uint32_t space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);
  return !is_logged() || m_user_space_id == space ||
    is_predefined_tablespace(space);
}
/** Check if a tablespace is associated with the mini-transaction
(needed for generating a FILE_MODIFY record)
@param[in]	space	tablespace
@return whether the mini-transaction is associated with the space */
bool mtr_t::is_named_space(const fil_space_t* space) const
{
  ut_ad(!m_user_space || m_user_space->id != TRX_SYS_SPACE);

  return !is_logged() || m_user_space == space ||
    is_predefined_tablespace(space->id);
}
#endif /* UNIV_DEBUG */

/** Acquire a tablespace X-latch.
@param[in]	space_id	tablespace ID
@return the tablespace object (never NULL) */
fil_space_t *mtr_t::x_lock_space(uint32_t space_id)
{
	fil_space_t*	space;

	ut_ad(is_active());

	if (space_id == TRX_SYS_SPACE) {
		space = fil_system.sys_space;
	} else if ((space = m_user_space) && space_id == space->id) {
	} else {
		space = fil_space_get(space_id);
		ut_ad(m_log_mode != MTR_LOG_NO_REDO
		      || space->is_temporary() || space->is_being_imported());
	}

	ut_ad(space);
	ut_ad(space->id == space_id);
	x_lock_space(space);
	return(space);
}

/** Acquire an exclusive tablespace latch.
@param space  tablespace */
void mtr_t::x_lock_space(fil_space_t *space)
{
	if (!memo_contains(*space))
	{
		ownerless_space_write_enter(space);
		memo_push(space, MTR_MEMO_SPACE_X_LOCK);
		space->x_lock();
	}
}

void mtr_t::release(const void *object)
{
  ut_ad(is_active());

  auto it=
    std::find_if(m_memo.begin(), m_memo.end(),
                 [object](const mtr_memo_slot_t& slot)
                 { return slot.object == object; });
  ut_ad(it != m_memo.end());
  ut_ad(!(it->type & MTR_MEMO_MODIFY));
  ownerless_page_write_leave(*it);
  it->release();
  ownerless_space_write_leave(*it);
  m_memo.erase(it, it + 1);
  ut_ad(std::find_if(m_memo.begin(), m_memo.end(),
                     [object](const mtr_memo_slot_t& slot)
                     { return slot.object == &object; }) == m_memo.end());
}

static time_t log_close_warn_time;

/** Display a warning that the log tail is overwriting the head,
making the server crash-unsafe. */
ATTRIBUTE_COLD static void log_overwrite_warning(lsn_t lsn)
{
  if (log_sys.overwrite_warned)
    return;

  time_t t= time(nullptr);
  if (difftime(t, log_close_warn_time) < 15)
    return;

  if (!log_sys.overwrite_warned)
    log_sys.overwrite_warned= lsn;
  log_close_warn_time= t;

  sql_print_error("InnoDB: Crash recovery is broken due to"
                  " insufficient innodb_log_file_size;"
                  " last checkpoint LSN=" LSN_PF ", current LSN=" LSN_PF
                  "%s.",
                  lsn_t{log_sys.last_checkpoint_lsn}, lsn,
                  srv_shutdown_state > SRV_SHUTDOWN_INITIATED
                  ? ". Shutdown is in progress" : "");
}

ATTRIBUTE_COLD void log_t::append_prepare_wait(bool late, bool ex) noexcept
{
  if (UNIV_LIKELY(!ex))
  {
    latch.rd_unlock();
    if (!late)
    {
      /* Wait for all threads to back off. */
      latch.wr_lock(SRW_LOCK_CALL);
      goto got_ex;
    }

    const auto delay= my_cpu_relax_multiplier / 4 * srv_spin_wait_delay;
    const auto rounds= srv_n_spin_wait_rounds;

    for (;;)
    {
      HMT_low();
      for (auto r= rounds + 1; r--; )
      {
        if (write_lsn_offset.load(std::memory_order_relaxed) & WRITE_BACKOFF)
        {
          for (auto d= delay; d--; )
            MY_RELAX_CPU();
        }
        else
        {
          HMT_medium();
          goto done;
        }
      }
      HMT_medium();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  else
  {
  got_ex:
    const uint64_t l= write_lsn_offset.load(std::memory_order_relaxed);
    const lsn_t lsn= base_lsn.load(std::memory_order_relaxed) +
      (l & (WRITE_BACKOFF - 1));
    waits++;
#ifdef HAVE_PMEM
    const bool is_pmem{is_mmap()};
    if (is_pmem)
    {
      ut_ad(lsn - get_flushed_lsn(std::memory_order_relaxed) < capacity() ||
            overwrite_warned);
      persist(lsn);
    }
#endif
    latch.wr_unlock();
    /* write_buf() or persist() will clear the WRITE_BACKOFF flag,
    which our caller will recheck. */
#ifdef HAVE_PMEM
    if (!is_pmem)
#endif
    log_write_up_to(lsn, false);
    if (ex)
    {
      latch.wr_lock(SRW_LOCK_CALL);
      return;
    }
  }

done:
  latch.rd_lock(SRW_LOCK_CALL);
}

/** Reserve space in the log buffer for appending data.
@tparam mmap  log_sys.is_mmap()
@param size   total length of the data to append(), in bytes
@param ex     whether log_sys.latch is exclusively locked
@return the start LSN and the buffer position for append() */
template<bool mmap>
inline
std::pair<lsn_t,byte*> log_t::append_prepare(size_t size, bool ex) noexcept
{
  ut_ad(ex ? latch_have_wr() : latch_have_rd());
  ut_ad(mmap == is_mmap());
  ut_ad(!mmap || buf_size == std::min<uint64_t>(capacity(), buf_size_max));
  const size_t buf_size{this->buf_size - size};
  uint64_t l;
  static_assert(WRITE_TO_BUF == WRITE_BACKOFF << 1, "");
  while (UNIV_UNLIKELY((l= write_lsn_offset.fetch_add(size + WRITE_TO_BUF) &
                        (WRITE_TO_BUF - 1)) >= buf_size))
  {
    /* The following is inlined here instead of being part of
    append_prepare_wait(), in order to increase the locality of reference
    and to set the WRITE_BACKOFF flag as soon as possible. */
    bool late(write_lsn_offset.fetch_or(WRITE_BACKOFF) & WRITE_BACKOFF);
    /* Subtract our LSN overshoot. */
    write_lsn_offset.fetch_sub(size);
    append_prepare_wait(late, ex);
  }

  const lsn_t lsn{l + base_lsn.load(std::memory_order_relaxed)},
    end_lsn{lsn + size};

  if (UNIV_UNLIKELY(end_lsn >= last_checkpoint_lsn + log_capacity))
    set_check_for_checkpoint(true);

  return {lsn,
          buf + size_t(mmap ? FIRST_LSN + (lsn - first_lsn) % capacity() : l)};
}

/** Finish appending data to the log.
@param lsn  the end LSN of the log record
@return lsn for invoking buf_flush_ahead() on, with "furious" flag in the LSB
@retval 0 if buf_flush_ahead() will not have to be invoked */
static lsn_t log_close(lsn_t lsn) noexcept
{
  ut_ad(log_sys.latch_have_any());

  const lsn_t checkpoint_age= lsn - log_sys.last_checkpoint_lsn;
  const lsn_t max_age= log_sys.max_modified_age_async;

  if (UNIV_UNLIKELY(checkpoint_age >= log_sys.log_capacity) &&
      /* silence message on create_log_file() after the log had been deleted */
      checkpoint_age != lsn)
    log_overwrite_warning(lsn);
  else if (UNIV_LIKELY(checkpoint_age <= max_age))
    return 0;

  /* The last checkpoint is too old. Let us set an appropriate
  checkpoint age target, that is, a checkpoint LSN target that is the
  current LSN minus the maximum age. Let us see if are exceeding the
  log_checkpoint_margin() limit that will involve a synchronous wait
  in each write operation. */

  const bool furious{checkpoint_age >= log_sys.max_checkpoint_age};

  /* If furious==true, we could set a less aggressive target
  (lsn - log_sys.max_checkpoint_age) instead of what we will be using
  in both cases (lsn - log_sys.max_checkpoint_age_async).

  The aim of the more aggressive target is that mtr_flush_ahead() will
  request more progress in buf_flush_page_cleaner() sooner, so that it
  will be less likely that several threads will end up waiting in
  log_checkpoint_margin(). That function will use the less aggressive
  limit (lsn - log_sys.max_checkpoint_age) in order to minimize the
  synchronous wait time. */
  if (furious)
    log_sys.set_check_for_checkpoint();

  return ((lsn - max_age) & ~lsn_t{1}) | lsn_t{furious};
}

inline void mtr_t::page_checksum(const buf_page_t &bpage)
{
  const byte *page= bpage.frame;
  size_t size= srv_page_size;

  if (UNIV_LIKELY_NULL(bpage.zip.data))
  {
    size= (UNIV_ZIP_SIZE_MIN >> 1) << bpage.zip.ssize;
    switch (fil_page_get_type(bpage.zip.data)) {
    case FIL_PAGE_TYPE_ALLOCATED:
    case FIL_PAGE_INODE:
    case FIL_PAGE_IBUF_BITMAP:
    case FIL_PAGE_TYPE_FSP_HDR:
    case FIL_PAGE_TYPE_XDES:
      /* These are essentially uncompressed pages. */
      break;
    default:
      page= bpage.zip.data;
    }
  }

  /* We have to exclude from the checksum the normal
  page checksum that is written by buf_flush_init_for_writing()
  and FIL_PAGE_LSN which would be updated once we have actually
  allocated the LSN.

  Unfortunately, we cannot access fil_space_t easily here. In order to
  be compatible with encrypted tablespaces in the pre-full_crc32
  format we will unconditionally exclude the 8 bytes at
  FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
  a.k.a. FIL_RTREE_SPLIT_SEQ_NUM. */
  const uint32_t checksum=
    my_crc32c(my_crc32c(my_crc32c(0, page + FIL_PAGE_OFFSET,
                                  FIL_PAGE_LSN - FIL_PAGE_OFFSET),
                        page + FIL_PAGE_TYPE, 2),
              page + FIL_PAGE_SPACE_ID, size - (FIL_PAGE_SPACE_ID + 8));

  byte *l= log_write<OPTION>(bpage.id(), nullptr, 5, true, 0);
  *l++= OPT_PAGE_CHECKSUM;
  mach_write_to_4(l, checksum);
  m_log.close(l + 4);
}

std::pair<lsn_t,lsn_t> mtr_t::do_write() noexcept
{
  ut_ad(!recv_no_log_write);
  ut_ad(is_logged());
  ut_ad(!m_log.empty());
  ut_ad(!m_latch_ex || log_sys.latch_have_wr());
  ut_ad(!m_user_space ||
        (m_user_space->id > 0 && m_user_space->id < SRV_SPACE_ID_UPPER_BOUND));
  m_commit_lsn= 0;

#ifndef DBUG_OFF
  do
  {
    if (m_log_mode != MTR_LOG_ALL ||
        _db_keyword_(nullptr, "skip_page_checksum", 1))
      continue;
    for (const mtr_memo_slot_t& slot : m_memo)
      if (slot.type & MTR_MEMO_MODIFY)
      {
        const buf_page_t &b= *static_cast<const buf_page_t*>(slot.object);
        if (!b.is_freed())
          page_checksum(b);
      }
  }
  while (0);
#endif
  const size_t len{log_sys.is_encrypted() ? 8 + encrypt() : crc32c()};

  ownerless_redo_enter();

  if (!m_latch_ex && !m_ownerless_redo_borrowed_latch)
    log_sys.latch.rd_lock(SRW_LOCK_CALL);

  if (UNIV_UNLIKELY(m_user_space && !m_user_space->max_lsn &&
                    !srv_is_undo_tablespace((m_user_space->id))))
  {
    if (!m_latch_ex)
    {
      m_latch_ex= true;
      log_sys.latch.rd_unlock();
      log_sys.latch.wr_lock(SRW_LOCK_CALL);
      if (UNIV_UNLIKELY(m_user_space->max_lsn != 0))
        goto func_exit;
    }
    name_write();
  }
func_exit:
  return finish_write(len);
}

inline void log_t::resize_write(lsn_t lsn, const byte *end, size_t len,
                                size_t seq) noexcept
{
  ut_ad(latch_have_any());

  if (UNIV_LIKELY_NULL(resize_buf))
  {
    ut_ad(end >= buf);
    end-= len;
    size_t s;

#ifdef HAVE_PMEM
    if (!resize_flush_buf)
    {
      ut_ad(is_mmap());
      resize_wrap_mutex.wr_lock();
      const size_t resize_capacity{resize_target - START_OFFSET};
      {
        const lsn_t resizing{resize_in_progress()};
        /* For memory-mapped log, log_t::resize_start() would never
        set log_sys.resize_lsn to less than log_sys.lsn. It cannot
        execute concurrently with this thread, because we are holding
        log_sys.latch and it would hold an exclusive log_sys.latch. */
        if (UNIV_UNLIKELY(lsn < resizing))
        {
          /* This function may execute in multiple concurrent threads
          that hold a shared log_sys.latch. Before we got resize_wrap_mutex,
          another thread could have executed resize_lsn.store(lsn) below
          with a larger lsn than ours.

          append_prepare() guarantees that the concurrent writes
          cannot overlap, that is, our entire log must be discarded.
          Besides, incomplete mini-transactions cannot be parsed anyway. */
          ut_ad(resizing >= lsn + len);
          goto mmap_done;
        }

        s= START_OFFSET;

        if (UNIV_UNLIKELY(lsn - resizing + len >= resize_capacity))
        {
          resize_lsn.store(lsn, std::memory_order_relaxed);
          lsn= 0;
        }
        else
        {
          lsn-= resizing;
          s+= lsn;
        }
      }

      ut_ad(s + len <= resize_target);

      if (UNIV_UNLIKELY(end < &buf[START_OFFSET]))
      {
        /* The source buffer (log_sys.buf) wrapped around */
        ut_ad(end + capacity() < &buf[file_size]);
        ut_ad(end + len >= &buf[START_OFFSET]);
        ut_ad(end + capacity() + len >= &buf[file_size]);

        size_t l= size_t(buf - (end - START_OFFSET));
        memcpy(resize_buf + s, end + capacity(), l);
        memcpy(resize_buf + s + l, &buf[START_OFFSET], len - l);
      }
      else
      {
        ut_ad(end + len <= &buf[file_size]);
        memcpy(resize_buf + s, end, len);
      }
      s+= len - seq;

      /* Always set the sequence bit. If the resized log were to wrap around,
      we will advance resize_lsn. */
      ut_ad(resize_buf[s] <= 1);
      resize_buf[s]= 1;
    mmap_done:
      resize_wrap_mutex.wr_unlock();
    }
    else
#endif
    {
      ut_ad(resize_flush_buf);
      s= end - buf;
      ut_ad(s + len <= buf_size);
      memcpy(resize_buf + s, end, len);
      s+= len - seq;
      /* Always set the sequence bit. If the resized log were to wrap around,
      we will advance resize_lsn. */
      ut_ad(resize_buf[s] <= 1);
      resize_buf[s]= 1;
    }
  }
}

inline void log_t::append(byte *&d, const void *s, size_t size) noexcept
{
  ut_ad(log_sys.latch_have_any());
  ut_ad(d + size <= log_sys.buf +
        (log_sys.is_mmap() ? log_sys.file_size : log_sys.buf_size));
  memcpy(d, s, size);
  d+= size;
}

template<bool mmap>
std::pair<lsn_t,lsn_t> mtr_t::finish_writer(mtr_t *mtr, size_t len)
{
  ut_ad(log_sys.is_latest());
  ut_ad(!recv_no_log_write);
  ut_ad(mtr->is_logged());
  const bool append_ex=
    mtr->m_latch_ex || mtr->m_ownerless_redo_borrowed_latch;
  ut_ad(append_ex ? log_sys.latch_have_wr() : log_sys.latch_have_rd());
  ut_ad(len < recv_sys.MTR_SIZE_MAX);

  const size_t size{mtr->m_commit_lsn ? 5U + 8U : 5U};
  uint64_t ownerless_start_lsn= 0;
  uint64_t ownerless_end_lsn= 0;
  if (mtr->m_ownerless_redo)
  {
    const lsn_t current_lsn= append_ex
      ? log_sys.get_lsn()
      : log_sys.get_lsn_approx();
    const int result= mylite_ownerless_innodb_redo_reserve(
      current_lsn, len, &ownerless_start_lsn, &ownerless_end_lsn);
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
      ut_error;
    if (ownerless_start_lsn < current_lsn)
      ut_error;
    if (ownerless_start_lsn > current_lsn)
      log_sys.set_recovered_lsn(ownerless_start_lsn);
  }

  std::pair<lsn_t, byte*> start=
    log_sys.append_prepare<mmap>(len, append_ex);
  if (mtr->m_ownerless_redo &&
      (ownerless_start_lsn != start.first ||
       ownerless_end_lsn != start.first + len))
    ut_error;
  if (mtr->m_ownerless_redo)
  {
    mtr->m_ownerless_redo_start_lsn= ownerless_start_lsn;
    mtr->m_ownerless_redo_end_lsn= ownerless_end_lsn;
  }

  if (!mmap)
  {
    for (const mtr_buf_t::block_t &b : mtr->m_log)
      log_sys.append(start.second, b.begin(), b.used());

  write_trailer:
    *start.second++= log_sys.get_sequence_bit(start.first + len - size);
    if (mtr->m_commit_lsn)
    {
      mach_write_to_8(start.second, mtr->m_commit_lsn);
      mtr->m_crc= my_crc32c(mtr->m_crc, start.second, 8);
      start.second+= 8;
    }
    mach_write_to_4(start.second, mtr->m_crc);
    start.second+= 4;
  }
  else
  {
    if (UNIV_LIKELY(start.second + len <= &log_sys.buf[log_sys.file_size]))
    {
      for (const mtr_buf_t::block_t &b : mtr->m_log)
        log_sys.append(start.second, b.begin(), b.used());
      goto write_trailer;
    }
    for (const mtr_buf_t::block_t &b : mtr->m_log)
    {
      size_t size{b.used()};
      const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
      const byte *src= b.begin();
      if (size > size_left)
      {
        ::memcpy(start.second, src, size_left);
        start.second= &log_sys.buf[log_sys.START_OFFSET];
        src+= size_left;
        size-= size_left;
      }
      ::memcpy(start.second, src, size);
      start.second+= size;
    }
    const size_t size_left(&log_sys.buf[log_sys.file_size] - start.second);
    if (size_left > size)
      goto write_trailer;

    byte tail[5 + 8];
    tail[0]= log_sys.get_sequence_bit(start.first + len - size);

    if (mtr->m_commit_lsn)
    {
      mach_write_to_8(tail + 1, mtr->m_commit_lsn);
      mtr->m_crc= my_crc32c(mtr->m_crc, tail + 1, 8);
      mach_write_to_4(tail + 9, mtr->m_crc);
    }
    else
      mach_write_to_4(tail + 1, mtr->m_crc);

    ::memcpy(start.second, tail, size_left);
    ::memcpy(log_sys.buf + log_sys.START_OFFSET, tail + size_left,
             size - size_left);
    start.second= log_sys.buf +
      ((size >= size_left) ? log_sys.START_OFFSET : log_sys.file_size) +
      (size - size_left);
  }

  log_sys.resize_write(start.first, start.second, len, size);

  mtr->m_commit_lsn= start.first + len;
  return {start.first, log_close(mtr->m_commit_lsn)};
}

bool mtr_t::have_x_latch(const buf_block_t &block) const
{
  ut_d(const mtr_memo_slot_t *found= nullptr);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object != &block)
      continue;

    ut_d(found= &slot);

    if (!(slot.type & MTR_MEMO_PAGE_X_FIX))
      continue;

    ut_ad(block.page.lock.have_x());
    return true;
  }

  ut_ad(!found);
  return false;
}

bool mtr_t::have_u_or_x_latch(const buf_block_t &block) const
{
  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &block &&
        slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX))
    {
      ut_ad(block.page.lock.have_u_or_x());
      return true;
    }
  }
  return false;
}

/** Check if we are holding exclusive tablespace latch
@param space  tablespace to search for
@return whether space.latch is being held */
bool mtr_t::memo_contains(const fil_space_t& space) const
{
  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &space && slot.type == MTR_MEMO_SPACE_X_LOCK)
    {
      ut_ad(space.is_owner());
      return true;
    }
  }

  return false;
}

buf_block_t *mtr_t::page_lock_upgrade(const buf_block_t &block) noexcept
{
  ut_ad(block.page.lock.have_x());

  for (mtr_memo_slot_t &slot : m_memo)
    if (slot.object == &block && slot.type & MTR_MEMO_PAGE_SX_FIX)
      slot.type= mtr_memo_type_t(slot.type ^
                                 (MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX));

  if (mylite_ownerless_innodb_lock_has_hooks() &&
      ownerless_page_write_should_prepare(block.page))
    ownerless_page_write_enter(block);

#ifdef BTR_CUR_HASH_ADAPT
  ut_d(if (dict_index_t *index= block.index))
  ut_ad(!index->freed());
#endif /* BTR_CUR_HASH_ADAPT */
  return const_cast<buf_block_t*>(&block);
}

buf_block_t *mtr_t::page_lock(buf_block_t *block, ulint rw_latch) noexcept
{
  mtr_memo_type_t fix_type;
  ut_d(const auto state= block->page.state());
  ut_ad(state > buf_page_t::FREED);
  ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);
  switch (rw_latch) {
  case RW_NO_LATCH:
    fix_type= MTR_MEMO_BUF_FIX;
    goto done;
  case RW_S_LATCH:
    fix_type= MTR_MEMO_PAGE_S_FIX;
    block->page.lock.s_lock();
    break;
  case RW_SX_LATCH:
    fix_type= MTR_MEMO_PAGE_SX_FIX;
    block->page.lock.u_lock();
    ut_ad(!block->page.is_io_fixed());
    break;
  default:
    ut_ad(rw_latch == RW_X_LATCH);
    fix_type= MTR_MEMO_PAGE_X_FIX;
    if (block->page.lock.x_lock_upgraded())
    {
      block->unfix();
      return page_lock_upgrade(*block);
    }
    ut_ad(!block->page.is_io_fixed());
  }

done:
  ut_ad(state < buf_page_t::UNFIXED ||
        page_id_t(page_get_space_id(block->page.frame),
                  page_get_page_no(block->page.frame)) == block->page.id());
  memo_push(block, fix_type);
  return block;
}

void mtr_t::upgrade_buffer_fix(ulint savepoint, rw_lock_type_t rw_latch)
  noexcept
{
  ut_ad(is_active());
  mtr_memo_slot_t &slot= m_memo[savepoint];
  ut_ad(slot.type == MTR_MEMO_BUF_FIX);
  buf_block_t *block= static_cast<buf_block_t*>(slot.object);
  ut_d(const auto state= block->page.state());
  ut_ad(state > buf_page_t::FREED);
  ut_ad(state > buf_page_t::WRITE_FIX || state < buf_page_t::READ_FIX);
  static_assert(int{MTR_MEMO_PAGE_S_FIX} == int{RW_S_LATCH}, "");
  static_assert(int{MTR_MEMO_PAGE_X_FIX} == int{RW_X_LATCH}, "");
  static_assert(int{MTR_MEMO_PAGE_SX_FIX} == int{RW_SX_LATCH}, "");
  slot.type= mtr_memo_type_t(rw_latch);

  switch (rw_latch) {
  default:
    ut_ad("invalid state" == 0);
    break;
  case RW_S_LATCH:
    block->page.lock.s_lock();
    break;
  case RW_SX_LATCH:
    block->page.lock.u_lock();
    ut_ad(!block->page.is_io_fixed());
    break;
  case RW_X_LATCH:
    block->page.lock.x_lock();
    ut_ad(!block->page.is_io_fixed());
  }

  ut_ad(page_id_t(page_get_space_id(block->page.frame),
                  page_get_page_no(block->page.frame)) == block->page.id());
  if (mylite_ownerless_innodb_lock_has_hooks() &&
      (slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX)) &&
      ownerless_page_write_should_prepare(block->page))
    ownerless_page_write_enter(*block);
}

#ifdef UNIV_DEBUG
/** Check if we are holding an rw-latch in this mini-transaction
@param lock   latch to search for
@param type   held latch type
@return whether (lock,type) is contained */
bool mtr_t::memo_contains(const index_lock &lock, mtr_memo_type_t type) const
{
  ut_ad(type == MTR_MEMO_X_LOCK || type == MTR_MEMO_S_LOCK ||
        type == MTR_MEMO_SX_LOCK);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &lock && slot.type == type)
    {
      switch (type) {
      case MTR_MEMO_X_LOCK:
        ut_ad(lock.have_x());
        break;
      case MTR_MEMO_SX_LOCK:
        ut_ad(lock.have_u_or_x());
        break;
      case MTR_MEMO_S_LOCK:
        ut_ad(lock.have_s());
        break;
      default:
        break;
      }
      return true;
    }
  }

  return false;
}

/** Check if memo contains the given item.
@param object		object to search
@param flags		specify types of object (can be ORred) of
			MTR_MEMO_PAGE_S_FIX ... values
@return true if contains */
bool mtr_t::memo_contains_flagged(const void *object, ulint flags) const
{
  ut_ad(is_active());
  ut_ad(flags);
  /* Look for rw-lock-related and page-related flags. */
  ut_ad(!(flags & ulint(~(MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_X_FIX |
                          MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_BUF_FIX |
                          MTR_MEMO_MODIFY | MTR_MEMO_X_LOCK |
                          MTR_MEMO_SX_LOCK | MTR_MEMO_S_LOCK))));
  /* Either some rw-lock-related or page-related flags
  must be specified, but not both at the same time. */
  ut_ad(!(flags & (MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_X_FIX |
                   MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_BUF_FIX |
                   MTR_MEMO_MODIFY)) ==
        !!(flags & (MTR_MEMO_X_LOCK | MTR_MEMO_SX_LOCK | MTR_MEMO_S_LOCK)));

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    if (object != slot.object)
      continue;

    auto f = flags & slot.type;
    if (!f)
      continue;

    if (f & (MTR_MEMO_PAGE_S_FIX | MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX))
    {
      const block_lock &lock= static_cast<const buf_page_t*>(object)->lock;
      ut_ad(!(f & MTR_MEMO_PAGE_S_FIX) || lock.have_s());
      ut_ad(!(f & MTR_MEMO_PAGE_SX_FIX) || lock.have_u_or_x());
      ut_ad(!(f & MTR_MEMO_PAGE_X_FIX) || lock.have_x());
    }
    else
    {
      const index_lock &lock= *static_cast<const index_lock*>(object);
      ut_ad(!(f & MTR_MEMO_S_LOCK) || lock.have_s());
      ut_ad(!(f & MTR_MEMO_SX_LOCK) || lock.have_u_or_x());
      ut_ad(!(f & MTR_MEMO_X_LOCK) || lock.have_x());
    }

    return true;
  }

  return false;
}

buf_block_t* mtr_t::memo_contains_page_flagged(const byte *ptr, ulint flags)
  const
{
  ptr= page_align(ptr);

  for (const mtr_memo_slot_t &slot : m_memo)
  {
    ut_ad(slot.object);
    if (!(flags & slot.type))
      continue;

    buf_page_t *bpage= static_cast<buf_page_t*>(slot.object);

    if (ptr != bpage->frame)
      continue;

    ut_ad(!(slot.type & MTR_MEMO_PAGE_S_FIX) || bpage->lock.have_s());
    ut_ad(!(slot.type & MTR_MEMO_PAGE_SX_FIX) || bpage->lock.have_u_or_x());
    ut_ad(!(slot.type & MTR_MEMO_PAGE_X_FIX) || bpage->lock.have_x());
    return static_cast<buf_block_t*>(slot.object);
  }

  return nullptr;
}
#endif /* UNIV_DEBUG */


/** Mark the given latched page as modified.
@param block   page that will be modified */
void mtr_t::set_modified(const buf_block_t &block)
{
  if (block.page.id().space() >= SRV_TMP_SPACE_ID)
  {
    const_cast<buf_block_t&>(block).page.set_temp_modified();
    return;
  }

  if (mylite_ownerless_innodb_lock_has_hooks())
  {
    bool ownerless_page_write_modified= false;
    for (const mtr_memo_slot_t &slot : m_memo)
    {
      if (slot.object == &block && slot.type & MTR_MEMO_MODIFY)
      {
        ownerless_page_write_modified= true;
        break;
      }
    }
    if (!ownerless_page_write_modified)
      ownerless_page_write_enter(block);
  }

  if (ownerless_page_write_uses_transaction_release())
    ownerless_page_write_note_transaction_page(block.page);
  m_modifications= true;

  if (UNIV_UNLIKELY(m_log_mode == MTR_LOG_NONE))
    return;

  for (mtr_memo_slot_t &slot : m_memo)
  {
    if (slot.object == &block &&
        slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX))
    {
      if (slot.type & MTR_MEMO_MODIFY)
        ut_ad(m_made_dirty || block.page.oldest_modification() > 1);
      else
      {
        slot.type= static_cast<mtr_memo_type_t>(slot.type | MTR_MEMO_MODIFY);
        if (!m_made_dirty)
          m_made_dirty= block.page.oldest_modification() <= 1;
      }
      return;
    }
  }

  /* This must be PageConverter::update_page() in IMPORT TABLESPACE. */
  ut_ad(m_memo.empty());
  ut_ad(!block.page.in_LRU_list);
}

void mtr_t::init(buf_block_t *b)
{
  const page_id_t id{b->page.id()};
  ut_ad(is_named_space(id.space()));
  ut_ad(!m_freed_pages == !m_freed_space);
  ut_ad(memo_contains_flagged(b, MTR_MEMO_PAGE_X_FIX));

  if (id.space() >= SRV_TMP_SPACE_ID)
    b->page.set_temp_modified();
  else
  {
    for (mtr_memo_slot_t &slot : m_memo)
    {
      if (slot.object == b && slot.type & MTR_MEMO_PAGE_X_FIX)
      {
        if (mylite_ownerless_innodb_lock_has_hooks())
          ownerless_page_write_enter(*b);
        slot.type= MTR_MEMO_PAGE_X_MODIFY;
        m_modifications= true;
        if (ownerless_page_write_uses_transaction_release())
          ownerless_page_write_note_transaction_page(b->page);
        if (!m_made_dirty)
          m_made_dirty= b->page.oldest_modification() <= 1;
        goto found;
      }
    }
    ut_ad("block not X-latched" == 0);
  }

 found:
  if (UNIV_LIKELY_NULL(m_freed_space) &&
      m_freed_space->id == id.space() &&
      m_freed_pages->remove_if_exists(id.page_no()) &&
      m_freed_pages->empty())
  {
    delete m_freed_pages;
    m_freed_pages= nullptr;
    m_freed_space= nullptr;
  }

  b->page.set_reinit(b->page.state() & buf_page_t::LRU_MASK);

  if (!is_logged())
    return;

  m_log.close(log_write<INIT_PAGE>(id, &b->page));
  m_last_offset= FIL_PAGE_TYPE;
}

/** Free a page.
@param space   tablespace
@param offset  offset of the page to be freed */
void mtr_t::free(const fil_space_t &space, uint32_t offset)
{
  ut_ad(is_named_space(&space));
  ut_ad(!m_freed_space || m_freed_space == &space);

  buf_block_t *freed= nullptr;
  const page_id_t id{space.id, offset};

  for (auto it= m_memo.end(); it != m_memo.begin(); )
  {
    it--;
  next:
    mtr_memo_slot_t &slot= *it;
    buf_block_t *block= static_cast<buf_block_t*>(slot.object);
    ut_ad(block);
    if (block == freed)
    {
      if (slot.type & (MTR_MEMO_PAGE_SX_FIX | MTR_MEMO_PAGE_X_FIX))
        slot.type= MTR_MEMO_PAGE_X_FIX;
      else
      {
        ut_ad(slot.type == MTR_MEMO_BUF_FIX);
        block->page.unfix();
        m_memo.erase(it, it + 1);
        goto next;
      }
    }
    else if (slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX) &&
             block->page.id() == id)
    {
      ut_ad(!block->page.is_freed());
      ut_ad(!freed);
      freed= block;
      if (!(slot.type & MTR_MEMO_PAGE_X_FIX))
      {
        ut_d(bool upgraded=) block->page.lock.x_lock_upgraded();
        ut_ad(upgraded);
      }
      if (id.space() >= SRV_TMP_SPACE_ID)
      {
        block->page.set_temp_modified();
        slot.type= MTR_MEMO_PAGE_X_FIX;
      }
      else
      {
        slot.type= MTR_MEMO_PAGE_X_MODIFY;
        if (ownerless_page_write_uses_transaction_release())
          ownerless_page_write_note_transaction_page(block->page);
        if (!m_made_dirty)
          m_made_dirty= block->page.oldest_modification() <= 1;
      }
#ifdef BTR_CUR_HASH_ADAPT
      if (block->index)
        btr_search_drop_page_hash_index(block, nullptr);
#endif /* BTR_CUR_HASH_ADAPT */
      block->page.set_freed(block->page.state());
    }
  }

  if (is_logged())
    m_log.close(log_write<FREE_PAGE>(id, nullptr));
}

void small_vector_base::grow_by_1(void *small, size_t element_size) noexcept
{
  const size_t cap= Capacity*= 2, s= cap * element_size;
  void *new_begin;
  if (BeginX == small)
  {
    new_begin= my_malloc(PSI_NOT_INSTRUMENTED, s, MYF(0));
    memcpy(new_begin, BeginX, s / 2);
    TRASH_FREE(small, size() * element_size);
  }
  else
    new_begin= my_realloc(PSI_NOT_INSTRUMENTED, BeginX, s, MYF(0));

  BeginX= new_begin;
}
