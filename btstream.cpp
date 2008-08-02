#include "btstream.h"  // def.h

#include <arpa/inet.h>
#include <inttypes.h>
#include <string.h>      // memcpy
#include <sys/types.h>   // shutdown
#include <sys/socket.h>  // shutdown

#include "btconfig.h"
#include "util.h"

bt_int_t get_bt_int(const char *from)
{
  bt_int_t t = 0;
#ifdef HAVE_NTOHL
  if( BT_LEN_INT == 4 && sizeof(bt_int_t) == 4 ){
    memcpy(&t, from, (size_t)BT_LEN_INT);
    return (bt_int_t)ntohl((uint32_t)t);
  }else
#endif
  {
    const char *end = from + BT_LEN_INT;
    do{
      t = (t << 8) | *from;
    }while( from++ < end );
    return t;
  }
}

void put_bt_int(char *to, bt_int_t from)
{
#ifdef HAVE_HTONL
  if( BT_LEN_INT == 4 && sizeof(bt_int_t) == 4 ){
    bt_int_t from32 = (bt_int_t)htonl((uint32_t)from);
    memcpy(to, &from32, (size_t)BT_LEN_INT);
  }else
#endif
  {
    int x = BT_LEN_INT << 3;  // == 8 * BT_LEN_INT
    memset(to, 0, (size_t)BT_LEN_INT);
    while( x -= 8 )
      *to++ = (from >> x) & 0xff;
  }
}

void btStream::Close()
{
  if( INVALID_SOCKET != sock ){
    if( !cfg_child_process )
      shutdown(sock, SHUT_RDWR);
    CLOSE_SOCKET(sock);
    sock_was = sock;
    sock = INVALID_SOCKET;
  }
  in_buffer.Close();
  out_buffer.Close();
}

ssize_t btStream::Send_State(bt_msg_t state)
{
  char msg[BT_LEN_PRE + BT_LEN_MSGID];

  put_bt_msglen(msg, BT_LEN_MSGID);
  msg[BT_LEN_PRE] = (char)state;
  return out_buffer.Put(sock, msg, BT_LEN_PRE + BT_LEN_MSGID);
}

ssize_t btStream::Send_Have(bt_index_t idx)
{
  char msg[BT_LEN_PRE + BT_MSGLEN_HAVE];

  put_bt_msglen(msg, BT_MSGLEN_HAVE);
  msg[BT_LEN_PRE] = (char)BT_MSG_HAVE;
  put_bt_index(msg + BT_LEN_PRE + BT_LEN_MSGID, idx);

  return out_buffer.Put(sock, msg, BT_LEN_PRE + BT_MSGLEN_HAVE);
}

ssize_t btStream::Send_Bitfield(const char *bit_buf, size_t len)
{
  char msg[BT_LEN_PRE + BT_LEN_MSGID];
  ssize_t r;

  put_bt_msglen(msg, BT_LEN_MSGID + len);
  msg[BT_LEN_PRE] = (char)BT_MSG_BITFIELD;
  if( (r = out_buffer.Put(sock, msg, BT_LEN_PRE + BT_LEN_MSGID)) < 0 )
    return r;
  return out_buffer.Put(sock, bit_buf, len);
}

ssize_t btStream::Send_Cancel(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  char msg[BT_LEN_PRE + BT_MSGLEN_CANCEL];

  put_bt_msglen(msg, BT_MSGLEN_CANCEL);
  msg[BT_LEN_PRE] = (char)BT_MSG_CANCEL;
  put_bt_index(msg + BT_LEN_PRE + BT_LEN_MSGID, idx);
  put_bt_offset(msg + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX, off);
  put_bt_length(msg + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX + BT_LEN_OFF, len);
  return out_buffer.Put(sock, msg, BT_LEN_PRE + BT_MSGLEN_CANCEL);
}

ssize_t btStream::Send_Piece(bt_index_t idx, bt_offset_t off,
  const char *piece_buf, bt_length_t len)
{
  char msg[BT_LEN_PRE + BT_MSGLEN_PIECE];
  ssize_t r;

  put_bt_msglen(msg, BT_MSGLEN_PIECE + len);
  msg[BT_LEN_PRE] = (char)BT_MSG_PIECE;
  put_bt_index(msg + BT_LEN_PRE + BT_LEN_MSGID, idx);
  put_bt_offset(msg + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX, off);
  if( (r = out_buffer.Put(sock, msg, BT_LEN_PRE + BT_MSGLEN_PIECE)) < 0 )
    return r;
  return out_buffer.PutFlush(sock, piece_buf, len);
}

