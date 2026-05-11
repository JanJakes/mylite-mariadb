/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mylite.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

struct SmokeOptions
{
  std::string database;
  std::string report;
};

struct CaseResult
{
  std::string label;
  int expected= MYLITE_OK;
  int actual= MYLITE_OK;
  int errcode= MYLITE_OK;
  int extended_errcode= MYLITE_OK;
  unsigned mariadb_errno= 0;
  std::string sqlstate;
  std::string message;
  bool passed= false;
};

struct SmokeResult
{
  int status= 1;
  std::string phase;
  std::string message;
  std::vector<CaseResult> cases;
};

static bool parse_options(int argc, char **argv, SmokeOptions *options,
                          std::string *error);
static int run_smoke(const SmokeOptions &options, SmokeResult *result);
static bool check_close_null(SmokeResult *result);
static bool check_null_out_db(const SmokeOptions &options, SmokeResult *result);
static bool check_null_filename(SmokeResult *result);
static bool check_readonly_missing(const SmokeOptions &options,
                                   SmokeResult *result);
static bool check_bad_profile(const SmokeOptions &options,
                              SmokeResult *result);
static bool check_bad_flags(const SmokeOptions &options, SmokeResult *result);
static bool check_default_open_close(const SmokeOptions &options,
                                     SmokeResult *result);
static bool check_repeated_open_close(const SmokeOptions &options,
                                      SmokeResult *result);
static bool check_same_path_multi_handle(const SmokeOptions &options,
                                         SmokeResult *result);
static bool check_different_path_busy(const SmokeOptions &options,
                                      SmokeResult *result);
static bool record_result(SmokeResult *result, const char *label, int expected,
                          int actual, mylite_db *db);
static void write_report(const SmokeOptions &options,
                         const SmokeResult &result);
static bool option_value(const char *arg, const char *name, std::string *value);
static bool require_option(const std::string &value, const char *name,
                           std::string *error);

int main(int argc, char **argv)
{
  SmokeOptions options;
  std::string error;
  if (!parse_options(argc, argv, &options, &error))
  {
    std::cerr << error << std::endl;
    return 2;
  }

  SmokeResult result;
  const int status= run_smoke(options, &result);
  write_report(options, result);
  return status;
}

static bool parse_options(int argc, char **argv, SmokeOptions *options,
                          std::string *error)
{
  for (int i= 1; i < argc; ++i)
  {
    std::string value;
    if (option_value(argv[i], "--database=", &value))
      options->database= value;
    else if (option_value(argv[i], "--report=", &value))
      options->report= value;
    else
    {
      *error= std::string("unknown argument: ") + argv[i];
      return false;
    }
  }

  return require_option(options->database, "--database", error) &&
         require_option(options->report, "--report", error);
}

static int run_smoke(const SmokeOptions &options, SmokeResult *result)
{
  bool ok= true;

  result->phase= "close_null";
  ok= check_close_null(result) && ok;

  result->phase= "null_out_db";
  ok= check_null_out_db(options, result) && ok;

  result->phase= "null_filename";
  ok= check_null_filename(result) && ok;

  result->phase= "readonly_missing";
  ok= check_readonly_missing(options, result) && ok;

  result->phase= "bad_profile";
  ok= check_bad_profile(options, result) && ok;

  result->phase= "bad_flags";
  ok= check_bad_flags(options, result) && ok;

  result->phase= "default_open_close";
  ok= check_default_open_close(options, result) && ok;

  result->phase= "repeated_open_close";
  ok= check_repeated_open_close(options, result) && ok;

  result->phase= "same_path_multi_handle";
  ok= check_same_path_multi_handle(options, result) && ok;

  result->phase= "different_path_busy";
  ok= check_different_path_busy(options, result) && ok;

  if (!ok)
  {
    result->message= "open/close smoke failed";
    return result->status;
  }

  result->phase= "complete";
  result->status= 0;
  result->message= "ok";
  return result->status;
}

