/*
** Copyright (c) 2006 D. Richard Hipp
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
** This file contains code used to resolved user-supplied object names.
*/
#include "config.h"
#include "name.h"
#include <assert.h>

/*
** Return TRUE if the string begins with something that looks roughly
** like an ISO date/time string.  The SQLite date/time functions will
** have the final say-so about whether or not the date/time string is
** well-formed.
*/
int fossil_isdate(const char *z){
  if( !fossil_isdigit(z[0]) ) return 0;
  if( !fossil_isdigit(z[1]) ) return 0;
  if( !fossil_isdigit(z[2]) ) return 0;
  if( !fossil_isdigit(z[3]) ) return 0;
  if( z[4]!='-') return 0;
  if( !fossil_isdigit(z[5]) ) return 0;
  if( !fossil_isdigit(z[6]) ) return 0;
  if( z[7]!='-') return 0;
  if( !fossil_isdigit(z[8]) ) return 0;
  if( !fossil_isdigit(z[9]) ) return 0;
  return 1;
}

/*
** Check to see if the string might be a compact date/time that omits
** the punctuation.  Example:  "20190327084549" instead of
** "2019-03-27 08:45:49".  If the string is of the appropriate form,
** then return an alternative string (in static space) that is the same
** string with punctuation inserted.
**
** If the bVerifyNotAHash flag is true, then a check is made to see if
** the string is a hash prefix and NULL is returned if it is.  If the
** bVerifyNotAHash flag is false, then the result is determined by syntax
** of the input string only, without reference to the artifact table.
*/
char *fossil_expand_datetime(const char *zIn, int bVerifyNotAHash){
  static char zEDate[20];
  static const char aPunct[] = { 0, 0, '-', '-', ' ', ':', ':' };
  int n = (int)strlen(zIn);
  int i, j;

  /* Only three forms allowed:
  **   (1)  YYYYMMDD
  **   (2)  YYYYMMDDHHMM
  **   (3)  YYYYMMDDHHMMSS
  */
  if( n!=8 && n!=12 && n!=14 ) return 0;

  /* Every character must be a digit */
  for(i=0; fossil_isdigit(zIn[i]); i++){}
  if( i!=n ) return 0;

  /* Expand the date */
  for(i=j=0; zIn[i]; i++){
    if( i>=4 && (i%2)==0 ){
      zEDate[j++] = aPunct[i/2];
    }
    zEDate[j++] = zIn[i];
  }
  zEDate[j] = 0;

  /* Check for reasonable date values.
  ** Offset references:
  **    YYYY-MM-DD HH:MM:SS
  **    0123456789 12345678
  */

  i = atoi(zEDate);
  if( i<1970 || i>2100 ) return 0;
  i = atoi(zEDate+5);
  if( i<1 || i>12 ) return 0;
  i = atoi(zEDate+8);
  if( i<1 || i>31 ) return 0;
  if( n>8 ){
    i = atoi(zEDate+11);
    if( i>24 ) return 0;
    i = atoi(zEDate+14);
    if( i>60 ) return 0;
    if( n==14 && atoi(zEDate+17)>60 ) return 0;
  }

  /* The string is not also a hash prefix */
  if( bVerifyNotAHash ){
    if( db_exists("SELECT 1 FROM blob WHERE uuid GLOB '%q*'",zIn) ) return 0;
  }

  /* It looks like this may be a date.  Return it with punctuation added. */
  return zEDate;
}

/*
** The data-time string in the argument is going to be used as an
** upper bound like this:    mtime<=julianday(zDate,'localtime').
** But if the zDate parameter omits the fractional seconds or the
** seconds, or the time, that might mess up the == part of the
** comparison.  So add in missing factional seconds or seconds or time.
**
** The returned string is held in a static buffer that is overwritten
** with each call, or else is just a copy of its input if there are
** no changes.
*/
const char *fossil_roundup_date(const char *zDate){
  static char zUp[24];
  int n = (int)strlen(zDate);
  if( n==19 ){  /* YYYY-MM-DD HH:MM:SS */
    memcpy(zUp, zDate, 19);
    memcpy(zUp+19, ".999", 5);
    return zUp;
  }
  if( n==16 ){ /* YYYY-MM-DD HH:MM */
    memcpy(zUp, zDate, 16);
    memcpy(zUp+16, ":59.999", 8);
    return zUp;
  }
  if( n==10 ){ /* YYYY-MM-DD */
    memcpy(zUp, zDate, 10);
    memcpy(zUp+10, " 23:59:59.999", 14);
    return zUp;
  }
  return zDate;
}


/*
** Return the RID that is the "root" of the branch that contains
** check-in "rid".  Details depending on eType:
**
**    eType==0    The check-in of the parent branch off of which
**                the branch containing RID originally diverged.
**
**    eType==1    The first check-in of the branch that contains RID.
**
**    eType==2    The youngest ancestor of RID that is on the branch
**                from which the branch containing RID diverged.
*/
int start_of_branch(int rid, int eType){
  Stmt q;
  int rc;
  int ans = rid;
  char *zBr = branch_of_rid(rid);
  db_prepare(&q,
    "SELECT pid, EXISTS(SELECT 1 FROM tagxref"
                       " WHERE tagid=%d AND tagtype>0"
                       "   AND value=%Q AND rid=plink.pid)"
    "  FROM plink"
    " WHERE cid=:cid AND isprim",
    TAG_BRANCH, zBr
  );
  fossil_free(zBr);
  do{
    db_reset(&q);
    db_bind_int(&q, ":cid", ans);
    rc = db_step(&q);
    if( rc!=SQLITE_ROW ) break;
    if( eType==1 && db_column_int(&q,1)==0 ) break;
    ans = db_column_int(&q, 0);
  }while( db_column_int(&q, 1)==1 && ans>0 );
  db_finalize(&q);
  if( eType==2 && ans>0 ){
    zBr = branch_of_rid(ans);
    ans = compute_youngest_ancestor_in_branch(rid, zBr);
    fossil_free(zBr);
  }
  return ans;
}

