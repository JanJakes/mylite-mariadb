/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "gtid_index.h"

/*
  The default embedded profile never opens binary logs, so GTID index reads and
  writes are unsupported. These definitions keep retained MariaDB no-binlog
  paths linkable while every GTID-index operation fails closed.
*/

Gtid_index_base::Index_node_base::Index_node_base()
  : first_page(nullptr), current_page(nullptr), current_ptr(nullptr)
{
}

Gtid_index_base::Index_node_base::~Index_node_base()
{
  free_pages();
}

void Gtid_index_base::Index_node_base::free_pages()
{
  Node_page *page= first_page;
  while (page)
  {
    Node_page *next= page->next;
    my_free(page);
    page= next;
  }
}

void Gtid_index_base::Index_node_base::reset()
{
  free_pages();
  first_page= current_page= nullptr;
  current_ptr= nullptr;
}

Gtid_index_base::Gtid_index_base()
  : gtid_buffer(nullptr), gtid_buffer_alloc(0), page_size(0)
{
  index_file_name[0]= '\0';
}

Gtid_index_base::~Gtid_index_base()
{
  my_free(gtid_buffer);
}

void Gtid_index_base::make_gtid_index_file_name(char *out_name, size_t bufsize,
                                                const char *base_filename)
{
  char *p= strmake(out_name, base_filename, bufsize - 1);
  size_t remain= bufsize - (p - out_name);
  strmake(p, ".idx", remain - 1);
}

int Gtid_index_base::update_gtid_state(rpl_binlog_state_base *,
                                       const rpl_gtid *, uint32)
{
  return 1;
}

Gtid_index_base::Node_page *Gtid_index_base::alloc_page()
{
  return nullptr;
}

rpl_gtid *Gtid_index_base::gtid_list_buffer(uint32)
{
  return nullptr;
}

void Gtid_index_base::build_index_filename(const char *filename)
{
  make_gtid_index_file_name(index_file_name, sizeof(index_file_name), filename);
}

Gtid_index_writer *Gtid_index_writer::hot_index_list= nullptr;
mysql_mutex_t Gtid_index_writer::gtid_index_mutex;

void Gtid_index_writer::gtid_index_init()
{
}

void Gtid_index_writer::gtid_index_cleanup()
{
}

Gtid_index_writer::Gtid_index_writer(const char *, uint32,
                                     rpl_binlog_state_base *,
                                     uint32, my_off_t opt_span_min)
  : offset_min_threshold(opt_span_min), next_hot_index(nullptr),
    nodes(nullptr), previous_offset(0), max_level(0), index_file(-1),
    error_state(true), file_header_written(false), in_hot_index_list(false)
{
  pending_state.init();
}

Gtid_index_writer::~Gtid_index_writer()
{
}

void Gtid_index_writer::process_gtid(uint32, const rpl_gtid *)
{
}

int Gtid_index_writer::process_gtid_check_batch(uint32, const rpl_gtid *,
                                                rpl_gtid **out_gtid_list,
                                                uint32 *out_gtid_count)
{
  if (out_gtid_list)
    *out_gtid_list= nullptr;
  if (out_gtid_count)
    *out_gtid_count= 0;
  return 1;
}

int Gtid_index_writer::async_update(uint32, rpl_gtid *, uint32)
{
  return 1;
}

void Gtid_index_writer::close()
{
}

const Gtid_index_writer *Gtid_index_writer::find_hot_index(const char *)
{
  return nullptr;
}

void Gtid_index_writer::insert_in_hot_index()
{
}

void Gtid_index_writer::remove_from_hot_index()
{
}

uint32 Gtid_index_writer::write_current_node(uint32, bool)
{
  return 0;
}

int Gtid_index_writer::reserve_space(Index_node *, size_t)
{
  return 1;
}

int Gtid_index_writer::do_write_record(uint32, uint32, const rpl_gtid *, uint32)
{
  return 1;
}

int Gtid_index_writer::add_child_ptr(uint32, my_off_t)
{
  return 1;
}

