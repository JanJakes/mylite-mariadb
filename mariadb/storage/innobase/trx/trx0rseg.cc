/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
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
@file trx/trx0rseg.cc
Rollback segment

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rseg.h"
#include "trx0undo.h"
#include "fut0lst.h"
#include "srv0srv.h"
#include "trx0purge.h"
#include "srv0mon.h"
#include "log.h"
#include "mylite_embedded_startup_perf.h"
#include "mylite_ownerless_innodb_lock_hooks.h"

#ifdef WITH_WSREP
# include <mysql/service_wsrep.h>

/** The offset to WSREP XID headers, after TRX_RSEG */
# define TRX_RSEG_WSREP_XID_INFO      TRX_RSEG_MAX_TRX_ID + 16 + 512

/** WSREP XID format (1 if present and valid, 0 if not present) */
# define TRX_RSEG_WSREP_XID_FORMAT    TRX_RSEG_WSREP_XID_INFO
/** WSREP XID GTRID length */
# define TRX_RSEG_WSREP_XID_GTRID_LEN TRX_RSEG_WSREP_XID_INFO + 4
/** WSREP XID bqual length */
# define TRX_RSEG_WSREP_XID_BQUAL_LEN TRX_RSEG_WSREP_XID_INFO + 8
/** WSREP XID data (XIDDATASIZE bytes) */
# define TRX_RSEG_WSREP_XID_DATA      TRX_RSEG_WSREP_XID_INFO + 12

# ifdef UNIV_DEBUG
/** The latest known WSREP XID sequence number */
static long long wsrep_seqno = -1;
# endif /* UNIV_DEBUG */
/** The latest known WSREP XID UUID */
static unsigned char wsrep_uuid[16];

/** Write the WSREP XID information into rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini transaction */
static void
trx_rseg_write_wsrep_checkpoint(
	buf_block_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr)
{
	DBUG_ASSERT(xid->gtrid_length >= 0);
	DBUG_ASSERT(xid->bqual_length >= 0);
	DBUG_ASSERT(xid->gtrid_length + xid->bqual_length < XIDDATASIZE);

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_FORMAT
				       + rseg_header->page.frame,
				       uint32_t(xid->formatID));

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_GTRID_LEN
				       + rseg_header->page.frame,
				       uint32_t(xid->gtrid_length));

	mtr->write<4,mtr_t::MAYBE_NOP>(*rseg_header,
				       TRX_RSEG + TRX_RSEG_WSREP_XID_BQUAL_LEN
				       + rseg_header->page.frame,
				       uint32_t(xid->bqual_length));

	const ulint xid_length = static_cast<ulint>(xid->gtrid_length
						    + xid->bqual_length);
	mtr->memcpy<mtr_t::MAYBE_NOP>(*rseg_header,
				      TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
				      + rseg_header->page.frame,
				      xid->data, xid_length);
	if (xid_length < XIDDATASIZE
	    && memcmp(TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
		      + rseg_header->page.frame, field_ref_zero,
		      XIDDATASIZE - xid_length)) {
		mtr->memset(rseg_header,
			    TRX_RSEG + TRX_RSEG_WSREP_XID_DATA + xid_length,
			    XIDDATASIZE - xid_length, 0);
	}
}

/** Update the WSREP XID information in rollback segment header.
@param[in,out]	rseg_header	rollback segment header
@param[in]	xid		WSREP XID
@param[in,out]	mtr		mini-transaction */
void
trx_rseg_update_wsrep_checkpoint(
	buf_block_t*	rseg_header,
	const XID*	xid,
	mtr_t*		mtr)
{
	ut_ad(wsrep_is_wsrep_xid(xid));

#ifdef UNIV_DEBUG
	/* Check that seqno is monotonically increasing */
	long long xid_seqno = wsrep_xid_seqno(xid);
	const byte* xid_uuid = wsrep_xid_uuid(xid);

	if (xid_seqno != -1
	    && !memcmp(xid_uuid, wsrep_uuid, sizeof wsrep_uuid)) {
		ut_ad(xid_seqno > wsrep_seqno);
	} else {
		memcpy(wsrep_uuid, xid_uuid, sizeof wsrep_uuid);
	}
	wsrep_seqno = xid_seqno;
#endif /* UNIV_DEBUG */
	trx_rseg_write_wsrep_checkpoint(rseg_header, xid, mtr);
}

static dberr_t trx_rseg_update_wsrep_checkpoint(const XID* xid, mtr_t* mtr)
{
  dberr_t err;
  buf_block_t *rseg_header = trx_sys.rseg_array[0].get(mtr, &err);

  if (UNIV_UNLIKELY(!rseg_header))
    return err;

  /* We must make check against wsrep_uuid here, the
  trx_rseg_update_wsrep_checkpoint() writes over wsrep_uuid with xid
  contents in debug mode and the memcmp() will never give nonzero
  result. */
  const bool must_clear_rsegs=
    memcmp(wsrep_uuid, wsrep_xid_uuid(xid), sizeof wsrep_uuid);

  if (UNIV_UNLIKELY(mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT +
                                     rseg_header->page.frame)))
    trx_rseg_format_upgrade(rseg_header, mtr);

  trx_rseg_update_wsrep_checkpoint(rseg_header, xid, mtr);

  if (must_clear_rsegs)
    /* Because the UUID part of the WSREP XID differed from
    current_xid_uuid, the WSREP group UUID was changed, and we must
    reset the XID in all rollback segment headers. */
    for (ulint rseg_id= 1; rseg_id < TRX_SYS_N_RSEGS; ++rseg_id)
      if (buf_block_t* block= trx_sys.rseg_array[rseg_id].get(mtr, &err))
        mtr->memset(block, TRX_RSEG + TRX_RSEG_WSREP_XID_INFO,
                    TRX_RSEG_WSREP_XID_DATA + XIDDATASIZE -
                    TRX_RSEG_WSREP_XID_INFO, 0);
  return err;
}

