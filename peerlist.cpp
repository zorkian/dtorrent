#include <sys/types.h>

#include "peerlist.h"

#include <stdlib.h>

#include <stdio.h>
#include <errno.h>

#include <string.h>

#include "btconfig.h"
#include "connect_nonb.h"
#include "setnonblock.h"
#include "btcontent.h"
#include "msgencode.h"

#include "iplist.h"
#include "tracker.h"
#include "ctcs.h"
#include "bttime.h"


#define MAX_UNCHOKE 3
#define UNCHOKE_INTERVAL 10

#define OPT_INTERVAL 30

#define KEEPALIVE_INTERVAL 117

#define LISTEN_PORT_MAX 2706
#define LISTEN_PORT_MIN 2106

#define PEER_IS_SUCCESS(peer) (P_SUCCESS == (peer)->GetStatus())
#define PEER_IS_FAILED(peer) (P_FAILED == (peer)->GetStatus())
#define NEED_MORE_PEERS() (m_peers_count < cfg_max_peers)

const char LIVE_CHAR[4] = {'-', '\\','|','/'};

PeerList WORLD;

PeerList::PeerList()
{
  m_unchoke_check_timestamp =
    m_keepalive_check_timestamp =
    m_opt_timestamp = time((time_t*) 0);

  m_head = (PEERNODE*) 0;
  m_listen_sock = INVALID_SOCKET;
  m_peers_count = m_seeds_count = 0;
  m_live_idx = 0;
  m_seed_only = 0;
}

PeerList::~PeerList()
{
  PEERNODE *p,*pnext;
  for(p = m_head; p ; ){
    pnext = p->next;
    delete p->peer;
    delete p;
    p = pnext;
  }
}

int PeerList::IsEmpty() const
{
  return m_peers_count ? 0 : 1;
}

void PeerList::CloseAll()
{
  PEERNODE *p;
  for(p = m_head; p;){
    m_head = p->next;
    delete (p->peer);
    delete p;
    p = m_head;
  }
}

