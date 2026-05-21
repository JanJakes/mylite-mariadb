/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_class.h"

#include "item.h"
#include "xa.h"

static bool mylite_xa_runtime_not_supported();

enum xa_states XID_STATE::get_state_code() const
{
  return XA_NO_STATE;
}

bool THD::fix_xid_hash_pins()
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
  mylite_xa_runtime_not_supported();
}

bool XID_STATE::check_has_uncommitted_xa() const
{
  return false;
}

XID *XID_STATE::get_xid() const
{
  DBUG_ASSERT(0);
  return NULL;
}

void xid_cache_init()
{
}

void xid_cache_free()
{
}

bool xid_cache_insert(XID *)
{
  return true;
}

bool xid_cache_insert(THD *, XID_STATE *, XID *)
{
  return mylite_xa_runtime_not_supported();
}

void xid_cache_delete(THD *, XID_STATE *xid_state)
{
  xid_state->xid_cache_element= 0;
}

bool trans_xa_start(THD *)
{
  return mylite_xa_runtime_not_supported();
}

bool trans_xa_end(THD *)
{
  return mylite_xa_runtime_not_supported();
}

bool trans_xa_prepare(THD *)
{
  return mylite_xa_runtime_not_supported();
}

bool trans_xa_commit(THD *)
{
  return mylite_xa_runtime_not_supported();
}

bool trans_xa_rollback(THD *)
{
  return mylite_xa_runtime_not_supported();
}

bool trans_xa_detach(THD *thd)
{
  thd->transaction->xid_state.xid_cache_element= 0;
  return mylite_xa_runtime_not_supported();
}

void xa_recover_get_fields(THD *thd, List<Item> *field_list,
                           my_hash_walk_action *action)
{
  MEM_ROOT *mem_root= thd->mem_root;

  if (action)
    *action= NULL;

  field_list->push_back(new (mem_root)
      Item_int(thd, "formatID", 0, MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list->push_back(new (mem_root)
      Item_int(thd, "gtrid_length", 0, MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list->push_back(new (mem_root)
      Item_int(thd, "bqual_length", 0, MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list->push_back(new (mem_root)
      Item_empty_string(thd, "data", XIDDATASIZE, &my_charset_bin), mem_root);
}

bool mysql_xa_recover(THD *)
{
  return mylite_xa_runtime_not_supported();
}

static bool mylite_xa_runtime_not_supported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0),
           "external XA transactions in the MyLite embedded profile");
  return true;
}
