/* Copyright (c) 2026 MyLite contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include <mysql_com.h>

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
