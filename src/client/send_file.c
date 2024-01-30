/* ISC license. */

#include <skalibs/sysdeps.h>

#ifdef SKALIBS_HASSENDFILE

#include <sys/types.h>
#include <sys/sendfile.h>
#include <limits.h>

#include <skalibs/uint64.h>
#include <skalibs/strerr.h>
#include <skalibs/tai.h>
#include <skalibs/unix-timed.h>

struct sendfile_s
{
  int fd ;
  buffer *out ;
  off_t pos ;  
  uint64_t n ;
} ;

static int s_getfd (void *p)
{
  struct sendfile_s *sf = p ;
  return buffer_fd(sf->out) ;
}

static int s_isnonempty (void *p)
{
  struct sendfile_s *sf = p ;
  return !!sf->n ;
}

static int s_flush (void *p)
{
  struct sendfile_s *sf = p ;
  while (sf->n)
  {
    ssize_t r = sendfile(buffer_fd(sf->out), sf->fd, &sf->pos, sf->n > SSIZE_MAX ? SSIZE_MAX : sf->n) ;
    if (r == -1) return 0 ;
    sf->n -= r ;
  }
  return 1 ;
}

void send_file (buffer *b, int fd, off_t size, char const *fn, tain const *deadline, tain *stamp)
{
  struct sendfile_s sf = { .fd = fd, .out = b, .pos = 0, .n = size } ;
  if (!buffer_timed_flush(b, deadline, stamp))
    strerr_diefu1sys(111, "write to network") ;
  if (!timed_flush(&sf, &s_getfd, &s_isnonempty, &s_flush, deadline, stamp))
    strerr_diefu3sys(111, "sendfile ", fn, " to network") ;
}

#else

#include <sys/uio.h>
#include <unistd.h>
#include <stdint.h>

#include <skalibs/uint64.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/buffer.h>
#include <skalibs/strerr.h>
#include <skalibs/tai.h>
#include <skalibs/unix-timed.h>

void send_file (buffer *b, int fd, uint64_t n, char const *fn, tain const *deadline, tain *stamp)
{
  struct iovec v[2] ;
  ssize_t r ;
  if (!n) goto flushit ;
 fillit:
  buffer_wpeek(b, v) ;
  r = allreadv(fd, v, 2) ;
  if (r == -1) strerr_diefu2sys(111, "read from ", fn) ;
  if (!r) strerr_diefu3x(111, "send ", fn, ": file was truncated") ;
  if (r > n)
  {
    r = n ;
    strerr_warnw2x("sending elongated file: ", fn) ;
  }
  buffer_wseek(b, r) ;
  n -= r ;
 flushit:
  if (!buffer_timed_flush(b, deadline, stamp))
    strerr_diefu1sys(111, "write to network") ;
  if (n) goto fillit ;
}

#endif