/** Update WSREP checkpoint XID in first rollback segment header
as part of wsrep_set_SE_checkpoint() when it is guaranteed that there
are no wsrep transactions committing.
If the UUID part of the WSREP XID does not match to the UUIDs of XIDs already
stored into rollback segments, the WSREP XID in all the remaining rollback
segments will be reset.
@param[in]	xid		WSREP XID */
void trx_rseg_update_wsrep_checkpoint(const XID* xid)
{
	mtr_t mtr{nullptr};
	mtr.start();
	trx_rseg_update_wsrep_checkpoint(xid, &mtr);
	mtr.commit();
}

/** Read the WSREP XID information in rollback segment header.
@param[in]	rseg_header	Rollback segment header
@param[out]	xid		Transaction XID
@return	whether the WSREP XID was present */
static
bool trx_rseg_read_wsrep_checkpoint(const buf_block_t *rseg_header, XID &xid)
{
	int formatID = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_FORMAT
				 + rseg_header->page.frame));
	if (formatID == 0) {
		return false;
	}

	xid.formatID = formatID;
	xid.gtrid_length = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_GTRID_LEN
				 + rseg_header->page.frame));

	xid.bqual_length = static_cast<int>(
		mach_read_from_4(TRX_RSEG + TRX_RSEG_WSREP_XID_BQUAL_LEN
				 + rseg_header->page.frame));

	memcpy(xid.data, TRX_RSEG + TRX_RSEG_WSREP_XID_DATA
	       + rseg_header->page.frame, XIDDATASIZE);

	return wsrep_is_wsrep_xid(&xid);
}

/** Read the WSREP XID from the TRX_SYS page (in case of upgrade).
@param[in]	page	TRX_SYS page
@param[out]	xid	WSREP XID (if present)
@return	whether the WSREP XID is present */
static bool trx_rseg_init_wsrep_xid(const page_t* page, XID& xid)
{
	if (memcmp(TRX_SYS + TRX_SYS_WSREP_XID_INFO + page,
		           field_ref_zero, TRX_SYS_WSREP_XID_LEN) == 0) {
		return false;
	}

	if (mach_read_from_4(TRX_SYS + TRX_SYS_WSREP_XID_INFO
			     + TRX_SYS_WSREP_XID_MAGIC_N_FLD
			     + page)
	    != TRX_SYS_WSREP_XID_MAGIC_N) {
		return false;
	}

	xid.formatID = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_FORMAT + page));
	xid.gtrid_length = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_GTRID_LEN + page));
	xid.bqual_length = static_cast<int>(
		mach_read_from_4(
			TRX_SYS + TRX_SYS_WSREP_XID_INFO
			+ TRX_SYS_WSREP_XID_BQUAL_LEN + page));
	memcpy(xid.data,
	       TRX_SYS + TRX_SYS_WSREP_XID_INFO
	       + TRX_SYS_WSREP_XID_DATA + page, XIDDATASIZE);

	return wsrep_is_wsrep_xid(&xid);
}

/** Recover the latest WSREP checkpoint XID.
@param[out]	xid	WSREP XID
@return	whether the WSREP XID was found */
bool trx_rseg_read_wsrep_checkpoint(XID& xid)
{
	mtr_t		mtr{nullptr};
	long long       max_xid_seqno = -1;
	bool		found = false;

	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS;
	     rseg_id++, mtr.commit()) {
		mtr.start();
		const buf_block_t* sys = trx_sysf_get(&mtr, false);
		if (UNIV_UNLIKELY(!sys)) {
			continue;
		}
		const uint32_t page_no = trx_sysf_rseg_get_page_no(
			sys, rseg_id);

		if (page_no == FIL_NULL) {
			continue;
		}

		const buf_block_t* rseg_header = buf_page_get_gen(
			page_id_t(trx_sysf_rseg_get_space(sys, rseg_id),
				  page_no),
			0, RW_S_LATCH, nullptr, BUF_GET, &mtr);

		if (!rseg_header) {
			continue;
		}

		if (mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT
				     + rseg_header->page.frame)) {
			continue;
		}

		XID tmp_xid;
		long long tmp_seqno = 0;
		if (trx_rseg_read_wsrep_checkpoint(rseg_header, tmp_xid)
		    && (tmp_seqno = wsrep_xid_seqno(&tmp_xid))
		    > max_xid_seqno) {
			found = true;
			max_xid_seqno = tmp_seqno;
			xid = tmp_xid;
			memcpy(wsrep_uuid, wsrep_xid_uuid(&tmp_xid),
			       sizeof wsrep_uuid);
		}
	}

	return found;
}
#endif /* WITH_WSREP */

buf_block_t *trx_rseg_t::get(mtr_t *mtr, dberr_t *err) const
{
  if (!space)
  {
    if (err) *err= DB_TABLESPACE_NOT_FOUND;
    return nullptr;
  }

  buf_block_t *block= buf_page_get_gen(page_id(), 0, RW_X_LATCH, nullptr,
                                       BUF_GET, mtr, err);
  if (UNIV_LIKELY(block != nullptr))
    buf_page_make_young_if_needed(&block->page);

  return block;
}

/** Upgrade a rollback segment header page to MariaDB 10.3 format.
@param[in,out]	rseg_header	rollback segment header page
@param[in,out]	mtr		mini-transaction */
void trx_rseg_format_upgrade(buf_block_t *rseg_header, mtr_t *mtr)
{
  mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_FORMAT, 4, 0);
  /* Clear also possible garbage at the end of the page. Old
  InnoDB versions did not initialize unused parts of pages. */
  mtr->memset(rseg_header, TRX_RSEG + TRX_RSEG_MAX_TRX_ID + 8,
              srv_page_size
              - (FIL_PAGE_DATA_END + TRX_RSEG + TRX_RSEG_MAX_TRX_ID + 8),
              0);
}