/*
** Convert a symbolic name into a RID.  Acceptable forms:
**
**   *  artifact hash (optionally enclosed in [...])
**   *  4-character or larger prefix of a artifact
**   *  Symbolic Name
**   *  "tag:" + symbolic name
**   *  Date or date-time
**   *  "date:" + Date or date-time
**   *  symbolic-name ":" date-time
**   *  "tip"
**
** The following additional forms are available in local checkouts:
**
**   *  "current"
**   *  "prev" or "previous"
**   *  "next"
**
** Return the RID of the matching artifact.  Or return 0 if the name does not
** match any known object.  Or return -1 if the name is ambiguous.
**
** The zType parameter specifies the type of artifact: ci, t, w, e, g, f.
** If zType is NULL or "" or "*" then any type of artifact will serve.
** If zType is "br" then find the first check-in of the named branch
** rather than the last.
**
** zType is "ci" in most use cases since we are usually searching for
** a check-in.
**
** Note that the input zTag for types "t" and "e" is the artifact hash of
** the ticket-change or technote-change artifact, not the randomly generated
** hexadecimal identifier assigned to tickets and events.  Those identifiers
** live in a separate namespace.
*/
int symbolic_name_to_rid(const char *zTag, const char *zType){
  int vid;
  int rid = 0;
  int nTag;
  int i;
  int startOfBranch = 0;
  const char *zXTag;     /* zTag with optional [...] removed */
  int nXTag;             /* Size of zXTag */
  const char *zDate;     /* Expanded date-time string */
  const char *zTagPrefix = "sym";

  if( zType==0 || zType[0]==0 ){
    zType = "*";
  }else if( zType[0]=='b' ){
    zType = "ci";
    startOfBranch = 1;
  }
  if( zTag==0 || zTag[0]==0 ) return 0;

  /* special keyword: "tip" */
  if( fossil_strcmp(zTag, "tip")==0 && (zType[0]=='*' || zType[0]=='c') ){
    rid = db_int(0,
      "SELECT objid"
      "  FROM event"
      " WHERE type='ci'"
      " ORDER BY event.mtime DESC"
    );
    if( rid ) return rid;
  }

  /* special keywords: "prev", "previous", "current", and "next" */
  if( g.localOpen && (vid=db_lget_int("checkout",0))!=0 ){
    if( fossil_strcmp(zTag, "current")==0 ){
      rid = vid;
    }else if( fossil_strcmp(zTag, "prev")==0
              || fossil_strcmp(zTag, "previous")==0 ){
      rid = db_int(0, "SELECT pid FROM plink WHERE cid=%d AND isprim", vid);
    }else if( fossil_strcmp(zTag, "next")==0 ){
      rid = db_int(0, "SELECT cid FROM plink WHERE pid=%d"
                      "  ORDER BY isprim DESC, mtime DESC", vid);
    }
    if( rid ) return rid;
  }

  /* Date and times */
  if( memcmp(zTag, "date:", 5)==0 ){
    zDate = fossil_expand_datetime(&zTag[5],0);
    if( zDate==0 ) zDate = &zTag[5];
    rid = db_int(0,
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q,fromLocal()) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      fossil_roundup_date(zDate), zType);
    return rid;
  }
  if( fossil_isdate(zTag) ){
    rid = db_int(0,
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q,fromLocal()) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      fossil_roundup_date(zTag), zType);
    if( rid) return rid;
  }

  /* Deprecated date & time formats:   "local:" + date-time and
  ** "utc:" + date-time */
  if( memcmp(zTag, "local:", 6)==0 ){
    rid = db_int(0,
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      &zTag[6], zType);
    return rid;
  }
  if( memcmp(zTag, "utc:", 4)==0 ){
    rid = db_int(0,
      "SELECT objid FROM event"
      " WHERE mtime<=julianday('%qz') AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      fossil_roundup_date(&zTag[4]), zType);
    return rid;
  }

  /* "tag:" + symbolic-name */
  if( memcmp(zTag, "tag:", 4)==0 ){
    rid = db_int(0,
       "SELECT event.objid, max(event.mtime)"
       "  FROM tag, tagxref, event"
       " WHERE tag.tagname='sym-%q' "
       "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
       "   AND event.objid=tagxref.rid "
       "   AND event.type GLOB '%q'",
       &zTag[4], zType
    );
    if( startOfBranch ) rid = start_of_branch(rid,1);
    return rid;
  }

  /* root:BR -> The origin of the branch named BR */
  if( strncmp(zTag, "root:", 5)==0 ){
    rid = symbolic_name_to_rid(zTag+5, zType);
    return start_of_branch(rid, 0);
  }
  /* rootx:BR -> Most recent merge-in for the branch name BR */
  if( strncmp(zTag, "merge-in:", 9)==0 ){
    rid = symbolic_name_to_rid(zTag+9, zType);
    return start_of_branch(rid, 2);
  }

  /* symbolic-name ":" date-time */
  nTag = strlen(zTag);
  for(i=0; i<nTag-8 && zTag[i]!=':'; i++){}
  if( zTag[i]==':' 
   && (fossil_isdate(&zTag[i+1]) || fossil_expand_datetime(&zTag[i+1],0)!=0)
  ){
    char *zDate = mprintf("%s", &zTag[i+1]);
    char *zTagBase = mprintf("%.*s", i, zTag);
    char *zXDate;
    int nDate = strlen(zDate);
    if( sqlite3_strnicmp(&zDate[nDate-3],"utc",3)==0 ){
      zDate[nDate-3] = 'z';
      zDate[nDate-2] = 0;
    }
    zXDate = fossil_expand_datetime(zDate,0);
    if( zXDate==0 ) zXDate = zDate;
    rid = db_int(0,
      "SELECT event.objid, max(event.mtime)"
      "  FROM tag, tagxref, event"
      " WHERE tag.tagname='sym-%q' "
      "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
      "   AND event.objid=tagxref.rid "
      "   AND event.mtime<=julianday(%Q,fromLocal())"
      "   AND event.type GLOB '%q'",
      zTagBase, fossil_roundup_date(zXDate), zType
    );
    fossil_free(zDate);
    fossil_free(zTagBase);
    return rid;
  }

  /* Remove optional [...] */
  zXTag = zTag;
  nXTag = nTag;
  if( zXTag[0]=='[' ){
    zXTag++;
    nXTag--;
  }
  if( nXTag>0 && zXTag[nXTag-1]==']' ){
    nXTag--;
  }

  /* artifact hash or prefix */
  if( nXTag>=4 && nXTag<=HNAME_MAX && validate16(zXTag, nXTag) ){
    Stmt q;
    char zUuid[HNAME_MAX+1];
    memcpy(zUuid, zXTag, nXTag);
    zUuid[nXTag] = 0;
    canonical16(zUuid, nXTag);
    rid = 0;
    if( zType[0]=='*' ){
      db_prepare(&q, "SELECT rid FROM blob WHERE uuid GLOB '%q*'", zUuid);
    }else{
      db_prepare(&q,
        "SELECT blob.rid"
        "  FROM blob CROSS JOIN event"
        " WHERE blob.uuid GLOB '%q*'"
        "   AND event.objid=blob.rid"
        "   AND event.type GLOB '%q'",
        zUuid, zType
      );
    }
    if( db_step(&q)==SQLITE_ROW ){
      rid = db_column_int(&q, 0);
      if( db_step(&q)==SQLITE_ROW ) rid = -1;
    }
    db_finalize(&q);
    if( rid ) return rid;
  }

  if( zType[0]=='w' ){
    zTagPrefix = "wiki";
  }
  /* Symbolic name */
  rid = db_int(0,
    "SELECT event.objid, max(event.mtime)"
    "  FROM tag, tagxref, event"
    " WHERE tag.tagname='%q-%q' "
    "   AND tagxref.tagid=tag.tagid AND tagxref.tagtype>0 "
    "   AND event.objid=tagxref.rid "
    "   AND event.type GLOB '%q'",
    zTagPrefix, zTag, zType
  );

  if( rid>0 ){
    if( startOfBranch ) rid = start_of_branch(rid,1);
    return rid;
  }

  /* Pure numeric date/time */
  zDate = fossil_expand_datetime(zTag, 0);
  if( zDate ){
    rid = db_int(0,
      "SELECT objid FROM event"
      " WHERE mtime<=julianday(%Q,fromLocal()) AND type GLOB '%q'"
      " ORDER BY mtime DESC LIMIT 1",
      fossil_roundup_date(zDate), zType);
    if( rid) return rid;
  }


  /* Undocumented:  numeric tags get translated directly into the RID */
  if( memcmp(zTag, "rid:", 4)==0 ){
    zTag += 4;
    for(i=0; fossil_isdigit(zTag[i]); i++){}
    if( zTag[i]==0 ){
      if( strcmp(zType,"*")==0 ){
        rid = atoi(zTag);
      }else{
        rid = db_int(0,
          "SELECT event.objid"
          "  FROM event"
          " WHERE event.objid=%s"
          "   AND event.type GLOB '%q'", zTag /*safe-for-%s*/, zType);
      }
    }
  }
  return rid;
}

