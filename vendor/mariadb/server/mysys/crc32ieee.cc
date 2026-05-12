/* Copyright (c) 2020, 2022, MariaDB

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


#include <my_global.h>
#include <my_sys.h>
#ifndef MYLITE_DISABLE_ZLIB_COMPRESSION
#include <zlib.h>
#endif

#ifdef MYLITE_DISABLE_ZLIB_COMPRESSION
static unsigned int my_crc32_software(unsigned int crc, const void *data,
                                      size_t len)
{
  const unsigned char *pos= static_cast<const unsigned char *>(data);
  crc= ~crc;
  while (len--)
  {
    crc^= *pos++;
    for (uint bit= 0; bit < 8; bit++)
      crc= (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
  }
  return ~crc;
}
#else
/* TODO: remove this once zlib adds inherent support for hardware accelerated
crc32 for all architectures. */
static unsigned int my_crc32_zlib(unsigned int crc, const void *data,
                                  size_t len)
{
  return (unsigned int) crc32(crc, (const Bytef *)data, (unsigned int) len);
}
#endif

typedef unsigned int (*my_crc32_t)(unsigned int, const void *, size_t);

#if defined _M_IX86 || defined _M_X64 || defined __i386__ || defined __x86_64__
extern "C" my_crc32_t crc32_pclmul_enabled();
#elif defined HAVE_ARMV8_CRC
extern "C" int crc32_aarch64_available();
extern "C" unsigned int crc32_aarch64(unsigned int, const void *, size_t);
#endif


static my_crc32_t init_crc32()
{
#if defined _M_IX86 || defined _M_X64 || defined __i386__ || defined __x86_64__
  if (my_crc32_t crc= crc32_pclmul_enabled())
    return crc;
#elif defined HAVE_ARMV8_CRC
  if (crc32_aarch64_available())
    return crc32_aarch64;
#endif
#ifdef MYLITE_DISABLE_ZLIB_COMPRESSION
  return my_crc32_software;
#else
  return my_crc32_zlib;
#endif
}

static const my_crc32_t my_checksum_func= init_crc32();

#ifdef __powerpc64__
# error "my_checksum() is defined in mysys/crc32/crc32_ppc64.c"
#endif
extern "C"
uint32 my_checksum(uint32 crc, const void *data, size_t len)
{
  return my_checksum_func(crc, data, len);
}
