/*
** Copyright (c) 2006 D. Richard Hipp
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
** This file contains code for miscellaneous utility routines.
*/
#include "config.h"
#include "util.h"
#if defined(USE_MMAN_H)
# include <sys/mman.h>
# include <unistd.h>
#endif

/*
** For the fossil_timer_xxx() family of functions...
*/
#ifdef _WIN32
# include <windows.h>
#else
# include <sys/time.h>
# include <sys/resource.h>
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
#endif


/*
** Exit.  Take care to close the database first.
*/
NORETURN void fossil_exit(int rc){
  db_close(1);
#ifndef _WIN32
  if( g.fAnyTrace ){
    fprintf(stderr, "/***** Subprocess %d exit(%d) *****/\n", getpid(), rc);
    fflush(stderr);
  }
#endif
  exit(rc);
}

/*
** Malloc and free routines that cannot fail
*/
void *fossil_malloc(size_t n){
  void *p = malloc(n==0 ? 1 : n);
  if( p==0 ) fossil_panic("out of memory");
  return p;
}
void fossil_free(void *p){
  free(p);
}
void *fossil_realloc(void *p, size_t n){
  p = realloc(p, n);
  if( p==0 ) fossil_panic("out of memory");
  return p;
}
void fossil_secure_zero(void *p, size_t n){
  volatile unsigned char *vp = (volatile unsigned char *)p;
  size_t i;

  if( p==0 ) return;
  assert( n>0 );
  if( n==0 ) return;
  for(i=0; i<n; i++){ vp[i] ^= 0xFF; }
  for(i=0; i<n; i++){ vp[i] ^= vp[i]; }
}
void fossil_get_page_size(size_t *piPageSize){
#if defined(_WIN32)
  SYSTEM_INFO sysInfo;
  memset(&sysInfo, 0, sizeof(SYSTEM_INFO));
  GetSystemInfo(&sysInfo);
  *piPageSize = (size_t)sysInfo.dwPageSize;
#elif defined(USE_MMAN_H)
  *piPageSize = (size_t)sysconf(_SC_PAGE_SIZE);
#else
  *piPageSize = 4096; /* FIXME: What for POSIX? */
#endif
}
void *fossil_secure_alloc_page(size_t *pN){
  void *p;
  size_t pageSize = 0;

  fossil_get_page_size(&pageSize);
  assert( pageSize>0 );
  assert( pageSize%2==0 );
#if defined(_WIN32)
  p = VirtualAlloc(NULL, pageSize, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
  if( p==NULL ){
    fossil_panic("VirtualAlloc failed: %lu\n", GetLastError());
  }
  if( !VirtualLock(p, pageSize) ){
    fossil_panic("VirtualLock failed: %lu\n", GetLastError());
  }
#elif defined(USE_MMAN_H)
  p = mmap(0, pageSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if( p==MAP_FAILED ){
    fossil_panic("mmap failed: %d\n", errno);
  }
  if( mlock(p, pageSize) ){
    fossil_panic("mlock failed: %d\n", errno);
  }
#else
  p = fossil_malloc(pageSize);
#endif
  fossil_secure_zero(p, pageSize);
  if( pN ) *pN = pageSize;
  return p;
}
void fossil_secure_free_page(void *p, size_t n){
  if( !p ) return;
  assert( n>0 );
  fossil_secure_zero(p, n);
#if defined(_WIN32)
  if( !VirtualUnlock(p, n) ){
    fossil_panic("VirtualUnlock failed: %lu\n", GetLastError());
  }
  if( !VirtualFree(p, 0, MEM_RELEASE) ){
    fossil_panic("VirtualFree failed: %lu\n", GetLastError());
  }
#elif defined(USE_MMAN_H)
  if( munlock(p, n) ){
    fossil_panic("munlock failed: %d\n", errno);
  }
  if( munmap(p, n) ){
    fossil_panic("munmap failed: %d\n", errno);
  }
#else
  fossil_free(p);
#endif
}

/*
** Translate every upper-case character in the input string into
** its equivalent lower-case.
*/
char *fossil_strtolwr(char *zIn){
  char *zStart = zIn;
  if( zIn ){
    while( *zIn ){
      *zIn = fossil_tolower(*zIn);
      zIn++;
    }
  }
  return zStart;
}

/*
** If this local variable is set, fossil_assert_safe_command_string()
** returns false on an unsafe command-string rather than abort.  Set
** this variable for testing.
*/
static int safeCmdStrTest = 0;

/*
** Check the input string to ensure that it is safe to pass into system().
** A string is unsafe for system() on unix if it contains any of the following:
**
**   *  Any occurrance of '$' or '`' except after \
**   *  Any of the following characters, unquoted:  ;|& or \n except
**      these characters are allowed as the very last character in the
**      string.
**   *  Unbalanced single or double quotes
**
** This routine is intended as a second line of defense against attack.
** It should never fail.  Dangerous shell strings should be detected and
** fixed before calling fossil_system().  This routine serves only as a
** safety net in case of bugs elsewhere in the system.
**
** If an unsafe string is seen, either abort (default) or print
** a warning message (if safeCmdStrTest is true).
*/
static void fossil_assert_safe_command_string(const char *z){
  int unsafe = 0;
#ifndef _WIN32
  /* Unix */
  int inQuote = 0;
  int i, c;
  for(i=0; !unsafe && (c = z[i])!=0; i++){
    switch( c ){
      case '$':
      case '`': {
        if( inQuote!='\'' ) unsafe = i+1;
        break;
      }
      case ';':
      case '|':
      case '&':
      case '\n': {
        if( inQuote!='\'' && z[i+1]!=0 ) unsafe = i+1;
        break;
      }
      case '"':
      case '\'': {
        if( inQuote==0 ){
          inQuote = c;
        }else if( inQuote==c ){
          inQuote = 0;
        }
        break;
      }
      case '\\': {
        if( z[i+1]==0 ){
          unsafe = i+1;
        }else if( inQuote!='\'' ){
          i++;
        }
        break;
      }
    }
  }
  if( inQuote ) unsafe = i;
#else
  /* Windows */
  int i, c;
  int inQuote = 0;
  for(i=0; !unsafe && (c = z[i])!=0; i++){
    switch( c ){
      case '>':
      case '<':
      case '|':
      case '&':
      case '\n': {
        if( inQuote==0 && z[i+1]!=0 ) unsafe = i+1;
        break;
      }
      case '\\': {
        if( z[i+1]=='"' ){ i++; }
        break;
      }
      case '"': {
        if( inQuote==c ){
          inQuote = 0;
        }else{
          inQuote = c;
        }
        break;
      }
      case '^': {
        if( z[i+1]=='"' ){
          unsafe = i+2;
        }else if( z[i+1]!=0 ){
          i++;
        }
        break;
      }
    }
  }
  if( inQuote ) unsafe = i;
#endif
  if( unsafe ){
    char *zMsg = mprintf("Unsafe command string: %s\n%*shere ----^",
                   z, unsafe+13, "");
    if( safeCmdStrTest ){
      fossil_print("%z\n", zMsg);
    }else{
      fossil_panic("%s", zMsg);
    }
  }
}

/*
** This function implements a cross-platform "system()" interface.
*/
int fossil_system(const char *zOrigCmd){
  int rc;
#if defined(_WIN32)
  /* On windows, we have to put double-quotes around the entire command.
  ** Who knows why - this is just the way windows works.
  */
  char *zNewCmd = mprintf("\"%s\"", zOrigCmd);
  wchar_t *zUnicode = fossil_utf8_to_unicode(zNewCmd);
  if( g.fSystemTrace ) {
    fossil_trace("SYSTEM: %s\n", zNewCmd);
  }
  fossil_assert_safe_command_string(zOrigCmd);
  rc = _wsystem(zUnicode);
  fossil_unicode_free(zUnicode);
  free(zNewCmd);
#else
  /* On unix, evaluate the command directly.
  */
  if( g.fSystemTrace ) fprintf(stderr, "SYSTEM: %s\n", zOrigCmd);
  fossil_assert_safe_command_string(zOrigCmd);

  /* Unix systems should never shell-out while processing an HTTP request,
  ** either via CGI, SCGI, or direct HTTP.  The following assert verifies
  ** this.  And the following assert proves that Fossil is not vulnerable
  ** to the ShellShock or BashDoor bug.
  */
  assert( g.cgiOutput==0 );

  /* The regular system() call works to get a shell on unix */
  fossil_limit_memory(0);
  rc = system(zOrigCmd);
  fossil_limit_memory(1);
#endif
  return rc;
}

/*
** COMMAND: test-fossil-system
**
** Read lines of input and send them to fossil_system() for evaluation.
** Use this command to verify that fossil_system() will not run "unsafe"
** commands.
*/
void test_fossil_system_cmd(void){
  char zLine[10000];
  safeCmdStrTest = 1;
  while(1){
    size_t n;
    printf("system-test> ");
    fflush(stdout);
    if( !fgets(zLine, sizeof(zLine), stdin) ) break;
    n = strlen(zLine);
    while( n>0 && fossil_isspace(zLine[n-1]) ) n--;
    zLine[n] = 0;
    printf("cmd: [%s]\n", zLine);
    fflush(stdout);
    fossil_system(zLine);
  }
}

/*
** Like strcmp() except that it accepts NULL pointers.  NULL sorts before
** all non-NULL string pointers.  Also, this strcmp() is a binary comparison
** that does not consider locale.
*/
int fossil_strcmp(const char *zA, const char *zB){
  if( zA==0 ){
    if( zB==0 ) return 0;
    return -1;
  }else if( zB==0 ){
    return +1;
  }else{
    return strcmp(zA,zB);
  }
}
int fossil_strncmp(const char *zA, const char *zB, int nByte){
  if( zA==0 ){
    if( zB==0 ) return 0;
    return -1;
  }else if( zB==0 ){
    return +1;
  }else if( nByte>0 ){
    int a, b;
    do{
      a = *zA++;
      b = *zB++;
    }while( a==b && a!=0 && (--nByte)>0 );
    return ((unsigned char)a) - (unsigned char)b;
  }else{
    return 0;
  }
}

/*
** Case insensitive string comparison.
*/
int fossil_strnicmp(const char *zA, const char *zB, int nByte){
  if( zA==0 ){
    if( zB==0 ) return 0;
    return -1;
  }else if( zB==0 ){
    return +1;
  }
  if( nByte<0 ) nByte = strlen(zB);
  return sqlite3_strnicmp(zA, zB, nByte);
}
int fossil_stricmp(const char *zA, const char *zB){
  int nByte;
  int rc;
  if( zA==0 ){
    if( zB==0 ) return 0;
    return -1;
  }else if( zB==0 ){
    return +1;
  }
  nByte = strlen(zB);
  rc = sqlite3_strnicmp(zA, zB, nByte);
  if( rc==0 && zA[nByte] ) rc = 1;
  return rc;
}

/*
** Get user and kernel times in microseconds.
*/
void fossil_cpu_times(sqlite3_uint64 *piUser, sqlite3_uint64 *piKernel){
#ifdef _WIN32
  FILETIME not_used;
  FILETIME kernel_time;
  FILETIME user_time;
  GetProcessTimes(GetCurrentProcess(), &not_used, &not_used,
                  &kernel_time, &user_time);
  if( piUser ){
     *piUser = ((((sqlite3_uint64)user_time.dwHighDateTime)<<32) +
                         (sqlite3_uint64)user_time.dwLowDateTime + 5)/10;
  }
  if( piKernel ){
     *piKernel = ((((sqlite3_uint64)kernel_time.dwHighDateTime)<<32) +
                         (sqlite3_uint64)kernel_time.dwLowDateTime + 5)/10;
  }
#else
  struct rusage s;
  getrusage(RUSAGE_SELF, &s);
  if( piUser ){
    *piUser = ((sqlite3_uint64)s.ru_utime.tv_sec)*1000000 + s.ru_utime.tv_usec;
  }
  if( piKernel ){
    *piKernel =
              ((sqlite3_uint64)s.ru_stime.tv_sec)*1000000 + s.ru_stime.tv_usec;
  }
#endif
}

/*
** Internal helper type for fossil_timer_xxx().
 */
enum FossilTimerEnum {
  FOSSIL_TIMER_COUNT = 10 /* Number of timers we can track. */
};
static struct FossilTimer {
  sqlite3_uint64 u; /* "User" CPU times */
  sqlite3_uint64 s; /* "System" CPU times */
  int id; /* positive if allocated, else 0. */
} fossilTimerList[FOSSIL_TIMER_COUNT] = {{0,0,0}};

/*
** Stores the current CPU times into the shared timer list
** and returns that timer's internal ID. Pass that ID to
** fossil_timer_fetch() to get the elapsed time for that
** timer.
**
** The system has a fixed number of timers, and they can be
** "deallocated" by passing this function's return value to
** fossil_timer_stop() Adjust FOSSIL_TIMER_COUNT to set the number of
** available timers.
**
** Returns 0 on error (no more timers available), with 1+ being valid
** timer IDs.
*/
int fossil_timer_start(){
  int i;
  for( i = 0; i < FOSSIL_TIMER_COUNT; ++i ){
    struct FossilTimer * ft = &fossilTimerList[i];
    if(ft->id) continue;
    ft->id = i+1;
    fossil_cpu_times( &ft->u, &ft->s );
    break;
  }
  return (i<FOSSIL_TIMER_COUNT) ? i+1 : 0;
}

/*
** Returns the difference in CPU times in microseconds since
** fossil_timer_start() was called and returned the given timer ID (or
** since it was last reset). Returns 0 if timerId is out of range.
*/
sqlite3_uint64 fossil_timer_fetch(int timerId){
  if( timerId>0 && timerId<=FOSSIL_TIMER_COUNT ){
    struct FossilTimer * start = &fossilTimerList[timerId-1];
    if( !start->id ){
      fossil_panic("Invalid call to fetch a non-allocated "
                   "timer (#%d)", timerId);
      /*NOTREACHED*/
    }else{
      sqlite3_uint64 eu = 0, es = 0;
      fossil_cpu_times( &eu, &es );
      return (eu - start->u) + (es - start->s);
    }
  }
  return 0;
}

/*
** Resets the timer associated with the given ID, as obtained via
** fossil_timer_start(), to the current CPU time values.
*/
sqlite3_uint64 fossil_timer_reset(int timerId){
  if( timerId>0 && timerId<=FOSSIL_TIMER_COUNT ){
    struct FossilTimer * start = &fossilTimerList[timerId-1];
    if( !start->id ){
      fossil_panic("Invalid call to reset a non-allocated "
                   "timer (#%d)", timerId);
      /*NOTREACHED*/
    }else{
      sqlite3_uint64 const rc = fossil_timer_fetch(timerId);
      fossil_cpu_times( &start->u, &start->s );
      return rc;
    }
  }
  return 0;
}

/**
   "Deallocates" the fossil timer identified by the given timer ID.
   returns the difference (in uSec) between the last time that timer
   was started or reset. Returns 0 if timerId is out of range (but
   note that, due to system-level precision restrictions, this
   function might return 0 on success, too!). It is not legal to
   re-use the passed-in timerId after calling this until/unless it is
   re-initialized using fossil_timer_start() (NOT
   fossil_timer_reset()).
*/
sqlite3_uint64 fossil_timer_stop(int timerId){
  if(timerId<1 || timerId>FOSSIL_TIMER_COUNT){
    return 0;
  }else{
    sqlite3_uint64 const rc = fossil_timer_fetch(timerId);
    struct FossilTimer * t = &fossilTimerList[timerId-1];
    t->id = 0;
    t->u = t->s = 0U;
    return rc;
  }
}

/*
** Returns true (non-0) if the given timer ID (as returned from
** fossil_timer_start() is currently active.
*/
int fossil_timer_is_active( int timerId ){
  if(timerId<1 || timerId>FOSSIL_TIMER_COUNT){
    return 0;
  }else{
    const int rc = fossilTimerList[timerId-1].id;
    assert(!rc || (rc == timerId));
    return fossilTimerList[timerId-1].id;
  }
}

/*
** Return TRUE if fd is a valid open file descriptor.  This only
** works on unix.  The function always returns true on Windows.
*/
int is_valid_fd(int fd){
#ifdef _WIN32
  return 1;
#else
  return fcntl(fd, F_GETFL)!=(-1) || errno!=EBADF;
#endif
}

/*
** Returns TRUE if zSym is exactly HNAME_LEN_SHA1 or HNAME_LEN_K256
** bytes long and contains only lower-case ASCII hexadecimal values.
*/
int fossil_is_artifact_hash(const char *zSym){
  int sz = zSym ? (int)strlen(zSym) : 0;
  return (HNAME_LEN_SHA1==sz || HNAME_LEN_K256==sz) && validate16(zSym, sz);
}

/*
** Return true if the input string is NULL or all whitespace.
** Return false if the input string contains text.
*/
int fossil_all_whitespace(const char *z){
  if( z==0 ) return 1;
  while( fossil_isspace(z[0]) ){ z++; }
  return z[0]==0;
}

/*
** Return the name of the users preferred text editor.  Return NULL if
** not found.
**
** Search algorithm:
** (1) The local "editor" setting
** (2) The global "editor" setting
** (3) The VISUAL environment variable
** (4) The EDITOR environment variable
** (5) (Windows only:) "notepad.exe"
*/
const char *fossil_text_editor(void){
  const char *zEditor = db_get("editor", 0);
  if( zEditor==0 ){
    zEditor = fossil_getenv("VISUAL");
  }
  if( zEditor==0 ){
    zEditor = fossil_getenv("EDITOR");
  }
#if defined(_WIN32) || defined(__CYGWIN__)
  if( zEditor==0 ){
    zEditor = mprintf("%s\\notepad.exe", fossil_getenv("SYSTEMROOT"));
#if defined(__CYGWIN__)
    zEditor = fossil_utf8_to_path(zEditor, 0);
#endif
  }
#endif
  return zEditor;
}

/*
** Construct a temporary filename.
**
** The returned string is obtained from sqlite3_malloc() and must be
** freed by the caller.
*/
char *fossil_temp_filename(void){
  char *zTFile = 0;
  sqlite3 *db;
  if( g.db ){
    db = g.db;
  }else{
    sqlite3_open("",&db);
  }
  sqlite3_file_control(db, 0, SQLITE_FCNTL_TEMPFILENAME, (void*)&zTFile);
  if( g.db==0 ) sqlite3_close(db);
  return zTFile;
}

/*
** Turn memory limits for stack and heap on and off.  The argument
** is true to turn memory limits on and false to turn them off.
**
** Memory limits should be enabled at startup, but then turned off
** before starting subprocesses.
*/
void fossil_limit_memory(int onOff){
#if defined(__unix__)
  static sqlite3_int64 origHeap = 10000000000LL;  /* 10GB */
  static sqlite3_int64 origStack =    8000000  ;  /*  8MB */
  struct rlimit x;

#if defined(RLIMIT_DATA)
  getrlimit(RLIMIT_DATA, &x);
  if( onOff ){
    origHeap = x.rlim_cur;
    if( sizeof(void*)<8 || sizeof(x.rlim_cur)<8 ){
      x.rlim_cur =  1000000000  ;  /* 1GB on 32-bit systems */
    }else{
      x.rlim_cur = 10000000000LL;  /* 10GB on 64-bit systems */
    }
  }else{
    x.rlim_cur = origHeap;
  }
  setrlimit(RLIMIT_DATA, &x);
#endif /* defined(RLIMIT_DATA) */
#if defined(RLIMIT_STACK)
  getrlimit(RLIMIT_STACK, &x);
  if( onOff ){
    origStack = x.rlim_cur;
    x.rlim_cur =  8000000;  /* 8MB */
  }else{
    x.rlim_cur = origStack;
  }
  setrlimit(RLIMIT_STACK, &x);
#endif /* defined(RLIMIT_STACK) */
#endif /* defined(__unix__) */
}

#if defined(HAVE_PLEDGE)
/*
** Interface to pledge() on OpenBSD 5.9 and later.
**
** On platforms that have pledge(), use this routine.
** On all other platforms, this routine does not exist, but instead
** a macro defined in config.h is used to provide a no-op.
*/
void fossil_pledge(const char *promises){
  if( pledge(promises, 0) ){
    fossil_panic("pledge(\"%s\",NULL) fails with errno=%d",
      promises, (int)errno);
  }
}
#endif /* defined(HAVE_PLEDGE) */

/*
** Construct a random password and return it as a string.  N is the
** recommended number of characters for the password.
**
** Space to hold the returned string is obtained from fossil_malloc()
** and should be freed by the caller.
*/
char *fossil_random_password(int N){
  char zSrc[60];
  int nSrc;
  int i;
  char z[60];

  /* Source characters for the password.  Omit characters like "0", "O",
  ** "1" and "I"  that might be easily confused */
  static const char zAlphabet[] =
           /*  0         1         2         3         4         5       */
           /*   123456789 123456789 123456789 123456789 123456789 123456 */
              "23456789abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ";

  if( N<8 ) N = 8;
  else if( N>sizeof(zAlphabet)-2 ) N = sizeof(zAlphabet)-2;
  nSrc = sizeof(zAlphabet) - 1;
  memcpy(zSrc, zAlphabet, nSrc);

  for(i=0; i<N; i++){
    unsigned r;
    sqlite3_randomness(sizeof(r), &r);
    r %= nSrc;
    z[i] = zSrc[r];
    zSrc[r] = zSrc[--nSrc];
  }
  z[i] = 0;
  return fossil_strdup(z);
}

/*
** COMMAND: test-random-password
**
** Usage: %fossil test-random-password ?N?
**
** Generate a random password string of approximately N characters in length.
** If N is omitted, use 10.  Values of N less than 8 are changed to 8
** and greater than 55 and changed to 55.
*/
void test_random_password(void){
  int N = 10;
  if( g.argc>=3 ){
    N = atoi(g.argv[2]);
  }
  fossil_print("%s\n", fossil_random_password(N));
}

/*
** Return the number of decimal digits in a nonnegative integer.  This is useful
** when formatting text.
*/
int fossil_num_digits(int n){
  return n<      10 ? 1 : n<      100 ? 2 : n<      1000 ? 3
       : n<   10000 ? 4 : n<   100000 ? 5 : n<   1000000 ? 6
       : n<10000000 ? 7 : n<100000000 ? 8 : n<1000000000 ? 9 : 10;
}
