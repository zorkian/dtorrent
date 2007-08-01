#include "peer.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ctorrent.h"
#include "btstream.h"
#include "./btcontent.h"
#include "./msgencode.h"
#include "./peerlist.h"
#include "./btconfig.h"
#include "bttime.h"

size_t get_nl(char *sfrom)
{
  unsigned char *from = (unsigned char *)sfrom;
  size_t t;
  t = (*from++) << 24;
  t |= (*from++) << 16;
  t |= (*from++) << 8;
  t |= *from;
  return t;
}

void set_nl(char *sto, size_t from)
{
  unsigned char *to = (unsigned char *)sto;
  *to++ = (from >> 24) & 0xff;
  *to++ = (from >> 16) & 0xff;
  *to++ = (from >> 8) & 0xff;
  *to = from & 0xff;
}

// Convert a peer ID to a printable string.
int TextPeerID(unsigned char *peerid, char *txtid)
{
  int i, j;

  for(i=j=0; i < PEER_ID_LEN; i++){
    if( i==j && isprint(peerid[i]) && !isspace(peerid[i]) )
      txtid[j++] = peerid[i];
    else{
      if(i==j){ sprintf(txtid+j, "0x"); j+=2; }
      snprintf(txtid+j, 3, "%.2X", (int)(peerid[i]));
      j += 2;
    }
  }
  txtid[j] = '\0';

  return 0;
}


/* g_next_up is used to rotate uploading.  If we have the opportunity to
   upload to a peer but skip it due to bw limiting, the var is set to point to
   that peer and it will be given priority at the next opportunity.
   g_next_dn is similar, but for downloading.
   g_defer_up/dn is used to let the g_next peer object know if it skipped.
*/
btPeer *g_next_up = (btPeer *)0;
btPeer *g_next_dn = (btPeer *)0;
unsigned char g_defer_up = 0;
unsigned char g_defer_dn = 0;
btBasic Self;

void btBasic::SetCurrentRates()
{
  m_current_dl = rate_dl.RateMeasure();
  m_current_ul = rate_ul.RateMeasure();
  m_use_current = 1;
}

void btBasic::SetIp(struct sockaddr_in addr)
{
  memcpy(&m_sin.sin_addr,&addr.sin_addr,sizeof(struct in_addr));
}

void btBasic::SetAddress(struct sockaddr_in addr)
{
  memcpy(&m_sin,&addr,sizeof(struct sockaddr_in));
}

int btBasic::IpEquiv(struct sockaddr_in addr)
{
//	fprintf(stdout,"IpEquiv: %s <=> ", inet_ntoa(m_sin.sin_addr));
//	fprintf(stdout,"%s\n", inet_ntoa(addr.sin_addr));
        return (memcmp(&m_sin.sin_addr,&addr.sin_addr,sizeof(struct in_addr)) == 0) ? 
    1 : 0;
}

int btPeer::Need_Local_Data()
{
  if( m_state.remote_interested && !bitfield.IsFull()){

    if( BTCONTENT.pBF->IsFull() ) return 1; // i am seed

    BitField tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(bitfield);
    return tmpBitfield.IsEmpty() ? 0 : 1;

  }
  return 0;
}

int btPeer::Need_Remote_Data()
{

  if( BTCONTENT.pBF->IsFull()) return 0;
  else if( bitfield.IsFull() ) return 1;
  else{
    BitField tmpBitfield = bitfield;
    tmpBitfield.Except(*BTCONTENT.pBF);
    tmpBitfield.Except(*BTCONTENT.pBFilter);
    return tmpBitfield.IsEmpty() ? 0 : 1;
  }
  return 0;
}

btPeer::btPeer()
{
  m_f_keepalive = 0;
  m_status = P_CONNECTING;
  m_unchoke_timestamp = (time_t) 0;
  m_last_timestamp = now;
  m_state.remote_choked = m_state.local_choked = 1;
  m_state.remote_interested = m_state.local_interested = 0;

  m_err_count = 0;
  m_cached_idx = BTCONTENT.GetNPieces();
  m_standby = 0;
  m_req_send = 5;
  m_req_out = 0;
  m_latency = 0;
  m_prev_dlrate = 0;
  m_health_time = m_receive_time = m_choketime = m_last_timestamp;
  m_bad_health = 0;
  m_want_again = m_connect = 0;
  m_connect_seed = 0;
}

int btPeer::SetLocal(unsigned char s)
{
  switch(s){
  case M_CHOKE:
    if( m_state.local_choked ) return 0;
    m_unchoke_timestamp = now;
//  if(arg_verbose) fprintf(stderr, "Choking %p\n", this);
    if(arg_verbose) fprintf(stderr, "Choking %p (D=%lluMB@%uK/s)\n", this,
      TotalDL() >> 20, RateDL() >> 10);
    m_state.local_choked = 1; 
    if( g_next_up == this ) g_next_up = (btPeer *)0;
    if( !reponse_q.IsEmpty()) reponse_q.Empty();
    break;
  case M_UNCHOKE: 
    if( !reponse_q.IsEmpty() ) StartULTimer();
    if( !m_state.local_choked ) return 0;
    m_unchoke_timestamp = now;
//  if(arg_verbose) fprintf(stderr, "Unchoking %p\n", this);
    if(arg_verbose) fprintf(stderr, "Unchoking %p (D=%lluMB@%uK/s)\n", this,
      TotalDL() >> 20, RateDL() >> 10);
    m_state.local_choked = 0;
    break;
  case M_INTERESTED: 
    if( WORLD.SeedOnly() ) return 0;
    m_standby = 0;
    if( m_state.local_interested ) return 0;
    if(arg_verbose) fprintf(stderr, "Interested in %p\n", this);
    m_state.local_interested = 1;
    break;
  case M_NOT_INTERESTED:
    if( !m_state.local_interested ) return 0;
    if(arg_verbose) fprintf(stderr, "Not interested in %p\n", this);
    m_state.local_interested = 0; 
    if( !request_q.IsEmpty() ){
      CancelRequest(request_q.GetHead());
      request_q.Empty();
    }
    break;
  default:
    return -1;			// BUG ???
  }
  return stream.Send_State(s);
}

