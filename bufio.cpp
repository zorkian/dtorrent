#include "bufio.h"

#ifndef WINDOWS
#include <unistd.h>
#include <stdio.h>   // autoconf manual: Darwin + others prereq for stdlib.h
#include <stdlib.h>  // autoconf manual: Darwin prereq for sys/socket.h
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <string.h>
#include <errno.h>

#include "btrequest.h"

BufIo::BufIo()
{
  m_valid = 1;
  m_socket_remote_closed = 0;
  if( (m_buf = new char[BUFIO_DEF_SIZ]) )
    m_size = BUFIO_DEF_SIZ;
  else m_size = 0;
  m_len = 0;
  m_max = BUFIO_DEF_SIZ + 4 * BUFIO_INC_SIZ;  // small default
}

inline ssize_t BufIo::_realloc_buffer()
{
  return SetSize(m_size + BUFIO_INC_SIZ);
}

void BufIo::MaxSize(size_t len)
{
  m_max = len;
  if( m_size > m_max ) SetSize(m_max);
}

ssize_t BufIo::SetSize(size_t len)
{
  char *tbuf;

  if( len > m_max && len > m_size ){  // buffer too long
#ifdef EMSGSIZE
    errno = EMSGSIZE;
#elif defined(ENOBUFS)
    errno = ENOBUFS;
#else
    errno = ENOMEM;
#endif
    return -1;
  }

  if( m_len > len ) len = m_len;
  if( len == m_size ) return 0;

  tbuf = new char[len];
#ifndef WINDOWS
  if( !tbuf ){
    errno = ENOMEM;
    return -1;
  }
#endif

  if( m_len ) memcpy(tbuf, m_buf, m_len);
  delete []m_buf;
  m_buf = tbuf;
  m_size = len;

  return 0;
}

// Returns the number of bytes sent if successful, -1 otherwise
ssize_t BufIo::_SEND(SOCKET sk, const char *buf, size_t len)
{
  ssize_t r;
  size_t t = 0;
  while( len ){
    r = SEND(sk, buf, len);
    if( r < 0 ){
#ifndef WINDOWS
      if( EINTR == errno ) continue;
#endif
      return (EWOULDBLOCK == errno || EAGAIN == errno) ? (ssize_t)t : -1;
    }else if( 0 == r ){
      return (ssize_t)t;  // not possible?
    }else{
      buf += r;
      len -= r;
      t += r;
    }
  }
  return (ssize_t)t;
}

ssize_t BufIo::_RECV(SOCKET sk, char *buf, size_t len)
{
  ssize_t r;
  size_t t = 0;
  while( len ){
    r = RECV(sk, buf, len);
    if( r < 0 ){
#ifndef WINDOWS
      if( EINTR == errno ) continue;
#endif
      return (EWOULDBLOCK == errno || EAGAIN == errno) ? (ssize_t)t : -1;
    }else if( 0 == r ){
      m_socket_remote_closed = 1;
      return (ssize_t)t;  // connection closed by remote
    }else{
      buf += r;
      len -= r;
      t += r;
    }
  }
  return (ssize_t)t;
}

ssize_t BufIo::Put(SOCKET sk, const char *buf, size_t len)
{
  if( !m_valid ){
#ifdef ENOBUFS
    errno = ENOBUFS;
#else
    errno = EIO;
#endif
    return -1;
  }

  if( LeftSize() < len ){  // not enough space in buffer
    if( m_len && FlushOut(sk) < 0 ) return -1;
    while( LeftSize() < len ){  // still not enough space
      if( _realloc_buffer() < 0 ){
        m_valid = 0;
        return -1;
      }
    }
  }
  memcpy(m_buf + m_len, buf, len);
  m_len += len;
  return 0;
}

ssize_t BufIo::PutFlush(SOCKET sk, const char *buf, size_t len)
{
  if( !m_valid ){
#ifdef ENOBUFS
    errno = ENOBUFS;
#else
    errno = EIO;
#endif
    return -1;
  }

  if( LeftSize() < len ){
    if( m_len && FlushOut(sk) < 0 ) return -1;
    while( LeftSize() < len ){
      if( _realloc_buffer() < 0 ){
        m_valid = 0;
        return -1;
      }
    }
  }
  memcpy(m_buf + m_len, buf, len);
  m_len += len;
  return FlushOut(sk);
}

// Returns <0 on failure, otherwise the number of bytes left in the buffer
ssize_t BufIo::FlushOut(SOCKET sk)
{
  ssize_t r;
  if( !m_len ) return 0;  // no data to send

  r = _SEND(sk, m_buf, m_len);
  if( r < 0 ){
    m_valid = 0;
    return r;
  }else if( r > 0 ){
    m_len -= r;
    if( m_len ) memmove(m_buf, m_buf + r, m_len);
  }
  return (ssize_t)m_len;
}

ssize_t BufIo::FeedIn(SOCKET sk, size_t limit)
{
  ssize_t r;

  if( !m_valid ){
#ifdef ENOBUFS
    errno = ENOBUFS;
#else
    errno = EIO;
#endif
    return -1;
  }

  if( !LeftSize() && _realloc_buffer() < 0 ){
    m_valid = 0;
    return (ssize_t)-1;
  }

  if( 0==limit || limit > LeftSize() )
    limit = LeftSize();
  r = _RECV(sk, m_buf + m_len, limit);
  if( r < 0 ){
    m_valid = 0;
    return -1;
  }else{
    if( r ) m_len += r;
    if( m_socket_remote_closed ){  // connection closed by remote
      errno = 0;
      return -1;
    }
  }
  return (ssize_t)m_len;
}

ssize_t BufIo::PickUp(size_t len)
{
  if( m_len < len ){
    errno = EINVAL;
    return -1;
  }
  m_len -= len;
  if( m_len ) memmove(m_buf, m_buf + len, m_len);
  return 0;
}

