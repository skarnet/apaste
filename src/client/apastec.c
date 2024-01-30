/* ISC license. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <signal.h>

#include <skalibs/posixplz.h>
#include <skalibs/uint16.h>
#include <skalibs/uint32.h>
#include <skalibs/uint64.h>
#include <skalibs/buffer.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr.h>
#include <skalibs/tai.h>
#include <skalibs/sig.h>
#include <skalibs/djbunix.h>
#include <skalibs/unix-timed.h>

#include <apaste/config.h>
#include "apaste-common.h"
#include "apastec.h"

#define USAGE "apastec [ -r rtimeout ] [ -w wtimeout ] file..."
#define dieusage() strerr_dieusage(100, USAGE)

static void apaste_send (buffer *b, char const *file, tain const *deadline)
{
  int fd ;
  struct stat st ;
  size_t len = strlen(file) ;
  if (!len) strerr_dief2x(100, "empty file", " names are invalid") ;
  if (len > UINT16_MAX) strerr_dief2x(100, "file name too long: ", file) ;
  if (len == 1 && file[0] == '-')
  {
    char tmp[sizeof(APASTE_TMPDIR) + 14] = APASTE_TMPDIR "/apaste:XXXXXX" ;
    fd = mkstemp(tmp) ;
    if (fd == -1) strerr_diefu2sys(111, "mkstemp ", tmp) ;
    unlink_void(tmp) ;
    if (fd_cat(0, fd) == -1) strerr_diefu2sys(111, "copy stdin to ", tmp) ;
    if (lseek(fd, 0, SEEK_SET) == -1) strerr_diefu1sys(111, "lseek") ;
    ndelay_on(fd) ;
  }
  else
  {
    if (file[len-1] == '/') strerr_dief2x(100, "directory", " names are invalid") ;
    fd = open_read(file) ;
    if (fd == -1) strerr_diefu2sys(111, "open ", file) ;
  }

  if (fstat(fd, &st) == -1) strerr_diefu2sys(111, "stat ", file) ;
  if (!S_ISREG(st.st_mode)) strerr_dief3x(100, "file ", file, " is not a regular file") ;
  {
    char fmt[UINT64_FMT] ;
    size_t m = uint16_fmt(fmt, len) ;
    fmt[m++] = '\n' ;
    if (buffer_timed_put_g(b, fmt, m, deadline) < m
     || buffer_timed_put_g(b, file, len, deadline) < len
     || buffer_timed_put_g(b, "\n", 1, deadline) < 1)
      strerr_diefu1sys(111, "write to server") ;
    m = uint64_fmt(fmt, st.st_size) ;
    fmt[m++] = '\n' ;
    if (buffer_timed_put_g(b, fmt, m, deadline) < m)
      strerr_diefu1sys(111, "write to server") ;
  }
  send_file_g(b, fd, st.st_size, file, deadline) ;
  fd_close(fd) ;
  if (buffer_timed_put_g(b, "\n", 1, deadline) < 1)
    strerr_diefu1sys(111, "write to server") ;
}

int main (int argc, char const *const *argv)
{
  tain rtto = TAIN_INFINITE_RELATIVE, wtto = TAIN_INFINITE_RELATIVE ;
  tain deadline ;
  PROG = "apastec" ;
  {
    uint32_t rt = 0, wt = 0 ;
    subgetopt l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, argv, "r:w:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'r' : if (!uint320_scan(l.arg, &rt)) dieusage() ; break ;
        case 'w' : if (!uint320_scan(l.arg, &wt)) dieusage() ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
    if (rt) tain_from_millisecs(&rtto, rt) ;
    if (wt) tain_from_millisecs(&rtto, wt) ;
  }
  if (!argc) dieusage() ;
  if (argc > UINT32_MAX) strerr_dief1x(100, "too many arguments") ;

  if (!sig_altignore(SIGPIPE))
    strerr_diefu1sys(111, "ignore SIGPIPE") ;
  tain_now_set_stopwatch_g() ;
  tain_add_g(&deadline, &wtto) ;

  {
    char buf[BUFFER_OUTSIZE] ;
    buffer b = BUFFER_INIT(&buffer_write, 7, buf, BUFFER_OUTSIZE) ;
    buffer_putnoflush(&b, APASTEC_BANNER "\n", sizeof(APASTEC_BANNER)) ;
    {
      char fmt[UINT32_FMT] ;
      size_t m = uint32_fmt(fmt, argc) ;
      fmt[m++] = '\n' ;
      buffer_putnoflush(&b, fmt, m) ;
    }
    {
      int gotstdin = 0 ;
      for (unsigned int i = 0 ; i < argc ; i++) if (argv[i][0] == '-' && !argv[i][1])
      {
        if (gotstdin) strerr_dief1x(100, "stdin specified more than once") ;
        gotstdin = 1 ;
      }
    }
    for (unsigned int i = 0 ; i < argc ; i++) apaste_send(&b, argv[i], &deadline) ;
    if (buffer_timed_put_g(&b, APASTEC_BANNER "\n", sizeof(APASTEC_BANNER), &deadline) < sizeof(APASTEC_BANNER)
     || !buffer_timed_flush_g(&b, &deadline))
      strerr_diefu1sys(111, "write to apaste server") ;
  }

  fd_shutdown(7, 1) ;
  fd_close(7) ;
  tain_add_g(&deadline, &rtto) ;

  {
    char buf[BUFFER_INSIZE] ;
    char banner[256] ;
    char slug[256] ;
    buffer b = BUFFER_INIT(&buffer_read, 6, buf, BUFFER_INSIZE) ;
    size_t w = 0, sluglen = 0 ;
    if (sanitize_read(timed_getlnmax_g(&b, banner, 256, &w, '\n', &deadline)) <= 0)
      strerr_diefu1sys(111, "read from apaste server") ;
    if (w != sizeof(APASTED_BANNER) || memcmp(banner, APASTED_BANNER, w-1))
      strerr_dief1x(1, "server returned invalid data") ;
    if (sanitize_read(timed_getlnmax_g(&b, slug, 256, &sluglen, '\n', &deadline)) <= 0)
      strerr_diefu1sys(111, "read from apaste server") ;
    w = 0 ;
    if (sanitize_read(timed_getlnmax_g(&b, banner, 256, &w, '\n', &deadline)) <= 0)
      strerr_diefu1sys(111, "read from apaste server") ;
    if (w != sizeof(APASTED_BANNER) || memcmp(banner, APASTED_BANNER, w-1))
      strerr_dief1x(1, "server returned invalid data") ;
    if (buffer_putflush(buffer_1small, slug, sluglen) == -1)
      strerr_diefu1sys(111, "write to stdout") ;
  }

  return 0 ;
}
