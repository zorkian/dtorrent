#ifndef COMPAT_H
#define COMPAT_H
/* compat.h:  Copyright 2007 Dennis Holmes  (dholmes@rahul.net) */

#ifdef __cplusplus
extern "C" {
#endif


#ifndef HAVE_VSNPRINTF
#include <stdarg.h>
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
#endif

#ifndef HAVE_SNPRINTF
#include <stdarg.h>
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


#ifdef __cplusplus
}
#endif
#endif  // COMPAT_H