int btPeer::RequestPiece()
{
  size_t idx;
  int endgame = 0;

  size_t qsize = request_q.Qsize();
  size_t psize = BTCONTENT.GetPieceLength() / cfg_req_slice_size;

  // See if there's room in the queue for a new piece.
  // Also, don't queue another piece if we still have a full piece queued.
  if( cfg_req_queue_length - qsize < psize || qsize >= psize ){
    m_req_send = m_req_out;   // don't come back until you receive something.
    return 0;
  }

  if( PENDINGQUEUE.ReAssign(&request_q,bitfield) ){
    if(arg_verbose) fprintf(stderr, "Assigning to %p from Pending\n", this);
    return SendRequest();
  }

  if( m_cached_idx < BTCONTENT.GetNPieces() && !BTCONTENT.pBF->IsEmpty() ){
    // A HAVE msg already selected what we want from this peer
    // but ignore it in initial-piece mode.
    idx = m_cached_idx;
    m_cached_idx = BTCONTENT.GetNPieces();
    if( !BTCONTENT.pBF->IsSet(idx) &&
        !PENDINGQUEUE.Exist(idx) &&
        !WORLD.AlreadyRequested(idx) ){
      if(arg_verbose) fprintf(stderr, "Assigning #%u to %p\n", idx, this);
      return (request_q.CreateWithIdx(idx) < 0) ? -1 : SendRequest();
    }
  }	// If we didn't want the cached piece, select another.
  if( BTCONTENT.pBF->IsEmpty() ){
    // If we don't have a complete piece yet, try to get one that's already
    // in progress.  (Initial-piece mode)
    BitField tmpBitField = bitfield;
    idx = WORLD.What_Can_Duplicate(tmpBitField, this, BTCONTENT.GetNPieces());
    if( idx < BTCONTENT.GetNPieces() ){
      if(arg_verbose) fprintf(stderr, "Want to dup #%u to %p\n", idx, this);
      btPeer *peer = WORLD.WhoHas(idx);
      if(peer){
        if(arg_verbose) fprintf( stderr, "Duping: %p to %p (#%u)\n",
          peer, this, idx );
        return (request_q.CopyShuffle(&peer->request_q, idx) < 0) ?
          -1 : SendRequest();
      }
    }else if(arg_verbose) fprintf(stderr, "Nothing to dup to %p\n", this);
  }	// Doesn't have a piece that's already in progress--choose another.
    BitField tmpBitField;
    if( bitfield.IsFull() ){
      // peer is a seed
      tmpBitField = *BTCONTENT.pBF;
      tmpBitField.Invert();
    }else{
      tmpBitField = bitfield;
      tmpBitField.Except(*BTCONTENT.pBF);
    }
    // The filter tells what we don't want.
    tmpBitField.Except(*BTCONTENT.pBFilter);
    // tmpBitField tells what we need from this peer...

    if( !tmpBitField.IsEmpty() ){
      BitField tmpBitField2 = tmpBitField;
      WORLD.CheckBitField(tmpBitField2);
      // [tmpBitField2]... that we haven't requested from anyone.
      if(tmpBitField2.IsEmpty()){
        // Everything this peer has that I want, I've already requested.
        if( arg_file_to_download ){
          BitField afdBitField =  *BTCONTENT.pBF;
          afdBitField.Except(*BTCONTENT.pBFilter);
          endgame = ( BTCONTENT.getFilePieces(arg_file_to_download)
                      - afdBitField.Count() ) < WORLD.TotalPeers();
        }else
          endgame = ( WORLD.Pieces_I_Can_Get() - BTCONTENT.pBF->Count() )
                      < WORLD.TotalPeers();
        if(endgame){	// OK to duplicate a request.
//        idx = tmpBitField.Random();
          idx = 0;	// flag for Who_Can_Duplicate()
          BitField tmpBitField3 = tmpBitField2;
          idx = WORLD.What_Can_Duplicate(tmpBitField3, this, idx);
          if( idx < BTCONTENT.GetNPieces() ){
            if(arg_verbose) fprintf(stderr,"Want to dup #%u to %p\n",idx,this);
            btPeer *peer = WORLD.WhoHas(idx);
            if(peer){
              if(arg_verbose) fprintf( stderr, "Duping: %p to %p (#%u)\n",
                peer, this, idx );
              return (request_q.CopyShuffle(&peer->request_q, idx) < 0) ?
                -1 : SendRequest();
            }
          }else if(arg_verbose) fprintf(stderr, "Nothing to dup to %p\n",this);
        }else{	// not endgame mode
          btPeer *peer = WORLD.Who_Can_Abandon(this); // slowest choice
          if(peer){
            // Cancel a request to the slowest peer & request it from this one.
            if(arg_verbose) fprintf( stderr, "Reassigning %p to %p (#%u)\n",
              peer, this, peer->request_q.GetRequestIdx() );
            // RequestQueue class "moves" rather than "copies" in assignment!
            if( request_q.Copy(&peer->request_q) < 0 ) return -1;
            if(peer->CancelPiece() < 0 || peer->RequestCheck() < 0)
              peer->CloseConnection();
            return SendRequest();
          }else{
            if(arg_verbose) fprintf(stderr, "%p standby\n", this);
            m_standby = 1;	// nothing to do at the moment
          }
        }
      }else{
        // Request something that we haven't requested yet (most common case).
        // Try to make it something that has good trade value.
        BitField tmpBitField3 = tmpBitField2;
        WORLD.FindValuedPieces(tmpBitField3, this, BTCONTENT.pBF->IsEmpty());
        if( tmpBitField3.IsEmpty() ) tmpBitField3 = tmpBitField2;
        idx = tmpBitField3.Random();
        if(arg_verbose) fprintf(stderr, "Assigning #%u to %p\n", idx, this);
        return (request_q.CreateWithIdx(idx) < 0) ? -1 : SendRequest();
      }
    }else{
      // We don't need anything from the peer.  How'd we get here?
      return SetLocal(M_NOT_INTERESTED);
    }
  return 0;
}

