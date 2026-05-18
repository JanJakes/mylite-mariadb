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

#ifndef MYLITE_VOLATILE_ROWS_INCLUDED
#define MYLITE_VOLATILE_ROWS_INCLUDED

#include <my_global.h>
#include <mylite/storage.h>

struct Mylite_volatile_snapshot;

void mylite_volatile_clear_tables();

mylite_storage_result mylite_volatile_begin_snapshot(
  const char *primary_file, Mylite_volatile_snapshot **out_snapshot);
mylite_storage_result mylite_volatile_commit_snapshot(
  Mylite_volatile_snapshot *snapshot);
mylite_storage_result mylite_volatile_rollback_snapshot(
  Mylite_volatile_snapshot *snapshot);
mylite_storage_result mylite_volatile_create_table(
  const char *primary_file, const char *schema_name, const char *table_name,
  bool participates_in_snapshots);
mylite_storage_result mylite_volatile_drop_table(
  const char *primary_file, const char *schema_name, const char *table_name);
mylite_storage_result mylite_volatile_register_table_alias(
  const char *primary_file, const char *alias_schema_name,
  const char *alias_table_name, const char *target_schema_name,
  const char *target_table_name);
mylite_storage_result mylite_volatile_drop_table_alias(
  const char *primary_file, const char *alias_schema_name,
  const char *alias_table_name);
mylite_storage_result mylite_volatile_rename_table(
  const char *primary_file, const char *old_schema_name,
  const char *old_table_name, const char *new_schema_name,
  const char *new_table_name);
mylite_storage_result mylite_volatile_read_rows(
  const char *primary_file, const char *schema_name, const char *table_name,
  mylite_storage_rowset *out_rows);
mylite_storage_result mylite_volatile_read_row(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong row_id, uchar **out_payload, size_t *out_payload_size);
mylite_storage_result mylite_volatile_read_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned index_number, mylite_storage_index_entryset *out_entries);
mylite_storage_result mylite_volatile_find_index_entry(
    const char *primary_file, const char *schema_name, const char *table_name,
    unsigned index_number, const uchar *key, size_t key_size,
    ulonglong *out_row_id);
mylite_storage_result mylite_volatile_count_rows(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long *out_row_count);
mylite_storage_result mylite_volatile_append_row_with_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  const uchar *payload, size_t payload_size,
  const mylite_storage_index_entry *index_entries, size_t index_entry_count,
  unsigned long long *out_row_id);
mylite_storage_result mylite_volatile_update_row_with_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong old_row_id, const uchar *payload, size_t payload_size,
  const mylite_storage_index_entry *index_entries, size_t index_entry_count,
  unsigned long long *out_row_id);
mylite_storage_result mylite_volatile_delete_row(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong row_id);
mylite_storage_result mylite_volatile_truncate_table(
  const char *primary_file, const char *schema_name, const char *table_name);
mylite_storage_result mylite_volatile_read_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long *out_next_value);
mylite_storage_result mylite_volatile_set_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long next_value);
mylite_storage_result mylite_volatile_advance_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long next_value);

#endif
