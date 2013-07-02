/************************************************************************
 *   IRC - Internet Relay Chat, ircd/ssl.c
 *   Copyright (C) 2002 Alex Badea <vampire@go.ro>
 *   Copyright (C) 2013 Matthew Beeching (Jobe)
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief Implimentation of common SSL functions.
 * @version $Id:$
 */
#include "config.h"
#include "client.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "listener.h"
#include "s_bsd.h"
#include "s_debug.h"
#include "send.h"
#include "ssl.h"

#ifdef USE_SSL

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/uio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif /* IOV_MAX */

struct ssl_data {
  struct Socket socket;
  struct Listener *listener;
  int fd;
};

SSL_CTX *ssl_server_ctx;
SSL_CTX *ssl_client_ctx;

SSL_CTX *ssl_init_server_ctx();
SSL_CTX *ssl_init_client_ctx();
int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *cert);
void ssl_set_nonblocking(SSL *s);
int ssl_smart_shutdown(SSL *ssl);
char *ssl_error_str(int err, int my_errno);
void sslfail(char *txt);
void binary_to_hex(unsigned char *bin, char *hex, int length);

int ssl_init(void)
{
  SSL_library_init();
  SSL_load_error_strings();

  Debug((DEBUG_NOTICE, "SSL: read %d bytes of randomness", RAND_load_file("/dev/urandom", 4096)));

  ssl_server_ctx = ssl_init_server_ctx();
  if (!ssl_server_ctx)
    return -1;
  ssl_client_ctx = ssl_init_client_ctx();
  if (!ssl_client_ctx)
    return -1;

  return 0;
}

int ssl_reinit(void)
{
  SSL_CTX *temp_ctx;

  /* Attempt to reinitialize server context, return on error */
  temp_ctx = ssl_init_server_ctx();
  if (!temp_ctx)
    return -1;

  /* Now reinitialize server context for real. */
  SSL_CTX_free(temp_ctx);
  SSL_CTX_free(ssl_server_ctx);
  ssl_server_ctx = ssl_init_server_ctx();

  /* Attempt to reinitialize client context, return on error */
  temp_ctx = ssl_init_client_ctx();
  if (!temp_ctx)
    return -1;

  /* Now reinitialize client context for real. */
  SSL_CTX_free(temp_ctx);
  SSL_CTX_free(ssl_client_ctx);
  ssl_client_ctx = ssl_init_client_ctx();

  return 0;
}

SSL_CTX *ssl_init_server_ctx(void)
{
  SSL_CTX *server_ctx = NULL;
  int vrfyopts = SSL_VERIFY_PEER|SSL_VERIFY_CLIENT_ONCE;

  server_ctx = SSL_CTX_new(SSLv23_server_method());
  if (!server_ctx)
  {
    sslfail("Error creating new server context");
    return NULL;
  }

  if (feature_bool(FEAT_SSL_REQUIRECLIENTCERT))
    vrfyopts |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

  /* SSL_CTX_set_options(server_ctx, SSL_OP_NO_SSLv2); */
  SSL_CTX_set_verify(server_ctx, vrfyopts, ssl_verify_callback);
  SSL_CTX_set_session_cache_mode(server_ctx, SSL_SESS_CACHE_OFF);

  if (SSL_CTX_use_certificate_chain_file(server_ctx, feature_str(FEAT_SSL_CERTFILE)) <= 0)
  {
    sslfail("Error loading SSL certificate for server context");
    SSL_CTX_free(server_ctx);
    return NULL;
  }
  if (SSL_CTX_use_PrivateKey_file(server_ctx, feature_str(FEAT_SSL_KEYFILE), SSL_FILETYPE_PEM) <= 0)
  {
    sslfail("Error loading SSL key for server context");
    SSL_CTX_free(server_ctx);
    return NULL;
  }

  if (!SSL_CTX_check_private_key(server_ctx))
  {
    sslfail("Error checking SSL private key for server context");
    SSL_CTX_free(server_ctx);
    return NULL;
  }

  if (!EmptyString(feature_str(FEAT_SSL_CACERTFILE)))
  {
    if (!SSL_CTX_load_verify_locations(server_ctx, feature_str(FEAT_SSL_CACERTFILE), NULL))
    {
      sslfail("Error loading trusted CA certificates file for server context");
      SSL_CTX_free(server_ctx);
      return NULL;
    }
  }

  return server_ctx;
}

