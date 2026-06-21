/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2015, 2023, MariaDB Corporation.

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
@file trx/trx0trx.cc
The transaction

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#define MYSQL_SERVER
#include "trx0trx.h"
#include "sql_class.h" // THD

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>
#endif

#include <mysql/service_thd_error_context.h>

#include "btr0sea.h"
#include "dict0dict.h"
#include "lock0lock.h"
#include "log0log.h"
#include "mylite_embedded_startup_perf.h"
#include "mylite_ownerless_innodb_deep_perf.h"
#include "mylite_ownerless_innodb_lock_hooks.h"
#include "que0que.h"
#include "srv0mon.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0undo.h"
#include "trx0xa.h"
#include "ut0pool.h"
#include "ut0vec.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>

/** The bit pattern corresponding to TRX_ID_MAX */
const byte trx_id_max_bytes[8] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

#if SIZEOF_VOIDP == 4
/* Max timestamp before 11.3 */
const byte timestamp_max_bytes[7] = {
	0x7f, 0xff, 0xff, 0xff, 0x0f, 0x42, 0x3f
};
#else
/** The bit pattern corresponding to max timestamp */
const byte timestamp_max_bytes[7] = {
	0xff, 0xff, 0xff, 0xff, 0x0f, 0x42, 0x3f
};
#endif /* SIZEOF_VOIDP */

static const ulint MAX_DETAILED_ERROR_LEN = 512;

namespace {

const size_t MYLITE_OWNERLESS_PAGE_SET_MIN_PAGES = 16;
const uint64_t MYLITE_OWNERLESS_PAGE_SET_EMPTY =
    std::numeric_limits<uint64_t>::max();

size_t mylite_ownerless_page_set_hash(uint64_t value) noexcept
{
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return static_cast<size_t>(value);
}

size_t mylite_ownerless_page_set_capacity(size_t page_count) noexcept
{
  size_t capacity= 64;
  while (capacity < page_count * 2)
    capacity <<= 1;
  return capacity;
}

bool mylite_ownerless_page_set_contains(
    const trx_t::mylite_ownerless_page_set &page_set,
    uint64_t packed_page) noexcept
{
  ut_ad(packed_page != MYLITE_OWNERLESS_PAGE_SET_EMPTY);
  ut_ad(!page_set.empty());
  const size_t mask= page_set.size() - 1;
  size_t slot= mylite_ownerless_page_set_hash(packed_page) & mask;
  for (;;)
  {
    const uint64_t candidate= page_set[slot];
    if (candidate == packed_page)
      return true;
    if (candidate == MYLITE_OWNERLESS_PAGE_SET_EMPTY)
      return false;
    slot= (slot + 1) & mask;
  }
}

void mylite_ownerless_page_set_insert(
    trx_t::mylite_ownerless_page_set &page_set,
    uint64_t packed_page) noexcept
{
  if (packed_page == MYLITE_OWNERLESS_PAGE_SET_EMPTY)
    return;
  const size_t mask= page_set.size() - 1;
  size_t slot= mylite_ownerless_page_set_hash(packed_page) & mask;
  for (;;)
  {
    uint64_t &candidate= page_set[slot];
    if (candidate == packed_page)
      return;
    if (candidate == MYLITE_OWNERLESS_PAGE_SET_EMPTY)
    {
      candidate= packed_page;
      return;
    }
    slot= (slot + 1) & mask;
  }
}

void mylite_ownerless_page_set_rebuild(
    trx_t::mylite_ownerless_page_set &page_set,
    const trx_t::mylite_ownerless_page_vector *pages) noexcept
{
  if (pages == nullptr ||
      pages->size() < MYLITE_OWNERLESS_PAGE_SET_MIN_PAGES)
  {
    page_set.clear();
    return;
  }

  page_set.assign(
      mylite_ownerless_page_set_capacity(pages->size()),
      MYLITE_OWNERLESS_PAGE_SET_EMPTY);
  for (uint64_t packed_page : *pages)
    mylite_ownerless_page_set_insert(page_set, packed_page);
}

trx_t::mylite_ownerless_page_set &mylite_ownerless_page_set_for_write(
    trx_t::mylite_ownerless_page_set *&page_set) noexcept
{
  if (page_set == nullptr)
  {
    page_set= UT_NEW_NOKEY(trx_t::mylite_ownerless_page_set());
    ut_a(page_set != nullptr);
  }
  return *page_set;
}

bool mylite_ownerless_page_vector_contains(
    const trx_t::mylite_ownerless_page_vector *pages,
    const trx_t::mylite_ownerless_page_set *page_set,
    uint64_t packed_page) noexcept
{
  if (packed_page != MYLITE_OWNERLESS_PAGE_SET_EMPTY &&
      page_set != nullptr && !page_set->empty())
    return mylite_ownerless_page_set_contains(*page_set, packed_page);

  return pages != nullptr &&
         std::find(pages->begin(), pages->end(), packed_page) != pages->end();
}

void mylite_ownerless_note_page(
    trx_t::mylite_ownerless_page_vector &pages,
    trx_t::mylite_ownerless_page_set *&page_set,
    uint64_t packed_page) noexcept
{
  pages.push_back(packed_page);
  if (page_set != nullptr && !page_set->empty())
  {
    if (pages.size() > page_set->size() / 2)
    {
      mylite_ownerless_page_set_rebuild(*page_set, &pages);
      return;
    }
    mylite_ownerless_page_set_insert(*page_set, packed_page);
    return;
  }
  if (pages.size() >= MYLITE_OWNERLESS_PAGE_SET_MIN_PAGES)
    mylite_ownerless_page_set_rebuild(
        mylite_ownerless_page_set_for_write(page_set), &pages);
}

} /* namespace */