/*
** This routine takes a user-entered string and tries to convert it to
** an artifact hash.
**
** We first try to treat the string as an artifact hash, or at least a
** unique prefix of an artifact hash. The input may be in mixed case.
** If we are passed such a string, this routine has the effect of
** converting the hash [prefix] to canonical form.
**
** If the input is not a hash or a hash prefix, then try to resolve
** the name as a tag.  If multiple tags match, pick the latest.
** A caller can force this routine to skip the hash case above by
** prefixing the string with "tag:", a useful property when the tag
** may be misinterpreted as a hex ASCII string. (e.g. "decade" or "facade")
**
** If the input is not a tag, then try to match it as an ISO-8601 date
** string YYYY-MM-DD HH:MM:SS and pick the nearest check-in to that date.
** If the input is of the form "date:*" then always resolve the name as
** a date. The forms "utc:*" and "local:" are deprecated.
**
** Return 0 on success.  Return 1 if the name cannot be resolved.
** Return 2 name is ambiguous.
*/
int name_to_uuid(Blob *pName, int iErrPriority, const char *zType){
  char *zName = blob_str(pName);
  int rid = symbolic_name_to_rid(zName, zType);
  if( rid<0 ){
    fossil_error(iErrPriority, "ambiguous name: %s", zName);
    return 2;
  }else if( rid==0 ){
    fossil_error(iErrPriority, "not found: %s", zName);
    return 1;
  }else{
    blob_reset(pName);
    db_blob(pName, "SELECT uuid FROM blob WHERE rid=%d", rid);
    return 0;
  }
}

/*
** This routine is similar to name_to_uuid() except in the form it
** takes its parameters and returns its value, and in that it does not
** treat errors as fatal. zName must be an artifact hash or prefix of
** a hash. zType is also as described for name_to_uuid(). If
** zName does not resolve, 0 is returned. If it is ambiguous, a
** negative value is returned. On success the rid is returned and
** pUuid (if it is not NULL) is set to a newly-allocated string,
** the full hash, which must eventually be free()d by the caller.
*/
int name_to_uuid2(const char *zName, const char *zType, char **pUuid){
  int rid = symbolic_name_to_rid(zName, zType);
  if((rid>0) && pUuid){
    *pUuid = db_text(NULL, "SELECT uuid FROM blob WHERE rid=%d", rid);
  }
  return rid;
}


/*
** name_collisions searches through events, blobs, and tickets for
** collisions of a given hash based on its length, counting only
** hashes greater than or equal to 4 hex ASCII characters (16 bits)
** in length.
*/
int name_collisions(const char *zName){
  int c = 0;         /* count of collisions for zName */
  int nLen;          /* length of zName */
  nLen = strlen(zName);
  if( nLen>=4 && nLen<=HNAME_MAX && validate16(zName, nLen) ){
    c = db_int(0,
      "SELECT"
      " (SELECT count(*) FROM ticket"
      "   WHERE tkt_uuid GLOB '%q*') +"
      " (SELECT count(*) FROM tag"
      "   WHERE tagname GLOB 'event-%q*') +"
      " (SELECT count(*) FROM blob"
      "   WHERE uuid GLOB '%q*');",
      zName, zName, zName
    );
    if( c<2 ) c = 0;
  }
  return c;
}

/*
** COMMAND: test-name-to-id
**
** Usage:  %fossil test-name-to-id [--count N] NAME
**
** Convert a NAME to a full artifact ID.  Repeat the conversion N
** times (for timing purposes) if the --count option is given.
*/
void test_name_to_id(void){
  int i;
  int n = 0;
  Blob name;
  db_must_be_within_tree();
  for(i=2; i<g.argc; i++){
    if( strcmp(g.argv[i],"--count")==0 && i+1<g.argc ){
      i++;
      n = atoi(g.argv[i]);
      continue;
    }
    do{
      blob_init(&name, g.argv[i], -1);
      fossil_print("%s -> ", g.argv[i]);
      if( name_to_uuid(&name, 1, "*") ){
        fossil_print("ERROR: %s\n", g.zErrMsg);
        fossil_error_reset();
      }else{
        fossil_print("%s\n", blob_buffer(&name));
      }
      blob_reset(&name);
    }while( n-- > 0 );
  }
}

/*
** Convert a name to a rid.  If the name can be any of the various forms
** accepted:
**
**   * artifact hash or prefix thereof
**   * symbolic name
**   * date
**   * label:date
**   * prev, previous
**   * next
**   * tip
**
** This routine is used by command-line routines to resolve command-line inputs
** into a rid.
*/
int name_to_typed_rid(const char *zName, const char *zType){
  int rid;

  if( zName==0 || zName[0]==0 ) return 0;
  rid = symbolic_name_to_rid(zName, zType);
  if( rid<0 ){
    fossil_fatal("ambiguous name: %s", zName);
  }else if( rid==0 ){
    fossil_fatal("not found: %s", zName);
  }
  return rid;
}
int name_to_rid(const char *zName){
  return name_to_typed_rid(zName, "*");
}

/*
** WEBPAGE: ambiguous
** URL: /ambiguous?name=NAME&src=WEBPAGE
**
** The NAME given by the name parameter is ambiguous.  Display a page
** that shows all possible choices and let the user select between them.
**
** The src= query parameter is optional.  If omitted it defaults
** to "info".
*/
void ambiguous_page(void){
  Stmt q;
  const char *zName = P("name");
  const char *zSrc = PD("src","info");
  char *z;

  if( zName==0 || zName[0]==0 || zSrc==0 || zSrc[0]==0 ){
    fossil_redirect_home();
  }
  style_header("Ambiguous Artifact ID");
  @ <p>The artifact hash prefix <b>%h(zName)</b> is ambiguous and might
  @ mean any of the following:
  @ <ol>
  z = mprintf("%s", zName);
  canonical16(z, strlen(z));
  db_prepare(&q, "SELECT uuid, rid FROM blob WHERE uuid GLOB '%q*'", z);
  while( db_step(&q)==SQLITE_ROW ){
    const char *zUuid = db_column_text(&q, 0);
    int rid = db_column_int(&q, 1);
    @ <li><p><a href="%R/%T(zSrc)/%!S(zUuid)">
    @ %s(zUuid)</a> -
    object_description(rid, 0, 0, 0);
    @ </p></li>
  }
  db_finalize(&q);
  db_prepare(&q,
    "   SELECT tkt_rid, tkt_uuid, title"
    "     FROM ticket, ticketchng"
    "    WHERE ticket.tkt_id = ticketchng.tkt_id"
    "      AND tkt_uuid GLOB '%q*'"
    " GROUP BY tkt_uuid"
    " ORDER BY tkt_ctime DESC", z);
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q, 0);
    const char *zUuid = db_column_text(&q, 1);
    const char *zTitle = db_column_text(&q, 2);
    @ <li><p><a href="%R/%T(zSrc)/%!S(zUuid)">
    @ %s(zUuid)</a> -
    @ <ul></ul>
    @ Ticket
    hyperlink_to_version(zUuid);
    @ - %h(zTitle).
    @ <ul><li>
    object_description(rid, 0, 0, 0);
    @ </li></ul>
    @ </p></li>
  }
  db_finalize(&q);
  db_prepare(&q,
    "SELECT rid, uuid FROM"
    "  (SELECT tagxref.rid AS rid, substr(tagname, 7) AS uuid"
    "     FROM tagxref, tag WHERE tagxref.tagid = tag.tagid"
    "      AND tagname GLOB 'event-%q*') GROUP BY uuid", z);
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q, 0);
    const char* zUuid = db_column_text(&q, 1);
    @ <li><p><a href="%R/%T(zSrc)/%!S(zUuid)">
    @ %s(zUuid)</a> -
    @ <ul><li>
    object_description(rid, 0, 0, 0);
    @ </li></ul>
    @ </p></li>
  }
  @ </ol>
  db_finalize(&q);
  style_footer();
}