/** Create a rollback segment header.
@param[in,out]  space           system, undo, or temporary tablespace
@param[in]      rseg_id         rollback segment identifier
@param[in]      max_trx_id      new value of TRX_RSEG_MAX_TRX_ID
@param[in,out]  mtr             mini-transaction
@param[out]     err             error code
@return the created rollback segment
@retval nullptr on failure */
buf_block_t *trx_rseg_header_create(fil_space_t *space, ulint rseg_id,
                                    trx_id_t max_trx_id, mtr_t *mtr,
                                    dberr_t *err)
{
  ut_ad(mtr->memo_contains(*space));
  buf_block_t *block=
    fseg_create(space, TRX_RSEG + TRX_RSEG_FSEG_HEADER, mtr, err);
  if (block)
  {
    ut_ad(0 == mach_read_from_4(TRX_RSEG_FORMAT + TRX_RSEG +
                                block->page.frame));
    ut_ad(0 == mach_read_from_4(TRX_RSEG_HISTORY_SIZE + TRX_RSEG +
                                block->page.frame));
    ut_ad(0 == mach_read_from_4(TRX_RSEG_MAX_TRX_ID + TRX_RSEG +
                                block->page.frame));

    /* Initialize the history list */
    flst_init(block, TRX_RSEG_HISTORY + TRX_RSEG, mtr);

    mtr->write<8,mtr_t::MAYBE_NOP>(*block, TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                   block->page.frame, max_trx_id);

    /* Reset the undo log slots */
    mtr->memset(block, TRX_RSEG_UNDO_SLOTS + TRX_RSEG, TRX_RSEG_N_SLOTS * 4,
                0xff);
  }
  return block;
}

void trx_rseg_t::destroy()
{
  latch.destroy();

#ifdef EMBEDDED_LIBRARY
  /* The embedded runtime can reopen InnoDB inside one process after a
  crash-style close.  Ownerless retained native undo is recovered from the
  persistent rollback-segment slots on the next open; any descriptors still in
  this in-memory list are stale lifetime state. */
  if (UNIV_UNLIKELY(UT_LIST_GET_LEN(undo_list)))
  {
    for (trx_undo_t *next, *undo= UT_LIST_GET_FIRST(undo_list); undo;
         undo= next)
    {
      next= UT_LIST_GET_NEXT(undo_list, undo);
      UT_LIST_REMOVE(undo_list, undo);
      ut_free(undo);
    }
  }
#endif

  /* There can't be any active transactions. */
  ut_a(!UT_LIST_GET_LEN(undo_list));

  for (trx_undo_t *next, *undo= UT_LIST_GET_FIRST(undo_cached); undo;
       undo= next)
  {
    next= UT_LIST_GET_NEXT(undo_list, undo);
    UT_LIST_REMOVE(undo_cached, undo);
    ut_free(undo);
  }
}

void trx_rseg_t::init(fil_space_t *space, uint32_t page)
{
  latch.SRW_LOCK_INIT(trx_rseg_latch_key);
  ut_ad(!this->space || this->space != space);
  this->space= space;
  page_no= page;
  last_page_no= FIL_NULL;
  curr_size= 1;

  UT_LIST_INIT(undo_list, &trx_undo_t::undo_list);
  UT_LIST_INIT(undo_cached, &trx_undo_t::undo_list);
}

void trx_rseg_t::reinit(uint32_t page)
{
  ut_ad(is_persistent());
  ut_ad(page_no == page);
  ut_a(!UT_LIST_GET_LEN(undo_list));
  ut_ad(!history_size || UT_LIST_GET_FIRST(undo_cached));

  history_size= 0;
  page_no= page;

  for (trx_undo_t *next, *undo= UT_LIST_GET_FIRST(undo_cached); undo;
       undo= next)
  {
    next= UT_LIST_GET_NEXT(undo_list, undo);
    UT_LIST_REMOVE(undo_cached, undo);
    ut_free(undo);
  }

  ut_ad(!is_referenced());
  needs_purge= 0;
  last_commit_and_offset= 0;
  last_page_no= FIL_NULL;
  curr_size= 1;
  ref.store(0, std::memory_order_release);
}

/** Read the undo log lists.
@param[in,out]  rseg            rollback segment
@param[in]      rseg_header     rollback segment header
@return error code */
static dberr_t trx_undo_lists_init(trx_rseg_t *rseg,
                                   const buf_block_t *rseg_header,
                                   mtr_t *mtr)
{
  ut_ad(srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN);
  bool is_undo_empty= true;

  for (ulint i= 0; i < TRX_RSEG_N_SLOTS; i++)
  {
    uint32_t page_no= trx_rsegf_get_nth_undo(rseg_header, i);
    if (page_no != FIL_NULL)
    {
      const trx_undo_t *undo=
        trx_undo_mem_create_at_db_start(rseg, i, page_no);
      if (!undo)
      {
        ib::error() << "Ownerless startup could not restore undo slot " << i
                    << " at page " << page_no << " for rollback segment "
                    << rseg->space->id << ':' << rseg->page_no;
        return DB_CORRUPTION;
      }
      mylite_embedded_startup_perf_count(
        MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_SLOT_COUNT);
      switch (undo->state) {
      case TRX_UNDO_ACTIVE:
        mylite_embedded_startup_perf_count(
          MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_ACTIVE_COUNT);
        break;
      case TRX_UNDO_PREPARED:
        mylite_embedded_startup_perf_count(
          MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_PREPARED_COUNT);
        break;
      case TRX_UNDO_CACHED:
        mylite_embedded_startup_perf_count(
          MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_UNDO_CACHED_COUNT);
        break;
      default:
        break;
      }
      if (is_undo_empty)
        is_undo_empty= !undo->size || undo->state == TRX_UNDO_CACHED;
      rseg->curr_size+= undo->size;
    }
  }

  trx_sys.set_undo_non_empty(!is_undo_empty);
  return DB_SUCCESS;
}

static bool trx_rseg_history_node_is_valid(const trx_rseg_t *rseg,
                                           fil_addr_t node_addr)
{
  return node_addr.page < rseg->space->free_limit &&
         node_addr.boffset >= TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_NODE &&
         node_addr.boffset < srv_page_size - TRX_UNDO_LOG_OLD_HDR_SIZE;
}

static bool trx_rseg_history_node_is_null(fil_addr_t node_addr)
{
  return node_addr.page == FIL_NULL && node_addr.boffset == 0;
}