int btPeer::MsgDeliver()
{
  size_t r,idx,off,len;
  int retval = 0;

  char *msgbuf = stream.in_buffer.BasePointer();

  r = get_nl(msgbuf);

  // Don't require keepalives if we're receiving other messages.
  m_last_timestamp = now;
  if( 0 == r ){
    if( !m_f_keepalive ) if( stream.Send_Keepalive() < 0 ) return -1;
    m_f_keepalive = 0;
    return 0;
  }else{
    switch(msgbuf[4]){
    case M_CHOKE:
      if(H_BASE_LEN != r){ return -1;}
      if(arg_verbose) fprintf(stderr, "%p choked me\n", this);
      if( m_lastmsg == M_UNCHOKE && m_last_timestamp <= m_choketime+1 ){
        m_err_count+=2;
        if(arg_verbose) fprintf(stderr,"err: %p (%d) Choke oscillation\n",
          this, m_err_count);
      }
      m_choketime = m_last_timestamp;
      m_state.remote_choked = 1;
      StopDLTimer();
      if( g_next_dn == this ) g_next_dn = (btPeer *)0;
      if( !request_q.IsEmpty()){
        m_req_out = 0;
        PENDINGQUEUE.Pending(&request_q);
      }
      break;

    case M_UNCHOKE:
      if(H_BASE_LEN != r){return -1;}
      if(arg_verbose) fprintf(stderr, "%p unchoked me\n", this);
      if( m_lastmsg == M_CHOKE && m_last_timestamp <= m_choketime+1 ){
        m_err_count+=2;
        if(arg_verbose) fprintf(stderr,"err: %p (%d) Choke oscillation\n",
          this, m_err_count);
      }
      m_choketime = m_last_timestamp;
      m_state.remote_choked = 0;
      retval = RequestCheck();
      break;

    case M_INTERESTED:
      if(H_BASE_LEN != r){return -1;}
      if(arg_verbose) fprintf(stderr, "%p is interested\n", this);
      m_state.remote_interested = 1;
      break;

    case M_NOT_INTERESTED:
      if(r != H_BASE_LEN){return -1;}
      if(arg_verbose) fprintf(stderr, "%p is not interested\n", this);

      m_state.remote_interested = 0;
      StopULTimer();

      /* remove peer's reponse queue */
      if( !reponse_q.IsEmpty()) reponse_q.Empty();

      /* if I've been seed for a while, nobody should be uninterested */
      if( BTCONTENT.pBF->IsFull() && BTCONTENT.GetSeedTime() - now >= 300 )
         return -2;
      break;

    case M_HAVE:
      if(H_HAVE_LEN != r){return -1;}

      idx = get_nl(msgbuf + 5);

      if( idx >= BTCONTENT.GetNPieces() || bitfield.IsSet(idx)) return -1;

      bitfield.Set(idx);

      if( bitfield.IsFull() && BTCONTENT.pBF->IsFull() ){ return -2; }

      if( !BTCONTENT.pBF->IsSet(idx) && !BTCONTENT.pBFilter->IsSet(idx) ){
        m_cached_idx = idx;
        if(arg_verbose && m_standby) fprintf(stderr, "%p un-standby\n", this);
        m_standby = 0;
      }
      //      if( !BTCONTENT.pBF->IsSet(idx) ) m_cached_idx = idx;
      
      // see if we're Interested now
      if(!m_standby) retval = RequestCheck();
      break;

    case M_REQUEST:
      if(H_REQUEST_LEN != r || !m_state.remote_interested){ return -1; }

      idx = get_nl(msgbuf + 5);
      
      if( !BTCONTENT.pBF->IsSet(idx) ) return -1;
      
      off = get_nl(msgbuf + 9);
      len = get_nl(msgbuf + 13);

      if( !reponse_q.IsValidRequest(idx, off, len) ) return -1;

      if( m_state.local_choked ){
        if( (m_latency && m_last_timestamp - m_unchoke_timestamp > m_latency) ||
            (!m_latency && m_last_timestamp - m_unchoke_timestamp > 60) ){
          m_err_count++;
          if(arg_verbose) fprintf(stderr,"err: %p (%d) choked request\n",
            this, m_err_count);
          if( stream.Send_State(M_CHOKE) < 0 ) return -1;
          // This will mess with the unchoke rotation (to this peer's
          // disadvantage), but otherwise we may spam them with choke msgs.
          m_unchoke_timestamp = m_last_timestamp;
        }
      }else retval = reponse_q.Add(idx, off, len);
      break;

    case M_PIECE:
      m_receive_time = m_last_timestamp;
      if( request_q.IsEmpty() || !m_state.local_interested){
        m_err_count++;
             if(arg_verbose) fprintf(stderr,"err: %p (%d) Unwanted piece\n",
               this, m_err_count);
      }else retval = PieceDeliver(r);
      break;

    case M_BITFIELD:
      if( (r - 1) != bitfield.NBytes() || !bitfield.IsEmpty()) return -1;
      bitfield.SetReferBuffer(msgbuf + 5);
      if(bitfield.IsFull()){
        if(arg_verbose) fprintf(stderr, "%p is a seed\n", this);
        if(BTCONTENT.pBF->IsFull()) return -2;
      }

      //This is needed in order to set our Interested state
      retval = RequestCheck(); // fixed client stall
      break;

    case M_CANCEL:
      if(r != H_CANCEL_LEN || !m_state.remote_interested) return -1;

      idx = get_nl(msgbuf + 5);
      off = get_nl(msgbuf + 9);
      len = get_nl(msgbuf + 13);
      if( reponse_q.Remove(idx,off,len) < 0 ){
        m_err_count++;
        if(arg_verbose) fprintf(stderr, "err: %p (%d) Bad cancel\n",
          this, m_err_count);
      }else{
        if( reponse_q.IsEmpty() ) StopULTimer();
        if( reponse_q.IsEmpty() || !CouldReponseSlice() ){
          if( g_next_up == this ) g_next_up = (btPeer *)0;
        }
      }
      break;

    default:
      if(arg_verbose) fprintf(stderr, "Unknown message type %u from peer %p\n",
        msgbuf[4], this);
    } // switch

    if( retval >= 0 ) m_lastmsg = msgbuf[4];
  }
  return retval;
}

