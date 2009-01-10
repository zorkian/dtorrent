#ifndef BUFIO_H
#define BUFIO_H

#include "def.h"
#include <sys/types.h>

#ifdef WINDOWS
#include <Winsock2.h>
#endif

#define BUFIO_DEF_SIZ 256
#define BUFIO_INC_SIZ 256

class BufIo
{
 private:
  char *m_buf;    // buffer
  size_t m_len;   // amount of data in the buffer
  size_t m_size;  // buffer size
  size_t m_max;   // max buffer size

  unsigned char m_valid:1;
  unsigned char m_socket_remote_closed:1;
  unsigned char m_reserved:6;

  ssize_t _realloc_buffer();
  ssize_t _SEND(SOCKET socket, const char *buf, size_t len);
  ssize_t _RECV(SOCKET socket, char *buf, size_t len);

 public:
  BufIo();
  ~BufIo(){ Close(); }

  void MaxSize(size_t len);
  ssize_t SetSize(size_t len);

  void Reset(){ m_len = 0; m_socket_remote_closed = 0; m_valid = 1; }

  void Close(){
    if( m_buf ){ delete []m_buf; m_buf = (char *)0; }
    m_len = m_size = 0;
  }

  size_t Count() const { return m_len; }
  size_t LeftSize() const { return (m_size - m_len); }

  ssize_t PickUp(size_t len);

  ssize_t FeedIn(SOCKET sk) { return FeedIn(sk, m_size - m_len); }
  ssize_t FeedIn(SOCKET sk, size_t limit);
  ssize_t FlushOut(SOCKET sk);
  ssize_t Put(SOCKET sk, const char *buf, size_t len);
  ssize_t PutFlush(SOCKET sk, const char *buf, size_t len);

  const char *BasePointer() const { return m_buf; }
  const char *CurrentPointer() const { return (m_buf + m_len); }
};

#endif  // BUFIO_H