int PeerList::NewPeer(struct sockaddr_in addr, SOCKET sk)
{
  PEERNODE *p;
  btPeer *peer = (btPeer*) 0;
  int r;

  if( m_peers_count >= cfg_max_peers ){
    if( INVALID_SOCKET != sk ) CLOSE_SOCKET(sk);
    return -4;
  }
  
  if( Self.IpEquiv(addr) ){ 
    if(INVALID_SOCKET != sk) CLOSE_SOCKET(sk); return -3;} // myself

  for(p = m_head; p; p = p->next){
    if(PEER_IS_FAILED(p->peer)) continue;
    if( p->peer->IpEquiv(addr)){  // already exist.
      if( INVALID_SOCKET != sk) CLOSE_SOCKET(sk); 
      return -3;
    }
  }
  
  if( INVALID_SOCKET == sk ){
    if( INVALID_SOCKET == (sk = socket(AF_INET,SOCK_STREAM,0)) ) return -1;
    
    if( setfd_nonblock(sk) < 0) goto err;

    if(arg_verbose) fprintf(stderr, "Connecting to %s:%hu\n",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    if( -1 == (r = connect_nonb(sk,(struct sockaddr*)&addr)) ) return -1;

    peer = new btPeer;

#ifndef WINDOWS
    if( !peer ) goto err;
#endif

    peer->SetConnect();
    peer->SetAddress(addr);
    peer->stream.SetSocket(sk);
    peer->SetStatus( (-2 == r) ? P_CONNECTING : P_HANDSHAKE );

  }else{
    if( setfd_nonblock(sk) < 0) goto err;

    peer = new btPeer;

#ifndef WINDOWS
    if( !peer ) goto err;
#endif

    peer->SetAddress(addr);
    peer->stream.SetSocket(sk);
    peer->SetStatus(P_HANDSHAKE);
  }

  if( P_HANDSHAKE == peer->GetStatus() )
    if( peer->Send_ShakeInfo() != 0 ) { delete peer; return -1; }

  p = new PEERNODE;
#ifndef WINDOWS
  if( !p ){ delete peer; return -1;}
#endif
  m_peers_count++;

  p->peer = peer;
  
  p->next = m_head;
  m_head = p;

  return 0;
 err:
  CLOSE_SOCKET(sk);
  return -1;
}

int PeerList::FillFDSET(const time_t *pnow,fd_set *rfdp,fd_set *wfdp)
{
  PEERNODE *p;
  PEERNODE *pp = (PEERNODE*) 0;
  int f_keepalive_check = 0;
  int f_unchoke_check = 0;
  int maxfd = -1;
  int i = 0;
  SOCKET sk = INVALID_SOCKET;
  struct sockaddr_in addr;
  btPeer * UNCHOKER[MAX_UNCHOKE + 1];
  
  if( !Tracker.IsPaused() && !Tracker.IsQuitting() )
    for( ;NEED_MORE_PEERS() && !IPQUEUE.IsEmpty(); ){
      if(IPQUEUE.Pop(&addr) < 0) break;
      if(NewPeer(addr,INVALID_SOCKET) == -4) break;
    }

  Self.SetCurrentRates();

  // show status line.
  if( m_pre_dlrate.TimeUsed(pnow) ){
    char partial[30] = "";
    if(arg_file_to_download){
      BitField tmpBitField =  *BTCONTENT.pBF;
      tmpBitField.Except(*BTCONTENT.pBFilter);
      sprintf( partial, "P:%u/%u ", 
        tmpBitField.Count(),
        BTCONTENT.getFilePieces(arg_file_to_download) );
    }
    printf("\r                                                                               ");
    printf("\r%c %u/%u/%u [%u/%u/%u] %lluMB,%lluMB | %u,%uK/s | %u,%uK E:%u,%u %s%s ",
           LIVE_CHAR[m_live_idx],

           m_seeds_count,
           m_peers_count - m_seeds_count,
           Tracker.GetPeersCount(),

           BTCONTENT.pBF->Count(),
           BTCONTENT.pBF->NBits(),
           Pieces_I_Can_Get(),

           Self.TotalDL() >> 20, Self.TotalUL() >> 20,

           Self.RateDL() >> 10, Self.RateUL() >> 10,

           m_pre_dlrate.RateMeasure(Self.GetDLRate()) >> 10,
           m_pre_ulrate.RateMeasure(Self.GetULRate()) >> 10,

           Tracker.GetRefuseClick(),
           Tracker.GetOkClick(),

           partial,

           (Tracker.GetStatus()==T_CONNECTING) ? "Connecting" :
             ( (Tracker.GetStatus()==T_READY) ? "Connected" :
                 (Tracker.IsPaused() ?
                  ((Tracker.GetStatus()==T_FINISHED) ? "Paused" : "Pausing") :
                  (Tracker.IsQuitting() ? "Quitting" : "")) )
    );
    fflush(stdout);
    m_pre_dlrate = Self.GetDLRate();
    m_pre_ulrate = Self.GetULRate();
    m_live_idx++;

    if(arg_ctcs) CTCS.Report_Status(
           m_seeds_count, m_peers_count - m_seeds_count,
           BTCONTENT.pBF->Count(), BTCONTENT.pBF->NBits(), Pieces_I_Can_Get(),
           Self.RateDL(), Self.RateUL(),
           Self.TotalDL(), Self.TotalUL(),
           cfg_max_bandwidth_down, cfg_max_bandwidth_up);

    /* after seeding for a while, disconnect uninterested peers */
    if( BTCONTENT.GetSeedTime() - now <= 301 &&
        BTCONTENT.GetSeedTime() - now >= 300 )
      CloseAllConnectionToSeed();
  }
    
  if(KEEPALIVE_INTERVAL <= (*pnow - m_keepalive_check_timestamp)){
    m_keepalive_check_timestamp = *pnow;
    f_keepalive_check = 1;
  }

  if(UNCHOKE_INTERVAL <= (*pnow - m_unchoke_check_timestamp)){
    
    m_unchoke_check_timestamp = *pnow;
    f_unchoke_check = 1;
    
  }

  if( f_unchoke_check ) {
    memset(UNCHOKER, 0, (MAX_UNCHOKE + 1) * sizeof(btPeer*));
    if (OPT_INTERVAL <= *pnow - m_opt_timestamp) m_opt_timestamp = 0;
  }

  m_seeds_count = 0;
  for(p = m_head; p;){
    if( PEER_IS_FAILED(p->peer)){
      if( p->peer->WantAgain() ){ // connect to this peer again
        if(arg_verbose) fprintf(stderr, "Adding %p for reconnect\n", p->peer);
        struct sockaddr_in addr;
        p->peer->GetAddress(&addr);
        IPQUEUE.Add(&addr);
      }
      if( pp ) pp->next = p->next; else m_head = p->next;
      delete p->peer;
      delete p;
      m_peers_count--;
      if( pp ) p = pp->next; else p = m_head;
      continue;
    }else{
      if (p->peer->bitfield.IsFull()) m_seeds_count++;
      if( f_keepalive_check ){

        if(3 * KEEPALIVE_INTERVAL <= (*pnow - p->peer->GetLastTimestamp())){
          if(arg_verbose) fprintf(stderr, "close: keepalive expired\n");
          p->peer->CloseConnection();
          goto skip_continue;
        }
        
        if(PEER_IS_SUCCESS(p->peer) && 
           KEEPALIVE_INTERVAL <= (*pnow - p->peer->GetLastTimestamp()) &&
           p->peer->AreYouOK() < 0){
          if(arg_verbose) fprintf(stderr, "close: keepalive death\n");
          p->peer->CloseConnection();
          goto skip_continue;
        }
      }

      if( f_unchoke_check && PEER_IS_SUCCESS(p->peer) ){

        if( p->peer->Is_Remote_Interested() && p->peer->Need_Local_Data() )
            UnChokeCheck(p->peer, UNCHOKER);
        else if(p->peer->SetLocal(M_CHOKE) < 0){
          if(arg_verbose) fprintf(stderr, "close: Can't choke peer\n");
          p->peer->CloseConnection();
          goto skip_continue;
        }
      }

      sk = p->peer->stream.GetSocket();
      if(maxfd < sk) maxfd = sk;
      if( p->peer->NeedRead() ) FD_SET(sk,rfdp);

      if( p->peer->NeedWrite() ) FD_SET(sk,wfdp);
    skip_continue: 
      pp = p;
      p = p->next;
    }
  } // end for
  

  if( INVALID_SOCKET != m_listen_sock && m_peers_count < cfg_max_peers){
    FD_SET(m_listen_sock, rfdp);
    if( maxfd < m_listen_sock ) maxfd = m_listen_sock;
  }

  if( f_unchoke_check ){
    if(arg_verbose) fprintf(stderr, "\nUnchoker ");
    if (!m_opt_timestamp){
      if(arg_verbose) fprintf(stderr, "(opt) ");
      m_opt_timestamp = *pnow;
    }
    for( i = 0; i < MAX_UNCHOKE + 1; i++){

      if( (btPeer*) 0 == UNCHOKER[i]) break;

      if( PEER_IS_FAILED(UNCHOKER[i]) ) continue;

      if(arg_verbose){
        fprintf(stderr, "D=%lluMB@%uK/s:U=%lluMB ",
          UNCHOKER[i]->TotalDL() >> 20, UNCHOKER[i]->RateDL() >> 10,
          UNCHOKER[i]->TotalUL() >> 20);
        if( UNCHOKER[i]->bitfield.IsEmpty() ) fprintf(stderr, "(empty) ");
      }
      if( UNCHOKER[i]->SetLocal(M_UNCHOKE) < 0){
        if(arg_verbose) fprintf(stderr, "close: Can't unchoke peer\n");
        UNCHOKER[i]->CloseConnection();
        continue;
      }

      sk = UNCHOKER[i]->stream.GetSocket();

      if(!FD_ISSET(sk,wfdp) && UNCHOKER[i]->NeedWrite()){
        FD_SET(sk,wfdp);
        if( maxfd < sk) maxfd = sk;
      }
    } // end for
    if(arg_verbose) fprintf(stderr, "\n");
  }

  Self.ClearCurrentRates();
  
  return maxfd;
}

btPeer* PeerList::Who_Can_Abandon(btPeer *proposer)
{
  PEERNODE *p;
  btPeer *peer = (btPeer*) 0;
  for(p = m_head; p; p = p->next){
    if(!PEER_IS_SUCCESS(p->peer) || p->peer == proposer ||
       p->peer->request_q.IsEmpty() ) continue;

    if(proposer->bitfield.IsSet(p->peer->request_q.GetRequestIdx())){
      if(!peer){
        if( p->peer->RateDL() < proposer->RateDL() ) peer = p->peer;
      }else{
        if( p->peer->RateDL() < peer->RateDL() ) peer = p->peer;
      }
    }
  }//end for
  return peer;
}

// This takes an index parameter to facilitate modification of the function to
// allow targeting of a specific piece.  It's currently only used as a flag to
// specify endgame or initial-piece mode though.
size_t PeerList::What_Can_Duplicate(BitField &bf, btPeer *proposer, size_t idx)
{
  PEERNODE *p;
  btPeer *peer = (btPeer*) 0;
  int endgame;
  PSLICE ps;
  size_t piece, piece1, qsize, mark, bench;
  unsigned long rndbits;
  int r=0;

  endgame = idx < BTCONTENT.GetNPieces();	// else initial-piece mode
  if(endgame) mark = 0;
  else mark = cfg_req_queue_length;
  bench = BTCONTENT.GetNPieces();

  // In initial mode, only dup a piece with trade value.
  // In endgame mode, dup any if there are no pieces with trade value.
  FindValuedPieces(bf, proposer, !endgame);
  if( bf.IsEmpty() ){
    if(endgame) bf = proposer->bitfield;
    else return BTCONTENT.GetNPieces();
  }

  /* In endgame mode, select from peers with the longest request queue.
     In initial mode, select from peers with the shortest non-empty request
     queue.  */
  for(p = m_head; p; p = p->next){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer == proposer ) continue;

    if( p->peer->request_q.IsEmpty() ) continue;

    piece = BTCONTENT.GetNPieces();
    ps = p->peer->request_q.GetHead();
    for( ; ps; ps = ps->next){
      if( piece == ps->index || bench == ps->index || piece1 == ps->index ||
          !bf.IsSet(ps->index) || proposer->request_q.HasIdx(ps->index) )
        continue;
      piece = ps->index;

      qsize = p->peer->request_q.Qlen(piece);
      if( (endgame && qsize > mark) ||
          (!endgame && (qsize < mark || !peer)) ){
        mark = qsize;
        peer = p->peer;
        piece1 = piece;
      }else if( qsize == mark ){
        if( !r-- ){
          rndbits = random();
          r = 30;
        }
        if( bench != piece && (rndbits>>=1)&01 ){
          bench = piece1;
          peer = p->peer;
          piece1 = piece;
        }
      }
    }
  }
  return peer ? piece1 : BTCONTENT.GetNPieces();
}

