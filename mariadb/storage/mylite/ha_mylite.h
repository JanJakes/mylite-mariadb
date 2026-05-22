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

#ifndef HA_MYLITE_INCLUDED
#define HA_MYLITE_INCLUDED

#include <stddef.h>

#include "handler.h"
#include "my_base.h"
#include "my_global.h"
#include "thr_lock.h"

class Mylite_share: public Handler_share
{
public:
  THR_LOCK lock;

  Mylite_share();
  ~Mylite_share()
  {
    thr_lock_delete(&lock);
  }
};

struct Mylite_index_cursor_entry
{
  size_t key_offset;
  size_t key_size;
  ulonglong row_id;
};

class ha_mylite: public handler
{
  THR_LOCK_DATA lock;
  Mylite_share *share;
  unsigned char *scan_rows;
  unsigned char *scan_blob_payloads;
  ulonglong *scan_row_ids;
  unsigned char *record_blob_payloads[2];
  unsigned char *index_keys;
  Mylite_index_cursor_entry *index_entries;
  unsigned char *index_rows;
  unsigned char *index_row_scratch;
  ulonglong *index_row_id_scratch;
  unsigned char index_inline_key[MAX_KEY_LENGTH];
  unsigned char direct_update_key_buffer[MAX_KEY_LENGTH];
  uchar direct_update_key_may_change[MAX_KEY];
  Mylite_index_cursor_entry index_inline_entry;
  size_t *index_row_offsets;
  size_t *index_row_sizes;
  size_t index_row_scratch_capacity;
  size_t index_row_id_scratch_capacity;
  size_t index_inline_row_offset;
  size_t index_inline_row_size;
  char storage_schema_name[NAME_LEN + 1];
  char storage_table_name[NAME_LEN + 1];
  char display_engine_name[NAME_LEN + 1];
  LEX_CSTRING display_engine_name_lex;
  size_t scan_row_size;
  size_t scan_row_count;
  size_t scan_row_index;
  size_t scan_blob_payloads_size;
  size_t record_blob_payloads_size[2];
  size_t index_row_bytes;
  size_t index_row_count;
  size_t index_row_index;
  uint index_cursor_number;
  ulonglong current_row_id;
  uint duplicate_key_index;
  mutable ulonglong foreign_key_presence_epoch;
  mutable bool child_foreign_key_presence_known;
  mutable bool child_foreign_key_presence;
  mutable bool parent_foreign_key_presence_known;
  mutable bool parent_foreign_key_presence;
  Field *auto_increment_field;
  Field *direct_update_key_field;
  bool index_cursor_filtered;
  bool discard_rows;
  bool volatile_rows;
  bool table_has_blob_fields;
  bool table_supports_row_write;
  bool table_supports_row_lifecycle;
  bool direct_update_row_in_progress;
  bool direct_update_can_compare_record;
  bool direct_update_can_skip_duplicate_key_checks;
  bool direct_update_may_change_index_entries;
  bool direct_update_condition_guaranteed_by_key;
  COND *direct_update_condition;
  List<Item> *direct_update_fields;
  List<Item> *direct_update_values;
  Item *direct_update_key_value;
  uint direct_update_key_number;
  uint direct_update_key_field_number;
  uchar direct_update_key_null;

  Mylite_share *get_share();
  void clear_scan_rows();
  void clear_index_cursor();
  void clear_index_row_scratch();
  void clear_record_blob_payloads();
  void clear_direct_update_state();
  const char *storage_schema() const;
  const char *storage_table() const;
  int build_index_cursor(uint index_number, const uchar *key_filter,
                         uint key_filter_length);
  int materialize_index_cursor_rows(const char *primary_file);
  int ensure_index_row_id_scratch(size_t row_count);
  int read_index_cursor_row(uchar *buf, size_t row_index);
  int read_exact_unique_index_row_into(uint index_number,
                                       const uchar *key_filter,
                                       uint key_filter_length, uchar *buf,
                                       bool *out_applied, bool *out_found);
  int build_direct_update_key(bool *out_has_key);
  int record_blob_payload_slot(const uchar *buf, size_t *out_slot) const;
  int preserve_record_blob_payloads(uchar *buf);
  void clear_foreign_key_presence_cache() const;
  void invalidate_foreign_key_presence_cache() const;
  int has_child_foreign_keys(bool *out_has) const;
  int has_parent_foreign_keys(bool *out_has) const;

public:
  ha_mylite(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_mylite() override;

  ulonglong table_flags() const override
  {
    return HA_NULL_IN_KEY | HA_REC_NOT_IN_SEQ | HA_CAN_INDEX_BLOBS |
           HA_AUTO_PART_KEY | HA_CAN_VIRTUAL_COLUMNS | HA_BINLOG_STMT_CAPABLE |
           HA_NO_ONLINE_ALTER | HA_CAN_DIRECT_UPDATE_AND_DELETE;
  }

  ulong index_flags(uint index_number, uint part, bool all_parts) const override;

  uint max_supported_keys() const override
  {
    return MAX_KEY;
  }

  uint max_supported_key_part_length() const override
  {
    return MAX_DATA_LENGTH_FOR_KEY;
  }

  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  LEX_CSTRING *engine_name() override;
  int index_init(uint idx, bool sorted) override;
  int index_end() override;
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_idx_map(uchar *buf, uint index, const uchar *key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;
  const COND *cond_push(const COND *cond) override;
  void cond_pop() override;
  int info_push(uint info_type, void *info) override;
  int direct_update_rows_init(List<Item> *update_fields) override;
  int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows) override;
  int rnd_init(bool scan) override;
  int rnd_end(void) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint flag) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;
  int external_lock(THD *thd, int lock_type) override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  int reset_auto_increment(ulonglong value) override;
  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;
  int truncate() override;
  int reset() override;
  char *get_foreign_key_create_info() override;
  void free_foreign_key_create_info(char *str) override;
  int get_foreign_key_list(THD *thd,
                           List<FOREIGN_KEY_INFO> *f_key_list) override;
  int get_parent_foreign_key_list(THD *thd,
                                  List<FOREIGN_KEY_INFO> *f_key_list) override;
  bool referenced_by_foreign_key() const noexcept override;
  enum_alter_inplace_result check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  int delete_table(const char *name) override;
  int rename_table(const char *from, const char *to) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
};

#endif
