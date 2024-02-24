/* ISC license. */

#include <stdint.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>

#include <skalibs/posixplz.h>
#include <skalibs/uint16.h>
#include <skalibs/uint32.h>
#include <skalibs/uint64.h>
#include <skalibs/bytestr.h>
#include <skalibs/siovec.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/buffer.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr.h>
#include <skalibs/tai.h>
#include <skalibs/stralloc.h>
#include <skalibs/sig.h>
#include <skalibs/djbunix.h>
#include <skalibs/unix-timed.h>

#include <apaste/config.h>
#include "apaste-common.h"

#define USAGE "apasted [ -r rtimeout ] [ -w wtimeout ] [ -d rootdir ] [ -p prefix ] [ -n maxfiles ] [ -s maxsize ] [ -S maxtotalsize ]"
#define dieusage() strerr_dieusage(100, USAGE)

#define FORBIDDEN " \t\r\n\"<>&;"

#define INDEXSTART "\
<html>\n\
  <head>\n\
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />\n\
    <meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />\n\
    <meta http-equiv=\"Content-Language\" content=\"en\" />\n\
    <title>apaste results</title>\n\
    <meta name=\"Description\" content=\"apaste results\" />\n\
  </head>\n\
<body>\n\
<h3> apaste results </h3>\n\
<ul>\n"

static void cleanup (char const *dir)
{
  int e = errno ;
  rm_rf(dir) ;
  errno = e ;
}

static void prepare_index (buffer *ib, char const *dir, char *buf, size_t buflen)
{
  int fd ;
  size_t dirlen = strlen(dir) ;
  char fn[dirlen + 12] ;
  memcpy(fn, dir, dirlen) ;
  memcpy(fn + dirlen, "/index.html", 12) ;
  fd = open_create(fn) ;
  if (fd == -1)
  {
    cleanup(dir) ;
    strerr_diefu3sys(111, "open ", dir, "/index.html for writing") ;
  }
  buffer_init(ib, &buffer_write, fd, buf, buflen) ;
  if (buffer_puts(ib, INDEXSTART) == -1)
  {
    cleanup(dir) ;
    strerr_diefu2sys(111, "prepare index for ", dir) ;
  }
}

static inline void add_index_entry (char const *dir, buffer *ib, char const *name, uint64_t filelen)
{
  char fmt[UINT64_FMT] ;
  size_t m = uint64_fmt(fmt, filelen) ;
  if (buffer_puts(ib, " <li> <a href=\"") == -1
   || buffer_puts(ib, name) == -1
   || buffer_puts(ib, ".txt\">") == -1
   || buffer_puts(ib, name) == -1
   || buffer_puts(ib, "</a> (") == -1
   || buffer_put(ib, fmt, m) == -1
   || buffer_puts(ib, " bytes) </li>\n") == -1)
  {
    cleanup(dir) ;
    strerr_diefu5sys(111, "add ", name, " to ", dir, "/index.html") ;
  }
}

static inline void finish_index (buffer *ib, char const *dir)
{
  if (buffer_putsflush(ib, "</ul>\n</body>\n</html>\n") == -1
   || fsync(buffer_fd(ib)) == -1)
  {
    cleanup(dir) ;
    strerr_diefu2sys(111, "finish index for ", dir) ;
  }
  fd_close(buffer_fd(ib)) ;
}

static inline void recv_one_file (char const *dir, char const *fn, buffer *b, int fd, uint64_t n, tain const *deadline)
{
  struct iovec v[2] ;
  size_t len ;
  while (n)
  {
    unsigned int j = 2 ;
    if (buffer_len(b) < n)
    {
      if (sanitize_read(buffer_timed_fill_g(b, deadline)) <= 0)
      {
        cleanup(dir) ;
        _exit(1) ;
      }
    }
    buffer_rpeek(b, v) ;
    len = siovec_len(v, 2) ;
    if (len > n)
    {
      j = siovec_trunc(v, 2, n) ;
      len = n ;
    }
    if (allwritev(fd, v, j) < len)
    {
      cleanup(dir) ;
      strerr_diefu2sys(111, "write to ", fn) ;
    }
    buffer_rseek(b, len) ;
    n -= len ;
  }
}

