#include "btrequest.h"  // def.h

#include <stdlib.h>

#include "btcontent.h"
#include "btconfig.h"
#include "console.h"
#include "util.h"


RequestQueue PENDING;


static void _empty_slice_list(PIECE **queue)
{
  PIECE *piece;
  SLICE *slice;

  while( (piece = *queue) ){
    while( (slice = (*queue)->slices) ){
      (*queue)->slices = slice->next;
      delete slice;
    }
    *queue = piece->next;
    delete piece;
  }
}


void RequestQueue::Sent(time_t timestamp, bt_index_t idx, bt_offset_t off,
  bt_length_t len)
{
  SLICE *slice;
  if( (slice = FindSlice(idx, off, len)) )
    Sent(timestamp, slice);
}


void RequestQueue::Sent(time_t timestamp, SLICE *slice)
{
  if( !slice && !(slice = rq_send) && !(slice = FixSend()) ){
    CONSOLE.Warning(1,
      "RequestQueue error:  Sent() called but nothing to send");
    return;
  }

  slice->reqtime = timestamp;
  slice->sent = true;

  if( rq_send == slice ){
    rq_send = slice->next;
    if( !rq_send ){
      const PIECE *piece = FindPiece(slice->index);
      if( piece->next ) rq_send = piece->next->slices;
    }
  }
}


// This function is a failsafe.
SLICE *RequestQueue::FixSend()
{
  PIECE *piece;
  SLICE *original = rq_send;
  bool report = true;

  if( !rq_head ){
    rq_send = (SLICE *)0;
    return rq_send;
  }

  if( !rq_send || !(piece = FindPiece(rq_send->index)) ){
    if( rq_send ){
      CONSOLE.Debug("%p rq_send invalid:  %d/%d/%d", this,
        (int)rq_send->index, (int)rq_send->offset, (int)rq_send->length);
    }
    rq_send = rq_head->slices;
    report = false;
  }
  while( rq_send && rq_send->sent ){  // rq_send is wrong, fix it
    if( report ){
      CONSOLE.Debug("%p rq_send wrong:  %d/%d/%d", this,
        (int)rq_send->index, (int)rq_send->offset, (int)rq_send->length);
      report = false;
    }
    if( rq_send->next ) rq_send = rq_send->next;
    else{
      piece = FindPiece(rq_send->index);
      rq_send = (piece && piece->next) ? piece->next->slices : (SLICE *)0;
    }
  }

  if( rq_send != original ){
    if( !original ) CONSOLE.Debug("%p rq_send was null", this);
    if( !rq_send ) CONSOLE.Debug("%p rq_send corrected to null", this);
    else{
      CONSOLE.Debug("%p rq_send corrected:  %d/%d/%d", this,
        (int)rq_send->index, (int)rq_send->offset, (int)rq_send->length);
    }
  }
  return rq_send;
}


void RequestQueue::Empty()
{
  if( rq_head ) _empty_slice_list(&rq_head);
  rq_send = (SLICE *)0;
}


// Note that -1 indicates a failure to copy/add, not in the source (this).
int RequestQueue::Copy(RequestQueue &dstq, bt_index_t idx) const
{
  const PIECE *piece;
  const SLICE *slice;

  if( dstq.FindPiece(idx) ) return 0;

  if( !(piece = FindPiece(idx)) ) return 0;
  for( slice = piece->slices; slice; slice = slice->next ){
    if( !dstq.Add(slice->index, slice->offset, slice->length) ){
      dstq.Delete(idx);
      return -1;
    }
  }
  return 1;
}


// Does not reset slice->sent, so must be used *only* after Copy().
void RequestQueue::Shuffle(bt_index_t idx)
{
  PIECE *piece;
  SLICE *slice, *prev, *end = (SLICE *)0, *snext, *temp;
  SLICE start;
  int len, shuffle;
  bt_offset_t firstoff;

  if( !(piece = FindPiece(idx)) ) return;
  len = piece->count;
  if( len == 1 ) return;
  firstoff = piece->slices->offset;

  start.next = piece->slices;
  shuffle = RandBits(3) + 2;
  if( shuffle > len/2 ) shuffle = len/2;
  for( ; shuffle; shuffle-- ){
    // Undock the list, then randomly re-insert each slice.
    prev = &start;             // the slice before the last insertion
    slice = start.next->next;  // first one is a no-op
    end = start.next;
    start.next->next = (SLICE *)0;
    for( ; slice; slice = snext ){
      snext = slice->next;
      if( RandBits(1) ){  // beginning or end of list
        if( RandBits(1) ){
          prev = end;
          slice->next = (SLICE *)0;
          end->next = slice;
          end = slice;
        }else{
          slice->next = start.next;
          start.next = slice;
          prev = &start;
        }
      }else{  // before or after previous insertion
        if( RandBits(1) ){  // put after prev->next
          if( end == prev->next ) end = slice;
          temp = prev->next;
          slice->next = prev->next->next;
          prev->next->next = slice;
          prev = temp;
        }else{  // put after prev (before prev->next)
          slice->next = prev->next;
          prev->next = slice;
        }
      }
    }
  }  // shuffle loop
  piece->slices = start.next;

  // If first slice is the same as in the original, move it to the end.
  if( piece->slices->offset == firstoff ){
    end->next = piece->slices;
    piece->slices = piece->slices->next;
    end = end->next;
    end->next = (SLICE *)0;
  }
  if( rq_send->index == piece->slices->index ) rq_send = piece->slices;
}