static bool trx_rseg_history_base_is_valid(const trx_rseg_t *rseg,
                                           const byte *page)
{
  const auto len= flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + page);
  const fil_addr_t first= flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page);
  const fil_addr_t last= flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + page);

  if (len == 0)
    return trx_rseg_history_node_is_null(first) &&
           trx_rseg_history_node_is_null(last);

  return trx_rseg_history_node_is_valid(rseg, first) &&
         trx_rseg_history_node_is_valid(rseg, last);
}

static bool trx_rseg_header_base_is_valid(const trx_rseg_t *rseg,
                                          const byte *page)
{
  const page_id_t page_id{mach_read_from_4(page + FIL_PAGE_SPACE_ID),
                          mach_read_from_4(page + FIL_PAGE_OFFSET)};
  return page_id == rseg->page_id() &&
         mach_read_from_2(page + FIL_PAGE_TYPE) == FIL_PAGE_TYPE_SYS &&
         trx_rseg_history_base_is_valid(rseg, page);
}

enum class mylite_ownerless_startup_rseg_refresh_result
{
  unavailable,
  native,
  retained,
  error
};

static bool trx_rseg_history_addr_is_equal(fil_addr_t left, fil_addr_t right)
{
  return left.page == right.page && left.boffset == right.boffset;
}

struct mylite_ownerless_startup_rseg_history_state
{
  bool valid;
  uint32_t len;
  fil_addr_t first;
  fil_addr_t last;
  uint64_t page_lsn;
};

static bool trx_rseg_history_state_is_equal(
    const mylite_ownerless_startup_rseg_history_state &left,
    const mylite_ownerless_startup_rseg_history_state &right)
{
  return left.len == right.len &&
         trx_rseg_history_addr_is_equal(left.first, right.first) &&
         trx_rseg_history_addr_is_equal(left.last, right.last);
}

static mylite_ownerless_startup_rseg_refresh_result
mylite_ownerless_startup_choose_rseg_history(
    const mylite_ownerless_startup_rseg_history_state &native,
    const mylite_ownerless_startup_rseg_history_state &retained,
    bool retained_has_pair)
{
  if (native.valid && retained.page_lsn <= native.page_lsn)
    return mylite_ownerless_startup_rseg_refresh_result::native;

  const bool same_history=
      native.valid && trx_rseg_history_state_is_equal(native, retained);
  if (retained.len != 0 && !same_history && !retained_has_pair)
  {
    return mylite_ownerless_startup_rseg_refresh_result::error;
  }

  return mylite_ownerless_startup_rseg_refresh_result::retained;
}

static mylite_ownerless_startup_rseg_refresh_result
mylite_ownerless_startup_choose_rseg_header(const trx_rseg_t *rseg,
                                            const byte *native,
                                            const byte *retained,
                                            bool retained_has_pair)
{
  const mylite_ownerless_startup_rseg_history_state native_state{
      trx_rseg_header_base_is_valid(rseg, native),
      flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + native),
      flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + native),
      flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + native),
      mach_read_from_8(native + FIL_PAGE_LSN)};
  const mylite_ownerless_startup_rseg_history_state retained_state{
      true,
      flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + retained),
      flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + retained),
      flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + retained),
      mach_read_from_8(retained + FIL_PAGE_LSN)};
  return mylite_ownerless_startup_choose_rseg_history(
      native_state, retained_state, retained_has_pair);
}

extern "C" int mylite_ownerless_innodb_test_choose_startup_rseg_history(
    int native_valid, uint32_t native_len, uint32_t native_first_page,
    uint16_t native_first_offset, uint32_t native_last_page,
    uint16_t native_last_offset, uint64_t native_page_lsn,
    uint32_t retained_len, uint32_t retained_first_page,
    uint16_t retained_first_offset, uint32_t retained_last_page,
    uint16_t retained_last_offset, uint64_t retained_page_lsn,
    int retained_has_pair)
{
  const mylite_ownerless_startup_rseg_history_state native{
      native_valid != 0,
      native_len,
      fil_addr_t{native_first_page, native_first_offset},
      fil_addr_t{native_last_page, native_last_offset},
      native_page_lsn};
  const mylite_ownerless_startup_rseg_history_state retained{
      true,
      retained_len,
      fil_addr_t{retained_first_page, retained_first_offset},
      fil_addr_t{retained_last_page, retained_last_offset},
      retained_page_lsn};
  return static_cast<int>(mylite_ownerless_startup_choose_rseg_history(
      native, retained, retained_has_pair != 0));
}

dberr_t mylite_ownerless_startup_refresh_undo_header_page(
    trx_rseg_t *rseg, const page_id_t &page_id, const buf_block_t *block,
    uint64_t required_commit_lsn, bool require_valid_page);

