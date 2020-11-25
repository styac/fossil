/*
** Copyright (c) 2007 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)

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
** This file contains code used to push, pull, and sync a repository
*/
#include "config.h"
#include "sync.h"
#include <assert.h>

/*
** If the repository is configured for autosyncing, then do an
** autosync.  Bits of the "flags" parameter determine details of behavior:
**
**   SYNC_PULL           Pull content from the server to the local repo
**   SYNC_PUSH           Push content from local up to the server
**   SYNC_CKIN_LOCK      Take a check-in lock on the current checkout.
**   SYNC_VERBOSE        Extra output
**
** Return the number of errors.
**
** The autosync setting can be a boolean or "pullonly".  No autosync
** is attempted if the autosync setting is off, and only auto-pull is
** attempted if autosync is set to "pullonly".  The check-in lock is
** not acquired unless autosync is set to "on".
**
** If dont-push setting is true, that is the same as having autosync
** set to pullonly.
*/
int autosync(int flags){
  const char *zAutosync;
  int rc;
  int configSync = 0;       /* configuration changes transferred */
  if( g.fNoSync ){
    return 0;
  }
  zAutosync = db_get("autosync", 0);
  if( zAutosync==0 ) zAutosync = "on";  /* defend against misconfig */
  if( is_false(zAutosync) ) return 0;
  if( db_get_boolean("dont-push",0) || fossil_strncmp(zAutosync,"pull",4)==0 ){
    flags &= ~SYNC_CKIN_LOCK;
    if( flags & SYNC_PUSH ) return 0;
  }
  url_parse(0, URL_REMEMBER);
  if( g.url.protocol==0 ) return 0;
  if( g.url.user!=0 && g.url.passwd==0 ){
    g.url.passwd = unobscure(db_get("last-sync-pw", 0));
    g.url.flags |= URL_PROMPT_PW;
    url_prompt_for_password();
  }
  g.zHttpAuth = get_httpauth();
  url_remember();
  if( find_option("verbose","v",0)!=0 ) flags |= SYNC_VERBOSE;
  fossil_print("Autosync:  %s\n", g.url.canonical);
  url_enable_proxy("via proxy: ");
  rc = client_sync(flags, configSync, 0, 0);
  return rc;
}

/*
** This routine will try a number of times to perform autosync with a
** 0.5 second sleep between attempts.
**
** Return zero on success and non-zero on a failure.  If failure occurs
** and doPrompt flag is true, ask the user if they want to continue, and
** if they answer "yes" then return zero in spite of the failure.
*/
int autosync_loop(int flags, int nTries, int doPrompt){
  int n = 0;
  int rc = 0;
  if( (flags & (SYNC_PUSH|SYNC_PULL))==(SYNC_PUSH|SYNC_PULL)
   && db_get_boolean("uv-sync",0)
  ){
    flags |= SYNC_UNVERSIONED;
  }
  while( (n==0 || n<nTries) && (rc=autosync(flags)) ){
    if( rc ){
      if( ++n<nTries ){
        fossil_warning("Autosync failed, making another attempt.");
        sqlite3_sleep(500);
      }else{
        fossil_warning("Autosync failed.");
      }
    }
  }
  if( rc && doPrompt ){
    Blob ans;
    char cReply;
    prompt_user("continue in spite of sync failure (y/N)? ", &ans);
    cReply = blob_str(&ans)[0];
    if( cReply=='y' || cReply=='Y' ) rc = 0;
    blob_reset(&ans);
  }
  return rc;
}