static bool check_close_null(SmokeResult *result)
{
  const int rc= mylite_close(nullptr);
  return record_result(result, "close_null", MYLITE_OK, rc, nullptr);
}

static bool check_null_out_db(const SmokeOptions &options, SmokeResult *result)
{
  const int rc= mylite_open(options.database.c_str(), nullptr);
  return record_result(result, "null_out_db", MYLITE_MISUSE, rc, nullptr);
}

static bool check_null_filename(SmokeResult *result)
{
  mylite_db *db= nullptr;
  const int rc= mylite_open(nullptr, &db);
  const bool ok= record_result(result, "null_filename", MYLITE_MISUSE, rc, db);
  mylite_close(db);
  return ok;
}

static bool check_readonly_missing(const SmokeOptions &options,
                                   SmokeResult *result)
{
  const std::string missing= options.database + ".missing";
  unlink(missing.c_str());

  mylite_db *db= nullptr;
  const int rc= mylite_open_v2(missing.c_str(), &db, MYLITE_OPEN_READONLY,
                               nullptr);
  const bool ok= record_result(result, "readonly_missing", MYLITE_CANTOPEN,
                               rc, db);
  mylite_close(db);
  return ok;
}

static bool check_bad_profile(const SmokeOptions &options,
                              SmokeResult *result)
{
  mylite_db *db= nullptr;
  const int rc= mylite_open_v2(options.database.c_str(), &db,
                               MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE,
                               "strict");
  const bool ok= record_result(result, "bad_profile", MYLITE_MISUSE, rc, db);
  mylite_close(db);
  return ok;
}

static bool check_bad_flags(const SmokeOptions &options, SmokeResult *result)
{
  mylite_db *db= nullptr;
  const int rc= mylite_open_v2(options.database.c_str(), &db,
                               MYLITE_OPEN_READONLY | MYLITE_OPEN_READWRITE,
                               nullptr);
  const bool ok= record_result(result, "bad_flags", MYLITE_MISUSE, rc, db);
  mylite_close(db);
  return ok;
}

static bool check_default_open_close(const SmokeOptions &options,
                                     SmokeResult *result)
{
  mylite_db *db= nullptr;
  int rc= mylite_open(options.database.c_str(), &db);
  bool ok= record_result(result, "default_open", MYLITE_OK, rc, db);
  if (db)
  {
    rc= mylite_close(db);
    ok= record_result(result, "default_close", MYLITE_OK, rc, nullptr) && ok;
  }
  return ok;
}

static bool check_repeated_open_close(const SmokeOptions &options,
                                      SmokeResult *result)
{
  bool ok= true;
  for (int i= 0; i < 2; ++i)
  {
    mylite_db *db= nullptr;
    int rc= mylite_open(options.database.c_str(), &db);
    const std::string open_label= std::string("repeated_open_") +
                                  static_cast<char>('1' + i);
    ok= record_result(result, open_label.c_str(), MYLITE_OK, rc, db) && ok;
    if (db)
    {
      rc= mylite_close(db);
      const std::string close_label= std::string("repeated_close_") +
                                     static_cast<char>('1' + i);
      ok= record_result(result, close_label.c_str(), MYLITE_OK, rc,
                        nullptr) && ok;
    }
  }
  return ok;
}

static bool check_same_path_multi_handle(const SmokeOptions &options,
                                         SmokeResult *result)
{
  mylite_db *first= nullptr;
  mylite_db *second= nullptr;

  int rc= mylite_open(options.database.c_str(), &first);
  bool ok= record_result(result, "same_path_first_open", MYLITE_OK, rc,
                         first);

  rc= mylite_open(options.database.c_str(), &second);
  ok= record_result(result, "same_path_second_open", MYLITE_OK, rc,
                    second) && ok;

  if (second)
  {
    rc= mylite_close(second);
    ok= record_result(result, "same_path_second_close", MYLITE_OK, rc,
                      nullptr) && ok;
  }
  if (first)
  {
    rc= mylite_close(first);
    ok= record_result(result, "same_path_first_close", MYLITE_OK, rc,
                      nullptr) && ok;
  }
  return ok;
}