// Counts all queued slices.
dt_count_t RequestQueue::Qsize() const
{
  dt_count_t total = 0;
  const PIECE *piece;

  for( piece = rq_head; piece; piece = piece->next ){
    total += piece->count;
  }
  return total;
}


// Counts only slices from one piece.
dt_count_t RequestQueue::Qlen(bt_index_t idx) const
{
  dt_count_t total = 0;
  const PIECE *piece;

  if( (piece = FindPiece(idx)) ){
    total = piece->count;
  }
  return total;
}


bool RequestQueue::Add(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  PIECE *piece, *last;
  SLICE *slice, *point = (SLICE *)0;

  piece = FindPiece(idx, &last);

  if( !(slice = new SLICE) ){
    errno = ENOMEM;
    return false;
  }
  if( piece ){
    for( point = piece->slices; point && point->next; point = point->next );
  }else{
    if( !(piece = new PIECE) ){
      delete slice;
      errno = ENOMEM;
      return false;
     }
    piece->count = 0;
    piece->slices = (SLICE *)0;
    piece->next = (PIECE *)0;
    if( last ) last->next = piece;
    else rq_head = piece;
  }

  slice->next = (SLICE *)0;
  slice->index = idx;
  slice->offset = off;
  slice->length = len;
  slice->reqtime = (time_t) 0;
  slice->sent = false;

  if( point ) point->next = slice;
  else piece->slices = slice;
  piece->count++;

  if( !rq_send && !piece->next ) rq_send = slice;
  return true;
}


// Returns true if all pieces were moved.
bool RequestQueue::Transfer(RequestQueue &dstq)
{
  bool result = true;

  while( rq_head ){
    if( !Transfer(dstq, rq_head->slices->index) ) result = false;
  }
  rq_send = (SLICE *)0;
  return result;
}


// Returns true if the piece was moved successfully.
bool RequestQueue::Transfer(RequestQueue &dstq, bt_index_t idx)
{
  bool result = true;
  PIECE *piece, *prev;

  if( !(piece = FindPiece(idx, &prev)) ) return true;

  if( rq_send && rq_send->index == idx )
    rq_send = piece->next ? piece->next->slices : (SLICE *)0;

  if( prev ) prev->next = piece->next;
  else rq_head = piece->next;
  piece->next = (PIECE *)0;

  if( (&PENDING == &dstq && piece->count >= NSlices(piece->slices->index)) ||
      !dstq.Append(piece) ){
    _empty_slice_list(&piece);
    result = false;
  }
  return result;
}


bool RequestQueue::Append(PIECE *piece)
{
  PIECE *last;
  SLICE *slice;

  if( FindPiece(piece->slices->index, &last) ) return false;

  if( last ) last->next = piece;
  else rq_head = piece;

  for( slice = piece->slices; slice && slice->sent; slice = slice->next ){
    slice->sent = false;
  }
  if( !rq_send ) rq_send = piece->slices;
  return true;
}


bt_index_t RequestQueue::Reassign(RequestQueue &dstq, const Bitfield &bf)
{
  PIECE *piece, *prev = (PIECE *)0;
  bt_index_t idx = BTCONTENT.GetNPieces();

  for( piece = rq_head; piece; piece = prev ? prev->next : rq_head ){
    if( bf.IsSet(piece->slices->index) &&
        !dstq.HasPiece(piece->slices->index) ){
      idx = piece->slices->index;
      if( rq_send && rq_send->index == idx )
        rq_send = piece->next ? piece->next->slices : (SLICE *)0;

      if( prev ) prev->next = piece->next;
      else rq_head = piece->next;
      piece->next = (PIECE *)0;

      if( !dstq.Append(piece) ) _empty_slice_list(&piece);
      else break;
    }else prev = piece;
  }
  return idx;
}


