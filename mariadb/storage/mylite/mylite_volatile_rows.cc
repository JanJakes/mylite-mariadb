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

#define MYSQL_SERVER 1

#include "mylite_volatile_rows.h"

#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <string.h>
#include <stdlib.h>

struct Mylite_volatile_index_entry
{
  uint index_number;
  std::vector<uchar> key;
};

struct Mylite_volatile_row
{
  ulonglong row_id;
  std::vector<uchar> payload;
  std::vector<Mylite_volatile_index_entry> index_entries;
  bool deleted;
};

struct Mylite_volatile_table
{
  std::string primary_file;
  std::string schema_name;
  std::string table_name;
  ulonglong next_row_id;
  ulonglong auto_increment_value;
  bool participates_in_snapshots;
  std::vector<Mylite_volatile_row> rows;
};

struct Mylite_volatile_table_alias
{
  std::string primary_file;
  std::string alias_schema_name;
  std::string alias_table_name;
  std::string target_schema_name;
  std::string target_table_name;
};

struct Mylite_volatile_snapshot
{
  std::string primary_file;
  std::vector<Mylite_volatile_table> tables;
  std::vector<Mylite_volatile_table_alias> aliases;
};

static bool mylite_volatile_table_args_valid(const char *primary_file,
                                             const char *schema_name,
                                             const char *table_name);
static bool mylite_volatile_primary_file_valid(const char *primary_file);
static bool mylite_same_volatile_table(const Mylite_volatile_table &table,
                                       const char *primary_file,
                                       const char *schema_name,
                                       const char *table_name);
static bool mylite_same_volatile_table_identity(
  const Mylite_volatile_table &left, const Mylite_volatile_table &right);
static bool mylite_volatile_alias_participates_in_snapshots_locked(
  const Mylite_volatile_table_alias &alias);
static Mylite_volatile_table *mylite_find_volatile_table_locked(
  const char *primary_file, const char *schema_name, const char *table_name);
static Mylite_volatile_table *mylite_find_or_create_volatile_table_locked(
  const char *primary_file, const char *schema_name, const char *table_name);
static bool mylite_erase_volatile_table_locked(const char *primary_file,
                                               const char *schema_name,
                                               const char *table_name);
static void mylite_preserve_volatile_table_counters_locked(
  Mylite_volatile_table *restored_table);

static std::mutex mylite_volatile_mutex;
static std::vector<Mylite_volatile_table> mylite_volatile_tables;
static std::vector<Mylite_volatile_table_alias> mylite_volatile_table_aliases;

void mylite_volatile_clear_tables()
{
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  mylite_volatile_tables.clear();
  mylite_volatile_table_aliases.clear();
}