int btPeer::ReponseSlice()
{
  size_t len = 0;

  reponse_q.Peek((size_t*) 0,(size_t*) 0, &len);

  if(len && stream.out_buffer.LeftSize() <= (len + 13 + 3 * 1024))
    stream.Flush();

  if(len && stream.out_buffer.LeftSize() > (len + 13 + 3 * 1024)){
    size_t idx,off;
    reponse_q.Pop(&idx,&off,(size_t *) 0);

    if(BTCONTENT.ReadSlice(BTCONTENT.global_piece_buffer,idx,off,len) != 0 ){
      return -1;
    }

    Self.DataSended(len);
    DataSended(len);
    if(arg_verbose) fprintf(stderr, "Sending %d/%d/%d to %p\n",
      (int)idx, (int)off, (int)len, this);
    return stream.Send_Piece(idx,off,BTCONTENT.global_piece_buffer,len);
  }

  return 0;
}

int btPeer::SendRequest()
{
  int first = 1;
  PSLICE ps = request_q.NextSend();

  if( m_req_out > cfg_req_queue_length ){
    if(arg_verbose)
      fprintf(stderr, "ERROR@5: %p m_req_out underflow, resetting\n", this);
    m_req_out = 0;
  }
  if( ps && m_req_out < m_req_send ){
    if(arg_verbose)
      fprintf(stderr, "Requesting #%u from %p (%d left, %d slots):",
      ps->index, this, request_q.Qsize(), m_req_send);
    for( int i=0; ps && m_req_out < m_req_send && i<5; ps = ps->next, i++ ){
      if( first && (!RateDL() ||
          0 >= (m_req_out+1) * ps->length / (double)RateDL() - m_latency) ){
        request_q.SetReqTime(ps, now);
        first = 0;
      } else request_q.SetReqTime(ps, (time_t)0);
      if(arg_verbose) fprintf(stderr, ".");
      if(stream.Send_Request(ps->index,ps->offset,ps->length) < 0){ return -1; }
      request_q.SetNextSend(ps->next);
      m_req_out++;
    }
    if(arg_verbose) fprintf(stderr, "\n");
    m_receive_time = now;
  }
  return ( m_req_out < m_req_send ) ? RequestPiece() : 0;
}

int btPeer::CancelPiece()
{
  PSLICE ps = request_q.GetHead();
  size_t idx;
  int cancel = 1;
  int retval;

  idx = ps->index;
  for( ; ps; ps = ps->next){
    if( ps->index != idx ) break;
    if( ps == request_q.NextSend() ) cancel = 0;
    if( cancel ){
      if(stream.Send_Cancel(ps->index,ps->offset,ps->length) < 0)
        return -1;
      m_req_out--;
      if( m_req_out > cfg_req_queue_length ){
        if(arg_verbose)
          fprintf(stderr, "ERROR@1: %p m_req_out underflow, resetting\n", this);
        m_req_out = 0;
      }
    }
    request_q.Remove(ps->index, ps->offset, ps->length);
  }
  if( !m_req_out && g_next_dn == this ) g_next_dn = (btPeer *)0;

  return 0;
}