static std::atomic<bool> ownerless_commit_visibility_stats_enabled{false};
static std::atomic<uint64_t> ownerless_commit_visibility_fast{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_recovery_lsn{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_dirty_pages{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_no_page_write_trx{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_deferred_pages{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_publish_failed{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_no_published_pages{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_unproven_statement{0};
static std::atomic<uint64_t> ownerless_commit_visibility_log_flush_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_total_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_publish_transaction_pages_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_publish_dirty_pages_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_flush_dirty_pages_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_publish_visible_ns{0};
static std::atomic<uint64_t> ownerless_commit_visibility_release_locks_ns{0};
std::atomic<bool> mylite_ownerless_innodb_deep_perf_stats_enabled_flag{false};
static std::atomic<uint64_t> ownerless_innodb_deep_perf_stats
    [MYLITE_OWNERLESS_INNODB_DEEP_PERF_STAT_COUNT];

static void ownerless_commit_visibility_count_if_enabled(
    bool stats_enabled,
    std::atomic<uint64_t> &counter) noexcept
{
  if (stats_enabled)
    counter.fetch_add(1, std::memory_order_relaxed);
}

static uint64_t ownerless_commit_visibility_now_ns() noexcept
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void ownerless_commit_visibility_add_elapsed(
    std::atomic<uint64_t> &counter, uint64_t start_ns) noexcept
{
  if (start_ns == 0)
    return;
  if (ownerless_commit_visibility_stats_enabled.load(
          std::memory_order_relaxed))
    counter.fetch_add(ownerless_commit_visibility_now_ns() - start_ns,
                      std::memory_order_relaxed);
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

static bool ownerless_transaction_has_deferred_page_writes(
    const trx_t *trx)
{
  if (trx == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_vector *vectors[]=
  {
    trx->mylite_ownerless_dirty_pages_for_read(),
    trx->mylite_ownerless_modified_pages_for_read()
  };

  for (const trx_t::mylite_ownerless_page_vector *pages : vectors)
  {
    if (pages == nullptr)
      continue;
    for (uint64_t packed_page : *pages)
      if (!ownerless_page_write_is_transaction_gate(packed_page))
        return true;
  }

  return false;
}

static bool ownerless_sql_command_requires_dirty_page_bridge(const trx_t *trx)
{
  if (trx == nullptr || trx->mysql_thd == nullptr ||
      trx->mysql_thd->lex == nullptr)
    return false;

  switch (trx->mysql_thd->lex->sql_command)
  {
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_INDEX:
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_TRUNCATE:
  case SQLCOM_DROP_TABLE:
  case SQLCOM_DROP_INDEX:
  case SQLCOM_RENAME_TABLE:
    return true;
  default:
    return false;
  }
}

static bool ownerless_sql_command_is_direct_insert(const trx_t *trx)
{
  return trx != nullptr && trx->mysql_thd != nullptr &&
         trx->mysql_thd->lex != nullptr &&
         trx->mysql_thd->lex->sql_command == SQLCOM_INSERT;
}

static bool ownerless_sql_command_allows_default_checked_bulk_insert(
    const trx_t *trx)
{
  return ownerless_sql_command_is_direct_insert(trx) &&
         mylite_ownerless_innodb_statement_deferred_page_publish() != 0;
}

static bool ownerless_sql_command_allows_visible_fast_path(const trx_t *trx)
{
  return trx != nullptr &&
         mylite_ownerless_innodb_statement_visible_fast_path() != 0;
}

static bool ownerless_history_wal_proof_allows_fast_path(const trx_t *trx)
{
  if (trx == nullptr || trx->read_only || trx->dict_operation)
    return false;

  return ownerless_sql_command_allows_visible_fast_path(trx);
}

extern "C" void mylite_ownerless_innodb_set_commit_visibility_stats_enabled(
    int enabled)
{
  ownerless_commit_visibility_stats_enabled.store(enabled != 0,
                                                  std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_reset_commit_visibility_stats(void)
{
  ownerless_commit_visibility_fast.store(0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush.store(0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_recovery_lsn.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_dirty_pages.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_no_page_write_trx.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_deferred_pages.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_publish_failed.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_no_published_pages.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_unproven_statement.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_log_flush_ns.store(0, std::memory_order_relaxed);
  ownerless_commit_visibility_total_ns.store(0, std::memory_order_relaxed);
  ownerless_commit_visibility_publish_transaction_pages_ns.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_publish_dirty_pages_ns.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_flush_dirty_pages_ns.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_publish_visible_ns.store(
      0, std::memory_order_relaxed);
  ownerless_commit_visibility_release_locks_ns.store(
      0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_read_commit_visibility_stats(
    uint64_t *out_values, size_t value_count)
{
  if (out_values == nullptr || value_count == 0)
    return;

  const std::atomic<uint64_t> *stats[]= {
      &ownerless_commit_visibility_fast,
      &ownerless_commit_visibility_flush,
      &ownerless_commit_visibility_flush_recovery_lsn,
      &ownerless_commit_visibility_flush_dirty_pages,
      &ownerless_commit_visibility_flush_no_page_write_trx,
      &ownerless_commit_visibility_flush_deferred_pages,
      &ownerless_commit_visibility_flush_publish_failed,
      &ownerless_commit_visibility_flush_no_published_pages,
      &ownerless_commit_visibility_flush_unproven_statement,
      &ownerless_commit_visibility_log_flush_ns,
      &ownerless_commit_visibility_total_ns,
      &ownerless_commit_visibility_publish_transaction_pages_ns,
      &ownerless_commit_visibility_publish_dirty_pages_ns,
      &ownerless_commit_visibility_flush_dirty_pages_ns,
      &ownerless_commit_visibility_publish_visible_ns,
      &ownerless_commit_visibility_release_locks_ns,
  };
  const size_t stats_count= sizeof stats / sizeof stats[0];
  const size_t copy_count= std::min(value_count, stats_count);
  for (size_t i= 0; i < copy_count; ++i)
    out_values[i]= stats[i]->load(std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_deep_set_perf_stats_enabled(
    int enabled)
{
  mylite_ownerless_innodb_deep_perf_stats_enabled_flag.store(
      enabled != 0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_deep_reset_perf_stats(void)
{
  for (size_t i= 0; i < MYLITE_OWNERLESS_INNODB_DEEP_PERF_STAT_COUNT; ++i)
    ownerless_innodb_deep_perf_stats[i].store(0, std::memory_order_relaxed);
  mylite_ownerless_innodb_deep_reset_flush_identity_stats();
}

extern "C" void mylite_ownerless_innodb_deep_read_perf_stats(
    uint64_t *out_values, size_t value_count)
{
  if (out_values == nullptr || value_count == 0)
    return;

  const size_t copy_count=
      std::min<size_t>(value_count,
                       MYLITE_OWNERLESS_INNODB_DEEP_PERF_STAT_COUNT);
  for (size_t i= 0; i < copy_count; ++i)
    out_values[i]= ownerless_innodb_deep_perf_stats[i].load(
        std::memory_order_relaxed);
}

extern "C" int mylite_ownerless_innodb_deep_perf_stats_enabled(void)
{
  return mylite_ownerless_innodb_deep_perf_stats_enabled_flag.load(
             std::memory_order_relaxed)
      ? 1
      : 0;
}

extern "C" uint64_t mylite_ownerless_innodb_deep_perf_now_ns(void)
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

extern "C" void mylite_ownerless_innodb_deep_perf_add(
    size_t index, uint64_t value)
{
  if (index < MYLITE_OWNERLESS_INNODB_DEEP_PERF_STAT_COUNT &&
      mylite_ownerless_innodb_deep_perf_stats_enabled_flag.load(
          std::memory_order_relaxed))
    ownerless_innodb_deep_perf_stats[index].fetch_add(
        value, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_deep_perf_add_elapsed(
    size_t index, uint64_t start_ns)
{
  if (start_ns != 0)
    mylite_ownerless_innodb_deep_perf_add(
        index, mylite_ownerless_innodb_deep_perf_now_ns() - start_ns);
}

/*************************************************************//**
Set detailed error message for the transaction. */
void
trx_set_detailed_error(
/*===================*/
	trx_t*		trx,	/*!< in: transaction struct */
	const char*	msg)	/*!< in: detailed error message */
{
	strncpy(trx->detailed_error, msg, MAX_DETAILED_ERROR_LEN - 1);
	trx->detailed_error[MAX_DETAILED_ERROR_LEN - 1] = '\0';
}

/*************************************************************//**
Set detailed error message for the transaction from a file. Note that the
file is rewinded before reading from it. */
void
trx_set_detailed_error_from_file(
/*=============================*/
	trx_t*	trx,	/*!< in: transaction struct */
	FILE*	file)	/*!< in: file to read message from */
{
	os_file_read_string(file, trx->detailed_error, MAX_DETAILED_ERROR_LEN);
}

/********************************************************************//**
Initialize transaction object.
@param trx trx to initialize */
static
void
trx_init(
/*=====*/
	trx_t*	trx)
{
	trx->state = TRX_STATE_NOT_STARTED;

	trx->mylite_ownerless_lock_trx_id = 0;
	trx->mylite_ownerless_page_write_trx_id = 0;
	trx->mylite_ownerless_page_write_publish_failed = false;
	trx->mylite_ownerless_page_write_published_page = false;
	trx->mylite_ownerless_page_write_deferred_pages_published = false;
	trx->mylite_ownerless_history_proof_active = false;
	trx->mylite_ownerless_history_proof_rseg_published = false;
	trx->mylite_ownerless_history_proof_undo_published = false;
	trx->mylite_ownerless_history_proof_space_id = 0;
	trx->mylite_ownerless_history_proof_rseg_page_no = 0;
	trx->mylite_ownerless_history_proof_undo_page_no = 0;
	trx->mylite_ownerless_modified_page_last_hit = 0;
	trx->mylite_ownerless_dirty_page_last_hit = 0;
	trx->mylite_ownerless_native_support_page_write_last_hit = 0;
	trx->mylite_ownerless_modified_page_last_hit_valid = false;
	trx->mylite_ownerless_dirty_page_last_hit_valid = false;
	trx->mylite_ownerless_native_support_page_write_last_hit_valid = false;
	trx->mylite_ownerless_page_write_waited_before_preread = false;
	trx->mylite_ownerless_page_refreshed_after_wait = false;

	trx->is_recovered = false;

	trx->op_info = "";

	trx->active_commit_ordered = false;

	trx->isolation_level = TRX_ISO_REPEATABLE_READ;

	trx->check_foreigns = true;

	trx->check_unique_secondary = true;

	trx->mylite_ownerless_default_checked_bulk_insert_sql_started = false;

	trx->lock.n_rec_locks = 0;

	trx->lock.set_nth_bit_calls = 0;

	trx->dict_operation = false;

	trx->error_state = DB_SUCCESS;

	trx->error_key_num = ULINT_UNDEFINED;

	trx->undo_no = 0;

	trx->rsegs.m_redo.rseg = NULL;

	trx->rsegs.m_noredo.rseg = NULL;

	trx->read_only = false;

	trx->auto_commit = false;

	trx->will_lock = false;

	trx->apply_online_log = false;

	ut_d(trx->start_file = 0);

	ut_d(trx->start_line = 0);

	trx->magic_n = TRX_MAGIC_N;

	trx->last_stmt_start = 0;

	ut_ad(!trx->read_view.is_open());

	trx->lock.rec_cached = 0;

	trx->lock.table_cached = 0;
#ifdef WITH_WSREP
	ut_ad(!trx->wsrep);
#endif /* WITH_WSREP */
}

trx_t::mylite_ownerless_page_vector &
trx_t::mylite_ownerless_modified_pages_for_write() noexcept
{
  if (mylite_ownerless_modified_pages == nullptr)
  {
    mylite_ownerless_modified_pages=
      UT_NEW_NOKEY(mylite_ownerless_page_vector());
    ut_a(mylite_ownerless_modified_pages != nullptr);
  }
  return *mylite_ownerless_modified_pages;
}

trx_t::mylite_ownerless_page_vector &
trx_t::mylite_ownerless_dirty_pages_for_write() noexcept
{
  if (mylite_ownerless_dirty_pages == nullptr)
  {
    mylite_ownerless_dirty_pages=
      UT_NEW_NOKEY(mylite_ownerless_page_vector());
    ut_a(mylite_ownerless_dirty_pages != nullptr);
  }
  return *mylite_ownerless_dirty_pages;
}

trx_t::mylite_ownerless_page_image_vector &
trx_t::mylite_ownerless_page_images_for_write() noexcept
{
  if (mylite_ownerless_page_images == nullptr)
  {
    mylite_ownerless_page_images=
      UT_NEW_NOKEY(mylite_ownerless_page_image_vector());
    ut_a(mylite_ownerless_page_images != nullptr);
  }
  return *mylite_ownerless_page_images;
}

trx_t::mylite_ownerless_page_vector &
trx_t::mylite_ownerless_native_support_page_write_pages_for_write() noexcept
{
  if (mylite_ownerless_native_support_page_write_pages == nullptr)
  {
    mylite_ownerless_native_support_page_write_pages=
      UT_NEW_NOKEY(mylite_ownerless_page_vector());
    ut_a(mylite_ownerless_native_support_page_write_pages != nullptr);
  }
  return *mylite_ownerless_native_support_page_write_pages;
}

bool trx_t::mylite_ownerless_modified_page_contains(
    uint64_t packed_page) const noexcept
{
  if (mylite_ownerless_modified_page_last_hit_valid &&
      mylite_ownerless_modified_page_last_hit == packed_page)
    return true;
  const bool found= mylite_ownerless_page_vector_contains(
      mylite_ownerless_modified_pages,
      mylite_ownerless_modified_page_set,
      packed_page);
  if (found)
  {
    mylite_ownerless_modified_page_last_hit= packed_page;
    mylite_ownerless_modified_page_last_hit_valid= true;
  }
  return found;
}

bool trx_t::mylite_ownerless_dirty_page_contains(
    uint64_t packed_page) const noexcept
{
  if (mylite_ownerless_dirty_page_last_hit_valid &&
      mylite_ownerless_dirty_page_last_hit == packed_page)
    return true;
  const bool found= mylite_ownerless_page_vector_contains(
      mylite_ownerless_dirty_pages,
      mylite_ownerless_dirty_page_set,
      packed_page);
  if (found)
  {
    mylite_ownerless_dirty_page_last_hit= packed_page;
    mylite_ownerless_dirty_page_last_hit_valid= true;
  }
  return found;
}

bool trx_t::mylite_ownerless_native_support_page_write_contains(
    uint64_t packed_page) const noexcept
{
  if (mylite_ownerless_native_support_page_write_last_hit_valid &&
      mylite_ownerless_native_support_page_write_last_hit == packed_page)
    return true;
  const bool found= mylite_ownerless_page_vector_contains(
      mylite_ownerless_native_support_page_write_pages,
      mylite_ownerless_native_support_page_write_page_set,
      packed_page);
  if (found)
  {
    mylite_ownerless_native_support_page_write_last_hit= packed_page;
    mylite_ownerless_native_support_page_write_last_hit_valid= true;
  }
  return found;
}

void trx_t::mylite_ownerless_note_modified_page(
    uint64_t packed_page) noexcept
{
  mylite_ownerless_note_page(
      mylite_ownerless_modified_pages_for_write(),
      mylite_ownerless_modified_page_set,
      packed_page);
  mylite_ownerless_modified_page_last_hit= packed_page;
  mylite_ownerless_modified_page_last_hit_valid= true;
}

void trx_t::mylite_ownerless_note_dirty_page(uint64_t packed_page) noexcept
{
  mylite_ownerless_note_page(
      mylite_ownerless_dirty_pages_for_write(),
      mylite_ownerless_dirty_page_set,
      packed_page);
  mylite_ownerless_dirty_page_last_hit= packed_page;
  mylite_ownerless_dirty_page_last_hit_valid= true;
}

void trx_t::mylite_ownerless_note_native_support_page_write(
    uint64_t packed_page) noexcept
{
  mylite_ownerless_note_page(
      mylite_ownerless_native_support_page_write_pages_for_write(),
      mylite_ownerless_native_support_page_write_page_set,
      packed_page);
  mylite_ownerless_native_support_page_write_last_hit= packed_page;
  mylite_ownerless_native_support_page_write_last_hit_valid= true;
}

void trx_t::mylite_ownerless_rebuild_modified_page_set() noexcept
{
  mylite_ownerless_modified_page_last_hit= 0;
  mylite_ownerless_modified_page_last_hit_valid= false;
  if (mylite_ownerless_modified_page_set == nullptr)
    return;

  mylite_ownerless_page_set_rebuild(
      *mylite_ownerless_modified_page_set,
      mylite_ownerless_modified_pages);
}

/** For managing the life-cycle of the trx_t instance that we get
from the pool. */
struct TrxFactory {

	/** Initializes a transaction object. It must be explicitly started
	with trx_start_if_not_started() before using it. The default isolation
	level is TRX_ISO_REPEATABLE_READ.
	@param trx Transaction instance to initialise */
	static void init(trx_t* trx)
	{
		/* Explicitly call the constructor of the already
		allocated object. trx_t objects are allocated by
		ut_zalloc_nokey() in Pool::Pool() which would not call
		the constructors of the trx_t members. */
		new(&trx->autoinc_locks) trx_t::autoinc_lock_vector();

		new(&trx->mod_tables) trx_mod_tables_t();

		new(&trx->lock.table_locks) lock_list();

		new(&trx->read_view) ReadView();

		trx->rw_trx_hash_pins = 0;
		trx_init(trx);

		trx->dict_operation_lock_mode = false;

		trx->detailed_error = reinterpret_cast<char*>(
			ut_zalloc_nokey(MAX_DETAILED_ERROR_LEN));

		trx->lock.lock_heap = mem_heap_create_typed(
			1024, MEM_HEAP_FOR_LOCK_HEAP);
		pthread_cond_init(&trx->lock.cond, nullptr);

		UT_LIST_INIT(trx->lock.trx_locks, &lock_t::trx_locks);
		UT_LIST_INIT(trx->lock.evicted_tables,
			     &dict_table_t::table_LRU);

		trx->mutex_init();
	}

	/** Release resources held by the transaction object.
	@param trx the transaction for which to release resources */
	static void destroy(trx_t* trx)
	{
#ifdef __SANITIZE_ADDRESS__
		/* Unpoison the memory for AddressSanitizer */
		MEM_MAKE_ADDRESSABLE(trx, sizeof *trx);
#elif !__has_feature(memory_sanitizer)
		/* In Valgrind, we cannot cancel MEM_NOACCESS() without
		changing the state of the V bits (which indicate
		which bits are initialized).
		We will declare the contents as initialized.
		We did invoke MEM_CHECK_DEFINED() in trx_t::free(). */
		MEM_MAKE_DEFINED(trx, sizeof *trx);
#endif

		ut_a(trx->magic_n == TRX_MAGIC_N);
		ut_ad(!trx->mysql_thd);

		ut_a(trx->lock.wait_lock == NULL);
		ut_a(trx->lock.wait_thr == NULL);
		ut_a(!trx->dict_operation_lock_mode);

		if (trx->lock.lock_heap != NULL) {
			mem_heap_free(trx->lock.lock_heap);
			trx->lock.lock_heap = NULL;
		}

		pthread_cond_destroy(&trx->lock.cond);

		ut_a(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);
		ut_ad(UT_LIST_GET_LEN(trx->lock.evicted_tables) == 0);

		ut_free(trx->detailed_error);

		trx->mutex_destroy();

		trx->autoinc_locks.~small_vector();

			if (trx->mylite_ownerless_modified_pages != nullptr) {
				UT_DELETE(trx->mylite_ownerless_modified_pages);
				trx->mylite_ownerless_modified_pages = nullptr;
			}
			if (trx->mylite_ownerless_dirty_pages != nullptr) {
				UT_DELETE(trx->mylite_ownerless_dirty_pages);
				trx->mylite_ownerless_dirty_pages = nullptr;
			}
			if (trx->mylite_ownerless_modified_page_set != nullptr) {
				UT_DELETE(trx->mylite_ownerless_modified_page_set);
				trx->mylite_ownerless_modified_page_set = nullptr;
			}
			if (trx->mylite_ownerless_dirty_page_set != nullptr) {
				UT_DELETE(trx->mylite_ownerless_dirty_page_set);
				trx->mylite_ownerless_dirty_page_set = nullptr;
			}
			if (trx->mylite_ownerless_page_images != nullptr) {
				UT_DELETE(trx->mylite_ownerless_page_images);
				trx->mylite_ownerless_page_images = nullptr;
			}
			if (trx->mylite_ownerless_native_support_page_write_pages != nullptr) {
				UT_DELETE(trx->mylite_ownerless_native_support_page_write_pages);
				trx->mylite_ownerless_native_support_page_write_pages = nullptr;
			}
			if (trx->mylite_ownerless_native_support_page_write_page_set != nullptr) {
				UT_DELETE(trx->mylite_ownerless_native_support_page_write_page_set);
				trx->mylite_ownerless_native_support_page_write_page_set = nullptr;
			}

		trx->mod_tables.~trx_mod_tables_t();

		ut_ad(!trx->read_view.is_open());

		trx->lock.table_locks.~lock_list();

		trx->read_view.~ReadView();
	}
};

/** The lock strategy for TrxPool */
class TrxPoolLock
{
  mysql_mutex_t mutex;

public:
  /** Create the mutex */
  void create()
  {
    mysql_mutex_init(trx_pool_mutex_key, &mutex, nullptr);
  }

  /** Acquire the mutex */
  void enter() { mysql_mutex_lock(&mutex); }

  /** Release the mutex */
  void exit() { mysql_mutex_unlock(&mutex); }

  /** Free the mutex */
  void destroy() { mysql_mutex_destroy(&mutex); }
};

/** The lock strategy for the TrxPoolManager */
class TrxPoolManagerLock
{
  mysql_mutex_t mutex;

public:
  /** Create the mutex */
  void create()
  {
    mysql_mutex_init(trx_pool_manager_mutex_key, &mutex, nullptr);
  }

  /** Acquire the mutex */
  void enter() { mysql_mutex_lock(&mutex); }

  /** Release the mutex */
  void exit() { mysql_mutex_unlock(&mutex); }

  /** Free the mutex */
  void destroy() { mysql_mutex_destroy(&mutex); }
};

/** Use explicit mutexes for the trx_t pool and its manager. */
typedef Pool<trx_t, TrxFactory, TrxPoolLock> trx_pool_t;
typedef PoolManager<trx_pool_t, TrxPoolManagerLock > trx_pools_t;

/** The trx_t pool manager */
static trx_pools_t* trx_pools;

/** Size of on trx_t pool in bytes. */
static const ulint MAX_TRX_BLOCK_SIZE = 1024 * 1024 * 4;

/** Create the trx_t pool */
void
trx_pool_init()
{
	trx_pools = UT_NEW_NOKEY(trx_pools_t(MAX_TRX_BLOCK_SIZE));

	ut_a(trx_pools != 0);
}

/** Destroy the trx_t pool */
void
trx_pool_close()
{
	UT_DELETE(trx_pools);

	trx_pools = 0;
}

/** @return an allocated transaction */
trx_t *trx_create()
{
	trx_t*	trx = trx_pools->get();

#ifdef __SANITIZE_ADDRESS__
	/* Unpoison the memory for AddressSanitizer.
	It may have been poisoned in trx_t::free().*/
	MEM_MAKE_ADDRESSABLE(trx, sizeof *trx);
#elif !__has_feature(memory_sanitizer)
	/* In Valgrind, we cannot cancel MEM_NOACCESS() without
	changing the state of the V bits (which indicate
	which bits are initialized).
	We will declare the contents as initialized.
	We did invoke MEM_CHECK_DEFINED() in trx_t::free(). */
	MEM_MAKE_DEFINED(trx, sizeof *trx);
#endif

	trx->assert_freed();

	/* We just got trx from pool, it should be non locking */
	ut_ad(!trx->will_lock);
	ut_ad(!trx->rw_trx_hash_pins);

	DBUG_LOG("trx", "Create: " << trx);

	ut_ad(trx->mod_tables.empty());
	ut_ad(trx->lock.n_rec_locks == 0);
	ut_ad(trx->lock.set_nth_bit_calls == 0);
	ut_ad(trx->lock.table_cached == 0);
	ut_ad(trx->lock.rec_cached == 0);
	ut_ad(UT_LIST_GET_LEN(trx->lock.evicted_tables) == 0);

	trx_sys.register_trx(trx);

	return(trx);
}

/** Free the memory to trx_pools */
void trx_t::free() noexcept
{
  autoinc_locks.fake_defined();
#ifdef HAVE_MEM_CHECK
  if (xid.is_null())
    MEM_MAKE_DEFINED(&xid, sizeof xid);
  else
    MEM_MAKE_DEFINED(&xid.data[xid.gtrid_length + xid.bqual_length],
                     sizeof xid.data - (xid.gtrid_length + xid.bqual_length));
#endif
  MEM_CHECK_DEFINED(this, sizeof *this);
  autoinc_locks.make_undefined();
  ut_ad(!active_handler_stats);

  if (size_t n_page_gets= pages_accessed)
  {
    pages_accessed= 0;
    buf_pool.stat.n_page_gets+= n_page_gets;
  }

  ut_ad(!n_mysql_tables_in_use);
  ut_ad(!mysql_log_file_name);
  ut_ad(!mysql_n_tables_locked);
  ut_ad(!will_lock);
  ut_ad(error_state == DB_SUCCESS);
  ut_ad(magic_n == TRX_MAGIC_N);
  ut_ad(!read_only);
  ut_ad(!lock.wait_lock);
  ut_ad(!commit_lsn);

  dict_operation= false;
  commit_lsn= 0;
  trx_sys.deregister_trx(this);
  check_unique_secondary= true;
  check_foreigns= true;
  assert_freed();
  trx_sys.rw_trx_hash.put_pins(this);
  mysql_thd= nullptr;

  autoinc_locks.deep_clear();
  mylite_ownerless_page_write_trx_id= 0;
  mylite_ownerless_page_write_publish_failed= false;
  mylite_ownerless_page_write_published_page= false;
  mylite_ownerless_page_write_deferred_pages_published= false;
  mylite_ownerless_history_proof_active= false;
  mylite_ownerless_history_proof_rseg_published= false;
  mylite_ownerless_history_proof_undo_published= false;
  mylite_ownerless_history_proof_space_id= 0;
  mylite_ownerless_history_proof_rseg_page_no= 0;
  mylite_ownerless_history_proof_undo_page_no= 0;
  mylite_ownerless_modified_page_last_hit= 0;
  mylite_ownerless_dirty_page_last_hit= 0;
  mylite_ownerless_native_support_page_write_last_hit= 0;
  mylite_ownerless_modified_page_last_hit_valid= false;
  mylite_ownerless_dirty_page_last_hit_valid= false;
  mylite_ownerless_native_support_page_write_last_hit_valid= false;
  mylite_ownerless_page_write_waited_before_preread= false;
  mylite_ownerless_page_refreshed_after_wait= false;
  mylite_ownerless_modified_pages_clear();

  MEM_NOACCESS(&skip_lock_inheritance_and_n_ref,
               sizeof skip_lock_inheritance_and_n_ref);
  /* do not poison mutex */
  MEM_NOACCESS(&id, sizeof id);
  MEM_NOACCESS(&max_inactive_id, sizeof id);
  MEM_NOACCESS(&state, sizeof state);
  MEM_NOACCESS(&is_recovered, sizeof is_recovered);
#ifdef WITH_WSREP
  MEM_NOACCESS(&wsrep, sizeof wsrep);
#endif
  read_view.mem_noaccess();
  MEM_NOACCESS(&lock, sizeof lock);
  MEM_NOACCESS(&op_info, sizeof op_info +
               sizeof(unsigned) /* isolation_level, snapshot_isolation,
                                   check_foreigns, check_unique_secondary,
                                   bulk_insert */);
  MEM_NOACCESS(&is_registered, sizeof is_registered);
  MEM_NOACCESS(&active_commit_ordered, sizeof active_commit_ordered);
  MEM_NOACCESS(&flush_log_later, sizeof flush_log_later);
  MEM_NOACCESS(&duplicates, sizeof duplicates);
  MEM_NOACCESS(&dict_operation, sizeof dict_operation);
  MEM_NOACCESS(&dict_operation_lock_mode, sizeof dict_operation_lock_mode);
  MEM_NOACCESS(&start_time, sizeof start_time);
  MEM_NOACCESS(&start_time_micro, sizeof start_time_micro);
  MEM_NOACCESS(&commit_lsn, sizeof commit_lsn);
  MEM_NOACCESS(&mysql_thd, sizeof mysql_thd);
  MEM_NOACCESS(&mysql_log_file_name, sizeof mysql_log_file_name);
  MEM_NOACCESS(&mysql_log_offset, sizeof mysql_log_offset);
  MEM_NOACCESS(&n_mysql_tables_in_use, sizeof n_mysql_tables_in_use);
  MEM_NOACCESS(&mysql_n_tables_locked, sizeof mysql_n_tables_locked);
  MEM_NOACCESS(&error_state, sizeof error_state);
  MEM_NOACCESS(&error_info, sizeof error_info);
  MEM_NOACCESS(&error_key_num, sizeof error_key_num);
  MEM_NOACCESS(&graph, sizeof graph);
  MEM_NOACCESS(&undo_no, sizeof undo_no);
  MEM_NOACCESS(&last_stmt_start, sizeof last_stmt_start);
  MEM_NOACCESS(&rsegs, sizeof rsegs);
  MEM_NOACCESS(&roll_limit, sizeof roll_limit);
  MEM_NOACCESS(&in_rollback, sizeof in_rollback);
  MEM_NOACCESS(&pages_undone, sizeof pages_undone);
  MEM_NOACCESS(&n_autoinc_rows, sizeof n_autoinc_rows);
  MEM_NOACCESS(&autoinc_locks, sizeof autoinc_locks);
	  MEM_NOACCESS(&mylite_ownerless_modified_pages,
	               sizeof mylite_ownerless_modified_pages);
	  MEM_NOACCESS(&mylite_ownerless_dirty_pages,
	               sizeof mylite_ownerless_dirty_pages);
	  MEM_NOACCESS(&mylite_ownerless_modified_page_set,
	               sizeof mylite_ownerless_modified_page_set);
	  MEM_NOACCESS(&mylite_ownerless_dirty_page_set,
	               sizeof mylite_ownerless_dirty_page_set);
	  MEM_NOACCESS(&mylite_ownerless_page_images,
	               sizeof mylite_ownerless_page_images);
	  MEM_NOACCESS(&mylite_ownerless_native_support_page_write_pages,
	               sizeof mylite_ownerless_native_support_page_write_pages);
	  MEM_NOACCESS(&mylite_ownerless_native_support_page_write_page_set,
	               sizeof mylite_ownerless_native_support_page_write_page_set);
  MEM_NOACCESS(&mylite_ownerless_page_write_publish_failed,
               sizeof mylite_ownerless_page_write_publish_failed);
  MEM_NOACCESS(&mylite_ownerless_page_write_published_page,
               sizeof mylite_ownerless_page_write_published_page);
  MEM_NOACCESS(&mylite_ownerless_page_write_deferred_pages_published,
               sizeof mylite_ownerless_page_write_deferred_pages_published);
  MEM_NOACCESS(&read_only, sizeof read_only);
  MEM_NOACCESS(&auto_commit, sizeof auto_commit);
  MEM_NOACCESS(&will_lock, sizeof will_lock);
  MEM_NOACCESS(&fts_trx, sizeof fts_trx);
  MEM_NOACCESS(&fts_next_doc_id, sizeof fts_next_doc_id);
  MEM_NOACCESS(&flush_tables, sizeof flush_tables);
#ifdef UNIV_DEBUG
  MEM_NOACCESS(&start_line, sizeof start_line);
  MEM_NOACCESS(&start_file, sizeof start_file);
#endif /* UNIV_DEBUG */
  MEM_NOACCESS(&xid, sizeof xid);
  MEM_NOACCESS(&mod_tables, sizeof mod_tables);
  MEM_NOACCESS(&detailed_error, sizeof detailed_error);
  MEM_NOACCESS(&magic_n, sizeof magic_n);
  MEM_NOACCESS(&apply_online_log, sizeof apply_online_log);
  trx_pools->mem_free(this);
}

/** Transition to committed state, to release implicit locks. */
TRANSACTIONAL_INLINE inline void trx_t::commit_state()
{
  ut_d(auto trx_state= state);
  ut_ad(trx_state == TRX_STATE_PREPARED ||
        trx_state == TRX_STATE_PREPARED_RECOVERED ||
        trx_state == TRX_STATE_ACTIVE);
  /* This makes the transaction committed in memory and makes its
  changes to data visible to other transactions. NOTE that there is a
  small discrepancy from the strict formal visibility rules here: a
  user of the database can see modifications made by another
  transaction T even before the necessary redo log segment has been
  flushed to the disk. If the database happens to crash before the
  flush, the user has seen modifications from T which will never be a
  committed transaction. However, any transaction T2 which sees the
  modifications of the committing transaction T, and which also itself
  makes modifications to the database, will get an lsn larger than the
  committing transaction T. In the case where the log flush fails, and
  T never gets committed, also T2 will never get committed. */
  TMTrxGuard tg{*this};
  state= TRX_STATE_COMMITTED_IN_MEMORY;
  ut_ad(id || !is_referenced());
}

/** Release any explicit locks of a committing transaction. */
inline void trx_t::release_locks()
{
  DEBUG_SYNC_C("trx_t_release_locks_enter");
  DBUG_ASSERT(state == TRX_STATE_COMMITTED_IN_MEMORY);
  DBUG_ASSERT(!is_referenced());

  if (UT_LIST_GET_LEN(lock.trx_locks))
  {
    lock_release(this);
    ut_ad(!lock.n_rec_locks);
    ut_ad(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(autoinc_locks.empty());
    mem_heap_empty(lock.lock_heap);
  }

  mylite_ownerless_innodb_lock_forget_transaction(this);
  lock.table_locks.clear();
  reset_skip_lock_inheritance();
  id= 0;
  while (dict_table_t *table= UT_LIST_GET_FIRST(lock.evicted_tables))
  {
    UT_LIST_REMOVE(lock.evicted_tables, table);
    dict_mem_table_free(table);
  }
  DEBUG_SYNC_C("after_trx_committed_in_memory");
}

/** At shutdown, frees a transaction object. */
TRANSACTIONAL_TARGET void trx_free_at_shutdown(trx_t *trx)
{
	ut_ad(trx->is_recovered);
	ut_a(trx_state_eq(trx, TRX_STATE_PREPARED)
	     || trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED)
	     || (trx_state_eq(trx, TRX_STATE_ACTIVE)
		 && (!srv_was_started
		     || srv_operation == SRV_OPERATION_RESTORE
		     || srv_operation == SRV_OPERATION_RESTORE_EXPORT
		     || srv_read_only_mode
		     || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO
		     || (!srv_is_being_started
		         && !srv_undo_sources && srv_fast_shutdown))));
	ut_a(trx->magic_n == TRX_MAGIC_N);

	ut_d(trx->apply_online_log = false);
	trx->bulk_insert = 0;
	trx->mylite_ownerless_default_checked_bulk_insert_sql_started = false;
	trx->commit_state();
	trx->release_locks();
	trx->mod_tables.clear();
	trx_undo_free_at_shutdown(trx);

	ut_a(!trx->read_only);

	DBUG_LOG("trx", "Free prepared: " << trx);
	trx->state = TRX_STATE_NOT_STARTED;
	ut_ad(!UT_LIST_GET_LEN(trx->lock.trx_locks));
	ut_d(*trx->detailed_error = '\0');
	trx->free();
}


/**
  Disconnect a prepared transaction from MySQL
  @param[in,out] trx transaction
*/
void trx_disconnect_prepared(trx_t *trx)
{
  ut_ad(trx_state_eq(trx, TRX_STATE_PREPARED));
  ut_ad(trx->mysql_thd);
  ut_ad(!trx->mysql_log_file_name);
  trx->read_view.close();
  trx_sys.trx_list.freeze();
  trx->is_recovered= true;
  trx->mysql_thd= NULL;
  trx_sys.trx_list.unfreeze();
  /* todo/fixme: suggest to do it at innodb prepare */
  trx->will_lock= false;
  trx_sys.rw_trx_hash.put_pins(trx);
}

MY_ATTRIBUTE((nonnull, warn_unused_result))
/** Resurrect the table locks for a resurrected transaction. */
static dberr_t trx_resurrect_table_locks(trx_t *trx, const trx_undo_t &undo)
{
  ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
        trx_state_eq(trx, TRX_STATE_PREPARED));
  ut_ad(undo.rseg == trx->rsegs.m_redo.rseg);

  if (undo.empty())
    return DB_SUCCESS;

  mtr_t mtr{trx};
  std::map<table_id_t, bool> tables;
  mtr.start();

  dberr_t err;
  if (buf_block_t *block=
      buf_page_get_gen(page_id_t(trx->rsegs.m_redo.rseg->space->id,
                                 undo.top_page_no), 0, RW_S_LATCH, nullptr,
                       BUF_GET, &mtr, &err))
  {
    buf_page_make_young_if_needed(&block->page);
    buf_block_t *undo_block= block;
    uint16_t undo_rec_offset= undo.top_offset;
    for (const trx_undo_rec_t *undo_rec= block->page.frame + undo_rec_offset;;)
    {
      byte type;
      byte cmpl_info;
      undo_no_t undo_no;
      table_id_t table_id;
      bool updated_extern;

      if (undo_block != block)
      {
        mtr.release(*undo_block);
        undo_block= block;
      }
      trx_undo_rec_get_pars(undo_rec, &type, &cmpl_info,
                            &updated_extern, &undo_no, &table_id);
      tables.emplace(table_id, type == TRX_UNDO_EMPTY);
      ut_ad(page_offset(undo_rec) == undo_rec_offset);
      undo_rec= trx_undo_get_prev_rec(block, undo_rec_offset,
                                      undo.hdr_page_no, undo.hdr_offset,
                                      true, &mtr);
      if (!undo_rec)
        break;
      undo_rec_offset= uint16_t(undo_rec - block->page.frame);
    }
  }

  mtr.commit();

  if (err != DB_SUCCESS)
    return err;

  for (auto p : tables)
  {
    if (dict_table_t *table=
        dict_table_open_on_id(p.first, FALSE, DICT_TABLE_OP_LOAD_TABLESPACE))
    {
      if (!table->is_readable())
      {
        dict_sys.lock(SRW_LOCK_CALL);
        table->release();
        dict_sys.remove(table);
        dict_sys.unlock();
        continue;
      }

      if (trx->state == TRX_STATE_PREPARED)
        trx->mod_tables.emplace(table, 0);

      lock_table_resurrect(table, trx, p.second ? LOCK_X : LOCK_IX);

      DBUG_LOG("ib_trx",
               "resurrect " << ib::hex(trx->id) << " lock on " << table->name);
      table->release();
    }
  }

  return DB_SUCCESS;
}


MY_ATTRIBUTE((nonnull, warn_unused_result))
/**
  Resurrect the transactions that were doing inserts/updates the time of the
  crash, they need to be undone.
*/
static dberr_t trx_resurrect(trx_undo_t *undo, trx_rseg_t *rseg,
                             time_t start_time, ulonglong start_time_micro,
                             uint64_t *rows_to_undo)
{
  uint64_t mylite_table_locks_start;
  trx_state_t state;
  ut_ad(rseg->needs_purge >= undo->trx_id);
  /*
    This is single-threaded startup code, we do not need the
    protection of trx->mutex here.
  */
  switch (undo->state)
  {
  case TRX_UNDO_ACTIVE:
    state= TRX_STATE_ACTIVE;
    break;
  case TRX_UNDO_PREPARED:
    /*
      Prepared transactions are left in the prepared state
      waiting for a commit or abort decision from MySQL
    */
    state= TRX_STATE_PREPARED;
    sql_print_information("InnoDB: Transaction " TRX_ID_FMT
                          " was in the XA prepared state.", undo->trx_id);
    break;
  default:
    return DB_SUCCESS;
  }

  rseg->acquire();
  trx_t *trx= trx_create();
  trx->state= state;
  ut_d(trx->start_file= __FILE__);
  ut_d(trx->start_line= __LINE__);

  trx->rsegs.m_redo.undo= undo;
  trx->undo_no= undo->top_undo_no + 1;
  trx->rsegs.m_redo.rseg= rseg;
  trx->xid= undo->xid;
  trx->id= undo->trx_id;
  trx->is_recovered= true;
  trx->start_time= start_time;
  trx->start_time_micro= start_time_micro;
  trx->dict_operation= undo->dict_operation;

  trx_sys.rw_trx_hash.insert(trx);
  trx_sys.rw_trx_hash.put_pins(trx);
  if (trx_state_eq(trx, TRX_STATE_ACTIVE))
    *rows_to_undo+= trx->undo_no;
  mylite_embedded_startup_perf_count(
    MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RESURRECT_TRX_COUNT);
  mylite_table_locks_start= mylite_embedded_startup_perf_start_ns();
  dberr_t err= trx_resurrect_table_locks(trx, *undo);
  mylite_embedded_startup_perf_add_elapsed(
    MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_TABLE_LOCKS_NS,
    mylite_table_locks_start);
  return err;
}


/** Initialize (resurrect) transactions at startup. */
dberr_t trx_lists_init_at_db_start()
{
	uint64_t mylite_stage_start;
	ut_a(srv_is_being_started);
	ut_ad(!srv_was_started);

	if (srv_operation == SRV_OPERATION_RESTORE) {
		/* mariabackup --prepare only deals with
		the redo log and the data files, not with
		transactions or the data dictionary. */
		mylite_stage_start= mylite_embedded_startup_perf_start_ns();
		dberr_t err= trx_rseg_array_init();
		mylite_embedded_startup_perf_add_elapsed(
			MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RSEG_ARRAY_INIT_NS,
			mylite_stage_start);
		return err;
	}

	if (srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN) {
		return DB_SUCCESS;
	}

	mylite_stage_start= mylite_embedded_startup_perf_start_ns();
	purge_sys.create();
	mylite_embedded_startup_perf_add_elapsed(
		MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_PURGE_CREATE_NS,
		mylite_stage_start);
	mylite_stage_start= mylite_embedded_startup_perf_start_ns();
	dberr_t err = trx_rseg_array_init();
	mylite_embedded_startup_perf_add_elapsed(
		MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RSEG_ARRAY_INIT_NS,
		mylite_stage_start);

	if (err != DB_SUCCESS) {
corrupted:
		ib::info() << "Retry with innodb_force_recovery=5";
		return err;
	}

	mylite_stage_start= mylite_embedded_startup_perf_start_ns();
	const bool mylite_undo_empty = trx_sys.is_undo_empty();
	mylite_embedded_startup_perf_add_elapsed(
		MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_EMPTY_CHECK_NS,
		mylite_stage_start);

	if (mylite_undo_empty) {
		mylite_embedded_startup_perf_count(
			MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_EMPTY_EXIT_COUNT);
		mylite_stage_start= mylite_embedded_startup_perf_start_ns();
		purge_sys.clone_oldest_view<true>();
		mylite_embedded_startup_perf_add_elapsed(
			MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_PURGE_CLONE_VIEW_NS,
			mylite_stage_start);
		return DB_SUCCESS;
	}

	/* Look from the rollback segments if there exist undo logs for
	transactions. */
	const time_t	start_time	= time(NULL);
	const ulonglong	start_time_micro= microsecond_interval_timer();
	uint64_t	rows_to_undo	= 0;
	mylite_stage_start= mylite_embedded_startup_perf_start_ns();

	for (auto& rseg : trx_sys.rseg_array) {
		trx_undo_t*	undo;

		/* Some rollback segment may be unavailable,
		especially if the server was previously run with a
		non-default value of innodb_undo_logs. */
		if (!rseg.space) {
			continue;
		}
		/* Resurrect other transactions. */
		for (undo = UT_LIST_GET_FIRST(rseg.undo_list);
		     undo != NULL;
		     undo = UT_LIST_GET_NEXT(undo_list, undo)) {
			trx_t *trx = trx_sys.find(0, undo->trx_id, false);
			if (!trx) {
				err = trx_resurrect(undo, &rseg, start_time,
						    start_time_micro,
						    &rows_to_undo);
			} else {
				ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE) ||
				      trx_state_eq(trx, TRX_STATE_PREPARED));
				ut_ad(trx->start_time == start_time);
				ut_ad(trx->is_recovered);
				ut_ad(trx->rsegs.m_redo.rseg == &rseg);
				ut_ad(rseg.is_referenced());
				ut_ad(rseg.needs_purge);

				trx->rsegs.m_redo.undo = undo;
				if (undo->top_undo_no >= trx->undo_no) {
					if (trx_state_eq(trx,
							 TRX_STATE_ACTIVE)) {
						rows_to_undo -= trx->undo_no;
						rows_to_undo +=
							undo->top_undo_no + 1;
					}

					trx->undo_no = undo->top_undo_no + 1;
				}
				const uint64_t mylite_table_locks_start =
					mylite_embedded_startup_perf_start_ns();
				err = trx_resurrect_table_locks(trx, *undo);
				mylite_embedded_startup_perf_add_elapsed(
					MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_TABLE_LOCKS_NS,
					mylite_table_locks_start);
			}

			if (err != DB_SUCCESS) {
				mylite_embedded_startup_perf_add_elapsed(
					MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RESURRECT_SCAN_NS,
					mylite_stage_start);
				goto corrupted;
			}
		}
	}
	mylite_embedded_startup_perf_add_elapsed(
		MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RESURRECT_SCAN_NS,
		mylite_stage_start);

	if (const auto size = trx_sys.rw_trx_hash.size()) {
		ib::info() << size
			<< " transaction(s) which must be rolled back or"
			" cleaned up in total " << rows_to_undo
			<< " row operations to undo";
		ib::info() << "Trx id counter is " << trx_sys.get_max_trx_id();
	}

	mylite_stage_start= mylite_embedded_startup_perf_start_ns();
	purge_sys.clone_oldest_view<true>();
	mylite_embedded_startup_perf_add_elapsed(
		MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_PURGE_CLONE_VIEW_NS,
		mylite_stage_start);
	return DB_SUCCESS;
}

/** Assign a persistent rollback segment in a round-robin fashion,
evenly distributed between 0 and innodb_undo_logs-1
@param trx transaction */
static void trx_assign_rseg_low(trx_t *trx)
{
	ut_ad(!trx->rsegs.m_redo.rseg);
	ut_ad(srv_available_undo_logs == TRX_SYS_N_RSEGS);

	/* The first slot is always assigned to the system tablespace. */
	ut_ad(trx_sys.rseg_array[0].space == fil_system.sys_space);

	trx_sys.register_rw(trx);
	ut_ad(trx->id);

	/* Choose a rollback segment evenly distributed between 0 and
	innodb_undo_logs-1 in a round-robin fashion, skipping those
	undo tablespaces that are scheduled for truncation. */
	static Atomic_counter<unsigned>	rseg_slot;
	unsigned slot = rseg_slot++ % TRX_SYS_N_RSEGS;
	DBUG_EXECUTE_IF("assign_same_rseg", slot= 0;);
	ut_d(const auto start_scan_slot = slot);
	ut_d(bool look_for_rollover = false);
	trx_rseg_t*	rseg;

	bool	allocated;

	do {
		for (;;) {
			rseg = &trx_sys.rseg_array[slot];
			ut_ad(!look_for_rollover || start_scan_slot != slot);
			ut_d(look_for_rollover = true);
			slot = (slot + 1) % TRX_SYS_N_RSEGS;
			DBUG_EXECUTE_IF("assign_same_rseg",
				slot= (slot - 1) % TRX_SYS_N_RSEGS;);

			if (!rseg->space) {
				continue;
			}

			ut_ad(rseg->is_persistent());

			if (rseg->space != fil_system.sys_space) {
				if (rseg->skip_allocation()) {
					continue;
				}
			} else if (const fil_space_t *space =
				   trx_sys.rseg_array[slot].space) {
				if (space != fil_system.sys_space
				    && srv_undo_tablespaces > 0) {
					/** If dedicated
					innodb_undo_tablespaces have
					been configured, try to use them
					instead of the system tablespace. */
					continue;
				}
			}

			break;
		}

		/* By now we have only selected the rseg but not marked it
		allocated. By marking it allocated we are ensuring that it will
		never be selected for UNDO truncate purge. */
		allocated = rseg->acquire_if_available();
	} while (!allocated);

	trx->rsegs.m_redo.rseg = rseg;
}

/** Assign a rollback segment for modifying temporary tables.
@return the assigned rollback segment */
trx_rseg_t *trx_t::assign_temp_rseg()
{
	ut_ad(!rsegs.m_noredo.rseg);
	ut_ad(!is_autocommit_non_locking());
	compile_time_assert(ut_is_2pow(TRX_SYS_N_RSEGS));

	/* Choose a temporary rollback segment between 0 and 127
	in a round-robin fashion. */
	static Atomic_counter<unsigned> rseg_slot;
	trx_rseg_t*	rseg = &trx_sys.temp_rsegs[
		rseg_slot++ & (TRX_SYS_N_RSEGS - 1)];
	ut_ad(!rseg->is_persistent());
	rsegs.m_noredo.rseg = rseg;

	if (id == 0) {
		trx_sys.register_rw(this);
	}

	return(rseg);
}

/****************************************************************//**
Starts a transaction. */
static
void
trx_start_low(
/*==========*/
	trx_t*	trx,		/*!< in: transaction */
	bool	read_write)	/*!< in: true if read-write transaction */
{
	ut_ad(!trx->in_rollback);
	ut_ad(!trx->is_recovered);
	ut_ad(trx->start_line != 0);
	ut_ad(trx->start_file != 0);
	ut_ad(trx->roll_limit == 0);
	ut_ad(trx->error_state == DB_SUCCESS);
	ut_ad(trx->rsegs.m_redo.rseg == NULL);
	ut_ad(trx->rsegs.m_noredo.rseg == NULL);
	ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED));
	ut_ad(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);

	trx->mylite_ownerless_page_write_publish_failed = false;
	trx->mylite_ownerless_page_write_published_page = false;
	trx->mylite_ownerless_page_write_deferred_pages_published = false;
	trx->mylite_ownerless_history_proof_active = false;
	trx->mylite_ownerless_history_proof_rseg_published = false;
	trx->mylite_ownerless_history_proof_undo_published = false;
	trx->mylite_ownerless_history_proof_space_id = 0;
	trx->mylite_ownerless_history_proof_rseg_page_no = 0;
	trx->mylite_ownerless_history_proof_undo_page_no = 0;
	trx->mylite_ownerless_modified_page_last_hit = 0;
	trx->mylite_ownerless_dirty_page_last_hit = 0;
	trx->mylite_ownerless_native_support_page_write_last_hit = 0;
	trx->mylite_ownerless_modified_page_last_hit_valid = false;
	trx->mylite_ownerless_dirty_page_last_hit_valid = false;
	trx->mylite_ownerless_native_support_page_write_last_hit_valid = false;

	/* Check whether it is an AUTOCOMMIT SELECT */
        if (const THD* thd = trx->mysql_thd) {
		trx->auto_commit = !(thd->variables.option_bits
                                     & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
			&& thd->lex->sql_command == SQLCOM_SELECT;
		trx->read_only = (!trx->dict_operation && thd->tx_read_only)
			|| srv_read_only_mode;
		if (!trx->auto_commit) {
			trx->will_lock = true;
		} else if (!trx->will_lock) {
			trx->read_only = true;
		}
	} else {
		trx->auto_commit = false;
		trx->read_only = false;
		trx->will_lock = true;
	}

#ifdef WITH_WSREP
	trx->xid.null();
	trx->wsrep = wsrep_on(trx->mysql_thd);
#endif /* WITH_WSREP */

	ut_a(trx->autoinc_locks.empty());
	ut_a(trx->lock.table_locks.empty());

	/* No other thread can access this trx object through rw_trx_hash,
	still it can be found through trx_sys.trx_list. Sometimes it's
	possible to indirectly protect trx_t::state by freezing
	trx_sys.trx_list.

	For now we update it without mutex protection, because original code
	did it this way. It has to be reviewed and fixed properly. */
	trx->state = TRX_STATE_ACTIVE;

	/* By default all transactions are in the read-only list unless they
	are non-locking auto-commit read only transactions or background
	(internal) transactions. Note: Transactions marked explicitly as
	read only can write to temporary tables, we put those on the RO
	list too. */

	if (!trx->read_only
	    && (!trx->mysql_thd || read_write || trx->dict_operation)) {
		/* Temporary rseg is assigned only if the transaction
		updates a temporary table */
		if (!high_level_read_only) {
			trx_assign_rseg_low(trx);
		}
	} else {
		if (!trx->is_autocommit_non_locking()) {

			/* If this is a read-only transaction that is writing
			to a temporary table then it needs a transaction id
			to write to the temporary table. */

			if (read_write) {
				ut_ad(!srv_read_only_mode);
				trx_sys.register_rw(trx);
			}
		} else {
			ut_ad(!read_write);
		}
	}

	trx->start_time = time(NULL);
	trx->start_time_micro = trx->mysql_thd
		? thd_start_utime(trx->mysql_thd)
		: microsecond_interval_timer();

	ut_a(trx->error_state == DB_SUCCESS);
}

/** Release an empty undo log that was associated with a transaction. */
ATTRIBUTE_COLD
void trx_t::commit_empty(mtr_t *mtr)
{
  trx_rseg_t *rseg= rsegs.m_redo.rseg;
  trx_undo_t *&undo= rsegs.m_redo.undo;

  ut_ad(undo->state == TRX_UNDO_ACTIVE || undo->state == TRX_UNDO_PREPARED);

  if (UNIV_UNLIKELY(undo->size != 1))
  {
    sql_print_error("InnoDB: Undo log for transaction " TRX_ID_FMT
                    " is corrupted (" UINT32PF "!=1)", id, undo->size);
    ut_ad("corrupted undo log" == 0);
  }

  if (buf_block_t *u=
      buf_page_get(page_id_t(rseg->space->id, undo->hdr_page_no), 0,
                   RW_X_LATCH, mtr))
  {
    ut_d(const uint16_t state=
         mach_read_from_2(TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + u->page.frame));
    ut_ad(state == undo->state || state == TRX_UNDO_ACTIVE);
    static_assert(TRX_UNDO_PAGE_START + 2 == TRX_UNDO_PAGE_FREE,
                  "compatibility");
    ut_ad(!memcmp(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + u->page.frame,
                  TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE + u->page.frame, 2));
    ut_ad(mach_read_from_4(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_PREV +
                           FIL_ADDR_PAGE + u->page.frame) == FIL_NULL);
    ut_ad(mach_read_from_2(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_PREV +
                           FIL_ADDR_BYTE + u->page.frame) == 0);
    ut_ad(!memcmp(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_PREV +
                  u->page.frame,
                  TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE + FLST_NEXT +
                  u->page.frame, FIL_ADDR_SIZE));

    /* Delete the last undo log header, which must be for this transaction.

    An undo segment can be reused (TRX_UNDO_CACHED) only if it
    comprises of one page and that single page contains enough space
    for the undo log header of a subsequent transaction. See
    trx_purge_add_undo_to_history(), which is executed when committing
    a nonempty transaction.

    If we simply changed the undo page state to TRX_UNDO_CACHED,
    then trx_undo_reuse_cached() could run out of space. We will
    release the space consumed by our empty undo log to avoid that. */
    for (byte *last= &u->page.frame[TRX_UNDO_SEG_HDR + TRX_UNDO_SEG_HDR_SIZE],
           *prev= nullptr;;)
    {
      /* TRX_UNDO_PREV_LOG is only being read in debug assertions, and
      written in trx_undo_header_create(). To remain compatible with
      possibly corrupted old data files, we will not read the field
      TRX_UNDO_PREV_LOG but instead rely on TRX_UNDO_NEXT_LOG. */
      ut_ad(mach_read_from_2(TRX_UNDO_PREV_LOG + last) ==
            (reinterpret_cast<size_t>(prev) & (srv_page_size - 1)));

      if (uint16_t next= mach_read_from_2(TRX_UNDO_NEXT_LOG + last))
      {
        ut_ad(ulint{next} + TRX_UNDO_LOG_XA_HDR_SIZE < srv_page_size - 100);
        ut_ad(&u->page.frame[next] > last);
        ut_ad(mach_read_from_2(TRX_UNDO_LOG_START + last) <= next);
        prev= last;
        last= &u->page.frame[next];
        continue;
      }

      ut_ad(mach_read_from_8(TRX_UNDO_TRX_ID + last) == id);
      ut_ad(!mach_read_from_8(TRX_UNDO_TRX_NO + last));
      ut_ad(!memcmp(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START + u->page.frame,
                    TRX_UNDO_LOG_START + last, 2));

      if (prev)
      {
        mtr->memcpy(*u, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_START +
                    u->page.frame, prev + TRX_UNDO_LOG_START, 2);
        const ulint free= last - u->page.frame;
        mtr->write<2>(*u, TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE +
                      u->page.frame, free);
        mtr->write<2>(*u, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + u->page.frame,
                      TRX_UNDO_CACHED);
        mtr->write<2>(*u, TRX_UNDO_SEG_HDR + TRX_UNDO_LAST_LOG + u->page.frame,
                      uintptr_t(prev - u->page.frame));
        mtr->write<2>(*u, prev + TRX_UNDO_NEXT_LOG, 0U);
        mtr->memset(u, free, srv_page_size - FIL_PAGE_DATA_END - free, 0);

        /* We may have updated PAGE_MAX_TRX_ID on secondary index pages
        to this->id. Ensure that trx_sys.m_max_trx_id will be recovered
        correctly, even though we removed our undo log record along
        with the TRX_UNDO_TRX_ID above. */

        /* Below, we are acquiring rseg_header->page.lock after
        u->page.lock (the opposite of trx_purge_add_undo_to_history()).
        This is fine, because both functions are holding exclusive
        rseg->latch. */

        if (mach_read_from_8(prev + TRX_UNDO_TRX_NO) >= id);
        else if (buf_block_t *rseg_header= rseg->get(mtr, nullptr))
        {
          byte *m= TRX_RSEG + TRX_RSEG_MAX_TRX_ID + rseg_header->page.frame;

          do
          {
            if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                               rseg_header->page.frame)))
              /* This must have been upgraded from before MariaDB 10.3.5. */
              trx_rseg_format_upgrade(rseg_header, mtr);
            else if (mach_read_from_8(m) >= id)
              continue;
            mtr->write<8>(*rseg_header, m, id);
          }
          while (0);
        }
      }
      else
        /* Our undo log header was right after the undo log segment header.
        This page should have been created by trx_undo_create(), not
        returned by trx_undo_reuse_cached().

        We retain the dummy empty log in order to remain compatible with
        trx_undo_mem_create_at_db_start(). This page will remain available
        to trx_undo_reuse_cached(), and it will eventually be freed by
        trx_purge_truncate_rseg_history(). */
        mtr->write<2>(*u, TRX_UNDO_SEG_HDR + TRX_UNDO_STATE + u->page.frame,
                      TRX_UNDO_CACHED);
      break;
    }
  }
  else
    ut_ad("undo log page was not found" == 0);

  UT_LIST_REMOVE(rseg->undo_list, undo);
  UT_LIST_ADD_FIRST(rseg->undo_cached, undo);
  undo->state= TRX_UNDO_CACHED;
  undo= nullptr;

  /* We must assign an "end" identifier even though we are not going
  to persistently write it anywhere, to make sure that the purge of
  history will not be stuck. */
  trx_sys.assign_new_trx_no(this);
}

/** Assign the transaction its history serialisation number and write the
UNDO log to the assigned rollback segment.
@param mtr   mini-transaction */
inline void trx_t::write_serialisation_history(mtr_t *mtr)
{
  ut_ad(!read_only);
  trx_rseg_t *rseg= rsegs.m_redo.rseg;
  trx_undo_t *&undo= rsegs.m_redo.undo;
  bool ownerless_history_lock_acquired= false;
  uint64_t ownerless_history_previous_visibility= 0;
  bool ownerless_history_visibility_pushed= false;
  uint32_t ownerless_history_undo_page_no= FIL_NULL;
  uint64_t mylite_deep_stage_start= 0;
  const bool ownerless_hooks=
    UNIV_UNLIKELY(mylite_ownerless_innodb_lock_has_hooks());
  const bool mylite_deep_stats_enabled=
    mylite_ownerless_innodb_deep_perf_stats_enabled_fast();
  if (UNIV_LIKELY(undo != nullptr))
  {
    MONITOR_INC(MONITOR_TRX_COMMIT_UNDO);

    bool ownerless_history_lock_waited= false;
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    while (ownerless_hooks)
    {
      uint32_t ownerless_history_lock_flags= 0;
      const int ownerless_history_lock_result=
        mylite_ownerless_innodb_lock_acquire_page_write(
          this, rseg->space->id, rseg->page_no, 30000U,
          &ownerless_history_lock_flags);
      if (ownerless_history_lock_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      {
        ownerless_history_lock_acquired= true;
        ownerless_history_lock_waited= ownerless_history_lock_waited ||
          (ownerless_history_lock_flags &
           MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED) != 0;
        break;
      }
      if (ownerless_history_lock_result ==
          MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
        break;
      if (ownerless_history_lock_result != MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
          ownerless_history_lock_result != MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
        ut_error;
      ownerless_history_lock_waited= true;
    }
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_LOCK_NS,
        mylite_deep_stage_start);
    if (ownerless_history_lock_waited)
    {
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
      uint64_t ownerless_latest_lsn= 0;
      const int ownerless_refresh_result=
        mylite_ownerless_innodb_redo_observe(&ownerless_latest_lsn);
      if (ownerless_refresh_result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
      {
        mylite_ownerless_innodb_refresh_external_pages(ownerless_latest_lsn);
        if (ownerless_latest_lsn != 0)
        {
          ownerless_history_previous_visibility=
            mylite_ownerless_innodb_push_external_page_visibility(
              ownerless_latest_lsn);
          ownerless_history_visibility_pushed= true;
        }
      }
      else if (ownerless_refresh_result !=
               MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      {
        ut_error;
      }
      mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_REFRESH_NS,
          mylite_deep_stage_start);
    }

    /* We have to hold exclusive rseg->latch because undo log headers have
    to be put to the history list in the (serialisation) order of the
    UNDO trx number. This is required for purge_sys too. */
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    rseg->latch.wr_lock(SRW_LOCK_CALL);
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_RSEG_LATCH_NS,
        mylite_deep_stage_start);
    uint64_t mylite_deep_history_list_start=
      mylite_ownerless_innodb_deep_perf_start_ns();
    ut_ad(undo->rseg == rseg);
    ownerless_history_undo_page_no= undo->hdr_page_no;
    /* Assign the transaction serialisation number and add any
    undo log to the purge queue. */
    if (UNIV_UNLIKELY(!undo_no))
    {
      /* The transaction was rolled back. */
      commit_empty(mtr);
      goto done;
    }
    else if (rseg->last_page_no == FIL_NULL)
    {
      /* trx_sys.assign_new_trx_no() and
      purge_sys.enqueue() must be invoked in the same
      critical section protected with purge queue mutex to avoid rseg with
      greater last commit number to be pushed to purge queue prior to rseg with
      lesser last commit number. In other words pushing to purge queue must be
      serialized along with assigning trx_no. Otherwise purge coordinator
      thread can also fetch redo log records from rseg with greater last commit
      number before rseg with lesser one. */
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
      purge_sys.queue_lock();
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_PURGE_QUEUE_NS,
          mylite_deep_stage_start);
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
      trx_sys.assign_new_trx_no(this);
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_ASSIGN_TRX_NO_NS,
          mylite_deep_stage_start);
      const trx_id_t end{rw_trx_hash_element->no};
      rseg->last_page_no= undo->hdr_page_no;
      /* end cannot be less than anything in rseg. User threads only
      produce events when a rollback segment is empty. */
      rseg->set_last_commit(undo->hdr_offset, end);
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
      purge_sys.enqueue(end, *rseg);
      purge_sys.queue_unlock();
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_PURGE_QUEUE_NS,
          mylite_deep_stage_start);
    }
    else
    {
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
      trx_sys.assign_new_trx_no(this);
      if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
        mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_ASSIGN_TRX_NO_NS,
          mylite_deep_stage_start);
    }
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
    UT_LIST_REMOVE(rseg->undo_list, undo);
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_UNDO_LIST_REMOVE_NS,
        mylite_deep_stage_start);
    /* Change the undo log segment state from TRX_UNDO_ACTIVE, to
    define the transaction as committed in the file based domain,
    at mtr->commit_lsn() obtained in mtr->commit() below. */
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
    trx_purge_add_undo_to_history(this, undo, mtr);
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_PURGE_ADD_UNDO_NS,
        mylite_deep_stage_start);
  done:
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_NS,
        mylite_deep_history_list_start);
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
    rseg->release();
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_RSEG_RELEASE_NS,
        mylite_deep_stage_start);
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_now_ns();
    rseg->latch.wr_unlock();
    if (UNIV_UNLIKELY(mylite_deep_stats_enabled))
      mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_HISTORY_LIST_RSEG_UNLOCK_NS,
        mylite_deep_stage_start);
  }
  else
    rseg->release();
  mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
  const bool ownerless_history_wal_proof_allowed=
    ownerless_hooks && ownerless_history_lock_acquired &&
    ownerless_history_undo_page_no != FIL_NULL &&
    ownerless_history_wal_proof_allows_fast_path(this);
  if (ownerless_history_wal_proof_allowed)
  {
    mylite_ownerless_history_proof_active= true;
    mylite_ownerless_history_proof_rseg_published= false;
    mylite_ownerless_history_proof_undo_published= false;
    mylite_ownerless_history_proof_space_id= rseg->space->id;
    mylite_ownerless_history_proof_rseg_page_no= rseg->page_no;
    mylite_ownerless_history_proof_undo_page_no= ownerless_history_undo_page_no;
  }
  mtr->commit();
  mylite_ownerless_innodb_deep_perf_add_elapsed(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_MTR_COMMIT_NS,
      mylite_deep_stage_start);
  const bool ownerless_history_wal_proved=
    ownerless_history_wal_proof_allowed && mtr->commit_lsn() != 0 &&
    !mylite_ownerless_page_write_publish_failed &&
    mylite_ownerless_history_proof_rseg_published &&
    mylite_ownerless_history_proof_undo_published;
  mylite_ownerless_history_proof_active= false;
  if (ownerless_hooks && ownerless_history_visibility_pushed)
    mylite_ownerless_innodb_restore_external_page_visibility(
      ownerless_history_previous_visibility);
  commit_lsn= undo_no || !xid.is_null() ? mtr->commit_lsn() : 0;
  if (ownerless_hooks && ownerless_history_lock_acquired)
  {
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    if (mtr->commit_lsn() != 0)
    {
      if (!ownerless_history_wal_proved)
      {
        const lsn_t flush_lsn= mtr->commit_lsn() < LSN_MAX - 1
          ? mtr->commit_lsn() + 1
          : LSN_MAX - 1;
        uint64_t ownerless_history_exact_flush_pages= 0;
        uint64_t ownerless_history_fallback_rounds= 0;
        const uint64_t ownerless_history_flush_pages=
          mylite_ownerless_innodb_flush_history_pages_to_lsn(
            rseg->space->id, rseg->page_no, ownerless_history_undo_page_no,
            flush_lsn, &ownerless_history_exact_flush_pages,
            &ownerless_history_fallback_rounds);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_FLUSH_PAGES,
          ownerless_history_flush_pages);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_EXACT_FLUSH_PAGES,
          ownerless_history_exact_flush_pages);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_EXACT_FLUSH_FALLBACK_ROUNDS,
          ownerless_history_fallback_rounds);
      }
    }
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_FLUSH_NS,
        mylite_deep_stage_start);
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    const int ownerless_history_release_result=
      mylite_ownerless_innodb_lock_release_page_write(
        this, rseg->space->id, rseg->page_no);
    if (ownerless_history_release_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
        ownerless_history_release_result !=
          MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      ut_error;
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_RELEASE_NS,
        mylite_deep_stage_start);
  }
}

/********************************************************************
Finalize a transaction containing updates for a FTS table. */
static
void
trx_finalize_for_fts_table(
/*=======================*/
	fts_trx_table_t*	ftt)	    /* in: FTS trx table */
{
	fts_t*		  fts = ftt->table->fts;
	fts_doc_ids_t*	  doc_ids = ftt->added_doc_ids;

	ut_a(fts->add_wq);

	mem_heap_t* heap = static_cast<mem_heap_t*>(doc_ids->self_heap->arg);

	ib_wqueue_add(fts->add_wq, doc_ids, heap);

	/* fts_trx_table_t no longer owns the list. */
	ftt->added_doc_ids = NULL;
}

/******************************************************************//**
Finalize a transaction containing updates to FTS tables. */
static
void
trx_finalize_for_fts(
/*=================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	is_commit)	/*!< in: true if the transaction was
				committed, false if it was rolled back. */
{
	if (is_commit) {
		const ib_rbt_node_t*	node;
		ib_rbt_t*		tables;
		fts_savepoint_t*	savepoint;

		savepoint = static_cast<fts_savepoint_t*>(
			ib_vector_last(trx->fts_trx->savepoints));

		tables = savepoint->tables;

		for (node = rbt_first(tables);
		     node;
		     node = rbt_next(tables, node)) {
			fts_trx_table_t**	ftt;

			ftt = rbt_value(fts_trx_table_t*, node);

			if ((*ftt)->added_doc_ids) {
				trx_finalize_for_fts_table(*ftt);
			}
		}
	}

	fts_trx_free(trx->fts_trx);
	trx->fts_trx = NULL;
}

extern "C" MYSQL_THD thd_increment_pending_ops(MYSQL_THD);
extern "C" void  thd_decrement_pending_ops(void*);


#include "../log/log0sync.h"

/*
  If required, initiates write and optionally flush of the log to
  disk
  @param lsn   LSN up to which logs are to be flushed.
  @param trx   transaction; if trx->state is PREPARED, the function will
  also wait for the flush to complete.
*/
static void trx_flush_log_if_needed(lsn_t lsn, trx_t *trx)
{
  ut_ad(srv_flush_log_at_trx_commit);
  ut_ad(trx->state != TRX_STATE_PREPARED);

  if (log_sys.get_flushed_lsn(std::memory_order_relaxed) >= lsn)
    return;

  ut_ad(!trx->mysql_thd || !trx->mysql_thd->tx_read_only);

  const bool flush= srv_flush_log_at_trx_commit & 1;
  if (!log_sys.is_mmap())
  {
    completion_callback cb;

    if ((cb.m_param= thd_increment_pending_ops(trx->mysql_thd)))
    {
      cb.m_callback= thd_decrement_pending_ops;
      log_write_up_to(lsn, flush, &cb);
      return;
    }
  }
  trx->op_info= "flushing log";
  log_write_up_to(lsn, flush);
  trx->op_info= "";
}

/** Process tables that were modified by the committing transaction. */
inline void trx_t::commit_tables()
{
  if (undo_no && !mod_tables.empty())
  {
    const trx_id_t max_trx_id= trx_sys.get_max_trx_id();
    const auto now= start_time;

    for (const auto &p : mod_tables)
    {
      dict_table_t *table= p.first;
      table->update_time= now;
      table->query_cache_inv_trx_id= max_trx_id;
    }
  }
}

/** Evict a table definition due to the rollback of ALTER TABLE.
@param table_id   table identifier
@param reset_only whether to only reset dict_table_t::def_trx_id */
void trx_t::evict_table(table_id_t table_id, bool reset_only)
{
	ut_ad(in_rollback);

	dict_table_t* table = dict_sys.find_table(table_id);
	if (!table) {
		return;
	}

	table->def_trx_id = 0;

	if (auto ref_count = table->get_ref_count()) {
		/* This must be a DDL operation that is being rolled
		back in an active connection. */
		ut_a(ref_count == 1);
		ut_ad(!is_recovered);
		ut_ad(mysql_thd);
		return;
	}

	if (reset_only) {
		return;
	}

	/* This table should only be locked by this transaction, if at all. */
	ut_ad(UT_LIST_GET_LEN(table->locks) <= 1);
	const bool locked = UT_LIST_GET_LEN(table->locks);
	ut_ad(!locked || UT_LIST_GET_FIRST(table->locks)->trx == this);
	dict_sys.remove(table, true, locked);
	if (locked) {
		UT_LIST_ADD_FIRST(lock.evicted_tables, table);
	}
}

/** Free temporary undo log after commit or rollback.
@param mtr   mini-transaction
@param undo  temporary undo log */
ATTRIBUTE_NOINLINE static void trx_commit_cleanup(mtr_t *mtr,
                                                  trx_undo_t *&undo)
{
  trx_rseg_t *const rseg= undo->rseg;
  ut_ad(rseg->space == fil_system.temp_space);
  rseg->latch.wr_lock(SRW_LOCK_CALL);
  UT_LIST_REMOVE(rseg->undo_list, undo);
  ut_ad(undo->state == TRX_UNDO_ACTIVE || undo->state == TRX_UNDO_PREPARED);
  ut_ad(undo->id < TRX_RSEG_N_SLOTS);
  /* Delete first the undo log segment in the file */
  bool finished;
  do
  {
    mtr->start();
    mtr->set_log_mode(MTR_LOG_NO_REDO);

    finished= true;

    if (buf_block_t *block=
        buf_page_get(page_id_t(SRV_TMP_SPACE_ID, undo->hdr_page_no), 0,
                     RW_X_LATCH, mtr))
    {
      finished= fseg_free_step(block, TRX_UNDO_SEG_HDR + TRX_UNDO_FSEG_HEADER,
                               mtr);

      if (!finished);
      else if (buf_block_t *rseg_header= rseg->get(mtr, nullptr))
      {
        static_assert(FIL_NULL == 0xffffffff, "compatibility");
        memset(rseg_header->page.frame + TRX_RSEG + TRX_RSEG_UNDO_SLOTS +
               undo->id * TRX_RSEG_SLOT_SIZE, 0xff, 4);
      }
    }

    mtr->commit();
  }
  while (!finished);

  ut_ad(rseg->curr_size > undo->size);
  rseg->curr_size-= undo->size;
  rseg->latch.wr_unlock();
  ut_free(undo);
  undo= nullptr;
}

TRANSACTIONAL_INLINE inline void trx_t::commit_in_memory(mtr_t *mtr)
{
  mylite_ownerless_innodb_deep_perf_count(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_CALLS);
  mylite_ownerless_innodb_deep_perf_scope mylite_deep_perf_scope(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_TOTAL_NS);
  uint64_t mylite_deep_stage_start= 0;

  /* We already detached from rseg in write_serialisation_history() */
  ut_ad(!rsegs.m_redo.undo);
  read_view.close();

  if (is_autocommit_non_locking())
  {
    ut_ad(id == 0);
    ut_ad(read_only);
    ut_ad(!will_lock);
    ut_a(!is_recovered);
    ut_ad(!rsegs.m_redo.rseg);
    ut_ad(!rsegs.m_redo.undo);
    ut_ad(mysql_thd);
    ut_ad(state == TRX_STATE_ACTIVE);

    /* Note: We do not have to hold any lock_sys latch here, because
    this is a non-locking transaction. */
    ut_a(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(UT_LIST_GET_LEN(lock.evicted_tables) == 0);

    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    /* This state change is not protected by any mutex, therefore
    there is an inherent race here around state transition during
    printouts. We ignore this race for the sake of efficiency.
    However, the freezing of trx_sys.trx_list will protect the trx_t
    instance and it cannot be removed from the trx_list and freed
    without first unfreezing trx_list. */
    state= TRX_STATE_NOT_STARTED;

    MONITOR_INC(MONITOR_TRX_NL_RO_COMMIT);

    DBUG_LOG("trx", "Autocommit in memory: " << this);
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_STATE_NS,
        mylite_deep_stage_start);
  }
  else
  {
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
#ifdef UNIV_DEBUG
    if (!UT_LIST_GET_LEN(lock.trx_locks))
      for (auto l : lock.table_locks)
        ut_ad(!l);
#endif /* UNIV_DEBUG */
    commit_state();

    if (id)
    {
      trx_sys.deregister_rw(this);

      /* Wait for any implicit-to-explicit lock conversions to cease,
      so that there will be no race condition in lock_release(). */
      while (UNIV_UNLIKELY(is_referenced()))
        LF_BACKOFF();
    }
    else
      ut_ad(read_only || !rsegs.m_redo.rseg);

    if (read_only || !rsegs.m_redo.rseg)
    {
      MONITOR_INC(MONITOR_TRX_RO_COMMIT);
    }
    else
    {
      commit_tables();
      MONITOR_INC(MONITOR_TRX_RW_COMMIT);
      is_recovered= false;
    }

    const bool release_ownerless_locks_after_flush =
      UNIV_UNLIKELY(mylite_ownerless_innodb_lock_has_hooks()) && !read_only &&
	      (id != 0 || mylite_ownerless_lock_trx_id != 0 ||
	       mylite_ownerless_page_write_trx_id != 0 ||
	       !mylite_ownerless_modified_pages_empty() ||
	       !mylite_ownerless_dirty_pages_empty());
    if (UNIV_LIKELY(!dict_operation) && !release_ownerless_locks_after_flush)
      release_locks();
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_STATE_NS,
        mylite_deep_stage_start);
  }

  lsn_t ownerless_commit_lsn= commit_lsn;

  if (commit_lsn)
  {
    /* Depending on the my.cnf options, we may now write the log
    buffer to the log files, making the transaction durable if the OS
    does not crash. We may also flush the log files to disk, making
    the transaction durable also at an OS crash or a power outage.

    The idea in InnoDB's group commit is that a group of transactions
    gather behind a trx doing a physical disk write to log files, and
    when that physical write has been completed, one of those
    transactions does a write which commits the whole group. Note that
    this group commit will only bring benefit if there are > 2 users
    in the database. Then at least 2 users can gather behind one doing
    the physical log write to disk.

    If we are calling trx_t::commit() under prepare_commit_mutex, we
    will delay possible log write and flush to a separate function
    trx_commit_complete_for_mysql(), which is only called when the
    thread has released the mutex. This is to make the group commit
    algorithm to work. Otherwise, the prepare_commit mutex would
    serialize all commits and prevent a group of transactions from
    gathering. */

    if (!flush_log_later && srv_flush_log_at_trx_commit)
    {
      mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
      const uint64_t ownerless_log_flush_start=
        ownerless_commit_visibility_stats_enabled.load(
            std::memory_order_relaxed) &&
        mylite_ownerless_innodb_lock_has_hooks()
            ? ownerless_commit_visibility_now_ns()
            : 0;
      trx_flush_log_if_needed(commit_lsn, this);
      ownerless_commit_visibility_add_elapsed(
          ownerless_commit_visibility_log_flush_ns, ownerless_log_flush_start);
      commit_lsn= 0;
      mylite_ownerless_innodb_deep_perf_add_elapsed(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_LOG_FLUSH_NS,
          mylite_deep_stage_start);
    }
  }

  if (UNIV_UNLIKELY(mylite_ownerless_innodb_lock_has_hooks()) && !read_only &&
	      (id != 0 || mylite_ownerless_lock_trx_id != 0 ||
	       mylite_ownerless_page_write_trx_id != 0 ||
	       !mylite_ownerless_modified_pages_empty() ||
	       !mylite_ownerless_dirty_pages_empty()))
  {
    const uint64_t mylite_deep_ownerless_start=
        mylite_ownerless_innodb_deep_perf_start_ns();
    const bool ownerless_visibility_stats_enabled=
      ownerless_commit_visibility_stats_enabled.load(
          std::memory_order_relaxed);
    const uint64_t ownerless_visibility_start=
      ownerless_visibility_stats_enabled ? ownerless_commit_visibility_now_ns() : 0;
    const bool ownerless_statement_allows_visible_fast_path=
      ownerless_sql_command_allows_visible_fast_path(this);
    const bool publish_ownerless_dirty_pages =
      dict_operation || ownerless_sql_command_requires_dirty_page_bridge(this);
    const bool ownerless_commit_needs_recovery_lsn=
      lock.was_chosen_as_deadlock_victim || ownerless_commit_lsn == 0;
    if (ownerless_commit_needs_recovery_lsn && !in_rollback)
      ownerless_commit_lsn= log_get_lsn();
    uint64_t ownerless_stage_start= 0;
    if (!in_rollback)
    {
      ownerless_stage_start=
        ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
      ownerless_commit_lsn= static_cast<lsn_t>(
          mylite_ownerless_innodb_publish_transaction_pages_to_lsn(
              this, ownerless_commit_lsn));
      ownerless_commit_visibility_add_elapsed(
          ownerless_commit_visibility_publish_transaction_pages_ns,
          ownerless_stage_start);
      const bool ownerless_has_deferred_page_writes=
        ownerless_transaction_has_deferred_page_writes(this);
      const bool ownerless_deferred_page_writes_proved=
        !ownerless_has_deferred_page_writes ||
        mylite_ownerless_page_write_deferred_pages_published;
      const bool publish_ownerless_visible_without_flush=
        !ownerless_commit_needs_recovery_lsn &&
        !publish_ownerless_dirty_pages &&
        ownerless_statement_allows_visible_fast_path &&
        mylite_ownerless_page_write_trx_id != 0 &&
        ownerless_deferred_page_writes_proved &&
        !mylite_ownerless_page_write_publish_failed &&
        mylite_ownerless_page_write_published_page;
      if (publish_ownerless_visible_without_flush)
        ownerless_commit_visibility_count_if_enabled(
            ownerless_visibility_stats_enabled,
            ownerless_commit_visibility_fast);
      else
      {
        ownerless_commit_visibility_count_if_enabled(
            ownerless_visibility_stats_enabled,
            ownerless_commit_visibility_flush);
        if (ownerless_commit_needs_recovery_lsn)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_recovery_lsn);
        if (publish_ownerless_dirty_pages)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_dirty_pages);
        if (mylite_ownerless_page_write_trx_id == 0)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_no_page_write_trx);
        if (!ownerless_deferred_page_writes_proved)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_deferred_pages);
        if (mylite_ownerless_page_write_publish_failed)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_publish_failed);
        if (!mylite_ownerless_page_write_published_page)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_no_published_pages);
        if (!ownerless_statement_allows_visible_fast_path)
          ownerless_commit_visibility_count_if_enabled(
              ownerless_visibility_stats_enabled,
              ownerless_commit_visibility_flush_unproven_statement);
      }
      ownerless_stage_start=
        ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
      if (publish_ownerless_dirty_pages)
        mylite_ownerless_innodb_publish_dirty_pages_to_lsn(ownerless_commit_lsn);
      ownerless_commit_visibility_add_elapsed(
          ownerless_commit_visibility_publish_dirty_pages_ns,
          ownerless_stage_start);
      ownerless_stage_start=
        ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
      if (publish_ownerless_visible_without_flush)
        mylite_ownerless_innodb_publish_pages_visible_lsn(ownerless_commit_lsn);
      else
      {
        const lsn_t ownerless_flush_lsn=
          std::max<lsn_t>(ownerless_commit_lsn, log_get_lsn());
        ownerless_commit_visibility_add_elapsed(
            ownerless_commit_visibility_publish_visible_ns,
            ownerless_stage_start);
        ownerless_stage_start=
          ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
        mylite_ownerless_innodb_flush_dirty_pages_for_page_writes(
            ownerless_flush_lsn);
        uint64_t ownerless_exact_flush_pages= 0;
        uint64_t ownerless_fallback_rounds= 0;
        const uint64_t ownerless_transaction_flush_pages=
          mylite_ownerless_innodb_flush_transaction_pages_for_page_writes(
              this, ownerless_flush_lsn, &ownerless_exact_flush_pages,
              &ownerless_fallback_rounds);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_FLUSH_PAGES,
          ownerless_transaction_flush_pages);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_EXACT_FLUSH_PAGES,
          ownerless_exact_flush_pages);
        mylite_ownerless_innodb_deep_perf_add(
          MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_OWNERLESS_EXACT_FLUSH_FALLBACK_ROUNDS,
          ownerless_fallback_rounds);
        ownerless_commit_visibility_add_elapsed(
            ownerless_commit_visibility_flush_dirty_pages_ns,
            ownerless_stage_start);
        ownerless_stage_start=
          ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
        mylite_ownerless_innodb_publish_pages_visible_lsn(ownerless_flush_lsn);
      }
      ownerless_commit_visibility_add_elapsed(
          ownerless_commit_visibility_publish_visible_ns, ownerless_stage_start);
    }
    ownerless_stage_start=
      ownerless_visibility_start != 0 ? ownerless_commit_visibility_now_ns() : 0;
    if (UNIV_LIKELY(!dict_operation))
      release_locks();
    else
      mylite_ownerless_innodb_lock_release_transaction_page_writes(this);
    ownerless_commit_visibility_add_elapsed(
        ownerless_commit_visibility_release_locks_ns, ownerless_stage_start);
    ownerless_commit_visibility_add_elapsed(
        ownerless_commit_visibility_total_ns, ownerless_visibility_start);
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_OWNERLESS_NS,
        mylite_deep_ownerless_start);
  }

  mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
  if (trx_undo_t *&undo= rsegs.m_noredo.undo)
  {
    ut_ad(undo->rseg == rsegs.m_noredo.rseg);
    trx_commit_cleanup(mtr, undo);
  }
  mylite_ownerless_innodb_deep_perf_add_elapsed(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_TEMP_UNDO_NS,
      mylite_deep_stage_start);

  mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
  if (fts_trx)
    trx_finalize_for_fts(this, undo_no != 0);

