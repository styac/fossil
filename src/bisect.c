/*
** Copyright (c) 2010 D. Richard Hipp
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the Simplified BSD License (also
** known as the "2-Clause License" or "FreeBSD License".)

** This program is distributed in the hope that it will be useful,
** but without any warranty; without even the implied warranty of
** merchantability or fitness for a particular purpose.
**
** Author contact information:
**   drh@sqlite.org
**
*******************************************************************************
**
** This file contains code used to implement the "bisect" command.
**
** This file also contains logic used to compute the closure of filename
** changes that have occurred across multiple check-ins.
*/
#include "config.h"
#include "bisect.h"
#include <assert.h>

/*
** Local variables for this module
*/
static struct {
  int bad;                /* The bad version */
  int good;               /* The good version */
} bisect;

/*
** Find the shortest path between bad and good.
*/
void bisect_path(void){
  PathNode *p;
  bisect.bad = db_lget_int("bisect-bad", 0);
  bisect.good = db_lget_int("bisect-good", 0);
  if( bisect.good>0 && bisect.bad==0 ){
    path_shortest(bisect.good, bisect.good, 0, 0, 0);
  }else if( bisect.bad>0 && bisect.good==0 ){
    path_shortest(bisect.bad, bisect.bad, 0, 0, 0);
  }else if( bisect.bad==0 && bisect.good==0 ){
    fossil_fatal("neither \"good\" nor \"bad\" versions have been identified");
  }else{
    Bag skip;
    int bDirect = bisect_option("direct-only");
    char *zLog = db_lget("bisect-log","");
    Blob log, id;
    bag_init(&skip);
    blob_init(&log, zLog, -1);
    while( blob_token(&log, &id) ){
      if( blob_str(&id)[0]=='s' ){
        bag_insert(&skip, atoi(blob_str(&id)+1));
      }
    }
    blob_reset(&log);
    p = path_shortest(bisect.good, bisect.bad, bDirect, 0, &skip);
    bag_clear(&skip);
    if( p==0 ){
      char *zBad = db_text(0,"SELECT uuid FROM blob WHERE rid=%d",bisect.bad);
      char *zGood = db_text(0,"SELECT uuid FROM blob WHERE rid=%d",bisect.good);
      fossil_fatal("no path from good ([%S]) to bad ([%S]) or back",
                   zGood, zBad);
    }
  }
}

/*
** The set of all bisect options.
*/
static const struct {
  const char *zName;
  const char *zDefault;
  const char *zDesc;
} aBisectOption[] = {
  { "auto-next",    "on",    "Automatically run \"bisect next\" after each "
                             "\"bisect good\", \"bisect bad\", or \"bisect "
                             "skip\"" },
  { "direct-only",  "on",    "Follow only primary parent-child links, not "
                             "merges\n" },
  { "display",    "chart",   "Command to run after \"next\".  \"chart\", "
                             "\"log\", \"status\", or \"none\"" },
};

/*
** Return the value of a boolean bisect option.
*/
int bisect_option(const char *zName){
  unsigned int i;
  int r = -1;
  for(i=0; i<count(aBisectOption); i++){
    if( fossil_strcmp(zName, aBisectOption[i].zName)==0 ){
      char *zLabel = mprintf("bisect-%s", zName);
      char *z;
      if( g.localOpen ){
        z = db_lget(zLabel, (char*)aBisectOption[i].zDefault);
      }else{
        z = (char*)aBisectOption[i].zDefault;
      }
      if( is_truth(z) ) r = 1;
      if( is_false(z) ) r = 0;
      if( r<0 ) r = is_truth(aBisectOption[i].zDefault);
      free(zLabel);
      break;
    }
  }
  assert( r>=0 );
  return r;
}

