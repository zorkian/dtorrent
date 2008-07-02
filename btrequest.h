#ifndef SLICE_H
#define SLICE_H

#include "def.h"

#include <sys/types.h>
#include <time.h>

#include "bttypes.h"
#include "btcontent.h"
#include "bitfield.h"

typedef struct _slice{
   bt_index_t index;
   bt_offset_t offset;
   bt_length_t length;
   time_t reqtime;
   struct _slice *next;
}SLICE,*PSLICE;

class RequestQueue
{
 private:
  PSLICE rq_head;
 public:
  PSLICE rq_send;  // next slice to request

  RequestQueue();
  ~RequestQueue();

  void Empty();

  void SetHead(PSLICE ps);
  void SetNextSend(PSLICE ps) { rq_send = ps; }
  PSLICE GetHead() const { return rq_head; }
  PSLICE NextSend() const { return rq_send; }
  bt_index_t GetRequestIdx() const { return rq_head ? rq_head->index :
                                                  BTCONTENT.GetNPieces(); }
  bt_length_t GetRequestLen() const { return rq_head ? rq_head->length : 0; }
  void Release(){ rq_head = rq_send = (PSLICE) 0; }
  int IsValidRequest(bt_index_t idx, bt_offset_t off, bt_length_t len) const;

  void operator=(RequestQueue &rq);
  int Copy(const RequestQueue *prq, bt_index_t idx);
  int CopyShuffle(const RequestQueue *prq, bt_index_t idx);
  dt_count_t Qsize() const;
  dt_count_t Qlen(bt_index_t piece) const;
  int LastSlice() const;

  int IsEmpty() const { return rq_head ? 0 : 1; }

  int Insert(PSLICE ps, bt_index_t idx, bt_offset_t off, bt_length_t len);
  int Add(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int Append(PSLICE ps);
  int Remove(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int Requeue(bt_index_t idx, bt_offset_t off, bt_length_t len);
  void MoveLast(PSLICE ps);
  int HasIdx(bt_index_t idx) const;
  int HasSlice(bt_index_t idx, bt_offset_t off, bt_length_t len) const;
  time_t GetReqTime(bt_index_t idx, bt_offset_t off, bt_length_t len) const;
  void SetReqTime(PSLICE n,time_t t);


  int Pop(bt_index_t *pidx, bt_offset_t *poff, bt_length_t *plen);
  int Peek(bt_index_t *pidx, bt_offset_t *poff, bt_length_t *plen) const;

  int CreateWithIdx(bt_index_t idx);
  dt_count_t NSlices(bt_index_t idx) const;
  bt_length_t Slice_Length(bt_index_t idx, dt_count_t sidx) const;
};

#define PENDING_QUEUE_SIZE 100

class PendingQueue
{
 private:
  PSLICE pending_array[PENDING_QUEUE_SIZE];
  dt_count_t pq_count;
  
 public:
  PendingQueue();
  ~PendingQueue();
  void Empty();
  int Pending(RequestQueue *prq);
  bt_index_t ReAssign(RequestQueue *prq, BitField &bf);
  int Exist(bt_index_t idx) const;
  int HasSlice(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int Delete(bt_index_t idx);
  int DeleteSlice(bt_index_t idx, bt_offset_t off, bt_length_t len);
};

extern PendingQueue PENDINGQUEUE;

#endif