void PeerList::FindValuedPieces(BitField &bf, btPeer *proposer, int initial)
{
  PEERNODE *p;
  BitField bf_all_have = bf, bf_int_have = bf,
    bf_others_have, bf_only_he_has = bf, bf_prefer;

  for(p = m_head; p; p = p->next){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer == proposer ) continue;
    if( p->peer->Need_Remote_Data() )
      bf_int_have.And(p->peer->bitfield);
    bf_all_have.And(p->peer->bitfield);
    if( !initial && !p->peer->bitfield.IsFull() )
      bf_only_he_has.Except(p->peer->bitfield);
    else bf_others_have.Comb(p->peer->bitfield);
  }
  /* bf_all_have is now pertinent pieces that all peers have
     bf_int_have is pertinent pieces that all peers in which I'm interested have
     We prefer to get pieces that those peers need, if we can.  Otherwise go
     for pieces that any peer needs in hopes of future reciprocation. */
  if( !bf_int_have.IsFull() )
    bf_all_have = bf_int_have;
  bf_all_have.Invert();
  bf.And(bf_all_have); // bf is now pertinent pieces that not everyone has

  bf_prefer = initial ? bf_others_have : bf_only_he_has;

  BitField tmpBitField = bf;
  tmpBitField.And(bf_prefer);
  /* If initial mode, tmpBitField is now pertinent pieces that more than one
     peer has, but not everyone.
     Otherwise, it's pertinent pieces that only the proposer has (not
     considering what other seeders have).
     In either case if there are no such pieces, revert to the simple answer.*/
  if( !tmpBitField.IsEmpty() ) bf = tmpBitField;
}