static mylite_ownerless_startup_rseg_refresh_result
mylite_ownerless_startup_refresh_rseg_header(
    trx_rseg_t *rseg, const buf_block_t *rseg_hdr, mtr_t *mtr,
    bool include_history_rseg_delta)
{
  uint64_t max_commit_lsn=
      mylite_ownerless_innodb_startup_native_support_page_visibility();
  if (max_commit_lsn == 0)
    return mylite_ownerless_startup_rseg_refresh_result::unavailable;

  const uint32_t expected_page_size=
      static_cast<uint32_t>(rseg_hdr->physical_size());
  byte *page= static_cast<byte*>(ut_malloc_nokey(UNIV_PAGE_SIZE_MAX));
  if (page == nullptr)
    return mylite_ownerless_startup_rseg_refresh_result::error;

  uint32_t page_size= 0;
  uint64_t page_lsn= 0;
  uint64_t commit_lsn= 0;
  uint32_t record_flags= 0;
  mylite_ownerless_startup_rseg_refresh_result result=
      trx_rseg_header_base_is_valid(rseg, rseg_hdr->page.frame)
        ? mylite_ownerless_startup_rseg_refresh_result::native
        : mylite_ownerless_startup_rseg_refresh_result::unavailable;
  for (;;)
  {
    const uint64_t requested_max_commit_lsn= max_commit_lsn;
    const int read_result=
        include_history_rseg_delta
            ? mylite_ownerless_innodb_read_startup_native_support_page_version_with_history_rseg_delta(
                  rseg->space->id, rseg->page_no, max_commit_lsn, page,
                  UNIV_PAGE_SIZE_MAX, &page_size, &page_lsn, &commit_lsn,
                  &record_flags)
            : mylite_ownerless_innodb_read_startup_native_support_page_version_with_metadata(
                  rseg->space->id, rseg->page_no, max_commit_lsn, page,
                  UNIV_PAGE_SIZE_MAX, &page_size, &page_lsn, &commit_lsn,
                  &record_flags);
    if (read_result == MYLITE_OWNERLESS_INNODB_LOCK_UNAVAILABLE)
    {
      if (result != mylite_ownerless_startup_rseg_refresh_result::native)
      {
        ib::error() << "Ownerless startup has no native or retained rollback "
                       "segment image for "
                    << rseg->space->id << ':' << rseg->page_no;
        result= mylite_ownerless_startup_rseg_refresh_result::error;
      }
      break;
    }
    if (read_result != MYLITE_OWNERLESS_INNODB_LOCK_OK)
    {
      ib::error() << "Ownerless startup could not read rollback segment "
                  << rseg->space->id << ':' << rseg->page_no
                  << " from retained state: " << read_result;
      result= mylite_ownerless_startup_rseg_refresh_result::error;
      break;
    }
    if (commit_lsn > requested_max_commit_lsn)
    {
      ib::error() << "Ownerless startup rollback segment " << rseg->space->id
                  << ':' << rseg->page_no << " has commit LSN " << commit_lsn
                  << " beyond visibility limit " << requested_max_commit_lsn;
      result= mylite_ownerless_startup_rseg_refresh_result::error;
      break;
    }

    if (page_size != expected_page_size)
    {
      ib::error() << "Ownerless startup rollback segment " << rseg->space->id
                  << ':' << rseg->page_no << " has retained page size "
                  << page_size << ", expected " << expected_page_size;
      result= mylite_ownerless_startup_rseg_refresh_result::error;
      break;
    }

    const page_id_t ownerless_id{mach_read_from_4(page + FIL_PAGE_SPACE_ID),
                                 mach_read_from_4(page + FIL_PAGE_OFFSET)};
    const bool native_support_record=
        (record_flags &
         MYLITE_OWNERLESS_INNODB_PAGE_VERSION_NATIVE_SUPPORT_STATE) != 0;
    const bool retained_has_pair=
        (record_flags &
         MYLITE_OWNERLESS_INNODB_PAGE_VERSION_HISTORY_RSEG_DELTA) != 0;
    const bool header_valid= trx_rseg_header_base_is_valid(rseg, page);
    if (ownerless_id == rseg->page_id() && page_lsn != 0 && commit_lsn != 0 &&
        header_valid && native_support_record)
    {
      result= mylite_ownerless_startup_choose_rseg_header(
          rseg, rseg_hdr->page.frame, page, retained_has_pair);
      if (result == mylite_ownerless_startup_rseg_refresh_result::error)
      {
        const byte *native= rseg_hdr->page.frame;
        ib::error() << "Ownerless startup rollback segment "
                    << rseg->space->id << ':' << rseg->page_no
                    << " rejected retained history at commit LSN " << commit_lsn
                    << ": native valid="
                    << trx_rseg_header_base_is_valid(rseg, native)
                    << " lsn=" << mach_read_from_8(native + FIL_PAGE_LSN)
                    << " len="
                    << flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + native)
                    << " first="
                    << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + native).page
                    << ':'
                    << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + native).boffset
                    << " last="
                    << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + native).page
                    << ':'
                    << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + native).boffset
                    << "; retained lsn=" << page_lsn << " len="
                    << flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + page)
                    << " first="
                    << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page).page
                    << ':'
                    << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page).boffset
                    << " last="
                    << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + page).page
                    << ':'
                    << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + page).boffset
                    << " paired=" << retained_has_pair;
      }
      if (result == mylite_ownerless_startup_rseg_refresh_result::retained)
      {
        if (mylite_ownerless_innodb_advance_external_lsn(page_lsn) !=
            MYLITE_OWNERLESS_INNODB_LOCK_OK)
        {
          ib::error() << "Ownerless startup could not advance to rollback "
                         "segment page LSN "
                      << page_lsn << " for " << rseg->space->id << ':'
                      << rseg->page_no;
          result= mylite_ownerless_startup_rseg_refresh_result::error;
          break;
        }

        const uint32_t retained_history_len=
            flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + page);
        if (retained_has_pair && retained_history_len != 0)
        {
          const fil_addr_t retained_first=
              flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page);
          const page_id_t proof_page_id{rseg->space->id,
                                        retained_first.page};
          dberr_t err;
          const buf_block_t *proof_block=
              buf_page_get_gen(proof_page_id, 0, RW_X_LATCH, nullptr,
                               BUF_GET, mtr, &err);
          if (proof_block == nullptr)
          {
            ib::error() << "Ownerless startup could not read rollback segment "
                           "history proof page "
                        << proof_page_id.space() << ':'
                        << proof_page_id.page_no() << ": " << err;
            result= mylite_ownerless_startup_rseg_refresh_result::error;
            break;
          }
          err= mylite_ownerless_startup_refresh_undo_header_page(
              rseg, proof_page_id, proof_block, commit_lsn, true);
          mtr->release_last_page();
          if (err != DB_SUCCESS)
          {
            ib::error() << "Ownerless startup could not reconcile rollback "
                           "segment history proof page "
                        << proof_page_id.space() << ':'
                        << proof_page_id.page_no() << ": " << err;
            result= mylite_ownerless_startup_rseg_refresh_result::error;
            break;
          }
        }

        memcpy(const_cast<byte*>(rseg_hdr->page.frame), page, page_size);
        mylite_ownerless_innodb_note_external_page_observed(
            rseg->space->id, rseg->page_no, commit_lsn);
        mylite_ownerless_mark_retained_native_write_page_dirty(
            const_cast<buf_block_t*>(rseg_hdr));
      }
      break;
    }

    ib::error() << "Ownerless startup rejected retained rollback segment "
                << rseg->space->id << ':' << rseg->page_no << ": page id "
                << ownerless_id.space() << ':' << ownerless_id.page_no()
                << ", page LSN " << page_lsn << ", commit LSN " << commit_lsn
                << ", flags " << record_flags << ", header valid "
                << header_valid << ", native-support record "
                << native_support_record << ", page type "
                << mach_read_from_2(page + FIL_PAGE_TYPE) << ", free limit "
                << rseg->space->free_limit << ", history len "
                << flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY + page)
                << ", first "
                << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page).page
                << ':'
                << flst_get_first(TRX_RSEG + TRX_RSEG_HISTORY + page).boffset
                << ", last "
                << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + page).page
                << ':'
                << flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY + page).boffset;
    result= mylite_ownerless_startup_rseg_refresh_result::error;
    break;
  }

  ut_free(page);
  return result;
}

