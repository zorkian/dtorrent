#ifndef UTIL_H
#define UTIL_H

#include <inttypes.h>

#if !defined(HAVE_SYS_TIME_H) || defined(TIME_WITH_SYS_TIME)
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif


void RandomInit(void);
unsigned long RandBits(int nbits);
double PreciseTime(void);
int IsPrivateAddress(uint32_t addr);

char *hexencode(const unsigned char *data, size_t length=0,
  char *dstbuf=(char *)0);
char *hexencode(const char *data, size_t length=0, char *dstbuf=(char *)0);
unsigned char *hexdecode(const char *data, size_t length=0,
  unsigned char *dstbuf=(unsigned char *)0);

const char *PrettyTime(time_t timestamp);
const char *RecentTime(time_t timestamp, bool nbsp=false);

#endif  // UTIL_H