/* Find a peer with the given piece in its request queue.
   Duplicating a request queue that's in progress rather than creating a new
   one helps avoid requesting slices that we already have. */
btPeer *PeerList::WhoHas(size_t idx)
{
  PEERNODE *p;
  btPeer *peer = (btPeer*) 0;

  for( p = m_head; p; p = p->next){
    if( p->peer->request_q.HasIdx(idx) ){
      peer = p->peer;
      break;
    }
  }
  return peer;
}

void PeerList::CancelSlice(size_t idx, size_t off, size_t len)
{
  PEERNODE *p;
  PSLICE ps;

  for( p = m_head; p; p = p->next){
    
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    if (p->peer->CancelSliceRequest(idx,off,len) < 0) {
      if(arg_verbose) fprintf(stderr, "close: CancelSlice\n");
      p->peer->CloseConnection();
    }
  }
}

void PeerList::Tell_World_I_Have(size_t idx)
{
  PEERNODE *p;
  int f_seed = 0;

  if ( BTCONTENT.pBF->IsFull() ) f_seed = 1;

  for( p = m_head; p; p = p->next){
    
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    // Don't send HAVE to seeders, except for our first piece.
    if( (!p->peer->bitfield.IsFull() || 1==BTCONTENT.pBF->Count()) &&
        p->peer->stream.Send_Have(idx) < 0) 
      p->peer->CloseConnection();
    
    else if( f_seed ){
      // request queue is emptied by setting not-interested state
      if(p->peer->SetLocal(M_NOT_INTERESTED) < 0) {
        if(arg_verbose)
          fprintf(stderr, "close: Can't set self not interested (T_W_I_H)\n");
        p->peer->CloseConnection();
      }
    }
    
  } // end for
}

