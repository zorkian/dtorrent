#include "peer.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "btstream.h"
#include "./btcontent.h"
#include "./msgencode.h"
#include "./peerlist.h"
#include "./btconfig.h"

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

btBasic Self;

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
  time(&m_last_timestamp);
  m_state.remote_choked = m_state.local_choked = 1;
  m_state.remote_interested = m_state.local_interested = 0;

  m_err_count = 0;
  m_cached_idx = BTCONTENT.GetNPieces();
  m_standby = 0;
}

int btPeer::SetLocal(unsigned char s)
{
  switch(s){
  case M_CHOKE:
    if( m_state.local_choked ) return 0;
    time(&m_unchoke_timestamp);
//  if(arg_verbose) fprintf(stderr, "Choking %p\n", this);
    if(arg_verbose) fprintf(stderr, "Choking %p (D=%lluMB@%uK/s)\n", this,
      TotalDL() >> 20, RateDL() >> 10);
    m_state.local_choked = 1; 
    break;
  case M_UNCHOKE: 
    if( !reponse_q.IsEmpty() ) StartULTimer();
    if( !m_state.local_choked ) return 0;
    time(&m_unchoke_timestamp);
//  if(arg_verbose) fprintf(stderr, "Unchoking %p\n", this);
    if(arg_verbose) fprintf(stderr, "Unchoking %p (D=%lluMB@%uK/s)\n", this,
      TotalDL() >> 20, RateDL() >> 10);
    m_state.local_choked = 0;
    break;
  case M_INTERESTED: 
    m_standby = 0;
    if( m_state.local_interested ) return 0;
    if(arg_verbose) fprintf(stderr, "Interested in %p\n", this);
    m_state.local_interested = 1;
    break;
  case M_NOT_INTERESTED:
    if( !m_state.local_interested ) return 0;
    if(arg_verbose) fprintf(stderr, "Not interested in %p\n", this);
    m_state.local_interested = 0; 
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

  PENDINGQUEUE.ReAssign(&request_q,bitfield);

  if( !request_q.IsEmpty() ) return SendRequest();

  if( m_cached_idx < BTCONTENT.GetNPieces() && !BTCONTENT.pBF->IsEmpty() ){
    // A HAVE msg already selected what we want from this peer
    // but ignore it in initial-piece mode.
    idx = m_cached_idx;
    m_cached_idx = BTCONTENT.GetNPieces();
    if( !BTCONTENT.pBF->IsSet(idx) &&
	!PENDINGQUEUE.Exist(idx) &&
	!WORLD.AlreadyRequested(idx) ){
      return (request_q.CreateWithIdx(idx) < 0) ? -1 : SendRequest();
    }
  }	// If we didn't want the cached piece, select another.
  if( BTCONTENT.pBF->IsEmpty() ){
    // If we don't have a complete piece yet, try to get one that's already
    // in progress.  (Initial-piece mode)
    btPeer *peer = WORLD.Who_Can_Duplicate(this, BTCONTENT.GetNPieces());
    if(peer){
      if(arg_verbose) fprintf( stderr, "Duping: %p to %p (#%u)\n",
        peer, this, peer->request_q.GetRequestIdx() );
      return (request_q.CopyShuffle(peer->request_q) < 0) ? -1 : SendRequest();
    }
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
        endgame = ( WORLD.Pieces_I_Can_Get() - BTCONTENT.pBF->Count() )
            < WORLD.TotalPeers();
        if(endgame){	// OK to duplicate a request.
//	  idx = tmpBitField.Random();
	  idx = 0;	// flag for Who_Can_Duplicate()
	  btPeer *peer = WORLD.Who_Can_Duplicate(this, idx);
	  if(arg_verbose) fprintf( stderr, "Duping: %p to %p (#%u)\n",
	    peer, this, peer->request_q.GetRequestIdx() );
	  return (request_q.CopyShuffle(peer->request_q) < 0) ?
	     -1 : SendRequest();
        }else{	// not endgame mode
	  btPeer *peer = WORLD.Who_Can_Abandon(this); // slowest choice
	  if(peer){
	    // Cancel a request to the slowest peer & request it from this one.
	    if(arg_verbose) fprintf( stderr, "Reassigning %p to %p (#%u)\n",
	      peer, this, peer->request_q.GetRequestIdx() );
	    peer->StopDLTimer();
	    // RequestQueue class "moves" rather than "copies" in assignment!
	    request_q = peer->request_q;

	    if(peer->CancelRequest(request_q.GetHead()) < 0 ||
	        peer->RequestCheck() < 0){
	      peer->CloseConnection();
	    }
	    return SendRequest();
	  }else m_standby = 1;	// nothing to do at the moment
        }
      }else{
        // Request something that we haven't requested yet (most common case).
        idx = tmpBitField2.Random();
        return (request_q.CreateWithIdx(idx) < 0) ? -1 : SendRequest();
      }
    } else {
      // We don't need anything from the peer.  How'd we get here?
      return SetLocal(M_NOT_INTERESTED);
    }
  return 0;
}

