#include "def.h"

/* compat.c:  Copyright 2007-2008 Dennis Holmes  (dholmes@rahul.net)
 *            except as noted.
 */

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#include "compat.h"


#ifndef HAVE_VSNPRINTF
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
  int r = 0;
  char *buffer = (char *)0;

  if( (buffer = (char *)malloc(size * 2)) &&
      (r = vsprintf(buffer, format, ap)) >= 0 ){
    strncpy(str, buffer, size);
    str[size - 1] = '\0';
  }
  if( buffer ) free(buffer);
  return r;
}
#endif


#ifndef HAVE_SNPRINTF
#ifdef HAVE_VSNPRINTF
#include <stdio.h>
#endif
int snprintf(char *str, size_t size, const char *format, ...)
{
  int r;
  va_list ap;

  va_start(ap, format);
  r = vsnprintf(str, size, format, ap);
  va_end(ap);

  return r;
}
#endif


#ifndef HAVE_STRNSTR
#include <string.h>
/* FUNCTION PROGRAMER: Siberiaic Sang */
char *strnstr(const char *haystack, const char *needle, size_t haystacklen)
{
  char *p;
  ssize_t plen;
  ssize_t len = strlen(needle);

  if( *needle == '\0' )
    return (char *)haystack;

  plen = haystacklen;
  for( p = (char *)haystack;
       p != (char *)0;
       p = (char *)memchr(p + 1, *needle, plen-1) ){
    plen = haystacklen - (p - haystack);

    if( plen < len ) return (char *)0;

    if( strncmp(p, needle, len) == 0 )
      return p;
  }
  return (char *)0;
}
#endif


#ifndef HAVE_STRNCASECMP
#include <ctype.h>
int strncasecmp(const char *s1, const char *s2, size_t len)
{
  unsigned char c1, c2;
  int r = 0;

  for( ; r==0 && len && *s1 && *s2; s1++, s2++, len-- ){
    if( *s1 == *s2 ) continue;
    c1 = (unsigned char)tolower((int)*s1);
    c2 = (unsigned char)tolower((int)*s2);
    if( c1 == c2 ) continue;
    else if( c1 < c2 ) r = -1;
    else if( c1 > c2 ) r = 1;
  }
  if( r==0 && len ){
    if( !*s1 && !*s2 ) r = 0;
    else if( *s1 ) r = 1;
    else r = -1;
  }
  return r;
}
#endif

#ifndef HAVE_STRCASECMP
int strcasecmp(const char *s1, const char *s2)
{
  return strncasecmp(s1, s2, strlen(s1));
}
#endif


#ifndef HAVE_RANDOM
long random(void)
{
  long result;
  unsigned long maxlong = 0, i = RAND_MAX;

  maxlong--;
  maxlong /= 2;

  result = (long)rand();
  while( i < maxlong ){
    result = (result * 2UL*(RAND_MAX+1UL)) | rand();
    i *= 2UL*(RAND_MAX+1UL);
  }
  return (result < 0) ? -result : result;
}

void srandom(unsigned long seed)
{
  unsigned useed;

  if( sizeof(seed) > sizeof(useed) ){
    unsigned long mask = 0xffff;
    int i = 2;
    while( i < sizeof(useed) ){
      mask = (mask << 16) | 0xffff;
      i += 2;
    }
    useed = (unsigned)(seed & mask);
  }else useed = seed;

  srand(useed);
}
#endif


#ifndef HAVE_NTOHL
uint32_t ntohl(uint32_t n)
{
  uint32_t h = 0;
  unsigned char *c = (unsigned char *)&n;
  h = c[0];
  h = (h << 8) | c[1];
  h = (h << 8) | c[2];
  h = (h << 8) | c[3];
  return h;
}
#endif

#ifndef HAVE_HTONL
uint32_t htonl(uint32_t h)
{
  uint32_t n = 0;
  unsigned char *c = (unsigned char *)&n;
  c[0] = h >> 24;
  c[1] = (h >> 16) & 0xff;
  c[2] = (h >> 8) & 0xff;
  c[3] = h & 0xff;
  return n;
}
#endif

#ifndef HAVE_NTOHS
uint16_t ntohs(uint16_t n)
{
  uint16_t h = 0;
  unsigned char *c = (unsigned char *)&n;
  h = c[0];
  h = (h << 8) | c[1];
  return h;
}
#endif

#ifndef HAVE_HTONS
uint16_t htons(uint16_t h)
{
  uint16_t n = 0;
  unsigned char *c = (unsigned char *)&n;
  c[0] = h >> 8;
  c[1] = h & 0xff;
  return n;
}
#endif