static inline size_t add_unique (stralloc *sa, size_t *indices, uint32_t n, char const *bname)
{
  size_t blen = strlen(bname) ;
  size_t pos = sa->len ;
  static uint32_t suffix = 0 ;
  if (!stralloc_readyplus(sa, blen + UINT32_FMT + 2)) return 0 ;
  memcpy(sa->s + pos, bname, blen + 1) ;
  for (;;)
  {
    uint32_t i = 0 ;
    for (; i < n ; i++) if (!strcmp(sa->s + indices[i], sa->s + pos)) break ;
    if (i == n) break ;
    sa->s[pos + blen] = '.' ;
    sa->s[pos + blen + 1 + uint32_fmt(sa->s + pos + blen + 1, suffix++)] = 0 ;
  }
  
  blen = strlen(sa->s + pos) ;
  indices[n] = sa->len ;
  sa->len += blen + 1 ;
  return blen ;
}

static void read_one_file (char const *dir, buffer *b, buffer *ib, stralloc *sa, size_t *indices, uint32_t n, uint64_t maxsize, uint64_t maxtotalsize, uint64_t *cursize, tain const *deadline)
{
  uint64_t filelen ;
  size_t bnamelen ;
  uint16_t namelen ;

  {
    size_t w = 0 ;
    size_t m ;
    char fmt[UINT16_FMT] ;
    if (sanitize_read(timed_getlnmax_g(b, fmt, UINT16_FMT, &w, '\n', deadline)) <= 0) goto err ;
    m = uint16_scan(fmt, &namelen) ;
    if (!m || m+1 != w || fmt[m] != '\n') goto err ;
  }

  {
    size_t w = 0 ;
    char *bname ;
    char name[namelen + 1] ;
    if (sanitize_read(timed_getlnmax_g(b, name, namelen + 1, &w, '\n', deadline)) <= 0) goto err ;
    if (!w || w != namelen + 1 || name[namelen] != '\n') goto err ;
    name[namelen] = 0 ;
    bname = strrchr(name, '/') ;
    if (bname) bname++ ; else bname = name ;
    if (!*bname) goto err ;
    
    if (ib)  /* sanitize the filename for inclusion in index.html */
    {
      size_t blen = strlen(bname) ;
      if (byte_in(bname, blen, FORBIDDEN, sizeof(FORBIDDEN)) != blen) goto err ;
      bnamelen = add_unique(sa, indices, n, bname) ;
      if (!bnamelen)
      {
        cleanup(dir) ;
        strerr_diefu3sys(111, "make ", bname, " unique") ;
      }
    }
    else bnamelen = 5 ;
  }

  {
    size_t w = 0 ;
    size_t m ;
    char fmt[UINT64_FMT] ;
    if (sanitize_read(timed_getlnmax_g(b, fmt, UINT64_FMT, &w, '\n', deadline)) <= 0) goto err ;
    m = uint64_scan(fmt, &filelen) ;
    if (!m || m+1 != w || fmt[m] != '\n' || (maxsize && filelen > maxsize) || (maxtotalsize && (filelen > maxtotalsize || *cursize + filelen > maxtotalsize))) goto err ;
  }

  {
    int fd ;
    size_t dirlen = strlen(dir) ;
    char fn[dirlen + bnamelen + 6] ;
    memcpy(fn, dir, dirlen) ;
    fn[dirlen] = '/' ;
    memcpy(fn + dirlen + 1, ib ? sa->s + indices[n] : "index", bnamelen) ;
    memcpy(fn + dirlen + 1 + bnamelen, ".txt", 5) ;
    fd = open_create(fn) ;
    if (fd == -1)
    {
      cleanup(dir) ;
      strerr_diefu3sys(111, "open ", fn, " for writing") ;
    }
    recv_one_file(dir, fn, b, fd, filelen, deadline) ;
    if (fsync(fd) == -1)
    {
      cleanup(dir) ;
      strerr_diefu2sys(111, "fsync ", fn) ;
    }
    fd_close(fd) ;
  }

  {
    char c ;
    if (sanitize_read(buffer_timed_get_g(b, &c, 1, deadline)) != 1) goto err ;
    if (c != '\n') goto err ;
  }

  *cursize += filelen ;

  if (ib) add_index_entry(dir, ib, sa->s + indices[n], filelen) ;
  return ;

 err:
  cleanup(dir) ;
  _exit(1) ;
}