/** Restore the state of a persistent rollback segment.
@param[in,out]	rseg		persistent rollback segment
@param[in,out]	mtr		mini-transaction
@return error code */
static dberr_t trx_rseg_mem_restore(trx_rseg_t *rseg, mtr_t *mtr)
{
  if (!rseg->space)
    return DB_TABLESPACE_NOT_FOUND;

  /* Access the tablespace header page to recover rseg->space->free_limit */
  page_id_t page_id{rseg->space->id, 0};
  dberr_t err;
  if (!buf_page_get_gen(page_id, 0, RW_X_LATCH, nullptr, BUF_GET, mtr, &err))
    return err;
  mtr->release_last_page();
  page_id.set_page_no(rseg->page_no);
  const buf_block_t *rseg_hdr=
    buf_page_get_gen(rseg->page_id(), 0, RW_X_LATCH, nullptr, BUF_GET, mtr,
                     &err);
  if (!rseg_hdr)
    return err;

  const mylite_ownerless_startup_rseg_refresh_result refresh_result=
      mylite_ownerless_startup_refresh_rseg_header(
          rseg, rseg_hdr, mtr, true);
  if (refresh_result == mylite_ownerless_startup_rseg_refresh_result::error)
  {
    ib::error() << "Ownerless startup could not reconcile rollback segment "
                << rseg->space->id << ':' << rseg->page_no;
    return DB_CORRUPTION;
  }

  if (!mach_read_from_4(TRX_RSEG + TRX_RSEG_FORMAT + rseg_hdr->page.frame))
  {
    trx_id_t id= mach_read_from_8(TRX_RSEG + TRX_RSEG_MAX_TRX_ID +
                                  rseg_hdr->page.frame);

    if (id > rseg->needs_purge)
      rseg->needs_purge= id;

    const byte *binlog_name=
      TRX_RSEG + TRX_RSEG_BINLOG_NAME + rseg_hdr->page.frame;
    if (*binlog_name)
    {
      static_assert(TRX_RSEG_BINLOG_NAME_LEN ==
                    sizeof trx_sys.recovered_binlog_filename, "compatibility");

      /* Always prefer a position from rollback segment over
      a legacy position from before version 10.3.5. */
      int cmp= *trx_sys.recovered_binlog_filename &&
        !trx_sys.recovered_binlog_is_legacy_pos
        ? strncmp(reinterpret_cast<const char*>(binlog_name),
                  trx_sys.recovered_binlog_filename,
                  TRX_RSEG_BINLOG_NAME_LEN)
        : 1;

      if (cmp >= 0) {
        uint64_t binlog_offset =
          mach_read_from_8(TRX_RSEG + TRX_RSEG_BINLOG_OFFSET +
                           rseg_hdr->page.frame);
        if (cmp)
        {
          memcpy(trx_sys.recovered_binlog_filename, binlog_name,
                 TRX_RSEG_BINLOG_NAME_LEN);
          trx_sys.recovered_binlog_offset= binlog_offset;
        }
        else if (binlog_offset > trx_sys.recovered_binlog_offset)
          trx_sys.recovered_binlog_offset= binlog_offset;
        trx_sys.recovered_binlog_is_legacy_pos= false;
      }
    }
#ifdef WITH_WSREP
    XID tmp_xid;
    tmp_xid.null();
    /* Update recovered wsrep xid only if we found wsrep xid from
       rseg header page and read xid seqno is larger than currently
       recovered xid seqno. */
    if (trx_rseg_read_wsrep_checkpoint(rseg_hdr, tmp_xid) &&
        wsrep_xid_seqno(&tmp_xid) > wsrep_xid_seqno(&trx_sys.recovered_wsrep_xid))
      trx_sys.recovered_wsrep_xid.set(&tmp_xid);
#endif
  }

  if (srv_operation == SRV_OPERATION_RESTORE)
    /* mariabackup --prepare only deals with
    the redo log and the data files, not with
    transactions or the data dictionary. */
    return DB_SUCCESS;

  /* Initialize the undo log lists according to the rseg header */

  rseg->curr_size = mach_read_from_4(TRX_RSEG + TRX_RSEG_HISTORY_SIZE +
                                     rseg_hdr->page.frame) + 1;
  err= trx_undo_lists_init(rseg, rseg_hdr, mtr);
  if (err != DB_SUCCESS)
  {
    ib::error() << "Ownerless startup could not initialize undo lists for "
                << "rollback segment " << rseg->space->id << ':'
                << rseg->page_no << ": " << err;
  }
  if (err == DB_SUCCESS)
  {
    if (auto len= flst_get_len(TRX_RSEG + TRX_RSEG_HISTORY +
                               rseg_hdr->page.frame))
    {
      fil_addr_t node_addr= flst_get_last(TRX_RSEG + TRX_RSEG_HISTORY +
                                          rseg_hdr->page.frame);

      rseg->history_size+= len;

      if (!trx_rseg_history_node_is_valid(rseg, node_addr))
      {
        ib::error() << "Ownerless startup found an invalid history node "
                    << node_addr.page << ':' << node_addr.boffset
                    << " for rollback segment " << rseg->space->id << ':'
                    << rseg->page_no;
        return DB_CORRUPTION;
      }

      node_addr.boffset= static_cast<uint16_t>(node_addr.boffset -
                                               TRX_UNDO_HISTORY_NODE);
      rseg->last_page_no= node_addr.page;

      const buf_block_t* block=
        buf_page_get_gen(page_id_t(rseg->space->id, node_addr.page),
                         0, RW_X_LATCH, nullptr, BUF_GET, mtr, &err);
      if (!block)
      {
        ib::error() << "Ownerless startup could not read last history page "
                    << rseg->space->id << ':' << node_addr.page
                    << " for rollback segment " << rseg->space->id << ':'
                    << rseg->page_no << ": " << err;
        return err;
      }
      err= mylite_ownerless_startup_refresh_undo_header_page(
          rseg, page_id_t(rseg->space->id, node_addr.page), block, 0, false);
      if (err != DB_SUCCESS)
      {
        ib::error() << "Ownerless startup could not refresh last history page "
                    << rseg->space->id << ':' << node_addr.page
                    << " for rollback segment " << rseg->space->id << ':'
                    << rseg->page_no << ": " << err;
        return err;
      }

      trx_id_t id= mach_read_from_8(block->page.frame + node_addr.boffset +
                                    TRX_UNDO_TRX_ID);
      if (id > rseg->needs_purge)
        rseg->needs_purge= id;
      id= mach_read_from_8(block->page.frame + node_addr.boffset +
                           TRX_UNDO_TRX_NO);
      if (id > rseg->needs_purge)
        rseg->needs_purge= id;

      rseg->set_last_commit(node_addr.boffset, id);

      if (rseg->last_page_no != FIL_NULL)
        /* There is no need to cover this operation by the purge
        mutex because we are still bootstrapping. */
        purge_sys.enqueue(*rseg);
    }
  }

  trx_sys.set_undo_non_empty(rseg->history_size > 0);
  return err;
}