/*
** This routine processes the command-line argument for push, pull,
** and sync.  If a command-line argument is given, that is the URL
** of a server to sync against.  If no argument is given, use the
** most recently synced URL.  Remember the current URL for next time.
*/
static void process_sync_args(
  unsigned *pConfigFlags,      /* Write configuration flags here */
  unsigned *pSyncFlags,        /* Write sync flags here */
  int uvOnly,                  /* Special handling flags for UV sync */
  unsigned urlOmitFlags        /* Omit these URL flags */
){
  const char *zUrl = 0;
  const char *zHttpAuth = 0;
  unsigned configSync = 0;
  unsigned urlFlags = URL_REMEMBER | URL_PROMPT_PW;
  int urlOptional = 0;
  if( find_option("autourl",0,0)!=0 ){
    urlOptional = 1;
    urlFlags = 0;
  }
  zHttpAuth = find_option("httpauth","B",1);
  if( find_option("once",0,0)!=0 ) urlFlags &= ~URL_REMEMBER;
  if( (*pSyncFlags) & SYNC_FROMPARENT ) urlFlags &= ~URL_REMEMBER;
  if( !uvOnly ){
    if( find_option("private",0,0)!=0 ){
      *pSyncFlags |= SYNC_PRIVATE;
    }
    /* The --verily option to sync, push, and pull forces extra igot cards
    ** to be exchanged.  This can overcome malfunctions in the sync protocol.
    */
    if( find_option("verily",0,0)!=0 ){
      *pSyncFlags |= SYNC_RESYNC;
    }
  }
  if( find_option("private",0,0)!=0 ){
    *pSyncFlags |= SYNC_PRIVATE;
  }
  if( find_option("verbose","v",0)!=0 ){
    *pSyncFlags |= SYNC_VERBOSE;
  }
  url_proxy_options();
  clone_ssh_find_options();
  if( !uvOnly ) db_find_and_open_repository(0, 0);
  db_open_config(0, 1);
  if( g.argc==2 ){
    if( db_get_boolean("auto-shun",0) ) configSync = CONFIGSET_SHUN;
  }else if( g.argc==3 ){
    zUrl = g.argv[2];
  }
  if( ((*pSyncFlags) & (SYNC_PUSH|SYNC_PULL))==(SYNC_PUSH|SYNC_PULL)
   && db_get_boolean("uv-sync",0)
  ){
    *pSyncFlags |= SYNC_UNVERSIONED;
  }
  urlFlags &= ~urlOmitFlags;
  if( urlFlags & URL_REMEMBER ){
    clone_ssh_db_set_options();
  }
  url_parse(zUrl, urlFlags);
  remember_or_get_http_auth(zHttpAuth, urlFlags & URL_REMEMBER, zUrl);
  url_remember();
  if( g.url.protocol==0 ){
    if( urlOptional ) fossil_exit(0);
    usage("URL");
  }
  user_select();
  if( g.url.isAlias ){
    if( ((*pSyncFlags) & (SYNC_PUSH|SYNC_PULL))==(SYNC_PUSH|SYNC_PULL) ){
      fossil_print("Sync with %s\n", g.url.canonical);
    }else if( (*pSyncFlags) & SYNC_PUSH ){
      fossil_print("Push to %s\n", g.url.canonical);
    }else if( (*pSyncFlags) & SYNC_PULL ){
      fossil_print("Pull from %s\n", g.url.canonical);
    }
  }
  url_enable_proxy("via proxy: ");
  *pConfigFlags |= configSync;
}

/*
** COMMAND: pull
**
** Usage: %fossil pull ?URL? ?options?
**
** Pull all sharable changes from a remote repository into the local
** repository.  Sharable changes include public check-ins, edits to
** wiki pages, tickets, and tech-notes, as well as forum content.  Add
** the --private option to pull private branches.  Use the
** "configuration pull" command to pull website configuration details.
**
** If URL is not specified, then the URL from the most recent clone, push,
** pull, remote, or sync command is used.  See "fossil help clone" for
** details on the URL formats.
**
** Options:
**
**   -B|--httpauth USER:PASS    Credentials for the simple HTTP auth protocol,
**                              if required by the remote website
**   --from-parent-project      Pull content from the parent project
**   --ipv4                     Use only IPv4, not IPv6
**   --once                     Do not remember URL for subsequent syncs
**   --private                  Pull private branches too
**   --project-code CODE        Use CODE as the project code
**   --proxy PROXY              Use the specified HTTP proxy
**   -R|--repository REPO       Local repository to pull into
**   --ssl-identity FILE        Local SSL credentials, if requested by remote
**   --ssh-command SSH          Use SSH as the "ssh" command
**   -v|--verbose               Additional (debugging) output
**   --verily                   Exchange extra information with the remote
**                              to ensure no content is overlooked
**
** See also: [[clone]], [[config]], [[push]], [[remote]], [[sync]]
*/
void pull_cmd(void){
  unsigned configFlags = 0;
  unsigned syncFlags = SYNC_PULL;
  unsigned urlOmitFlags = 0;
  const char *zAltPCode = find_option("project-code",0,1);
  if( find_option("from-parent-project",0,0)!=0 ){
    syncFlags |= SYNC_FROMPARENT;
  }
  if( zAltPCode ) urlOmitFlags = URL_REMEMBER;
  process_sync_args(&configFlags, &syncFlags, 0, urlOmitFlags);

  /* We should be done with options.. */
  verify_all_options();

  client_sync(syncFlags, configFlags, 0, zAltPCode);
}