int btPeer::CancelRequest(PSLICE ps)
{
  int retval;

  for( ; ps; ps = ps->next){
    if( ps == request_q.NextSend() ) break;
    if(stream.Send_Cancel(ps->index,ps->offset,ps->length) < 0)
      return -1;
    m_req_out--;
    if( m_req_out > cfg_req_queue_length ){
      if(arg_verbose)
        fprintf(stderr, "ERROR@2: %p m_req_out underflow, resetting\n", this);
      m_req_out = 0;
    }
  }
  if( !m_req_out && g_next_dn == this ) g_next_dn = (btPeer *)0;

  return 0;
}

int btPeer::CancelSliceRequest(size_t idx, size_t off, size_t len)
{
  PSLICE ps;
  int cancel = 1;
  int idxfound = 0;
  int retval;

  for(ps = request_q.GetHead() ; ps; ps = ps->next){
    if( ps == request_q.NextSend() ) cancel = 0;
    if( idx == ps->index ){
      if( off == ps->offset && len == ps->length ){
        if( request_q.Remove(idx,off,len) < 0 ){
          m_err_count++;
          if(arg_verbose) fprintf(stderr,"err: %p (%d) Bad CS remove\n",
            this, m_err_count);
        }
        if(cancel){
          if(stream.Send_Cancel(idx,off,len) < 0)
            return -1;
          m_req_out--;
          if( m_req_out > cfg_req_queue_length ){
            if(arg_verbose) fprintf(stderr,
              "ERROR@3: %p m_req_out underflow, resetting\n", this);
            m_req_out = 0;
          }
          if( !m_req_out && g_next_dn == this ) g_next_dn = (btPeer *)0;

          // Don't call RequestCheck() here since that could cause the slice
          // we're cancelling to be dup'd from another peer.
          return 0;
        }
        break;
      }
      idxfound = 1;
    }else if( idxfound ) break;
  }
  return 0;
}

int btPeer::ReportComplete(size_t idx)
{
  if( BTCONTENT.APieceComplete(idx) ){
    if(arg_verbose) fprintf(stderr, "Piece #%u completed\n", idx);
    WORLD.Tell_World_I_Have(idx);
    PENDINGQUEUE.Delete(idx);
    if( BTCONTENT.pBF->IsFull() ){
      ResetDLTimer();
      WORLD.CloseAllConnectionToSeed();
    }

    if( arg_file_to_download ){
      BitField tmpBitField =  *BTCONTENT.pBF;
      tmpBitField.Except(*BTCONTENT.pBFilter);

      while( arg_file_to_download &&
        tmpBitField.Count() >= BTCONTENT.getFilePieces(arg_file_to_download) ){
        //when the file is complete, we go after the next
        ++arg_file_to_download;
        BTCONTENT.FlushCache();
        BTCONTENT.SetFilter();
        tmpBitField =  *BTCONTENT.pBF;
        tmpBitField.Except(*BTCONTENT.pBFilter);
      }
      WORLD.CheckInterest();
    }
  }else{
    m_err_count++;
    if(arg_verbose) fprintf(stderr, "err: %p (%d) Bad complete\n",
      this, m_err_count);
  }
  return (P_FAILED == m_status) ? -1 : RequestCheck();
}

