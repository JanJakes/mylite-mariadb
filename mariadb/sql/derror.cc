/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (C) 2011, 2020, MariaDB

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

/**
  @file

  @brief
  Read language depeneded messagefile
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "derror.h"
#include "mysys_err.h"
#include "mysqld.h"                             // lc_messages_dir
#include "derror.h"                             // read_texts
#include "sql_class.h"                          // THD

#ifndef MYLITE_WITH_FULL_ERROR_MESSAGES
#define MYLITE_WITH_FULL_ERROR_MESSAGES 1
#endif

uint errors_per_range[MAX_ERROR_RANGES+1];

static bool check_error_mesg(const char *file_name, const char **errmsg);
#if !MYLITE_WITH_FULL_ERROR_MESSAGES
static bool init_compact_english_error_messages(void);
static void init_compact_error_ranges(void);
static const char *compact_error_message(uint id);
#endif
static void init_myfunc_errs(void);


C_MODE_START
static const char **get_server_errmsgs(int nr)
{
  int section= (nr-ER_ERROR_FIRST) / ERRORS_PER_RANGE;
  if (!current_thd)
    return DEFAULT_ERRMSGS[section];
  return CURRENT_THD_ERRMSGS[section];
}
C_MODE_END

/**
  Read messages from errorfile.

  This function can be called multiple times to reload the messages.

  If it fails to load the messages:
   - If we already have error messages loaded, keep the old ones and
     return FALSE(ok)
  - Initializing the errmesg pointer to an array of empty strings
    and return TRUE (error)

  @retval
    FALSE       OK
  @retval
    TRUE        Error
*/

static const char ***original_error_messages;

bool init_errmessage(void)
{
  bool error= FALSE;
  const char *lang= my_default_lc_messages->errmsgs->language;
  my_bool use_english;

  DBUG_ENTER("init_errmessage");

  free_error_messages();
  my_free(original_error_messages);
  original_error_messages= 0;

  error_message_charset_info= system_charset_info;

  use_english= !strcmp(lang, "english");
  if (!use_english)
  {
    /* Read messages from file. */
    use_english= read_texts(ERRMSG_FILE,lang, &original_error_messages);
    error= use_english != FALSE;
    if (error)
      sql_print_error("Could not load error messages for %s",lang);
  }

  if (use_english)
  {
#if MYLITE_WITH_FULL_ERROR_MESSAGES
    static const struct
    {
      const char* name;
      uint id;
      const char* fmt;
    }
    english_msgs[]=
    {
      #include <mysqld_ername.h>
    };

    memset(errors_per_range, 0, sizeof(errors_per_range));
    /* Calculate nr of messages per range. */
    for (size_t i= 0; i < array_elements(english_msgs); i++)
    {
      uint id= english_msgs[i].id;

      // We rely on the fact the array is sorted by id.
      DBUG_ASSERT(i == 0 || english_msgs[i-1].id < id);

      errors_per_range[id/ERRORS_PER_RANGE-1]= id%ERRORS_PER_RANGE + 1;
    }

    size_t all_errors= 0;
    for (size_t i= 0; i < MAX_ERROR_RANGES; i++)
      all_errors+= errors_per_range[i];

    const char **errmsgs;
    if (!(original_error_messages= (const char***)
          my_malloc(PSI_NOT_INSTRUMENTED,
                    (all_errors + MAX_ERROR_RANGES)*sizeof(void*),
                    MYF(MY_ZEROFILL))))
      DBUG_RETURN(TRUE);

    errmsgs= (const char**)(original_error_messages + MAX_ERROR_RANGES);

    original_error_messages[0]= errmsgs;
    for (uint i= 1; i < MAX_ERROR_RANGES; i++)
    {
      original_error_messages[i]=
        original_error_messages[i-1] + errors_per_range[i-1];
    }

    for (uint i= 0; i < array_elements(english_msgs); i++)
    {
      uint id= english_msgs[i].id;
      original_error_messages[id/ERRORS_PER_RANGE-1][id%ERRORS_PER_RANGE]=
         english_msgs[i].fmt;
    }
#else
    if (init_compact_english_error_messages())
      DBUG_RETURN(TRUE);
#endif
  }

  /* Register messages for use with my_error(). */
  for (uint i=0 ; i < MAX_ERROR_RANGES ; i++)
  {
    if (errors_per_range[i])
    {
      if (my_error_register(get_server_errmsgs, (i+1)*ERRORS_PER_RANGE,
                            (i+1)*ERRORS_PER_RANGE +
                            errors_per_range[i]-1))
      {
        my_free(original_error_messages);
        original_error_messages= 0;
        DBUG_RETURN(TRUE);
      }
    }
  }
  DEFAULT_ERRMSGS= original_error_messages;
  init_myfunc_errs();			/* Init myfunc messages */
  DBUG_RETURN(error);
}