/*
** COMMAND: push
**
** Usage: %fossil push ?URL? ?options?
**
** Push all sharable changes from the local repository to a remote
** repository.  Sharable changes include public check-ins, edits to
** wiki pages, tickets, and tech-notes, as well as forum content.  Use
** --private to also push private branches.  Use the "configuration
** push" command to push website configuration details.
**
** If URL is not specified, then the URL from the most recent clone, push,
** pull, remote, or sync command is used.  See "fossil help clone" for
** details on the URL formats.
**
** Options:
**
**   -B|--httpauth USER:PASS    Credentials for the simple HTTP auth protocol,
**                              if required by the remote website
**   --ipv4                     Use only IPv4, not IPv6
**   --once                     Do not remember URL for subsequent syncs
**   --proxy PROXY              Use the specified HTTP proxy
**   --private                  Push private branches too
**   -R|--repository REPO       Local repository to push from
**   --ssl-identity FILE        Local SSL credentials, if requested by remote
**   --ssh-command SSH          Use SSH as the "ssh" command
**   -v|--verbose               Additional (debugging) output
**   --verily                   Exchange extra information with the remote
**                              to ensure no content is overlooked
**
** See also: [[clone]], [[config]], [[pull]], [[remote]], [[sync]]
*/
void push_cmd(void){
  unsigned configFlags = 0;
  unsigned syncFlags = SYNC_PUSH;
  process_sync_args(&configFlags, &syncFlags, 0, 0);

  /* We should be done with options.. */
  verify_all_options();

  if( db_get_boolean("dont-push",0) ){
    fossil_fatal("pushing is prohibited: the 'dont-push' option is set");
  }
  client_sync(syncFlags, 0, 0, 0);
}


/*
** COMMAND: sync
**
** Usage: %fossil sync ?URL? ?options?
**
** Synchronize all sharable changes between the local repository and a
** remote repository.  Sharable changes include public check-ins and
** edits to wiki pages, tickets, and technical notes.
**
** If URL is not specified, then the URL from the most recent clone, push,
** pull, remote, or sync command is used.  See "fossil help clone" for
** details on the URL formats.
**
** Options:
**
**   -B|--httpauth USER:PASS    Credentials for the simple HTTP auth protocol,
**                              if required by the remote website
**   --ipv4                     Use only IPv4, not IPv6
**   --once                     Do not remember URL for subsequent syncs
**   --proxy PROXY              Use the specified HTTP proxy
**   --private                  Sync private branches too
**   -R|--repository REPO       Local repository to sync with
**   --ssl-identity FILE        Local SSL credentials, if requested by remote
**   --ssh-command SSH          Use SSH as the "ssh" command
**   -u|--unversioned           Also sync unversioned content
**   -v|--verbose               Additional (debugging) output
**   --verily                   Exchange extra information with the remote
**                              to ensure no content is overlooked
**
** See also: [[clone]], [[pull]], [[push]], [[remote]]
*/
void sync_cmd(void){
  unsigned configFlags = 0;
  unsigned syncFlags = SYNC_PUSH|SYNC_PULL;
  if( find_option("unversioned","u",0)!=0 ){
    syncFlags |= SYNC_UNVERSIONED;
  }
  process_sync_args(&configFlags, &syncFlags, 0, 0);

  /* We should be done with options.. */
  verify_all_options();

  if( db_get_boolean("dont-push",0) ) syncFlags &= ~SYNC_PUSH;
  client_sync(syncFlags, configFlags, 0, 0);
  if( (syncFlags & SYNC_PUSH)==0 ){
    fossil_warning("pull only: the 'dont-push' option is set");
  }
}