mylite_storage_result mylite_volatile_begin_snapshot(
  const char *primary_file, Mylite_volatile_snapshot **out_snapshot)
{
  if (!mylite_volatile_primary_file_valid(primary_file) || !out_snapshot)
    return MYLITE_STORAGE_MISUSE;

  *out_snapshot= NULL;

  Mylite_volatile_snapshot *snapshot= NULL;
  try
  {
    snapshot= new Mylite_volatile_snapshot;
    snapshot->primary_file= primary_file;

    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    for (std::vector<Mylite_volatile_table>::const_iterator it=
           mylite_volatile_tables.begin();
         it != mylite_volatile_tables.end(); ++it)
    {
      if (it->primary_file == primary_file && it->participates_in_snapshots)
        snapshot->tables.push_back(*it);
    }
    for (std::vector<Mylite_volatile_table_alias>::const_iterator it=
           mylite_volatile_table_aliases.begin();
         it != mylite_volatile_table_aliases.end(); ++it)
    {
      if (it->primary_file == primary_file &&
          mylite_volatile_alias_participates_in_snapshots_locked(*it))
        snapshot->aliases.push_back(*it);
    }

    *out_snapshot= snapshot;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    delete snapshot;
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_commit_snapshot(
  Mylite_volatile_snapshot *snapshot)
{
  if (!snapshot)
    return MYLITE_STORAGE_MISUSE;

  delete snapshot;
  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_rollback_snapshot(
  Mylite_volatile_snapshot *snapshot)
{
  if (!snapshot)
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::vector<Mylite_volatile_table> restored_tables;
    std::vector<Mylite_volatile_table_alias> restored_aliases;

    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    restored_tables.reserve(mylite_volatile_tables.size() +
                            snapshot->tables.size());
    for (std::vector<Mylite_volatile_table>::const_iterator it=
           mylite_volatile_tables.begin();
         it != mylite_volatile_tables.end(); ++it)
    {
      if (it->primary_file != snapshot->primary_file ||
          !it->participates_in_snapshots)
        restored_tables.push_back(*it);
    }
    for (std::vector<Mylite_volatile_table>::const_iterator it=
           snapshot->tables.begin();
         it != snapshot->tables.end(); ++it)
    {
      restored_tables.push_back(*it);
      mylite_preserve_volatile_table_counters_locked(&restored_tables.back());
    }

    restored_aliases.reserve(mylite_volatile_table_aliases.size() +
                             snapshot->aliases.size());
    for (std::vector<Mylite_volatile_table_alias>::const_iterator it=
           mylite_volatile_table_aliases.begin();
         it != mylite_volatile_table_aliases.end(); ++it)
    {
      if (it->primary_file != snapshot->primary_file ||
          !mylite_volatile_alias_participates_in_snapshots_locked(*it))
        restored_aliases.push_back(*it);
    }
    restored_aliases.insert(restored_aliases.end(), snapshot->aliases.begin(),
                            snapshot->aliases.end());

    mylite_volatile_tables.swap(restored_tables);
    mylite_volatile_table_aliases.swap(restored_aliases);
    delete snapshot;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    delete snapshot;
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_create_table(
  const char *primary_file, const char *schema_name, const char *table_name,
  bool participates_in_snapshots)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name))
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    Mylite_volatile_table *table=
      mylite_find_or_create_volatile_table_locked(primary_file, schema_name,
                                                  table_name);
    table->rows.clear();
    table->next_row_id= 1ULL;
    table->auto_increment_value= 1ULL;
    table->participates_in_snapshots= participates_in_snapshots;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_drop_table(
  const char *primary_file, const char *schema_name, const char *table_name)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name))
    return MYLITE_STORAGE_MISUSE;

  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  mylite_erase_volatile_table_locked(primary_file, schema_name, table_name);

  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_register_table_alias(
  const char *primary_file, const char *alias_schema_name,
  const char *alias_table_name, const char *target_schema_name,
  const char *target_table_name)
{
  if (!mylite_volatile_table_args_valid(primary_file, alias_schema_name,
                                        alias_table_name) ||
      !mylite_volatile_table_args_valid(primary_file, target_schema_name,
                                        target_table_name))
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    for (std::vector<Mylite_volatile_table_alias>::iterator it=
           mylite_volatile_table_aliases.begin();
         it != mylite_volatile_table_aliases.end(); ++it)
    {
      if (it->primary_file == primary_file &&
          it->alias_schema_name == alias_schema_name &&
          it->alias_table_name == alias_table_name)
      {
        it->target_schema_name= target_schema_name;
        it->target_table_name= target_table_name;
        return MYLITE_STORAGE_OK;
      }
    }

    Mylite_volatile_table_alias alias;
    alias.primary_file= primary_file;
    alias.alias_schema_name= alias_schema_name;
    alias.alias_table_name= alias_table_name;
    alias.target_schema_name= target_schema_name;
    alias.target_table_name= target_table_name;
    mylite_volatile_table_aliases.push_back(alias);
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_drop_table_alias(
  const char *primary_file, const char *alias_schema_name,
  const char *alias_table_name)
{
  if (!mylite_volatile_table_args_valid(primary_file, alias_schema_name,
                                        alias_table_name))
    return MYLITE_STORAGE_MISUSE;

  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  for (std::vector<Mylite_volatile_table_alias>::iterator it=
         mylite_volatile_table_aliases.begin();
       it != mylite_volatile_table_aliases.end(); ++it)
  {
    if (it->primary_file != primary_file ||
        it->alias_schema_name != alias_schema_name ||
        it->alias_table_name != alias_table_name)
      continue;

    mylite_erase_volatile_table_locked(primary_file,
                                       it->target_schema_name.c_str(),
                                       it->target_table_name.c_str());
    mylite_volatile_table_aliases.erase(it);
    return MYLITE_STORAGE_OK;
  }

  return MYLITE_STORAGE_NOTFOUND;
}

mylite_storage_result mylite_volatile_rename_table(
  const char *primary_file, const char *old_schema_name,
  const char *old_table_name, const char *new_schema_name,
  const char *new_table_name)
{
  if (!mylite_volatile_table_args_valid(primary_file, old_schema_name,
                                        old_table_name) ||
      !new_schema_name || !new_schema_name[0] ||
      !new_table_name || !new_table_name[0])
    return MYLITE_STORAGE_MISUSE;
  if (strcmp(old_schema_name, new_schema_name) == 0 &&
      strcmp(old_table_name, new_table_name) == 0)
    return MYLITE_STORAGE_OK;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    if (!mylite_find_volatile_table_locked(primary_file, old_schema_name,
                                           old_table_name))
      return MYLITE_STORAGE_OK;

    for (std::vector<Mylite_volatile_table>::iterator it=
           mylite_volatile_tables.begin();
         it != mylite_volatile_tables.end(); ++it)
    {
      if (mylite_same_volatile_table(*it, primary_file, new_schema_name,
                                     new_table_name))
      {
        mylite_volatile_tables.erase(it);
        break;
      }
    }

    Mylite_volatile_table *table=
      mylite_find_volatile_table_locked(primary_file, old_schema_name,
                                        old_table_name);
    if (!table)
      return MYLITE_STORAGE_OK;

    table->schema_name= new_schema_name;
    table->table_name= new_table_name;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_read_rows(
  const char *primary_file, const char *schema_name, const char *table_name,
  mylite_storage_rowset *out_rows)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !out_rows || out_rows->size < sizeof(*out_rows))
    return MYLITE_STORAGE_MISUSE;

  *out_rows= (mylite_storage_rowset){sizeof(*out_rows)};
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_OK;

  size_t row_count= 0;
  size_t row_bytes= 0;
  for (std::vector<Mylite_volatile_row>::const_iterator it=
         table->rows.begin(); it != table->rows.end(); ++it)
  {
    if (it->deleted)
      continue;
    if (it->payload.size() > SIZE_MAX - row_bytes)
      return MYLITE_STORAGE_FULL;
    row_bytes+= it->payload.size();
    ++row_count;
  }
  if (row_count == 0)
    return MYLITE_STORAGE_OK;
  if (row_count > SIZE_MAX / sizeof(size_t) ||
      row_count > SIZE_MAX / sizeof(unsigned long long))
    return MYLITE_STORAGE_FULL;

  out_rows->rows= static_cast<uchar *>(malloc(row_bytes));
  out_rows->row_offsets= static_cast<size_t *>(malloc(row_count * sizeof(size_t)));
  out_rows->row_sizes= static_cast<size_t *>(malloc(row_count * sizeof(size_t)));
  out_rows->row_ids= static_cast<unsigned long long *>(
    malloc(row_count * sizeof(unsigned long long)));
  if (!out_rows->rows || !out_rows->row_offsets || !out_rows->row_sizes ||
      !out_rows->row_ids)
  {
    mylite_storage_free_rowset(out_rows);
    return MYLITE_STORAGE_NOMEM;
  }

  size_t row_index= 0;
  size_t row_offset= 0;
  for (std::vector<Mylite_volatile_row>::const_iterator it=
         table->rows.begin(); it != table->rows.end(); ++it)
  {
    if (it->deleted)
      continue;
    out_rows->row_offsets[row_index]= row_offset;
    out_rows->row_sizes[row_index]= it->payload.size();
    out_rows->row_ids[row_index]= it->row_id;
    memcpy(out_rows->rows + row_offset, it->payload.data(),
           it->payload.size());
    row_offset+= it->payload.size();
    ++row_index;
  }

  out_rows->row_count= row_count;
  out_rows->row_bytes= row_bytes;
  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_read_row(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong row_id, uchar **out_payload, size_t *out_payload_size)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !out_payload || !out_payload_size || row_id == 0ULL)
    return MYLITE_STORAGE_MISUSE;

  *out_payload= NULL;
  *out_payload_size= 0;
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_NOTFOUND;

  for (std::vector<Mylite_volatile_row>::const_iterator it=
         table->rows.begin(); it != table->rows.end(); ++it)
  {
    if (it->deleted || it->row_id != row_id)
      continue;
    uchar *payload= static_cast<uchar *>(malloc(it->payload.size()));
    if (!payload)
      return MYLITE_STORAGE_NOMEM;
    memcpy(payload, it->payload.data(), it->payload.size());
    *out_payload= payload;
    *out_payload_size= it->payload.size();
    return MYLITE_STORAGE_OK;
  }

  return MYLITE_STORAGE_NOTFOUND;
}

