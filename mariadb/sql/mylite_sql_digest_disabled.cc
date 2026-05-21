/* Copyright (c) 2026, MyLite contributors.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"

#include <string.h>

#include "my_md5.h"
#include "unireg.h"

#include "sql_string.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sp_pcontext.h"
#include "sql_digest.h"
#include "sql_digest_stream.h"
#include "sql_get_diagnostics.h"

/* Generated code */
#include "yy_mariadb.hh"

/* Name pollution from sql/sql_lex.h */
#ifdef LEX_YYSTYPE
#undef LEX_YYSTYPE
#endif

#define LEX_YYSTYPE YYSTYPE*

void compute_digest_md5(const sql_digest_storage *digest_storage,
                        unsigned char *md5)
{
  (void) digest_storage;
  memset(md5, 0, MD5_HASH_SIZE);
}

void compute_digest_text(const sql_digest_storage *digest_storage,
                         String *digest_text)
{
  (void) digest_storage;
  digest_text->length(0);
}

sql_digest_state *digest_add_token(sql_digest_state *state, uint token,
                                   LEX_YYSTYPE yylval)
{
  (void) token;
  (void) yylval;
  return state;
}

sql_digest_state *digest_reduce_token(sql_digest_state *state, uint token_left,
                                      uint token_right)
{
  (void) token_left;
  (void) token_right;
  return state;
}
