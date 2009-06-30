#ifndef BTREQUEST_H
#define BTREQUEST_H

#include "def.h"

#include <time.h>

#include "bttypes.h"
#include "btcontent.h"
#include "bitfield.h"

struct SLICE{
   bt_index_t index;
   bt_offset_t offset;
   bt_length_t length;
   time_t reqtime;
   bool sent;
   SLICE *next;
};

struct PIECE{
  dt_count_t count;
  SLICE *slices;
  PIECE *next;
};


class RequestQueue
{
 private:
  PIECE *rq_head;
  mutable const SLICE *m_peek;
  SLICE *rq_send;  // next slice to request

  SLICE *FixSend();
  PIECE *FindPiece(bt_index_t idx, PIECE **prev=(PIECE **)0) const;
  SLICE *FindSlice(bt_index_t idx, bt_offset_t off, bt_length_t len,
    SLICE **prev=(SLICE **)0) const;

  bool Append(PIECE *piece);
  dt_count_t NSlices(bt_index_t idx) const;

 public:
  RequestQueue(){ rq_head = (PIECE *)0; m_peek = rq_send = (SLICE *)0; }
  ~RequestQueue(){ Empty(); }

  bt_index_t GetRequestIdx() const {
    return rq_head ? rq_head->slices->index : BTCONTENT.GetNPieces();
  }
  bt_length_t GetRequestLen() const {
    return rq_head ? rq_head->slices->length : 0;
  }
  bool IsValidRequest(bt_index_t idx, bt_offset_t off, bt_length_t len) const;

  bool IsEmpty() const { return rq_head ? false : true; }
  bool HasPiece(bt_index_t idx) const { return FindPiece(idx) ? true : false; }
  bool HasSlice(bt_index_t idx, bt_offset_t off, bt_length_t len) const {
    return FindSlice(idx, off, len) ? true : false;
  }
  dt_count_t Qsize() const;
  dt_count_t Qlen(bt_index_t piece) const;
  bool LastSlice() const { return (rq_head && !rq_head->slices->next); }

  bool Add(bt_index_t idx, bt_offset_t off, bt_length_t len);
  bool Remove(bt_index_t idx, bt_offset_t off, bt_length_t len);
  bool Delete(bt_index_t idx);
  void Empty();
  int Requeue(bt_index_t idx, bt_offset_t off, bt_length_t len);
  void MoveLast(bt_index_t idx, bt_offset_t off, bt_length_t len);

  bool Transfer(RequestQueue &dstq);
  bool Transfer(RequestQueue &dstq, bt_index_t idx);
  bt_index_t Reassign(RequestQueue &dstq, const Bitfield &bf);
  int Copy(RequestQueue &dstq, bt_index_t idx) const;
  void Shuffle(bt_index_t idx);

  time_t GetReqTime(bt_index_t idx, bt_offset_t off, bt_length_t len) const;
  void Sent(time_t timestamp, bt_index_t idx, bt_offset_t off, bt_length_t len);
  void Sent(time_t timestamp, SLICE *slice=(SLICE *)0);
  bool IsSent(bt_index_t idx, bt_offset_t off, bt_length_t len) const {
    const SLICE *slice;
    return (slice = FindSlice(idx, off, len)) ? slice->sent : false;
  }

  bt_index_t FindLastCommonRequest(const Bitfield &proposerbf) const;
  bt_index_t FindCommonRequest(const Bitfield &proposerbf,
    const RequestQueue &proposerq) const;
  Bitfield &QueuedPieces(Bitfield &bf) const;
  dt_count_t CountSlicesBeforePiece(bt_index_t idx) const;

  bool Pop(bt_index_t *pidx, bt_offset_t *poff, bt_length_t *plen);
  bool Peek(bt_index_t *pidx, bt_offset_t *poff=(bt_offset_t *)0,
      bt_length_t *plen=(bt_length_t *)0, bool *psent=(bool *)0) const {
    m_peek = (SLICE *)0;
    return PeekNext(pidx, poff, plen, psent);
  }
  bool PeekPiece(bt_index_t idx, bt_offset_t *poff=(bt_offset_t *)0,
    bt_length_t *plen=(bt_length_t *)0, bool *psent=(bool *)0) const;
  bool PeekNext(bt_index_t *pidx=(bt_index_t *)0,
    bt_offset_t *poff=(bt_offset_t *)0, bt_length_t *plen=(bt_length_t *)0,
    bool *psent=(bool *)0) const;
  bool PeekNextPiece(bt_index_t *pidx=(bt_index_t *)0,
    bt_offset_t *poff=(bt_offset_t *)0, bt_length_t *plen=(bt_length_t *)0)
    const;
  bool PeekSend() const { return rq_send ? true : false; }
  bool PeekSend(bt_index_t *pidx, bt_offset_t *poff=(bt_offset_t *)0,
    bt_length_t *plen=(bt_length_t *)0) const;

  int AddPiece(bt_index_t idx);
};

extern RequestQueue PENDING;


#endif  // BTREQUEST_H

