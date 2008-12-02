#ifndef UTIL_H
#define UTIL_H

void RandomInit(void);
unsigned long RandBits(int nbits);
double PreciseTime(void);
char *hexencode(const unsigned char *data, size_t length=0,
  char *dstbuf=(char *)0);
char *hexencode(const char *data, size_t length=0, char *dstbuf=(char *)0);
unsigned char *hexdecode(const char *data, size_t length=0,
  unsigned char *dstbuf=(unsigned char *)0);

#endif  // UTIL_H