int main (int argc, char const *const *argv)
{
  char const *prefix = "" ;
  tain rtto = TAIN_INFINITE_RELATIVE, wtto = TAIN_INFINITE_RELATIVE ;
  tain deadline ;
  uint64_t maxtotalsize = 10485760 ;
  uint64_t cursize = 0 ;
  uint64_t maxsize = 1048576 ;
  uint32_t maxfiles = 0 ;
  uint32_t n ;
  char buf[4097] ;
  char dir[7] = "XXXXXX" ;
  buffer b = BUFFER_INIT(&buffer_read, 0, buf, 4097) ;
  PROG = "apasted" ;
  {
    char const *rootdir = 0 ;
    uint32_t rt = 0, wt = 0 ;
    subgetopt l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, argv, "r:w:d:p:n:s:S:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'r' : if (!uint320_scan(l.arg, &rt)) dieusage() ; break ;
        case 'w' : if (!uint320_scan(l.arg, &wt)) dieusage() ; break ;
        case 'd' : rootdir = l.arg ; break ;
        case 'p' : prefix = l.arg ; break ;
        case 'n' : if (!uint320_scan(l.arg, &maxfiles)) dieusage() ; break ;
        case 's' : if (!uint640_scan(l.arg, &maxsize)) dieusage() ; break ;
        case 'S' : if (!uint640_scan(l.arg, &maxtotalsize)) dieusage() ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
    if (rt) tain_from_millisecs(&rtto, rt) ;
    if (wt) tain_from_millisecs(&rtto, wt) ;
    if (rootdir && chdir(rootdir) == -1)
      strerr_diefu2sys(111, "chdir to ", rootdir) ;
    if (strlen(prefix) > 4000) strerr_dief1x(100, "prefix too long") ;
  }

  if (!sig_altignore(SIGPIPE))
    strerr_diefu1sys(111, "ignore SIGPIPE") ;
  tain_now_set_stopwatch_g() ;
  tain_add_g(&deadline, &rtto) ;

  {
    char banner[256] ;
    size_t w = 0 ;
    if (sanitize_read(timed_getlnmax_g(&b, banner, 256, &w, '\n', &deadline)) <= 0) _exit(1) ;
    if (w != sizeof(APASTEC_BANNER) || memcmp(banner, APASTEC_BANNER, w-1)) _exit(1) ;
  }

  {
    char fmt[UINT32_FMT] ;
    size_t w = 0 ;
    size_t m ;
    if (sanitize_read(timed_getlnmax_g(&b, fmt, UINT32_FMT, &w, '\n', &deadline)) <= 0)
      strerr_diefu1sys(111, "read from apaste server") ;
    m = uint32_scan(fmt, &n) ;
    if (!m || m+1 != w) _exit(1) ;
    if (!n || (maxfiles && n > maxfiles)) _exit(1) ;
  }

  if (!mkdtemp(dir)) strerr_diefu1sys(111, "mkdtemp") ;
  if (n == 1) read_one_file(dir, &b, 0, 0, 0, 0, maxsize, maxtotalsize, &cursize, &deadline) ;
  else
  {
    stralloc sa = STRALLOC_ZERO ;
    size_t indices[n] ;
    buffer ib ;
    char ibuf[4096] ;
    prepare_index(&ib, dir, ibuf, 4096) ;
    for (uint32_t i = 0 ; i < n ; i++) read_one_file(dir, &b, &ib, &sa, indices, i, maxsize, maxtotalsize, &cursize, &deadline) ;
    finish_index(&ib, dir) ;
  }

  {
    char banner[256] ;
    size_t w = 0 ;
    if (sanitize_read(timed_getlnmax_g(&b, banner, 256, &w, '\n', &deadline)) <= 0
     || w != sizeof(APASTEC_BANNER)
     || memcmp(banner, APASTEC_BANNER, w-1)) goto err ;
  }

  buffer_init(&b, &buffer_write, 1, buf, 4097) ;
  buffer_putsnoflush(&b, APASTED_BANNER "\n") ;
  if (prefix[0]) buffer_putsnoflush(&b, prefix) ;
  buffer_putnoflush(&b, dir, 6) ;
  if (!strncmp(prefix, "http", 4)) buffer_putnoflush(&b, "/", 1) ;
  buffer_putsnoflush(&b, "\n" APASTED_BANNER "\n") ;
  tain_add_g(&deadline, &wtto) ;
  if (!buffer_timed_flush_g(&b, &deadline)) goto err ;

  return 0 ;

 err:
  cleanup(dir) ;
  return 1 ;
}
