#include "config.h"
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#if !defined(HAVE_SYS_TIME_H) || defined(TIME_WITH_SYS_TIME)
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "util.h"

#ifndef HAVE_RANDOM
#include "compat.h"
#endif


void RandomInit(void)
{
  unsigned long seed;
#ifdef HAVE_GETTIMEOFDAY
  struct timeval tv;
  gettimeofday(&tv, (struct timezone *)0);
  seed = tv.tv_usec + tv.tv_sec + getpid();
#else
  seed = (unsigned long)time((time_t *)0);
#endif
  srandom(seed);
}


unsigned long RandBits(int nbits)
{
  static int remain = 0, maxbits = 8;
  static unsigned long rndbits;
  unsigned long result, tmpbits;

  if( remain < nbits ){
    rndbits = random();
    if( rndbits & ~((1UL << maxbits) - 1) ){
      tmpbits = rndbits >> maxbits;
      while( tmpbits ){
        maxbits++;
        tmpbits >>= 1;
      }
    }
    remain = maxbits;
  }
  result = rndbits & ((1 << nbits) - 1);
  rndbits >>= nbits;
  remain -= nbits;
  return result;
}


double PreciseTime()
{
#ifdef HAVE_CLOCK_GETTIME
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
  struct timespec nowspec;
  clock_gettime(CLOCK_REALTIME, &nowspec);
  return nowspec.tv_sec + (double)nowspec.tv_nsec/1000000000;
#elif defined(HAVE_GETTIMEOFDAY)
  struct timeval tv;
  gettimeofday(&tv, (struct timezone *)0);
  return tv.tv_sec + (double)tv.tv_usec/1000000;
#else
#error No suitable precision timing functions appear to be available!
#error Please report this problem and identify your system platform.
#endif
}