int btPeer::MsgDeliver()
{
  size_t r,idx,off,len;

  char *msgbuf = stream.in_buffer.BasePointer();

  r = get_nl(msgbuf);

  // Don't require keepalives if we're receiving other messages.
  time(&m_last_timestamp);
  if( 0 == r ){
    if( !m_f_keepalive ) if( stream.Send_Keepalive() < 0 ) return -1;
    m_f_keepalive = 0;
    return 0;
  }else{
    switch(msgbuf[4]){
    case M_CHOKE:
      if(H_BASE_LEN != r){ return -1;}
      if(arg_verbose) fprintf(stderr, "%p choked me\n", this);
      m_state.remote_choked = 1;
      StopDLTimer();
      if( !request_q.IsEmpty()){
	PSLICE ps = request_q.GetHead();
	if( !PENDINGQUEUE.Exist(request_q.GetRequestIdx()) )
	  PENDINGQUEUE.Pending(&request_q);
	if( CancelRequest(ps) < 0) return -1;
      }
      return 0;

    case M_UNCHOKE:
      if(H_BASE_LEN != r){return -1;}
      if(arg_verbose) fprintf(stderr, "%p unchoked me\n", this);
      m_state.remote_choked = 0;
      if(!request_q.IsEmpty())	// shouldn't happen; maybe peer is confused.
        return SendRequest();
      return RequestCheck();

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
      return 0;

    case M_HAVE:
      if(H_HAVE_LEN != r){return -1;}

      idx = get_nl(msgbuf + 5);

      if( idx >= BTCONTENT.GetNPieces() || bitfield.IsSet(idx)) return -1;

      bitfield.Set(idx);

      if( bitfield.IsFull() && BTCONTENT.pBF->IsFull() ){ return -2; }

      if( !BTCONTENT.pBF->IsSet(idx) && !BTCONTENT.pBFilter->IsSet(idx) ){
        m_cached_idx = idx;
        m_standby = 0;
      }
      //      if( !BTCONTENT.pBF->IsSet(idx) ) m_cached_idx = idx;
      
      // see if we're Interested now
      return request_q.IsEmpty() ? RequestCheck() : 0;

    case M_REQUEST:
      if(H_REQUEST_LEN != r || !m_state.remote_interested){ return -1; }

      idx = get_nl(msgbuf + 5);
      
      if( !BTCONTENT.pBF->IsSet(idx) ) return -1;
      
      off = get_nl(msgbuf + 9);
      len = get_nl(msgbuf + 13);

      if( !reponse_q.IsValidRequest(idx, off, len) ) return -1;
      
      return reponse_q.Add(idx, off, len);

    case M_PIECE:
      if( request_q.IsEmpty() || !m_state.local_interested){
	m_err_count++;
	if(arg_verbose) fprintf(stderr,"err: %p (%d) Unwanted piece\n",
	  this, m_err_count);
	return 0;
      }
      return PieceDeliver(r);

    case M_BITFIELD:
      if( (r - 1) != bitfield.NBytes() || !bitfield.IsEmpty()) return -1;
      bitfield.SetReferBuffer(msgbuf + 5);
      if(bitfield.IsFull() && BTCONTENT.pBF->IsFull()) return -2;

      //This is needed in order to set our Interested state
      return RequestCheck(); // fixed client stall

    case M_CANCEL:
      if(r != H_CANCEL_LEN || !m_state.remote_interested) return -1;

      idx = get_nl(msgbuf + 5);
      off = get_nl(msgbuf + 9);
      len = get_nl(msgbuf + 13);
      if( reponse_q.Remove(idx,off,len) < 0 ){
	m_err_count++;
	if(arg_verbose) fprintf(stderr, "err: %p (%d) Bad cancel\n",
	  this, m_err_count);
	return 0;
      }
      if( reponse_q.IsEmpty() ) StopULTimer();
      return 0;
    default:
      if(arg_verbose) fprintf(stderr, "Unknown message type %u from peer %p\n",
        msgbuf[4], this);
      return 0;	// ignore unknown message & continue (forward compatibility)
    }
  }
  return 0;
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
    return stream.Send_Piece(idx,off,BTCONTENT.global_piece_buffer,len);
  }

  return 0;
}