/*
** Convert the name in CGI parameter zParamName into a rid and return that
** rid.  If the CGI parameter is missing or is not a valid artifact tag,
** return 0.  If the CGI parameter is ambiguous, redirect to a page that
** shows all possibilities and do not return.
*/
int name_to_rid_www(const char *zParamName){
  int rid;
  const char *zName = P(zParamName);
#ifdef FOSSIL_ENABLE_JSON
  if(!zName && fossil_has_json()){
    zName = json_find_option_cstr(zParamName,NULL,NULL);
  }
#endif
  if( zName==0 || zName[0]==0 ) return 0;
  rid = symbolic_name_to_rid(zName, "*");
  if( rid<0 ){
    cgi_redirectf("%R/ambiguous/%T?src=%t", zName, g.zPath);
    rid = 0;
  }
  return rid;
}

/*
** Generate a description of artifact "rid"
*/
void whatis_rid(int rid, int verboseFlag){
  Stmt q;
  int cnt;

  /* Basic information about the object. */
  db_prepare(&q,
     "SELECT uuid, size, datetime(mtime,toLocal()), ipaddr"
     "  FROM blob, rcvfrom"
     " WHERE rid=%d"
     "   AND rcvfrom.rcvid=blob.rcvid",
     rid);
  if( db_step(&q)==SQLITE_ROW ){
    if( verboseFlag ){
      fossil_print("artifact:   %s (%d)\n", db_column_text(&q,0), rid);
      fossil_print("size:       %d bytes\n", db_column_int(&q,1));
      fossil_print("received:   %s from %s\n",
         db_column_text(&q, 2),
         db_column_text(&q, 3));
    }else{
      fossil_print("artifact:   %s\n", db_column_text(&q,0));
      fossil_print("size:       %d bytes\n", db_column_int(&q,1));
    }
  }
  db_finalize(&q);

  /* Report any symbolic tags on this artifact */
  db_prepare(&q,
    "SELECT substr(tagname,5)"
    "  FROM tag JOIN tagxref ON tag.tagid=tagxref.tagid"
    " WHERE tagxref.rid=%d"
    "   AND tagname GLOB 'sym-*'"
    " ORDER BY 1",
    rid
  );
  cnt = 0;
  while( db_step(&q)==SQLITE_ROW ){
    const char *zPrefix = cnt++ ? ", " : "tags:       ";
    fossil_print("%s%s", zPrefix, db_column_text(&q,0));
  }
  if( cnt ) fossil_print("\n");
  db_finalize(&q);

  /* Report any HIDDEN, PRIVATE, CLUSTER, or CLOSED tags on this artifact */
  db_prepare(&q,
    "SELECT tagname"
    "  FROM tag JOIN tagxref ON tag.tagid=tagxref.tagid"
    " WHERE tagxref.rid=%d"
    "   AND tag.tagid IN (5,6,7,9)"
    " ORDER BY 1",
    rid
  );
  cnt = 0;
  while( db_step(&q)==SQLITE_ROW ){
    const char *zPrefix = cnt++ ? ", " : "raw-tags:   ";
    fossil_print("%s%s", zPrefix, db_column_text(&q,0));
  }
  if( cnt ) fossil_print("\n");
  db_finalize(&q);

  /* Check for entries on the timeline that reference this object */
  db_prepare(&q,
     "SELECT type, datetime(mtime,toLocal()),"
     "       coalesce(euser,user), coalesce(ecomment,comment)"
     "  FROM event WHERE objid=%d", rid);
  if( db_step(&q)==SQLITE_ROW ){
    const char *zType;
    switch( db_column_text(&q,0)[0] ){
      case 'c':  zType = "Check-in";       break;
      case 'w':  zType = "Wiki-edit";      break;
      case 'e':  zType = "Technote";       break;
      case 'f':  zType = "Forum-post";     break;
      case 't':  zType = "Ticket-change";  break;
      case 'g':  zType = "Tag-change";     break;
      default:   zType = "Unknown";        break;
    }
    fossil_print("type:       %s by %s on %s\n", zType, db_column_text(&q,2),
                 db_column_text(&q, 1));
    fossil_print("comment:    ");
    comment_print(db_column_text(&q,3), 0, 12, -1, get_comment_format());
    cnt++;
  }
  db_finalize(&q);

  /* Check to see if this object is used as a file in a check-in */
  db_prepare(&q,
    "SELECT filename.name, blob.uuid, datetime(event.mtime,toLocal()),"
    "       coalesce(euser,user), coalesce(ecomment,comment)"
    "  FROM mlink, filename, blob, event"
    " WHERE mlink.fid=%d"
    "   AND filename.fnid=mlink.fnid"
    "   AND event.objid=mlink.mid"
    "   AND blob.rid=mlink.mid"
    " ORDER BY event.mtime DESC /*sort*/",
    rid);
  while( db_step(&q)==SQLITE_ROW ){
    fossil_print("file:       %s\n", db_column_text(&q,0));
    fossil_print("            part of [%S] by %s on %s\n",
      db_column_text(&q, 1),
      db_column_text(&q, 3),
      db_column_text(&q, 2));
    fossil_print("            ");
    comment_print(db_column_text(&q,4), 0, 12, -1, get_comment_format());
    cnt++;
  }
  db_finalize(&q);

  /* Check to see if this object is used as an attachment */
  db_prepare(&q,
    "SELECT attachment.filename,"
    "       attachment.comment,"
    "       attachment.user,"
    "       datetime(attachment.mtime,toLocal()),"
    "       attachment.target,"
    "       CASE WHEN EXISTS(SELECT 1 FROM tag WHERE tagname=('tkt-'||target))"
    "            THEN 'ticket'"
    "       WHEN EXISTS(SELECT 1 FROM tag WHERE tagname=('wiki-'||target))"
    "            THEN 'wiki' END,"
    "       attachment.attachid,"
    "       (SELECT uuid FROM blob WHERE rid=attachid)"
    "  FROM attachment JOIN blob ON attachment.src=blob.uuid"
    " WHERE blob.rid=%d",
    rid
  );
  while( db_step(&q)==SQLITE_ROW ){
    fossil_print("attachment: %s\n", db_column_text(&q,0));
    fossil_print("            attached to %s %s\n",
                 db_column_text(&q,5), db_column_text(&q,4));
    if( verboseFlag ){
      fossil_print("            via %s (%d)\n",
                   db_column_text(&q,7), db_column_int(&q,6));
    }else{
      fossil_print("            via %s\n",
                   db_column_text(&q,7));
    }
    fossil_print("            by user %s on %s\n",
                 db_column_text(&q,2), db_column_text(&q,3));
    fossil_print("            ");
    comment_print(db_column_text(&q,1), 0, 12, -1, get_comment_format());
    cnt++;
  }
  db_finalize(&q);

  /* If other information available, try to describe the object */
  if( cnt==0 ){
    char *zWhere = mprintf("=%d", rid);
    char *zDesc;
    describe_artifacts(zWhere);
    free(zWhere);
    zDesc = db_text(0,
       "SELECT printf('%%-12s%%s %%s',type||':',summary,substr(ref,1,16))"
       "  FROM description WHERE rid=%d", rid);
    fossil_print("%s\n", zDesc);
    fossil_free(zDesc);
  }
}