SSL_CTX *ssl_init_client_ctx(void)
{
  SSL_CTX *client_ctx = NULL;

  client_ctx = SSL_CTX_new(SSLv3_client_method());
  if (!client_ctx)
  {
    sslfail("Error creating new client context");
    return NULL;
  }

  SSL_CTX_set_session_cache_mode(client_ctx, SSL_SESS_CACHE_OFF);

  if (SSL_CTX_use_certificate_chain_file(client_ctx, feature_str(FEAT_SSL_CERTFILE)) <= 0)
  {
    sslfail("Error loading SSL certificate for client context");
    SSL_CTX_free(client_ctx);
    return NULL;
  }
  if (SSL_CTX_use_PrivateKey_file(client_ctx, feature_str(FEAT_SSL_KEYFILE), SSL_FILETYPE_PEM) <= 0)
  {
    sslfail("Error loading SSL key for client context");
    SSL_CTX_free(client_ctx);
    return NULL;
  }

  if (!SSL_CTX_check_private_key(client_ctx))
  {
    sslfail("Error checking SSL private key for client context");
    SSL_CTX_free(client_ctx);
    return NULL;
  }

  return client_ctx;
}

int ssl_verify_callback(int preverify_ok, X509_STORE_CTX *cert)
{
  int err = 0;

  err = X509_STORE_CTX_get_error(cert);

  if (feature_bool(FEAT_SSL_NOSELFSIGNED) &&
      (err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT))
    return 0;

  if (feature_bool(FEAT_SSL_VERIFYCERT))
  {
    if (!feature_bool(FEAT_SSL_NOSELFSIGNED) &&
        (err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT))
      return 1;
    return preverify_ok;
  }

  return 1;
}

static void ssl_abort(struct ssl_data *data)
{
  Debug((DEBUG_DEBUG, "SSL: aborted"));
  SSL_free(data->socket.ssl);
  close(data->fd);
  socket_del(&data->socket);
}

static void ssl_accept(struct ssl_data *data)
{
  const char* const error_ssl = "ERROR :SSL connection error\r\n";

  if (SSL_accept(data->socket.ssl) <= 0) {
    unsigned long err = ERR_get_error();
    char string[120];

    if (err) {
      ERR_error_string(err, string);
      Debug((DEBUG_ERROR, "SSL_accept: %s", string));

      write(data->fd, error_ssl, strlen(error_ssl));

      ssl_abort(data);
    }
    return;
  }
  if (SSL_is_init_finished(data->socket.ssl)) {
    socket_del(&data->socket);
    add_connection(data->listener, data->fd, data->socket.ssl);
  }
}

static void ssl_sock_callback(struct Event* ev)
{
  struct ssl_data *data;

  assert(0 != ev_socket(ev));
  assert(0 != s_data(ev_socket(ev)));

  data = s_data(ev_socket(ev));
  assert(0 != data);

  switch (ev_type(ev)) {
  case ET_DESTROY:
    --data->listener->ref_count;
    MyFree(data);
    return;
  case ET_ERROR:
  case ET_EOF:
    ssl_abort(data);
    break;
  case ET_READ:
  case ET_WRITE:
    ssl_accept(data);
    break;
  default:
    break;
  }
}

void ssl_add_connection(struct Listener *listener, int fd)
{
  struct ssl_data *data;

  assert(0 != listener);

  if (!os_set_nonblocking(fd)) {
    close(fd);
    return;
  }
  os_disable_options(fd);

  data = (struct ssl_data *)MyMalloc(sizeof(struct ssl_data));
  data->listener = listener;
  data->fd = fd;
  if (!socket_add(&data->socket, ssl_sock_callback, (void *) data, SS_CONNECTED, SOCK_EVENT_READABLE, fd)) {
    close(fd);
    return;
  }
  if (!(data->socket.ssl = SSL_new(ssl_server_ctx))) {
    Debug((DEBUG_DEBUG, "SSL_new failed"));
    close(fd);
    return;
  }
  SSL_set_fd(data->socket.ssl, fd);
  ssl_set_nonblocking(data->socket.ssl);
  ++listener->ref_count;
}

/*
 * ssl_recv - non blocking read of a connection
 * returns:
 *  1  if data was read or socket is blocked (recoverable error)
 *    count_out > 0 if data was read
 *
 *  0  if socket closed from other end
 *  -1 if an unrecoverable error occurred
 */