int Gtid_index_writer::write_record(uint32, const rpl_gtid *, uint32)
{
  return 1;
}

bool Gtid_index_writer::check_room(uint32, uint32)
{
  return false;
}

int Gtid_index_writer::alloc_level_if_missing(uint32)
{
  return 1;
}

uchar *Gtid_index_writer::init_header(Node_page *, bool, bool)
{
  return nullptr;
}

int Gtid_index_writer::give_error(const char *)
{
  return 1;
}

Gtid_index_reader::Gtid_index_reader()
  : n(nullptr), search_cmp_function(nullptr), in_search_gtid_pos(nullptr),
    read_page(nullptr), read_ptr(nullptr), index_file(-1), current_offset(0),
    in_search_offset(0), file_open(false), index_valid(false),
    has_root_node(false), version_major(0), version_minor(0)
{
  current_state.init();
  compare_state.init();
}

Gtid_index_reader::~Gtid_index_reader()
{
}

int Gtid_index_reader::open_index_file(const char *)
{
  return 1;
}

void Gtid_index_reader::close_index_file()
{
}

int Gtid_index_reader::search_offset(uint32, uint32 *out_offset,
                                     uint32 *out_gtid_count)
{
  if (out_offset)
    *out_offset= 0;
  if (out_gtid_count)
    *out_gtid_count= 0;
  return -1;
}

int Gtid_index_reader::search_gtid_pos(slave_connection_state *,
                                       uint32 *out_offset,
                                       uint32 *out_gtid_count)
{
  if (out_offset)
    *out_offset= 0;
  if (out_gtid_count)
    *out_gtid_count= 0;
  return -1;
}

rpl_gtid *Gtid_index_reader::search_gtid_list()
{
  return nullptr;
}

int Gtid_index_reader::search_cmp_offset(uint32, rpl_binlog_state_base *)
{
  return 0;
}

int Gtid_index_reader::search_cmp_gtid_pos(uint32, rpl_binlog_state_base *)
{
  return 0;
}

int Gtid_index_reader::do_index_search(uint32 *, uint32 *)
{
  return -1;
}

int Gtid_index_reader::do_index_search_root(uint32 *, uint32 *)
{
  return -1;
}

int Gtid_index_reader::do_index_search_leaf(bool, uint32 *, uint32 *)
{
  return -1;
}

int Gtid_index_reader::next_page()
{
  return 1;
}

int Gtid_index_reader::find_bytes(uint32)
{
  return 1;
}

int Gtid_index_reader::get_child_ptr(uint32 *)
{
  return 1;
}

int Gtid_index_reader::get_offset_count(uint32 *, uint32 *)
{
  return 1;
}

int Gtid_index_reader::get_gtid_list(rpl_gtid *, uint32)
{
  return 1;
}

int Gtid_index_reader::read_file_header()
{
  return 1;
}

int Gtid_index_reader::verify_checksum(Node_page *)
{
  return 1;
}

Gtid_index_base::Node_page *Gtid_index_reader::alloc_and_read_page()
{
  return nullptr;
}

int Gtid_index_reader::read_root_node()
{
  return 1;
}

int Gtid_index_reader::read_node(uint32)
{
  return 1;
}

int Gtid_index_reader::read_node_cold(uint32)
{
  return 1;
}

int Gtid_index_reader::give_error(const char *)
{
  return 1;
}

Gtid_index_reader_hot::Gtid_index_reader_hot()
  : hot_writer(nullptr), hot_level(0)
{
}

int Gtid_index_reader_hot::do_index_search(uint32 *, uint32 *)
{
  return -1;
}

int Gtid_index_reader_hot::get_child_ptr(uint32 *)
{
  return 1;
}

int Gtid_index_reader_hot::read_file_header()
{
  return 1;
}

int Gtid_index_reader_hot::read_root_node()
{
  return 1;
}

int Gtid_index_reader_hot::read_node(uint32)
{
  return 1;
}

int Gtid_index_reader_hot::read_node_hot()
{
  return 1;
}
