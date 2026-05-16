/* Copyright (c) 2026, MyLite contributors.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef MYLITE_ZLIB_DISABLED_H_INCLUDED
#define MYLITE_ZLIB_DISABLED_H_INCLUDED

typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef void *voidpf;
typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void (*free_func)(voidpf opaque, voidpf address);

typedef struct z_stream_s
{
  Bytef *next_in;
  uInt avail_in;
  uLong total_in;

  Bytef *next_out;
  uInt avail_out;
  uLong total_out;

  char *msg;
  void *state;

  alloc_func zalloc;
  free_func zfree;
  voidpf opaque;

  int data_type;
  uLong adler;
  uLong reserved;
} z_stream;

typedef z_stream *z_streamp;

#define ZLIB_VERSION "disabled"
#define Z_NO_FLUSH 0
#define Z_SYNC_FLUSH 2
#define Z_FULL_FLUSH 3
#define Z_FINISH 4
#define Z_BLOCK 5
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_STREAM_ERROR (-2)
#define Z_BUF_ERROR (-5)
#define Z_DEFAULT_COMPRESSION (-1)
#define Z_DEFLATED 8
#define Z_DEFAULT_STRATEGY 0
#define MAX_WBITS 15
#define MAX_MEM_LEVEL 9
#define Z_NULL 0

static inline uLong compressBound(uLong sourceLen)
{
  (void) sourceLen;
  return 0;
}

static inline uLong adler32(uLong adler, const Bytef *buf, uInt len)
{
  uLong s1= adler & 0xffffU;
  uLong s2= (adler >> 16) & 0xffffU;

  if (!buf)
    return 1U;

  while (len--)
  {
    s1= (s1 + *buf++) % 65521U;
    s2= (s2 + s1) % 65521U;
  }

  return (s2 << 16) | s1;
}

static inline int deflateInit(z_streamp strm, int level)
{
  (void) strm;
  (void) level;
  return Z_STREAM_ERROR;
}

static inline int deflateInit2(z_streamp strm, int level, int method,
                               int windowBits, int memLevel, int strategy)
{
  (void) strm;
  (void) level;
  (void) method;
  (void) windowBits;
  (void) memLevel;
  (void) strategy;
  return Z_STREAM_ERROR;
}

static inline int deflate(z_streamp strm, int flush)
{
  (void) strm;
  (void) flush;
  return Z_STREAM_ERROR;
}

static inline int deflateEnd(z_streamp strm)
{
  (void) strm;
  return Z_STREAM_ERROR;
}

static inline int deflateReset(z_streamp strm)
{
  (void) strm;
  return Z_STREAM_ERROR;
}

static inline int inflateInit(z_streamp strm)
{
  (void) strm;
  return Z_STREAM_ERROR;
}

static inline int inflateInit2(z_streamp strm, int windowBits)
{
  (void) strm;
  (void) windowBits;
  return Z_STREAM_ERROR;
}

static inline int inflate(z_streamp strm, int flush)
{
  (void) strm;
  (void) flush;
  return Z_STREAM_ERROR;
}

static inline int inflateEnd(z_streamp strm)
{
  (void) strm;
  return Z_STREAM_ERROR;
}

#endif