/*
** Handle the "fossil unversioned sync" and "fossil unversioned revert"
** commands.
*/
void sync_unversioned(unsigned syncFlags){
  unsigned configFlags = 0;
  (void)find_option("uv-noop",0,0);
  process_sync_args(&configFlags, &syncFlags, 1, 0);
  verify_all_options();
  client_sync(syncFlags, 0, 0, 0);
}

/*
** COMMAND: remote
** COMMAND: remote-url*
**
** Usage: %fossil remote ?SUBCOMMAND ...?
**
** Use this command to view or modify the set of remote repositories
** used as the default target for sync, push, and pull and for autosync.
**
** The default remote is set automatically by a "clone" command or by any
** "sync", "push", or "pull" command that specifies an explicit URL
** and omits the --once flag.  The default remote is used by
** auto-syncing and by "sync", "push", and "pull" that omit the server URL.
** Additional remotes can be added using the "add" command or deleted
** using the "delete" command.  The name of any additional remote can be
** used as an argument to the "sync", "push", and "pull" commands where
** one would normally put a URL argument.
**
** See "fossil help clone" for further information about URL formats.
**
** The official name of this command is "remote-url" but most people
** use the shortened name "remote".
**
** > fossil remote
**
**     With no arguments, this command shows the current default remote.
**     Or if there is no default, it shows "off".  The default remote is
**     used by autosync.  The default remote is whatever URL was specified
**     for the most recent "sync", "push", or "pull" command that omitted
**     the --once option.
**
** > fossil remote add NAME URL
**
**     Add a new URL to the set of remotes.  The new URL is assigned the
**     symbolic identifier "NAME".  Subsequently, NAME can be used in place
**     of the full URL on commands like "push" and "pull".
**
** > fossil remote delete NAME
**
**     Delete a URL previously added by the "add" subcommand.
**
** > fossil remote list
**
**     Show all remote URLs
**
** > fossil remote off
**
**     Disable the default URL.  Use this as a shorthand to prevent
**     autosync while in airplane mode, for example.
**
** > fossil remote REF
**
**     Make REF the new default URL.  The prior default URL is replaced.
**     REF can be either an explicit URL or a NAME from a prior "add".
*/
void remote_url_cmd(void){
  char *zUrl, *zArg;
  int nArg;
  db_find_and_open_repository(0, 0);

  /* We should be done with options.. */
  verify_all_options();

  if( g.argc==2 ){
    /* "fossil remote" with no arguments:  Show the last sync URL. */
    zUrl = db_get("last-sync-url", 0);
    if( zUrl==0 ){
      fossil_print("off\n");
    }else{
      url_parse(zUrl, 0);
      fossil_print("%s\n", g.url.canonical);
    }
    return;
  }
  zArg = g.argv[2];
  nArg = (int)strlen(zArg);
  if( strcmp(zArg,"off")==0 ){
    /* fossil remote off
    ** Forget the last-sync-URL and its password
    */
    if( g.argc!=3 ) usage("off");
remote_delete_default:
    db_unprotect(PROTECT_CONFIG);
    db_multi_exec(
      "DELETE FROM config WHERE name GLOB 'last-sync-*';"
    );
    db_protect_pop();
    return;
  }
  if( strncmp(zArg, "list", nArg)==0 || strcmp(zArg,"ls")==0 ){
    Stmt q;
    if( g.argc!=3 ) usage("list");
    db_prepare(&q,
      "SELECT 'default', value FROM config WHERE name='last-sync-url'"
      " UNION ALL "
      "SELECT substr(name,10), value FROM config"
      " WHERE name GLOB 'sync-url:*'"
      " ORDER BY 1"
    );
    while( db_step(&q)==SQLITE_ROW ){
      fossil_print("%-18s %s\n", db_column_text(&q,0), db_column_text(&q,1));
    }
    db_finalize(&q);
    return;
  }
  if( strcmp(zArg, "add")==0 ){
    char *zName;
    char *zUrl;
    UrlData x;
    if( g.argc!=5 ) usage("add NAME URL");
    memset(&x, 0, sizeof(x));
    zName = g.argv[3];
    zUrl = g.argv[4];
    if( strcmp(zName,"default")==0 ) goto remote_add_default;
    url_parse_local(zUrl, URL_PROMPT_PW, &x);
    db_begin_write();
    db_unprotect(PROTECT_CONFIG);
    db_multi_exec(
       "REPLACE INTO config(name, value, mtime)"
       " VALUES('sync-url:%q',%Q,now())",
       zName, x.canonical
    );
    db_multi_exec(
       "REPLACE INTO config(name, value, mtime)"
       " VALUES('sync-pw:%q',obscure(%Q),now())",
       zName, x.passwd
    );
    db_protect_pop();
    db_commit_transaction();
    return;
  }
  if( strncmp(zArg, "delete", nArg)==0 ){
    char *zName;
    if( g.argc!=4 ) usage("delete NAME");
    zName = g.argv[3];
    if( strcmp(zName,"default")==0 ) goto remote_delete_default;
    db_begin_write();
    db_unprotect(PROTECT_CONFIG);
    db_multi_exec("DELETE FROM config WHERE name glob 'sync-url:%q'", zName);
    db_multi_exec("DELETE FROM config WHERE name glob 'sync-pw:%q'", zName);
    db_protect_pop();
    db_commit_transaction();
    return;
  }
  if( sqlite3_strlike("http://%",zArg,0)==0
   || sqlite3_strlike("https://%",zArg,0)==0
   || sqlite3_strlike("ssh:%",zArg,0)==0
   || sqlite3_strlike("file:%",zArg,0)==0
   || db_exists("SELECT 1 FROM config WHERE name='sync-url:%q'",zArg)
  ){
remote_add_default:
    db_unset("last-sync-url", 0);
    db_unset("last-sync-pw", 0);
    url_parse(g.argv[2], URL_REMEMBER|URL_PROMPT_PW|URL_ASK_REMEMBER_PW);
    url_remember();
    return;
  }
  fossil_fatal("unknown command \"%s\" - should be a URL or one of: "
               "add delete list off", zArg);
}

