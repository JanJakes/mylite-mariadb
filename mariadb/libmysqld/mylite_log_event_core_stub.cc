/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"

#include "sql_string.h"

#include <mysql/psi/psi_memory.h>
#include <mysql_com.h>
#include <typelib.h>

PSI_memory_key key_memory_Incident_log_event_message;

const char *binlog_checksum_type_names[]= {
  "NONE",
  "CRC32",
  NullS
};

TYPELIB binlog_checksum_typelib= CREATE_TYPELIB_FOR(binlog_checksum_type_names);

char *str_to_hex(char *to, const uchar *from, size_t len)
{
  if (!len)
  {
    *to++= '"';
    *to++= '"';
    *to= '\0';
    return to;
  }

  *to++= 'X';
  *to++= '\'';
  to= octet2hex(to, from, len);
  *to++= '\'';
  *to= '\0';
  return to;
}

int append_query_string(CHARSET_INFO *csinfo, String *to, const char *str,
                        size_t len, bool no_backslash)
{
  char *beg, *ptr;
  my_bool overflow;
  uint32 const orig_len= to->length();
  if (to->reserve(orig_len + len * 2 + 4))
    return 1;

  beg= (char*)to->ptr() + to->length();
  ptr= beg;
  if (csinfo->escape_with_backslash_is_dangerous)
    ptr= str_to_hex(ptr, (uchar*)str, len);
  else
  {
    *ptr++= '\'';
    if (!no_backslash)
    {
      ptr+= escape_string_for_mysql(csinfo, ptr, 0, str, len, &overflow);
    }
    else
    {
      const char *frm_str= str;

      for (; frm_str < (str + len); frm_str++)
      {
        if (*frm_str == '\'')
          *ptr++= *frm_str;

        *ptr++= *frm_str;
      }
    }

    *ptr++= '\'';
  }
  to->length((uint32)(orig_len + ptr - beg));
  return 0;
}