#if !MYLITE_WITH_FULL_ERROR_MESSAGES
static bool init_compact_english_error_messages(void)
{
  const char **errmsgs;

  init_compact_error_ranges();

  size_t all_errors= 0;
  for (size_t i= 0; i < MAX_ERROR_RANGES; i++)
    all_errors+= errors_per_range[i];

  if (!(original_error_messages= (const char***)
        my_malloc(PSI_NOT_INSTRUMENTED,
                  (all_errors + MAX_ERROR_RANGES)*sizeof(void*),
                  MYF(MY_ZEROFILL))))
    return true;

  errmsgs= (const char**)(original_error_messages + MAX_ERROR_RANGES);

  original_error_messages[0]= errmsgs;
  for (uint i= 1; i < MAX_ERROR_RANGES; i++)
  {
    original_error_messages[i]=
      original_error_messages[i-1] + errors_per_range[i-1];
  }

  for (uint range= 0; range < MAX_ERROR_RANGES; range++)
  {
    for (uint offset= 0; offset < errors_per_range[range]; offset++)
    {
      uint id= (range + 1) * ERRORS_PER_RANGE + offset;
      original_error_messages[range][offset]= compact_error_message(id);
    }
  }

  return false;
}


static void init_compact_error_ranges(void)
{
  memset(errors_per_range, 0, sizeof(errors_per_range));
  errors_per_range[0]= ER_ERROR_LAST_SECTION_2 - ER_ERROR_FIRST + 1;
  errors_per_range[2]= ER_ERROR_LAST_SECTION_4 -
                       ER_ERROR_FIRST_SECTION_4 + 1;
  errors_per_range[3]= ER_ERROR_LAST - ER_ERROR_FIRST_SECTION_5 + 1;
}