/*
** List a bisect path.
*/
static void bisect_list(int abbreviated){
  PathNode *p;
  int vid = db_lget_int("checkout", 0);
  int n;
  Stmt s;
  int nStep;
  int nHidden = 0;
  bisect_path();
  db_prepare(&s, "SELECT blob.uuid, datetime(event.mtime) "
                 "  FROM blob, event"
                 " WHERE blob.rid=:rid AND event.objid=:rid"
                 "   AND event.type='ci'");
  nStep = path_length();
  if( abbreviated ){
    for(p=path_last(); p; p=p->pFrom) p->isHidden = 1;
    for(p=path_last(), n=0; p; p=p->pFrom, n++){
      if( p->rid==bisect.good
       || p->rid==bisect.bad
       || p->rid==vid
       || (nStep>1 && n==nStep/2)
      ){
        p->isHidden = 0;
        if( p->pFrom ) p->pFrom->isHidden = 0;
      }
    }
    for(p=path_last(); p; p=p->pFrom){
      if( p->pFrom && p->pFrom->isHidden==0 ) p->isHidden = 0;
    }
  }
  for(p=path_last(), n=0; p; p=p->pFrom, n++){
    if( p->isHidden && (nHidden || (p->pFrom && p->pFrom->isHidden)) ){
      nHidden++;
      continue;
    }else if( nHidden ){
      fossil_print("  ... %d other check-ins omitted\n", nHidden);
      nHidden = 0;
    }
    db_bind_int(&s, ":rid", p->rid);
    if( db_step(&s)==SQLITE_ROW ){
      const char *zUuid = db_column_text(&s, 0);
      const char *zDate = db_column_text(&s, 1);
      fossil_print("%s %S", zDate, zUuid);
      if( p->rid==bisect.good ) fossil_print(" GOOD");
      if( p->rid==bisect.bad ) fossil_print(" BAD");
      if( p->rid==vid ) fossil_print(" CURRENT");
      if( nStep>1 && n==nStep/2 ) fossil_print(" NEXT");
      fossil_print("\n");
    }
    db_reset(&s);
  }
  db_finalize(&s);
}

/*
** Append a new entry to the bisect log.  Update the bisect-good or
** bisect-bad values as appropriate.
**
** The bisect-log consists of a list of token.  Each token is an
** integer RID of a check-in.  The RID is negative for "bad" check-ins
** and positive for "good" check-ins.
*/
static void bisect_append_log(int rid){
  if( rid<0 ){
    if( db_lget_int("bisect-bad",0)==(-rid) ) return;
    db_lset_int("bisect-bad", -rid);
  }else{
    if( db_lget_int("bisect-good",0)==rid ) return;
    db_lset_int("bisect-good", rid);
  }
  db_multi_exec(
     "REPLACE INTO vvar(name,value) VALUES('bisect-log',"
       "COALESCE((SELECT value||' ' FROM vvar WHERE name='bisect-log'),'')"
       " || '%d')", rid);
}

/*
** Append a new skip entry to the bisect log.
*/
static void bisect_append_skip(int rid){
  db_multi_exec(
     "UPDATE vvar SET value=value||' s%d' WHERE name='bisect-log'", rid
  );
}