int btPeer::PieceDeliver(size_t mlen)
{
  size_t idx,off,len;
  char *msgbuf = stream.in_buffer.BasePointer();
  time_t t;
  int dup = 0, requested = 1;

  idx = get_nl(msgbuf + 5);
  off = get_nl(msgbuf + 9);
  len = mlen - 9;

  if(arg_verbose) fprintf(stderr, "Receiving piece %d/%d/%d from %p\n",
    (int)idx, (int)off, (int)len, this);

  t = request_q.GetReqTime(idx,off,len);

  // Verify whether this was actually requested (for queue management only).
  PSLICE ps = request_q.GetHead();
  if( request_q.NextSend() )
    for( ; ps; ps = ps->next){
      if( ps == request_q.NextSend() ){
        requested = 0;
        break;
      }
      if( idx==ps->index && off==ps->offset && len==ps->length ) break;
    }

  // If I can't keep it, leave it in the queue--I'll need to request it again.
  if(BTCONTENT.WriteSlice((char*)(msgbuf + 13),idx,off,len) < 0){
    warning(2, "warn, WriteSlice failed; is filesystem full?");
    return 0;
  }

  // request_q should only be empty here if cache flush failed (so no error).
  // in that case, slice will be removed from pending queue later.
  if( !request_q.IsEmpty() && request_q.Remove(idx,off,len) < 0 ){
    m_err_count++;
    if(arg_verbose) fprintf(stderr, "err: %p (%d) Bad remove\n",
      this, m_err_count);
    return 0;
  }

  Self.StartDLTimer();
  Self.DataRecved(len);
  DataRecved(len);

  // Check for & cancel requests for this slice from other peers in initial
  // and endgame modes.
  if( BTCONTENT.pBF->Count() < 2 ||
      WORLD.Pieces_I_Can_Get() - BTCONTENT.pBF->Count() < WORLD.TotalPeers() )
    dup = 1;
  else if( arg_file_to_download ){
    BitField afdBitField =  *BTCONTENT.pBF;
    afdBitField.Except(*BTCONTENT.pBFilter);
    if( BTCONTENT.getFilePieces(arg_file_to_download) - afdBitField.Count()
        < WORLD.TotalPeers() )
      dup = 1;
  }
  if( dup ){
    WORLD.CancelSlice(idx, off, len);
    PENDINGQUEUE.DeleteSlice(idx, off, len);
  }

  // Determine how many outstanding requests we should maintain, roughly:
  // (request turnaround latency) / (time to transmit one slice)
  if(t){
    m_latency = (m_last_timestamp <= t) ? 1 : m_last_timestamp - t;
    if(arg_verbose) fprintf(stderr, "%p latency is %d sec\n",
      this, (int)m_latency);
    m_latency_timestamp = m_last_timestamp;
  }

  if( RateDL() > len/20 ){
    m_req_send = (int)( m_latency / (len / (double)RateDL()) + 1 );
    m_req_send = (m_req_send < 2) ? 2 : m_req_send;

    // If latency increases, we will see this as a dlrate decrease.
    if( RateDL() < m_prev_dlrate ) m_req_send++;
    else if( m_last_timestamp - m_latency_timestamp >= 30 &&
      // Try to force latency measurement every 30 seconds.
        m_req_out == m_req_send - 1 ){
      m_req_send--;
      m_latency_timestamp = m_last_timestamp;
    }
    m_prev_dlrate = RateDL();
  }else if (m_req_send < 5) m_req_send = 5;

  if( requested ) m_req_out--;

  /* if piece download complete. */
  if( request_q.IsEmpty() || !request_q.HasIdx(idx) ){
    // If flushing cache failed, we may not really have a complete piece.
    // This is an interim method until we have a "master map" of recv'd slices.
    if( WORLD.SeedOnly() && PENDINGQUEUE.Exist(idx) ){
      PENDINGQUEUE.DeleteSlice(idx, off, len);
      if( PENDINGQUEUE.Exist(idx) ) return RequestCheck();
      else return ReportComplete(idx);
    }else return ReportComplete(idx);
  }else return RequestCheck();
}

int btPeer::RequestCheck()
{
  if( BTCONTENT.pBF->IsFull() ){
    if( bitfield.IsFull() ){ return -2; }
    return SetLocal(M_NOT_INTERESTED);
  }

  if( Need_Remote_Data() && !WORLD.SeedOnly() ){
    if(!m_state.local_interested && SetLocal(M_INTERESTED) < 0) return -1;
    if( !m_state.remote_choked ){
      if( m_req_out > cfg_req_queue_length ){
        if(arg_verbose)
          fprintf(stderr, "ERROR@4: %p m_req_out underflow, resetting\n", this);
        m_req_out = 0;
      }
      if( request_q.IsEmpty() && RequestPiece() < 0 ) return -1;
      else if( m_req_out < m_req_send &&
               (m_req_out < 2 || !RateDL() ||
                1 >= (m_req_out+1) * request_q.GetRequestLen() /
                     (double)RateDL() - m_latency)
      // above formula is to try to allow delay between sending batches of reqs
        && SendRequest() < 0 ) return -1;
    }
  }else
    if(m_state.local_interested && SetLocal(M_NOT_INTERESTED) < 0) return -1;
  
  if(!request_q.IsEmpty()) StartDLTimer();
  else StopDLTimer();
  return 0;
}

void btPeer::CloseConnection()
{
  if(arg_verbose) fprintf(stderr, "%p closed\n", this);
  if( P_FAILED != m_status ){
    m_status = P_FAILED;
    stream.Close();
    if( !request_q.IsEmpty() )
      PENDINGQUEUE.Pending(&request_q);
  }
  if( g_next_up == this ) g_next_up = (btPeer *)0;
  if( g_next_dn == this ) g_next_dn = (btPeer *)0;
}

