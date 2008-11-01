#include "peer.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "bttime.h"
#include "btstream.h"
#include "peerlist.h"
#include "console.h"
#include "util.h"

#if !defined(HAVE_SNPRINTF)
#include "compat.h"
#endif

// Convert a peer ID to a printable string.
int TextPeerID(const unsigned char *peerid, char *txtid)
{
  int i, j;

  for( i = j = 0; i < PEER_ID_LEN; i++ ){
    if( i == j && isprint(peerid[i]) && !isspace(peerid[i]) )
      txtid[j++] = peerid[i];
    else{
      if( i == j ){
         sprintf(txtid + j, "0x");
         j += 2;
      }
      snprintf(txtid + j, 3, "%.2X", (int)peerid[i]);
      j += 2;
    }
  }
  txtid[j] = '\0';

  return 0;
}

btBasic Self;

void btBasic::SetIp(struct sockaddr_in addr)
{
  memcpy(&m_sin.sin_addr, &addr.sin_addr, sizeof(struct in_addr));
}

void btBasic::SetAddress(struct sockaddr_in addr)
{
  memcpy(&m_sin, &addr, sizeof(struct sockaddr_in));
}

int btBasic::IpEquiv(struct sockaddr_in addr)
{
//  CONSOLE.Debug_n("IpEquiv: %s <=> ", inet_ntoa(m_sin.sin_addr));
//  CONSOLE.Debug_n("%s", inet_ntoa(addr.sin_addr));
//  CONSOLE.Debug_n("");
  return
    (memcmp(&m_sin.sin_addr, &addr.sin_addr, sizeof(struct in_addr)) == 0) ?
      1 : 0;
}

int btPeer::Need_Local_Data() const
{
  if( m_state.remote_interested && !bitfield.IsFull() ){
    if( BTCONTENT.IsFull() ) return 1;

    Bitfield tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(bitfield);
    return tmpBitfield.IsEmpty() ? 0 : 1;
  }
  return 0;
}

int btPeer::Need_Remote_Data() const
{
  if( BTCONTENT.Seeding() || bitfield.IsEmpty() ) return 0;
  else if( bitfield.IsFull() &&
           BTCONTENT.CheckedPieces() >= BTCONTENT.GetNPieces() ){
     return 1;
  }else{
    Bitfield tmpBitfield = bitfield;                // what peer has
    tmpBitfield.Except(*BTCONTENT.pBF);             // what I have
    tmpBitfield.Except(*BTCONTENT.pBMasterFilter);  // what I don't want
    tmpBitfield.And(*BTCONTENT.pBChecked);          // what I've checked
    return tmpBitfield.IsEmpty() ? 0 : 1;
  }
  return 0;
}

btPeer::btPeer()
{
  m_f_keepalive = 0;
  m_status = DT_PEER_CONNECTING;
  m_unchoke_timestamp = (time_t)0;
  m_last_timestamp = m_next_send_time = now;
  m_state.remote_choked = m_state.local_choked = 1;
  m_state.remote_interested = m_state.local_interested = 0;

  m_err_count = 0;
  m_cached_idx = m_last_req_piece = BTCONTENT.GetNPieces();
  m_standby = 0;
  m_req_send = 5;
  m_req_out = 0;
  m_latency = 0;
  m_prev_dlrate = 0;
  m_health_time = m_receive_time = m_choketime = m_last_timestamp;
  m_cancel_time = m_latency_timestamp = (time_t)0;
  m_bad_health = 0;
  m_want_again = m_connect = m_retried = 0;
  m_connect_seed = 0;
  m_prefetch_time = (time_t)0;
  m_requested = 0;
  m_prefetch_completion = 0;
  readycnt = 0;

  for( int i=0; i < HAVEQ_SIZE; i++ ){
    m_haveq[i] = BTCONTENT.GetNPieces();
  }

  rate_dl.SetSelf(Self.DLRatePtr());
  rate_ul.SetSelf(Self.ULRatePtr());
}

void btPeer::CopyStats(btPeer *peer)
{
  SetDLRate(peer->GetDLRate());
  SetULRate(peer->GetULRate());
  m_unchoke_timestamp = peer->GetLastUnchokeTime();
  m_retried = peer->Retried();  // don't try to reconnect over & over
}

int btPeer::SetLocal(bt_msg_t s)
{
  switch( s ){
  case BT_MSG_CHOKE:
    if( m_state.local_choked ) return 0;
    m_unchoke_timestamp = now;
//  if(*cfg_verbose) CONSOLE.Debug("Choking %p", this);
    if(*cfg_verbose) CONSOLE.Debug("Choking %p (D=%lluMB@%dK/s)", this,
      (unsigned long long)TotalDL() >> 20, (int)(RateDL() >> 10));
    m_state.local_choked = 1;
    WORLD.DontWaitUL(this);
    if( !respond_q.IsEmpty() ) respond_q.Empty();
    StopULTimer();
    if( !m_requested && BTCONTENT.IsFull() ){
      // hasn't sent a request since unchoke
      if(*cfg_verbose) CONSOLE.Debug("%p inactive", this);
      return -1;
    }
    m_requested = 0;
    break;
  case BT_MSG_UNCHOKE:
    if( !respond_q.IsEmpty() ) StartULTimer();
    if( !m_state.local_choked ) return 0;
    m_unchoke_timestamp = now;
//  if(*cfg_verbose) CONSOLE.Debug("Unchoking %p", this);
    if(*cfg_verbose) CONSOLE.Debug("Unchoking %p (D=%lluMB@%dK/s)", this,
      (unsigned long long)TotalDL() >> 20, (int)(RateDL() >> 10));
    m_state.local_choked = 0;
    // No data is queued, so rate cannot delay sending.
    m_next_send_time = now;
    break;
  case BT_MSG_INTERESTED:
    if( BTCONTENT.Seeding() ) return 0;
    m_standby = 0;
    if( m_state.local_interested ) return 0;
    if(*cfg_verbose) CONSOLE.Debug("Interested in %p", this);
    m_state.local_interested = 1;
    break;
  case BT_MSG_NOT_INTERESTED:
    if( !m_state.local_interested ) return 0;
    if(*cfg_verbose) CONSOLE.Debug("Not interested in %p", this);
    m_state.local_interested = 0;
    if( !request_q.IsEmpty() ){
      if( CancelRequest() < 0 ) return -1;
      request_q.Empty();
    }
    break;
  default:
    CONSOLE.Warning(1, "error, invalid state %d in SetLocal", (int)s);
    return -1;  // invalid state (bug)
  }

  if( stream.Send_State(s) < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  }else return 0;
}