mylite_storage_result mylite_volatile_read_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned index_number, mylite_storage_index_entryset *out_entries)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !out_entries || out_entries->size < sizeof(*out_entries))
    return MYLITE_STORAGE_MISUSE;

  *out_entries= (mylite_storage_index_entryset){sizeof(*out_entries)};
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_OK;

  size_t entry_count= 0;
  size_t key_bytes= 0;
  for (std::vector<Mylite_volatile_row>::const_iterator row=
         table->rows.begin(); row != table->rows.end(); ++row)
  {
    if (row->deleted)
      continue;
    for (std::vector<Mylite_volatile_index_entry>::const_iterator entry=
           row->index_entries.begin(); entry != row->index_entries.end();
         ++entry)
    {
      if (entry->index_number != index_number)
        continue;
      if (entry->key.size() > SIZE_MAX - key_bytes)
        return MYLITE_STORAGE_FULL;
      key_bytes+= entry->key.size();
      ++entry_count;
    }
  }
  if (entry_count == 0)
    return MYLITE_STORAGE_OK;
  if (entry_count > SIZE_MAX / sizeof(size_t) ||
      entry_count > SIZE_MAX / sizeof(unsigned long long))
    return MYLITE_STORAGE_FULL;

  out_entries->keys= static_cast<uchar *>(malloc(key_bytes));
  out_entries->key_offsets= static_cast<size_t *>(
    malloc(entry_count * sizeof(size_t)));
  out_entries->key_sizes= static_cast<size_t *>(
    malloc(entry_count * sizeof(size_t)));
  out_entries->row_ids= static_cast<unsigned long long *>(
    malloc(entry_count * sizeof(unsigned long long)));
  if (!out_entries->keys || !out_entries->key_offsets ||
      !out_entries->key_sizes || !out_entries->row_ids)
  {
    mylite_storage_free_index_entryset(out_entries);
    return MYLITE_STORAGE_NOMEM;
  }

  size_t entry_index= 0;
  size_t key_offset= 0;
  for (std::vector<Mylite_volatile_row>::const_iterator row=
         table->rows.begin(); row != table->rows.end(); ++row)
  {
    if (row->deleted)
      continue;
    for (std::vector<Mylite_volatile_index_entry>::const_iterator entry=
           row->index_entries.begin(); entry != row->index_entries.end();
         ++entry)
    {
      if (entry->index_number != index_number)
        continue;
      out_entries->key_offsets[entry_index]= key_offset;
      out_entries->key_sizes[entry_index]= entry->key.size();
      out_entries->row_ids[entry_index]= row->row_id;
      memcpy(out_entries->keys + key_offset, entry->key.data(),
             entry->key.size());
      key_offset+= entry->key.size();
      ++entry_index;
    }
  }

  out_entries->key_bytes= key_bytes;
  out_entries->entry_count= entry_count;
  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_count_rows(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long *out_row_count)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !out_row_count)
    return MYLITE_STORAGE_MISUSE;

  *out_row_count= 0ULL;
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_OK;

  for (std::vector<Mylite_volatile_row>::const_iterator it=
         table->rows.begin(); it != table->rows.end(); ++it)
  {
    if (!it->deleted)
      ++*out_row_count;
  }

  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_append_row_with_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  const uchar *payload, size_t payload_size,
  const mylite_storage_index_entry *index_entries, size_t index_entry_count,
  unsigned long long *out_row_id)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !payload || payload_size == 0 || !out_row_id ||
      (index_entry_count > 0 && !index_entries))
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    Mylite_volatile_table *table=
      mylite_find_or_create_volatile_table_locked(primary_file, schema_name,
                                                  table_name);
    if (table->next_row_id == ULONGLONG_MAX)
      return MYLITE_STORAGE_FULL;

    Mylite_volatile_row row;
    row.row_id= table->next_row_id;
    row.deleted= false;
    row.payload.assign(payload, payload + payload_size);
    row.index_entries.reserve(index_entry_count);
    for (size_t i= 0; i < index_entry_count; ++i)
    {
      if (!index_entries[i].key || index_entries[i].key_size == 0)
        return MYLITE_STORAGE_MISUSE;

      Mylite_volatile_index_entry entry;
      entry.index_number= index_entries[i].index_number;
      entry.key.assign(index_entries[i].key,
                       index_entries[i].key + index_entries[i].key_size);
      row.index_entries.push_back(entry);
    }

    table->rows.push_back(row);
    ++table->next_row_id;
    *out_row_id= row.row_id;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_update_row_with_index_entries(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong old_row_id, const uchar *payload, size_t payload_size,
  const mylite_storage_index_entry *index_entries, size_t index_entry_count,
  unsigned long long *out_row_id)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      old_row_id == 0ULL || !payload || payload_size == 0 || !out_row_id ||
      (index_entry_count > 0 && !index_entries))
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    Mylite_volatile_table *table=
      mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
    if (!table)
      return MYLITE_STORAGE_NOTFOUND;

    size_t old_row_index= 0;
    bool found= false;
    for (; old_row_index < table->rows.size(); ++old_row_index)
    {
      if (!table->rows[old_row_index].deleted &&
          table->rows[old_row_index].row_id == old_row_id)
      {
        found= true;
        break;
      }
    }
    if (!found)
      return MYLITE_STORAGE_NOTFOUND;
    if (table->next_row_id == ULONGLONG_MAX)
      return MYLITE_STORAGE_FULL;

    Mylite_volatile_row row;
    row.row_id= table->next_row_id;
    row.deleted= false;
    row.payload.assign(payload, payload + payload_size);
    row.index_entries.reserve(index_entry_count);
    for (size_t i= 0; i < index_entry_count; ++i)
    {
      if (!index_entries[i].key || index_entries[i].key_size == 0)
        return MYLITE_STORAGE_MISUSE;

      Mylite_volatile_index_entry entry;
      entry.index_number= index_entries[i].index_number;
      entry.key.assign(index_entries[i].key,
                       index_entries[i].key + index_entries[i].key_size);
      row.index_entries.push_back(entry);
    }

    table->rows.push_back(row);
    table->rows[old_row_index].deleted= true;
    ++table->next_row_id;
    *out_row_id= row.row_id;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_delete_row(
  const char *primary_file, const char *schema_name, const char *table_name,
  ulonglong row_id)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      row_id == 0ULL)
    return MYLITE_STORAGE_MISUSE;

  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_NOTFOUND;

  for (std::vector<Mylite_volatile_row>::iterator it= table->rows.begin();
       it != table->rows.end(); ++it)
  {
    if (!it->deleted && it->row_id == row_id)
    {
      it->deleted= true;
      return MYLITE_STORAGE_OK;
    }
  }

  return MYLITE_STORAGE_NOTFOUND;
}

