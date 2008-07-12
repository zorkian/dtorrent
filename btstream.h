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

public:
  BufIo in_buffer;
  BufIo out_buffer;

  btStream(){ sock = sock_was = INVALID_SOCKET; m_oldbytes = 0; }
  ~btStream(){ Close(); }

  SOCKET GetSocket(){ return (INVALID_SOCKET==sock) ? sock_was : sock; }
  void SetSocket(SOCKET sk){ sock = sk; }

  void Close();

  ssize_t PickMessage();
  ssize_t Feed();
  ssize_t Feed(Rate *rate);
  ssize_t Feed(size_t limit, Rate *rate);

  int HaveMessage();
  bt_msg_t PeekMessage();
  int PeekMessage(bt_msg_t m);
  int PeekNextMessage(bt_msg_t m);

  ssize_t Send_Keepalive();
  ssize_t Send_State(bt_msg_t state);
  ssize_t Send_Have(bt_index_t idx);
  ssize_t Send_Piece(bt_index_t idx, bt_offset_t off, char *piece_buf,
    bt_length_t len);
  ssize_t Send_Bitfield(char *bit_buf, size_t len);
  ssize_t Send_Request(bt_index_t idx, bt_offset_t off, bt_length_t len);
  ssize_t Send_Cancel(bt_index_t idx, bt_offset_t off, bt_length_t len);
  ssize_t Send_Buffer(char *buf, bt_length_t len);

  ssize_t Flush();
};

#endif  // BTSTREAM_H

