#ifndef BUFIO_H
#define BUFIO_H

#include "def.h"
#include <sys/types.h>

#ifdef WINDOWS
#include <Winsock2.h>
#endif

#include "btconfig.h"

#define BUF_DEF_SIZ 256
#define BUF_INC_SIZ 256
#define BUF_MAX_SIZ (cfg_max_slice_size + BUF_DEF_SIZ + BUF_INC_SIZ)

class BufIo
{
 private:
  char *b;   // buffer
  size_t p;  // amount of data in the buffer
  size_t n;  // buffer size

  int f_socket_remote_closed;

  ssize_t _realloc_buffer();
  ssize_t _SEND(SOCKET socket, const char *buf, size_t len);
  ssize_t _RECV(SOCKET socket, char *buf, size_t len);

 public:
  BufIo();
  ~BufIo(){ if(b){ delete []b; b = (char *)0; } }

  ssize_t SetSize(size_t len);

  void Reset(){ p = 0; f_socket_remote_closed = 0; }

  void Close(){
    if( b ){ delete []b; b = (char *)0; }
    p = n = 0;
  }

  size_t Count() const { return p; }
  size_t LeftSize() const { return (n - p); }

  ssize_t PickUp(size_t len);

  ssize_t FeedIn(SOCKET sk) { return FeedIn(sk, n - p); }
  ssize_t FeedIn(SOCKET sk, size_t limit);
  ssize_t FlushOut(SOCKET sk);
  ssize_t Put(SOCKET sk, const char *buf, size_t len);
  ssize_t PutFlush(SOCKET sk, const char *buf, size_t len);

  const char *BasePointer() const { return b; }
  const char *CurrentPointer() const { return (b + p); }
};

#endif  // BUFIO_H