mylite_storage_result mylite_volatile_truncate_table(
  const char *primary_file, const char *schema_name, const char *table_name)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name))
    return MYLITE_STORAGE_MISUSE;

  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (!table)
    return MYLITE_STORAGE_OK;

  table->rows.clear();
  table->next_row_id= 1ULL;
  table->auto_increment_value= 1ULL;
  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_read_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long *out_next_value)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      !out_next_value)
    return MYLITE_STORAGE_MISUSE;

  *out_next_value= 1ULL;
  std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (table)
    *out_next_value= table->auto_increment_value;
  return MYLITE_STORAGE_OK;
}

mylite_storage_result mylite_volatile_set_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long next_value)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      next_value == 0ULL)
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    Mylite_volatile_table *table=
      mylite_find_or_create_volatile_table_locked(primary_file, schema_name,
                                                  table_name);
    table->auto_increment_value= next_value;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

mylite_storage_result mylite_volatile_advance_auto_increment(
  const char *primary_file, const char *schema_name, const char *table_name,
  unsigned long long next_value)
{
  if (!mylite_volatile_table_args_valid(primary_file, schema_name, table_name) ||
      next_value == 0ULL)
    return MYLITE_STORAGE_MISUSE;

  try
  {
    std::lock_guard<std::mutex> guard(mylite_volatile_mutex);
    Mylite_volatile_table *table=
      mylite_find_or_create_volatile_table_locked(primary_file, schema_name,
                                                  table_name);
    if (next_value > table->auto_increment_value)
      table->auto_increment_value= next_value;
    return MYLITE_STORAGE_OK;
  }
  catch (const std::bad_alloc &)
  {
    return MYLITE_STORAGE_NOMEM;
  }
}

