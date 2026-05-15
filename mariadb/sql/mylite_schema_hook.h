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

#ifndef MYLITE_SCHEMA_HOOK_INCLUDED
#define MYLITE_SCHEMA_HOOK_INCLUDED

#include <stddef.h>

class THD;
struct Schema_specification_st;

enum mylite_schema_hook_result
{
  MYLITE_SCHEMA_HOOK_OK= 0,
  MYLITE_SCHEMA_HOOK_NOTFOUND= 1,
  MYLITE_SCHEMA_HOOK_ERROR= 2
};

struct Mylite_schema_options
{
  char *default_character_set_name;
  char *default_collation_name;
  char *schema_comment;
};

typedef int (*mylite_schema_name_callback)(void *ctx, const char *name);

struct Mylite_schema_hooks
{
  bool (*active)();
  int (*schema_exists)(const char *schema_name);
  int (*store_schema)(const char *schema_name,
                      const char *default_character_set_name,
                      const char *default_collation_name,
                      const char *schema_comment,
                      size_t schema_comment_size);
  int (*drop_schema)(const char *schema_name);
  int (*read_schema)(const char *schema_name, Mylite_schema_options *options);
  void (*free_schema)(Mylite_schema_options *options);
  int (*list_schemas)(mylite_schema_name_callback callback, void *ctx);
  int (*list_tables)(const char *schema_name,
                     mylite_schema_name_callback callback, void *ctx);
};

void mylite_register_schema_hooks(const Mylite_schema_hooks *hooks);
bool mylite_schema_hooks_active();
bool mylite_schema_exists(const char *schema_name);
int mylite_schema_store_options(const char *schema_name,
                                const Schema_specification_st *create_info);
int mylite_schema_drop(const char *schema_name);
int mylite_schema_load_options(THD *thd, const char *schema_name,
                               Schema_specification_st *create_info);
int mylite_schema_list(mylite_schema_name_callback callback, void *ctx);
int mylite_schema_list_tables(const char *schema_name,
                              mylite_schema_name_callback callback,
                              void *ctx);

#endif