int btPeer::SendRequest()
{
  PSLICE ps = request_q.GetHead();
  if(arg_verbose) fprintf(stderr, "Requesting #%u from %p:",
    request_q.GetRequestIdx(), this);
  for( ; ps ; ps = ps->next ){
    if(arg_verbose) fprintf(stderr, ".");
    if(stream.Send_Request(ps->index,ps->offset,ps->length) < 0){ return -1; }
  }
  if(arg_verbose) fprintf(stderr, "\n");

  return stream.Flush();
}

int btPeer::CancelRequest(PSLICE ps)
{
  for( ; ps; ps = ps->next){
    if(stream.Send_Cancel(ps->index,ps->offset,ps->length) < 0)
      return -1;
  }
  return stream.Flush();
}

int btPeer::CancelSliceRequest(size_t idx, size_t off, size_t len)
{
  PSLICE ps;

  for(ps = request_q.GetHead() ; ps; ps = ps->next){
    if( idx == ps->index && off == ps->offset && len == ps->length ){
      if( request_q.Remove(idx,off,len) < 0 ){
        m_err_count++;
        if(arg_verbose) fprintf(stderr,"err: %p (%d) Bad CS remove\n",
          this, m_err_count);
      }
      if(stream.Send_Cancel(idx,off,len) < 0)
        return -1;
      return stream.Flush();
    }
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

  idx = get_nl(msgbuf + 5);
  off = get_nl(msgbuf + 9);
  len = mlen - 9;

  if( request_q.Remove(idx,off,len) < 0 ){
    m_err_count++;
    if(arg_verbose) fprintf(stderr, "err: %p (%d) Bad remove\n",
      this, m_err_count);
    return 0;
  }

  if(BTCONTENT.WriteSlice((char*)(msgbuf + 13),idx,off,len) < 0){
    return 0;
  }

  Self.StartDLTimer();
  Self.DataRecved(len);
  DataRecved(len);

  // Check for & cancel requests for this slice from other peers in initial
  // and endgame modes.
  if( BTCONTENT.pBF->Count() < 2 ||
      WORLD.Pieces_I_Can_Get() - BTCONTENT.pBF->Count() < WORLD.TotalPeers() ){
    WORLD.CancelSlice(idx, off, len);
    PENDINGQUEUE.DeleteSlice(idx, off, len);
  }

  /* if piece download complete. */
  return request_q.IsEmpty() ? ReportComplete(idx) : 0;
}

int btPeer::RequestCheck()
{
  if( BandWidthLimitDown() ) return 0;
  
  if( BTCONTENT.pBF->IsFull() ){
    if( bitfield.IsFull() ){ return -1; }
    return SetLocal(M_NOT_INTERESTED);
  }

  if( Need_Remote_Data() ){
    if(!m_state.local_interested && SetLocal(M_INTERESTED) < 0) return -1;
    if(request_q.IsEmpty() && !m_state.remote_choked){
      if( RequestPiece() < 0 ) return -1;
    }
  } else
    if(m_state.local_interested && SetLocal(M_NOT_INTERESTED) < 0) return -1;
  
  if(!request_q.IsEmpty()) StartDLTimer();
  return 0;
}

void btPeer::CloseConnection()
{
  if(arg_verbose) fprintf(stderr, "%p closed\n", this);
  if( P_FAILED != m_status ){
    m_status = P_FAILED;
    stream.Close();
  }
}

int btPeer::HandShake()
{
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
          if( r>48 ) fprintf( stderr, "\npeer %p gave 0x", this);
          else fprintf( stderr, "\npeer gave 0x" );
          for(int i=20; i<r && i<27; i++) fprintf(stderr, "%2.2hx",
            (u_short)(u_char)(stream.in_buffer.BasePointer()[i]));
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
          (u_short)(u_char)(BTCONTENT.GetShakeBuffer()[i]));
        fprintf(stderr, "\npeer: 0x");
        for(int i=0; i<r && i<48; i++) fprintf(stderr, "%2.2hx",
          (u_short)(u_char)(stream.in_buffer.BasePointer()[i]));
        fprintf(stderr, "\n");
        fprintf(stderr, "peer is %.8s\n", stream.in_buffer.BasePointer()+48);
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
        (u_short)(u_char)(stream.in_buffer.BasePointer()[i]));
      fprintf( stderr, " as reserved bytes\n" );
    }
    memcpy(stream.in_buffer.BasePointer()+20, BTCONTENT.GetShakeBuffer()+20, 8);
  }
  if( memcmp(stream.in_buffer.BasePointer(),BTCONTENT.GetShakeBuffer(),48) != 0 ){
    if(arg_verbose){
      fprintf(stderr, "\nmine: 0x");
      for(int i=0; i<48; i++) fprintf(stderr, "%2.2hx",
        (u_short)(u_char)(BTCONTENT.GetShakeBuffer()[i]));
      fprintf(stderr, "\npeer: 0x");
      for(int i=0; i<48; i++) fprintf(stderr, "%2.2hx",
        (u_short)(u_char)(stream.in_buffer.BasePointer()[i]));
      fprintf(stderr, "\n");
    }
    return -1;
  }

  if(arg_verbose){
    fprintf(stderr, "Peer %p ID: ", this);
    for(int i=48; i<60; i++){
      if( isprint(stream.in_buffer.BasePointer()[i]) )
        fprintf(stderr, "%c", stream.in_buffer.BasePointer()[i]);
      else break;
    }
    fprintf(stderr, "\n");
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
  if( cfg_max_bandwidth_down <= 0 ) return 0;
  return ((Self.RateDL()) >= cfg_max_bandwidth_down) ?
    1:0;
}

