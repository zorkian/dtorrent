#ifndef BTSTREAM_H
#define BTSTREAM_H

#include "def.h"
#include "bttypes.h"
#include "bufio.h"

#ifdef WINDOWS
#include <Winsock2.h>
#else
#include <unistd.h>
#endif

#include "rate.h"

bt_int_t get_bt_int(const char *from);
// The underlying type and length of each of these are all the same.
inline bt_msglen_t get_bt_msglen(const char *from){
  return (bt_msglen_t)get_bt_int(from);
}
inline bt_index_t get_bt_index(const char *from){
  return (bt_index_t)get_bt_int(from);
}
inline bt_offset_t get_bt_offset(const char *from){
  return (bt_offset_t)get_bt_int(from);
}
inline bt_length_t get_bt_length(const char *from){
  return (bt_length_t)get_bt_int(from);
}

void put_bt_int(char *to, bt_int_t from);
// The underlying type and length of each of these are all the same.
inline void put_bt_msglen(char *to, bt_msglen_t from){
  put_bt_int(to, (bt_int_t)from);
}
inline void put_bt_index(char *to, bt_index_t from){
  put_bt_int(to, (bt_int_t)from);
}
inline void put_bt_offset(char *to, bt_offset_t from){
  put_bt_int(to, (bt_int_t)from);
}
inline void put_bt_length(char *to, bt_length_t from){
  put_bt_int(to, (bt_int_t)from);
}


class btStream
{
private:
  SOCKET sock, sock_was;
  size_t m_oldbytes;
  bt_msglen_t m_msglen;

public:
  BufIo in_buffer;
  BufIo out_buffer;

  btStream(){ sock = sock_was = INVALID_SOCKET; m_oldbytes = 0; m_msglen = 0; }
  ~btStream(){ Close(); }

  SOCKET GetSocket() const { return (INVALID_SOCKET==sock) ? sock_was : sock; }
  void SetSocket(SOCKET sk){ sock = sk; }

  void Close();

  ssize_t PickMessage();
  ssize_t Feed() { return in_buffer.FeedIn(sock); }
  ssize_t Feed(Rate *rate) { return Feed(0, rate); }
  ssize_t Feed(size_t limit, Rate *rate);

  int HaveMessage() const;
  bt_msg_t PeekMessage() const;
  int PeekMessage(bt_msg_t m) const;
  int PeekNextMessage(bt_msg_t m) const;

  ssize_t Send_Keepalive();
  ssize_t Send_State(bt_msg_t state);
  ssize_t Send_Have(bt_index_t idx);
  ssize_t Send_Piece(bt_index_t idx, bt_offset_t off, const char *piece_buf,
    bt_length_t len);
  ssize_t Send_Bitfield(const char *bit_buf, size_t len);
  ssize_t Send_Request(bt_index_t idx, bt_offset_t off, bt_length_t len);
  ssize_t Send_Cancel(bt_index_t idx, bt_offset_t off, bt_length_t len);
  ssize_t Send_Buffer(const char *buf, bt_length_t len);

  ssize_t Flush() { return out_buffer.FlushOut(sock); }
};

#endif  // BTSTREAM_H

