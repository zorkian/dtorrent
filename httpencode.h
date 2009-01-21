#ifndef HTTPENCODE_H
#define HTTPENCODE_H

#define CR '\x0d'
#define LF '\x0a'
#define CRLF "\x0d\x0a"
#define LFLF "\x0a\x0a"
#define LFCR "\x0a\x0d"

char *urlencode(char *dst, const unsigned char *src, size_t len);
int UrlSplit(const char *url, char **host, int *port, char **path);
size_t HttpSplit(const char *buf, size_t blen, const char **data, size_t *dlen);
int HttpGetStatusCode(const char *buf, size_t blen);
int HttpGetHeader(const char *buf, size_t remain, const char *header,
  char **value);

#endif  // HTTPENCODE_H