static bool mylite_volatile_table_args_valid(const char *primary_file,
                                             const char *schema_name,
                                             const char *table_name)
{
  return mylite_volatile_primary_file_valid(primary_file) &&
         schema_name && schema_name[0] &&
         table_name && table_name[0];
}

static bool mylite_volatile_primary_file_valid(const char *primary_file)
{
  return primary_file && primary_file[0];
}

static bool mylite_same_volatile_table(const Mylite_volatile_table &table,
                                       const char *primary_file,
                                       const char *schema_name,
                                       const char *table_name)
{
  return table.primary_file == primary_file &&
         table.schema_name == schema_name &&
         table.table_name == table_name;
}

static bool mylite_same_volatile_table_identity(
  const Mylite_volatile_table &left, const Mylite_volatile_table &right)
{
  return left.primary_file == right.primary_file &&
         left.schema_name == right.schema_name &&
         left.table_name == right.table_name;
}

static bool mylite_volatile_alias_participates_in_snapshots_locked(
  const Mylite_volatile_table_alias &alias)
{
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(alias.primary_file.c_str(),
                                      alias.target_schema_name.c_str(),
                                      alias.target_table_name.c_str());
  return table && table->participates_in_snapshots;
}

static Mylite_volatile_table *mylite_find_volatile_table_locked(
  const char *primary_file, const char *schema_name, const char *table_name)
{
  for (std::vector<Mylite_volatile_table>::iterator it=
         mylite_volatile_tables.begin();
       it != mylite_volatile_tables.end(); ++it)
  {
    if (mylite_same_volatile_table(*it, primary_file, schema_name, table_name))
      return &*it;
  }

  return NULL;
}