bool RequestQueue::Remove(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  PIECE *piece, *prevpiece = (PIECE *)0;
  SLICE *slice, *prev = (SLICE *)0;

  if( !(piece = FindPiece(idx, &prevpiece)) ) return false;
  if( !(slice = FindSlice(idx, off, len, &prev)) ) return false;

  if( prev ) prev->next = slice->next;
  else piece->slices = slice->next;
  if( rq_send == slice ){
    rq_send = slice->next;
    if( !rq_send && piece->next ) rq_send = piece->next->slices;
  }
  delete slice;
  piece->count--;

  if( !piece->slices ){
    if( prevpiece ) prevpiece->next = piece->next;
    else rq_head = piece->next;
    delete piece;
  }
  return true;
}


bool RequestQueue::Delete(bt_index_t idx)
{
  PIECE *piece, *prev;

  if( !(piece = FindPiece(idx, &prev)) ) return false;
  if( prev ) prev->next = piece->next;
  else rq_head = piece->next;

  if( rq_send && rq_send->index == idx )
    rq_send = piece->next ? piece->next->slices : (SLICE *)0;

  piece->next = (PIECE *)0;
  _empty_slice_list(&piece);
  return true;
}


/* Add a slice at an appropriate place in the queue.
   returns -1 if failed, 1 if request needs to be sent. */
int RequestQueue::Requeue(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  int result;
  bool send = true;
  const PIECE *piece;
  SLICE *save_send = rq_send;

  for( piece = rq_head; piece; piece = piece->next ){
    if( rq_send && rq_send->index == piece->slices->index ) send = false;
    if( piece->slices->index == idx ) break;
  }

  result = Add(idx, off, len) ? (send ? 1 : 0) : -1;
  rq_send = save_send;
  return result;
}


// Move the slice to the end of its piece sequence.
void RequestQueue::MoveLast(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  PIECE *piece;
  SLICE *slice, *point;

  if( !(piece = FindPiece(idx)) ) return;
  slice = FindSlice(idx, off, len, &point);
  if( !slice || !slice->next ) return;

  if( rq_send == slice ) rq_send = slice->next;
  if( point ) point->next = slice->next;
  else piece->slices = slice->next;
  slice->sent = false;

  for( point = slice->next; point->next; point = point->next );
  point->next = slice;
  slice->next = (SLICE *)0;
}


// If prev is given/non-null, it will point to the piece preceding the target.
PIECE *RequestQueue::FindPiece(bt_index_t idx, PIECE **prev) const
{
  PIECE *piece;

  if( prev ) *prev = (PIECE *)0;
  for( piece = rq_head;
       piece && piece->slices->index != idx;
       piece = piece->next ){
    if( prev ) *prev = piece;
  }
  return piece;
}


// If prev is given/non-null, it will point to the slice preceding the target.
SLICE *RequestQueue::FindSlice(bt_index_t idx, bt_offset_t off,
  bt_length_t len, SLICE **prev) const
{
  PIECE *piece;
  SLICE *slice = (SLICE *)0;

  if( prev ) *prev = (SLICE *)0;
  if( (piece = FindPiece(idx)) ){
    for( slice = piece->slices; slice; slice = slice->next ){
      if( slice->index == idx && slice->offset == off && slice->length == len )
        break;
      if( prev ) *prev = slice;
    }
  }
  return slice;
}


bt_index_t RequestQueue::FindCommonRequest(const Bitfield &proposerbf,
  const RequestQueue &proposerq) const
{
  const PIECE *piece;
  bt_index_t idx = BTCONTENT.GetNPieces();

  for( piece = rq_head; piece; piece = piece->next ){
    if( proposerbf.IsSet(piece->slices->index) &&
        !proposerq.HasPiece(piece->slices->index) ){
      idx = piece->slices->index;
      break;
    }
  }
  return idx;
}


bt_index_t RequestQueue::FindLastCommonRequest(const Bitfield &proposerbf) const
{
  const PIECE *piece;
  bt_index_t idx = BTCONTENT.GetNPieces();

  for( piece = rq_head; piece; piece = piece->next ){
    if( proposerbf.IsSet(piece->slices->index) )
      idx = piece->slices->index;
  }
  return idx;
}


Bitfield &RequestQueue::QueuedPieces(Bitfield &bf) const
{
  const PIECE *piece;

  bf.Clear();
  for( piece = rq_head; piece; piece = piece->next ){
    bf.Set(piece->slices->index);
  }
  return bf;
}


