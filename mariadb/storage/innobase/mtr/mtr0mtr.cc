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
#include "ha_prototypes.h"
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
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "sql_class.h" // THD
#include "mylite_ownerless_innodb_lock_hooks.h"
#include "log.h"
#include "my_cpu.h"
#include "ut0new.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

#ifdef HAVE_PMEM
void (*mtr_t::commit_logger)(mtr_t *, std::pair<lsn_t,lsn_t>);
#endif

std::pair<lsn_t,lsn_t> (*mtr_t::finisher)(mtr_t *, size_t);

static thread_local unsigned ownerless_redo_log_latch_depth= 0;
static thread_local trx_t *ownerless_page_write_trx_override= nullptr;

static std::atomic<bool> ownerless_page_publish_stats_enabled{false};
static constexpr uint64_t ownerless_page_publish_sys_identity_none=
    std::numeric_limits<uint64_t>::max();
static std::atomic<uint64_t> ownerless_page_publish_candidates{0};
static std::atomic<uint64_t> ownerless_page_publish_published{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_unpublishable{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_lock_only{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_no_source{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_no_space{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_alloc{0};
static std::atomic<uint64_t> ownerless_page_publish_skipped_lsn_mismatch{0};
static std::atomic<uint64_t> ownerless_page_publish_failed{0};
static std::atomic<uint64_t> ownerless_page_publish_type_index{0};
static std::atomic<uint64_t> ownerless_page_publish_type_undo{0};
static std::atomic<uint64_t> ownerless_page_publish_type_space_metadata{0};
static std::atomic<uint64_t> ownerless_page_publish_type_trx_system{0};
static std::atomic<uint64_t> ownerless_page_publish_type_blob{0};
static std::atomic<uint64_t> ownerless_page_publish_type_other{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_elided{0};
static std::atomic<uint64_t> ownerless_page_publish_snapshot_boundary{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_unique{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_native_support{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_snapshot_boundary{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_index{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_undo{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_space_metadata{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_trx_system{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_blob{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_duplicate_type_other{0};
static std::atomic<uint64_t> ownerless_page_publish_identity_table_overflow{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_published{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_published_type_undo{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_space_metadata{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_published_type_trx_system{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_elided_type_undo{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_space_metadata{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_elided_type_trx_system{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_published_type_sys{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_published_type_trx_sys{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_elided_type_sys{0};
static std::atomic<uint64_t> ownerless_page_publish_native_support_elided_type_trx_sys{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_first_identity{
        ownerless_page_publish_sys_identity_none};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_first_identity_count{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_other_identity_count{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_ibuf_header{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_ibuf_root{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_first_rseg{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_dict_header{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_undo_space{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_other_system_space{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_type_sys_other_space{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_first_identity{
        ownerless_page_publish_sys_identity_none};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_first_identity_count{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_other_identity_count{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_ibuf_header{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_ibuf_root{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_first_rseg{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_dict_header{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_undo_space{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_other_system_space{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elided_type_sys_other_space{0};
static std::atomic<uint64_t> ownerless_page_publish_trx_system_samples{0};
static std::atomic<uint64_t> ownerless_page_publish_trx_system_first_samples{0};
static std::atomic<uint64_t> ownerless_page_publish_trx_system_diff_samples{0};
static std::atomic<uint64_t> ownerless_page_publish_trx_system_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_fil_header_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_trx_id_store_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_fseg_header_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_rseg_slot_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_mysql_log_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_doublewrite_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_trx_system_other_changed_bytes{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_history_proof_rseg{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_published_history_proof_undo{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elision_blocked_history_proof_rseg{0};
static std::atomic<uint64_t>
    ownerless_page_publish_native_support_elision_blocked_history_proof_undo{0};
static constexpr size_t ownerless_page_publish_identity_slot_count= 16384;
static constexpr size_t ownerless_page_publish_identity_probe_limit= 8;
static std::atomic<uint64_t>
    ownerless_page_publish_identity_slots[ownerless_page_publish_identity_slot_count];
static std::atomic_flag ownerless_page_publish_trx_system_stats_lock=
    ATOMIC_FLAG_INIT;
static byte ownerless_page_publish_trx_system_previous_page[UNIV_PAGE_SIZE_MAX];
static ulint ownerless_page_publish_trx_system_previous_page_size= 0;
static bool ownerless_page_publish_trx_system_previous_page_valid= false;

enum ownerless_page_write_perf_stat_index {
  OWNERLESS_PAGE_WRITE_PERF_ENTER_CALLS= 0,
  OWNERLESS_PAGE_WRITE_PERF_ENTER_TOTAL_NS,
  OWNERLESS_PAGE_WRITE_PERF_ACQUIRE_NS,
  OWNERLESS_PAGE_WRITE_PERF_REFRESH_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_REFRESH_NS,
  OWNERLESS_PAGE_WRITE_PERF_LEAVE_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_LEAVE_TOTAL_NS,
  OWNERLESS_PAGE_WRITE_PERF_RELEASE_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_TOTAL_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SCAN_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SCAN_TOTAL_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_DEFERRED_PAGES,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SPACE_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_ALLOC_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_COPY_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_CHECKSUM_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_HOOK_NS,
  OWNERLESS_PAGE_WRITE_PERF_PUBLISH_FREE_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_MADE_DIRTY_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_NO_DIRTY_CALLS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_TOTAL_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_FLUSH_LIST_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_RELEASE_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_REDO_LEAVE_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_PUBLISH_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_RELEASE_MEMO_NS,
  OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_NO_DIRTY_LOOP_NS,
  OWNERLESS_PAGE_WRITE_PERF_STAT_COUNT
};

static std::atomic<bool> ownerless_page_write_perf_stats_enabled{false};
static std::atomic<uint64_t> ownerless_page_write_perf_stats
    [OWNERLESS_PAGE_WRITE_PERF_STAT_COUNT];

static void ownerless_page_publish_count(
    std::atomic<uint64_t> &counter) noexcept
{
  if (ownerless_page_publish_stats_enabled.load(std::memory_order_relaxed))
    counter.fetch_add(1, std::memory_order_relaxed);
}

static void ownerless_page_publish_add(
    std::atomic<uint64_t> &counter, uint64_t value) noexcept
{
  if (value != 0 &&
      ownerless_page_publish_stats_enabled.load(std::memory_order_relaxed))
    counter.fetch_add(value, std::memory_order_relaxed);
}

static bool ownerless_page_publish_type_has_native_support(
    uint16_t page_type) noexcept
{
  switch (page_type) {
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

static bool ownerless_space_is_undo_tablespace(uint32_t space_id);

static uint64_t ownerless_page_publish_mix64(uint64_t value) noexcept
{
  value^= value >> 30;
  value*= 0xbf58476d1ce4e5b9ULL;
  value^= value >> 27;
  value*= 0x94d049bb133111ebULL;
  value^= value >> 31;
  return value;
}

static uint64_t ownerless_page_publish_identity_fingerprint(
    uint32_t space_id, uint32_t page_no, uint64_t visible_lsn) noexcept
{
  uint64_t value= (static_cast<uint64_t>(space_id) << 32) | page_no;
  value^= visible_lsn + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
  value= ownerless_page_publish_mix64(value);
  return value == 0 ? 1 : value;
}

static void ownerless_page_publish_count_duplicate_page_type(
    uint16_t page_type) noexcept
{
  if (fil_page_type_is_index(page_type))
    ownerless_page_publish_count(
        ownerless_page_publish_identity_duplicate_type_index);
  else
  {
    switch (page_type) {
    case FIL_PAGE_UNDO_LOG:
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate_type_undo);
      break;
    case FIL_PAGE_TYPE_ALLOCATED:
    case FIL_PAGE_INODE:
    case FIL_PAGE_IBUF_FREE_LIST:
    case FIL_PAGE_IBUF_BITMAP:
    case FIL_PAGE_TYPE_FSP_HDR:
    case FIL_PAGE_TYPE_XDES:
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate_type_space_metadata);
      break;
    case FIL_PAGE_TYPE_SYS:
    case FIL_PAGE_TYPE_TRX_SYS:
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate_type_trx_system);
      break;
    case FIL_PAGE_TYPE_BLOB:
    case FIL_PAGE_TYPE_ZBLOB:
    case FIL_PAGE_TYPE_ZBLOB2:
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate_type_blob);
      break;
    default:
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate_type_other);
      break;
    }
  }
}

static void ownerless_page_publish_count_identity(
    uint32_t space_id, uint32_t page_no, uint64_t visible_lsn,
    uint16_t page_type) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  const uint64_t fingerprint=
      ownerless_page_publish_identity_fingerprint(space_id, page_no,
                                                  visible_lsn);
  const size_t first_slot=
      static_cast<size_t>(fingerprint) &
      (ownerless_page_publish_identity_slot_count - 1);
  for (size_t attempt= 0; attempt < ownerless_page_publish_identity_probe_limit;
       ++attempt)
  {
    std::atomic<uint64_t> &slot= ownerless_page_publish_identity_slots
        [(first_slot + attempt) & (ownerless_page_publish_identity_slot_count - 1)];
    uint64_t observed= slot.load(std::memory_order_relaxed);
    if (observed == fingerprint)
    {
      ownerless_page_publish_count(
          ownerless_page_publish_identity_duplicate);
      ownerless_page_publish_count(
          ownerless_page_publish_type_has_native_support(page_type) ?
              ownerless_page_publish_identity_duplicate_native_support :
              ownerless_page_publish_identity_duplicate_snapshot_boundary);
      ownerless_page_publish_count_duplicate_page_type(page_type);
      return;
    }
    if (observed == 0 &&
        slot.compare_exchange_strong(observed, fingerprint,
                                     std::memory_order_relaxed,
                                     std::memory_order_relaxed))
    {
      ownerless_page_publish_count(ownerless_page_publish_identity_unique);
      return;
    }
  }

  ownerless_page_publish_count(
      ownerless_page_publish_identity_table_overflow);
}

static void ownerless_page_publish_count_page_type(
    uint16_t page_type) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  if (fil_page_type_is_index(page_type))
    ownerless_page_publish_count(ownerless_page_publish_type_index);
  else
  {
    switch (page_type) {
    case FIL_PAGE_UNDO_LOG:
      ownerless_page_publish_count(ownerless_page_publish_type_undo);
      break;
    case FIL_PAGE_TYPE_ALLOCATED:
    case FIL_PAGE_INODE:
    case FIL_PAGE_IBUF_FREE_LIST:
    case FIL_PAGE_IBUF_BITMAP:
    case FIL_PAGE_TYPE_FSP_HDR:
    case FIL_PAGE_TYPE_XDES:
      ownerless_page_publish_count(
          ownerless_page_publish_type_space_metadata);
      break;
    case FIL_PAGE_TYPE_SYS:
    case FIL_PAGE_TYPE_TRX_SYS:
      ownerless_page_publish_count(ownerless_page_publish_type_trx_system);
      break;
    case FIL_PAGE_TYPE_BLOB:
    case FIL_PAGE_TYPE_ZBLOB:
    case FIL_PAGE_TYPE_ZBLOB2:
      ownerless_page_publish_count(ownerless_page_publish_type_blob);
      break;
    default:
      ownerless_page_publish_count(ownerless_page_publish_type_other);
      break;
    }
  }

  ownerless_page_publish_count(
      ownerless_page_publish_type_has_native_support(page_type) ?
          ownerless_page_publish_native_support :
          ownerless_page_publish_snapshot_boundary);
}

static void ownerless_page_publish_count_native_support_page_type(
    uint16_t page_type,
    std::atomic<uint64_t> &undo_counter,
    std::atomic<uint64_t> &space_metadata_counter,
    std::atomic<uint64_t> &trx_system_counter) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  switch (page_type) {
  case FIL_PAGE_UNDO_LOG:
    ownerless_page_publish_count(undo_counter);
    break;
  case FIL_PAGE_TYPE_ALLOCATED:
  case FIL_PAGE_INODE:
  case FIL_PAGE_IBUF_FREE_LIST:
  case FIL_PAGE_IBUF_BITMAP:
  case FIL_PAGE_TYPE_FSP_HDR:
  case FIL_PAGE_TYPE_XDES:
    ownerless_page_publish_count(space_metadata_counter);
    break;
  case FIL_PAGE_TYPE_SYS:
  case FIL_PAGE_TYPE_TRX_SYS:
    ownerless_page_publish_count(trx_system_counter);
    break;
  default:
    break;
  }
}

static void ownerless_page_publish_count_native_support_published_page_type(
    uint16_t page_type) noexcept
{
  ownerless_page_publish_count_native_support_page_type(
      page_type,
      ownerless_page_publish_native_support_published_type_undo,
      ownerless_page_publish_native_support_published_type_space_metadata,
      ownerless_page_publish_native_support_published_type_trx_system);
}

static void ownerless_page_publish_count_native_support_elided_page_type(
    uint16_t page_type) noexcept
{
  ownerless_page_publish_count_native_support_page_type(
      page_type,
      ownerless_page_publish_native_support_elided_type_undo,
      ownerless_page_publish_native_support_elided_type_space_metadata,
      ownerless_page_publish_native_support_elided_type_trx_system);
}

static void ownerless_page_publish_count_native_support_system_page_type(
    uint16_t page_type, std::atomic<uint64_t> &sys_counter,
    std::atomic<uint64_t> &trx_sys_counter) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  switch (page_type) {
  case FIL_PAGE_TYPE_SYS:
    ownerless_page_publish_count(sys_counter);
    break;
  case FIL_PAGE_TYPE_TRX_SYS:
    ownerless_page_publish_count(trx_sys_counter);
    break;
  default:
    break;
  }
}

static void ownerless_page_publish_count_native_support_published_system_page_type(
    uint16_t page_type) noexcept
{
  ownerless_page_publish_count_native_support_system_page_type(
      page_type,
      ownerless_page_publish_native_support_published_type_sys,
      ownerless_page_publish_native_support_published_type_trx_sys);
}

static void ownerless_page_publish_count_native_support_elided_system_page_type(
    uint16_t page_type) noexcept
{
  ownerless_page_publish_count_native_support_system_page_type(
      page_type,
      ownerless_page_publish_native_support_elided_type_sys,
      ownerless_page_publish_native_support_elided_type_trx_sys);
}

static uint64_t ownerless_page_publish_pack_sys_identity(
    uint32_t space_id, uint32_t page_no) noexcept
{
  return (static_cast<uint64_t>(space_id) << 32) | page_no;
}

static void ownerless_page_publish_count_sys_identity_match(
    uint64_t identity, std::atomic<uint64_t> &first_identity,
    std::atomic<uint64_t> &first_identity_count,
    std::atomic<uint64_t> &other_identity_count) noexcept
{
  uint64_t observed= first_identity.load(std::memory_order_relaxed);
  if (observed == ownerless_page_publish_sys_identity_none)
  {
    if (first_identity.compare_exchange_strong(
            observed, identity, std::memory_order_relaxed,
            std::memory_order_relaxed))
      observed= identity;
  }

  ownerless_page_publish_count(
      observed == identity ? first_identity_count : other_identity_count);
}

static void ownerless_page_publish_count_sys_identity_class(
    uint32_t space_id, uint32_t page_no,
    std::atomic<uint64_t> &ibuf_header_counter,
    std::atomic<uint64_t> &ibuf_root_counter,
    std::atomic<uint64_t> &first_rseg_counter,
    std::atomic<uint64_t> &dict_header_counter,
    std::atomic<uint64_t> &undo_space_counter,
    std::atomic<uint64_t> &other_system_space_counter,
    std::atomic<uint64_t> &other_space_counter) noexcept
{
  if (space_id == TRX_SYS_SPACE)
  {
    switch (page_no) {
    case FSP_IBUF_HEADER_PAGE_NO:
      ownerless_page_publish_count(ibuf_header_counter);
      return;
    case FSP_IBUF_TREE_ROOT_PAGE_NO:
      ownerless_page_publish_count(ibuf_root_counter);
      return;
    case FSP_FIRST_RSEG_PAGE_NO:
      ownerless_page_publish_count(first_rseg_counter);
      return;
    case FSP_DICT_HDR_PAGE_NO:
      ownerless_page_publish_count(dict_header_counter);
      return;
    default:
      ownerless_page_publish_count(other_system_space_counter);
      return;
    }
  }

  ownerless_page_publish_count(
      ownerless_space_is_undo_tablespace(space_id) ?
          undo_space_counter :
          other_space_counter);
}

static void ownerless_page_publish_count_sys_identity(
    uint32_t space_id, uint32_t page_no,
    std::atomic<uint64_t> &first_identity,
    std::atomic<uint64_t> &first_identity_count,
    std::atomic<uint64_t> &other_identity_count,
    std::atomic<uint64_t> &ibuf_header_counter,
    std::atomic<uint64_t> &ibuf_root_counter,
    std::atomic<uint64_t> &first_rseg_counter,
    std::atomic<uint64_t> &dict_header_counter,
    std::atomic<uint64_t> &undo_space_counter,
    std::atomic<uint64_t> &other_system_space_counter,
    std::atomic<uint64_t> &other_space_counter) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  const uint64_t identity=
      ownerless_page_publish_pack_sys_identity(space_id, page_no);
  ownerless_page_publish_count_sys_identity_match(
      identity, first_identity, first_identity_count, other_identity_count);
  ownerless_page_publish_count_sys_identity_class(
      space_id, page_no, ibuf_header_counter, ibuf_root_counter,
      first_rseg_counter, dict_header_counter, undo_space_counter,
      other_system_space_counter, other_space_counter);
}

static void ownerless_page_publish_count_published_sys_identity(
    uint32_t space_id, uint32_t page_no) noexcept
{
  ownerless_page_publish_count_sys_identity(
      space_id, page_no,
      ownerless_page_publish_native_support_published_type_sys_first_identity,
      ownerless_page_publish_native_support_published_type_sys_first_identity_count,
      ownerless_page_publish_native_support_published_type_sys_other_identity_count,
      ownerless_page_publish_native_support_published_type_sys_ibuf_header,
      ownerless_page_publish_native_support_published_type_sys_ibuf_root,
      ownerless_page_publish_native_support_published_type_sys_first_rseg,
      ownerless_page_publish_native_support_published_type_sys_dict_header,
      ownerless_page_publish_native_support_published_type_sys_undo_space,
      ownerless_page_publish_native_support_published_type_sys_other_system_space,
      ownerless_page_publish_native_support_published_type_sys_other_space);
}

static void ownerless_page_publish_count_elided_sys_identity(
    uint32_t space_id, uint32_t page_no) noexcept
{
  ownerless_page_publish_count_sys_identity(
      space_id, page_no,
      ownerless_page_publish_native_support_elided_type_sys_first_identity,
      ownerless_page_publish_native_support_elided_type_sys_first_identity_count,
      ownerless_page_publish_native_support_elided_type_sys_other_identity_count,
      ownerless_page_publish_native_support_elided_type_sys_ibuf_header,
      ownerless_page_publish_native_support_elided_type_sys_ibuf_root,
      ownerless_page_publish_native_support_elided_type_sys_first_rseg,
      ownerless_page_publish_native_support_elided_type_sys_dict_header,
      ownerless_page_publish_native_support_elided_type_sys_undo_space,
      ownerless_page_publish_native_support_elided_type_sys_other_system_space,
      ownerless_page_publish_native_support_elided_type_sys_other_space);
}

static constexpr unsigned ownerless_page_write_history_proof_role_rseg= 1U;
static constexpr unsigned ownerless_page_write_history_proof_role_undo= 2U;

static unsigned ownerless_page_write_history_proof_roles(
    const trx_t *trx, uint32_t space_id, uint32_t page_no) noexcept
{
  if (trx == nullptr || !trx->mylite_ownerless_history_proof_active ||
      trx->mylite_ownerless_history_proof_space_id != space_id)
    return 0;

  unsigned roles= 0;
  if (trx->mylite_ownerless_history_proof_rseg_page_no == page_no)
    roles|= ownerless_page_write_history_proof_role_rseg;
  if (trx->mylite_ownerless_history_proof_undo_page_no == page_no)
    roles|= ownerless_page_write_history_proof_role_undo;
  return roles;
}

static void ownerless_page_publish_count_history_proof_roles(
    unsigned roles, std::atomic<uint64_t> &rseg_counter,
    std::atomic<uint64_t> &undo_counter) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)))
    return;

  if (roles & ownerless_page_write_history_proof_role_rseg)
    rseg_counter.fetch_add(1, std::memory_order_relaxed);
  if (roles & ownerless_page_write_history_proof_role_undo)
    undo_counter.fetch_add(1, std::memory_order_relaxed);
}

static void ownerless_page_publish_trx_system_lock_stats() noexcept
{
  while (ownerless_page_publish_trx_system_stats_lock.test_and_set(
             std::memory_order_acquire))
    MY_RELAX_CPU();
}

static void ownerless_page_publish_trx_system_unlock_stats() noexcept
{
  ownerless_page_publish_trx_system_stats_lock.clear(std::memory_order_release);
}

struct ownerless_page_publish_trx_system_diff_counts
{
  uint64_t changed_bytes= 0;
  uint64_t fil_header_changed_bytes= 0;
  uint64_t trx_id_store_changed_bytes= 0;
  uint64_t fseg_header_changed_bytes= 0;
  uint64_t rseg_slot_changed_bytes= 0;
  uint64_t mysql_log_changed_bytes= 0;
  uint64_t doublewrite_changed_bytes= 0;
  uint64_t other_changed_bytes= 0;
};

static void ownerless_page_publish_count_trx_system_diff_byte(
    ownerless_page_publish_trx_system_diff_counts &counts, ulint offset,
    ulint page_size) noexcept
{
  ++counts.changed_bytes;

  if (offset < TRX_SYS || offset >= page_size - FIL_PAGE_DATA_END)
  {
    ++counts.fil_header_changed_bytes;
    return;
  }

  const ulint trx_offset= offset - TRX_SYS;
  if (trx_offset >= TRX_SYS_TRX_ID_STORE &&
      trx_offset < TRX_SYS_TRX_ID_STORE + 8)
  {
    ++counts.trx_id_store_changed_bytes;
    return;
  }
  if (trx_offset >= TRX_SYS_FSEG_HEADER &&
      trx_offset < TRX_SYS_FSEG_HEADER + FSEG_HEADER_SIZE)
  {
    ++counts.fseg_header_changed_bytes;
    return;
  }
  if (trx_offset >= TRX_SYS_RSEGS &&
      trx_offset < TRX_SYS_RSEGS +
                       TRX_SYS_N_RSEGS * TRX_SYS_RSEG_SLOT_SIZE)
  {
    ++counts.rseg_slot_changed_bytes;
    return;
  }

  const ulint mysql_log_start= page_size > 1000 ? page_size - 1000 : page_size;
  const ulint doublewrite_start= page_size > 200 ? page_size - 200 : page_size;
  if (offset >= mysql_log_start && offset < doublewrite_start)
  {
    ++counts.mysql_log_changed_bytes;
    return;
  }
  if (offset >= doublewrite_start && offset < page_size - FIL_PAGE_DATA_END)
  {
    ++counts.doublewrite_changed_bytes;
    return;
  }

  ++counts.other_changed_bytes;
}

static void ownerless_page_publish_record_trx_system_diff(
    const ownerless_page_publish_trx_system_diff_counts &counts) noexcept
{
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_changed_bytes,
      counts.changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_fil_header_changed_bytes,
      counts.fil_header_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_trx_id_store_changed_bytes,
      counts.trx_id_store_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_fseg_header_changed_bytes,
      counts.fseg_header_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_rseg_slot_changed_bytes,
      counts.rseg_slot_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_mysql_log_changed_bytes,
      counts.mysql_log_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_doublewrite_changed_bytes,
      counts.doublewrite_changed_bytes);
  ownerless_page_publish_add(
      ownerless_page_publish_trx_system_other_changed_bytes,
      counts.other_changed_bytes);
}

static void ownerless_page_publish_count_trx_system_diff(
    const byte *page, ulint page_size) noexcept
{
  if (UNIV_UNLIKELY(!ownerless_page_publish_stats_enabled.load(
          std::memory_order_relaxed)) ||
      page == nullptr || page_size == 0 || page_size > UNIV_PAGE_SIZE_MAX ||
      page_size <= FIL_PAGE_DATA_END)
    return;

  ownerless_page_publish_count(ownerless_page_publish_trx_system_samples);

  ownerless_page_publish_trx_system_lock_stats();
  if (!ownerless_page_publish_trx_system_previous_page_valid ||
      ownerless_page_publish_trx_system_previous_page_size != page_size)
  {
    ::memcpy(ownerless_page_publish_trx_system_previous_page, page,
             page_size);
    ownerless_page_publish_trx_system_previous_page_size= page_size;
    ownerless_page_publish_trx_system_previous_page_valid= true;
    ownerless_page_publish_trx_system_unlock_stats();
    ownerless_page_publish_count(
        ownerless_page_publish_trx_system_first_samples);
    return;
  }

  ownerless_page_publish_trx_system_diff_counts counts;
  for (ulint offset= 0; offset < page_size; ++offset)
    if (ownerless_page_publish_trx_system_previous_page[offset] !=
        page[offset])
      ownerless_page_publish_count_trx_system_diff_byte(
          counts, offset, page_size);
  ::memcpy(ownerless_page_publish_trx_system_previous_page, page, page_size);
  ownerless_page_publish_trx_system_unlock_stats();

  ownerless_page_publish_count(ownerless_page_publish_trx_system_diff_samples);
  ownerless_page_publish_record_trx_system_diff(counts);
}

static bool ownerless_page_write_perf_enabled() noexcept
{
  return ownerless_page_write_perf_stats_enabled.load(std::memory_order_relaxed);
}

static uint64_t ownerless_page_write_perf_now_ns() noexcept
{
  const auto now= std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

static void ownerless_page_write_perf_add(
    ownerless_page_write_perf_stat_index index, uint64_t value) noexcept
{
  if (ownerless_page_write_perf_enabled())
    ownerless_page_write_perf_stats[index].fetch_add(value,
                                                     std::memory_order_relaxed);
}

static void ownerless_page_write_perf_add_elapsed(
    ownerless_page_write_perf_stat_index index, uint64_t start_ns) noexcept
{
  if (start_ns != 0)
    ownerless_page_write_perf_add(index,
                                  ownerless_page_write_perf_now_ns() -
                                      start_ns);
}

class ownerless_page_write_perf_scope
{
public:
  explicit ownerless_page_write_perf_scope(
      ownerless_page_write_perf_stat_index index) noexcept
      : m_index(index),
        m_start_ns(ownerless_page_write_perf_enabled() ?
                       ownerless_page_write_perf_now_ns() :
                       0)
  {}

  ~ownerless_page_write_perf_scope() noexcept
  {
    ownerless_page_write_perf_add_elapsed(m_index, m_start_ns);
  }

  ownerless_page_write_perf_scope(const ownerless_page_write_perf_scope&)=
      delete;
  ownerless_page_write_perf_scope& operator=(
      const ownerless_page_write_perf_scope&)= delete;

private:
  ownerless_page_write_perf_stat_index m_index;
  uint64_t m_start_ns;
};

static void ownerless_page_write_note_publish_failure(trx_t *trx) noexcept
{
  if (trx != nullptr)
    trx->mylite_ownerless_page_write_publish_failed= true;
}

static void ownerless_page_write_note_publish_success(trx_t *trx) noexcept
{
  if (trx != nullptr)
    trx->mylite_ownerless_page_write_published_page= true;
}

static void ownerless_page_write_note_history_proof_page(
    trx_t *trx, uint32_t space_id, uint32_t page_no) noexcept
{
  if (trx == nullptr || !trx->mylite_ownerless_history_proof_active ||
      trx->mylite_ownerless_history_proof_space_id != space_id)
    return;
  if (trx->mylite_ownerless_history_proof_rseg_page_no == page_no)
    trx->mylite_ownerless_history_proof_rseg_published= true;
  if (trx->mylite_ownerless_history_proof_undo_page_no == page_no)
    trx->mylite_ownerless_history_proof_undo_published= true;
}

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

static bool ownerless_page_write_sql_allows_visible_fast_path(
    const trx_t *ownerless_trx) noexcept
{
  return ownerless_trx != nullptr &&
         mylite_ownerless_innodb_statement_visible_fast_path() != 0;
}

static bool ownerless_page_write_can_elide_native_support_page(
    const trx_t *trx, uint32_t space_id, uint32_t page_no,
    uint16_t page_type) noexcept
{
  if (!ownerless_page_publish_type_has_native_support(page_type))
    return false;
  if (trx == nullptr || trx->read_only || trx->dict_operation)
    return false;
  const unsigned history_proof_roles=
      ownerless_page_write_history_proof_roles(trx, space_id, page_no);
  if (history_proof_roles != 0)
  {
    ownerless_page_publish_count_history_proof_roles(
        history_proof_roles,
        ownerless_page_publish_native_support_elision_blocked_history_proof_rseg,
        ownerless_page_publish_native_support_elision_blocked_history_proof_undo);
    return false;
  }
  if (!trx->auto_commit && !ownerless_page_write_sql_autocommit(trx))
    return false;
  if (!ownerless_page_write_sql_allows_visible_fast_path(trx))
    return false;

  const trx_rseg_t *rseg= trx->rsegs.m_redo.rseg;
  return rseg != nullptr && rseg->space != nullptr &&
         rseg->space->id == space_id;
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

static unsigned ownerless_page_write_lock_timeout_ms(
    const trx_t *ownerless_trx)
{
  if (ownerless_page_write_in_startup_or_recovery())
    return 0U;
  THD *thd= ownerless_trx != nullptr ? ownerless_trx->mysql_thd : nullptr;
  if (thd == nullptr)
    thd= current_thd;
  if (thd == nullptr)
    return 30000U;

  const ulong timeout_seconds= thd_lock_wait_timeout(thd);
  if (timeout_seconds >
      static_cast<ulong>(std::numeric_limits<unsigned>::max() / 1000U))
    return std::numeric_limits<unsigned>::max();
  return static_cast<unsigned>(timeout_seconds * 1000U);
}

static void ownerless_page_write_note_lock_timeout(trx_t *ownerless_trx)
{
  if (ownerless_trx != nullptr && ownerless_trx->error_state == DB_SUCCESS)
    ownerless_trx->error_state= DB_LOCK_WAIT_TIMEOUT;
}

static void ownerless_page_write_note_deadlock(trx_t *ownerless_trx)
{
  if (ownerless_trx != nullptr)
  {
    ownerless_trx->lock.was_chosen_as_deadlock_victim= true;
    ownerless_trx->error_state= DB_DEADLOCK;
  }
}

static bool ownerless_page_write_timeout_aborts_statement(const trx_t *trx)
{
  THD *thd= trx != nullptr ? trx->mysql_thd : nullptr;
  if (thd == nullptr)
    thd= current_thd;
  if (thd == nullptr || thd->lex == nullptr)
    return false;

  switch (thd->lex->sql_command)
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

static bool ownerless_page_write_sql_is_select(const trx_t *trx)
{
  const THD *thd= trx != nullptr ? trx->mysql_thd : nullptr;
  if (mylite_ownerless_innodb_statement_plain_read() != 0)
    return true;
  return thd != nullptr && thd->lex != nullptr &&
         thd->lex->sql_command == SQLCOM_SELECT;
}

extern "C" void mylite_ownerless_innodb_reset_thread_redo_latch_depth(void)
{
  ownerless_redo_log_latch_depth= 0;
}

extern "C" void mylite_ownerless_innodb_set_page_publish_stats_enabled(
    int enabled)
{
  ownerless_page_publish_stats_enabled.store(enabled != 0,
                                             std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_set_page_write_perf_stats_enabled(
    int enabled)
{
  ownerless_page_write_perf_stats_enabled.store(enabled != 0,
                                                std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_reset_page_publish_stats(void)
{
  ownerless_page_publish_candidates.store(0, std::memory_order_relaxed);
  ownerless_page_publish_published.store(0, std::memory_order_relaxed);
  ownerless_page_publish_skipped_unpublishable.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_skipped_lock_only.store(0,
                                                 std::memory_order_relaxed);
  ownerless_page_publish_skipped_no_source.store(0,
                                                 std::memory_order_relaxed);
  ownerless_page_publish_skipped_no_space.store(0,
                                                std::memory_order_relaxed);
  ownerless_page_publish_skipped_alloc.store(0, std::memory_order_relaxed);
  ownerless_page_publish_skipped_lsn_mismatch.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_failed.store(0, std::memory_order_relaxed);
  ownerless_page_publish_type_index.store(0, std::memory_order_relaxed);
  ownerless_page_publish_type_undo.store(0, std::memory_order_relaxed);
  ownerless_page_publish_type_space_metadata.store(0,
                                                   std::memory_order_relaxed);
  ownerless_page_publish_type_trx_system.store(0,
                                               std::memory_order_relaxed);
  ownerless_page_publish_type_blob.store(0, std::memory_order_relaxed);
  ownerless_page_publish_type_other.store(0, std::memory_order_relaxed);
  ownerless_page_publish_native_support.store(0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_snapshot_boundary.store(0,
                                                 std::memory_order_relaxed);
  ownerless_page_publish_identity_unique.store(0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate.store(0,
                                                 std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_native_support.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_snapshot_boundary.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_index.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_undo.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_space_metadata.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_trx_system.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_blob.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_duplicate_type_other.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_identity_table_overflow.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_undo.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_space_metadata.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_trx_system.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_undo.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_space_metadata.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_trx_system.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_trx_sys.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_trx_sys.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_first_identity.store(
      ownerless_page_publish_sys_identity_none, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_first_identity_count.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_other_identity_count.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_ibuf_header.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_ibuf_root.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_first_rseg.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_dict_header.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_undo_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_other_system_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_type_sys_other_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_first_identity.store(
      ownerless_page_publish_sys_identity_none, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_first_identity_count.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_other_identity_count.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_ibuf_header.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_ibuf_root.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_first_rseg.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_dict_header.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_undo_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_other_system_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elided_type_sys_other_space.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_samples.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_first_samples.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_diff_samples.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_fil_header_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_trx_id_store_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_fseg_header_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_rseg_slot_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_mysql_log_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_doublewrite_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_other_changed_bytes.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_history_proof_rseg.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_published_history_proof_undo.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elision_blocked_history_proof_rseg.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_native_support_elision_blocked_history_proof_undo.store(
      0, std::memory_order_relaxed);
  ownerless_page_publish_trx_system_lock_stats();
  ownerless_page_publish_trx_system_previous_page_size= 0;
  ownerless_page_publish_trx_system_previous_page_valid= false;
  ownerless_page_publish_trx_system_unlock_stats();
  for (size_t i= 0; i < ownerless_page_publish_identity_slot_count; ++i)
    ownerless_page_publish_identity_slots[i].store(
        0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_reset_page_write_perf_stats(void)
{
  for (size_t i= 0; i < OWNERLESS_PAGE_WRITE_PERF_STAT_COUNT; ++i)
    ownerless_page_write_perf_stats[i].store(0, std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_read_page_publish_stats(
    uint64_t *out_values, size_t value_count)
{
  if (out_values == nullptr || value_count == 0)
    return;

  const std::atomic<uint64_t> *stats[]= {
      &ownerless_page_publish_candidates,
      &ownerless_page_publish_published,
      &ownerless_page_publish_skipped_unpublishable,
      &ownerless_page_publish_skipped_lock_only,
      &ownerless_page_publish_skipped_no_source,
      &ownerless_page_publish_skipped_no_space,
      &ownerless_page_publish_skipped_alloc,
      &ownerless_page_publish_skipped_lsn_mismatch,
      &ownerless_page_publish_failed,
      &ownerless_page_publish_type_index,
      &ownerless_page_publish_type_undo,
      &ownerless_page_publish_type_space_metadata,
      &ownerless_page_publish_type_trx_system,
      &ownerless_page_publish_type_blob,
      &ownerless_page_publish_type_other,
      &ownerless_page_publish_native_support,
      &ownerless_page_publish_native_support_elided,
      &ownerless_page_publish_snapshot_boundary,
      &ownerless_page_publish_identity_unique,
      &ownerless_page_publish_identity_duplicate,
      &ownerless_page_publish_identity_duplicate_native_support,
      &ownerless_page_publish_identity_duplicate_snapshot_boundary,
      &ownerless_page_publish_identity_duplicate_type_index,
      &ownerless_page_publish_identity_duplicate_type_undo,
      &ownerless_page_publish_identity_duplicate_type_space_metadata,
      &ownerless_page_publish_identity_duplicate_type_trx_system,
      &ownerless_page_publish_identity_duplicate_type_blob,
      &ownerless_page_publish_identity_duplicate_type_other,
      &ownerless_page_publish_identity_table_overflow,
      &ownerless_page_publish_native_support_published,
      &ownerless_page_publish_native_support_published_type_undo,
      &ownerless_page_publish_native_support_published_type_space_metadata,
      &ownerless_page_publish_native_support_published_type_trx_system,
      &ownerless_page_publish_native_support_elided_type_undo,
      &ownerless_page_publish_native_support_elided_type_space_metadata,
      &ownerless_page_publish_native_support_elided_type_trx_system,
      &ownerless_page_publish_native_support_published_type_sys,
      &ownerless_page_publish_native_support_published_type_trx_sys,
      &ownerless_page_publish_native_support_elided_type_sys,
      &ownerless_page_publish_native_support_elided_type_trx_sys,
      &ownerless_page_publish_native_support_published_type_sys_first_identity,
      &ownerless_page_publish_native_support_published_type_sys_first_identity_count,
      &ownerless_page_publish_native_support_published_type_sys_other_identity_count,
      &ownerless_page_publish_native_support_published_type_sys_ibuf_header,
      &ownerless_page_publish_native_support_published_type_sys_ibuf_root,
      &ownerless_page_publish_native_support_published_type_sys_first_rseg,
      &ownerless_page_publish_native_support_published_type_sys_dict_header,
      &ownerless_page_publish_native_support_published_type_sys_undo_space,
      &ownerless_page_publish_native_support_published_type_sys_other_system_space,
      &ownerless_page_publish_native_support_published_type_sys_other_space,
      &ownerless_page_publish_native_support_elided_type_sys_first_identity,
      &ownerless_page_publish_native_support_elided_type_sys_first_identity_count,
      &ownerless_page_publish_native_support_elided_type_sys_other_identity_count,
      &ownerless_page_publish_native_support_elided_type_sys_ibuf_header,
      &ownerless_page_publish_native_support_elided_type_sys_ibuf_root,
      &ownerless_page_publish_native_support_elided_type_sys_first_rseg,
      &ownerless_page_publish_native_support_elided_type_sys_dict_header,
      &ownerless_page_publish_native_support_elided_type_sys_undo_space,
      &ownerless_page_publish_native_support_elided_type_sys_other_system_space,
      &ownerless_page_publish_native_support_elided_type_sys_other_space,
      &ownerless_page_publish_trx_system_samples,
      &ownerless_page_publish_trx_system_first_samples,
      &ownerless_page_publish_trx_system_diff_samples,
      &ownerless_page_publish_trx_system_changed_bytes,
      &ownerless_page_publish_trx_system_fil_header_changed_bytes,
      &ownerless_page_publish_trx_system_trx_id_store_changed_bytes,
      &ownerless_page_publish_trx_system_fseg_header_changed_bytes,
      &ownerless_page_publish_trx_system_rseg_slot_changed_bytes,
      &ownerless_page_publish_trx_system_mysql_log_changed_bytes,
      &ownerless_page_publish_trx_system_doublewrite_changed_bytes,
      &ownerless_page_publish_trx_system_other_changed_bytes,
      &ownerless_page_publish_native_support_published_history_proof_rseg,
      &ownerless_page_publish_native_support_published_history_proof_undo,
      &ownerless_page_publish_native_support_elision_blocked_history_proof_rseg,
      &ownerless_page_publish_native_support_elision_blocked_history_proof_undo,
  };
  const size_t stats_count= sizeof stats / sizeof stats[0];
  const size_t copy_count= std::min(value_count, stats_count);
  for (size_t i= 0; i < copy_count; ++i)
    out_values[i]= stats[i]->load(std::memory_order_relaxed);
}

extern "C" void mylite_ownerless_innodb_read_page_write_perf_stats(
    uint64_t *out_values, size_t value_count)
{
  if (out_values == nullptr || value_count == 0)
    return;

  const size_t copy_count= std::min(
      value_count,
      static_cast<size_t>(OWNERLESS_PAGE_WRITE_PERF_STAT_COUNT));
  for (size_t i= 0; i < copy_count; ++i)
    out_values[i]= ownerless_page_write_perf_stats[i].load(
        std::memory_order_relaxed);
}

extern "C" trx_t *mylite_ownerless_innodb_push_page_write_trx_override(
    trx_t *trx)
{
  trx_t *previous_trx= ownerless_page_write_trx_override;
  ownerless_page_write_trx_override= trx;
  return previous_trx;
}

extern "C" void mylite_ownerless_innodb_restore_page_write_trx_override(
    trx_t *previous_trx)
{
  ownerless_page_write_trx_override= previous_trx;
}

static uint64_t ownerless_page_write_pack(uint32_t space_id, uint32_t page_no)
{
  return (static_cast<uint64_t>(space_id) << 32) | page_no;
}

static bool ownerless_page_write_transaction_owns_page(
    const trx_t *trx, uint64_t packed_page)
{
  if (trx == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages_for_read();
  return pages != nullptr &&
         std::find(pages->begin(), pages->end(), packed_page) != pages->end();
}

static void ownerless_page_write_release_lock(
    trx_t *trx, uint32_t space_id, uint32_t page_no)
{
  const uint64_t start_ns=
      ownerless_page_write_perf_enabled() ?
          ownerless_page_write_perf_now_ns() :
          0;
  const int result= mylite_ownerless_innodb_lock_release_page_write(
    trx, space_id, page_no);
  ownerless_page_write_perf_add_elapsed(OWNERLESS_PAGE_WRITE_PERF_RELEASE_NS,
                                        start_ns);
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

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_dirty_pages_for_read();
  if (pages == nullptr)
    return false;
  return std::find_if(pages->begin(), pages->end(), [](uint64_t packed_page) {
    return !ownerless_page_write_is_transaction_gate(packed_page);
  }) != pages->end();
}

static bool ownerless_page_write_transaction_has_modified_pages_in_space(
    const trx_t *trx, uint32_t space_id)
{
  if (trx == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_dirty_pages_for_read();
  if (pages == nullptr)
    return false;
  return std::find_if(pages->begin(), pages->end(),
                      [space_id](uint64_t packed_page) {
    return !ownerless_page_write_is_transaction_gate(packed_page) &&
           static_cast<uint32_t>(packed_page >> 32) == space_id;
                      }) != pages->end();
}

static bool ownerless_page_write_transaction_has_modified_page(
    const trx_t *trx, const buf_page_t &bpage)
{
  if (trx == nullptr)
    return false;

  const trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_dirty_pages_for_read();
  if (pages == nullptr)
    return false;

  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  return std::find(pages->begin(), pages->end(), packed_page) != pages->end();
}

static void ownerless_page_write_forget_transaction_gate(trx_t *trx)
{
  if (trx == nullptr)
    return;

  trx_t::mylite_ownerless_page_vector *pages=
      trx->mylite_ownerless_modified_pages;
  if (pages == nullptr)
    return;
  pages->erase(std::remove_if(pages->begin(), pages->end(),
                              ownerless_page_write_is_transaction_gate),
               pages->end());
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
mtr_t::~mtr_t()
{
  if (m_ownerless_page_write_mtr_pages != nullptr)
    UT_DELETE(m_ownerless_page_write_mtr_pages);
}

/** Start a mini-transaction. */
void mtr_t::start()
{
  ut_ad(m_memo.empty());
  ut_ad(m_ownerless_page_write_mtr_pages == nullptr ||
        m_ownerless_page_write_mtr_pages->empty());
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
  m_ownerless_hooks= mylite_ownerless_innodb_lock_has_hooks() != 0;
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
  if (m_ownerless_page_write_mtr_pages != nullptr)
    m_ownerless_page_write_mtr_pages->clear();
  m_trim_pages= false;
}

/** Release the resources */
inline void mtr_t::release_resources()
{
  ut_ad(is_active());
  ut_ad(m_memo.empty());
  ut_ad(m_ownerless_page_write_mtr_pages == nullptr ||
        m_ownerless_page_write_mtr_pages->empty());
  m_log.erase();
  if (m_ownerless_page_write_mtr_pages != nullptr)
  {
    UT_DELETE(m_ownerless_page_write_mtr_pages);
    m_ownerless_page_write_mtr_pages= nullptr;
  }
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
      if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
	        if (UNIV_UNLIKELY(ownerless_hooks_enabled()) &&
	            ownerless_page_write_uses_transaction_release())
	          ownerless_page_write_note_dirty_transaction_page(block->page);
        insert_imported(block);
      }

      if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
  const bool ownerless_hooks= ownerless_hooks_enabled();
  for (auto it= m_memo.rbegin(); it != m_memo.rend(); it++)
  {
    if (UNIV_UNLIKELY(ownerless_hooks))
      ownerless_page_write_leave(*it);
    it->release();
    if (UNIV_UNLIKELY(ownerless_hooks))
      ownerless_space_write_leave(*it);
  }
  m_memo.clear();
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_redo_enter() noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
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
    const buf_block_t &block, bool allow_refresh) noexcept
{
  ownerless_page_write_perf_add(OWNERLESS_PAGE_WRITE_PERF_ENTER_CALLS, 1);
  ownerless_page_write_perf_scope perf_scope(
      OWNERLESS_PAGE_WRITE_PERF_ENTER_TOTAL_NS);

  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;

  if (!ownerless_page_write_requires_lock(block.page))
  {
    if (allow_refresh)
      ownerless_page_write_refresh(block);
    return;
  }

  const page_id_t id{block.page.id()};
  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_page_write_sql_is_select(ownerless_trx))
  {
    if (allow_refresh)
      ownerless_page_write_refresh(block);
    return;
  }
  const bool lock_only_page=
    ownerless_page_write_lock_only_transaction_page(ownerless_trx, block.page);
  if (lock_only_page)
  {
    if (allow_refresh)
      ownerless_page_write_refresh(block);
    return;
  }
  uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (m_ownerless_page_write_mtr_pages != nullptr &&
      std::find(m_ownerless_page_write_mtr_pages->begin(),
                m_ownerless_page_write_mtr_pages->end(),
                packed_page) != m_ownerless_page_write_mtr_pages->end())
    return;

  bool page_write_waited= false;
  if (ownerless_trx != nullptr &&
      ownerless_trx->mylite_ownerless_page_write_waited_before_preread)
  {
    ownerless_trx->mylite_ownerless_page_write_waited_before_preread=
        false;
    page_write_waited= true;
  }
  if (ownerless_page_write_uses_transaction_release() &&
      ownerless_page_write_holds_for_transaction(block.page))
  {
    for (;;)
    {
      const unsigned timeout_ms=
        ownerless_page_write_lock_timeout_ms(ownerless_trx);
      uint32_t gate_acquire_flags= 0U;
      const int result=
        mylite_ownerless_innodb_lock_acquire_transaction_page_write_gate(
            ownerless_trx, id.space(), timeout_ms, &gate_acquire_flags);
      page_write_waited= page_write_waited ||
          (gate_acquire_flags &
           MYLITE_OWNERLESS_INNODB_LOCK_ACQUIRE_WAITED) != 0U;
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
          result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
        break;
      if (result != MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
          result != MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
        ut_error;
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
      {
        ownerless_page_write_note_deadlock(ownerless_trx);
        return;
      }
      if (result == MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
          ownerless_page_write_timeout_aborts_statement(ownerless_trx))
      {
        ownerless_page_write_note_lock_timeout(ownerless_trx);
        return;
      }
      if (ownerless_page_write_in_startup_or_recovery())
        return;
      page_write_waited= true;
    }
  }

  bool page_already_modified_by_transaction= false;
  if (ownerless_page_write_uses_transaction_release() &&
      ownerless_page_write_holds_for_transaction(block.page) &&
      ownerless_trx != nullptr)
  {
    const trx_t::mylite_ownerless_page_vector *pages=
        ownerless_trx->mylite_ownerless_dirty_pages_for_read();
    page_already_modified_by_transaction=
        pages != nullptr &&
        std::find(pages->begin(), pages->end(), packed_page) != pages->end();
  }
  if (page_already_modified_by_transaction)
  {
    return;
  }
  bool page_write_acquired= false;
  for (;;)
  {
    uint32_t acquire_flags= 0U;
    const unsigned timeout_ms=
      ownerless_page_write_lock_timeout_ms(ownerless_trx);
    const uint64_t start_ns=
        ownerless_page_write_perf_enabled() ?
            ownerless_page_write_perf_now_ns() :
            0;
    const int result= mylite_ownerless_innodb_lock_acquire_page_write(
      ownerless_trx, id.space(), id.page_no(), timeout_ms, &acquire_flags);
    ownerless_page_write_perf_add_elapsed(OWNERLESS_PAGE_WRITE_PERF_ACQUIRE_NS,
                                          start_ns);
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
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
        ownerless_page_write_timeout_aborts_statement(ownerless_trx))
    {
      ownerless_page_write_note_lock_timeout(ownerless_trx);
      return;
    }
    if (ownerless_page_write_in_startup_or_recovery())
    {
      if (allow_refresh)
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
        if (allow_refresh)
          ownerless_page_write_refresh(block, true);
        page_write_waited= true;
        continue;
      }
      page_write_waited= true;
      ownerless_page_write_note_deadlock(ownerless_trx);
      return;
    }
    page_write_waited= true;
  }

  if (page_write_acquired)
  {
    ownerless_page_write_note_mtr_page(block.page);
    if (ownerless_page_write_uses_transaction_release() &&
        ownerless_page_write_holds_for_transaction(block.page))
      ownerless_page_write_note_transaction_page(block.page);
  }

  if (page_write_waited)
  {
    if (allow_refresh)
      ownerless_page_write_refresh(block, true);
    if (page_write_acquired)
      ownerless_page_write_publish_boundary(block.page);
    if (ownerless_trx != nullptr)
      ownerless_trx->mylite_ownerless_page_refreshed_after_wait= true;
    return;
  }

  if (block.page.oldest_modification_acquire() > 1)
  {
    if (ownerless_page_write_uses_transaction_release() &&
        ownerless_page_write_holds_for_transaction(block.page))
    {
      if (allow_refresh)
        ownerless_page_write_refresh(block, true);
      if (ownerless_trx != nullptr)
        ownerless_trx->mylite_ownerless_page_refreshed_after_wait= true;
    }
    else if (page_write_waited &&
             !ownerless_page_write_transaction_has_modified_page(
                 ownerless_trx, block.page))
    {
      if (allow_refresh)
        ownerless_page_write_refresh(block, true);
      if (ownerless_trx != nullptr)
        ownerless_trx->mylite_ownerless_page_refreshed_after_wait= true;
    }
    else if (page_write_acquired)
      ownerless_page_write_publish_boundary(block.page);
    return;
  }
  const bool force_transaction_page_refresh=
    allow_refresh &&
    ownerless_page_write_uses_transaction_release() &&
    ownerless_page_write_holds_for_transaction(block.page);
  if (allow_refresh)
    ownerless_page_write_refresh(block, force_transaction_page_refresh);
  if (force_transaction_page_refresh && ownerless_trx != nullptr)
    ownerless_trx->mylite_ownerless_page_refreshed_after_wait= true;
  if (page_write_acquired)
    ownerless_page_write_publish_boundary(block.page);
}

trx_t *mtr_t::ownerless_page_write_trx() const noexcept
{
  if (ownerless_page_write_trx_override != nullptr)
    return ownerless_page_write_trx_override;
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
  ownerless_page_write_perf_add(OWNERLESS_PAGE_WRITE_PERF_REFRESH_CALLS, 1);
  const uint64_t start_ns=
      ownerless_page_write_perf_enabled() ?
          ownerless_page_write_perf_now_ns() :
          0;
  const int refresh_result= force_page_version
      ? mylite_ownerless_innodb_refresh_page_for_write_force(&block)
      : mylite_ownerless_innodb_refresh_page_for_write(&block);
  ownerless_page_write_perf_add_elapsed(OWNERLESS_PAGE_WRITE_PERF_REFRESH_NS,
                                        start_ns);
  if (refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_OK &&
      refresh_result != MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
  {
    ut_error;
  }
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_leave(
    const mtr_memo_slot_t &slot) noexcept
{
  ownerless_page_write_perf_add(OWNERLESS_PAGE_WRITE_PERF_LEAVE_CALLS, 1);
  ownerless_page_write_perf_scope perf_scope(
      OWNERLESS_PAGE_WRITE_PERF_LEAVE_TOTAL_NS);

  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;

  if (!(slot.type & (MTR_MEMO_PAGE_X_FIX | MTR_MEMO_PAGE_SX_FIX)))
    return;
  const buf_page_t *bpage= static_cast<const buf_page_t*>(slot.object);
  if (ownerless_page_write_lock_only_transaction_page(
          ownerless_page_write_trx(), *bpage))
    return;
  trx_t *ownerless_trx= ownerless_page_write_trx();
  const bool uses_transaction_release=
      ownerless_page_write_uses_transaction_release();
  const bool holds_for_transaction=
      ownerless_page_write_holds_for_transaction(*bpage);
  const bool mtr_page_acquired= ownerless_page_write_forget_mtr_page(*bpage);
  const bool deferred_release=
      uses_transaction_release && holds_for_transaction &&
      ownerless_page_write_release_deferred(slot);
  if (!mtr_page_acquired)
  {
    return;
  }

  const page_id_t id{bpage->id()};
  if (deferred_release)
    return;
  ownerless_page_write_release_lock(ownerless_trx, id.space(), id.page_no());
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_space_write_enter(
    fil_space_t *space) noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()) || space == nullptr ||
      space->id >= SRV_TMP_SPACE_ID || space->is_temporary())
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_page_write_sql_is_select(ownerless_trx))
  {
    return;
  }
  for (;;)
  {
    const unsigned timeout_ms=
      ownerless_page_write_lock_timeout_ms(ownerless_trx);
    const int result= mylite_ownerless_innodb_lock_acquire_page_write(
      ownerless_trx, space->id,
      MYLITE_OWNERLESS_INNODB_SPACE_WRITE_PAGE_NO, timeout_ms, nullptr);
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK ||
        result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
      break;
    if (result != MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
        result != MYLITE_OWNERLESS_INNODB_LOCK_DEADLOCK)
      ut_error;
    if (result == MYLITE_OWNERLESS_INNODB_LOCK_TIMEOUT &&
        ownerless_page_write_timeout_aborts_statement(ownerless_trx))
    {
      ownerless_page_write_note_lock_timeout(ownerless_trx);
      return;
    }
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
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
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
  if (UNIV_LIKELY(!ownerless_hooks_enabled()) || m_commit_lsn == 0)
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;

  ownerless_page_write_perf_add(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SCAN_CALLS, 1);
  ownerless_page_write_perf_scope perf_scope(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SCAN_TOTAL_NS);

  mylite_ownerless_innodb_begin_page_publish_batch();
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
	      ownerless_page_write_note_dirty_transaction_page(*bpage);
	      ownerless_page_write_capture_dirty_transaction_page(*bpage);
	      continue;
	    }

    ownerless_page_write_publish(*bpage);
  }
  mylite_ownerless_innodb_end_page_publish_batch();
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_publish(
    const buf_page_t &bpage) noexcept
{
  ownerless_page_write_perf_add(OWNERLESS_PAGE_WRITE_PERF_PUBLISH_CALLS, 1);
  ownerless_page_write_perf_scope perf_scope(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_TOTAL_NS);

  if (UNIV_LIKELY(!ownerless_hooks_enabled()) || m_commit_lsn == 0)
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;

  ownerless_page_publish_count(ownerless_page_publish_candidates);
  trx_t *ownerless_trx= ownerless_page_write_trx();
  const page_id_t id{bpage.id()};
  if (id.space() >= SRV_TMP_SPACE_ID || !bpage.in_file())
  {
    ownerless_page_publish_count(
        ownerless_page_publish_skipped_unpublishable);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }
  if (ownerless_page_write_lock_only_transaction_page(
          ownerless_trx, bpage))
  {
    ownerless_page_publish_count(ownerless_page_publish_skipped_lock_only);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }

  const byte *source= bpage.zip.data ? bpage.zip.data : bpage.frame;
  if (source == nullptr)
  {
    ownerless_page_publish_count(ownerless_page_publish_skipped_no_source);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }

  const lsn_t source_page_lsn= mach_read_from_8(source + FIL_PAGE_LSN);
  if (source_page_lsn != m_commit_lsn)
  {
    ownerless_page_publish_count(
        ownerless_page_publish_skipped_lsn_mismatch);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }

  const uint16_t source_page_type= fil_page_get_type(source);
  if (ownerless_page_write_can_elide_native_support_page(
          ownerless_trx, id.space(), id.page_no(), source_page_type))
  {
    ownerless_page_publish_count_page_type(source_page_type);
    ownerless_page_publish_count_identity(
        id.space(), id.page_no(), m_commit_lsn, source_page_type);
    ownerless_page_publish_count(
        ownerless_page_publish_native_support_elided);
    ownerless_page_publish_count_native_support_elided_page_type(
        source_page_type);
    ownerless_page_publish_count_native_support_elided_system_page_type(
        source_page_type);
    if (source_page_type == FIL_PAGE_TYPE_SYS)
      ownerless_page_publish_count_elided_sys_identity(
          id.space(), id.page_no());
    return;
  }

  fil_space_t *space= fil_space_t::get(id.space());
  uint64_t start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  if (space == nullptr)
  {
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SPACE_NS, start_ns);
    ownerless_page_publish_count(ownerless_page_publish_skipped_no_space);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }
  const bool full_crc32= space->full_crc32();
  space->release();
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_SPACE_NS, start_ns);

  const ulint page_size= bpage.physical_size();
  start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  byte *page= static_cast<byte*>(aligned_malloc(page_size, page_size));
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_ALLOC_NS, start_ns);
  if (page == nullptr)
  {
    ownerless_page_publish_count(ownerless_page_publish_skipped_alloc);
    ownerless_page_write_note_publish_failure(ownerless_trx);
    return;
  }

  start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  ::memcpy(page, source, page_size);
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_COPY_NS, start_ns);
  start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  if (bpage.zip.data)
    buf_flush_update_zip_checksum(page, page_size);
  else
    buf_flush_init_for_writing(nullptr, page, nullptr, full_crc32);
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_CHECKSUM_NS, start_ns);

  const uint16_t page_type= fil_page_get_type(page);
  ownerless_page_publish_count_page_type(page_type);
  ownerless_page_publish_count_identity(
      id.space(), id.page_no(), m_commit_lsn, page_type);
  if (id.space() == TRX_SYS_SPACE && id.page_no() == TRX_SYS_PAGE_NO &&
      page_type == FIL_PAGE_TYPE_TRX_SYS)
    ownerless_page_publish_count_trx_system_diff(page, page_size);
  start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  const int result= mylite_ownerless_innodb_publish_page_version(
      id.space(), id.page_no(), source_page_lsn, m_commit_lsn, page,
      static_cast<uint32_t>(page_size));
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_HOOK_NS, start_ns);
  ownerless_page_publish_count(
      result == MYLITE_OWNERLESS_INNODB_LOCK_OK ?
          ownerless_page_publish_published :
          ownerless_page_publish_failed);
  if (result == MYLITE_OWNERLESS_INNODB_LOCK_OK)
  {
    if (ownerless_page_publish_type_has_native_support(page_type))
    {
      ownerless_page_publish_count(
          ownerless_page_publish_native_support_published);
      ownerless_page_publish_count_native_support_published_page_type(
          page_type);
      ownerless_page_publish_count_native_support_published_system_page_type(
          page_type);
      if (page_type == FIL_PAGE_TYPE_SYS)
        ownerless_page_publish_count_published_sys_identity(
            id.space(), id.page_no());
      ownerless_page_publish_count_history_proof_roles(
          ownerless_page_write_history_proof_roles(
              ownerless_trx, id.space(), id.page_no()),
          ownerless_page_publish_native_support_published_history_proof_rseg,
          ownerless_page_publish_native_support_published_history_proof_undo);
    }
    ownerless_page_write_note_publish_success(ownerless_trx);
    ownerless_page_write_note_history_proof_page(
        ownerless_trx, id.space(), id.page_no());
  }
  else
    ownerless_page_write_note_publish_failure(ownerless_trx);

  start_ns= ownerless_page_write_perf_enabled() ?
      ownerless_page_write_perf_now_ns() :
      0;
  aligned_free(page);
  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_PUBLISH_FREE_NS, start_ns);
}

ATTRIBUTE_NOINLINE void mtr_t::ownerless_page_write_publish_boundary(
    const buf_page_t &bpage) noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;
  if (recv_recovery_is_on() || !srv_was_started)
    return;
  if (!ownerless_page_write_publishes_with_transaction(bpage))
    return;

  /*
  Transaction-deferred data pages can carry uncommitted row versions while the
  page-write latch is held. Commit-time publish and native snapshot-boundary
  synthesis own safe page-version boundary records for these pages.
  */
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
  const trx_t::mylite_ownerless_page_vector *pages=
      ownerless_trx->mylite_ownerless_modified_pages_for_read();
  return pages != nullptr &&
         std::find(pages->begin(), pages->end(), packed_page) != pages->end();
}

void mtr_t::ownerless_page_write_note_transaction_page(
    const buf_page_t &bpage) const noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;

  if (!ownerless_page_write_publishes_with_transaction(bpage))
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx == nullptr)
    return;

  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  trx_t::mylite_ownerless_page_vector &pages=
      ownerless_trx->mylite_ownerless_modified_pages_for_write();
  if (std::find(pages.begin(), pages.end(), packed_page) == pages.end())
  {
    pages.push_back(packed_page);
    ownerless_page_write_perf_add(
        OWNERLESS_PAGE_WRITE_PERF_PUBLISH_DEFERRED_PAGES, 1);
  }
}

void mtr_t::ownerless_page_write_note_dirty_transaction_page(
    const buf_page_t &bpage) const noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;

  if (!ownerless_page_write_publishes_with_transaction(bpage))
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx == nullptr)
    return;

  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (!ownerless_page_write_transaction_owns_page(ownerless_trx, packed_page))
  {
    ownerless_page_write_note_transaction_page(bpage);
    if (!ownerless_page_write_transaction_owns_page(ownerless_trx, packed_page))
      return;
  }

  trx_t::mylite_ownerless_page_vector &pages=
      ownerless_trx->mylite_ownerless_dirty_pages_for_write();
  if (std::find(pages.begin(), pages.end(), packed_page) == pages.end())
    pages.push_back(packed_page);
}

void mtr_t::ownerless_page_write_capture_dirty_transaction_page(
    const buf_page_t &bpage) const noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return;

  if (!ownerless_page_write_publishes_with_transaction(bpage))
    return;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx == nullptr)
    return;

  const byte *source= bpage.zip.data ? bpage.zip.data : bpage.frame;
  if (source == nullptr || !bpage.in_file())
    return;

  const page_id_t id{bpage.id()};
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (!ownerless_page_write_transaction_owns_page(ownerless_trx, packed_page))
    return;

  const uint64_t page_lsn= mach_read_from_8(source + FIL_PAGE_LSN);
  if (page_lsn == 0)
    return;

  trx_t::mylite_ownerless_page_image_vector &images=
      ownerless_trx->mylite_ownerless_page_images_for_write();
  auto it= std::find_if(
      images.begin(), images.end(),
      [packed_page](const trx_t::mylite_ownerless_page_image &image) {
        return image.packed_page == packed_page;
      });
  if (it == images.end())
  {
    trx_t::mylite_ownerless_page_image image;
    image.packed_page= packed_page;
    image.page_lsn= page_lsn;
    image.page_size= static_cast<uint32_t>(bpage.physical_size());
    image.compressed= bpage.zip.data != nullptr;
    image.page.assign(source, source + image.page_size);
    images.push_back(std::move(image));
    return;
  }

  if (page_lsn < it->page_lsn)
    return;
  it->page_lsn= page_lsn;
  it->page_size= static_cast<uint32_t>(bpage.physical_size());
  it->compressed= bpage.zip.data != nullptr;
  it->page.assign(source, source + it->page_size);
}

void mtr_t::ownerless_page_write_note_mtr_page(
    const buf_page_t &bpage) noexcept
{
  const page_id_t id{bpage.id()};
  uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  if (m_ownerless_page_write_mtr_pages == nullptr)
  {
    m_ownerless_page_write_mtr_pages=
      UT_NEW_NOKEY(ownerless_page_write_mtr_page_vector());
    ut_a(m_ownerless_page_write_mtr_pages != nullptr);
  }
  if (std::find(m_ownerless_page_write_mtr_pages->begin(),
                m_ownerless_page_write_mtr_pages->end(),
                packed_page) == m_ownerless_page_write_mtr_pages->end())
    m_ownerless_page_write_mtr_pages->emplace_back(packed_page);
}

bool mtr_t::ownerless_page_write_forget_mtr_page(
    const buf_page_t &bpage) noexcept
{
  const page_id_t id{bpage.id()};
  if (m_ownerless_page_write_mtr_pages == nullptr)
    return false;
  const uint64_t packed_page=
      ownerless_page_write_pack(id.space(), id.page_no());
  auto it= std::find(m_ownerless_page_write_mtr_pages->begin(),
                     m_ownerless_page_write_mtr_pages->end(),
                     packed_page);
  if (it == m_ownerless_page_write_mtr_pages->end())
    return false;
  m_ownerless_page_write_mtr_pages->erase(it, it + 1);
  return true;
}

bool mtr_t::ownerless_page_write_uses_transaction_release() const noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return false;

  if (ownerless_page_write_in_startup_or_recovery())
    return false;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_page_write_lock_only_transaction(ownerless_trx))
    return false;
  if (ownerless_trx == nullptr || ownerless_trx->read_only ||
      ownerless_trx->dict_operation)
    return false;
  if ((ownerless_trx->auto_commit ||
       ownerless_page_write_sql_autocommit(ownerless_trx)) &&
      ownerless_page_write_sql_allows_visible_fast_path(ownerless_trx))
    return false;
  if (ownerless_trx->id != 0)
    return true;
  if (ownerless_trx->mylite_ownerless_page_write_trx_id != 0)
    return true;
  return ownerless_trx->mysql_thd != nullptr &&
         ownerless_trx->mysql_thd->lex != nullptr;
}

bool mtr_t::ownerless_page_write_should_prepare(
    const buf_page_t &bpage) const noexcept
{
  if (UNIV_LIKELY(!ownerless_hooks_enabled()))
    return false;

  if (ownerless_page_write_in_startup_or_recovery())
    return false;

  trx_t *ownerless_trx= ownerless_page_write_trx();
  if (ownerless_trx != nullptr && ownerless_trx->read_only)
    return false;
  if (ownerless_page_write_sql_is_select(ownerless_trx))
    return false;
  if (!ownerless_page_write_holds_for_transaction(bpage))
    return true;
  if (ownerless_trx != nullptr && ownerless_trx->mysql_thd != nullptr &&
      !ownerless_trx->auto_commit &&
      !ownerless_page_write_sql_autocommit(ownerless_trx))
    return true;
  return ownerless_trx == nullptr || ownerless_trx->auto_commit ||
         ownerless_page_write_sql_autocommit(ownerless_trx) ||
         !ownerless_page_write_transaction_has_modified_pages(ownerless_trx) ||
         ownerless_page_write_transaction_has_modified_pages_in_space(
             ownerless_trx, bpage.id().space());
}

ATTRIBUTE_NOINLINE void mtr_t::commit_log_release() noexcept
{
  if (m_ownerless_redo_borrowed_latch)
    return;

  if (m_latch_ex)
  {
    if (m_ownerless_redo)
    {
      if (ownerless_redo_log_latch_depth != 0)
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
  const bool ownerless_perf= mtr->m_ownerless_hooks != 0 &&
      UNIV_UNLIKELY(ownerless_page_write_perf_enabled()) &&
      mtr->ownerless_hooks_enabled();
  const uint64_t commit_start_ns= ownerless_perf ?
      ownerless_page_write_perf_now_ns() :
      0;

  if (ownerless_perf)
    ownerless_page_write_perf_add(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_CALLS, 1);

  if (mtr->m_made_dirty)
  {
    if (ownerless_perf)
      ownerless_page_write_perf_add(
          OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_MADE_DIRTY_CALLS, 1);

    auto it= mtr->m_memo.rbegin();
    uint64_t phase_start_ns= ownerless_perf ?
        ownerless_page_write_perf_now_ns() :
        0;

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
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_FLUSH_LIST_NS,
        phase_start_ns);

    phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
    mtr->commit_log_release();
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_RELEASE_NS, phase_start_ns);
    if (mtr->m_ownerless_redo)
    {
      phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
      mtr->ownerless_redo_leave();
      ownerless_page_write_perf_add_elapsed(
          OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_REDO_LEAVE_NS,
          phase_start_ns);
    }
    if (UNIV_UNLIKELY(mtr->ownerless_hooks_enabled()))
    {
      phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
      mtr->ownerless_page_writes_publish();
      ownerless_page_write_perf_add_elapsed(
          OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_PUBLISH_NS,
          phase_start_ns);
    }
    phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
    mtr->release();
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_RELEASE_MEMO_NS,
        phase_start_ns);
  }
  else
  {
    if (ownerless_perf)
      ownerless_page_write_perf_add(
          OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_NO_DIRTY_CALLS, 1);

    uint64_t phase_start_ns= ownerless_perf ?
        ownerless_page_write_perf_now_ns() :
        0;
    mtr->commit_log_release();
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_RELEASE_NS, phase_start_ns);
    if (mtr->m_ownerless_redo)
    {
      phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
      mtr->ownerless_redo_leave();
      ownerless_page_write_perf_add_elapsed(
          OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_REDO_LEAVE_NS,
          phase_start_ns);
    }

    phase_start_ns= ownerless_perf ? ownerless_page_write_perf_now_ns() : 0;
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
        if (UNIV_UNLIKELY(mtr->ownerless_hooks_enabled()))
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
          if (UNIV_UNLIKELY(mtr->ownerless_hooks_enabled()))
          {
            const uint64_t publish_start_ns= ownerless_perf ?
                ownerless_page_write_perf_now_ns() :
                0;
	            if (mtr->ownerless_page_write_uses_transaction_release() &&
	                ownerless_page_write_publishes_with_transaction(*bpage))
	            {
	              mtr->ownerless_page_write_note_dirty_transaction_page(*bpage);
	              mtr->ownerless_page_write_capture_dirty_transaction_page(*bpage);
	            }
            else
              mtr->ownerless_page_write_publish(*bpage);
            ownerless_page_write_perf_add_elapsed(
                OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_PUBLISH_NS,
                publish_start_ns);
          }
          modified++;
        }
        switch (auto latch= slot.type & ~MTR_MEMO_MODIFY) {
        case MTR_MEMO_PAGE_S_FIX:
          bpage->lock.s_unlock();
          continue;
        case MTR_MEMO_PAGE_SX_FIX:
        case MTR_MEMO_PAGE_X_FIX:
          if (UNIV_UNLIKELY(mtr->ownerless_hooks_enabled()))
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
    ownerless_page_write_perf_add_elapsed(
        OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_NO_DIRTY_LOOP_NS,
        phase_start_ns);
  }

  if (modified != 0 && mtr->trx)
    if (ha_handler_stats *stats= mtr->trx->active_handler_stats)
      stats->pages_updated+= modified;

  if (UNIV_UNLIKELY(lsns.second != 0))
  {
    ut_ad(lsns.second < mtr->m_commit_lsn);
    mtr_flush_ahead(lsns.second);
  }

  ownerless_page_write_perf_add_elapsed(
      OWNERLESS_PAGE_WRITE_PERF_COMMIT_LOG_TOTAL_NS, commit_start_ns);
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
    if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
    if (ownerless_redo_log_latch_depth != 0)
      ownerless_redo_log_latch_depth--;
  }
  log_sys.latch.wr_unlock();
  m_latch_ex= false;
  if (m_ownerless_redo)
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
  if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
    if (ownerless_redo_log_latch_depth != 0)
      ownerless_redo_log_latch_depth--;
  }
  log_sys.latch.wr_unlock();
  m_latch_ex= false;
  if (m_ownerless_redo)
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
  if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
    if (ownerless_redo_log_latch_depth != 0)
      ownerless_redo_log_latch_depth--;
    log_sys.latch.wr_unlock();
    m_latch_ex= false;
    if (checkpoint_lsn && m_commit_lsn)
      log_write_up_to(m_commit_lsn, true);
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
		if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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
  if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
    ownerless_page_write_leave(*it);
  it->release();
  if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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

  if (UNIV_UNLIKELY(ownerless_hooks_enabled()))
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

  if (UNIV_UNLIKELY(ownerless_hooks_enabled()) &&
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
  if (UNIV_UNLIKELY(ownerless_hooks_enabled()) &&
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

  const bool ownerless_hooks= ownerless_hooks_enabled();
  if (UNIV_UNLIKELY(ownerless_hooks))
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
      ownerless_page_write_enter(block, false);
  }

	  if (UNIV_UNLIKELY(ownerless_hooks) &&
	      ownerless_page_write_uses_transaction_release())
	    ownerless_page_write_note_dirty_transaction_page(block.page);
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
        const bool ownerless_hooks= ownerless_hooks_enabled();
        if (UNIV_UNLIKELY(ownerless_hooks))
          ownerless_page_write_enter(*b);
        slot.type= MTR_MEMO_PAGE_X_MODIFY;
        m_modifications= true;
	        if (UNIV_UNLIKELY(ownerless_hooks) &&
	            ownerless_page_write_uses_transaction_release())
	          ownerless_page_write_note_dirty_transaction_page(b->page);
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
	        if (UNIV_UNLIKELY(ownerless_hooks_enabled()) &&
	            ownerless_page_write_uses_transaction_release())
	          ownerless_page_write_note_dirty_transaction_page(block->page);
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
