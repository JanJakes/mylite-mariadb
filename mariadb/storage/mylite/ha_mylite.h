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

class ha_mylite: public handler
{
  THR_LOCK_DATA lock;
  Mylite_share *share;
  unsigned char *scan_rows;
  unsigned char *scan_blob_payloads;
  size_t scan_row_size;
  size_t scan_row_count;
  size_t scan_row_index;
  size_t scan_blob_payloads_size;
  uint duplicate_key_index;

  Mylite_share *get_share();
  void clear_scan_rows();

public:
  ha_mylite(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_mylite() override;

  ulonglong table_flags() const override
  {
    return HA_BINLOG_STMT_CAPABLE;
  }

  ulong index_flags(uint, uint, bool) const override
  {
    return 0;
  }

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
  int rnd_init(bool scan) override;
  int rnd_end(void) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint flag) override;
  int external_lock(THD *thd, int lock_type) override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  int write_row(const uchar *buf) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  int delete_table(const char *name) override;
  int rename_table(const char *from, const char *to) override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
};

#endif
