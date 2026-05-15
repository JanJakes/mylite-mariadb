/* Copyright (c) 2026, MyLite contributors.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mylite_schema_hook.h"

#include "handler.h"
#include "sql_class.h"

#include <m_ctype.h>
#include <string.h>

static const Mylite_schema_hooks *mylite_schema_hooks;

static CHARSET_INFO *mylite_schema_charset_from_options(
  THD *thd, const Mylite_schema_options *options);

void mylite_register_schema_hooks(const Mylite_schema_hooks *hooks)
{
  mylite_schema_hooks= hooks;
}

bool mylite_schema_hooks_active()
{
  return mylite_schema_hooks && mylite_schema_hooks->active &&
         mylite_schema_hooks->active();
}

bool mylite_schema_exists(const char *schema_name)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->schema_exists)
    return false;
  return mylite_schema_hooks->schema_exists(schema_name) ==
         MYLITE_SCHEMA_HOOK_OK;
}

int mylite_schema_store_options(const char *schema_name,
                                const Schema_specification_st *create_info)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->store_schema ||
      !create_info || !create_info->default_table_charset)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  const char *comment= NULL;
  size_t comment_size= 0;
  if (create_info->schema_comment && create_info->schema_comment->str)
  {
    comment= create_info->schema_comment->str;
    comment_size= create_info->schema_comment->length;
  }

  return mylite_schema_hooks->store_schema(
    schema_name,
    create_info->default_table_charset->cs_name.str,
    create_info->default_table_charset->coll_name.str,
    comment,
    comment_size);
}

int mylite_schema_drop(const char *schema_name)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->drop_schema)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;
  return mylite_schema_hooks->drop_schema(schema_name);
}

int mylite_schema_load_options(THD *thd, const char *schema_name,
                               Schema_specification_st *create_info)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->read_schema ||
      !mylite_schema_hooks->free_schema)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;

  Mylite_schema_options options= {NULL, NULL, NULL};
  int result= mylite_schema_hooks->read_schema(schema_name, &options);
  if (result != MYLITE_SCHEMA_HOOK_OK)
    return result;

  bzero((char*) create_info, sizeof(*create_info));
  create_info->default_table_charset=
    mylite_schema_charset_from_options(thd, &options);
  if (!create_info->default_table_charset)
    create_info->default_table_charset= thd->variables.collation_server;

  if (options.schema_comment && options.schema_comment[0])
  {
    create_info->schema_comment=
      thd->make_clex_string(options.schema_comment,
                            strlen(options.schema_comment));
    if (!create_info->schema_comment)
      result= MYLITE_SCHEMA_HOOK_ERROR;
  }

  mylite_schema_hooks->free_schema(&options);
  return result;
}

int mylite_schema_list(mylite_schema_name_callback callback, void *ctx)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->list_schemas)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;
  return mylite_schema_hooks->list_schemas(callback, ctx);
}

int mylite_schema_list_tables(const char *schema_name,
                              mylite_schema_name_callback callback,
                              void *ctx)
{
  if (!mylite_schema_hooks_active() || !mylite_schema_hooks->list_tables)
    return MYLITE_SCHEMA_HOOK_NOTFOUND;
  return mylite_schema_hooks->list_tables(schema_name, callback, ctx);
}

static CHARSET_INFO *mylite_schema_charset_from_options(
  THD *thd, const Mylite_schema_options *options)
{
  myf utf8_flag= thd->get_utf8_flag();
  if (options->default_collation_name && options->default_collation_name[0])
  {
    CHARSET_INFO *charset=
      get_charset_by_name(options->default_collation_name, MYF(utf8_flag));
    if (charset)
      return charset;
  }

  if (options->default_character_set_name &&
      options->default_character_set_name[0])
  {
    return get_charset_by_csname(options->default_character_set_name,
                                 MY_CS_PRIMARY, MYF(utf8_flag));
  }

  return NULL;
}