/*
** COMMAND: whatis*
**
** Usage: %fossil whatis NAME
**
** Resolve the symbol NAME into its canonical artifact hash
** artifact name and provide a description of what role that artifact
** plays.
**
** Options:
**
**    --type TYPE          Only find artifacts of TYPE (one of: 'ci', 't',
**                         'w', 'g', or 'e').
**    -v|--verbose         Provide extra information (such as the RID)
*/
void whatis_cmd(void){
  int rid;
  const char *zName;
  int verboseFlag;
  int i;
  const char *zType = 0;
  db_find_and_open_repository(0,0);
  verboseFlag = find_option("verbose","v",0)!=0;
  zType = find_option("type",0,1);

  /* We should be done with options.. */
  verify_all_options();

  if( g.argc<3 ) usage("NAME ...");
  for(i=2; i<g.argc; i++){
    zName = g.argv[i];
    if( i>2 ) fossil_print("%.79c\n",'-');
    rid = symbolic_name_to_rid(zName, zType);
    if( rid<0 ){
      Stmt q;
      int cnt = 0;
      fossil_print("name:       %s (ambiguous)\n", zName);
      db_prepare(&q,
         "SELECT rid FROM blob WHERE uuid>=lower(%Q) AND uuid<(lower(%Q)||'z')",
         zName, zName
      );
      while( db_step(&q)==SQLITE_ROW ){
        if( cnt++ ) fossil_print("%12s---- meaning #%d ----\n", " ", cnt);
        whatis_rid(db_column_int(&q, 0), verboseFlag);
      }
      db_finalize(&q);
    }else if( rid==0 ){
                 /* 0123456789 12 */
      fossil_print("unknown:    %s\n", zName);
    }else{
      fossil_print("name:       %s\n", zName);
      whatis_rid(rid, verboseFlag);
    }
  }
}

/*
** COMMAND: test-whatis-all
**
** Usage: %fossil test-whatis-all
**
** Show "whatis" information about every artifact in the repository
*/
void test_whatis_all_cmd(void){
  Stmt q;
  int cnt = 0;
  db_find_and_open_repository(0,0);
  db_prepare(&q, "SELECT rid FROM blob ORDER BY rid");
  while( db_step(&q)==SQLITE_ROW ){
    if( cnt++ ) fossil_print("%.79c\n", '-');
    whatis_rid(db_column_int(&q,0), 1);
  }
  db_finalize(&q);
}


/*
** COMMAND: test-ambiguous
**
** Usage: %fossil test-ambiguous [--minsize N]
**
** Show a list of ambiguous artifact hash abbreviations of N characters or
** more where N defaults to 4.  Change N to a different value using
** the "--minsize N" command-line option.
*/
void test_ambiguous_cmd(void){
  Stmt q, ins;
  int i;
  int minSize = 4;
  const char *zMinsize;
  char zPrev[100];
  db_find_and_open_repository(0,0);
  zMinsize = find_option("minsize",0,1);
  if( zMinsize && atoi(zMinsize)>0 ) minSize = atoi(zMinsize);
  db_multi_exec("CREATE TEMP TABLE dups(uuid, cnt)");
  db_prepare(&ins,"INSERT INTO dups(uuid) VALUES(substr(:uuid,1,:cnt))");
  db_prepare(&q,
    "SELECT uuid FROM blob "
    "UNION "
    "SELECT substr(tagname,7) FROM tag WHERE tagname GLOB 'event-*' "
    "UNION "
    "SELECT tkt_uuid FROM ticket "
    "ORDER BY 1"
  );
  zPrev[0] = 0;
  while( db_step(&q)==SQLITE_ROW ){
    const char *zUuid = db_column_text(&q, 0);
    for(i=0; zUuid[i]==zPrev[i] && zUuid[i]!=0; i++){}
    if( i>=minSize ){
      db_bind_int(&ins, ":cnt", i);
      db_bind_text(&ins, ":uuid", zUuid);
      db_step(&ins);
      db_reset(&ins);
    }
    sqlite3_snprintf(sizeof(zPrev), zPrev, "%s", zUuid);
  }
  db_finalize(&ins);
  db_finalize(&q);
  db_prepare(&q, "SELECT uuid FROM dups ORDER BY length(uuid) DESC, uuid");
  while( db_step(&q)==SQLITE_ROW ){
    fossil_print("%s\n", db_column_text(&q, 0));
  }
  db_finalize(&q);
}

/*
** Schema for the description table
*/
static const char zDescTab[] =
@ CREATE TEMP TABLE IF NOT EXISTS description(
@   rid INTEGER PRIMARY KEY,       -- RID of the object
@   uuid TEXT,                     -- hash of the object
@   ctime DATETIME,                -- Time of creation
@   isPrivate BOOLEAN DEFAULT 0,   -- True for unpublished artifacts
@   type TEXT,                     -- file, checkin, wiki, ticket, etc.
@   rcvid INT,                     -- When the artifact was received
@   summary TEXT,                  -- Summary comment for the object
@   ref TEXT                       -- hash of an object to link against
@ );
@ CREATE INDEX IF NOT EXISTS desctype
@   ON description(summary) WHERE summary='unknown';
;

/*
** Attempt to describe all phantom artifacts.  The artifacts are
** already loaded into the description table and have summary='unknown'.
** This routine attempts to generate a better summary, and possibly
** fill in the ref field.
*/
static void describe_unknown_artifacts(){
  /* Try to figure out the origin of unknown artifacts */
  db_multi_exec(
    "REPLACE INTO description(rid,uuid,isPrivate,type,summary,ref)\n"
    "  SELECT description.rid, description.uuid, isPrivate, type,\n"
    "         CASE WHEN plink.isprim THEN '' ELSE 'merge ' END ||\n"
    "         'parent of check-in', blob.uuid\n"
    "    FROM description, plink, blob\n"
    "   WHERE description.summary='unknown'\n"
    "     AND plink.pid=description.rid\n"
    "     AND blob.rid=plink.cid;"
  );
  db_multi_exec(
    "REPLACE INTO description(rid,uuid,isPrivate,type,summary,ref)\n"
    "  SELECT description.rid, description.uuid, isPrivate, type,\n"
    "         'child of check-in', blob.uuid\n"
    "    FROM description, plink, blob\n"
    "   WHERE description.summary='unknown'\n"
    "     AND plink.cid=description.rid\n"
    "     AND blob.rid=plink.pid;"
  );
  db_multi_exec(
    "REPLACE INTO description(rid,uuid,isPrivate,type,summary,ref)\n"
    "  SELECT description.rid, description.uuid, isPrivate, type,\n"
    "         'check-in referenced by \"'||tag.tagname ||'\" tag',\n"
    "         blob.uuid\n"
    "    FROM description, tagxref, tag, blob\n"
    "   WHERE description.summary='unknown'\n"
    "     AND tagxref.origid=description.rid\n"
    "     AND tag.tagid=tagxref.tagid\n"
    "     AND blob.rid=tagxref.srcid;"
  );
  db_multi_exec(
    "REPLACE INTO description(rid,uuid,isPrivate,type,summary,ref)\n"
    "  SELECT description.rid, description.uuid, isPrivate, type,\n"
    "         'file \"'||filename.name||'\"',\n"
    "         blob.uuid\n"
    "    FROM description, mlink, filename, blob\n"
    "   WHERE description.summary='unknown'\n"
    "     AND mlink.fid=description.rid\n"
    "     AND blob.rid=mlink.mid\n"
    "     AND filename.fnid=mlink.fnid;"
  );
  if( !db_exists("SELECT 1 FROM description WHERE summary='unknown'") ){
    return;
  }
  add_content_sql_commands(g.db);
  db_multi_exec(
    "REPLACE INTO description(rid,uuid,isPrivate,type,summary,ref)\n"
    "  SELECT description.rid, description.uuid, isPrivate, type,\n"
    "         'referenced by cluster', blob.uuid\n"
    "    FROM description, tagxref, blob\n"
    "   WHERE description.summary='unknown'\n"
    "     AND tagxref.tagid=(SELECT tagid FROM tag WHERE tagname='cluster')\n"
    "     AND blob.rid=tagxref.rid\n"
    "     AND CAST(content(blob.uuid) AS text)"
    "                   GLOB ('*M '||description.uuid||'*');"
  );
}