#ifdef WITH_WSREP
  ut_ad(is_wsrep() == wsrep_on(mysql_thd));

  /* Serialization history has been written and the transaction is
  committed in memory, which makes this commit ordered. Release commit
  order critical section. */
  if (wsrep)
  {
    wsrep= false;
    wsrep_commit_ordered(mysql_thd);
  }
#endif /* WITH_WSREP */
  lock.was_chosen_as_deadlock_victim= false;
  mylite_ownerless_innodb_deep_perf_add_elapsed(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_IN_MEMORY_FTS_WSREP_NS,
      mylite_deep_stage_start);
}

bool trx_t::commit_cleanup() noexcept
{
  mylite_ownerless_innodb_deep_perf_count(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_CLEANUP_CALLS);
  mylite_ownerless_innodb_deep_perf_scope mylite_deep_perf_scope(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_CLEANUP_TOTAL_NS);
  ut_ad(!dict_operation);
  ut_ad(!was_dict_operation);

  if (is_bulk_insert())
    for (auto &t : mod_tables)
      delete t.second.bulk_store;

  if (size_t n_page_gets= pages_accessed)
  {
    pages_accessed= 0;
    buf_pool.stat.n_page_gets+= n_page_gets;
  }
  mutex.wr_lock();
  state= TRX_STATE_NOT_STARTED;
  *detailed_error= '\0';
  mylite_ownerless_page_write_publish_failed= false;
  mylite_ownerless_page_write_published_page= false;
  mylite_ownerless_page_write_deferred_pages_published= false;
  mylite_ownerless_history_proof_active= false;
  mylite_ownerless_history_proof_rseg_published= false;
  mylite_ownerless_history_proof_undo_published= false;
  mylite_ownerless_history_proof_space_id= 0;
  mylite_ownerless_history_proof_rseg_page_no= 0;
  mylite_ownerless_history_proof_undo_page_no= 0;
  mylite_ownerless_modified_pages_clear();
  mod_tables.clear();

  bulk_insert= TRX_NO_BULK;
  mylite_ownerless_default_checked_bulk_insert_sql_started= false;
  check_foreigns= true;
  check_unique_secondary= true;
  assert_freed();
  trx_init(this);
  mutex.wr_unlock();

  ut_a(error_state == DB_SUCCESS);
  return false;
}