IOResult ssl_recv(struct Socket *socketh, char* buf,
                 unsigned int length, unsigned int* count_out)
{
  int res;

  assert(0 != socketh);
  assert(0 != buf);
  assert(0 != count_out);

  *count_out = 0;
  errno = 0;

  res = SSL_read(socketh->ssl, buf, length);
  switch (SSL_get_error(socketh->ssl, res)) {
  case SSL_ERROR_NONE:
    *count_out = (unsigned) res;
    return IO_SUCCESS;
  case SSL_ERROR_WANT_WRITE:
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_X509_LOOKUP:
    Debug((DEBUG_DEBUG, "SSL_read returned WANT_ - retrying"));
    return IO_BLOCKED;
  case SSL_ERROR_SYSCALL:
    if (res < 0 && errno == EINTR)
      return IO_BLOCKED; /* ??? */
    break;
  case SSL_ERROR_ZERO_RETURN: /* close_notify received */
    SSL_shutdown(socketh->ssl); /* Send close_notify back */
    break;
  }
  return IO_FAILURE;
}

/*
 * ssl_sendv - non blocking writev to a connection
 * returns:
 *  1  if data was written
 *    count_out contains amount written
 *
 *  0  if write call blocked, recoverable error
 *  -1 if an unrecoverable error occurred
 */
IOResult ssl_sendv(struct Socket *socketh, struct MsgQ* buf,
                  unsigned int* count_in, unsigned int* count_out)
{
  int res;
  int count;
  int k;
  struct iovec iov[IOV_MAX];
  IOResult retval = IO_BLOCKED;
  int ssl_err = 0;

  errno = 0;

  assert(0 != socketh);
  assert(0 != buf);
  assert(0 != count_in);
  assert(0 != count_out);

  *count_in = 0;
  *count_out = 0;

  count = msgq_mapiov(buf, iov, IOV_MAX, count_in);
  for (k = 0; k < count; k++) {
    res = SSL_write(socketh->ssl, iov[k].iov_base, iov[k].iov_len);
    ssl_err = SSL_get_error(socketh->ssl, res);
    Debug((DEBUG_DEBUG, "SSL_write returned %d, error code %d.", res, ssl_err));
    switch (ssl_err) {
    case SSL_ERROR_NONE:
      *count_out += (unsigned) res;
      retval = IO_SUCCESS;
      break;
    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_X509_LOOKUP:
      Debug((DEBUG_DEBUG, "SSL_write returned want WRITE, READ, or X509; returning retval %d", retval));
      return retval;
    case SSL_ERROR_SSL:
      {
          int errorValue;
          Debug((DEBUG_ERROR, "SSL_write returned SSL_ERROR_SSL, errno %d, retval %d, res %d, ssl error code %d", errno, retval, res, ssl_err));
          ERR_load_crypto_strings();
          while((errorValue = ERR_get_error())) {
            Debug((DEBUG_ERROR, "  Error Queue: %d -- %s", errorValue, ERR_error_string(errorValue, NULL)));
          }
          return IO_FAILURE;
       }
    case SSL_ERROR_SYSCALL:
      if(res < 0 && (errno == EWOULDBLOCK ||
                     errno == EINTR ||
                     errno == EBUSY ||
                     errno == EAGAIN)) {
             Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SYSCALL, errno %d - returning retval %d", errno, retval));
             return retval;
      }
      else {
             Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SYSCALL - errno %d - returning IO_FAILURE", errno));
             return IO_FAILURE;
      }
      /*
      if(errno == EAGAIN) * its what unreal ircd does..*
      {
          Debug((DEBUG_DEBUG, "SSL_write returned ERROR_SSL - errno %d returning retval %d", errno, retval));
          return retval;
      }
      */
    case SSL_ERROR_ZERO_RETURN:
      SSL_shutdown(socketh->ssl);
      return IO_FAILURE;
    default:
      Debug((DEBUG_DEBUG, "SSL_write return fell through - errno %d returning retval %d", errno, retval));
      return retval; /* unknown error, assume block or success*/
    }
  }
  Debug((DEBUG_DEBUG, "SSL_write return fell through(2) - errno %d returning retval %d", errno, retval));
  return retval;
}

int ssl_send(struct Client *cptr, const char *buf, unsigned int len)
{
  char fmt[16];

  if (!cli_socket(cptr).ssl)
    return write(cli_fd(cptr), buf, len);

  /*
   * XXX HACK
   *
   * Incomplete SSL writes must be retried with the same write buffer;
   * at this point SSL_write usually fails, so the data must be queued.
   * We're abusing the normal send queue for this.
   * Also strip \r\n from message, as sendrawto_one adds it later
   * this hack sucks. it conflicted with prority queues - caused random
   * ssl disconnections for YEARS. In summery, this hack == bad. I may
   * have solved that, but this still makes me nervous.
   */
  ircd_snprintf(0, fmt, sizeof(fmt), "%%.%us", len - 2);
  sendrawto_one(cptr, fmt, buf);
  send_queued(cptr);
  return len;
}