/*
** Create the description table if it does not already exists.
** Populate fields of this table with descriptions for all artifacts
** whose RID matches the SQL expression in zWhere.
*/
void describe_artifacts(const char *zWhere){
  db_multi_exec("%s", zDescTab/*safe-for-%s*/);

  /* Describe check-ins */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, event.mtime, 'checkin',\n"
          " 'check-in to '\n"
          " ||  coalesce((SELECT value FROM tagxref WHERE tagid=%d"
                     "   AND tagtype>0 AND tagxref.rid=blob.rid),'trunk')\n"
          " || ' by ' || coalesce(event.euser,event.user)\n"
          " || ' on ' || strftime('%%Y-%%m-%%d %%H:%%M',event.mtime)\n"
    "  FROM event, blob\n"
    " WHERE (event.objid %s) AND event.type='ci'\n"
    "   AND event.objid=blob.rid;",
    TAG_BRANCH, zWhere /*safe-for-%s*/
  );

  /* Describe files */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, event.mtime,"
    "       'file', 'file '||filename.name\n"
    "  FROM mlink, blob, event, filename\n"
    " WHERE (mlink.fid %s)\n"
    "   AND mlink.mid=event.objid\n"
    "   AND filename.fnid=mlink.fnid\n"
    "   AND mlink.fid=blob.rid;",
    zWhere /*safe-for-%s*/
  );

  /* Describe tags */
  db_multi_exec(
   "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, tagxref.mtime, 'tag',\n"
    "     'tag '||substr((SELECT uuid FROM blob WHERE rid=tagxref.rid),1,16)\n"
    "  FROM tagxref, blob\n"
    " WHERE (tagxref.srcid %s) AND tagxref.srcid!=tagxref.rid\n"
    "   AND tagxref.srcid=blob.rid;",
    zWhere /*safe-for-%s*/
  );

  /* Cluster artifacts */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, rcvfrom.mtime,"
    "       'cluster', 'cluster'\n"
    "  FROM tagxref, blob, rcvfrom\n"
    " WHERE (tagxref.rid %s)\n"
    "   AND tagxref.tagid=(SELECT tagid FROM tag WHERE tagname='cluster')\n"
    "   AND blob.rid=tagxref.rid"
    "   AND rcvfrom.rcvid=blob.rcvid;",
    zWhere /*safe-for-%s*/
  );

  /* Ticket change artifacts */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, tagxref.mtime, 'ticket',\n"
    "       'ticket '||substr(tag.tagname,5,21)\n"
    "  FROM tagxref, tag, blob\n"
    " WHERE (tagxref.rid %s)\n"
    "   AND tag.tagid=tagxref.tagid\n"
    "   AND tag.tagname GLOB 'tkt-*'"
    "   AND blob.rid=tagxref.rid;",
    zWhere /*safe-for-%s*/
  );

  /* Wiki edit artifacts */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, tagxref.mtime, 'wiki',\n"
    "       printf('wiki \"%%s\"',substr(tag.tagname,6))\n"
    "  FROM tagxref, tag, blob\n"
    " WHERE (tagxref.rid %s)\n"
    "   AND tag.tagid=tagxref.tagid\n"
    "   AND tag.tagname GLOB 'wiki-*'"
    "   AND blob.rid=tagxref.rid;",
    zWhere /*safe-for-%s*/
  );

  /* Event edit artifacts */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, tagxref.mtime, 'event',\n"
    "       'event '||substr(tag.tagname,7)\n"
    "  FROM tagxref, tag, blob\n"
    " WHERE (tagxref.rid %s)\n"
    "   AND tag.tagid=tagxref.tagid\n"
    "   AND tag.tagname GLOB 'event-*'"
    "   AND blob.rid=tagxref.rid;",
    zWhere /*safe-for-%s*/
  );

  /* Attachments */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, attachment.mtime,"
    "       'attach-control',\n"
    "       'attachment-control for '||attachment.filename\n"
    "  FROM attachment, blob\n"
    " WHERE (attachment.attachid %s)\n"
    "   AND blob.rid=attachment.attachid",
    zWhere /*safe-for-%s*/
  );
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
    "SELECT blob.rid, blob.uuid, blob.rcvid, attachment.mtime, 'attachment',\n"
    "       'attachment '||attachment.filename\n"
    "  FROM attachment, blob\n"
    " WHERE (blob.rid %s)\n"
    "   AND blob.rid NOT IN (SELECT rid FROM description)\n"
    "   AND blob.uuid=attachment.src",
    zWhere /*safe-for-%s*/
  );

  /* Forum posts */
  if( db_table_exists("repository","forumpost") ){
    db_multi_exec(
      "INSERT OR IGNORE INTO description(rid,uuid,rcvid,ctime,type,summary)\n"
      "SELECT postblob.rid, postblob.uuid, postblob.rcvid,"
      "       forumpost.fmtime, 'forumpost',\n"
      "       CASE WHEN fpid=froot THEN 'forum-post '\n"
      "            ELSE 'forum-reply-to ' END || substr(rootblob.uuid,1,14)\n"
      "  FROM forumpost, blob AS postblob, blob AS rootblob\n"
      " WHERE (forumpost.fpid %s)\n"
      "   AND postblob.rid=forumpost.fpid"
      "   AND rootblob.rid=forumpost.froot",
      zWhere /*safe-for-%s*/
    );
  }

  /* Mark all other artifacts as "unknown" for now */
  db_multi_exec(
    "INSERT OR IGNORE INTO description(rid,uuid,rcvid,type,summary)\n"
    "SELECT blob.rid, blob.uuid,blob.rcvid,\n"
    "       CASE WHEN EXISTS(SELECT 1 FROM phantom WHERE rid=blob.rid)\n"
           " THEN 'phantom' ELSE '' END,\n"
    "       'unknown'\n"
    "  FROM blob\n"
    " WHERE (blob.rid %s)\n"
    "   AND (blob.rid NOT IN (SELECT rid FROM description));",
    zWhere /*safe-for-%s*/
  );

  /* Mark private elements */
  db_multi_exec(
   "UPDATE description SET isPrivate=1 WHERE rid IN private"
  );

  if( db_exists("SELECT 1 FROM description WHERE summary='unknown'") ){
    describe_unknown_artifacts();
  }
}

/*
** Print the content of the description table on stdout.
**
** The description table is computed using the WHERE clause zWhere if
** the zWhere parameter is not NULL.  If zWhere is NULL, then this
** routine assumes that the description table already exists and is
** populated and merely prints the contents.
*/
int describe_artifacts_to_stdout(const char *zWhere, const char *zLabel){
  Stmt q;
  int cnt = 0;
  if( zWhere!=0 ) describe_artifacts(zWhere);
  db_prepare(&q,
    "SELECT uuid, summary, coalesce(ref,''), isPrivate\n"
    "  FROM description\n"
    " ORDER BY ctime, type;"
  );
  while( db_step(&q)==SQLITE_ROW ){
    if( zLabel ){
      fossil_print("%s\n", zLabel);
      zLabel = 0;
    }
    fossil_print("  %.16s %s %s", db_column_text(&q,0),
           db_column_text(&q,1), db_column_text(&q,2));
    if( db_column_int(&q,3) ) fossil_print(" (private)");
    fossil_print("\n");
    cnt++;
  }
  db_finalize(&q);
  if( zWhere!=0 ) db_multi_exec("DELETE FROM description;");
  return cnt;
}

