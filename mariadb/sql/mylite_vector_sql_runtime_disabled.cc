/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"

#include "handler.h"
#include "mysqld_error.h"
#include "sql_class.h"
#include "sql_priv.h"
#include "vector_mhnsw.h"

static int mylite_mhnsw_unsupported();

const LEX_CSTRING mhnsw_hlindex_table_def(THD *, uint)
{
  mylite_mhnsw_unsupported();
  return {nullptr, 0};
}

int mhnsw_insert(TABLE *, KEY *)
{
  return mylite_mhnsw_unsupported();
}

int mhnsw_read_first(TABLE *, KEY *, Item *, ulonglong)
{
  return mylite_mhnsw_unsupported();
}

int mhnsw_read_next(TABLE *)
{
  return mylite_mhnsw_unsupported();
}

int mhnsw_read_end(TABLE *)
{
  return mylite_mhnsw_unsupported();
}

int mhnsw_invalidate(TABLE *, const uchar *, KEY *)
{
  return mylite_mhnsw_unsupported();
}

int mhnsw_delete_all(TABLE *, KEY *, bool)
{
  return mylite_mhnsw_unsupported();
}

void mhnsw_free(TABLE_SHARE *)
{
}

Item_func_vec_distance::distance_kind mhnsw_uses_distance(const TABLE *, KEY *)
{
  return Item_func_vec_distance::EUCLIDEAN;
}

ha_create_table_option mhnsw_index_options[]=
{
  HA_IOPTION_END
};

st_plugin_int *mhnsw_plugin;

static int mylite_mhnsw_unsupported()
{
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "MHNSW vector indexes in MyLite");
  return HA_ERR_UNSUPPORTED;
}
