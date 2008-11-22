#ifndef COMPAT_H
#define COMPAT_H
/* compat.h:  Copyright 2007-2008 Dennis Holmes  (dholmes@rahul.net) */

#ifdef __cplusplus
extern "C" {
#endif


#if !defined(HAVE_VSNPRINTF) || !defined(HAVE_SNPRINTF)
#include <stdarg.h>
#endif

#ifndef HAVE_VSNPRINTF
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
#endif

#ifndef HAVE_SNPRINTF
int snprintf(char *str, size_t size, const char *format, ...);
#endif


#ifndef HAVE_STRNSTR
char *strnstr(const char *haystack, const char *needle, size_t haystacklen);
#endif

#ifndef HAVE_STRNCASECMP
int strncasecmp(const char *s1, const char *s2, size_t len);
#endif

#ifndef HAVE_STRCASECMP
int strcasecmp(const char *s1, const char *s2);
#endif


#ifndef HAVE_RANDOM
long random(void);
void srandom(unsigned long seed);
#endif


#ifndef HAVE_FSEEKO
#define fseeko(stream, offset, whence) \
        fseek((stream), (long)(offset), (whence))
#endif


#if !defined(HAVE_NTOHL) || !defined(HAVE_HTONL) || \
    !defined(HAVE_NTOHS) || !defined(HAVE_HTONS)
#include <inttypes.h>
#endif

#ifndef HAVE_NTOHL
uint32_t ntohl(uint32_t n);
#endif

#ifndef HAVE_HTONL
uint32_t htonl(uint32_t h);
#endif

#ifndef HAVE_NTOHS
uint16_t ntohs(uint16_t n);
#endif

#ifndef HAVE_HTONS
uint16_t htons(uint16_t h);
#endif


#ifdef __cplusplus
}
#endif

#endif  /* COMPAT_H */