int PeerList::Accepter()
{
  SOCKET newsk;
  socklen_t addrlen;
  struct sockaddr_in addr;
  addrlen = sizeof(struct sockaddr_in);
  newsk = accept(m_listen_sock,(struct sockaddr*) &addr,&addrlen);
  
  if( INVALID_SOCKET == newsk ) return -1;
  
  if( AF_INET != addr.sin_family || addrlen != sizeof(struct sockaddr_in)) {
    CLOSE_SOCKET(newsk);
    return -1;
  }

  if( Tracker.IsPaused() || Tracker.IsQuitting() ){
    CLOSE_SOCKET(newsk);
    return -1;
  }
  
  return NewPeer(addr,newsk);
}

int PeerList::Initial_ListenPort()
{
  int r = 0;
  struct sockaddr_in lis_addr;
  memset(&lis_addr,0, sizeof(sockaddr_in));
  lis_addr.sin_family = AF_INET;
  lis_addr.sin_addr.s_addr = INADDR_ANY;

  m_listen_sock = socket(AF_INET,SOCK_STREAM,0);

  if( INVALID_SOCKET == m_listen_sock ) return -1;

  if ( cfg_listen_ip != 0 )
    lis_addr.sin_addr.s_addr = cfg_listen_ip;

  if(cfg_listen_port && cfg_listen_port != LISTEN_PORT_MAX){
    lis_addr.sin_port = htons(cfg_listen_port);
    if(bind(m_listen_sock,(struct sockaddr*)&lis_addr,sizeof(struct sockaddr_in)) == 0) 
      r = 1;
    else
      fprintf(stderr,"warn,couldn't bind on specified port %d: %s\n",
        cfg_listen_port,strerror(errno));
  }

  if( !r ){
    r = -1;
    cfg_listen_port = cfg_max_listen_port;
    for( ; r != 0; ){
      lis_addr.sin_port = htons(cfg_listen_port);
      r = bind(m_listen_sock,(struct sockaddr*)&lis_addr,sizeof(struct sockaddr_in));
      if(r != 0){
        cfg_listen_port--;
        if(cfg_listen_port < cfg_min_listen_port){
          CLOSE_SOCKET(m_listen_sock);
          fprintf(stderr,"error,couldn't bind port from %d to %d: %s\n",
            cfg_min_listen_port,cfg_max_listen_port,strerror(errno));
          return -1;
        }
      }
    } /* end for(; r != 0;) */
  }

  if(listen(m_listen_sock,5) == -1){
    CLOSE_SOCKET(m_listen_sock);
    fprintf(stderr,"error, couldn't listen on port %d: %s\n",
      cfg_listen_port,strerror(errno));
    return -1;
  }
  
  if( setfd_nonblock(m_listen_sock) < 0){
    CLOSE_SOCKET(m_listen_sock);
    fprintf(stderr,"error, couldn't set socket to nonblock mode.\n");
    return -1;
  }

  printf("Listening on %s:%d\n", inet_ntoa(lis_addr.sin_addr),
    ntohs(lis_addr.sin_port));
  
  return 0;
}

