/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_class.h"
#include "transaction.h"
#include "xa.h"

static bool mylite_xa_not_supported(THD *thd);

void xid_cache_init()
{
}

void xid_cache_free()
{
}

bool xid_cache_insert(XID *)
{
  return false;
}

bool xid_cache_insert(THD *thd, XID_STATE *, XID *)
{
  return mylite_xa_not_supported(thd);
}

void xid_cache_delete(THD *, XID_STATE *xid_state)
{
  xid_state->xid_cache_element= nullptr;
}

bool trans_xa_start(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

bool trans_xa_end(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

bool trans_xa_prepare(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

bool trans_xa_commit(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

bool trans_xa_rollback(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

bool trans_xa_detach(THD *thd)
{
  thd->transaction->xid_state.xid_cache_element= nullptr;
  return trans_rollback(thd);
}

bool mysql_xa_recover(THD *thd)
{
  return mylite_xa_not_supported(thd);
}

static bool mylite_xa_not_supported(THD *thd)
{
  my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "embedded");
  return true;
}

void xa_recover_get_fields(THD *thd, List<Item> *field_list,
                           my_hash_walk_action *action)
{
  (void) thd;
  (void) field_list;
  if (action)
    *action= nullptr;
}

bool XID_STATE::check_has_uncommitted_xa() const
{
  return false;
}

void XID_STATE::set_error(uint)
{
}

void XID_STATE::set_online_alter_cache(Online_alter_cache_list *)
{
}

void XID_STATE::set_rollback_only()
{
}

void XID_STATE::er_xaer_rmfail() const
{
  my_error(ER_XAER_RMFAIL, MYF(0), "NON-EXISTING");
}

XID *XID_STATE::get_xid() const
{
  DBUG_ASSERT(false);
  return nullptr;
}

enum xa_states XID_STATE::get_state_code() const
{
  return XA_NO_STATE;
}
