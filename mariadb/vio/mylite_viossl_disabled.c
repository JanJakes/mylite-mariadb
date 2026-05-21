/* Copyright (c) 2026 MyLite contributors

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License. */

#include "vio_priv.h"

#ifdef HAVE_OPENSSL

#include <errno.h>

#ifndef ENOTSUP
#define ENOTSUP EINVAL
#endif

static void mylite_vio_tls_set_unsupported_error(void);

const char *sslGetErrString(enum enum_ssl_init_error error __attribute__((unused)))
{
  return "VIO TLS transport is disabled in the MyLite embedded profile";
}

void vio_check_ssl_init()
{
}

size_t vio_ssl_read(Vio *vio __attribute__((unused)),
                    uchar *buf __attribute__((unused)),
                    size_t size __attribute__((unused)))
{
  mylite_vio_tls_set_unsupported_error();
  return (size_t) -1;
}

size_t vio_ssl_write(Vio *vio __attribute__((unused)),
                     const uchar *buf __attribute__((unused)),
                     size_t size __attribute__((unused)))
{
  mylite_vio_tls_set_unsupported_error();
  return (size_t) -1;
}

int vio_ssl_close(Vio *vio)
{
  return vio_close(vio);
}

void vio_ssl_delete(Vio *vio)
{
  if (!vio)
    return;
  vio_delete(vio);
}

int sslaccept(struct st_VioSSLFd *ptr __attribute__((unused)),
              Vio *vio __attribute__((unused)),
              long timeout __attribute__((unused)),
              unsigned long *errptr)
{
  mylite_vio_tls_set_unsupported_error();
  if (errptr)
    *errptr= ENOTSUP;
  return 1;
}

int sslconnect(struct st_VioSSLFd *ptr __attribute__((unused)),
               Vio *vio __attribute__((unused)),
               long timeout __attribute__((unused)),
               unsigned long *errptr)
{
  mylite_vio_tls_set_unsupported_error();
  if (errptr)
    *errptr= ENOTSUP;
  return 1;
}

int vio_ssl_blocking(Vio *vio __attribute__((unused)),
                     my_bool set_blocking_mode, my_bool *old_mode)
{
  if (old_mode)
    *old_mode= TRUE;
  return set_blocking_mode ? 0 : 1;
}

my_bool vio_ssl_has_data(Vio *vio __attribute__((unused)))
{
  return FALSE;
}

struct st_VioSSLFd *new_VioSSLConnectorFd(const char *key_file
                                          __attribute__((unused)),
                                          const char *cert_file
                                          __attribute__((unused)),
                                          const char *ca_file
                                          __attribute__((unused)),
                                          const char *ca_path
                                          __attribute__((unused)),
                                          const char *cipher
                                          __attribute__((unused)),
                                          enum enum_ssl_init_error *error,
                                          const char *crl_file
                                          __attribute__((unused)),
                                          const char *crl_path
                                          __attribute__((unused)))
{
  if (error)
    *error= SSL_INITERR_PROTOCOL;
  mylite_vio_tls_set_unsupported_error();
  return 0;
}

struct st_VioSSLFd *new_VioSSLAcceptorFd(const char *key_file
                                         __attribute__((unused)),
                                         const char *cert_file
                                         __attribute__((unused)),
                                         const char *ca_file
                                         __attribute__((unused)),
                                         const char *ca_path
                                         __attribute__((unused)),
                                         const char *cipher
                                         __attribute__((unused)),
                                         enum enum_ssl_init_error *error,
                                         const char *crl_file
                                         __attribute__((unused)),
                                         const char *crl_path
                                         __attribute__((unused)),
                                         ulonglong tls_version
                                         __attribute__((unused)))
{
  if (error)
    *error= SSL_INITERR_PROTOCOL;
  mylite_vio_tls_set_unsupported_error();
  return 0;
}

void free_vio_ssl_acceptor_fd(struct st_VioSSLFd *fd __attribute__((unused)))
{
}

static void mylite_vio_tls_set_unsupported_error(void)
{
  errno= ENOTSUP;
  my_errno= ENOTSUP;
}

#endif /* HAVE_OPENSSL */