/*
** COMMAND: test-describe-artifacts
**
** Usage: %fossil test-describe-artifacts [--from S] [--count N]
**
** Display a one-line description of every artifact.
*/
void test_describe_artifacts_cmd(void){
  int iFrom = 0;
  int iCnt = 1000000;
  const char *z;
  char *zRange;
  db_find_and_open_repository(0,0);
  z = find_option("from",0,1);
  if( z ) iFrom = atoi(z);
  z = find_option("count",0,1);
  if( z ) iCnt = atoi(z);
  zRange = mprintf("BETWEEN %d AND %d", iFrom, iFrom+iCnt-1);
  describe_artifacts_to_stdout(zRange, 0);
}

/*
** WEBPAGE: bloblist
**
** Return a page showing all artifacts in the repository.  Query parameters:
**
**   n=N         Show N artifacts
**   s=S         Start with artifact number S
**   priv        Show only unpublished or private artifacts
**   phan        Show only phantom artifacts
**   hclr        Color code hash types (SHA1 vs SHA3)
*/
void bloblist_page(void){
  Stmt q;
  int s = atoi(PD("s","0"));
  int n = atoi(PD("n","5000"));
  int mx = db_int(0, "SELECT max(rid) FROM blob");
  int privOnly = PB("priv");
  int phantomOnly = PB("phan");
  int hashClr = PB("hclr");
  char *zRange;
  char *zSha1Bg;
  char *zSha3Bg;

  login_check_credentials();
  if( !g.perm.Read ){ login_needed(g.anon.Read); return; }
  style_header("List Of Artifacts");
  style_submenu_element("250 Largest", "bigbloblist");
  if( g.perm.Admin ){
    style_submenu_element("Artifact Log", "rcvfromlist");
  }
  if( !phantomOnly ){
    style_submenu_element("Phantoms", "bloblist?phan");
  }
  if( g.perm.Private || g.perm.Admin ){
    if( !privOnly ){
      style_submenu_element("Private", "bloblist?priv");
    }
  }else{
    privOnly = 0;
  }
  if( g.perm.Write ){
    style_submenu_element("Artifact Stats", "artifact_stats");
  }
  if( !privOnly && !phantomOnly && mx>n && P("s")==0 ){
    int i;
    @ <p>Select a range of artifacts to view:</p>
    @ <ul>
    for(i=1; i<=mx; i+=n){
      @ <li> %z(href("%R/bloblist?s=%d&n=%d",i,n))
      @ %d(i)..%d(i+n-1<mx?i+n-1:mx)</a>
    }
    @ </ul>
    style_footer();
    return;
  }
  if( phantomOnly || privOnly || mx>n ){
    style_submenu_element("Index", "bloblist");
  }
  if( privOnly ){
    zRange = mprintf("IN private");
  }else if( phantomOnly ){
    zRange = mprintf("IN phantom");
  }else{
    zRange = mprintf("BETWEEN %d AND %d", s, s+n-1);
  }
  describe_artifacts(zRange);
  fossil_free(zRange);
  db_prepare(&q,
    "SELECT rid, uuid, summary, isPrivate, type='phantom', rcvid, ref"
    "  FROM description ORDER BY rid"
  );
  if( skin_detail_boolean("white-foreground") ){
    zSha1Bg = "#714417";
    zSha3Bg = "#177117";
  }else{
    zSha1Bg = "#ebffb0";
    zSha3Bg = "#b0ffb0";
  }
  @ <table cellpadding="2" cellspacing="0" border="1">
  if( g.perm.Admin ){
    @ <tr><th>RID<th>Hash<th>Rcvid<th>Description<th>Ref<th>Remarks
  }else{
    @ <tr><th>RID<th>Hash<th>Description<th>Ref<th>Remarks
  }
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q,0);
    const char *zUuid = db_column_text(&q, 1);
    const char *zDesc = db_column_text(&q, 2);
    int isPriv = db_column_int(&q,3);
    int isPhantom = db_column_int(&q,4);
    const char *zRef = db_column_text(&q,6);
    if( isPriv && !isPhantom && !g.perm.Private && !g.perm.Admin ){
      /* Don't show private artifacts to users without Private (x) permission */
      continue;
    }
    if( hashClr ){
      const char *zClr = db_column_bytes(&q,1)>40 ? zSha3Bg : zSha1Bg;
      @ <tr style='background-color:%s(zClr);'><td align="right">%d(rid)</td>
    }else{
      @ <tr><td align="right">%d(rid)</td>
    }
    @ <td>&nbsp;%z(href("%R/info/%!S",zUuid))%S(zUuid)</a>&nbsp;</td>
    if( g.perm.Admin ){
      int rcvid = db_column_int(&q,5);
      if( rcvid<=0 ){
        @ <td>&nbsp;
      }else{
        @ <td><a href='%R/rcvfrom?rcvid=%d(rcvid)'>%d(rcvid)</a>
      }
    }
    @ <td align="left">%h(zDesc)</td>
    if( zRef && zRef[0] ){
      @ <td>%z(href("%R/info/%!S",zRef))%S(zRef)</a>
    }else{
      @ <td>&nbsp;
    }
    if( isPriv || isPhantom ){
      if( isPriv==0 ){
        @ <td>phantom</td>
      }else if( isPhantom==0 ){
        @ <td>private</td>
      }else{
        @ <td>private,phantom</td>
      }
    }else{
      @ <td>&nbsp;
    }
    @ </tr>
  }
  @ </table>
  db_finalize(&q);
  style_footer();
}

/*
** Output HTML that shows a table of all public phantoms.
*/
void table_of_public_phantoms(void){
  Stmt q;
  char *zRange;
  zRange = mprintf("IN (SELECT rid FROM phantom EXCEPT"
                   " SELECT rid FROM private)");
  describe_artifacts(zRange);
  fossil_free(zRange);
  db_prepare(&q,
    "SELECT rid, uuid, summary, ref"
    "  FROM description ORDER BY rid"
  );
  @ <table cellpadding="2" cellspacing="0" border="1">
  @ <tr><th>RID<th>Description<th>Source
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q,0);
    const char *zUuid = db_column_text(&q, 1);
    const char *zDesc = db_column_text(&q, 2);
    const char *zRef = db_column_text(&q,3);
    @ <tr><td valign="top">%d(rid)</td>
    @ <td valign="top" align="left">%h(zUuid)<br>%h(zDesc)</td>
    if( zRef && zRef[0] ){
      @ <td valign="top">%z(href("%R/info/%!S",zRef))%!S(zRef)</a>
    }else{
      @ <td>&nbsp;
    }
    @ </tr>
  }
  @ </table>
  db_finalize(&q);
}

/*
** WEBPAGE: phantoms
**
** Show a list of all "phantom" artifacts that are not marked as "private".
**
** A "phantom" artifact is an artifact whose hash named appears in some
** artifact but whose content is unknown.  For example, if a manifest
** references a particular SHA3 hash of a file, but that SHA3 hash is
** not on the shunning list and is not in the database, then the file
** is a phantom.  We know it exists, but we do not know its content.
**
** Whenever a sync occurs, both each party looks at its phantom list
** and for every phantom that is not also marked private, it asks the
** other party to send it the content.  This mechanism helps keep all
** repositories synced up.
**
** This page is similar to the /bloblist page in that it lists artifacts.
** But this page is a special case in that it only shows phantoms that
** are not private.  In other words, this page shows all phantoms that
** generate extra network traffic on every sync request.
*/
void phantom_list_page(void){
  login_check_credentials();
  if( !g.perm.Read ){ login_needed(g.anon.Read); return; }
  style_header("Public Phantom Artifacts");
  if( g.perm.Admin ){
    style_submenu_element("Artifact Log", "rcvfromlist");
    style_submenu_element("Artifact List", "bloblist");
  }
  if( g.perm.Write ){
    style_submenu_element("Artifact Stats", "artifact_stats");
  }
  table_of_public_phantoms();
  style_footer();
}