dt_count_t RequestQueue::CountSlicesBeforePiece(bt_index_t idx) const
{
  const PIECE *piece;
  dt_count_t total = 0;

  for( piece = rq_head; piece; piece = piece->next ){
    if( idx == piece->slices->index ) break;
    total += piece->count;
  }
  return total;
}


time_t RequestQueue::GetReqTime(bt_index_t idx, bt_offset_t off,
  bt_length_t len) const
{
  const SLICE *slice;

  slice = FindSlice(idx, off, len);
  return slice ? slice->reqtime : (time_t)0;
}


bool RequestQueue::Pop(bt_index_t *pidx, bt_offset_t *poff, bt_length_t *plen)
{
  PIECE *piece;
  SLICE *slice;

  if( !(piece = rq_head) ) return false;
  slice = piece->slices;
  if( pidx ) *pidx = slice->index;
  if( poff ) *poff = slice->offset;
  if( plen ) *plen = slice->length;

  if( rq_send == slice ){
    rq_send = slice->next;
    if( !rq_send && piece->next ) rq_send = piece->next->slices;
  }

  piece->slices = slice->next;
  delete slice;
  piece->count--;

  if( !piece->slices ){
    rq_head = piece->next;
    delete piece;
  }

  return true;
}


bool RequestQueue::PeekPiece(bt_index_t idx, bt_offset_t *poff,
  bt_length_t *plen, bool *psent) const
{
  const PIECE *piece;

  if( !(piece = FindPiece(idx)) ) return false;
  m_peek = piece->slices;
  if( poff ) *poff = m_peek->offset;
  if( plen ) *plen = m_peek->length;
  if( psent ) *psent = m_peek->sent;
  return true;
}


/* Peek() or PeekPiece() must be called first, to insure m_peek is initialized
   to a valid state.  The queue must not be changed between calls to
   PeekNext(). */
bool RequestQueue::PeekNext(bt_index_t *pidx, bt_offset_t *poff,
  bt_length_t *plen, bool *psent) const
{
  const SLICE *slice = (SLICE *)0;

  if( m_peek ){
    slice = m_peek->next;
    if( !slice ){
      const PIECE *piece;
      if( (piece = FindPiece(m_peek->index)) ){
        piece = piece->next;
        if( piece ) slice = piece->slices;
      }
    }
  }else if( rq_head ) slice = rq_head->slices;
  if( slice ){
    if( pidx ) *pidx = slice->index;
    if( poff ) *poff = slice->offset;
    if( plen ) *plen = slice->length;
    if( psent ) *psent = slice->sent;
  }
  m_peek = slice;
  return slice ? true : false;
}


bool RequestQueue::PeekNextPiece(bt_index_t *pidx, bt_offset_t *poff,
  bt_length_t *plen) const
{
  const PIECE *piece;

  if( !m_peek ) return PeekNext(pidx, poff, plen);

  if( !(piece = FindPiece(m_peek->index)) ) return false;
  piece = piece->next;
  m_peek = piece ? piece->slices : (SLICE *)0;
  if( m_peek ){
    if( pidx ) *pidx = m_peek->index;
    if( poff ) *poff = m_peek->offset;
    if( plen ) *plen = m_peek->length;
  }
  return m_peek ? true : false;
}


bool RequestQueue::PeekSend(bt_index_t *pidx, bt_offset_t *poff,
  bt_length_t *plen) const
{
  /* FixSend() could be called (if rq_send is mutable and FixSend is const)
     as a failsafe, though it is a bit expensive to do so unconditionally. */

  if( !rq_send ) return false;

  if( pidx ) *pidx = rq_send->index;
  if( poff ) *poff = rq_send->offset;
  if( plen ) *plen = rq_send->length;
  return true;
}


int RequestQueue::AddPiece(bt_index_t idx)
{
  bt_offset_t off = 0;
  bt_length_t len, remain;

  for( remain = BTCONTENT.GetPieceLength(idx); remain; remain -= len ){
    len = (remain < *cfg_req_slice_size) ? remain : *cfg_req_slice_size;
    if( !Add(idx, off, len) ) return -1;
    off += len;
  }
  return 0;
}


dt_count_t RequestQueue::NSlices(bt_index_t idx) const
{
  bt_length_t len;
  dt_count_t n;

  len = BTCONTENT.GetPieceLength(idx);
  n = len / *cfg_req_slice_size;
  return ( len % *cfg_req_slice_size ) ? n + 1 : n;
}


bool RequestQueue::IsValidRequest(bt_index_t idx, bt_offset_t off,
  bt_length_t len) const
{
  return ( idx < BTCONTENT.GetNPieces() &&
           len > 0 &&
           off + len <= BTCONTENT.GetPieceLength(idx) &&
           len <= MAX_SLICE_SIZE );
}