int btPeer::RequestPiece()
{
  bt_index_t idx;
  Bitfield tmpBitfield;
  const Bitfield *pfilter;

  dt_count_t qsize = request_q.Qsize();
  bt_length_t psize = BTCONTENT.GetPieceLength() / *cfg_req_slice_size;

  /* See if there's room in the queue for a new piece.
     Also, don't queue another piece if we still have a full piece queued. */
  if( ReqQueueLength() - qsize < psize || qsize >= psize ){
    m_req_send = m_req_out;  // don't come back until you receive something.
    return 0;
  }

  tmpBitfield = bitfield;
  tmpBitfield.Except(*BTCONTENT.pBMasterFilter);
  if( m_last_req_piece < BTCONTENT.GetNPieces() && tmpBitfield.Count() > 1 )
    tmpBitfield.UnSet(m_last_req_piece);
  if( (idx = PENDINGQUEUE.Reassign(&request_q, tmpBitfield)) <
      BTCONTENT.GetNPieces() ){
    if(*cfg_verbose)
      CONSOLE.Debug("Assigning #%d to %p from Pending", (int)idx, this);
    if( BTCONTENT.pBMultPeer->IsSet(idx) ) WORLD.CompareRequest(this, idx);
    BTCONTENT.pBMultPeer->Set(idx);
    return SendRequest();
  }

  // If we didn't want the cached piece, select another.
  if( BTCONTENT.pBF->IsEmpty() ){
    /* If we don't have a complete piece yet, try to get one that's already
       in progress.  (Initial-piece mode) */
    pfilter = BTCONTENT.GetFilter();
    do{
      tmpBitfield = bitfield;
      if( pfilter ){
        tmpBitfield.Except(*pfilter);
        pfilter = BTCONTENT.GetNextFilter(pfilter);
      }
    }while( pfilter && tmpBitfield.IsEmpty() );
    if( m_latency < 60 ){
      // Don't dup to very slow/high latency peers.
      if( m_last_req_piece < BTCONTENT.GetNPieces() && tmpBitfield.Count() > 1 )
        tmpBitfield.UnSet(m_last_req_piece);
      idx = WORLD.What_Can_Duplicate(tmpBitfield, this, BTCONTENT.GetNPieces());
      if( idx < BTCONTENT.GetNPieces() ){
        if(*cfg_verbose) CONSOLE.Debug("Want to dup #%d to %p", (int)idx, this);
        btPeer *peer = WORLD.WhoHas(idx);
        if( peer ){
          if(*cfg_verbose)
            CONSOLE.Debug("Duping #%d from %p to %p", (int)idx, peer, this);
          if( request_q.CopyShuffle(&peer->request_q, idx) < 0 ) return -1;
          WORLD.CompareRequest(this, idx);
          BTCONTENT.pBMultPeer->Set(idx);
          return SendRequest();
        }
      }else if(*cfg_verbose) CONSOLE.Debug("Nothing to dup to %p", this);
    }
  }

  // Doesn't have a piece that's already in progress--choose another.
  pfilter = BTCONTENT.GetFilter();
  do{
    tmpBitfield = bitfield;
    tmpBitfield.Except(*BTCONTENT.pBF);
    if( pfilter ){
      tmpBitfield.Except(*pfilter);
      pfilter = BTCONTENT.GetNextFilter(pfilter);
    }
    // Don't go after pieces we might already have (but don't know yet)
    tmpBitfield.And(*BTCONTENT.pBChecked);
    // tmpBitfield tells what we need from this peer...
  }while( pfilter && tmpBitfield.IsEmpty() );
  if( m_last_req_piece < BTCONTENT.GetNPieces() && tmpBitfield.Count() > 1 )
    tmpBitfield.UnSet(m_last_req_piece);

  if( tmpBitfield.IsEmpty() ){
    // We don't need to request anything from the peer.
    if( !Need_Remote_Data() )
      return SetLocal(BT_MSG_NOT_INTERESTED);
    else if( m_last_req_piece < BTCONTENT.GetNPieces() &&
             !BTCONTENT.pBF->IsSet(m_last_req_piece) ){
      // May have excluded the only viable request; allow a retry.
      m_last_req_piece = BTCONTENT.GetNPieces();
      return 0;  // Allow another peer a shot at it first.
    }else{
      if(*cfg_verbose) CONSOLE.Debug("%p standby", this);
      m_standby = 1;  // nothing to do at the moment
      return 0;
    }
  }

  WORLD.CheckBitfield(tmpBitfield);
  // [tmpBitfield] ...that we haven't requested from anyone.
  if( tmpBitfield.IsEmpty() ){
    // Everything this peer has that I want, I've already requested.
    int endgame = WORLD.Endgame();
    if( endgame && m_latency < 60 ){
      // OK to duplicate a request, but not to very slow/high latency peers.
      idx = 0;  // flag for Who_Can_Duplicate()
      Bitfield tmpBitfield2 = tmpBitfield;
      idx = WORLD.What_Can_Duplicate(tmpBitfield2, this, idx);
      if( idx < BTCONTENT.GetNPieces() ){
        if(*cfg_verbose) CONSOLE.Debug("Want to dup #%d to %p", (int)idx, this);
        btPeer *peer = WORLD.WhoHas(idx);
        if( peer ){  // failsafe
          if(*cfg_verbose)
            CONSOLE.Debug("Duping #%d from %p to %p", (int)idx, peer, this);
          if( request_q.CopyShuffle(&peer->request_q, idx) < 0 ) return -1;
          WORLD.CompareRequest(this, idx);
          BTCONTENT.pBMultPeer->Set(idx);
          return SendRequest();
        }
      }
    }
    btPeer *peer;
    if( request_q.IsEmpty() && (peer = WORLD.Who_Can_Abandon(this)) ){
      // Cancel a request to the slowest peer & request it from this one.
      idx = peer->FindLastCommonRequest(bitfield);
      if(*cfg_verbose)
        CONSOLE.Debug("Reassigning #%d from %p to %p", (int)idx, peer, this);
      // RequestQueue class "moves" rather than "copies" in assignment!
      if( request_q.Copy(&peer->request_q, idx) < 0 ) return -1;
      WORLD.CompareRequest(this, idx);
      BTCONTENT.pBMultPeer->Set(idx);
      if( endgame ) peer->UnStandby();
      if( peer->CancelPiece(idx) < 0 ) peer->CloseConnection();
      return SendRequest();
    }else if( BTCONTENT.CheckedPieces() >= BTCONTENT.GetNPieces() ){
      if(*cfg_verbose) CONSOLE.Debug("%p standby", this);
      m_standby = 1;  // nothing to do at the moment
    }
  }else{
    /* Request something that we haven't requested yet (most common case).
       Try to make it something that has good trade value. */
    Bitfield tmpBitfield2 = tmpBitfield;
    WORLD.FindValuedPieces(tmpBitfield2, this, BTCONTENT.pBF->IsEmpty());
    if( tmpBitfield2.IsEmpty() ) tmpBitfield2 = tmpBitfield;

    if( m_cached_idx < BTCONTENT.CheckedPieces() &&
        tmpBitfield2.IsSet(m_cached_idx) ){
      // Prefer the piece indicated by a recent HAVE message.
      idx = m_cached_idx;
    }else idx = BTCONTENT.GetNPieces();

    if( BTCONTENT.pBF->Count() < BTCONTENT.GetNFiles() ||
        (idx = BTCONTENT.ChoosePiece(tmpBitfield2, tmpBitfield, idx)) >=
          BTCONTENT.GetNPieces() ){
      idx = tmpBitfield2.Random();
    }
    if(*cfg_verbose) CONSOLE.Debug("Assigning #%d to %p", (int)idx, this);
    return ( request_q.CreateWithIdx(idx) < 0 ) ? -1 : SendRequest();
  }
  return 0;
}