int btPeer::HandShake()
{
  char txtid[PEER_ID_LEN*2+3];
  ssize_t r = stream.Feed();
  if( r < 0 ){
//  if(arg_verbose) fprintf(stderr, "hs: r<0 (%d)\n", r);
    return -1;
  }
  else if( r < 68 ){
    if(r >= 21){	// Ignore 8 reserved bytes following protocol ID.
      if( memcmp(stream.in_buffer.BasePointer()+20,
          BTCONTENT.GetShakeBuffer()+20, (r<28) ? r-20 : 8) != 0 ){
        if(arg_verbose){
          fprintf( stderr, "\npeer %p gave 0x", this);
          for(int i=20; i<r && i<27; i++) fprintf(stderr, "%2.2hx",
            (unsigned short)(unsigned char)(stream.in_buffer.BasePointer()[i]));
          fprintf( stderr, " as reserved bytes (partial)\n" );
        }
        memcpy(stream.in_buffer.BasePointer()+20, BTCONTENT.GetShakeBuffer()+20,
          (r<28) ? r-20 : 8);
      }
    }
    if(r && memcmp(stream.in_buffer.BasePointer(),BTCONTENT.GetShakeBuffer(),
        (r<48) ? r : 48) != 0){
      if(arg_verbose){
        fprintf(stderr, "\nmine: 0x");
        for(int i=0; i<r && i<48; i++) fprintf(stderr, "%2.2hx",
          (unsigned short)(unsigned char)(BTCONTENT.GetShakeBuffer()[i]));
        fprintf(stderr, "\npeer: 0x");
        for(int i=0; i<r && i<48; i++) fprintf(stderr, "%2.2hx",
          (unsigned short)(unsigned char)(stream.in_buffer.BasePointer()[i]));
        fprintf(stderr, "\n");
        if( r>48 ){
          TextPeerID((unsigned char *)(stream.in_buffer.BasePointer()+48),
            txtid);
          fprintf(stderr, "peer is %s\n", txtid);
        }
      }
      return -1;
    }
    return 0;
  }

  // If the reserved bytes differ, make them the same.
  // If they mean anything important, the handshake is likely to fail anyway.
  if( memcmp(stream.in_buffer.BasePointer()+20, BTCONTENT.GetShakeBuffer()+20,
      8) != 0 ){
    if(arg_verbose){
      fprintf(stderr, "\npeer %p gave 0x", this);
      for(int i=20; i<27; i++) fprintf(stderr, "%2.2hx",
        (unsigned short)(unsigned char)(stream.in_buffer.BasePointer()[i]));
      fprintf( stderr, " as reserved bytes\n" );
    }
    memcpy(stream.in_buffer.BasePointer()+20, BTCONTENT.GetShakeBuffer()+20, 8);
  }
  if( memcmp(stream.in_buffer.BasePointer(),
             BTCONTENT.GetShakeBuffer(),48) != 0 ){
    if(arg_verbose){
      fprintf(stderr, "\nmine: 0x");
      for(int i=0; i<48; i++) fprintf(stderr, "%2.2hx",
        (unsigned short)(unsigned char)(BTCONTENT.GetShakeBuffer()[i]));
      fprintf(stderr, "\npeer: 0x");
      for(int i=0; i<48; i++) fprintf(stderr, "%2.2hx",
        (unsigned short)(unsigned char)(stream.in_buffer.BasePointer()[i]));
      fprintf(stderr, "\n");
    }
    return -1;
  }

  memcpy(id, stream.in_buffer.BasePointer()+48, PEER_ID_LEN);
  if(arg_verbose){
    TextPeerID((unsigned char *)(stream.in_buffer.BasePointer()+48), txtid);
    fprintf(stderr, "Peer %p ID: %s\n", this, txtid);
  }

  // ignore peer id verify
  if( !BTCONTENT.pBF->IsEmpty()){
    char *bf = new char[BTCONTENT.pBF->NBytes()];
#ifndef WINDOWS
    if(!bf) return -1;
#endif
    BTCONTENT.pBF->WriteToBuffer(bf);
    r = stream.Send_Bitfield(bf,BTCONTENT.pBF->NBytes());
    delete []bf;
  }

  if( r >= 0){
    if( stream.in_buffer.PickUp(68) < 0 ) return -1;
    m_status = P_SUCCESS;
    m_want_again = 1;
    // When seeding, new peer starts at the end of the line.
    if( BTCONTENT.pBF->IsFull() ){	// i am seed
      m_unchoke_timestamp = now;
      m_connect_seed = 1;
    }
  }
  return r;
}

int btPeer::Send_ShakeInfo()
{
  return stream.Send_Buffer((char*)BTCONTENT.GetShakeBuffer(),68);
}

int btPeer::BandWidthLimitUp()
{
  if( cfg_max_bandwidth_up <= 0 ) return 0;
  return ((Self.RateUL()) >= cfg_max_bandwidth_up) ?
    1:0;
}

int btPeer::BandWidthLimitDown()
{
  if( WORLD.SeedOnly() ) return 1;
  if( cfg_max_bandwidth_down <= 0 ) return 0;
  return ((Self.RateDL()) >= cfg_max_bandwidth_down) ?
    1:0;
}

int btPeer::NeedWrite()
{
  int yn = 0;

  if( m_standby && WORLD.Endgame() ){
    if(arg_verbose) fprintf(stderr, "%p un-standby (endgame)\n", this);
    m_standby = 0;
  }

  if( stream.out_buffer.Count() || // data need send in buffer.
      // can upload a slice
      (!reponse_q.IsEmpty() && CouldReponseSlice() && !BandWidthLimitUp()) ||

      ( (request_q.NextSend() && m_req_out < m_req_send &&
            (m_req_out < 2 || !RateDL() ||
             1 >= (m_req_out+1) * request_q.GetRequestLen() /
                  (double)RateDL() - m_latency)) // can send queued request
        ||
        (request_q.IsEmpty() && !m_state.remote_choked 
          && m_state.local_interested && !m_standby) // can request a new piece
      ) // ok to send requests

      || P_CONNECTING == m_status ){ // peer is connecting

    yn = 1;

    if( g_next_up==this && g_defer_up ){
      if(arg_verbose) fprintf(stderr, "%p skipped UL\n", this);
      g_next_up = (btPeer *)0;
    }
  }
  return yn;
}

