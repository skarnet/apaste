/* ISC license. */

#ifndef APASTEC_H
#define APASTEC_H

#include <sys/types.h>

#include <skalibs/buffer.h>
#include <skalibs/tai.h>


/* send_file.c */

extern void send_file (buffer *, int, off_t, char const *, tain const *, tain *) ;
#define send_file_g(b, fd, size, file, deadline) send_file(b, fd, size, file, (deadline), &STAMP)

#endif