/*
** Create a TEMP table named "bilog" that contains the complete history
** of the current bisect.
**
** If iCurrent>0 then it is the RID of the current checkout and is included
** in the history table.
**
** If zDesc is not NULL, then it is the bid= query parameter to /timeline
** that describes a bisect.  Use the information in zDesc rather than in
** the bisect-log variable.
**
** If bDetail is true, then also include information about every node
** in between the inner-most GOOD and BAD nodes.
*/
int bisect_create_bilog_table(int iCurrent, const char *zDesc, int bDetail){
  char *zLog;
  Blob log, id;
  Stmt q;
  int cnt = 0;
  int lastGood = -1;
  int lastBad = -1;

  if( zDesc!=0 ){
    blob_init(&log, 0, 0);
    while( zDesc[0]=='y' || zDesc[0]=='n' || zDesc[0]=='s' ){
      int i;
      char c;
      int rid;
      if( blob_size(&log) ) blob_append(&log, " ", 1);
      if( zDesc[0]=='n' ) blob_append(&log, "-", 1);
      if( zDesc[0]=='s' ) blob_append(&log, "s", 1);
      for(i=1; ((c = zDesc[i])>='0' && c<='9') || (c>='a' && c<='f'); i++){}
      if( i==1 ) break;
      rid = db_int(0,
        "SELECT rid FROM blob"
        " WHERE uuid LIKE '%.*q%%'"
        "   AND EXISTS(SELECT 1 FROM plink WHERE cid=rid)",
        i-1, zDesc+1
      );
      if( rid==0 ) break;
      blob_appendf(&log, "%d", rid);
      zDesc += i;
    }
  }else{
    zLog = db_lget("bisect-log","");
    blob_init(&log, zLog, -1);
  }
  db_multi_exec(
     "CREATE TEMP TABLE bilog("
     "  rid INTEGER PRIMARY KEY,"  /* Sequence of events */
     "  stat TEXT,"                /* Type of occurrence */
     "  seq INTEGER UNIQUE"        /* Check-in number */
     ");"
  );
  db_prepare(&q, "INSERT OR IGNORE INTO bilog(seq,stat,rid)"
                 " VALUES(:seq,:stat,:rid)");
  while( blob_token(&log, &id) ){
    int rid;
    db_bind_int(&q, ":seq", ++cnt);
    if( blob_str(&id)[0]=='s' ){
      rid = atoi(blob_str(&id)+1);
      db_bind_text(&q, ":stat", "SKIP");
      db_bind_int(&q, ":rid", rid);
    }else{
      rid = atoi(blob_str(&id));
      if( rid>0 ){
        db_bind_text(&q, ":stat","GOOD");
        db_bind_int(&q, ":rid", rid);
        lastGood = rid;
      }else{
        db_bind_text(&q, ":stat", "BAD");
        db_bind_int(&q, ":rid", -rid);
        lastBad = -rid;
      }
    }
    db_step(&q);
    db_reset(&q);
  }
  if( iCurrent>0 ){
    db_bind_int(&q, ":seq", ++cnt);
    db_bind_text(&q, ":stat", "CURRENT");
    db_bind_int(&q, ":rid", iCurrent);
    db_step(&q);
    db_reset(&q);
  }
  if( bDetail && lastGood>0 && lastBad>0 ){
    PathNode *p;
    p = path_shortest(lastGood, lastBad, bisect_option("direct-only"),0, 0);
    while( p ){
      db_bind_null(&q, ":seq");
      db_bind_null(&q, ":stat");
      db_bind_int(&q, ":rid", p->rid);
      db_step(&q);
      db_reset(&q);
      p = p->u.pTo;
    }
    path_reset();
  }
  db_finalize(&q);
  return 1;
}

/* Return a permalink description of a bisect.  Space is obtained from
** fossil_malloc() and should be freed by the caller.
**
** A bisect description consists of characters 'y' and 'n' and lowercase
** hex digits.  Each term begins with 'y' or 'n' (success or failure) and
** is followed by a hash prefix in lowercase hex.
*/
char *bisect_permalink(void){
  char *zLog = db_lget("bisect-log","");
  char *zResult;
  Blob log;
  Blob link = BLOB_INITIALIZER;
  Blob id;
  blob_init(&log, zLog, -1);
  while( blob_token(&log, &id) ){
    const char *zUuid;
    int rid;
    char cPrefix = 'y';
    if( blob_str(&id)[0]=='s' ){
      rid = atoi(blob_str(&id)+1);
      cPrefix = 's';
    }else{
      rid = atoi(blob_str(&id));
      if( rid<0 ){
        cPrefix = 'n';
        rid = -rid;
      }
    }
    zUuid = db_text(0,"SELECT lower(uuid) FROM blob WHERE rid=%d", rid);
    blob_appendf(&link, "%c%.10s", cPrefix, zUuid);
  }
  zResult = mprintf("%s", blob_str(&link));
  blob_reset(&link);
  blob_reset(&log);
  blob_reset(&id);
  return zResult;
}

/*
** Show a chart of bisect "good" and "bad" versions.  The chart can be
** sorted either chronologically by bisect time, or by check-in time.
*/
static void bisect_chart(int sortByCkinTime){
  Stmt q;
  int iCurrent = db_lget_int("checkout",0);
  bisect_create_bilog_table(iCurrent, 0, 0);
  db_prepare(&q,
    "SELECT bilog.seq, bilog.stat,"
    "       substr(blob.uuid,1,16), datetime(event.mtime),"
    "       blob.rid==%d"
    "  FROM bilog, blob, event"
    " WHERE blob.rid=bilog.rid AND event.objid=bilog.rid"
    "   AND event.type='ci'"
    " ORDER BY %s bilog.rowid ASC",
    iCurrent, (sortByCkinTime ? "event.mtime DESC, " : "")
  );
  while( db_step(&q)==SQLITE_ROW ){
    const char *zGoodBad = db_column_text(&q, 1);
    fossil_print("%3d %-7s %s %s%s\n",
        db_column_int(&q, 0),
        zGoodBad,
        db_column_text(&q, 3),
        db_column_text(&q, 2),
        (db_column_int(&q, 4) && zGoodBad[0]!='C') ? " CURRENT" : "");
  }
  db_finalize(&q);
}


