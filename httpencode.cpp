#include "def.h"
#include <sys/types.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include "httpencode.h"

#if !defined(HAVE_STRNSTR) || !defined(HAVE_STRNCASECMP)
#include "compat.h"
#endif

static void url_encode_char(char *dst, unsigned char src)
{
  char HEX_TABLE[] = "0123456789ABCDEF";
  dst[0] = '%';
  dst[1] = HEX_TABLE[(src >> 4) & 0x0f];
  dst[2] = HEX_TABLE[src & 0x0f];
}

char *urlencode(char *dst, const unsigned char *src, size_t len)
{
  size_t r, i;

  for( r = 0, i = 0; i < len; i++ ){
    if( !(src[i] & ~0x7f) &&                     // quick ASCII test
        ((src[i] >= 0x41 && src[i] <= 0x5a) ||   // A-Z [ASCII]
         (src[i] >= 0x61 && src[i] <= 0x7a) ||   // a-z
         (src[i] >= 0x30 && src[i] <= 0x39)) ){  // 0-9
      dst[r] = (char)src[i];
      r++;
    }else{
      url_encode_char(dst + r, src[i]);
      r += 3;
    }
  }
  dst[r] = '\0';
  return dst;
}

int UrlSplit(const char *url, char **host, int *port, char **path)
{
  const char *p;
  int r;

  *port = 80;  // default port 80
  p = strstr(url, "://");
  if( !p )
    p = url;
  else
    p += 3;

  /* host */
  for( r = 0;
       p[r] && ((p[r] >= 'a' && p[r] <= 'z') || (p[r] >= 'A' && p[r] <= 'Z') ||
                (p[r] >= '0' && p[r] <= '9') || p[r] == '.' || p[r] == '-');
       r++ );
  if( !(*host = new char[r + 1]) ){
    errno = ENOMEM;
    return -1;
  }
  strncpy(*host, p, r);
  (*host)[r] = '\0';
  p += r;

  if( *p == ':' ){
    /* port */
    p++;
    for( r = 0; p[r] >= '0' && p[r] <= '9' && r < 6; r++ );
    if( 0==r ){
      errno = EINVAL;
      return -1;
    }
    *port = atoi(p);
    if( *port > 65536 ) return -1;
    p += r;
  }

  /* path */
  if( *p != '/' ){
    errno = 0;
    return -1;
  }
  if( !(*path = new char[strlen(p) + 1]) ){
    errno = ENOMEM;
    return -1;
  }
  strcpy(*path, p);

  return 0;
}

size_t HttpSplit(const char *buf, size_t blen, const char **data, size_t *dlen)
{
  char *p;
  size_t addition = 0, hlen;

  hlen = 0;

  if( blen < 16 ) return 0;  // too small to contain an HTTP header

  if( strncasecmp(buf, "HTTP/", 5) )
    return 0;  // no HTTP header
  else{
    if( (p = strnstr(buf, CRLF CRLF, blen)) ) addition = 4;
    else if( (p = strnstr(buf, LFLF, blen)) ) addition = 2;
    else if( (p = strnstr(buf, LFCR LFCR, blen)) ) addition = 4;
    else return 0;

    if( p ){
      hlen = p - buf;
      *data = ( p + addition );
      *dlen = blen - hlen - addition;
    }else{
      hlen = blen;
      *data = (char *)0;
      *dlen = 0;
    }
  }
  return hlen;
}

int HttpGetStatusCode(const char *buf, size_t blen)
{
  int r = -1;

  for( ; blen && *buf != ' ' && *buf != CR && *buf != LF; buf++, blen-- );
  if( !blen || *buf != ' ' ) r = -1;
  else{
    r = atoi(buf);
    if( r < 100 || r > 600 ) r = -1;
  }
  return r;
}

int HttpGetHeader(const char *buf, size_t remain, const char *header,
  char **value)
{
  const char *begin = buf, *end;
  char h[64];
  size_t line_len, header_len;

  strcpy(h, header);
  strcat(h, ": ");
  header_len = strlen(h);

  /* remove status line. */
  end = strchr(begin, LF);
  if( !end ) return -1;
  end++;
  remain -= (end - begin);
  begin = end;

  while( remain > header_len ){
    end = strchr(begin, LF);
    if( !end ) line_len = remain;  // last line
    else line_len = end - begin + 1;

    if( line_len > header_len && 0==strncasecmp(begin, h, header_len) ){
      size_t i = 0;
      begin += header_len;
      for( ; begin[i] != CR && begin[i] != LF && i < line_len - header_len;
           i++ );
      if( !(*value = new char[i + 1]) ){
        errno = ENOMEM;
        return -1;
      }
      memcpy(*value, begin, i);
      (*value)[i] = '\0';
      return 0;
    }
    begin += line_len;
    remain -= line_len;
  }
  errno = 0;
  return -1;
}