bool trx_t::mylite_ownerless_default_checked_bulk_insert_allowed(
    const dict_table_t &table) const noexcept
{
  if (!mysql_thd || thd_test_options(mysql_thd, OPTION_NOT_AUTOCOMMIT |
                                     OPTION_BEGIN) ||
      !mylite_ownerless_innodb_lock_has_hooks() ||
      !mylite_ownerless_default_checked_bulk_insert_sql_started ||
      !ownerless_sql_command_allows_default_checked_bulk_insert(this))
    return false;
  if (!check_unique_secondary && !check_foreigns)
    return false;
  if (check_unique_secondary && UT_LIST_GET_LEN(table.indexes) != 1)
    return false;
  if (check_foreigns && (!table.foreign_set.empty() ||
                         !table.referenced_set.empty()))
    return false;
  return true;
}

/** Commit the transaction in the file system. */
TRANSACTIONAL_TARGET void trx_t::commit_persist() noexcept
{
  mylite_ownerless_innodb_deep_perf_count(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_CALLS);
  mylite_ownerless_innodb_deep_perf_scope mylite_deep_perf_scope(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_TOTAL_NS);
  uint64_t mylite_deep_stage_start= 0;
  mtr_t mtr{this};
  mtr.start();

  if (fts_trx && undo_no)
  {
    ut_a(!is_autocommit_non_locking());
    /* MDEV-24088 FIXME: Invoke fts_commit() earlier (before possible
    XA PREPARE), so that we will be able to return an error and rollback
    the transaction, instead of violating consistency!

    The original claim about DB_DUPLICATE KEY was:
    This is a possible scenario if there is a crash between
    insert to DELETED table committing and transaction committing. The
    fix would be able to return error from this function */
    if (ut_d(dberr_t error=) fts_commit(this))
      ut_ad(error == DB_DUPLICATE_KEY || error == DB_LOCK_WAIT_TIMEOUT);
  }

#ifdef ENABLED_DEBUG_SYNC
  const bool debug_sync= mysql_thd && has_logged_persistent();
#endif
  commit_lsn =0;

  if (has_logged_persistent())
  {
    if (UNIV_UNLIKELY(apply_online_log))
      apply_log();

    /* The following call commits the mini-transaction, making the
    whole transaction committed in the file-based world, at this log
    sequence number. The transaction becomes 'durable' when we write
    the log to disk, but in the logical sense the commit in the
    file-based data structures (undo logs etc.) happens here.

    NOTE that transaction numbers do not necessarily come in
    exactly the same order as commit lsn's, if the transactions have
    different rollback segments. However, if a transaction T2 is
    able to see modifications made by a transaction T1, T2 will always
    get a bigger transaction number and a bigger commit lsn than T1. */
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    write_serialisation_history(&mtr);
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_WRITE_HISTORY_NS,
        mylite_deep_stage_start);
  }
  else if (trx_rseg_t *rseg= rsegs.m_redo.rseg)
  {
    ut_ad(id);
    ut_ad(!rsegs.m_redo.undo);
    rseg->release();
  }