/*
** Reset the bisect subsystem.
*/
void bisect_reset(void){
  db_multi_exec(
    "DELETE FROM vvar WHERE name IN "
    " ('bisect-good', 'bisect-bad', 'bisect-log')"
  );
}

/*
** COMMAND: bisect
**
** Usage: %fossil bisect SUBCOMMAND ...
**
** Run various subcommands useful for searching back through the change
** history for a particular checkin that causes or fixes a problem.
**
** > fossil bisect bad ?VERSION?
**
**       Identify version VERSION as non-working.  If VERSION is omitted,
**       the current checkout is marked as non-working.
**
** > fossil bisect good ?VERSION?
**
**       Identify version VERSION as working.  If VERSION is omitted,
**       the current checkout is marked as working.
**
** > fossil bisect log
** > fossil bisect chart
**
**       Show a log of "good", "bad", and "skip" versions.  "bisect log"
**       shows the  events in the order that they were tested.
**       "bisect chart" shows them in order of check-in.
**
** > fossil bisect next
**
**       Update to the next version that is halfway between the working and
**       non-working versions.
**
** > fossil bisect options ?NAME? ?VALUE?
**
**       List all bisect options, or the value of a single option, or set the
**       value of a bisect option.
**
** > fossil bisect reset
**
**       Reinitialize a bisect session.  This cancels prior bisect history
**       and allows a bisect session to start over from the beginning.
**
** > fossil bisect skip ?VERSION?
**
**       Cause VERSION (or the current checkout if VERSION is omitted) to
**       be ignored for the purpose of the current bisect.  This might
**       be done, for example, because VERSION does not compile correctly
**       or is otherwise unsuitable to participate in this bisect.
**
** > fossil bisect vlist|ls|status ?-a|--all?
**
**       List the versions in between the inner-most "bad" and "good".
**
** > fossil bisect ui
**
**       Like "fossil ui" except start on a timeline that shows only the
**       check-ins that are part of the current bisect.
**
** > fossil bisect undo
**
**       Undo the most recent "good", "bad", or "skip" command.
*/
void bisect_cmd(void){
  int n;
  const char *zCmd;
  int foundCmd = 0;
  db_must_be_within_tree();
  if( g.argc<3 ){
    goto usage;
  }
  zCmd = g.argv[2];
  n = strlen(zCmd);
  if( n==0 ) zCmd = "-";
  if( strncmp(zCmd, "bad", n)==0 ){
    int ridBad;
    foundCmd = 1;
    if( g.argc==3 ){
      ridBad = db_lget_int("checkout",0);
    }else{
      ridBad = name_to_typed_rid(g.argv[3], "ci");
    }
    if( ridBad>0 ){
      bisect_append_log(-ridBad);
      if( bisect_option("auto-next") && db_lget_int("bisect-good",0)>0 ){
        zCmd = "next";
        n = 4;
      }
    }
  }else if( strncmp(zCmd, "good", n)==0 ){
    int ridGood;
    foundCmd = 1;
    if( g.argc==3 ){
      ridGood = db_lget_int("checkout",0);
    }else{
      ridGood = name_to_typed_rid(g.argv[3], "ci");
    }
    if( ridGood>0 ){
      bisect_append_log(ridGood);
      if( bisect_option("auto-next") && db_lget_int("bisect-bad",0)>0 ){
        zCmd = "next";
        n = 4;
      }
    }
  }else if( strncmp(zCmd, "skip", n)==0 ){
    int ridSkip;
    foundCmd = 1;
    if( g.argc==3 ){
      ridSkip = db_lget_int("checkout",0);
    }else{
      ridSkip = name_to_typed_rid(g.argv[3], "ci");
    }
    if( ridSkip>0 ){
      bisect_append_skip(ridSkip);
      if( bisect_option("auto-next")
       && db_lget_int("bisect-bad",0)>0
       && db_lget_int("bisect-good",0)>0
      ){
        zCmd = "next";
        n = 4;
      }
    }
  }else if( strncmp(zCmd, "undo", n)==0 ){
    char *zLog;
    Blob log, id;
    int ridBad = 0;
    int ridGood = 0;
    int cnt = 0, i;
    foundCmd = 1;
    db_begin_transaction();
    zLog = db_lget("bisect-log","");
    blob_init(&log, zLog, -1);
    while( blob_token(&log, &id) ){ cnt++; }
    if( cnt==0 ){
      fossil_fatal("no previous bisect steps to undo");
    }
    blob_rewind(&log);
    for(i=0; i<cnt-1; i++){
      int rid;
      blob_token(&log, &id);
      rid = atoi(blob_str(&id));
      if( rid<0 ) ridBad = -rid;
      else        ridGood = rid;
    }
    db_multi_exec(
      "UPDATE vvar SET value=substr(value,1,%d) WHERE name='bisect-log'",
      log.iCursor-1
    );
    db_lset_int("bisect-bad", ridBad);
    db_lset_int("bisect-good", ridGood);
    db_end_transaction(0);
    if( ridBad && ridGood ){
      zCmd = "next";
      n = 4;
    }
  }
  /* No else here so that the above commands can morph themselves into
  ** a "next" command */
  if( strncmp(zCmd, "next", n)==0 ){
    PathNode *pMid;
    char *zDisplay = db_lget("bisect-display","chart");
    int m = (int)strlen(zDisplay);
    bisect_path();
    pMid = path_midpoint();
    if( pMid==0 ){
      fossil_print("bisect complete\n");
    }else{
      int nSpan = path_length_not_hidden();
      int nStep = path_search_depth();
      g.argv[1] = "update";
      g.argv[2] = db_text(0, "SELECT uuid FROM blob WHERE rid=%d", pMid->rid);
      g.argc = 3;
      g.fNoSync = 1;
      update_cmd();
      fossil_print("span: %d  steps-remaining: %d\n", nSpan, nStep);
    }

    if( strncmp(zDisplay,"chart",m)==0 ){
      bisect_chart(1);
    }else if( strncmp(zDisplay, "log", m)==0 ){
      bisect_chart(0);
    }else if( strncmp(zDisplay, "status", m)==0 ){
      bisect_list(1);
    }
  }else if( strncmp(zCmd, "log", n)==0 ){
    bisect_chart(0);
  }else if( strncmp(zCmd, "chart", n)==0 ){
    bisect_chart(1);
  }else if( strncmp(zCmd, "options", n)==0 ){
    if( g.argc==3 ){
      unsigned int i;
      for(i=0; i<count(aBisectOption); i++){
        char *z = mprintf("bisect-%s", aBisectOption[i].zName);
        fossil_print("  %-15s  %-6s  ", aBisectOption[i].zName,
               db_lget(z, (char*)aBisectOption[i].zDefault));
        fossil_free(z);
        comment_print(aBisectOption[i].zDesc, 0, 27, -1, get_comment_format());
      }
    }else if( g.argc==4 || g.argc==5 ){
      unsigned int i;
      n = strlen(g.argv[3]);
      for(i=0; i<count(aBisectOption); i++){
        if( strncmp(g.argv[3], aBisectOption[i].zName, n)==0 ){
          char *z = mprintf("bisect-%s", aBisectOption[i].zName);
          if( g.argc==5 ){
            db_lset(z, g.argv[4]);
          }
          fossil_print("%s\n", db_lget(z, (char*)aBisectOption[i].zDefault));
          fossil_free(z);
          break;
        }
      }
      if( i>=count(aBisectOption) ){
        fossil_fatal("no such bisect option: %s", g.argv[3]);
      }
    }else{
      usage("options ?NAME? ?VALUE?");
    }
  }else if( strncmp(zCmd, "reset", n)==0 ){
    bisect_reset();
  }else if( strcmp(zCmd, "ui")==0 ){
    char *newArgv[8];
    newArgv[0] = g.argv[0];
    newArgv[1] = "ui";
    newArgv[2] = "--page";
    newArgv[3] = "timeline?bisect";
    newArgv[4] = 0;
    g.argv = newArgv;
    g.argc = 4;
    cmd_webserver();
  }else if( strncmp(zCmd, "vlist", n)==0
         || strncmp(zCmd, "ls", n)==0
         || strncmp(zCmd, "status", n)==0
  ){
    int fAll = find_option("all", "a", 0)!=0;
    bisect_list(!fAll);
  }else if( !foundCmd ){
usage:
    usage("bad|good|log|chart|next|options|reset|skip|status|ui|undo");
  }
}
