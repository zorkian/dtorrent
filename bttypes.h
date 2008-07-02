#ifndef BTTYPES_H
#define BTTYPES_H

#include "config.h"
#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

typedef uint32_t bt_int_t;     // a protocol integer
typedef bt_int_t bt_msglen_t;  // protocol message length
typedef bt_int_t bt_index_t;   // piece index
typedef bt_int_t bt_offset_t;  // piece offset
typedef bt_int_t bt_length_t;  // piece length
enum bt_msg_t {                // protocol message IDs
  BT_MSG_NONE           = -1,
  BT_MSG_CHOKE          = 0,
  BT_MSG_UNCHOKE        = 1,
  BT_MSG_INTERESTED     = 2,
  BT_MSG_NOT_INTERESTED = 3,
  BT_MSG_HAVE           = 4,
  BT_MSG_BITFIELD       = 5,
  BT_MSG_REQUEST        = 6,
  BT_MSG_PIECE          = 7,
  BT_MSG_CANCEL         = 8,
  BT_MSG_PORT           = 9
};

typedef uint64_t dt_datalen_t;  // data amount (file length, UL/DL total, etc.)
typedef uint32_t dt_rate_t;     // transfer rate
typedef uint32_t dt_count_t;    // count of things
typedef uint32_t dt_mem_t;      // quantity of memory

// Protocol message component sizes, in bytes
#define BT_LEN_INT      4             // integer
#define BT_LEN_PRE      BT_LEN_INT    // msg length prefix
#define BT_LEN_MSGID    1             // msg ID
#define BT_LEN_IDX      BT_LEN_INT    // piece index
#define BT_LEN_OFF      BT_LEN_INT    // piece offset
#define BT_LEN_LEN      BT_LEN_INT    // piece length
#define BT_LEN_PORT     2

// Message sizes, without length prefix or variable payload
#define BT_MSGLEN_HAVE     (BT_LEN_MSGID + BT_LEN_IDX)
#define BT_MSGLEN_REQUEST  (BT_LEN_MSGID + BT_LEN_IDX + BT_LEN_OFF + BT_LEN_LEN)
#define BT_MSGLEN_PIECE    (BT_LEN_MSGID + BT_LEN_IDX + BT_LEN_OFF)
#define BT_MSGLEN_CANCEL   (BT_LEN_MSGID + BT_LEN_IDX + BT_LEN_OFF + BT_LEN_LEN)
#define BT_MSGLEN_PORT     (BT_LEN_MSGID + BT_LEN_PORT)

#endif

