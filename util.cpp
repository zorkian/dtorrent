#include "config.h"
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>  // htonl()
#include <arpa/inet.h>   // htonl()
#include <unistd.h>
#include <string.h>

#include "util.h"

#if !defined(HAVE_RANDOM) || !defined(HAVE_HTONL)
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


int IsPrivateAddress(uint32_t addr)
{
  return (addr & htonl(0xff000000)) == htonl(0x0a000000) ||  // 10.x.x.x/8
         (addr & htonl(0xfff00000)) == htonl(0xac100000) ||  // 172.16.x.x/12
         (addr & htonl(0xffff0000)) == htonl(0xc0a80000) ||  // 192.168.x.x/16
         (addr & htonl(0xff000000)) == htonl(0x7f000000);    // 127.x.x.x/8
}


char *hexencode(const unsigned char *data, size_t length, char *dstbuf)
{
  static char hexdigit[17] = "0123456789abcdef";
  const unsigned char *src, *end;
  char *dst;

  if( 0==length ) length = strlen((char *)data);
  end = data + length;
  if( !dstbuf ) dstbuf = new char[length * 2 + 1];
  dst = dstbuf;

  if( dst ){
    for( src = data; src < end; src++ ){
      *dst++ = hexdigit[*src >> 4];
      *dst++ = hexdigit[*src & 0x0f];
    }
    *dst = '\0';
  }
  return dstbuf;
}


char *hexencode(const char *data, size_t length, char *dstbuf)
{
  return hexencode((unsigned char *)data, length, dstbuf);
}


unsigned char *hexdecode(const char *data, size_t length, unsigned char *dstbuf)
{
  const char *src, *end;
  unsigned char c, *dst;

  if( 0==length ) length = strlen(data);
  end = data + length;
  if( !dstbuf ) dstbuf = new unsigned char[length / 2 + 1];
  dst = dstbuf;

  if( dst ){
    for( src = data; src < end; src += 2, dst++ ){
      c = *src;
      if( c >= '0' && c <= '9' ) *dst = (c - '0') << 4;
      else if( c >= 'a' && c <= 'f' ) *dst = (c - 'a' + 10) << 4;
      else *dst = 0;
      c = *(src + 1);
      if( c >= '0' && c <= '9' ) *dst |= (c - '0');
      else if( c >= 'a' && c <= 'f' ) *dst |= (c - 'a' + 10);
    }
    *dst = '\0';
  }
  return dstbuf;
}


const char *PrettyTime(time_t timestamp)
{
  static char result[32];

#ifdef HAVE_CTIME_R_3
  ctime_r(&timestamp, result, sizeof(result));
#else
  ctime_r(&timestamp, result);
#endif
  if( result[strlen(result)-1] == '\n' )
    result[strlen(result)-1] = '\0';
  return result;
}


const char *RecentTime(time_t timestamp, bool nbsp)
{
  static char result[48];

  strftime(result, sizeof(result),
    nbsp ? "%a&nbsp;%d&nbsp;%b&nbsp;%X&nbsp;%Z" : "%a %d %b %X %Z",
    localtime(&timestamp));
  return result;
}