#ifdef ENABLED_DEBUG_SYNC
  if (debug_sync)
    DEBUG_SYNC_C("before_trx_state_committed_in_memory");
#endif

  mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
  commit_in_memory(&mtr);
  mylite_ownerless_innodb_deep_perf_add_elapsed(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_PERSIST_IN_MEMORY_NS,
      mylite_deep_stage_start);
}


bool trx_t::commit() noexcept
{
  mylite_ownerless_innodb_deep_perf_count(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_CALLS);
  mylite_ownerless_innodb_deep_perf_scope mylite_deep_perf_scope(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_TOTAL_NS);
  ut_ad(!was_dict_operation);
  ut_d(was_dict_operation= dict_operation);
  dict_operation= false;
  commit_persist();
#ifdef UNIV_DEBUG
  if (!was_dict_operation)
    for (const auto &p : mod_tables) ut_ad(!p.second.is_dropped());
#endif /* UNIV_DEBUG */
  ut_d(was_dict_operation= false);
  return commit_cleanup();
}


/****************************************************************//**
Prepares a transaction for commit/rollback. */
void
trx_commit_or_rollback_prepare(
/*===========================*/
	trx_t*	trx)		/*!< in/out: transaction */
{
	/* We are reading trx->state without holding trx->mutex
	here, because the commit or rollback should be invoked for a
	running (or recovered prepared) transaction that is associated
	with the current thread. */

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, true);
		/* fall through */

	case TRX_STATE_ACTIVE:
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		trx->lock.wait_thr = NULL;
		return;

	case TRX_STATE_COMMITTED_IN_MEMORY:
	case TRX_STATE_ABORTED:
		break;
	}

	ut_error;
}