int btPeer::NeedWrite()
{
  int yn = 0;
  if( stream.out_buffer.Count() || // data need send in buffer.
      (!reponse_q.IsEmpty() && CouldReponseSlice() && !  BandWidthLimitUp()) ||
      ( !m_state.remote_choked && request_q.IsEmpty()
	    && m_state.local_interested
	    && !BandWidthLimitDown() && !m_standby ) ||	// can request a piece.
      P_CONNECTING == m_status ) // peer is connecting
    yn = 1;
  return yn;
}

int btPeer::NeedRead()
{
  int yn = 1;
  if( !request_q.IsEmpty() && BandWidthLimitDown() )
    yn = 0;
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
  
  r = stream.Feed();

  if( r < 0 && r != -2 )
    return -1;
  else if ( r == -2 )
    f_peer_closed = 1;
  
  r = stream.HaveMessage();
  for( ; r;){
    if( r < 0 ) return -1;
    if(MsgDeliver() < 0 || stream.PickMessage() < 0) return -1;
    r = stream.HaveMessage();
  }
  return f_peer_closed ? -1 : 0;
}

int btPeer::SendModule()
{
  if( stream.out_buffer.Count() && stream.Flush() < 0) return -1;

  if( !reponse_q.IsEmpty() && CouldReponseSlice() && !BandWidthLimitUp() ) {
    StartULTimer();
    Self.StartULTimer();
  }

  for(; !reponse_q.IsEmpty() && CouldReponseSlice() && !BandWidthLimitUp(); )
    if( ReponseSlice() < 0) return -1;

  return (!m_state.remote_choked && request_q.IsEmpty()) ? RequestCheck() : 0;
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