size_t PeerList::Pieces_I_Can_Get()
{
  PEERNODE *p;
  BitField tmpBitField = *BTCONTENT.pBF;

  if( tmpBitField.IsFull() ) return BTCONTENT.GetNPieces();

  for( p = m_head; p && !tmpBitField.IsFull(); p = p->next){
    if( !PEER_IS_SUCCESS(p->peer) ) continue;
    tmpBitField.Comb(p->peer->bitfield);
  }
  return tmpBitField.Count();
}

int PeerList::AlreadyRequested(size_t idx)
{
  PEERNODE *p;
  for(p = m_head; p; p = p->next){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty()) continue;
    if( p->peer->request_q.HasIdx(idx) ) return 1;
  }
  return 0;
}

void PeerList::CheckBitField(BitField &bf)
{
  PEERNODE *p;
  PSLICE ps;
  size_t idx;
  for(p = m_head; p ; p = p->next){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty()) continue;
    ps = p->peer->request_q.GetHead();
    idx = BTCONTENT.GetNPieces();
    for( ; ps; ps = ps->next){
      if( ps->index != idx ){
        bf.UnSet(ps->index);
        idx = ps->index;
      }
    }
  }
}

void PeerList::PrintOut()
{
  PEERNODE *p = m_head;
  struct sockaddr_in sin;
  printf("\nPEER LIST\n");
  for( ; p ; p = p->next){
        if(PEER_IS_FAILED(p->peer)) continue;
        p->peer->dump();
  }
}

void PeerList::AnyPeerReady(fd_set *rfdp, fd_set *wfdp, int *nready)
{
  PEERNODE *p,*p2;
  btPeer *peer;
  SOCKET sk;

  if( FD_ISSET(m_listen_sock, rfdp)){
    FD_CLR(m_listen_sock,rfdp);
    (*nready)--;
    Accepter();
  }

  for(p = m_head; p && *nready ; p = p->next){
    if( PEER_IS_FAILED(p->peer) ) continue;

    peer = p->peer;
    sk = peer->stream.GetSocket();

    if( P_CONNECTING == peer->GetStatus()){
      if(FD_ISSET(sk,wfdp)){
        (*nready)--; 
        FD_CLR(sk,wfdp);

        if(FD_ISSET(sk,rfdp)){	// connect failed.
          (*nready)--; 
          FD_CLR(sk,rfdp);
          peer->CloseConnection();
        }else{
          if(peer->Send_ShakeInfo() < 0){
            if(arg_verbose) fprintf(stderr, "close: Sending handshake\n");
            peer->CloseConnection();
          }
          else 
            peer->SetStatus(P_HANDSHAKE);
        }
      }else if(FD_ISSET(sk,rfdp)){
        (*nready)--; 
        peer->CloseConnection();
      }
    }else{
      if(FD_ISSET(sk,rfdp)){
        (*nready)--;
        FD_CLR(sk,rfdp);
        if(peer->GetStatus() == P_HANDSHAKE){
          if( peer->HandShake() < 0 ) {
            if(arg_verbose) fprintf(stderr, "close: bad handshake\n");
            peer->CloseConnection();
          }
        } // fixed client stall
        if(peer->GetStatus() == P_SUCCESS){
          if( peer->RecvModule() < 0 ) {
            if(arg_verbose) fprintf(stderr, "close: receive\n");
            peer->CloseConnection();
          }
        }
      }else if( peer->HealthCheck(now) < 0 ){
        if(arg_verbose) fprintf(stderr, "close: unresponsive\n");
        peer->CloseConnection();
      }

      if( (PEER_IS_SUCCESS(peer) || peer->GetStatus() == P_HANDSHAKE) &&
          FD_ISSET(sk,wfdp) ){
        (*nready)--;
        FD_CLR(sk,wfdp);
        if( peer->SendModule() < 0 ) {
          if(arg_verbose) fprintf(stderr, "close: send\n");
          peer->CloseConnection();
        }
      }else peer->CheckSendStatus();
    }
  }// end for
}

void PeerList::CloseAllConnectionToSeed()
{
  PEERNODE *p = m_head;
  for( ; p; p = p->next)
    if( p->peer->bitfield.IsFull() ||
        /* Drop peers who remain uninterested, but keep recent connections.
           Peers who connected recently will resolve by bitfield exchange. */
        (PEER_IS_SUCCESS(p->peer) && !p->peer->Is_Remote_Interested() &&
          BTCONTENT.GetSeedTime() - now >= 300 &&
          !p->peer->ConnectedWhileSeed()) ){
      p->peer->DontWantAgain();
      if(arg_verbose) fprintf(stderr, "close: seed<->seed\n");
      p->peer->CloseConnection();
    }
}