/*********************************************************************//**
Creates a commit command node struct.
@return own: commit node struct */
commit_node_t*
trx_commit_node_create(
/*===================*/
	mem_heap_t*	heap)	/*!< in: mem heap where created */
{
	commit_node_t*	node;

	node = static_cast<commit_node_t*>(mem_heap_alloc(heap, sizeof(*node)));
	node->common.type  = QUE_NODE_COMMIT;
	node->state = COMMIT_NODE_SEND;

	return(node);
}

/***********************************************************//**
Performs an execution step for a commit type node in a query graph.
@return query thread to run next, or NULL */
que_thr_t*
trx_commit_step(
/*============*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	commit_node_t*	node;

	node = static_cast<commit_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_COMMIT);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = COMMIT_NODE_SEND;
	}

	if (node->state == COMMIT_NODE_SEND) {
		trx_t*	trx;

		node->state = COMMIT_NODE_WAIT;

		trx = thr_get_trx(thr);

		ut_a(trx->lock.wait_thr == NULL);

		trx_commit_or_rollback_prepare(trx);

		trx->commit();
		ut_ad(trx->lock.wait_thr == NULL);

		thr = NULL;
	} else {
		ut_ad(node->state == COMMIT_NODE_WAIT);

		node->state = COMMIT_NODE_SEND;

		thr->run_node = que_node_get_parent(node);
	}

	return(thr);
}

void trx_commit_for_mysql(trx_t *trx) noexcept
{
  mylite_ownerless_innodb_deep_perf_count(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_FOR_MYSQL_CALLS);
  mylite_ownerless_innodb_deep_perf_scope mylite_deep_perf_scope(
      MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_FOR_MYSQL_TOTAL_NS);
  uint64_t mylite_deep_stage_start= 0;

  switch (trx->state) {
  case TRX_STATE_ABORTED:
    trx->state= TRX_STATE_NOT_STARTED;
    /* fall through */
  case TRX_STATE_NOT_STARTED:
    trx->will_lock= false;
    break;
  case TRX_STATE_ACTIVE:
  case TRX_STATE_PREPARED:
  case TRX_STATE_PREPARED_RECOVERED:
    trx->op_info= "committing";
    mylite_deep_stage_start= mylite_ownerless_innodb_deep_perf_start_ns();
    trx->commit();
    mylite_ownerless_innodb_deep_perf_add_elapsed(
        MYLITE_OWNERLESS_INNODB_DEEP_TRX_COMMIT_FOR_MYSQL_COMMIT_NS,
        mylite_deep_stage_start);
    trx->op_info= "";
    break;
  case TRX_STATE_COMMITTED_IN_MEMORY:
    ut_error;
    break;
  }
}

