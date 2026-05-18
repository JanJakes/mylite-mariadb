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
  size_t row_offset;
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
  unsigned char *index_rows;
  unsigned char *index_blob_payloads;
  unsigned char *index_keys;
  Mylite_index_cursor_entry *index_entries;
  char storage_schema_name[NAME_LEN + 1];
  char storage_table_name[NAME_LEN + 1];
  char display_engine_name[NAME_LEN + 1];
  LEX_CSTRING display_engine_name_lex;
  size_t scan_row_size;
  size_t scan_row_count;
  size_t scan_row_index;
  size_t scan_blob_payloads_size;
  size_t record_blob_payloads_size[2];
  size_t index_row_size;
  size_t index_row_count;
  size_t index_row_index;
  size_t index_blob_payloads_size;
  uint index_cursor_number;
  ulonglong current_row_id;
  uint duplicate_key_index;
  bool discard_rows;
  bool volatile_rows;

  Mylite_share *get_share();
  void clear_scan_rows();
  void clear_index_cursor();
  void clear_record_blob_payloads();
  const char *storage_schema() const;
  const char *storage_table() const;
  int build_index_cursor(uint index_number);
  int read_index_cursor_row(uchar *buf, size_t row_index);
  int record_blob_payload_slot(const uchar *buf, size_t *out_slot) const;
  int preserve_record_blob_payloads(uchar *buf);

public:
  ha_mylite(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_mylite() override;

  ulonglong table_flags() const override
  {
    return HA_NO_TRANSACTIONS | HA_NULL_IN_KEY | HA_REC_NOT_IN_SEQ |
           HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY | HA_CAN_VIRTUAL_COLUMNS |
           HA_BINLOG_STMT_CAPABLE | HA_NO_ONLINE_ALTER;
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
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;
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
