/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_time.h"
#include "sql_base.h"
#include "log.h"
#include "tztime.h"
#include "tzfile.h"

#include <hash.h>
#include <m_string.h>

typedef longlong my_int_time_t;

static const String tz_SYSTEM_name("SYSTEM", 6, &my_charset_latin1);
static const String tz_UTC_name("UTC", 3, &my_charset_latin1);

class Time_zone_system : public Time_zone
{
public:
  my_time_t TIME_to_gmt_sec(const MYSQL_TIME *t,
                            uint *error_code) const override;
  void gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const override;
  const String *get_name() const override;
  void get_timezone_information(struct my_tz *curr_tz,
                                const MYSQL_TIME *local_TIME) const override;
};

class Time_zone_utc : public Time_zone
{
public:
  my_time_t TIME_to_gmt_sec(const MYSQL_TIME *t,
                            uint *error_code) const override;
  void gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const override;
  const String *get_name() const override;
  void get_timezone_information(struct my_tz *curr_tz,
                                const MYSQL_TIME *local_TIME) const override;
};

class Time_zone_offset : public Time_zone
{
public:
  explicit Time_zone_offset(long tz_offset_arg);
  my_time_t TIME_to_gmt_sec(const MYSQL_TIME *t,
                            uint *error_code) const override;
  void gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const override;
  const String *get_name() const override;
  void get_timezone_information(struct my_tz *curr_tz,
                                const MYSQL_TIME *local_TIME) const override;

  long offset;

private:
  char name_buff[7 + 16];
  String name;
};

static Time_zone_utc tz_UTC;
static Time_zone_system tz_SYSTEM;
static Time_zone_offset tz_OFFSET0(0);

Time_zone *my_tz_OFFSET0= &tz_OFFSET0;
Time_zone *my_tz_UTC= &tz_UTC;
Time_zone *my_tz_SYSTEM= &tz_SYSTEM;

static HASH offset_tzs;
static MEM_ROOT tz_storage;
static mysql_mutex_t tz_LOCK;
static bool tz_inited= false;

static bool time_zone_name_eq(const String *name, const char *literal,
                              size_t length);
static my_bool str_to_offset(const char *str, uint length, long *offset);
static const uchar *my_offset_tzs_get_key(const void *entry_, size_t *length,
                                          my_bool);
static my_int_time_t sec_since_epoch(int year, int mon, int mday, int hour,
                                     int min, int sec);
static void sec_to_TIME(MYSQL_TIME *tmp, my_time_t t, long offset);

my_bool my_tz_init(THD *, const char *default_tzname, my_bool)
{
  if (!tz_inited)
  {
    if (my_hash_init(PSI_NOT_INSTRUMENTED, &offset_tzs, &my_charset_bin, 26,
                     0, 0, my_offset_tzs_get_key, 0, 0))
    {
      sql_print_error("Fatal error: OOM while initializing time zones");
      return 1;
    }
    init_alloc_root(PSI_NOT_INSTRUMENTED, &tz_storage, 4096, 0, MYF(0));
    mysql_mutex_init(0, &tz_LOCK, MY_MUTEX_INIT_FAST);
    tz_inited= true;
  }

  if (default_tzname)
  {
    String tz_name(default_tzname, strlen(default_tzname), &my_charset_latin1);
    if (!(global_system_variables.time_zone= my_tz_find(NULL, &tz_name)))
    {
      sql_print_error("Fatal error: Illegal or unknown default time zone '%s'",
                      default_tzname);
      return 1;
    }
  }

  default_tz= default_tz_name ? global_system_variables.time_zone :
                                my_tz_SYSTEM;
  return 0;
}

void my_tz_free()
{
  if (tz_inited)
  {
    tz_inited= false;
    mysql_mutex_destroy(&tz_LOCK);
    my_hash_free(&offset_tzs);
    free_root(&tz_storage, MYF(0));
  }
}