/*
** COMMAND: backup*
**
** Usage: %fossil backup ?OPTIONS? FILE|DIRECTORY
**
** Make a backup of the repository into the named file or into the named
** directory.  This backup is guaranteed to be consistent even if there are
** concurrent changes taking place on the repository.  In other words, it
** is safe to run "fossil backup" on a repository that is in active use.
**
** Only the main repository database is backed up by this command.  The
** open checkout file (if any) is not saved.  Nor is the global configuration
** database.
**
** Options:
**
**    --overwrite              OK to overwrite an existing file
**    -R NAME                  Filename of the repository to backup
*/
void backup_cmd(void){
  char *zDest;
  int bOverwrite = 0;
  db_find_and_open_repository(OPEN_ANY_SCHEMA, 0);
  bOverwrite = find_option("overwrite",0,0)!=0;
  verify_all_options();
  if( g.argc!=3 ){
    usage("FILE|DIRECTORY");
  }
  zDest = g.argv[2];
  if( file_isdir(zDest, ExtFILE)==1 ){
    zDest = mprintf("%s/%s", zDest, file_tail(g.zRepositoryName));
  }
  if( file_isfile(zDest, ExtFILE) ){
    if( bOverwrite ){
      if( file_delete(zDest) ){
        fossil_fatal("unable to delete old copy of \"%s\"", zDest);
      }
    }else{
      fossil_fatal("backup \"%s\" already exists", zDest);
    }
  }
  db_unprotect(PROTECT_ALL);
  db_multi_exec("VACUUM repository INTO %Q", zDest);
}