static const char *compact_error_message(uint id)
{
  switch (id)
  {
  case ER_CANT_CREATE_FILE:
    return "Can't create file '%-.200s' (errno: %iE)";
  case ER_CANT_CREATE_TABLE:
    return "Can't create table %sQ.%sQ (errno: %iE)";
  case ER_CANT_CREATE_DB:
    return "Can't create database '%-.192s' (errno: %iE)";
  case ER_DB_CREATE_EXISTS:
    return "Can't create database '%-.192s'; database exists";
  case ER_DB_DROP_EXISTS:
    return "Can't drop database '%-.192s'; database doesn't exist";
  case ER_CANT_DELETE_FILE:
    return "Error on delete of '%-.192s' (errno: %iE)";
  case ER_CANT_GET_STAT:
    return "Can't get status of '%-.200s' (errno: %iE)";
  case ER_CANT_LOCK:
    return "Can't lock file (errno: %iE)";
  case ER_CANT_OPEN_FILE:
    return "Can't open file: '%-.200s' (errno: %iE)";
  case ER_FILE_NOT_FOUND:
    return "Can't find file: '%-.200s' (errno: %iE)";
  case ER_CANT_READ_DIR:
    return "Can't read dir of '%-.192s' (errno: %iE)";
  case ER_DISK_FULL:
    return "Disk full (%s); waiting for someone to free some space... "
           "(errno: %iE)";
  case ER_DUP_KEY:
    return "Can't write; duplicate key in table '%-.192s'";
  case ER_ERROR_ON_CLOSE:
    return "Error on close of '%-.192s' (errno: %iE)";
  case ER_ERROR_ON_READ:
    return "Error reading file '%-.200s' (errno: %iE)";
  case ER_ERROR_ON_RENAME:
    return "Error on rename of '%-.210s' to '%-.210s' (errno: %iE)";
  case ER_ERROR_ON_WRITE:
    return "Error writing file '%-.200s' (errno: %iE)";
  case ER_GET_ERRNO:
    return "Got error %iE from storage engine %s";
  case ER_ILLEGAL_HA:
    return "Storage engine %s of the table %sQ.%sQ doesn't have this option";
  case ER_KEY_NOT_FOUND:
    return "Can't find record in '%-.192s'";
  case ER_NOT_KEYFILE:
    return "Index for table '%-.200s' is corrupt; try to repair it";
  case ER_OPEN_AS_READONLY:
    return "Table '%-.192s' is read only";
  case ER_OUTOFMEMORY:
    return "Out of memory; restart server and try again (needed %d bytes)";
  case ER_OUT_OF_SORTMEMORY:
    return "Out of sort memory, consider increasing server sort buffer size";
  case ER_UNEXPECTED_EOF:
    return "Unexpected EOF found when reading file '%-.192s' (errno: %iE)";
  case ER_NO_DB_ERROR:
    return "No database selected";
  case ER_BAD_NULL_ERROR:
    return "Column '%-.192s' cannot be null";
  case ER_BAD_DB_ERROR:
    return "Unknown database '%-.192s'";
  case ER_TABLE_EXISTS_ERROR:
    return "Table '%-.192s' already exists";
  case ER_BAD_TABLE_ERROR:
    return "Unknown table '%-.100sT'";
  case ER_BAD_FIELD_ERROR:
    return "Unknown column '%-.192s' in '%-.192s'";
  case ER_WRONG_VALUE_COUNT:
    return "Column count doesn't match value count";
  case ER_DUP_FIELDNAME:
    return "Duplicate column name '%-.192s'";
  case ER_DUP_KEYNAME:
    return "Duplicate key name '%-.192s'";
  case ER_DUP_ENTRY:
    return "Duplicate entry '%-.192sT' for key %d";
  case ER_PARSE_ERROR:
    return "%s near '%-.80sT' at line %d";
  case ER_INVALID_DEFAULT:
    return "Invalid default value for '%-.192s'";
  case ER_UNKNOWN_TABLE:
    return "Unknown table '%-.192s' in %-.32s";
  case ER_SYNTAX_ERROR:
    return "You have an error in your SQL syntax; check the manual that "
           "corresponds to your MariaDB server version for the right syntax "
           "to use";
  case ER_RECORD_FILE_FULL:
    return "The table '%-.192s' is full";
  case ER_WRONG_VALUE_COUNT_ON_ROW:
    return "Column count doesn't match value count at row %lu";
  case ER_NO_SUCH_TABLE:
    return "Table '%-.192s.%-.192s' doesn't exist";
  case ER_LOCK_WAIT_TIMEOUT:
    return "Lock wait timeout exceeded; try restarting transaction";
  case ER_LOCK_DEADLOCK:
    return "Deadlock found when trying to get lock; try restarting "
           "transaction";
  case ER_CANNOT_ADD_FOREIGN:
    return "Cannot add foreign key constraint for `%s`";
  case ER_NO_REFERENCED_ROW:
    return "Cannot add or update a child row: a foreign key constraint fails";
  case ER_ROW_IS_REFERENCED:
    return "Cannot delete or update a parent row: a foreign key constraint "
           "fails";
  case ER_NOT_SUPPORTED_YET:
    return "This version of MariaDB doesn't yet support '%s'";
  case WARN_DATA_TRUNCATED:
    return "Data truncated for column '%s' at row %lu";
  case ER_UNKNOWN_STORAGE_ENGINE:
    return "Unknown storage engine '%s'";
  case ER_TRUNCATED_WRONG_VALUE:
    return "Truncated incorrect %-.32sT value: '%-.128sT'";
  case ER_SP_DOES_NOT_EXIST:
    return "%s %s does not exist";
  case ER_TRUNCATED_WRONG_VALUE_FOR_FIELD:
    return "Incorrect %-.32s value: '%-.128sT' for column "
           "`%.192s`.`%.192s`.`%.192s` at row %lu";
  case ER_DATA_TOO_LONG:
    return "Data too long for column '%s' at row %lu";
  case ER_ROW_IS_REFERENCED_2:
    return "Cannot delete or update a parent row: a foreign key constraint "
           "fails (%s)";
  case ER_NO_REFERENCED_ROW_2:
    return "Cannot add or update a child row: a foreign key constraint "
           "fails (%s)";
  case ER_DUP_ENTRY_WITH_KEY_NAME:
    return "Duplicate entry '%-.64sT' for key '%-.192s'";
  case ER_FUNC_INEXISTENT_NAME_COLLISION:
    return "FUNCTION %s does not exist. Check the 'Function Name Parsing and "
           "Resolution' section in the Reference Manual";
  case ER_INDEX_COLUMN_TOO_LONG:
    return "Index column size too large. The maximum column size is %lu bytes";
  case ER_INDEX_CORRUPT:
    return "Index %s is corrupted";
  case ER_TABLESPACE_MISSING:
    return "Tablespace is missing for table '%-.192s'";
  case ER_DATA_TRUNCATED:
    return "Truncated value '%-.128s' when converting to %-.32s";
  case ER_NO_SUCH_TABLE_IN_ENGINE:
    return "Table '%-.192s.%-.192s' doesn't exist in engine";
  default:
    return "MariaDB error";
  }
}
#endif