Time_zone *my_tz_find(THD *thd, const String *name)
{
  Time_zone *result_tz= NULL;
  long offset;
  DBUG_ENTER("my_tz_find");

  if (!name || name->is_empty())
    DBUG_RETURN(0);

  if (time_zone_name_eq(name, STRING_WITH_LEN("SYSTEM")))
    DBUG_RETURN(my_tz_SYSTEM);

  if (str_to_offset(name->ptr(), name->length(), &offset))
    DBUG_RETURN(0);

  if (offset == 0)
    result_tz= my_tz_OFFSET0;
  else
  {
    mysql_mutex_lock(&tz_LOCK);
    if (!(result_tz= (Time_zone_offset *)my_hash_search(
            &offset_tzs, (const uchar *)&offset, sizeof(long))))
    {
      if (!(result_tz= new (&tz_storage) Time_zone_offset(offset)) ||
          my_hash_insert(&offset_tzs, (const uchar *)result_tz))
      {
        result_tz= 0;
        sql_print_error("Fatal error: Out of memory while setting time zone");
      }
    }
    mysql_mutex_unlock(&tz_LOCK);
  }

  if (thd && result_tz && result_tz != my_tz_SYSTEM && result_tz != my_tz_UTC)
    status_var_increment(thd->status_var.feature_timezone);

  DBUG_RETURN(result_tz);
}

void Time_zone::adjust_leap_second(MYSQL_TIME *t)
{
  if (t->second == 60 || t->second == 61)
    t->second= 59;
}

bool Time_zone::is_monotone_continuous_around(my_time_t sec) const
{
  const my_time_t width= 24 * 60 * 60;
  if (sec < width || sec > TIMESTAMP_MAX_VALUE - width)
    return false;

  MYSQL_TIME dtmin, dtmax;
  gmt_sec_to_TIME(&dtmin, sec - width);
  gmt_sec_to_TIME(&dtmax, sec + width);

  ulonglong seconds;
  ulong useconds;
  if (calc_time_diff(&dtmax, &dtmin, 1, &seconds, &useconds))
    return false;
  return seconds == (ulonglong) width * 2;
}

my_time_t
Time_zone_system::TIME_to_gmt_sec(const MYSQL_TIME *t, uint *error_code) const
{
  long not_used;
  return my_system_gmt_sec(t, &not_used, error_code);
}

void Time_zone_system::gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const
{
  struct tm tmp_tm;
  time_t tmp_t= (time_t)t;

  localtime_r(&tmp_t, &tmp_tm);
  localtime_to_TIME(tmp, &tmp_tm);
  tmp->time_type= MYSQL_TIMESTAMP_DATETIME;
  adjust_leap_second(tmp);
}

const String *Time_zone_system::get_name() const
{
  return &tz_SYSTEM_name;
}

void
Time_zone_system::get_timezone_information(struct my_tz *curr_tz,
                                           const MYSQL_TIME *local_TIME) const
{
  uint error;
  time_t time_sec= TIME_to_gmt_sec(local_TIME, &error);
  my_tzinfo(time_sec, curr_tz);
}

my_time_t
Time_zone_utc::TIME_to_gmt_sec(const MYSQL_TIME *, uint *error_code) const
{
  *error_code= ER_WARN_DATA_OUT_OF_RANGE;
  return 0;
}

void Time_zone_utc::gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const
{
  struct tm tmp_tm;
  time_t tmp_t= (time_t)t;

  gmtime_r(&tmp_t, &tmp_tm);
  localtime_to_TIME(tmp, &tmp_tm);
  tmp->time_type= MYSQL_TIMESTAMP_DATETIME;
  adjust_leap_second(tmp);
}

const String *Time_zone_utc::get_name() const
{
  return &tz_UTC_name;
}

void
Time_zone_utc::get_timezone_information(struct my_tz *curr_tz,
                                        const MYSQL_TIME *) const
{
  strmake_buf(curr_tz->abbreviation, "UTC");
  curr_tz->seconds_offset= 0;
}

Time_zone_offset::Time_zone_offset(long tz_offset_arg):
  offset(tz_offset_arg)
{
  const long positive_offset= offset < 0 ? -offset : offset;
  const uint hours= (uint)(positive_offset / SECS_PER_HOUR);
  const uint minutes= (uint)(positive_offset % SECS_PER_HOUR / SECS_PER_MIN);
  const size_t length= my_snprintf(name_buff, sizeof(name_buff), "%s%02d:%02d",
                                  offset >= 0 ? "+" : "-", hours, minutes);
  name.set(name_buff, length, &my_charset_latin1);
}