/** Durably write log until trx->commit_lsn
(if trx_t::commit_in_memory() was invoked with flush_log_later=true). */
void trx_commit_complete_for_mysql(trx_t *trx)
{
  const lsn_t lsn= trx->commit_lsn;
  if (!lsn)
    return;
  uint64_t ownerless_log_flush_start= 0;
  switch (srv_flush_log_at_trx_commit) {
  case 0:
    goto func_exit;
  case 1:
    if (trx->active_commit_ordered)
      return;
  }
  ownerless_log_flush_start=
    ownerless_commit_visibility_stats_enabled.load(
        std::memory_order_relaxed) &&
    mylite_ownerless_innodb_lock_has_hooks()
        ? ownerless_commit_visibility_now_ns()
        : 0;
  trx_flush_log_if_needed(lsn, trx);
  ownerless_commit_visibility_add_elapsed(
      ownerless_commit_visibility_log_flush_ns, ownerless_log_flush_start);
 func_exit:
  trx->commit_lsn= 0;
}

/**********************************************************************//**
Prints info about a transaction. */
void
trx_print_low(
/*==========*/
	FILE*		f,
			/*!< in: output stream */
	const trx_t*	trx,
			/*!< in: transaction */
	ulint		n_rec_locks,
			/*!< in: trx->lock.n_rec_locks */
	ulint		n_trx_locks,
			/*!< in: length of trx->lock.trx_locks */
	ulint		heap_size)
			/*!< in: mem_heap_get_size(trx->lock.lock_heap) */
{
	if (const trx_id_t id = trx->id) {
		fprintf(f, "TRANSACTION " TRX_ID_FMT, id);
	} else {
		fprintf(f, "TRANSACTION (%p)", trx);
	}

	THD* thd = trx->mysql_thd;

	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		fputs(", not started", f);
		thd = nullptr;
		goto state_ok;
	case TRX_STATE_ABORTED:
		fputs(", forced rollback done", f);
		thd = nullptr;
		goto state_ok;
	case TRX_STATE_ACTIVE:
		fprintf(f, ", ACTIVE %lu sec",
			(ulong) difftime(time(NULL), trx->start_time));
		goto state_ok;
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
		fprintf(f, ", ACTIVE (PREPARED) %lu sec",
			(ulong) difftime(time(NULL), trx->start_time));
		goto state_ok;
	case TRX_STATE_COMMITTED_IN_MEMORY:
		fputs(", COMMITTED IN MEMORY", f);
		goto state_ok;
	}
	fprintf(f, ", state %lu", (ulong) trx->state);
	ut_ad(0);