/** Read binlog metadata from the TRX_SYS page, in case we are upgrading
from MySQL or a MariaDB version older than 10.3.5. */
static void trx_rseg_init_binlog_info(const page_t* page)
{
	if (mach_read_from_4(TRX_SYS + TRX_SYS_MYSQL_LOG_INFO
			     + TRX_SYS_MYSQL_LOG_MAGIC_N_FLD
			     + page)
	    == TRX_SYS_MYSQL_LOG_MAGIC_N) {
		memcpy(trx_sys.recovered_binlog_filename,
		       TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_NAME
		       + TRX_SYS + page, TRX_SYS_MYSQL_LOG_NAME_LEN);
		trx_sys.recovered_binlog_offset = mach_read_from_8(
			TRX_SYS_MYSQL_LOG_INFO + TRX_SYS_MYSQL_LOG_OFFSET
			+ TRX_SYS + page);
		trx_sys.recovered_binlog_is_legacy_pos= true;
	}
}

/** Initialize or recover the rollback segments at startup. */
dberr_t trx_rseg_array_init()
{
	trx_id_t max_trx_id = 0;

	*trx_sys.recovered_binlog_filename = '\0';
	trx_sys.recovered_binlog_offset = 0;
	trx_sys.recovered_binlog_is_legacy_pos= false;
#ifdef WITH_WSREP
	trx_sys.recovered_wsrep_xid.null();
	XID wsrep_sys_xid;
	wsrep_sys_xid.null();
	bool wsrep_xid_in_rseg_found = false;
#endif
	mtr_t mtr{nullptr};
	dberr_t err = DB_SUCCESS;
	/* mariabackup --prepare only deals with the redo log and the data
	files, not with	transactions or the data dictionary, that's why
	trx_lists_init_at_db_start() does not invoke purge_sys.create() and
	purge queue mutex stays uninitialized, and trx_rseg_mem_restore() quits
	before initializing undo log lists. */
	if (srv_operation != SRV_OPERATION_RESTORE)
		/* Acquiring purge queue mutex here should be fine from the
		deadlock prevention point of view, because executing that
		function is a prerequisite for starting the purge subsystem or
		any transactions. */
		purge_sys.queue_lock();
	for (ulint rseg_id = 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++) {
		mtr.start();
		if (const buf_block_t* sys = trx_sysf_get(&mtr, true)) {
			if (rseg_id == 0) {
				/* In case this is an upgrade from
				before MariaDB 10.3.5, fetch the base
				information from the TRX_SYS page. */
				max_trx_id = mach_read_from_8(
					TRX_SYS + TRX_SYS_TRX_ID_STORE
					+ sys->page.frame);
				trx_rseg_init_binlog_info(sys->page.frame);
#ifdef WITH_WSREP
				if (trx_rseg_init_wsrep_xid(
					    sys->page.frame, trx_sys.recovered_wsrep_xid)) {
					wsrep_sys_xid.set(
						&trx_sys.recovered_wsrep_xid);
				}
#endif
			}

			const uint32_t	page_no = trx_sysf_rseg_get_page_no(
				sys, rseg_id);
			if (page_no != FIL_NULL) {
				uint64_t mylite_restore_start;
				trx_rseg_t& rseg = trx_sys.rseg_array[rseg_id];
				uint32_t space_id=
					trx_sysf_rseg_get_space(
						sys, rseg_id);

				fil_space_t *rseg_space =
					fil_space_get(space_id);
				if (!rseg_space) {
					mtr.commit();
					err = DB_ERROR;
					sql_print_error(
					  "InnoDB: Failed to open the undo "
					  "tablespace undo%03" PRIu32,
					  (space_id -
					   srv_undo_space_id_start + 1));
					break;
				}

				rseg.destroy();
				rseg.init(rseg_space, page_no);
				ut_ad(rseg.is_persistent());
				mylite_embedded_startup_perf_count(
					MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RSEG_COUNT);
				mylite_restore_start= mylite_embedded_startup_perf_start_ns();
				err = trx_rseg_mem_restore(&rseg, &mtr);
				mylite_embedded_startup_perf_add_elapsed(
					MYLITE_EMBEDDED_STARTUP_PERF_INNODB_RECOVERY_TRX_LISTS_RSEG_MEM_RESTORE_NS,
					mylite_restore_start);
				if (rseg.needs_purge > max_trx_id) {
					max_trx_id = rseg.needs_purge;
				}
				if (err != DB_SUCCESS) {
					ib::error()
						<< "Failed to restore rollback segment "
						<< rseg_space->id << "/" << page_no
						<< " with error " << err;
					mtr.commit();
					break;
				}
#ifdef WITH_WSREP
				if (!wsrep_sys_xid.is_null() &&
				    !wsrep_sys_xid.eq(&trx_sys.recovered_wsrep_xid)) {
					wsrep_xid_in_rseg_found = true;
					ut_ad(memcmp(wsrep_xid_uuid(&wsrep_sys_xid),
						     wsrep_xid_uuid(&trx_sys.recovered_wsrep_xid),
						     sizeof wsrep_uuid)
					      || wsrep_xid_seqno(
						      &wsrep_sys_xid)
					      <= wsrep_xid_seqno(
						      &trx_sys.recovered_wsrep_xid));
				}
#endif
			}
		}

		mtr.commit();
	}
	if (srv_operation != SRV_OPERATION_RESTORE)
		purge_sys.queue_unlock();
	if (err != DB_SUCCESS) {
		for (auto& rseg : trx_sys.rseg_array) {
			while (auto u = UT_LIST_GET_FIRST(rseg.undo_list)) {
				UT_LIST_REMOVE(rseg.undo_list, u);
				ut_free(u);
			}
		}
		return err;
	}

#ifdef WITH_WSREP
	if (srv_operation == SRV_OPERATION_NORMAL && !wsrep_sys_xid.is_null()) {
		/* Upgrade from a version prior to 10.3.5,
		where WSREP XID was stored in TRX_SYS page.
		If no rollback segment has a WSREP XID set,
		we must copy the XID found in TRX_SYS page
		to rollback segments. */
		mtr.start();

		if (!wsrep_xid_in_rseg_found) {
			trx_rseg_update_wsrep_checkpoint(&wsrep_sys_xid, &mtr);
		}

		/* Finally, clear WSREP XID in TRX_SYS page. */
		mtr.memset(trx_sysf_get(&mtr),
			   TRX_SYS + TRX_SYS_WSREP_XID_INFO,
			   TRX_SYS_WSREP_XID_LEN, 0);
		mtr.commit();
	}
#endif

	trx_sys.init_max_trx_id(max_trx_id + 1);
	return DB_SUCCESS;
}