int ssl_murder(void *ssl, int fd, const char *buf)
{
  if (!ssl) {
    write(fd, buf, strlen(buf));
  } else {
    SSL_write((SSL *) ssl, buf, strlen(buf));
    SSL_free((SSL *) ssl);
  }
  close(fd);
  return 0;
}

void ssl_free(struct Socket *socketh)
{
  if (!socketh->ssl)
    return;
  SSL_free(socketh->ssl);
}

char *ssl_get_cipher(SSL *ssl)
{
  static char buf[400];
  int bits;
  const SSL_CIPHER *c;

  buf[0] = '\0';
  strcpy(buf, SSL_get_version(ssl));
  strcat(buf, "-");
  strcat(buf, SSL_get_cipher(ssl));
  c = SSL_get_current_cipher(ssl);
  SSL_CIPHER_get_bits(c, &bits);
  strcat(buf, "-");
  strcat(buf, (char *)itoa(bits));
  strcat(buf, "bits");
  return (buf);
}

int ssl_connect(struct Socket* sock)
{
  int r = 0;

  if (!sock->ssl) {
    sock->ssl = SSL_new(ssl_client_ctx);
    SSL_set_fd(sock->ssl, sock->s_fd);
    ssl_set_nonblocking(sock->ssl);
  }

  r = SSL_connect(sock->ssl);
  if (r<=0) {
    if ((SSL_get_error(sock->ssl, r) == SSL_ERROR_WANT_WRITE) || (SSL_get_error(sock->ssl, r) == SSL_ERROR_WANT_READ))
      return 0; /* Needs to call SSL_connect() again */
    else
      return -1; /* Fatal error */
  }
  return 1; /* Connection complete */
}

int ssl_starttls(struct Client *cptr)
{
  int r = 0;

  if (!cli_socket(cptr).ssl) {
    cli_socket(cptr).ssl = SSL_new(ssl_server_ctx);
    SSL_set_fd(cli_socket(cptr).ssl, cli_socket(cptr).s_fd);
    ssl_set_nonblocking(cli_socket(cptr).ssl);
  }

  if ((r = SSL_accept(cli_socket(cptr).ssl)) <= 0) {
    unsigned long err = SSL_get_error(cli_socket(cptr).ssl, r);

    if (err) {
      switch (err) {
        case SSL_ERROR_SYSCALL:
          if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return 1;
        default:
          Debug((DEBUG_ERROR, "SSL_accept: %s", ssl_error_str(err, errno)));

          SSL_set_shutdown(cli_socket(cptr).ssl, SSL_RECEIVED_SHUTDOWN);
          ssl_smart_shutdown(cli_socket(cptr).ssl);
          SSL_free(cli_socket(cptr).ssl);
          cli_socket(cptr).ssl = NULL;

          return 0;
      }
      return 0;
    }
    return 1;
  }

  return 1;
}

char* ssl_get_fingerprint(SSL *ssl)
{
  X509* cert;
  unsigned int n = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  const EVP_MD *digest = EVP_sha256();
  static char hex[BUFSIZE + 1];

  cert = SSL_get_peer_certificate(ssl);

  if (!(cert))
    return NULL;

  if (!X509_digest(cert, digest, md, &n))
  {
    X509_free(cert);
    return NULL;
  }

  binary_to_hex(md, hex, n);
  X509_free(cert);

  return (hex);
}

void ssl_set_nonblocking(SSL *s)
{
  BIO_set_nbio(SSL_get_rbio(s),1);
  BIO_set_nbio(SSL_get_wbio(s),1);
}

int ssl_is_init_finished(SSL *s)
{
  return SSL_is_init_finished(s);
}

int ssl_smart_shutdown(SSL *ssl)
{
    char i;
    int rc;
    rc = 0;
    for(i = 0; i < 4; i++) {
        if((rc = SSL_shutdown(ssl)))
            break;
    }

    return rc;
}

/**
 * Retrieve a static string for the given SSL error.
 *
 * \param err The error to look up.
 * \param my_errno The value of errno to use in case we want to call strerror().
 */