void free_error_messages()
{
  /* We don't need to free errmsg as it's done in cleanup_errmsg */
  for (uint i= 0 ; i < MAX_ERROR_RANGES ; i++)
  {
    if (errors_per_range[i])
    {
      my_error_unregister((i+1)*ERRORS_PER_RANGE,
                          (i+1)*ERRORS_PER_RANGE +
                          errors_per_range[i]-1);
      errors_per_range[i]= 0;
    }
  }
}


/**
   Check the error messages array contains all relevant error messages
*/

static bool check_error_mesg(const char *file_name, const char **errmsg)
{
  /*
    The last MySQL error message can't be an empty string; If it is,
    it means that the error file doesn't contain all MySQL messages
    and is probably from an older version of MySQL / MariaDB.
    We also check that each section has enough error messages.
  */
  if (errmsg[ER_LAST_MYSQL_ERROR_MESSAGE -1 - ER_ERROR_FIRST][0] == 0 ||
      (errors_per_range[0] < ER_ERROR_LAST_SECTION_2 - ER_ERROR_FIRST + 1) ||
      errors_per_range[1] != 0 ||
      (errors_per_range[2] < ER_ERROR_LAST_SECTION_4 - 
       ER_ERROR_FIRST_SECTION_4 +1) ||
      (errors_per_range[3] < ER_ERROR_LAST - ER_ERROR_FIRST_SECTION_5 + 1))
  {
    sql_print_error("Error message file '%s' is probably from and older "
                    "version of MariaDB as it doesn't contain all "
                    "error messages", file_name);
    return 1;
  }
  return 0;
}


struct st_msg_file
{
  uint sections;
  uint max_error;
  uint errors;
  size_t text_length;
};

/**
  Open file for packed textfile in language-directory.
*/

static File open_error_msg_file(const char *file_name, const char *language,
                                uint error_messages, struct st_msg_file *ret)
{
  int error_pos= 0;
  File file;
  char name[FN_REFLEN];
  char lang_path[FN_REFLEN];
  uchar head[32];
  DBUG_ENTER("open_error_msg_file");

  convert_dirname(lang_path, language, NullS);
  (void) my_load_path(lang_path, lang_path, lc_messages_dir);
  if ((file= mysql_file_open(key_file_ERRMSG,
                             fn_format(name, file_name, lang_path, "", 4),
                             O_RDONLY | O_SHARE | O_BINARY,
                             MYF(0))) < 0)
  {
    /*
      Trying pre-5.4 semantics of the --language parameter.
      It included the language-specific part, e.g.:
      --language=/path/to/english/
    */
    if ((file= mysql_file_open(key_file_ERRMSG,
                               fn_format(name, file_name, lc_messages_dir, "",
                                         4),
                               O_RDONLY | O_SHARE | O_BINARY,
                               MYF(0))) < 0)
      goto err;
    if (global_system_variables.log_warnings > 2)
    {
      sql_print_warning("An old style --language or -lc-message-dir value with language specific part detected: %s", lc_messages_dir);
      sql_print_warning("Use --lc-messages-dir without language specific part instead.");
    }
  }
  error_pos=1;
  if (mysql_file_read(file, (uchar*) head, 32, MYF(MY_NABP)))
    goto err;
  error_pos=2;
  if (head[0] != (uchar) 254 || head[1] != (uchar) 254 ||
      head[2] != 2 || head[3] != 5)
    goto err; /* purecov: inspected */

  ret->text_length= uint4korr(head+6);
  ret->max_error=   uint2korr(head+10);
  ret->errors=      uint2korr(head+12);
  ret->sections=    uint2korr(head+14);

  if (unlikely(ret->max_error < error_messages ||
               ret->sections != MAX_ERROR_RANGES))
  {
    sql_print_error("\
Error message file '%s' had only %d error messages, but it should contain at least %d error messages.\nCheck that the above file is the right version for this program!",
		    name,ret->errors,error_messages);
    (void) mysql_file_close(file, MYF(MY_WME));
    DBUG_RETURN(FERR);
  }
  DBUG_RETURN(file);

err:
  sql_print_error((error_pos == 2) ?
                  "Incompatible header in messagefile '%s'. Probably from "
                  "another version of MariaDB" :
                  ((error_pos == 1) ? "Can't read from messagefile '%s'" :
                   "Can't find messagefile '%s'"), name);
  if (file != FERR)
    (void) mysql_file_close(file, MYF(MY_WME));
  DBUG_RETURN(FERR);
}