static Mylite_volatile_table *mylite_find_or_create_volatile_table_locked(
  const char *primary_file, const char *schema_name, const char *table_name)
{
  Mylite_volatile_table *table=
    mylite_find_volatile_table_locked(primary_file, schema_name, table_name);
  if (table)
    return table;

  Mylite_volatile_table new_table;
  new_table.primary_file= primary_file;
  new_table.schema_name= schema_name;
  new_table.table_name= table_name;
  new_table.next_row_id= 1ULL;
  new_table.auto_increment_value= 1ULL;
  new_table.participates_in_snapshots= true;
  mylite_volatile_tables.push_back(new_table);
  return &mylite_volatile_tables.back();
}

static bool mylite_erase_volatile_table_locked(const char *primary_file,
                                               const char *schema_name,
                                               const char *table_name)
{
  for (std::vector<Mylite_volatile_table>::iterator it=
         mylite_volatile_tables.begin();
       it != mylite_volatile_tables.end(); ++it)
  {
    if (mylite_same_volatile_table(*it, primary_file, schema_name, table_name))
    {
      mylite_volatile_tables.erase(it);
      return true;
    }
  }

  return false;
}

static void mylite_preserve_volatile_table_counters_locked(
  Mylite_volatile_table *restored_table)
{
  for (std::vector<Mylite_volatile_table>::const_iterator it=
         mylite_volatile_tables.begin();
       it != mylite_volatile_tables.end(); ++it)
  {
    if (!mylite_same_volatile_table_identity(*it, *restored_table))
      continue;

    if (it->next_row_id > restored_table->next_row_id)
      restored_table->next_row_id= it->next_row_id;
    if (it->auto_increment_value > restored_table->auto_increment_value)
      restored_table->auto_increment_value= it->auto_increment_value;
    return;
  }
}