/*
** WEBPAGE: bigbloblist
**
** Return a page showing the largest artifacts in the repository in order
** of decreasing size.
**
**   n=N         Show the top N artifacts
*/
void bigbloblist_page(void){
  Stmt q;
  int n = atoi(PD("n","250"));

  login_check_credentials();
  if( !g.perm.Read ){ login_needed(g.anon.Read); return; }
  if( g.perm.Admin ){
    style_submenu_element("Artifact Log", "rcvfromlist");
  }
  if( g.perm.Write ){
    style_submenu_element("Artifact Stats", "artifact_stats");
  }
  style_submenu_element("All Artifacts", "bloblist");
  style_header("%d Largest Artifacts", n);
  db_multi_exec(
    "CREATE TEMP TABLE toshow(rid INTEGER PRIMARY KEY);"
    "INSERT INTO toshow(rid)"
    "  SELECT rid FROM blob"
    "   ORDER BY length(content) DESC"
    "   LIMIT %d;", n
  );
  describe_artifacts("IN toshow");
  db_prepare(&q,
    "SELECT description.rid, description.uuid, description.summary,"
    "       length(blob.content), coalesce(delta.srcid,''),"
    "       datetime(description.ctime)"
    "  FROM description, blob LEFT JOIN delta ON delta.rid=blob.rid"
    " WHERE description.rid=blob.rid"
    " ORDER BY length(content) DESC"
  );
  @ <table cellpadding="2" cellspacing="0" border="1" \
  @  class='sortable' data-column-types='NnnttT' data-init-sort='0'>
  @ <thead><tr><th align="right">Size<th align="right">RID
  @ <th align="right">Delta From<th>Hash<th>Description<th>Date</tr></thead>
  @ <tbody>
  while( db_step(&q)==SQLITE_ROW ){
    int rid = db_column_int(&q,0);
    const char *zUuid = db_column_text(&q, 1);
    const char *zDesc = db_column_text(&q, 2);
    int sz = db_column_int(&q,3);
    const char *zSrcId = db_column_text(&q,4);
    const char *zDate = db_column_text(&q,5);
    @ <tr><td align="right">%d(sz)</td>
    @ <td align="right">%d(rid)</td>
    @ <td align="right">%s(zSrcId)</td>
    @ <td>&nbsp;%z(href("%R/info/%!S",zUuid))%S(zUuid)</a>&nbsp;</td>
    @ <td align="left">%h(zDesc)</td>
    @ <td align="left">%z(href("%R/timeline?c=%T",zDate))%s(zDate)</a></td>
    @ </tr>
  }
  @ </tbody></table>
  db_finalize(&q);
  style_table_sorter();
  style_footer();
}

/*
** COMMAND: test-unsent
**
** Usage: %fossil test-unsent
**
** Show all artifacts in the unsent table
*/
void test_unsent_cmd(void){
  db_find_and_open_repository(0,0);
  describe_artifacts_to_stdout("IN unsent", 0);
}

/*
** COMMAND: test-unclustered
**
** Usage: %fossil test-unclustered
**
** Show all artifacts in the unclustered table
*/
void test_unclusterd_cmd(void){
  db_find_and_open_repository(0,0);
  describe_artifacts_to_stdout("IN unclustered", 0);
}

/*
** COMMAND: test-phantoms
**
** Usage: %fossil test-phantoms
**
** Show all phantom artifacts
*/
void test_phatoms_cmd(void){
  db_find_and_open_repository(0,0);
  describe_artifacts_to_stdout("IN (SELECT rid FROM blob WHERE size<0)", 0);
}

/* Maximum number of collision examples to remember */
#define MAX_COLLIDE 25

/*
** Generate a report on the number of collisions in artifact hashes
** generated by the SQL given in the argument.
*/
static void collision_report(const char *zSql){
  int i, j, kk;
  int nHash = 0;
  Stmt q;
  char zPrev[HNAME_MAX+1];
  struct {
    int cnt;
    char *azHit[MAX_COLLIDE];
    char z[HNAME_MAX+1];
  } aCollide[HNAME_MAX+1];
  memset(aCollide, 0, sizeof(aCollide));
  memset(zPrev, 0, sizeof(zPrev));
  db_prepare(&q,"%s",zSql/*safe-for-%s*/);
  while( db_step(&q)==SQLITE_ROW ){
    const char *zUuid = db_column_text(&q,0);
    int n = db_column_bytes(&q,0);
    int i;
    nHash++;
    for(i=0; zPrev[i] && zPrev[i]==zUuid[i]; i++){}
    if( i>0 && i<=HNAME_MAX ){
      if( i>=4 && aCollide[i].cnt<MAX_COLLIDE ){
        aCollide[i].azHit[aCollide[i].cnt] = mprintf("%.*s", i, zPrev);
      }
      aCollide[i].cnt++;
      if( aCollide[i].z[0]==0 ) memcpy(aCollide[i].z, zPrev, n+1);
    }
    memcpy(zPrev, zUuid, n+1);
  }
  db_finalize(&q);
  @ <table border=1><thead>
  @ <tr><th>Length<th>Instances<th>First Instance</tr>
  @ </thead><tbody>
  for(i=1; i<=HNAME_MAX; i++){
    if( aCollide[i].cnt==0 ) continue;
    @ <tr><td>%d(i)<td>%d(aCollide[i].cnt)<td>%h(aCollide[i].z)</tr>
  }
  @ </tbody></table>
  @ <p>Total number of hashes: %d(nHash)</p>
  kk = 0;
  for(i=HNAME_MAX; i>=4; i--){
    if( aCollide[i].cnt==0 ) continue;
    if( aCollide[i].cnt>200 ) break;
    kk += aCollide[i].cnt;
    if( aCollide[i].cnt<25 ){
      @ <p>Collisions of length %d(i):
    }else{
      @ <p>First 25 collisions of length %d(i):
    }
    for(j=0; j<aCollide[i].cnt && j<MAX_COLLIDE; j++){
      char *zId = aCollide[i].azHit[j];
      if( zId==0 ) continue;
      @ %z(href("%R/ambiguous/%s",zId))%h(zId)</a>
    }
  }
  for(i=4; i<count(aCollide); i++){
    for(j=0; j<aCollide[i].cnt && j<MAX_COLLIDE; j++){
      fossil_free(aCollide[i].azHit[j]);
    }
  }
}

/*
** WEBPAGE: hash-collisions
**
** Show the number of hash collisions for hash prefixes of various lengths.
*/
void hash_collisions_webpage(void){
  login_check_credentials();
  if( !g.perm.Read ){ login_needed(g.anon.Read); return; }
  style_header("Hash Prefix Collisions");
  style_submenu_element("Activity Reports", "reports");
  style_submenu_element("Stats", "stat");
  @ <h1>Hash Prefix Collisions on Check-ins</h1>
  collision_report("SELECT (SELECT uuid FROM blob WHERE rid=objid)"
                   "  FROM event WHERE event.type='ci'"
                   " ORDER BY 1");
  @ <h1>Hash Prefix Collisions on All Artifacts</h1>
  collision_report("SELECT uuid FROM blob ORDER BY 1");
  style_footer();
}