int btPeer::NeedRead()
{
  int yn = 1;
  if( !request_q.IsEmpty() && BandWidthLimitDown() )
    yn = 0;
  else if( g_next_dn==this && g_defer_dn ){
    if(arg_verbose) fprintf(stderr, "%p skipped DL\n", this);
    g_next_dn = (btPeer *)0;
  }
  return yn;
}

int btPeer::CouldReponseSlice()
{
  if(!m_state.local_choked &&
     (stream.out_buffer.LeftSize() > reponse_q.GetRequestLen() + 4 * 1024 ))
    return 1;
  return 0;
}

int btPeer::AreYouOK()
{
  m_f_keepalive = 1;
  return stream.Send_Keepalive();
}

int btPeer::RecvModule()
{
  int f_peer_closed = 0;
  ssize_t r;
  
  if ( 64 < m_err_count ) return -1;

  if( request_q.IsEmpty() || !BandWidthLimitDown() ){
    if ( request_q.IsEmpty() || !g_next_dn || g_next_dn==this ){
      if( g_next_dn ) g_next_dn = (btPeer *)0;

      r = stream.Feed();

      if( r < 0 && r != -2 )
        return -1;
      else if ( r == -2 )  // remote closed
        f_peer_closed = 1;
  
      r = stream.HaveMessage();
      for( ; r;){
        if( r < 0 ) return -1;
        if( (r = MsgDeliver()) == -2 ){
          if(arg_verbose) fprintf(stderr, "%p seed<->seed detected\n", this);
          m_want_again = 0;
        }
        if( r < 0 || stream.PickMessage() < 0 ) return -1;
        r = stream.HaveMessage();
      }
    }else{
      if(arg_verbose)
        fprintf(stderr, "%p deferring DL to %p\n", this, g_next_dn);
      if( !g_defer_dn ) g_defer_dn = 1;
    }
  }else if( !g_next_dn ){
    if(arg_verbose) fprintf(stderr, "%p waiting for DL bandwidth\n", this);
    g_next_dn = this;
    if( g_defer_dn ) g_defer_dn = 0;
  }

  return f_peer_closed ? -1 : 0;
}

int btPeer::SendModule()
{
  if( stream.out_buffer.Count() && stream.Flush() < 0) return -1;

  if( !reponse_q.IsEmpty() && CouldReponseSlice() ) {
    if( !BandWidthLimitUp() ){
      if( !g_next_up || g_next_up==this ){
        if( g_next_up ) g_next_up = (btPeer *)0;

        StartULTimer();
        Self.StartULTimer();
        if( ReponseSlice() < 0 ) return -1;
      }else{
        if(arg_verbose)
          fprintf(stderr, "%p deferring UL to %p\n", this, g_next_up);
        if( !g_defer_up ) g_defer_up = 1;
      }
    }else if( !g_next_up ){
      if(arg_verbose) fprintf(stderr, "%p waiting for UL bandwidth\n", this);
      g_next_up = this;
      if( g_defer_up ) g_defer_up = 0;
    }
  }else if( g_next_up == this ) g_next_up = (btPeer *)0;

  return (!m_state.remote_choked) ? RequestCheck() : 0;
}

// Prevent a peer object from holding g_next_up when it's not ready to write.
void btPeer::CheckSendStatus()
{
  if( g_next_up == this && !BandWidthLimitUp() ){
    if(arg_verbose) fprintf(stderr, "%p is not write-ready\n", this);
    g_next_up = (btPeer *)0;
  }
}

/* Detect if a peer ignored, discarded, or lost my request and we're waiting
   for a piece that may never arrive. */
int btPeer::HealthCheck(time_t now)
{
  if( m_health_time <= now - 60 ){
    m_health_time = now;
    if( !m_state.remote_choked && m_req_out &&
        m_receive_time < now - (!m_latency ? 300 :
                               ((m_latency < 30) ? 60 : 2*m_latency)) ){
      // if a repeat occurrence, get rid of the peer
      if( m_bad_health ) return -1;
      m_bad_health = 1;
      if(arg_verbose)
        fprintf(stderr, "%p unresponsive; resetting request queue\n", this);
      int retval = CancelRequest(request_q.GetHead());
      PENDINGQUEUE.Pending(&request_q);
      return (retval < 0) ? -1 : RequestCheck();
    } else m_bad_health = 0;
  }
  return 0;
}

// This handles peers that suppress HAVE messages so that we don't always think
// that they're empty.  If we've sent the peer an amount of data equivalent to
// two pieces, assume that they now have at least one complete piece.
int btPeer::IsEmpty() const
{
  return ( bitfield.IsEmpty() && TotalUL() < BTCONTENT.GetPieceLength()*2 ) ?
    1:0;
}

int btPeer::PutPending()
{
  int retval = 0;

  if( !request_q.IsEmpty() ){
    retval = CancelRequest(request_q.GetHead());
    PENDINGQUEUE.Pending(&request_q);
  }
  return retval;
}

void btPeer::dump()
{
  struct sockaddr_in sin;

  GetAddress(&sin);
  printf("%s: %d -> %d:%d   %lud:%lud\n", inet_ntoa(sin.sin_addr), 
          bitfield.Count(),
          Is_Remote_UnChoked() ? 1 : 0,
          request_q.IsEmpty() ? 0 : 1,
          (unsigned long)TotalDL(),
          (unsigned long)TotalUL());
}