/*
  Define the number of normal and extra error messages in the errmsg.sys
  file
*/

static const uint error_messages= ER_ERROR_LAST - ER_ERROR_FIRST+1;

/**
  Read text from packed textfile in language-directory.
*/

bool read_texts(const char *file_name, const char *language,
                const char ****data)
{
  uint i, range_size;
  const char **point;
  size_t offset;
  File file;
  uchar *buff, *pos;
  struct st_msg_file msg_file;
  DBUG_ENTER("read_texts");

  if (unlikely((file= open_error_msg_file(file_name, language, error_messages,
                                          &msg_file)) == FERR))
    DBUG_RETURN(1);

  if (!(*data= (const char***)
	my_malloc(key_memory_errmsgs,
                  (size_t) ((MAX_ERROR_RANGES+1) * sizeof(char**) +
                            MY_MAX(msg_file.text_length, msg_file.errors * 2)+
                            msg_file.errors * sizeof(char*)),
                  MYF(MY_WME))))
    goto err;					/* purecov: inspected */

  point= (const char**) ((*data) + MAX_ERROR_RANGES);
  buff=  (uchar*) (point + msg_file.errors);

  if (mysql_file_read(file, buff,
                      (size_t) (msg_file.errors + msg_file.sections) * 2,
                      MYF(MY_NABP | MY_WME)))
    goto err;

  pos= buff;
  /* read in sections */
  for (i= 0, offset= 0; i < msg_file.sections ; i++)
  {
    (*data)[i]= point + offset;
    errors_per_range[i]= range_size= uint2korr(pos);
    offset+= range_size;
    pos+= 2;
  }

  /* Calculate pointers to text data */
  for (i=0, offset=0 ; i < msg_file.errors ; i++)
  {
    point[i]= (char*) buff+offset;
    offset+=uint2korr(pos);
    pos+=2;
  }

  /* Read error message texts */
  if (mysql_file_read(file, buff, msg_file.text_length, MYF(MY_NABP | MY_WME)))
    goto err;

  (void) mysql_file_close(file, MYF(MY_WME));

  DBUG_RETURN(check_error_mesg(file_name, point));

err:
  (void) mysql_file_close(file, MYF(0));
  DBUG_RETURN(1);
} /* read_texts */


/**
  Initiates error-messages used by my_func-library.
*/

static void init_myfunc_errs()
{
  init_glob_errs();			/* Initiate english errors */
  if (!(specialflag & SPECIAL_ENGLISH))
  {
    EE(EE_FILENOTFOUND)   = ER_DEFAULT(ER_FILE_NOT_FOUND);
    EE(EE_CANTCREATEFILE) = ER_DEFAULT(ER_CANT_CREATE_FILE);
    EE(EE_READ)           = ER_DEFAULT(ER_ERROR_ON_READ);
    EE(EE_WRITE)          = ER_DEFAULT(ER_ERROR_ON_WRITE);
    EE(EE_BADCLOSE)       = ER_DEFAULT(ER_ERROR_ON_CLOSE);
    EE(EE_OUTOFMEMORY)    = ER_DEFAULT(ER_OUTOFMEMORY);
    EE(EE_DELETE)         = ER_DEFAULT(ER_CANT_DELETE_FILE);
    EE(EE_LINK)           = ER_DEFAULT(ER_ERROR_ON_RENAME);
    EE(EE_EOFERR)         = ER_DEFAULT(ER_UNEXPECTED_EOF);
    EE(EE_CANTLOCK)       = ER_DEFAULT(ER_CANT_LOCK);
    EE(EE_DIR)            = ER_DEFAULT(ER_CANT_READ_DIR);
    EE(EE_STAT)           = ER_DEFAULT(ER_CANT_GET_STAT);
    EE(EE_GETWD)          = ER_DEFAULT(ER_CANT_GET_WD);
    EE(EE_SETWD)          = ER_DEFAULT(ER_CANT_SET_WD);
    EE(EE_DISK_FULL)      = ER_DEFAULT(ER_DISK_FULL);
  }
}