int btPeer::MsgDeliver()
{
  bt_msglen_t msglen;
  bt_index_t idx;
  bt_offset_t off;
  bt_length_t len;
  int retval = 0;

  const char *msgbuf = stream.in_buffer.BasePointer();

  msglen = get_bt_msglen(msgbuf);

  // Don't require keepalives if we're receiving other messages.
  m_last_timestamp = now;
  if( 0 == msglen ){
    if( !m_f_keepalive && stream.Send_Keepalive() < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
    m_f_keepalive = 0;
    return 0;
  }else{
    bt_msg_t msg = (bt_msg_t)msgbuf[BT_LEN_PRE];
    switch( msg ){
    case BT_MSG_CHOKE:
      if( BT_LEN_MSGID != msglen ) return -1;
      if(*cfg_verbose) CONSOLE.Debug("%p choked me", this);
      if( m_lastmsg == BT_MSG_UNCHOKE && m_last_timestamp <= m_choketime+1 ){
        if( PeerError(2, "Choke oscillation") < 0 ) return -1;
      }
      m_choketime = m_last_timestamp;
      m_state.remote_choked = 1;
      StopDLTimer();
      WORLD.DontWaitDL(this);
      if( !request_q.IsEmpty() ){
        BTCONTENT.pBMultPeer->Set(request_q.GetRequestIdx());
        PutPending();
      }
      m_cancel_time = now;
      break;

    case BT_MSG_UNCHOKE:
      if( BT_LEN_MSGID != msglen ) return -1;
      if(*cfg_verbose) CONSOLE.Debug("%p unchoked me", this);
      if( m_lastmsg == BT_MSG_CHOKE && m_last_timestamp <= m_choketime+1 ){
        if( PeerError(2, "Choke oscillation") < 0 ) return -1;
      }
      m_choketime = m_last_timestamp;
      m_state.remote_choked = 0;
      m_standby = 0;
      if( !stream.PeekNextMessage(BT_MSG_CHOKE) ){
        m_prefetch_completion = 0;
        retval = RequestCheck();
      }
      break;

    case BT_MSG_INTERESTED:
      if( BT_LEN_MSGID != msglen ) return -1;
      if(*cfg_verbose) CONSOLE.Debug("%p is interested", this);
      m_state.remote_interested = 1;
      if( Need_Local_Data() ) WORLD.UnchokeIfFree(this);
      break;

    case BT_MSG_NOT_INTERESTED:
      if( BT_LEN_MSGID != msglen ) return -1;
      if(*cfg_verbose) CONSOLE.Debug("%p is not interested", this);

      m_state.remote_interested = 0;

      /* remove peer's respond queue */
      if( !respond_q.IsEmpty() ) respond_q.Empty();

      /* if I've been seed for a while, nobody should be uninterested */
      if( BTCONTENT.IsFull() && BTCONTENT.GetSeedTime() - now >= 300 )
         return -2;
      break;

    case BT_MSG_HAVE:
      if( BT_MSGLEN_HAVE != msglen ) return -1;

      idx = get_bt_index(msgbuf + BT_LEN_PRE + BT_LEN_MSGID);

      if( idx >= BTCONTENT.GetNPieces() || bitfield.IsSet(idx) ) return -1;

      bitfield.Set(idx);

      if( bitfield.IsFull() ){
        if( BTCONTENT.IsFull() ) return -2;
        else stream.out_buffer.SetSize(BUF_DEF_SIZ);
      }

      if( !BTCONTENT.pBF->IsSet(idx) && !BTCONTENT.pBMasterFilter->IsSet(idx) ){
        if( m_cached_idx >= BTCONTENT.GetNPieces() || m_standby ||
            (!BTCONTENT.GetFilter() || !BTCONTENT.GetFilter()->IsSet(idx)) ){
          m_cached_idx = idx;
        }
        if( *cfg_verbose && m_standby ) CONSOLE.Debug("%p un-standby", this);
        m_standby = 0;
      }

      // see if we're Interested now
      if( !m_standby ) retval = RequestCheck();
      break;

    case BT_MSG_REQUEST:
      if( BT_MSGLEN_REQUEST != msglen || !m_state.remote_interested ) return -1;

      idx = get_bt_index(msgbuf + BT_LEN_PRE + BT_LEN_MSGID);

      if( !BTCONTENT.pBF->IsSet(idx) ) return -1;

      off = get_bt_offset(msgbuf + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX);
      len = get_bt_length(msgbuf + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX +
                          BT_LEN_OFF);

      if(*cfg_verbose) CONSOLE.Debug("%p is requesting %d/%d/%d",
        this, (int)idx, (int)off, (int)len);

      if( !respond_q.IsValidRequest(idx, off, len) ) return -1;

      if( m_state.local_choked ){
        if( m_last_timestamp - m_unchoke_timestamp >
              (m_latency ? (m_latency*2) : 60) ){
          if( PeerError(1, "choked request") < 0 ) return -1;
          if( stream.Send_State(BT_MSG_CHOKE) < 0 ){
            if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
            return -1;
          }
          /* This will mess with the unchoke rotation (to this peer's
             disadvantage), but otherwise we may spam them with choke msgs. */
          m_unchoke_timestamp = m_last_timestamp;
        }
      }else{
        if( !m_requested ){
          m_requested = 1;
          if( stream.out_buffer.SetSize(BUF_DEF_SIZ +
              (len < DEFAULT_SLICE_SIZE) ? DEFAULT_SLICE_SIZE : len) < 0 ){
            return -1;
          }
          if( (!m_receive_time || BTCONTENT.Seeding()) &&
              now > m_unchoke_timestamp ){
            m_latency = (now <= m_unchoke_timestamp) ? 1 :
              (now - m_unchoke_timestamp);
            if(*cfg_verbose) CONSOLE.Debug("%p latency is %d sec (request)",
              this, (int)m_latency);
          }
        }
        retval = respond_q.Add(idx, off, len);
      }
      break;

    case BT_MSG_PIECE:
      if( BT_MSGLEN_PIECE >= msglen ) return -1;
      m_receive_time = m_last_timestamp;
      // PieceDeliver handles the error determination & DL counting
      retval = PieceDeliver(msglen);
      break;

    case BT_MSG_BITFIELD:
      if( msglen - BT_LEN_MSGID != bitfield.NBytes() || !bitfield.IsEmpty() )
        return -1;
      bitfield.SetReferBuffer(msgbuf + BT_LEN_PRE + BT_LEN_MSGID);
      if( bitfield.IsFull() ){
        if(*cfg_verbose) CONSOLE.Debug("%p is a seed (bitfield is full)", this);
        if( BTCONTENT.IsFull() ) return -2;
        else{
          stream.out_buffer.SetSize(BUF_DEF_SIZ);
          if( !m_want_again ) m_want_again = 1;
        }
      }else if(*cfg_verbose){
        if( bitfield.IsEmpty() ) CONSOLE.Debug("%p bitfield is empty", this);
        else CONSOLE.Debug("%p bitfield has %d%%", this,
                           100 * bitfield.Count() / BTCONTENT.GetNPieces());
      }

      // This is needed in order to set our Interested state.
      retval = RequestCheck();
      break;

    case BT_MSG_CANCEL:
      if( BT_MSGLEN_CANCEL != msglen ) return -1;

      idx = get_bt_index(msgbuf + BT_LEN_PRE + BT_LEN_MSGID);
      off = get_bt_offset(msgbuf + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX);
      len = get_bt_length(msgbuf + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX +
                          BT_LEN_OFF);
      if( respond_q.Remove(idx, off, len) < 0 ){
        if( m_state.local_choked &&
            m_last_timestamp - m_unchoke_timestamp >
              (m_latency ? (m_latency*2) : 60) ){
          if( PeerError(1, "Bad cancel") < 0 ) return -1;
        }
      }else if( respond_q.IsEmpty() )
        WORLD.DontWaitUL(this);
      break;

    default:
      if(*cfg_verbose)
        CONSOLE.Debug("Unknown message type %d from peer %p", (int)msg, this);
    }  // end switch

    if( retval >= 0 ) m_lastmsg = msg;
  }
  return retval;
}

int btPeer::RespondSlice()
{
  bt_length_t len = 0;
  double rightnow;

  int r;
  bt_index_t idx;
  bt_offset_t off;
  respond_q.Pop(&idx, &off, &len);

  if( BTCONTENT.global_buffer_size < len ){
    delete []BTCONTENT.global_piece_buffer;
    BTCONTENT.global_piece_buffer = new char[len];
    BTCONTENT.global_buffer_size = BTCONTENT.global_piece_buffer ? len : 0;
  }
  r = BTCONTENT.ReadSlice(BTCONTENT.global_piece_buffer, idx, off, len);
  if( r < 0 ) return -1;
  else if( r && *cfg_cache_size ) Self.OntimeUL(0);  // disk read delay
  // If not using cache, need to always allow time for a disk read.

  dt_rate_t currentrate = CurrentUL();
  if(*cfg_verbose)
    CONSOLE.Debug("Sending %d/%d/%d to %p", (int)idx, (int)off, (int)len, this);
  // project the time to send another slice
  if( 0==currentrate ){  // don't know peer's rate; use best guess
    dt_rate_t rate = Self.RateUL();
    dt_count_t unchoked = WORLD.GetUnchoked();  // can't be 0 here
    if( *cfg_max_bandwidth_up < unchoked ||
        *cfg_max_bandwidth_up <= rate ){
      if( rate < unchoked || rate < (dt_rate_t)(unchoked*len/3600) )
        m_next_send_time = now;
      else m_next_send_time = now + (time_t)(len / ((double)rate / unchoked));
    }else{
      m_next_send_time = now + (time_t)(len /
        ( (*cfg_max_bandwidth_up - rate > *cfg_max_bandwidth_up / unchoked) ?
            *cfg_max_bandwidth_up - rate :
            (double)*cfg_max_bandwidth_up / unchoked ));
    }
  }else m_next_send_time = now + (time_t)(len /
    ( (currentrate < *cfg_max_bandwidth_up || 0==*cfg_max_bandwidth_up) ?
        currentrate : *cfg_max_bandwidth_up ));

  m_prefetch_time = (time_t)0;

  rightnow = PreciseTime();
  if( stream.Send_Piece(idx, off, BTCONTENT.global_piece_buffer, len) < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  }else{
    WORLD.Upload();
    DataSent(len, rightnow);
    if( !m_want_again && BTCONTENT.Seeding() )
      m_want_again = 1;
  }
  return 0;
}

int btPeer::SendRequest()
{
  int first = 1;
  PSLICE ps = request_q.NextSend();

  if( m_req_out > MaxReqQueueLength() ){
    if(*cfg_verbose)
      CONSOLE.Debug("ERROR@5: %p m_req_out underflow, resetting", this);
    m_req_out = 0;
  }
  if( ps && m_req_out < m_req_send ){
    if(*cfg_verbose){
      CONSOLE.Debug_n("");
      CONSOLE.Debug_n("Requesting #%d from %p (%d left, %d slots):",
        (int)ps->index, this, (int)request_q.Qsize(), (int)m_req_send);
    }
    for( int i=0; ps && m_req_out < m_req_send && i<5; ps = ps->next, i++ ){
      if( first && (!RateDL() ||
            0 >= (m_req_out+1) * ps->length / (double)RateDL() - m_latency) ){
        request_q.SetReqTime(ps, now);
        first = 0;
      }else request_q.SetReqTime(ps, (time_t)0);
      if(*cfg_verbose) CONSOLE.Debug_n(".");
      if( stream.Send_Request(ps->index, ps->offset, ps->length) < 0 ){
        if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
        return -1;
      }
      m_last_req_piece = ps->index;
      request_q.SetNextSend(ps->next);
      m_req_out++;
    }
    if(*cfg_verbose) CONSOLE.Debug_n("");
    m_receive_time = now;
  }
  return ( m_req_out < m_req_send && !m_standby ) ? RequestPiece() : 0;
}

int btPeer::CancelPiece(bt_index_t idx)
{
  PSLICE ps = request_q.GetHead();
  PSLICE next;
  int cancel = 1;

  for( ; ps && ps->index != idx; ps=ps->next ){  // find the piece
    if( ps == request_q.NextSend() ) cancel = 0;
  }
  if( !ps ) return 0;

  for( ; ps; ps = next ){
    if( ps->index != idx ) break;
    if( ps == request_q.NextSend() ) cancel = 0;
    if( cancel ){
      if(*cfg_verbose) CONSOLE.Debug("Cancelling %d/%d/%d to %p",
        (int)ps->index, (int)ps->offset, (int)ps->length, this);
      if( stream.Send_Cancel(ps->index, ps->offset, ps->length) < 0 ){
        if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
        return -1;
      }
      m_req_out--;
      if( m_req_out > MaxReqQueueLength() ){
        if(*cfg_verbose)
          CONSOLE.Debug("ERROR@1: %p m_req_out underflow, resetting", this);
        m_req_out = 0;
      }
      m_cancel_time = now;
    }
    next = ps->next;
    request_q.Remove(ps->index, ps->offset, ps->length);
  }
  if( request_q.IsEmpty() ){
    StopDLTimer();
    m_standby = 0;
  }
  if( !m_req_out ) WORLD.DontWaitDL(this);

  return 1;
}

int btPeer::CancelRequest()
{
  PSLICE ps;

  ps = request_q.GetHead();
  for( ; ps; ps = ps->next ){
    if( ps == request_q.NextSend() ) break;
    if(*cfg_verbose) CONSOLE.Debug("Cancelling %d/%d/%d to %p",
      (int)ps->index, (int)ps->offset, (int)ps->length, this);
    if( stream.Send_Cancel(ps->index, ps->offset, ps->length) < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
    m_req_out--;
    if( m_req_out > MaxReqQueueLength() ){
      if(*cfg_verbose)
        CONSOLE.Debug("ERROR@2: %p m_req_out underflow, resetting", this);
      m_req_out = 0;
    }
    m_cancel_time = now;
  }
  if( !m_req_out ) WORLD.DontWaitDL(this);

  return 0;
}

int btPeer::CancelSliceRequest(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  PSLICE ps;
  int cancel = 1;
  int idxfound = 0;
  int retval = 0;

  if( request_q.IsEmpty() ) return 0;

  for( ps = request_q.GetHead(); ps; ps = ps->next ){
    if( ps == request_q.NextSend() ) cancel = 0;
    if( idx == ps->index ){
      if( off == ps->offset && len == ps->length ){
        retval = 1;
        request_q.Remove(idx, off, len);
        if( cancel ){
          if(*cfg_verbose) CONSOLE.Debug("Cancelling %d/%d/%d to %p",
            (int)idx, (int)off, (int)len, this);
          if( stream.Send_Cancel(idx, off, len) < 0 ){
            if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
            return -1;
          }
          m_req_out--;
          if( m_req_out > MaxReqQueueLength() ){
            if(*cfg_verbose)
              CONSOLE.Debug("ERROR@3: %p m_req_out underflow, resetting", this);
            m_req_out = 0;
          }
          if( !m_req_out ) WORLD.DontWaitDL(this);
          m_cancel_time = now;

          /* Don't call RequestCheck() here since that could cause the slice
             we're cancelling to be dup'd from another peer. */
        }
        break;
      }
      idxfound = 1;
    }else if( idxfound ) break;
  }
  if( request_q.IsEmpty() ){
    StopDLTimer();
    m_standby = 0;
  }

  return retval;
}

bt_index_t btPeer::FindLastCommonRequest(const Bitfield &proposerbf) const
{
  PSLICE ps;
  bt_index_t idx, piece;

  idx = piece = BTCONTENT.GetNPieces();
  if( request_q.IsEmpty() ) return piece;
  ps = request_q.GetHead();
  for( ; ps; ps = ps->next ){
    if( ps->index != idx ){
      idx = ps->index;
      if( proposerbf.IsSet(idx) ) piece = idx;
    }
  }
  return piece;
}

int btPeer::ReportComplete(bt_index_t idx, bt_length_t len)
{
  int r;

  if( (r = BTCONTENT.APieceComplete(idx)) > 0 ){
    if(*cfg_verbose) CONSOLE.Debug("Piece #%d completed", (int)idx);
    PeerError(-1, "Piece completed");
    WORLD.Tell_World_I_Have(idx);
    BTCONTENT.CheckFilter();
    if( BTCONTENT.IsFull() )
      WORLD.CloseAllConnectionToSeed();
  }else if( 0 == r ){  // hash check failed
    /* Don't count an error against the peer in initial or endgame mode, since
       some slices may have come from other peers. */
    if( !BTCONTENT.pBMultPeer->IsSet(idx) ){
      // The entire piece came from this peer.
      DataUnRec(BTCONTENT.GetPieceLength(idx) - len);
      if( PeerError(4, "Bad complete") < 0 ) CloseConnection();
      else{
        ResetDLTimer();  // set peer rate=0 so we don't favor for upload
        bitfield.UnSet(idx);  // don't request this piece from this peer again
      }
    }
  }
  // Need to re-download entire piece if check failed, so cleanup in any case.
  m_prefetch_completion = 0;
  if( WORLD.GetDupReqs() && BTCONTENT.pBMultPeer->IsSet(idx) ){
    if( WORLD.CancelPiece(idx) && *cfg_verbose )
      CONSOLE.Debug("Duplicate request cancelled in piece completion");
  }
  if( PENDINGQUEUE.Delete(idx) && *cfg_verbose )
    CONSOLE.Debug("Duplicate found in Pending, shouldn't be there");
  BTCONTENT.pBMultPeer->UnSet(idx);
  return r;
}

int btPeer::PieceDeliver(bt_msglen_t mlen)
{
  bt_index_t idx;
  bt_offset_t off;
  bt_length_t len;
  const char *msgbuf = stream.in_buffer.BasePointer();
  time_t t = (time_t)0;
  int f_accept = 0, f_requested = 0, f_success = 1, f_count = 1, f_want = 1;
  int f_complete = 0, dup = 0;

  idx = get_bt_index(msgbuf + BT_LEN_PRE + BT_LEN_MSGID);
  off = get_bt_offset(msgbuf + BT_LEN_PRE + BT_LEN_MSGID + BT_LEN_IDX);
  len = mlen - BT_MSGLEN_PIECE;

  if( !request_q.IsEmpty() ){
    t = request_q.GetReqTime(idx, off, len);
    // Verify whether this is an outstanding request (not for error counting).
    PSLICE ps = request_q.GetHead();
    for( ; ps; ps = ps->next ){
      if( ps == request_q.NextSend() ) break;
      if( idx==ps->index && off==ps->offset && len==ps->length ){
        f_requested = 1;
        break;
      }
    }
  }

  // If the slice is outstanding and was cancelled from this peer, accept.
  if( !f_requested && BTCONTENT.pBMultPeer->IsSet(idx) &&
      m_last_timestamp - m_cancel_time <= (m_latency ? (m_latency*2) : 60) &&
      (WORLD.HasSlice(idx, off, len) || PENDINGQUEUE.HasSlice(idx, off, len)) ){
    f_accept = dup = 1;
  }

  Self.StartDLTimer();

  if( f_requested || f_accept ){
    if(*cfg_verbose) CONSOLE.Debug("Receiving piece %d/%d/%d from %p",
      (int)idx, (int)off, (int)len, this);
    if( !BTCONTENT.pBF->IsSet(idx) &&
        BTCONTENT.WriteSlice(msgbuf + BT_LEN_PRE + BT_MSGLEN_PIECE, idx, off,
          len) < 0 ){
      CONSOLE.Warning(2, "warn, WriteSlice failed; is filesystem full?");
      f_success = 0;
      /* Re-queue the request, unless WriteSlice triggered flush failure
         (then the request is already in Pending). */
      if( f_requested && !BTCONTENT.FlushFailed() ){
        // This removes only the first instance; re-queued request is safe.
        request_q.Remove(idx, off, len);
        m_req_out--;
        if( RequestSlice(idx, off, len) < 0 ){
          // At least it's still queued & will go to Pending at peer close.
          if( f_count ) DataRecd(len);
          return -1;
        }
      }
    }else{  // saved or had the data
      request_q.Remove(idx, off, len);
      if( f_requested ) m_req_out--;
      /* Check for & cancel requests for this slice from other peers in initial
         and endgame modes. */
      if( dup || (WORLD.GetDupReqs() && BTCONTENT.pBMultPeer->IsSet(idx)) )
        dup = WORLD.CancelSlice(idx, off, len);
      if( WORLD.GetDupReqs() || BTCONTENT.FlushFailed() )
        dup += PENDINGQUEUE.DeleteSlice(idx, off, len);
    }
  }else{  // not requested--not saved
    if( m_last_timestamp - m_cancel_time > (m_latency ? (m_latency*2) : 60) ){
      char msg[40];
      BTCONTENT.CountUnwantedBlock();
      sprintf(msg, "Unrequested piece %d/%d/%d", (int)idx, (int)off, (int)len);
      if( PeerError(1, msg) < 0 ) return -1;
      ResetDLTimer();  // set peer rate=0 so we don't favor for upload
      f_count = f_want = 0;
    }else{
      if(*cfg_verbose) CONSOLE.Debug("Unneeded piece %d/%d/%d from %p",
        (int)idx, (int)off, (int)len, this);
      BTCONTENT.CountDupBlock(len);
    }
    f_success = 0;
  }
  if( !m_want_again && f_want ) m_want_again = 1;

  /* Determine how many outstanding requests we should maintain, roughly:
     (request turnaround latency) / (time to transmit one slice) */
  if( f_requested ){
    if( t ){
      m_latency = (m_last_timestamp <= t) ? 1 : (m_last_timestamp - t);
      if(*cfg_verbose) CONSOLE.Debug("%p latency is %d sec (receive)",
        this, (int)m_latency);
      m_latency_timestamp = m_last_timestamp;
    }
    dt_rate_t rate;
    if( (rate = RateDL()) > len/20 && m_latency_timestamp ){
      /* 20==RATE_INTERVAL from rate.cpp.  This is really just a check to see
         if rate is measurable/usable. */
      m_req_send = (dt_count_t)(m_latency / (len / (double)rate) + 1);
      if( m_req_send < 2 ) m_req_send = 2;

      // If latency increases, we will see this as a dlrate decrease.
      if( rate < m_prev_dlrate ) m_req_send++;
      else if( m_last_timestamp - m_latency_timestamp >= 30 &&
          m_req_out == m_req_send - 1 ){
        // Try to force latency measurement every 30 seconds.
        m_req_send--;
        m_latency_timestamp = m_last_timestamp;
      }
      m_prev_dlrate = rate;
    }else if( m_req_send < 5 ) m_req_send = 5;
  }

  /* if piece download complete. */
  if( f_success && !BTCONTENT.pBF->IsSet(idx) &&
      ( (f_requested && (request_q.IsEmpty() || !request_q.HasIdx(idx))) ||
        (f_accept && !WORLD.WhoHas(idx) && !PENDINGQUEUE.Exist(idx)) ) ){
    /* Above WriteSlice may have triggered flush failure.  If data was saved,
       slice was deleted from Pending.  If piece is incomplete, it's in
       Pending. */
    if( !(BTCONTENT.FlushFailed() && PENDINGQUEUE.Exist(idx)) &&
        !(f_complete = ReportComplete(idx, len)) ){
      f_count = 0;
    }
  }

  /* Don't count the slice in our DL total if it was unsolicited or bad.
     (We don't owe the swarm any UL for such data.) */
  if( f_count ) DataRecd(len);

  if( !f_complete && dup ) WORLD.CancelOneRequest(idx);

  if( request_q.IsEmpty() ){
    StopDLTimer();
    if( f_requested ) m_standby = 0;
  }

  return ( DT_PEER_FAILED == m_status ) ? -1 :
                                          ( m_standby || !f_requested ) ? 0 :
                                          RequestCheck();
}

/* This is for re-requesting unsuccessful slices.
   Use RequestPiece for normal request queueing. */
int btPeer::RequestSlice(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  int r;
  r = request_q.Requeue(idx, off, len);
  if( r < 0 ) return -1;
  else if( r ){
    if( stream.Send_Request(idx, off, len) < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
    m_req_out++;
    m_receive_time = now;
  }
  return 0;
}

int btPeer::RequestCheck()
{
  if( BTCONTENT.Seeding() || WORLD.IsPaused() )
    return SetLocal(BT_MSG_NOT_INTERESTED);

  if( Need_Remote_Data() ){
    if( !m_state.local_interested && SetLocal(BT_MSG_INTERESTED) < 0 )
      return -1;
    if( !m_state.remote_choked ){
      if( m_req_out > MaxReqQueueLength() ){
        if(*cfg_verbose)
          CONSOLE.Debug("ERROR@4: %p m_req_out underflow, resetting", this);
        m_req_out = 0;
      }
      if( request_q.IsEmpty() && RequestPiece() < 0 ) return -1;
      else if( m_req_out < m_req_send &&
               (m_req_out < 2 || !RateDL() ||
                1 >= (m_req_out+1) * request_q.GetRequestLen() /
                     (double)RateDL() - m_latency) &&
               SendRequest() < 0 ){
        // try to allow delay between sending batches of reqs
        return -1;
      }
    }
  }else
    if( m_state.local_interested && SetLocal(BT_MSG_NOT_INTERESTED) < 0 )
      return -1;

  if( !request_q.IsEmpty() ) StartDLTimer();
  else StopDLTimer();
  return 0;
}

void btPeer::CloseConnection()
{
  if(*cfg_verbose) CONSOLE.Debug("%p closed", this);
  if( DT_PEER_FAILED != m_status ){
    m_status = DT_PEER_FAILED;
    StopDLTimer();
    StopULTimer();
    stream.Close();
    PutPending();
  }
  WORLD.DontWaitUL(this);
  WORLD.DontWaitDL(this);
}

int btPeer::HandShake()
{
  char txtid[PEER_ID_LEN*2+3];
  ssize_t r;

  if( (r = stream.Feed()) < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this,
      (r==-2) ? "remote closed" : strerror(errno));
    return -1;
  }
  if( (r = stream.in_buffer.Count()) < 68 ){
    // If it's not BitTorrent, don't wait around for a complete handshake.

    // Report if the 8 reserved bytes following protocol ID differ.
    if( *cfg_verbose && r > 20 &&
        memcmp(stream.in_buffer.BasePointer()+20,
               BTCONTENT.GetShakeBuffer()+20, (r<28) ? (r-20) : 8) != 0 ){
      CONSOLE.Debug_n("");
      CONSOLE.Debug_n("peer %p gave 0x", this);
      for( int i = 20; i < r && i < 27; i++ ){
        CONSOLE.Debug_n("%2.2hx",
          (unsigned short)(unsigned char)stream.in_buffer.BasePointer()[i]);
      }
      CONSOLE.Debug_n(" as reserved bytes (partial)");
    }

    if( r && memcmp(stream.in_buffer.BasePointer(), BTCONTENT.GetShakeBuffer(),
                    (r<48) ? r : 48) != 0 ){
      if(*cfg_verbose){
        CONSOLE.Debug("%p: handshake mismatch", this);
        CONSOLE.Debug_n("");
        CONSOLE.Debug_n("mine: 0x");
        for( int i=0; i < r && i < 48; i++ ){
          CONSOLE.Debug_n("%2.2hx",
            (unsigned short)(unsigned char)BTCONTENT.GetShakeBuffer()[i]);
        }
        CONSOLE.Debug_n("");
        CONSOLE.Debug_n("peer: 0x");
        for( int i=0; i < r && i < 48; i++ ){
          CONSOLE.Debug_n("%2.2hx",
            (unsigned short)(unsigned char)stream.in_buffer.BasePointer()[i]);
        }
        if( r > 48 ){
          TextPeerID((unsigned char *)(stream.in_buffer.BasePointer()+48),
            txtid);
          CONSOLE.Debug("peer is %s", txtid);
        }
      }
      return -1;
    }
    return 0;
  }

  if( memcmp(stream.in_buffer.BasePointer(), BTCONTENT.GetShakeBuffer(), 68)
        == 0 ){
    if(*cfg_verbose) CONSOLE.Debug("peer %p is myself", this);
    if( m_connect && !*cfg_public_ip ) Self.SetIp(m_sin);
    WORLD.AdjustPeersCount();
    return -1;
  }

  // Report if the reserved bytes differ.
  if( *cfg_verbose &&
      memcmp(stream.in_buffer.BasePointer()+20, BTCONTENT.GetShakeBuffer()+20,
             8) != 0 ){
    CONSOLE.Debug_n("");
    CONSOLE.Debug_n("peer %p gave 0x", this);
    for( int i = 20; i < 27; i++ ){
      CONSOLE.Debug_n("%2.2hx",
        (unsigned short)(unsigned char)stream.in_buffer.BasePointer()[i]);
    }
    CONSOLE.Debug_n(" as reserved bytes" );
  }

  // Compare the handshake, ignoring the reserved bytes.
  if( memcmp(stream.in_buffer.BasePointer(),
             BTCONTENT.GetShakeBuffer(), 20) != 0 ||
      memcmp(stream.in_buffer.BasePointer() + 28,
             BTCONTENT.GetShakeBuffer() + 28, 20) != 0 ){
    if(*cfg_verbose){
      CONSOLE.Debug("%p: handshake mismatch", this);
      CONSOLE.Debug_n("");
      CONSOLE.Debug_n("mine: 0x");
      for( int i=0; i < 48; i++ ){
        CONSOLE.Debug_n("%2.2hx",
          (unsigned short)(unsigned char)BTCONTENT.GetShakeBuffer()[i]);
      }
      CONSOLE.Debug_n("");
      CONSOLE.Debug_n("peer: 0x");
      for( int i=0; i < 48; i++ ){
        CONSOLE.Debug_n("%2.2hx",
          (unsigned short)(unsigned char)stream.in_buffer.BasePointer()[i]);
      }
    }
    return -1;
  }

  memcpy(id, stream.in_buffer.BasePointer()+48, PEER_ID_LEN);
  if(*cfg_verbose){
    TextPeerID((unsigned char *)(stream.in_buffer.BasePointer()+48), txtid);
    CONSOLE.Debug("Peer %p ID: %s", this, txtid);
  }

  // ignore peer id verify
  if( !BTCONTENT.pBF->IsEmpty() ){
    char *bf = new char[BTCONTENT.pBF->NBytes()];
#ifndef WINDOWS
    if( !bf ) return -1;
#endif
    BTCONTENT.pBF->WriteToBuffer(bf);
    r = stream.Send_Bitfield(bf, BTCONTENT.pBF->NBytes());
    delete []bf;
  }

  if( r >= 0 ){
    if( stream.in_buffer.PickUp(68) < 0 ) return -1;
    m_status = DT_PEER_SUCCESS;
    m_retried = 0;  // allow reconnect attempt
    // When seeding, new peer starts at the end of the line.
    if( BTCONTENT.Seeding() ){
      // Allow resurrected peer to resume its place in line.
      if( 0 == m_unchoke_timestamp ) m_unchoke_timestamp = now;
      m_connect_seed = 1;
    }
    if( stream.HaveMessage() ) return RecvModule();
  }
  return (r < 0) ? -1 : 0;
}

int btPeer::Send_ShakeInfo()
{
  if( stream.Send_Buffer((char *)BTCONTENT.GetShakeBuffer(), 68) < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  } else return 0;
}

int btPeer::NeedWrite(int limited)
{
  int yn = 0;

  if( stream.out_buffer.Count() )
    yn = 1;                                           // data in buffer to send
  else if( DT_PEER_CONNECTING == m_status )
    yn = 1;                                               // peer is connecting
  else if( WORLD.IsPaused() )
    yn = 0;                                   // paused--no up/download allowed
  else if( !m_state.local_choked && !respond_q.IsEmpty() && !limited )
    yn = 1;                                               // can upload a slice
  else if( !m_state.remote_choked && m_state.local_interested &&
           request_q.IsEmpty() && !m_standby ){
    yn = 1;                                          // can request a new piece
  }else if( request_q.NextSend() && m_req_out < m_req_send &&
            (m_req_out < 2 || !RateDL() ||
             1 >= (m_req_out+1) * request_q.GetRequestLen() / (double)RateDL() -
             m_latency) ){
    yn = 1;                                        // can send a queued request
  }

  return yn;
}

int btPeer::NeedRead(int limited) const
{
  int yn = 1;

  if( DT_PEER_SUCCESS == m_status && stream.PeekMessage(BT_MSG_PIECE) &&
      (!WORLD.IsNextDL(this) || limited) ){
    yn = 0;
  }

  return yn;
}

int btPeer::CouldRespondSlice() const
{
  // If the entire buffer isn't big enough, go ahead and let the put resize it.
  if( !m_state.local_choked &&
      (stream.out_buffer.LeftSize() >=
                   BT_LEN_PRE + BT_MSGLEN_PIECE + respond_q.GetRequestLen() ||
       stream.out_buffer.Count() + stream.out_buffer.LeftSize() <
                   BT_LEN_PRE + BT_MSGLEN_PIECE + respond_q.GetRequestLen()) ){
    return 1;
  }else return 0;
}

int btPeer::AreYouOK()
{
  m_f_keepalive = 1;
  if( stream.Send_Keepalive() < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  }else return 0;
}

int btPeer::RecvModule()
{
  ssize_t r = 0;

  if( stream.PeekMessage(BT_MSG_PIECE) ){
    if( WORLD.IsNextDL(this) ){
      int limited = WORLD.BandwidthLimitDown(Self.LateDL());
      if( !limited ){
        WORLD.DontWaitDL(this);
        r = stream.Feed(&rate_dl);  // feed full amount (can download)
//      if( r>=0 ) CONSOLE.Debug("%p fed piece, now has %d bytes", this, r);
        Self.OntimeDL(0);
      }else{
        if(*cfg_verbose) CONSOLE.Debug("%p waiting for DL bandwidth", this);
        WORLD.WaitDL(this);
      }
    }else{  // deferring DL, unless limited.
      if(*cfg_verbose)
        CONSOLE.Debug("%p deferring or waiting for DL bandwidth", this);
      WORLD.WaitDL(this);
    }
//  m_deferred_dl = 0;  // not used
  }else if( !stream.HaveMessage() ){  // could have been called post-handshake
    r = stream.Feed(BUF_DEF_SIZ, &rate_dl);
//  if( r>=0 ) CONSOLE.Debug("%p fed, now has %d bytes (msg=%d)",
//    this, r, (int)stream.PeekMessage());
  }
  if( r < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this,
      (r==-2) ? "remote closed" : strerror(errno));
    return -1;
  }

  while( (r = stream.HaveMessage()) ){
    if( r < 0 ) return -1;
    if( (r = MsgDeliver()) == -2 ){
      if(*cfg_verbose) CONSOLE.Debug("%p seed<->seed detected", this);
      m_want_again = 0;
    }
    if( r < 0 || stream.PickMessage() < 0 ) return -1;
  }

  return 0;
}

int btPeer::SendModule()
{
  int f_flushed = 0;

  if( !m_state.remote_choked && RequestCheck() < 0 )
    return -1;

  /* Don't want to send HAVEs unless we're sending something else too.  So
     insure there's data present (here), or there's about to be (below). */
  if( stream.out_buffer.Count() &&
      m_haveq[0] < BTCONTENT.GetNPieces() && SendHaves() < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  }

  if( stream.out_buffer.Count() &&
      !respond_q.IsEmpty() && !CouldRespondSlice() ){
    if( stream.Flush() < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
    f_flushed = 1;
  }

  if( !respond_q.IsEmpty() && CouldRespondSlice() ){
    int limited = WORLD.BandwidthLimitUp(Self.LateUL());
    if( WORLD.IsNextUL(this) ){
      if( !limited ){
        WORLD.DontWaitUL(this);
        StartULTimer();
        Self.StartULTimer();
        if( m_haveq[0] < BTCONTENT.GetNPieces() && SendHaves() < 0 ) return -1;
        if( RespondSlice() < 0 ) return -1;
        f_flushed = 1;
        Self.OntimeUL(0);
      }else{
        if(*cfg_verbose) CONSOLE.Debug("%p waiting for UL bandwidth", this);
        WORLD.WaitUL(this);
      }
    }else{
      if( !limited ){
        if(*cfg_verbose)
          CONSOLE.Debug("%p deferring UL to %p", this, WORLD.GetNextUL());
        WORLD.GetNextUL()->DeferUL();
        WORLD.Defer();
      }else if(*cfg_verbose) CONSOLE.Debug("%p waiting for UL bandwidth", this);
      WORLD.WaitUL(this);
    }
    m_deferred_ul = 0;
  }else if( this == WORLD.GetNextUL() ) WORLD.DontWaitUL(this);

  if( !f_flushed && stream.Flush() < 0 ){
    if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
    return -1;
  }else return 0;
}

int btPeer::SendHaves()
{
  for( int i=0; i < HAVEQ_SIZE && m_haveq[i] < BTCONTENT.GetNPieces(); i++ ){
    if( stream.Send_Have(m_haveq[i]) < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
    m_haveq[i] = BTCONTENT.GetNPieces();
  }
  return 0;
}

int btPeer::QueueHave(bt_index_t idx)
{
  if( m_haveq[HAVEQ_SIZE - 1] < BTCONTENT.GetNPieces() ){
    if( SendHaves() < 0 ) return -1;
    if( stream.Send_Have(idx) < 0 ){
      if(*cfg_verbose) CONSOLE.Debug("%p: %s", this, strerror(errno));
      return -1;
    }
  }else{
    int i;
    for( i=0; m_haveq[i] < BTCONTENT.GetNPieces(); i++ );
    m_haveq[i] = idx;
  }
  return 0;
}

// Prevent a peer object from holding the queue when it's not ready to write.
void btPeer::CheckSendStatus()
{
  if( m_deferred_ul ){
    if(*cfg_verbose) CONSOLE.Debug("%p skipped UL", this);
    WORLD.ReQueueUL(this);
    m_deferred_ul = 0;
  }
}

/* Detect if a peer ignored, discarded, or lost my request and we're waiting
   for a piece that may never arrive. */
int btPeer::HealthCheck()
{
  if( BTCONTENT.IsFull() ){
    /* Catch seeders who suppress HAVE and don't disconnect other seeders,
       or who just sit there and waste a connection. */
    if( m_health_time <= now - 300 ){
      m_health_time = now;
      if( !m_state.remote_interested ){
        if( m_bad_health ) return -1;
        m_bad_health = 1;
      }else m_bad_health = 0;
    }
  }else if( m_health_time <= now - 60 ){
    m_health_time = now;
    if( !m_state.remote_choked && m_req_out ){
      time_t allowance = !m_latency ? 150 : ((m_latency < 60) ? 60 : m_latency);
      if( m_receive_time < now - 2*allowance ){
        // if a repeat occurrence, get rid of the peer
        if( m_bad_health || PeerError(2, "unresponsive") < 0 ) return -1;
        m_bad_health = 1;
        if(*cfg_verbose)
          CONSOLE.Debug("%p unresponsive; resetting request queue", this);
        int retval = CancelRequest();
        PutPending();
        return ( retval < 0 ) ? -1 : 0;
      }else if( m_receive_time < now - allowance ){
        CONSOLE.Debug("%p unresponsive; sending keepalive", this);
        AreYouOK();  // keepalive--may stimulate the connection
      }else m_bad_health = 0;
    }else m_bad_health = 0;
  }
  return 0;
}

/* This handles peers that suppress HAVE messages so that we don't always think
   that they're empty.  If we've sent the peer an amount of data equivalent to
   two pieces, assume that they now have at least one complete piece. */
int btPeer::IsEmpty() const
{
  return ( bitfield.IsEmpty() && TotalUL() < BTCONTENT.GetPieceLength()*2 ) ?
    1:0;
}

void btPeer::PutPending()
{
  if( !request_q.IsEmpty() ){
    if( PENDINGQUEUE.Pending(&request_q) != 0 )
      WORLD.RecalcDupReqs();
    WORLD.UnStandby();
  }
  m_req_out = 0;
}

int btPeer::NeedPrefetch() const
{
  if( DT_PEER_SUCCESS == m_status &&
      ( Is_Local_Unchoked() ||
        (!BTCONTENT.IsFull() && Is_Remote_Unchoked() &&
         m_prefetch_completion < 2 && request_q.LastSlice()) ) ){
    return 1;
  }else return 0;
}

/* Call NeedPrefetch() first, which checks additional conditions!
   Returns 1 if triggered disk activity, 0 otherwise */
int btPeer::Prefetch(time_t deadline)
{
  int retval = 0;
  bt_index_t idx;
  bt_offset_t off;
  bt_length_t len;
  time_t predict, next_chance;

  if( !BTCONTENT.IsFull() && Is_Remote_Unchoked() &&
      m_prefetch_completion < 2 && request_q.LastSlice() && RateDL() > 0 &&
      request_q.Peek(&idx, &off, &len)==0 &&
      m_last_timestamp + (time_t)(len / RateDL()) <
        now + WORLD.GetUnchokeInterval() &&
      Self.RateDL() > 0 &&
      m_last_timestamp + len / RateDL() <
        now + ((*cfg_cache_size)*1024U*1024U - BTCONTENT.GetPieceLength(idx)) /
              Self.RateDL() ){
    switch( BTCONTENT.CachePrep(idx) ){
    case -1:  // don't prefetch
      m_prefetch_completion = 2;
      break;
    case 0:  // ready, no data flushed
      if( m_prefetch_completion || off==0 ){
        if( off+len < BTCONTENT.GetPieceLength(idx) )
          BTCONTENT.ReadSlice(NULL, idx, off+len,
            BTCONTENT.GetPieceLength(idx)-off-len);
        m_prefetch_completion = 2;
      }else{
        retval = BTCONTENT.ReadSlice(NULL, idx, 0, off);
        if( off+len < BTCONTENT.GetPieceLength(idx) )
          m_prefetch_completion = 1;
        else m_prefetch_completion = 2;
      }
      break;
    case 1:  // data was flushed (time used)
      retval = 1;
      break;
    }
  }
  else if( Is_Local_Unchoked() && respond_q.Peek(&idx, &off, &len) == 0 ){
    if( *cfg_max_bandwidth_up > 0 )
      next_chance = (time_t)(Self.LastSendTime() +
                           (double)Self.LastSizeSent() / *cfg_max_bandwidth_up);
    else next_chance = now;

    if( WORLD.GetNextUL() ){
      if( WORLD.IsNextUL(this) ){
        m_next_send_time = next_chance;  // I am the next sender
      }else{
        // deferral pending; we'll get another chance to prefetch
        return 0;
      }
    }
    if( m_next_send_time < next_chance ) predict = next_chance;
    else predict = m_next_send_time;

    // Don't prefetch if it will expire from cache before being sent.
    if( predict < deadline &&
        (0==Self.RateDL() || predict <=
                now + (time_t)(*cfg_cache_size*1024U*1024U / Self.RateDL())) ){
      // This allows re-prefetch if it might have expired from the cache.
      if( !m_prefetch_time || (0==Self.RateDL() && 0==Self.RateUL()) ||
          now - m_prefetch_time >
            (time_t)(BTCONTENT.CacheSize() / (Self.RateDL() + Self.RateUL())) ){
        retval = BTCONTENT.ReadSlice(NULL, idx, off, len);
        m_prefetch_time = now;
      }
    }
  }
  return retval;
}

int btPeer::PeerError(int weight, const char *message)
{
  int old_count = m_err_count;

  m_err_count += weight;
  if( m_err_count < 0 ) m_err_count = 0;
  if( *cfg_verbose && (weight > 0 || old_count > 0) )
    CONSOLE.Debug("%p error %+d (%d) %s", this, weight, m_err_count, message);

  if( m_err_count >= 16 ){
    m_want_again = 0;
    return -1;
  }else return 0;
}

void btPeer::Dump() const
{
  struct sockaddr_in sin;

  GetAddress(&sin);
  CONSOLE.Print("%s: %d -> %d:%d   %llud:%lluu", inet_ntoa(sin.sin_addr),
    (int)bitfield.Count(),
    Is_Remote_Unchoked() ? 1 : 0,
    request_q.IsEmpty() ? 0 : 1,
    (unsigned long long)TotalDL(),
    (unsigned long long)TotalUL());
}