state_ok:
	const char* op_info = trx->op_info;

	if (*op_info) {
		putc(' ', f);
		fputs(op_info, f);
	}

	if (trx->is_recovered) {
		fputs(" recovered trx", f);
	}

	putc('\n', f);

	if (trx->n_mysql_tables_in_use > 0 || trx->mysql_n_tables_locked > 0) {
		fprintf(f, "mysql tables in use %lu, locked %lu\n",
			(ulong) trx->n_mysql_tables_in_use,
			(ulong) trx->mysql_n_tables_locked);
	}

	bool newline = true;

	if (trx->in_rollback) { /* dirty read for performance reasons */
		fputs("ROLLING BACK ", f);
	} else if (trx->lock.wait_lock) {
		fputs("LOCK WAIT ", f);
	} else {
		newline = false;
	}

	if (n_trx_locks > 0 || heap_size > 400) {
		newline = true;

		fprintf(f, "%lu lock struct(s), heap size %lu,"
			" %lu row lock(s)",
			(ulong) n_trx_locks,
			(ulong) heap_size,
			(ulong) n_rec_locks);
	}

	if (trx->undo_no != 0) {
		newline = true;
		fprintf(f, ", undo log entries " TRX_ID_FMT, trx->undo_no);
	}

	if (newline) {
		putc('\n', f);
	}

	if (thd) {
		innobase_mysql_print_thd(f, thd);
	}
}

/**********************************************************************//**
Prints info about a transaction.
The caller must hold lock_sys.latch.
When possible, use trx_print() instead. */
void
trx_print_latched(
/*==============*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx)		/*!< in: transaction */
{
	lock_sys.assert_locked();

	trx_print_low(f, trx,
		      trx->lock.n_rec_locks,
		      UT_LIST_GET_LEN(trx->lock.trx_locks),
		      mem_heap_get_size(trx->lock.lock_heap));
}

/**********************************************************************//**
Prints info about a transaction.
Acquires and releases lock_sys.latch. */
TRANSACTIONAL_TARGET
void
trx_print(
/*======*/
	FILE*		f,		/*!< in: output stream */
	const trx_t*	trx)		/*!< in: transaction */
{
  ulint n_rec_locks, n_trx_locks, heap_size;
  {
    TMLockMutexGuard g{SRW_LOCK_CALL};
    n_rec_locks= trx->lock.n_rec_locks;
    n_trx_locks= UT_LIST_GET_LEN(trx->lock.trx_locks);
    heap_size= mem_heap_get_size(trx->lock.lock_heap);
  }

  trx_print_low(f, trx, n_rec_locks, n_trx_locks, heap_size);
}

/** Prepare a transaction.
@return	log sequence number that makes the XA PREPARE durable
@retval	0	if no changes needed to be made durable */
static lsn_t trx_prepare_low(trx_t *trx)
{
	ut_ad(!trx->is_recovered);

	mtr_t mtr{trx};

	if (trx_undo_t* undo = trx->rsegs.m_noredo.undo) {
		ut_ad(undo->rseg == trx->rsegs.m_noredo.rseg);

		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		trx_undo_set_state_at_prepare(undo, false, &mtr);
		mtr.commit();
	}

	trx_undo_t* undo = trx->rsegs.m_redo.undo;

	if (!undo) {
		/* There were no changes to persistent tables. */
		return(0);
	}

	ut_ad(undo->rseg == trx->rsegs.m_redo.rseg);

	mtr.start();

	/* Change the undo log segment states from TRX_UNDO_ACTIVE to
	TRX_UNDO_PREPARED: these modifications to the file data
	structure define the transaction as prepared in the file-based
	world, at the serialization point of lsn. */
	trx_undo_set_state_at_prepare(undo, false, &mtr);

	/* Make the XA PREPARE durable. */
	mtr.commit();
	ut_ad(mtr.commit_lsn() > 0);
	return(mtr.commit_lsn());
}

/****************************************************************//**
Prepares a transaction. */
TRANSACTIONAL_TARGET
static
void
trx_prepare(
/*========*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	/* Only fresh user transactions can be prepared.
	Recovered transactions cannot. */
	ut_a(!trx->is_recovered);

	lsn_t	lsn = trx_prepare_low(trx);

	ut_a(trx->state == TRX_STATE_ACTIVE);
	{
		TMTrxGuard tg{*trx};
		trx->state = TRX_STATE_PREPARED;
	}

	if (lsn) {
		/* Depending on the my.cnf options, we may now write the log
		buffer to the log files, making the prepared state of the
		transaction durable if the OS does not crash. We may also
		flush the log files to disk, making the prepared state of the
		transaction durable also at an OS crash or a power outage.

		The idea in InnoDB's group prepare is that a group of
		transactions gather behind a trx doing a physical disk write
		to log files, and when that physical write has been completed,
		one of those transactions does a write which prepares the whole
		group. Note that this group prepare will only bring benefit if
		there are > 2 users in the database. Then at least 2 users can
		gather behind one doing the physical log write to disk.

		We must not be holding any mutexes or latches here. */
		if (auto f = srv_flush_log_at_trx_commit) {
			log_write_up_to(lsn, f & 1);
		}

		if (!UT_LIST_GET_LEN(trx->lock.trx_locks)
		    || trx->isolation_level == TRX_ISO_SERIALIZABLE) {
			/* Do not release any locks at the
			SERIALIZABLE isolation level. */
		} else if (!trx->mysql_thd
			   || trx->mysql_thd->lex->sql_command
			   != SQLCOM_XA_PREPARE) {
			/* Do not release locks for XA COMMIT ONE PHASE
			or for internal distributed transactions
			(XID::get_my_xid() would be nonzero). */
		} else {
			lock_release_on_prepare(trx);
		}
	}
}

/** XA PREPARE a transaction.
@param[in,out]	trx	transaction to prepare */
void trx_prepare_for_mysql(trx_t* trx)
{
	trx_start_if_not_started_xa(trx, false);

	trx->op_info = "preparing";

	trx_prepare(trx);

	trx->op_info = "";
}


struct trx_recover_for_mysql_callback_arg
{
  XID *xid_list;
  uint len;
  uint count;
};


static my_bool trx_recover_for_mysql_callback(rw_trx_hash_element_t *element,
  trx_recover_for_mysql_callback_arg *arg)
{
  DBUG_ASSERT(arg->len > 0);
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    /*
      The state of a read-write transaction can only change from ACTIVE to
      PREPARED while we are holding the element->mutex. But since it is
      executed at startup no state change should occur.
    */
    if (trx_state_eq(trx, TRX_STATE_PREPARED))
    {
      ut_ad(trx->is_recovered);
      ut_ad(trx->id);
      if (arg->count == 0)
        ib::info() << "Starting recovery for XA transactions...";
      XID& xid= arg->xid_list[arg->count];
      if (arg->count++ < arg->len)
      {
        trx->state= TRX_STATE_PREPARED_RECOVERED;
        ib::info() << "Transaction " << trx->id
                   << " in prepared state after recovery";
        ib::info() << "Transaction contains changes to " << trx->undo_no
                   << " rows";
        xid= trx->xid;
      }
    }
  }
  element->mutex.wr_unlock();
  /* Do not terminate upon reaching arg->len; count all transactions */
  return false;
}


static my_bool trx_recover_reset_callback(void *el, void*)
{
  rw_trx_hash_element_t *element= static_cast<rw_trx_hash_element_t*>(el);
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    if (trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED))
      trx->state= TRX_STATE_PREPARED;
  }
  element->mutex.wr_unlock();
  return false;
}


/**
  Find prepared transaction objects for recovery.

  @param[out]  xid_list  prepared transactions
  @param[in]   len       number of slots in xid_list

  @return number of prepared transactions stored in xid_list
*/

int trx_recover_for_mysql(XID *xid_list, uint len)
{
  trx_recover_for_mysql_callback_arg arg= { xid_list, len, 0 };

  ut_ad(xid_list);
  ut_ad(len);

  /* Fill xid_list with PREPARED transactions. */
  trx_sys.rw_trx_hash.iterate_no_dups(trx_recover_for_mysql_callback, &arg);
  if (arg.count)
  {
    ib::info() << arg.count
        << " transactions in prepared state after recovery";
    /* After returning the full list, reset the state, because
    init_server_components() wants to recover the collection of
    transactions twice, by first calling tc_log->open() and then
    ha_recover() directly. */
    if (arg.count <= len)
      trx_sys.rw_trx_hash.iterate(trx_recover_reset_callback);
  }
  return int(std::min(arg.count, len));
}


struct trx_get_trx_by_xid_callback_arg
{
  const XID *xid;
  trx_t *trx;
};


static my_bool trx_get_trx_by_xid_callback(void *el, void *a)
{
  auto element= static_cast<rw_trx_hash_element_t*>(el);
  auto arg= static_cast<trx_get_trx_by_xid_callback_arg*>(a);
  my_bool found= 0;
  element->mutex.wr_lock();
  if (trx_t *trx= element->trx)
  {
    trx->mutex_lock();
    if (trx->is_recovered &&
	(trx_state_eq(trx, TRX_STATE_PREPARED) ||
	 trx_state_eq(trx, TRX_STATE_PREPARED_RECOVERED)) &&
        arg->xid->eq(&trx->xid))
    {
#ifdef WITH_WSREP
      /* The commit of a prepared recovered Galera
      transaction needs a valid trx->xid for
      invoking trx_sys_update_wsrep_checkpoint(). */
      if (!wsrep_is_wsrep_xid(&trx->xid))
#endif /* WITH_WSREP */
      /* Invalidate the XID, so that subsequent calls will not find it. */
      trx->xid.null();
      arg->trx= trx;
      found= 1;
    }
    trx->mutex_unlock();
  }
  element->mutex.wr_unlock();
  return found;
}

/** Look up an X/Open distributed transaction in XA PREPARE state.
@param[in]	xid	X/Open XA transaction identifier
@return	transaction on match (the trx_t::xid will be invalidated);
note that the trx may have been committed before the caller acquires
trx_t::mutex
@retval	NULL if no match */
trx_t* trx_get_trx_by_xid(const XID* xid)
{
  trx_get_trx_by_xid_callback_arg arg= { xid, 0 };

  if (xid)
    trx_sys.rw_trx_hash.iterate(trx_get_trx_by_xid_callback, &arg);
  return arg.trx;
}


/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_xa_low(
/*============================*/
	trx_t*	trx,		/*!< in/out: transaction */
	bool	read_write)	/*!< in: true if read write transaction */
{
	switch (trx->state) {
	case TRX_STATE_ABORTED:
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, read_write);
		return;

	case TRX_STATE_ACTIVE:
		if (trx->id == 0 && read_write) {
			/* If the transaction is tagged as read-only then
			it can only write to temp tables and for such
			transactions we don't want to move them to the
			trx_sys_t::rw_trx_hash. */
			if (!trx->read_only) {
				trx_set_rw_mode(trx);
			}
		}
		return;
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/*************************************************************//**
Starts the transaction if it is not yet started. */
void
trx_start_if_not_started_low(
/*==========================*/
	trx_t*	trx,		/*!< in: transaction */
	bool	read_write)	/*!< in: true if read write transaction */
{
	switch (trx->state) {
	case TRX_STATE_NOT_STARTED:
		trx_start_low(trx, read_write);
		return;

	case TRX_STATE_ACTIVE:
		if (read_write && trx->id == 0 && !trx->read_only) {
			trx_set_rw_mode(trx);
		}
		return;

	case TRX_STATE_ABORTED:
	case TRX_STATE_PREPARED:
	case TRX_STATE_PREPARED_RECOVERED:
	case TRX_STATE_COMMITTED_IN_MEMORY:
		break;
	}

	ut_error;
}

/**
Start a transaction for internal processing.
@param trx          transaction
@param read_write   whether writes may be performed */
void trx_start_internal_low(trx_t *trx, bool read_write)
{
  trx->will_lock= true;
  trx_start_low(trx, read_write);
}

/** Start a transaction for a DDL operation.
@param trx   transaction */
void trx_start_for_ddl_low(trx_t *trx)
{
  /* Flag this transaction as a dictionary operation, so that
  the data dictionary will be locked in crash recovery. */
  trx->dict_operation= true;
  trx_start_internal_low(trx, true);
}

/*************************************************************//**
Set the transaction as a read-write transaction if it is not already
tagged as such. Read-only transactions that are writing to temporary
tables are assigned an ID and a rollback segment but are not added
to the trx read-write list because their updates should not be visible
to other transactions and therefore their changes can be ignored by
by MVCC. */
void
trx_set_rw_mode(
/*============*/
	trx_t*		trx)		/*!< in/out: transaction that is RW */
{
	ut_ad(trx->rsegs.m_redo.rseg == 0);
	ut_ad(!trx->is_autocommit_non_locking());
	ut_ad(!trx->read_only);
	ut_ad(trx->id == 0);

	if (high_level_read_only) {
		return;
	}

	trx_assign_rseg_low(trx);

	/* So that we can see our own changes. */
	if (trx->read_view.is_open()) {
		trx->read_view.set_creator_trx_id(trx->id);
	}
}