my_time_t
Time_zone_offset::TIME_to_gmt_sec(const MYSQL_TIME *t, uint *error_code) const
{
  my_int_time_t local_t;
  int shift= 0;

  if (!validate_timestamp_range(t))
  {
    *error_code= ER_WARN_DATA_OUT_OF_RANGE;
    return 0;
  }
  *error_code= 0;

  if (t->year == TIMESTAMP_MAX_YEAR && t->month == 1 && t->day > 4)
    shift= 2;

  local_t= sec_since_epoch(t->year, t->month, t->day - shift, t->hour,
                           t->minute, t->second) - offset;
  if (shift)
    local_t+= shift * SECS_PER_DAY;

  if (local_t >= TIMESTAMP_MIN_VALUE && local_t <= TIMESTAMP_MAX_VALUE)
    return (my_time_t)local_t;

  *error_code= ER_WARN_DATA_OUT_OF_RANGE;
  return 0;
}

void Time_zone_offset::gmt_sec_to_TIME(MYSQL_TIME *tmp, my_time_t t) const
{
  sec_to_TIME(tmp, t, offset);
}

const String *Time_zone_offset::get_name() const
{
  return &name;
}

void
Time_zone_offset::get_timezone_information(struct my_tz *curr_tz,
                                           const MYSQL_TIME *) const
{
  curr_tz->seconds_offset= offset;
  strmake_buf(curr_tz->abbreviation, get_name()->ptr());
}

static bool time_zone_name_eq(const String *name, const char *literal,
                              size_t length)
{
  return name->length() == length &&
         my_strnncoll(&my_charset_latin1,
                      (const uchar *)name->ptr(), name->length(),
                      (const uchar *)literal, length) == 0;
}

static my_bool str_to_offset(const char *str, uint length, long *offset)
{
  const char *end= str + length;
  my_bool negative;
  ulong number_tmp;
  long offset_tmp;

  if (length < 4)
    return 1;

  if (*str == '+')
    negative= 0;
  else if (*str == '-')
    negative= 1;
  else
    return 1;
  str++;

  number_tmp= 0;
  while (str < end && my_isdigit(&my_charset_latin1, *str))
  {
    number_tmp= number_tmp * 10 + *str - '0';
    str++;
  }

  if (str + 1 >= end || *str != ':')
    return 1;
  str++;

  offset_tmp= number_tmp * MINS_PER_HOUR;
  number_tmp= 0;

  while (str < end && my_isdigit(&my_charset_latin1, *str))
  {
    number_tmp= number_tmp * 10 + *str - '0';
    str++;
  }

  if (str != end)
    return 1;

  offset_tmp= (offset_tmp + number_tmp) * SECS_PER_MIN;
  if (negative)
    offset_tmp= -offset_tmp;

  if (number_tmp > 59 || offset_tmp < -13 * SECS_PER_HOUR + 1 ||
      offset_tmp > 13 * SECS_PER_HOUR)
    return 1;

  *offset= offset_tmp;
  return 0;
}

static const uchar *my_offset_tzs_get_key(const void *entry_, size_t *length,
                                          my_bool)
{
  const Time_zone_offset *entry= static_cast<const Time_zone_offset *>(entry_);
  *length= sizeof(long);
  return reinterpret_cast<const uchar *>(&entry->offset);
}

static my_int_time_t sec_since_epoch(int year, int mon, int mday, int hour,
                                     int min, int sec)
{
  const long days_at_epoch= calc_daynr(1970, 1, 1);
  const long days= calc_daynr((uint)year, (uint)mon, (uint)mday) -
                   days_at_epoch;
  return (my_int_time_t)days * SECS_PER_DAY + hour * SECS_PER_HOUR +
         min * SECS_PER_MIN + sec;
}

static void sec_to_TIME(MYSQL_TIME *tmp, my_time_t t, long offset)
{
  const my_time_t shifted= t + offset;
  time_t tmp_t= (time_t)shifted;
  struct tm tmp_tm;

  gmtime_r(&tmp_t, &tmp_tm);
  localtime_to_TIME(tmp, &tmp_tm);
  tmp->time_type= MYSQL_TIMESTAMP_DATETIME;
}