void PeerList::UnChokeCheck(btPeer* peer, btPeer *peer_array[])
{
  int i = 0;
  int cancel_idx = 0;
  btPeer *loster = (btPeer*) 0;
  int f_seed = BTCONTENT.pBF->IsFull() || m_seed_only;
  int no_opt = 0;
  unsigned long rndbits;
  int r=0;

  if(m_opt_timestamp) no_opt = 1;
  if(f_seed) no_opt = 1 - no_opt;

// Find my 3 or 4 fastest peers.
// The MAX_UNCHOKE+1 (4th) slot is for the optimistic unchoke when it happens.

  // Find a slot for the candidate--the slowest peer, or an available slot.
  for( cancel_idx = i = 0; i < MAX_UNCHOKE+no_opt; i++ ){
    if((btPeer*) 0 == peer_array[i] ||
        PEER_IS_FAILED(peer_array[i]) ){	// сп©ун╩
      cancel_idx = i; 
      break;
    }else{
      if(cancel_idx == i) continue;

      if(f_seed){
        // compare time unchoked
        if( (!peer_array[i]->Is_Local_UnChoked() &&
            (peer_array[cancel_idx]->Is_Local_UnChoked() ||
              peer_array[cancel_idx]->GetLastUnchokeTime() <
                peer_array[i]->GetLastUnchokeTime())) ||
            (peer_array[i]->Is_Local_UnChoked() &&
             peer_array[cancel_idx]->Is_Local_UnChoked() &&
             peer_array[i]->GetLastUnchokeTime() <
               peer_array[cancel_idx]->GetLastUnchokeTime()) )
          cancel_idx = i;
      }else{
        // compare download rate.
        if( peer_array[cancel_idx]->RateDL() > peer_array[i]->RateDL()
          //if equal, reciprocate to the peer we've sent less to, proportionally
          ||(peer_array[cancel_idx]->RateDL() == peer_array[i]->RateDL()
            && peer_array[cancel_idx]->TotalUL()
                / (peer_array[cancel_idx]->TotalDL()+.001)
              < peer_array[i]->TotalUL() / (peer_array[i]->TotalDL()+.001)) )
          cancel_idx = i;
      }
    }
  } // end for

  if( (btPeer*) 0 != peer_array[cancel_idx] &&
      PEER_IS_SUCCESS(peer_array[cancel_idx]) ){
    if(f_seed){
      if( (!peer_array[cancel_idx]->Is_Local_UnChoked() &&
           (peer->Is_Local_UnChoked() ||
             peer->GetLastUnchokeTime() <
               peer_array[cancel_idx]->GetLastUnchokeTime())) ||
           (peer_array[cancel_idx]->Is_Local_UnChoked() &&
            peer->Is_Local_UnChoked() &&
            peer_array[cancel_idx]->GetLastUnchokeTime() <
              peer->GetLastUnchokeTime()) ){
        loster = peer_array[cancel_idx];
        peer_array[cancel_idx] = peer;
      }else
        loster = peer;
    }else{
      if( peer->RateDL() > peer_array[cancel_idx]->RateDL()
        // If equal, reciprocate to the peer we've sent less to, proportionally
        ||(peer_array[cancel_idx]->RateDL() == peer->RateDL()
          && peer_array[cancel_idx]->TotalUL()
                / (peer_array[cancel_idx]->TotalDL()+.001)
            > peer->TotalUL() / (peer->TotalDL()+.001)) ){
        loster = peer_array[cancel_idx];
        peer_array[cancel_idx] = peer;
      }else
        loster = peer;
    }

    // opt unchoke
    if (no_opt) {
      if(loster->SetLocal(M_CHOKE) < 0) loster->CloseConnection();
    }
    else
    // The last slot is for the optimistic unchoke.
    if( (btPeer*) 0 == peer_array[MAX_UNCHOKE] ||
        PEER_IS_FAILED(peer_array[MAX_UNCHOKE]) )
      peer_array[MAX_UNCHOKE] = loster;
    else {
      if( !r-- ){
        rndbits = random();
        r = 15;
      }
      // if loser is empty and current is not, loser gets 75% chance.
      if( loster->IsEmpty() && !peer_array[MAX_UNCHOKE]->IsEmpty()
            && (rndbits>>=2)&3 ) {
        btPeer* tmp = peer_array[MAX_UNCHOKE];
        peer_array[MAX_UNCHOKE] = loster;
        loster = tmp;
      } else
        // This mess chooses the loser:
        // if loser is choked and current is not
        // OR if both are choked and loser has waited longer
        // OR if both are unchoked and loser has had less time unchoked.
      if( (!loster->Is_Local_UnChoked() &&
            ( peer_array[MAX_UNCHOKE]->Is_Local_UnChoked() ||
              loster->GetLastUnchokeTime() <
                peer_array[MAX_UNCHOKE]->GetLastUnchokeTime() )) ||
          (loster->Is_Local_UnChoked() &&
            peer_array[MAX_UNCHOKE]->Is_Local_UnChoked() &&
            peer_array[MAX_UNCHOKE]->GetLastUnchokeTime() <
              loster->GetLastUnchokeTime()) ){
        // if current is empty and loser is not, loser gets 25% chance;
        //    else loser wins.
        // transformed to: if loser is empty or current isn't, or 25% chance,
        //    then loser wins.
        if( !peer_array[MAX_UNCHOKE]->IsEmpty() || loster->IsEmpty()
            || !((rndbits>>=2)&3) ) {
          btPeer* tmp = peer_array[MAX_UNCHOKE];
          peer_array[MAX_UNCHOKE] = loster;
          loster = tmp;
        }
      }
      if(loster->SetLocal(M_CHOKE) < 0) loster->CloseConnection();
    }
  }else //else if((btPeer*) 0 != peer_array[cancel_idx].....
    peer_array[cancel_idx] = peer;
}

