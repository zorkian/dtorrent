#include "def.h"

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

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


#ifndef HAVE_VSNPRINTF
int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
  int r;
  char *buffer[4*MAXPATHLEN];  // git-r-dun

  if( (r = vsprintf(buffer, format, ap)) >= 0){
    strncpy(str, buffer, size-1);
    str[size] = '\0';
  }
  return r;
}
#endif


#ifndef HAVE_SNPRINTF
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

