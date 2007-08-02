#include "def.h"
#include "compat.h"

#ifndef HAVE_CLOCK_GETTIME
int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
  int r = 0;

#if defined(HAVE_GETTIMEOFDAY)
  struct timeval tv;
  if( (r = gettimeofday(&tv, (struct timezone *)0)) == 0 ){
    tp->tv_sec = tv.tv_sec;
    tp->tv_nsec = tv.tv_usec * 1000;
  }
#else
#error No suitable precision timing functions appear to be available!
#error Please report this problem and identify your system platform.
#endif

  return r;
}
#endif

