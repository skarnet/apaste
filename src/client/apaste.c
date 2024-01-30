/* ISC license. */

#include <stdint.h>
#include <string.h>

#include <skalibs/uint16.h>
#include <skalibs/uint32.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr.h>
#include <skalibs/exec.h>

#include <s6-networking/config.h>

#include <apaste/config.h>

#define USAGE "apaste [ -S | -s ] [ -C cadir ] [ -d server[:port] ] [ -r rtimeout ] [ -w wtimeout ] file..."
#define dieusage() strerr_dieusage(100, USAGE)

int main (int argc, char *const *argv)
{
  char const *cadir = APASTE_DEFAULT_CADIR ;
  char const *server = APASTE_DEFAULT_SERVER ;
  int dotls = APASTE_DEFAULT_TLS ;
  uint32_t rt = 0 ;
  uint32_t wt = 0 ;
  uint16_t port = 0 ;
  PROG = "apaste" ;

  {
    subgetopt l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, (char const *const *)argv, "SsC:d:r:w:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'S' : dotls = 0 ; break ;
        case 's' : dotls = 1 ; break ;
        case 'C' : cadir = l.arg ; break ;
        case 'd' :
        {
          char *colon = strchr(l.arg, ':') ;
          server = l.arg ;
          if (colon)
          {
            *colon = 0 ;
            if (!uint160_scan(colon + 1, &port)) dieusage() ;
          }
          break ;
        }
        case 'r' : if (!uint320_scan(l.arg, &rt)) dieusage() ; break ;
        case 'w' : if (!uint320_scan(l.arg, &wt)) dieusage() ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
  }
  if (!argc) dieusage() ;
  if (!port) port = dotls ? APASTE_DEFAULT_TLSPORT : APASTE_DEFAULT_PORT ;
  {
    char const *newargv[12 + argc] ;
    unsigned int m = 0 ;
    char fmtr[UINT32_FMT] ;
    char fmtw[UINT32_FMT] ;
    char fmtp[UINT16_FMT] ;

    fmtp[uint16_fmt(fmtp, port)] = 0 ;

    newargv[m++] = dotls ? S6_NETWORKING_EXTBINPREFIX "s6-tlsclient" : S6_NETWORKING_EXTBINPREFIX "s6-tcpclient" ;
    newargv[m++] = "-N" ;
    newargv[m++] = "--" ;
    newargv[m++] = server ;
    newargv[m++] = fmtp ;
    newargv[m++] = APASTE_BINPREFIX "apastec" ;
    if (rt)
    {
      fmtr[uint32_fmt(fmtr, rt)] = 0 ;
      newargv[m++] = "-r" ;
      newargv[m++] = fmtr ;
    }
    if (rt)
    {
      fmtw[uint32_fmt(fmtw, wt)] = 0 ;
      newargv[m++] = "-w" ;
      newargv[m++] = fmtw ;
    }
    newargv[m++] = "--" ;
    while (argc--) newargv[m++] = *argv++ ;
    newargv[m++] = 0 ;

    if (dotls)
    {
      size_t len = strlen(cadir) ;
      char modif[7 + len] ;
      memcpy(modif, "CADIR=", 6) ;
      memcpy(modif + 6, cadir, len + 1) ;
      xmexec_n(newargv, modif, len + 7, 1) ;
    }
    else xexec(newargv) ;
  }
}
