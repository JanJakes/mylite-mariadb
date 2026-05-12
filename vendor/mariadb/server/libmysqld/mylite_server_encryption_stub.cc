/* Copyright (c) 2026, MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "mariadb.h"
#include <mysql/service_encryption.h>

extern "C" {

static uint mylite_no_key(uint)
{
  return ENCRYPTION_KEY_VERSION_INVALID;
}

static uint mylite_no_get_key(uint, uint, uchar *, uint *)
{
  return ENCRYPTION_KEY_VERSION_INVALID;
}

static uint mylite_zero_size(uint, uint)
{
  return 0;
}

static int mylite_disabled_ctx_init(void *, const uchar *, uint,
                                    const uchar *, uint, int, uint, uint)
{
  return 1;
}

static int mylite_disabled_ctx_update(void *, const uchar *, uint,
                                      uchar *, uint *)
{
  return 1;
}

static int mylite_disabled_ctx_finish(void *, uchar *, uint *)
{
  return 1;
}

static uint mylite_encrypted_length(uint slen, uint, uint)
{
  return slen;
}

} /* extern "C" */

struct encryption_service_st encryption_handler=
{
  mylite_no_key,
  mylite_no_get_key,
  mylite_zero_size,
  mylite_disabled_ctx_init,
  mylite_disabled_ctx_update,
  mylite_disabled_ctx_finish,
  mylite_encrypted_length
};

int initialize_encryption_plugin(void *)
{
  return 1;
}

int finalize_encryption_plugin(void *)
{
  return 0;
}

int init_io_cache_encryption()
{
  return 0;
}