// When we change what we're going after, we need to evaluate & set our
// interest with each peer appropriately.
void PeerList::CheckInterest()
{
  PEERNODE *p = m_head;
  for( ; p; p = p->next) {
    // Don't shortcut by checking Is_Local_Interested(), as we need to let
    // SetLocal() reset the m_standby flag.
    if( p->peer->Need_Remote_Data() ) {
      if( p->peer->SetLocal(M_INTERESTED) < 0 )
        p->peer->CloseConnection();
    } else {
      if( p->peer->SetLocal(M_NOT_INTERESTED) < 0 )
        p->peer->CloseConnection();
    }
  }
}

btPeer* PeerList::GetNextPeer(btPeer *peer)
{
  static PEERNODE *p = m_head;

  if( 0==peer ) p = m_head;
  else if( p->peer == peer ){
    p = p->next;
  }else{
    for( p=m_head; p && (p->peer != peer); p = p->next);
    if( 0 == p->peer ){
      p = m_head;
    }else{
      p = p->next;
    }
  }
  for( ; p; p = p->next)
    if( p->peer && PEER_IS_SUCCESS(p->peer) ) break;

  if(p) return p->peer;
  else return (btPeer*) 0;
}

// This is used to un-standby peers when we enter endgame mode.
// It re-evaluates at most once per second for CPU efficiency, so isn't used
// when greatest accuracy is desired.
int PeerList::Endgame()
{
  static time_t timestamp = 0;
  static int endgame = 0;

  if( now > timestamp ){
    timestamp = now;
    if( arg_file_to_download ){
      BitField afdBitField =  *BTCONTENT.pBF;
      afdBitField.Except(*BTCONTENT.pBFilter);
      endgame = ( BTCONTENT.getFilePieces(arg_file_to_download)
                  - afdBitField.Count() ) < TotalPeers();
    }else
      endgame = ( Pieces_I_Can_Get() - BTCONTENT.pBF->Count() ) < TotalPeers();
  }
  return endgame;
}

// Note, if other seed-only reasons are added, m_seed_only will need to
// become a multiple-bit value and this function will need to change.
// Currently only btContent::FlushCache() sets the state.
void PeerList::SeedOnly(int state)
{
  PEERNODE *p = m_head;

  m_seed_only = state;

  if( m_seed_only ){
    for( ; p; p = p->next) {
      if( p->peer->Is_Local_Interested() ) {
        if(p->peer->PutPending() < 0 || p->peer->SetLocal(M_NOT_INTERESTED) < 0)
          p->peer->CloseConnection();
      }
    }
  }else CheckInterest();
}

