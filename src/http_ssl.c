/*
** Copyright (c) 2009 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)
**
** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@hwaci.com
**   http://www.hwaci.com/drh/
**
*******************************************************************************
**
** This file manages low-level SSL communications.
**
** This file implements a singleton.  A single SSL connection may be active
** at a time.  State information is stored in static variables.  The identity
** of the server is held in global variables that are set by url_parse().
**
** SSL support is abstracted out into this module because Fossil can
** be compiled without SSL support (which requires OpenSSL library)
*/

#include "config.h"
#include "http_ssl.h"

#ifdef FOSSIL_ENABLE_SSL

#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

#include <assert.h>
#include <sys/types.h>

/*
** There can only be a single OpenSSL IO connection open at a time.
** State information about that IO is stored in the following
** local variables:
*/
static int sslIsInit = 0;    /* True after global initialization */
static BIO *iBio = 0;        /* OpenSSL I/O abstraction */
static char *sslErrMsg = 0;  /* Text of most recent OpenSSL error */
static SSL_CTX *sslCtx;      /* SSL context */
static SSL *ssl;
static struct {              /* Accept this SSL cert for this session only */
  char *zHost;                  /* Subject or host name */
  char *zHash;                  /* SHA2-256 hash of the cert */
} sException;
static int sslNoCertVerify = 0;  /* Do not verify SSL certs */

/*
** Clear the SSL error message
*/
static void ssl_clear_errmsg(void){
  free(sslErrMsg);
  sslErrMsg = 0;
}

/*
** Set the SSL error message.
*/
void ssl_set_errmsg(const char *zFormat, ...){
  va_list ap;
  ssl_clear_errmsg();
  va_start(ap, zFormat);
  sslErrMsg = vmprintf(zFormat, ap);
  va_end(ap);
}

/*
** Return the current SSL error message
*/
const char *ssl_errmsg(void){
  return sslErrMsg;
}

/*
** When a server requests a client certificate that hasn't been provided,
** display a warning message explaining what to do next.
*/
static int ssl_client_cert_callback(SSL *ssl, X509 **x509, EVP_PKEY **pkey){
  fossil_warning("The remote server requested a client certificate for "
    "authentication. Specify the pathname to a file containing the PEM "
    "encoded certificate and private key with the --ssl-identity option "
    "or the ssl-identity setting.");
  return 0; /* no cert available */
}

