/* Copyright (c) 2000-2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "ma_ftdefs.h"

ulong ft_min_word_len= 4;
ulong ft_max_word_len= HA_FT_MAXCHARLEN;
ulong ft_query_expansion_limit= 5;
const char *ft_boolean_syntax= DEFAULT_FTB_SYNTAX;
const char *ft_stopword_file= 0;
const char *ft_precompiled_stopwords[]= { 0 };

const HA_KEYSEG ft_keysegs[FT_SEGS]= {
  {
    0,
    HA_FT_WLEN,
    0,
    0,
    HA_VAR_LENGTH_PART | HA_PACK_KEY,
    HA_FT_MAXBYTELEN,
    63,
    HA_KEYTYPE_VARTEXT2,
    0,
    2,
    0,
  },
  {
    0,
    0,
    0,
    0,
    HA_NO_SORT,
    HA_FT_WLEN,
    63,
    HA_FT_WTYPE,
    0,
    0,
    0,
  },
};

int ft_init_stopwords(void)
{
  return 0;
}

void ft_free_stopwords(void)
{
  ft_stopword_file= 0;
}

int is_stopword(const char *word, size_t len)
{
  (void) word;
  (void) len;
  return 0;
}

my_bool ft_boolean_check_syntax_string(const uchar *str, size_t length,
                                       CHARSET_INFO *cs)
{
  uint i, j;

  if (cs->mbminlen != 1)
  {
    DBUG_ASSERT(0);
    return 1;
  }

  if (!str ||
      (length + 1 != sizeof(DEFAULT_FTB_SYNTAX)) ||
      (str[0] != ' ' && str[1] != ' '))
    return 1;

  for (i= 0; i < sizeof(DEFAULT_FTB_SYNTAX); i++)
  {
    if ((unsigned char) str[i] > 127 || my_isalnum(cs, str[i]))
      return 1;
    for (j= 0; j < i; j++)
    {
      if (str[i] == str[j] && (i != 11 || j != 10))
        return 1;
    }
  }
  return 0;
}

static int mylite_ft_default_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  return param->mysql_parse(param, param->doc, param->length);
}

struct st_mysql_ftparser ft_default_parser=
{
  MYSQL_FTPARSER_INTERFACE_VERSION, mylite_ft_default_parser_parse, 0, 0
};