static bool check_different_path_busy(const SmokeOptions &options,
                                      SmokeResult *result)
{
  mylite_db *first= nullptr;
  mylite_db *second= nullptr;
  const std::string other= options.database + ".other";

  int rc= mylite_open(options.database.c_str(), &first);
  bool ok= record_result(result, "busy_first_open", MYLITE_OK, rc, first);

  rc= mylite_open(other.c_str(), &second);
  ok= record_result(result, "busy_second_open", MYLITE_BUSY, rc, second) &&
      ok;

  mylite_close(second);
  if (first)
  {
    rc= mylite_close(first);
    ok= record_result(result, "busy_first_close", MYLITE_OK, rc, nullptr) &&
        ok;
  }
  return ok;
}

static bool record_result(SmokeResult *result, const char *label, int expected,
                          int actual, mylite_db *db)
{
  CaseResult case_result;
  case_result.label= label;
  case_result.expected= expected;
  case_result.actual= actual;
  if (db)
  {
    case_result.errcode= mylite_errcode(db);
    case_result.extended_errcode= mylite_extended_errcode(db);
    case_result.mariadb_errno= mylite_mariadb_errno(db);
    case_result.sqlstate= mylite_sqlstate(db);
    case_result.message= mylite_errmsg(db);
  }
  else if (actual == MYLITE_OK)
  {
    case_result.errcode= MYLITE_OK;
    case_result.extended_errcode= MYLITE_OK;
    case_result.sqlstate= "00000";
    case_result.message= "not an error";
  }
  else
  {
    case_result.errcode= mylite_errcode(nullptr);
    case_result.extended_errcode= mylite_extended_errcode(nullptr);
    case_result.mariadb_errno= mylite_mariadb_errno(nullptr);
    case_result.sqlstate= mylite_sqlstate(nullptr);
    case_result.message= mylite_errmsg(nullptr);
  }
  case_result.passed= expected == actual;
  result->cases.push_back(case_result);
  return case_result.passed;
}

static void write_report(const SmokeOptions &options,
                         const SmokeResult &result)
{
  std::ofstream report(options.report.c_str());
  if (!report)
  {
    std::cerr << "could not open report: " << options.report << std::endl;
    return;
  }

  report << "# MyLite Open Close Smoke Report\n\n";
  report << "database=" << options.database << "\n\n";
  report << "## Result\n\n";
  report << "status=" << result.status << "\n";
  report << "phase=" << result.phase << "\n";
  report << "message=" << result.message << "\n\n";

  report << "## Cases\n\n";
  for (const CaseResult &case_result : result.cases)
  {
    report << "label=" << case_result.label << "\n";
    report << "passed=" << (case_result.passed ? "yes" : "no") << "\n";
    report << "expected=" << case_result.expected << "\n";
    report << "actual=" << case_result.actual << "\n";
    report << "errcode=" << case_result.errcode << "\n";
    report << "extended_errcode=" << case_result.extended_errcode << "\n";
    report << "mariadb_errno=" << case_result.mariadb_errno << "\n";
    report << "sqlstate=" << case_result.sqlstate << "\n";
    report << "message=" << case_result.message << "\n\n";
  }
}

static bool option_value(const char *arg, const char *name, std::string *value)
{
  const size_t name_len= std::strlen(name);
  if (std::strncmp(arg, name, name_len) != 0)
    return false;
  *value= arg + name_len;
  return true;
}

static bool require_option(const std::string &value, const char *name,
                           std::string *error)
{
  if (!value.empty())
    return true;
  *error= std::string("missing required option: ") + name;
  return false;
}