/*
** Call this routine once before any other use of the SSL interface.
** This routine does initial configuration of the SSL module.
*/
void ssl_global_init(void){
  const char *zCaSetting = 0, *zCaFile = 0, *zCaDirectory = 0;
  const char *identityFile;

  if( sslIsInit==0 ){
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();
    sslCtx = SSL_CTX_new(SSLv23_client_method());
    /* Disable SSLv2 and SSLv3 */
    SSL_CTX_set_options(sslCtx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

    /* Set up acceptable CA root certificates */
    zCaSetting = db_get("ssl-ca-location", 0);
    if( zCaSetting==0 || zCaSetting[0]=='\0' ){
      /* CA location not specified, use platform's default certificate store */
      X509_STORE_set_default_paths(SSL_CTX_get_cert_store(sslCtx));
    }else{
      /* User has specified a CA location, make sure it exists and use it */
      switch( file_isdir(zCaSetting, ExtFILE) ){
        case 0: { /* doesn't exist */
          fossil_panic("ssl-ca-location is set to '%s', "
              "but is not a file or directory", zCaSetting);
          break;
        }
        case 1: { /* directory */
          zCaDirectory = zCaSetting;
          break;
        }
        case 2: { /* file */
          zCaFile = zCaSetting;
          break;
        }
      }
      if( SSL_CTX_load_verify_locations(sslCtx, zCaFile, zCaDirectory)==0 ){
        fossil_panic("Failed to use CA root certificates from "
          "ssl-ca-location '%s'", zCaSetting);
      }
    }

    /* Load client SSL identity, preferring the filename specified on the
    ** command line */
    if( g.zSSLIdentity!=0 ){
      identityFile = g.zSSLIdentity;
    }else{
      identityFile = db_get("ssl-identity", 0);
    }
    if( identityFile!=0 && identityFile[0]!='\0' ){
      if( SSL_CTX_use_certificate_file(sslCtx,identityFile,SSL_FILETYPE_PEM)!=1
       || SSL_CTX_use_PrivateKey_file(sslCtx,identityFile,SSL_FILETYPE_PEM)!=1
      ){
        fossil_panic("Could not load SSL identity from %s", identityFile);
      }
    }
    /* Register a callback to tell the user what to do when the server asks
    ** for a cert */
    SSL_CTX_set_client_cert_cb(sslCtx, ssl_client_cert_callback);

    sslIsInit = 1;
  }
}

/*
** Call this routine to shutdown the SSL module prior to program exit.
*/
void ssl_global_shutdown(void){
  if( sslIsInit ){
    SSL_CTX_free(sslCtx);
    ssl_clear_errmsg();
    sslIsInit = 0;
  }
}

/*
** Close the currently open SSL connection.  If no connection is open,
** this routine is a no-op.
*/
void ssl_close(void){
  if( iBio!=NULL ){
    (void)BIO_reset(iBio);
    BIO_free_all(iBio);
    iBio = NULL;
  }
}

/* See RFC2817 for details */
static int establish_proxy_tunnel(UrlData *pUrlData, BIO *bio){
  int rc, httpVerMin;
  char *bbuf;
  Blob snd, reply;
  int done=0,end=0;
  blob_zero(&snd);
  blob_appendf(&snd, "CONNECT %s:%d HTTP/1.1\r\n", pUrlData->hostname,
      pUrlData->proxyOrigPort);
  blob_appendf(&snd, "Host: %s:%d\r\n",
               pUrlData->hostname, pUrlData->proxyOrigPort);
  if( pUrlData->proxyAuth ){
    blob_appendf(&snd, "Proxy-Authorization: %s\r\n", pUrlData->proxyAuth);
  }
  blob_append(&snd, "Proxy-Connection: keep-alive\r\n", -1);
  blob_appendf(&snd, "User-Agent: %s\r\n", get_user_agent());
  blob_append(&snd, "\r\n", 2);
  BIO_write(bio, blob_buffer(&snd), blob_size(&snd));
  blob_reset(&snd);

  /* Wait for end of reply */
  blob_zero(&reply);
  do{
    int len;
    char buf[256];
    len = BIO_read(bio, buf, sizeof(buf));
    blob_append(&reply, buf, len);

    bbuf = blob_buffer(&reply);
    len = blob_size(&reply);
    while(end < len) {
      if(bbuf[end] == '\r') {
        if(len - end < 4) {
          /* need more data */
          break;
        }
        if(memcmp(&bbuf[end], "\r\n\r\n", 4) == 0) {
          done = 1;
          break;
        }
      }
      end++;
    }
  }while(!done);
  sscanf(bbuf, "HTTP/1.%d %d", &httpVerMin, &rc);
  blob_reset(&reply);
  return rc;
}

/*
** Invoke this routine to disable SSL cert verification.  After
** this call is made, any SSL cert that the server provides will
** be accepted.  Communication will still be encrypted, but the
** client has no way of knowing whether it is talking to the
** real server or a man-in-the-middle imposter.
*/
void ssl_disable_cert_verification(void){
  sslNoCertVerify = 1;  
}

/*
** Open an SSL connection.  The identify of the server is determined
** as follows:
**
**    pUrlData->name  Name of the server.  Ex: www.fossil-scm.org
**    g.url.name      Name of the proxy server, if proxying.
**    pUrlData->port  TCP/IP port to use.  Ex: 80
**
** Return the number of errors.
*/
int ssl_open(UrlData *pUrlData){
  X509 *cert;

  ssl_global_init();
  if( pUrlData->useProxy ){
    int rc;
    char *connStr = mprintf("%s:%d", g.url.name, pUrlData->port);
    BIO *sBio = BIO_new_connect(connStr);
    free(connStr);
    if( BIO_do_connect(sBio)<=0 ){
      ssl_set_errmsg("SSL: cannot connect to proxy %s:%d (%s)",
            pUrlData->name, pUrlData->port,
            ERR_reason_error_string(ERR_get_error()));
      ssl_close();
      return 1;
    }
    rc = establish_proxy_tunnel(pUrlData, sBio);
    if( rc<200||rc>299 ){
      ssl_set_errmsg("SSL: proxy connect failed with HTTP status code %d", rc);
      return 1;
    }

    pUrlData->path = pUrlData->proxyUrlPath;

    iBio = BIO_new_ssl(sslCtx, 1);
    BIO_push(iBio, sBio);
  }else{
    iBio = BIO_new_ssl_connect(sslCtx);
  }
  if( iBio==NULL ) {
    ssl_set_errmsg("SSL: cannot open SSL (%s)",
                    ERR_reason_error_string(ERR_get_error()));
    return 1;
  }
  BIO_get_ssl(iBio, &ssl);

#if (SSLEAY_VERSION_NUMBER >= 0x00908070) && !defined(OPENSSL_NO_TLSEXT)
  if( !SSL_set_tlsext_host_name(ssl, 
           (pUrlData->useProxy?pUrlData->hostname:pUrlData->name))
  ){
    fossil_warning("WARNING: failed to set server name indication (SNI), "
                  "continuing without it.\n");
  }
#endif

  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

  if( !pUrlData->useProxy ){
    char *connStr = mprintf("%s:%d", pUrlData->name, pUrlData->port);
    BIO_set_conn_hostname(iBio, connStr);
    free(connStr);
    if( BIO_do_connect(iBio)<=0 ){
      ssl_set_errmsg("SSL: cannot connect to host %s:%d (%s)",
         pUrlData->name, pUrlData->port,
         ERR_reason_error_string(ERR_get_error()));
      ssl_close();
      return 1;
    }
  }

  if( BIO_do_handshake(iBio)<=0 ) {
    ssl_set_errmsg("Error establishing SSL connection %s:%d (%s)",
        pUrlData->useProxy?pUrlData->hostname:pUrlData->name,
        pUrlData->useProxy?pUrlData->proxyOrigPort:pUrlData->port,
        ERR_reason_error_string(ERR_get_error()));
    ssl_close();
    return 1;
  }
  /* Check if certificate is valid */
  cert = SSL_get_peer_certificate(ssl);

  if ( cert==NULL ){
    ssl_set_errmsg("No SSL certificate was presented by the peer");
    ssl_close();
    return 1;
  }

  if( !sslNoCertVerify && SSL_get_verify_result(ssl)!=X509_V_OK ){
    int x, desclen;
    char *desc, *prompt;
    Blob ans;
    char cReply;
    BIO *mem;
    unsigned char md[EVP_MAX_MD_SIZE];
    char zHash[EVP_MAX_MD_SIZE*2+1];
    unsigned int mdLength = (int)sizeof(md);

    memset(md, 0, sizeof(md));
    zHash[0] = 0;
                            /*  MMNNFFPPS */
#if OPENSSL_VERSION_NUMBER >= 0x010000000
    x = X509_digest(cert, EVP_sha256(), md, &mdLength);
#else
    x = X509_digest(cert, EVP_sha1(), md, &mdLength);
#endif
    if( x ){
      int j;
      for(j=0; j<mdLength && j*2+1<sizeof(zHash); ++j){
        zHash[j*2] = "0123456789abcdef"[md[j]>>4];
        zHash[j*2+1] = "0123456789abcdef"[md[j]&0xf];
      }
      zHash[j*2] = 0;
    }

    if( ssl_certificate_exception_exists(pUrlData, zHash) ){
      /* Ignore the failure because an exception exists */
      ssl_one_time_exception(pUrlData, zHash);
    }else{
      /* Tell the user about the failure and ask what to do */
      mem = BIO_new(BIO_s_mem());
      BIO_puts(mem,     "  subject: ");
      X509_NAME_print_ex(mem, X509_get_subject_name(cert), 0, XN_FLAG_ONELINE);
      BIO_puts(mem,   "\n  issuer:  ");
      X509_NAME_print_ex(mem, X509_get_issuer_name(cert), 0, XN_FLAG_ONELINE);
      BIO_printf(mem, "\n  sha256:  %s", zHash);
      desclen = BIO_get_mem_data(mem, &desc);
  
      prompt = mprintf("Unable to verify SSL cert from %s\n%.*s\n"
          "accept this cert and continue (y/N)? ",
          pUrlData->name, desclen, desc);
      BIO_free(mem);
  
      prompt_user(prompt, &ans);
      free(prompt);
      cReply = blob_str(&ans)[0];
      blob_reset(&ans);
      if( cReply!='y' && cReply!='Y' ){
        X509_free(cert);
        ssl_set_errmsg("SSL cert declined");
        ssl_close();
        return 1;
      }
      ssl_one_time_exception(pUrlData, zHash);
      prompt_user("remember this exception (y/N)? ", &ans);
      cReply = blob_str(&ans)[0];
      if( cReply=='y' || cReply=='Y') {
        ssl_remember_certificate_exception(pUrlData, zHash);
      }
      blob_reset(&ans);
    }
  }

  /* Set the Global.zIpAddr variable to the server we are talking to.
  ** This is used to populate the ipaddr column of the rcvfrom table,
  ** if any files are received from the server.
  */
  {
  /* As soon as libressl implements
  ** BIO_ADDR_hostname_string/BIO_get_conn_address.
  ** check here for the correct LIBRESSL_VERSION_NUMBER too. For now: disable
  */
#if defined(OPENSSL_VERSION_NUMBER) && OPENSSL_VERSION_NUMBER >= 0x10100000L \
      && !defined(LIBRESSL_VERSION_NUMBER)
    char *ip = BIO_ADDR_hostname_string(BIO_get_conn_address(iBio),1);
    g.zIpAddr = mprintf("%s", ip);
    OPENSSL_free(ip);
#else
    /* IPv4 only code */
    const unsigned char *ip;
    ip = (const unsigned char*)BIO_ptr_ctrl(iBio,BIO_C_GET_CONNECT,2);
    g.zIpAddr = mprintf("%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
#endif
  }

  X509_free(cert);
  return 0;
}

/*
** Remember that the cert with the given hash is a acceptable for
** use with pUrlData->name.
*/
LOCAL void ssl_remember_certificate_exception(
  UrlData *pUrlData,
  const char *zHash
){
  char *zName = mprintf("cert:%s", pUrlData->name);
  db_set(zName, zHash, 1);
  fossil_free(zName);
}

/*
** Return true if the there exists a certificate exception for
** pUrlData->name that matches the hash.
*/
LOCAL int ssl_certificate_exception_exists(
  UrlData *pUrlData,
  const char *zHash
){
  char *zName, *zValue;
  if( fossil_strcmp(sException.zHost,pUrlData->name)==0
   && fossil_strcmp(sException.zHash,zHash)==0
  ){
    return 1;
  }
  zName = mprintf("cert:%s", pUrlData->name);
  zValue = db_get(zName,0);
  fossil_free(zName);
  return zValue!=0 && strcmp(zHash,zValue)==0;
}

/*
** Remember zHash as an acceptable certificate for this session only.
*/
LOCAL void ssl_one_time_exception(
  UrlData *pUrlData,
  const char *zHash
){
  fossil_free(sException.zHost);
  sException.zHost = fossil_strdup(pUrlData->name);
  fossil_free(sException.zHash);
  sException.zHash = fossil_strdup(zHash);
}

/*
** Send content out over the SSL connection.
*/
size_t ssl_send(void *NotUsed, void *pContent, size_t N){
  size_t total = 0;
  while( N>0 ){
    int sent = BIO_write(iBio, pContent, N);
    if( sent<=0 ){
      if( BIO_should_retry(iBio) ){
        continue;
      }
      break;
    }
    total += sent;
    N -= sent;
    pContent = (void*)&((char*)pContent)[sent];
  }
  return total;
}

/*
** Receive content back from the SSL connection.
*/
size_t ssl_receive(void *NotUsed, void *pContent, size_t N){
  size_t total = 0;
  while( N>0 ){
    int got = BIO_read(iBio, pContent, N);
    if( got<=0 ){
      if( BIO_should_retry(iBio) ){
        continue;
      }
      break;
    }
    total += got;
    N -= got;
    pContent = (void*)&((char*)pContent)[got];
  }
  return total;
}

#endif /* FOSSIL_ENABLE_SSL */

/*
** COMMAND: tls-config*
**
** Usage: %fossil tls-config [SUBCOMMAND] [OPTIONS...] [ARGS...]
**
** This command is used to view or modify the TLS (Transport Layer
** Security) configuration for Fossil.  TLS (formerly SSL) is the
** encryption technology used for secure HTTPS transport.
**
** Sub-commands:
**
**    show                            Show the TLS configuration
**
**    remove-exception DOMAIN...      Remove TLS cert exceptions
**                                    for the domains listed.  Or if
**                                    the --all option is specified,
**                                    remove all TLS cert exceptions.
*/
void test_tlsconfig_info(void){
#if !defined(FOSSIL_ENABLE_SSL)
  fossil_print("TLS disabled in this build\n");
#else
  const char *zCmd;
  size_t nCmd;
  int nHit = 0;
  db_find_and_open_repository(OPEN_OK_NOT_FOUND|OPEN_SUBSTITUTE,0);
  db_open_config(1,0);
  zCmd = g.argc>=3 ? g.argv[2] : "show";
  nCmd = strlen(zCmd);
  if( strncmp("show",zCmd,nCmd)==0 ){
    const char *zName, *zValue;
    size_t nName;
    Stmt q;
    fossil_print("OpenSSL-version:   %s  (0x%09x)\n",
         SSLeay_version(SSLEAY_VERSION), OPENSSL_VERSION_NUMBER);
    fossil_print("OpenSSL-cert-file: %s\n", X509_get_default_cert_file());
    fossil_print("OpenSSL-cert-dir:  %s\n", X509_get_default_cert_dir());
    zName = X509_get_default_cert_file_env();
    zValue = fossil_getenv(zName);
    if( zValue==0 ) zValue = "";
    nName = strlen(zName);
    fossil_print("%s:%*s%s\n", zName, 18-nName, "", zValue);
    zName = X509_get_default_cert_dir_env();
    zValue = fossil_getenv(zName);
    if( zValue==0 ) zValue = "";
    nName = strlen(zName);
    fossil_print("%s:%*s%s\n", zName, 18-nName, "", zValue);
    nHit++;
    fossil_print("ssl-ca-location:   %s\n", db_get("ssl-ca-location",""));
    fossil_print("ssl-identity:      %s\n", db_get("ssl-identity",""));
    db_prepare(&q,
       "SELECT name FROM global_config"
       " WHERE name GLOB 'cert:*'"
       "UNION ALL "
       "SELECT name FROM config"
       " WHERE name GLOB 'cert:*'"
       " ORDER BY name"
    );
    while( db_step(&q)==SQLITE_ROW ){
      fossil_print("exception:         %s\n", db_column_text(&q,0)+5);
    }
    db_finalize(&q);
  }else
  if( strncmp("remove-exception",zCmd,nCmd)==0 ){
    int i;
    Blob sql;
    char *zSep = "(";
    db_begin_transaction();
    blob_init(&sql, 0, 0);
    if( g.argc==4 && find_option("all",0,0)!=0 ){
      db_unprotect(PROTECT_CONFIG);
      blob_append_sql(&sql,
        "DELETE FROM global_config WHERE name GLOB 'cert:*';\n"
        "DELETE FROM global_config WHERE name GLOB 'trusted:*';\n"
        "DELETE FROM config WHERE name GLOB 'cert:*';\n"
        "DELETE FROM config WHERE name GLOB 'trusted:*';\n"
      );
      db_protect_pop();
    }else{
      if( g.argc<4 ){
        usage("remove-exception DOMAIN-NAME ...");
      }
      blob_append_sql(&sql,"DELETE FROM global_config WHERE name IN ");
      for(i=3; i<g.argc; i++){
        blob_append_sql(&sql,"%s'cert:%q','trust:%q'",
           zSep/*safe-for-%s*/, g.argv[i], g.argv[i]);
        zSep = ",";
      }
      blob_append_sql(&sql,");\n");
      zSep = "(";
      blob_append_sql(&sql,"DELETE FROM config WHERE name IN ");
      for(i=3; i<g.argc; i++){
        blob_append_sql(&sql,"%s'cert:%q','trusted:%q'",
           zSep/*safe-for-%s*/, g.argv[i], g.argv[i]);
        zSep = ",";
      }
      blob_append_sql(&sql,");");
    }
    db_exec_sql(blob_str(&sql));
    db_commit_transaction();
    blob_reset(&sql);
  }else
  /*default*/{
    fossil_fatal("unknown sub-command \"%s\".\nshould be one of:"
                 " remove-exception show",
       zCmd);
  }
#endif
}