ssize_t btStream::Send_Request(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  char msg[BT_LEN_PRE + BT_MSGLEN_REQUEST];

  put_bt_msglen(msg, BT_MSGLEN_REQUEST);
  msg[BT_LEN_PRE] = (char)BT_MSG_REQUEST;
  put_bt_index(msg + BT_LEN_PRE + BT_LEN_MSGID, idx);
  put_bt_offset(msg + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX, off);
  put_bt_length(msg + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX + BT_LEN_OFF, len);
  return out_buffer.Put(sock, msg, BT_LEN_PRE + BT_MSGLEN_REQUEST);
}

ssize_t btStream::Send_Keepalive()
{
  return out_buffer.Put(sock, "", BT_LEN_PRE);
}

/* returns:
    1 if buffer contains a complete message
    0 if not
   -1 if garbage in buffer
*/
int btStream::HaveMessage() const
{
  bt_msglen_t msglen;
  if( BT_LEN_PRE <= in_buffer.Count() ){
    msglen = get_bt_msglen(in_buffer.BasePointer());
    if( cfg_max_slice_size + BT_LEN_PRE + BT_MSGLEN_PIECE < msglen )
      return -1;  // message too long
    if( msglen + BT_LEN_PRE <= in_buffer.Count() ) return 1;
  }
  return 0; // no message arrived
}

ssize_t btStream::Feed(size_t limit, Rate *rate)
{
  ssize_t retval;
  double rightnow;

  rightnow = PreciseTime();
  retval = in_buffer.FeedIn(sock, limit);

  if( BT_LEN_PRE + BT_MSGLEN_PIECE < in_buffer.Count() &&
      BT_MSG_PIECE == in_buffer.BasePointer()[BT_LEN_PRE] ){
    bt_msglen_t msglen = get_bt_msglen(in_buffer.BasePointer());
    if( msglen > BT_MSGLEN_PIECE ){
      size_t change;
      if( in_buffer.Count() >= msglen + BT_LEN_PRE ){  // have the whole message
        change = msglen - BT_MSGLEN_PIECE - m_oldbytes;
        m_oldbytes = 0;
      }else{
        size_t nbytes = in_buffer.Count() - BT_LEN_PRE - BT_MSGLEN_PIECE;
        change = nbytes - m_oldbytes;
        m_oldbytes = nbytes;
      }
      rate->RateAdd(change, cfg_max_bandwidth_down, rightnow);
    }
  }
  return retval;
}

ssize_t btStream::PickMessage()
{
  return in_buffer.PickUp(get_bt_msglen(in_buffer.BasePointer()) + BT_LEN_PRE);
}

// Used only for sending peer handshake
ssize_t btStream::Send_Buffer(const char *buf, bt_length_t len)
{
  return out_buffer.PutFlush(sock, buf, len);
}

// Does not distinguish between keepalive and no message.
bt_msg_t btStream::PeekMessage() const
{
  return ( BT_LEN_PRE < in_buffer.Count() &&
           get_bt_msglen(in_buffer.BasePointer()) ) ?
             (bt_msg_t)in_buffer.BasePointer()[BT_LEN_PRE] : BT_MSG_NONE;
}

// Is the next message known to match m?
int btStream::PeekMessage(bt_msg_t m) const
{
  return ( BT_LEN_PRE < in_buffer.Count() &&
           m == in_buffer.BasePointer()[BT_LEN_PRE] &&
           get_bt_msglen(in_buffer.BasePointer()) ) ? 1 : 0;
}

// Is the next next message known to match m?
int btStream::PeekNextMessage(bt_msg_t m) const
{
  const char *base;

  base = in_buffer.BasePointer() + BT_LEN_PRE +
         get_bt_msglen(in_buffer.BasePointer());
  return ( BT_LEN_PRE < in_buffer.Count() - (base - in_buffer.BasePointer()) &&
           m == base[BT_LEN_PRE] && get_bt_msglen(base) ) ? 1 : 0;
}