/** Create the temporary rollback segments. */
dberr_t trx_temp_rseg_create(mtr_t *mtr)
{
  mylite_embedded_startup_perf_count(
    MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_CALLS);

  for (ulong i= 0; i < MYLITE_EMBEDDED_TEMP_RSEGS; i++)
  {
    uint64_t mylite_stage_start=
      mylite_embedded_startup_perf_start_ns();
    mtr->start();
    mtr->set_log_mode(MTR_LOG_NO_REDO);
    mtr->x_lock_space(fil_system.temp_space);
    mylite_embedded_startup_perf_add_elapsed(
      MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_SETUP_NS,
      mylite_stage_start);

    dberr_t err;
    mylite_stage_start= mylite_embedded_startup_perf_start_ns();
    buf_block_t *rblock=
      trx_rseg_header_create(fil_system.temp_space, i, 0, mtr, &err);
    mylite_embedded_startup_perf_add_elapsed(
      MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_HEADER_NS,
      mylite_stage_start);

    if (UNIV_UNLIKELY(!rblock))
    {
      mylite_stage_start= mylite_embedded_startup_perf_start_ns();
      mtr->commit();
      mylite_embedded_startup_perf_add_elapsed(
        MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_COMMIT_NS,
        mylite_stage_start);
      return err;
    }
    mylite_stage_start= mylite_embedded_startup_perf_start_ns();
    trx_sys.temp_rsegs[i].destroy();
    trx_sys.temp_rsegs[i].init(fil_system.temp_space,
                               rblock->page.id().page_no());
    mylite_embedded_startup_perf_add_elapsed(
      MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_MEMORY_NS,
      mylite_stage_start);
    mylite_embedded_startup_perf_count(
      MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_CREATED_COUNT);

    mylite_stage_start= mylite_embedded_startup_perf_start_ns();
    mtr->commit();
    mylite_embedded_startup_perf_add_elapsed(
      MYLITE_EMBEDDED_STARTUP_PERF_INNODB_TEMP_RSEG_CREATE_COMMIT_NS,
      mylite_stage_start);
  }
  return DB_SUCCESS;
}

/** Update the offset information about the end of the binlog entry
which corresponds to the transaction just being committed.
In a replication slave, this updates the master binlog position
up to which replication has proceeded.
@param[in,out]	rseg_header	rollback segment header
@param[in]	log_file_name	binlog file name
@param[in]	log_offset	binlog file offset
@param[in,out]	mtr		mini-transaction */
void trx_rseg_update_binlog_offset(buf_block_t *rseg_header,
                                   const char *log_file_name,
                                   ulonglong log_offset,
                                   mtr_t *mtr)
{
  DBUG_PRINT("trx", ("trx_mysql_binlog_offset %llu", log_offset));
  const size_t len= strlen(log_file_name) + 1;
  ut_ad(len > 1);

  if (UNIV_UNLIKELY(len > TRX_RSEG_BINLOG_NAME_LEN))
    return;

  mtr->write<8,mtr_t::MAYBE_NOP>(
    *rseg_header,
    TRX_RSEG + TRX_RSEG_BINLOG_OFFSET + rseg_header->page.frame,
    log_offset);

  byte *name= TRX_RSEG + TRX_RSEG_BINLOG_NAME + rseg_header->page.frame;

  if (memcmp(log_file_name, name, len))
    mtr->memcpy(*rseg_header, name, log_file_name, len);
}