char *ssl_error_str(int err, int my_errno)
{
  static char ssl_errbuf[256];
  char *ssl_errstr = NULL;

  switch(err) {
    case SSL_ERROR_NONE:
      ssl_errstr = "SSL: No error";
      break;
    case SSL_ERROR_SSL:
      ssl_errstr = "Internal OpenSSL error or protocol error";
      break;
    case SSL_ERROR_WANT_READ:
      ssl_errstr = "OpenSSL functions requested a read()";
      break;
    case SSL_ERROR_WANT_WRITE:
      ssl_errstr = "OpenSSL functions requested a write()";
      break;
    case SSL_ERROR_WANT_X509_LOOKUP:
      ssl_errstr = "OpenSSL requested a X509 lookup which didn't arrive";
      break;
    case SSL_ERROR_SYSCALL:
      snprintf(ssl_errbuf, sizeof(ssl_errbuf), "%s", strerror(my_errno));
      ssl_errstr = ssl_errbuf;
      break;
    case SSL_ERROR_ZERO_RETURN:
      ssl_errstr = "Underlying socket operation returned zero";
      break;
    case SSL_ERROR_WANT_CONNECT:
      ssl_errstr = "OpenSSL functions wanted a connect()";
      break;
    default:
      ssl_errstr = "Unknown OpenSSL error (huh?)";
  }
  return ssl_errstr;
}

const char* ssl_get_verify_result(SSL *ssl)
{
  switch (SSL_get_verify_result(ssl)) {
    case X509_V_OK: return "ok"; break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: return "unable to get issuer"; break;
    case X509_V_ERR_UNABLE_TO_GET_CRL: return "unable to get certificate CRL"; break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE: return "unable to decrypt certificate's signature"; break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE: return "unable to decrypt CRL's signature"; break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY: return "unable to decode issuer public key"; break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE: return "certificate signature failure"; break;
    case X509_V_ERR_CRL_SIGNATURE_FAILURE: return "CRL signature failure"; break;
    case X509_V_ERR_CERT_NOT_YET_VALID: return "certificate is not yet valid"; break;
    case X509_V_ERR_CERT_HAS_EXPIRED: return "certificate has expired"; break;
    case X509_V_ERR_CRL_NOT_YET_VALID: return "CRL is not yet valid"; break;
    case X509_V_ERR_CRL_HAS_EXPIRED: return "CRL has expired"; break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD: return "format error in certificate's notBefore field"; break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD: return "format error in certificate's notAfter field"; break;
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD: return "format error in CRL's lastUpdate field"; break;
    case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD: return "format error in CRL's nextUpdate field"; break;
    case X509_V_ERR_OUT_OF_MEM: return "out of memory"; break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: return "self signed certificate"; break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: return "self signed certificate in certificate chain"; break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: return "unable to get local issuer certificate"; break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return "unable to verify the first certificate"; break;
    case X509_V_ERR_CERT_CHAIN_TOO_LONG: return "certificate chain too long"; break;
    case X509_V_ERR_CERT_REVOKED: return "certificate revoked"; break;
    case X509_V_ERR_INVALID_CA: return "invalid CA certificate"; break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED: return "path length constraint exceeded"; break;
    case X509_V_ERR_INVALID_PURPOSE: return "unsupported certificate purpose"; break;
    case X509_V_ERR_CERT_UNTRUSTED: return "certificate not trusted"; break;
    case X509_V_ERR_CERT_REJECTED: return "certificate rejected"; break;
    case X509_V_ERR_SUBJECT_ISSUER_MISMATCH: return "subject issuer mismatch"; break;
    case X509_V_ERR_AKID_SKID_MISMATCH: return "authority and subject key identifier mismatch"; break;
    case X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH: return "authority and issuer serial number mismatch"; break;
    case X509_V_ERR_KEYUSAGE_NO_CERTSIGN: return "key usage does not include certificate signing"; break;
    case X509_V_ERR_APPLICATION_VERIFICATION: return "application verification failure"; break;
    default: return "unknown"; break;
  }
}

void sslfail(char *txt)
{
  unsigned long err = ERR_get_error();
  char string[120];

  if (!err) {
    Debug((DEBUG_DEBUG, "%s: poof", txt));
  } else {
    ERR_error_string(err, string);
    Debug((DEBUG_FATAL, "%s: %s", txt, string));
  }
}

void binary_to_hex(unsigned char *bin, char *hex, int length)
{
  static const char trans[] = "0123456789ABCDEF";
  int i;

  for(i = 0; i < length; i++)
  {
    hex[i  << 1]      = trans[bin[i] >> 4];
    hex[(i << 1) + 1] = trans[bin[i] & 0xf];
  }

  hex[i << 1] = '\0';
}

#endif /* USE_SSL */

