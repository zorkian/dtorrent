#include "peerlist.h"  // def.h

#include <sys/types.h>

#include <stdlib.h>

#include <stdio.h>
#include <errno.h>

#include <string.h>

#include "btconfig.h"
#include "connect_nonb.h"
#include "setnonblock.h"
#include "btcontent.h"

#include "iplist.h"
#include "tracker.h"
#include "ctcs.h"
#include "bttime.h"
#include "console.h"
#include "util.h"

#if !defined(HAVE_SNPRINTF) || !defined(HAVE_NTOHS) || !defined(HAVE_HTONS)
#include "compat.h"
#endif

#define MIN_UNCHOKES 3
#define MIN_OPT_CYCLE 3
#define MIN_UNCHOKE_INTERVAL 10

#define KEEPALIVE_INTERVAL 117

#define PEER_IS_SUCCESS(peer) (DT_PEER_SUCCESS == (peer)->GetStatus())
#define PEER_IS_FAILED(peer) (DT_PEER_FAILED == (peer)->GetStatus())
#define NEED_MORE_PEERS() (m_peers_count < cfg_max_peers)

PeerList WORLD;

PeerList::PeerList()
{
  m_unchoke_check_timestamp =
    m_keepalive_check_timestamp =
    m_opt_timestamp =
    m_interval_timestamp = time((time_t *)0);
  m_unchoke_interval = MIN_UNCHOKE_INTERVAL;
  m_opt_interval = MIN_OPT_CYCLE * MIN_UNCHOKE_INTERVAL;

  m_head = m_dead = m_next_dl = m_next_ul = (PEERNODE *)0;
  m_listen_sock = INVALID_SOCKET;
  m_peers_count = m_seeds_count = m_conn_count = m_downloads = 0;
  m_f_pause = m_endgame = 0;
  m_max_unchoke = MIN_UNCHOKES;
  m_defer_count = m_missed_count = 0;
  m_upload_count = m_up_opt_count = 0;
  m_prev_limit_up = cfg_max_bandwidth_up;
  m_dup_req_pieces = 0;
  m_readycnt = 0;
}

PeerList::~PeerList()
{
  PEERNODE *p, *pnext;
  for( p = m_head; p; p = pnext ){
    pnext = p->next;
    delete p->peer;
    delete p;
  }
  for( p = m_dead; p; p = pnext ){
    pnext = p->next;
    delete p->peer;
    delete p;
  }
}

void PeerList::CloseAll()
{
  PEERNODE *p;
  for( p = m_head; p; p = m_head ){
    m_head = p->next;
    delete (p->peer);
    delete p;
  }
}

int PeerList::NewPeer(struct sockaddr_in addr, SOCKET sk)
{
  PEERNODE *p, *pp, *pnext;
  btPeer *peer = (btPeer *)0;
  int r;

  if( m_peers_count >= cfg_max_peers ){
    if( INVALID_SOCKET != sk ) CLOSE_SOCKET(sk);
    return -4;
  }

  if( INVALID_SOCKET != sk && Self.IpEquiv(addr) ){
    if(arg_verbose)
      CONSOLE.Debug("Connection from myself %s", inet_ntoa(addr.sin_addr));
    Tracker.AdjustPeersCount();
    if( INVALID_SOCKET != sk ) CLOSE_SOCKET(sk);
    return -3;
  }

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_FAILED(p->peer) && p->peer->IpEquiv(addr) ){
      if(arg_verbose) CONSOLE.Debug("Connection from duplicate peer %s",
        inet_ntoa(addr.sin_addr));
      if( INVALID_SOCKET != sk ) CLOSE_SOCKET(sk);
      return -3;
    }
  }

  /* See if we've had this peer before, and maintain its stats.
     Do it here instead of later to insure we purge old entries
     periodically. */
  pp = (PEERNODE *)0;
  for( p = m_dead; p; p = pnext ){
    if( p->peer->IpEquiv(addr) ) break;
    else{
      pnext = p->next;
      if( p->peer->GetLastTimestamp() + 2 * Tracker.GetInterval() < now ){
        delete p->peer;
        if( pp ) pp->next = p->next;
        else m_dead = p->next;
        delete p;
      }else pp = p;
    }
  }

  if( INVALID_SOCKET == sk ){  // outbound connection
    if( INVALID_SOCKET == (sk = socket(AF_INET, SOCK_STREAM, 0)) ) return -1;

    if( setfd_nonblock(sk) < 0 ) goto err;

    if( -1 == (r = connect_nonb(sk, (struct sockaddr *)&addr)) ){
      if(arg_verbose) CONSOLE.Debug("Connect to peer at %s:%hu failed:  %s",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), strerror(errno));
      return -1;
    }

    peer = new btPeer;
#ifndef WINDOWS
    if( !peer ) goto err;
#endif

    peer->SetConnect();
    peer->SetAddress(addr);
    peer->stream.SetSocket(sk);
    peer->SetStatus( (-2 == r) ? DT_PEER_CONNECTING : DT_PEER_HANDSHAKE );
    if(arg_verbose)
      CONSOLE.Debug("Connect%s to %s:%hu (peer %p)", (-2==r) ? "ing" : "ed",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), peer);

  }else{  // inbound connection
    if( setfd_nonblock(sk) < 0 ) goto err;

    peer = new btPeer;
#ifndef WINDOWS
    if( !peer ) goto err;
#endif

    peer->SetAddress(addr);
    peer->stream.SetSocket(sk);
    peer->SetStatus(DT_PEER_HANDSHAKE);
    if(arg_verbose) CONSOLE.Debug("Connection from %s:%hu (peer %p)",
        inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), peer);
  }

  if( !BTCONTENT.Seeding() &&
      peer->stream.in_buffer.SetSize(BUF_DEF_SIZ + cfg_req_slice_size) < 0 ){
    goto err;
  }

  if( DT_PEER_HANDSHAKE == peer->GetStatus() )
    if( peer->Send_ShakeInfo() != 0 ) goto err;

  if( p ){  // resurrected! (reconnected with an old peer)
    if( pp ) pp->next = p->next;
    else m_dead = p->next;
    peer->CopyStats(p->peer);
    delete p->peer;
  }else{
    p = new PEERNODE;
#ifndef WINDOWS
    if( !p ) goto err;
#endif
  }

  m_peers_count++;
  p->peer = peer;
  p->next = m_head;
  m_head = p;
  return 0;

 err:
  if( peer ) delete peer;
  if( INVALID_SOCKET != sk ) CLOSE_SOCKET(sk);
  return -1;
}

int PeerList::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
  int f_keepalive_check = 0;
  int f_unchoke_check = 0;
  btPeer **UNCHOKER = (btPeer **)0;

  // No pause check here--stay ready by continuing to acquire peers.
  if( !Tracker.IsQuitting() ){
    struct sockaddr_in addr;
    while( NEED_MORE_PEERS() && !IPQUEUE.IsEmpty() ){
      if( IPQUEUE.Pop(&addr) < 0 ) break;
      if( NewPeer(addr, INVALID_SOCKET) == -4 ) break;
    }
  }

  m_ul_limited = BandwidthLimitUp(Self.LateUL());

  // After seeding a while, disconnect uninterested peers & shrink in_buffers.
  if( now - BTCONTENT.GetSeedTime() <= 301 &&
      now - BTCONTENT.GetSeedTime() >= 300 ){
    CloseAllConnectionToSeed();
  }

  if( KEEPALIVE_INTERVAL <= now - m_keepalive_check_timestamp ){
    m_keepalive_check_timestamp = now;
    f_keepalive_check = 1;
  }

  if( m_unchoke_interval <= now - m_unchoke_check_timestamp && m_head &&
      !m_f_pause ){
    f_unchoke_check = 1;

    if( m_missed_count > m_upload_count && cfg_max_bandwidth_up ){
      dt_count_t unchokes = GetUnchoked();  // already adds one (opt)
      if( unchokes < MIN_UNCHOKES ) m_max_unchoke = MIN_UNCHOKES;
      else{
        m_max_unchoke = unchokes;
        if(arg_verbose)
          CONSOLE.Debug("max unchokes up to %d", (int)m_max_unchoke);
      }
    }else if(arg_verbose) CONSOLE.Debug("UL missed %d sending %d",
      (int)m_missed_count, (int)m_upload_count);
    m_up_opt_count += m_upload_count;
    m_missed_count = m_upload_count = 0;

    if( m_opt_interval && m_opt_interval <= now - m_opt_timestamp ){
      m_opt_timestamp = 0;
      if( m_defer_count > m_up_opt_count &&
          m_max_unchoke > MIN_UNCHOKES && cfg_max_bandwidth_up ){
        m_max_unchoke--;
        if(arg_verbose)
          CONSOLE.Debug("max unchokes down to %d", (int)m_max_unchoke);
      }else if(arg_verbose) CONSOLE.Debug("UL deferred %d sending %d",
        (int)m_defer_count, (int)m_up_opt_count);
      m_defer_count = m_up_opt_count = 0;
    }

    if( 0==cfg_max_bandwidth_up ) m_max_unchoke = MIN_UNCHOKES;

    UNCHOKER = new btPeer *[m_max_unchoke + 1];
    if( UNCHOKER ) memset(UNCHOKER, 0, (m_max_unchoke + 1) * sizeof(btPeer *));
    else CONSOLE.Warning(1, "warn, failed to allocate unchoke array.");

    SetUnchokeIntervals();
  }else{  // no unchoke check
    if( now < m_unchoke_check_timestamp ) m_unchoke_check_timestamp = now;
    if( MIN_UNCHOKE_INTERVAL <= now - m_interval_timestamp ){
      m_interval_timestamp = now;
      /* If up bw limit has changed enough, recompute the intervals.
         This is primarily to prevent a low limit from delaying an unchoke for
         a long time even after the limit has been increased. */
      if( !BandwidthLimitUp() ||
          ( m_prev_limit_up &&
            labs((long)cfg_max_bandwidth_up - (long)m_prev_limit_up) /
                 (double)m_prev_limit_up  >
              1 / (double)m_unchoke_interval &&
            ( cfg_max_bandwidth_up < cfg_req_slice_size * (MIN_OPT_CYCLE-1) /
                                     (MIN_UNCHOKE_INTERVAL * MIN_OPT_CYCLE) ||
              m_prev_limit_up < cfg_req_slice_size * (MIN_OPT_CYCLE-1) /
                                (MIN_UNCHOKE_INTERVAL * MIN_OPT_CYCLE) ) ) ){
        SetUnchokeIntervals();
      }
    }else if( now < m_interval_timestamp ) m_interval_timestamp = now;
  }

  return FillFDSet(rfdp, wfdp, f_keepalive_check, f_unchoke_check, UNCHOKER);
}

int PeerList::FillFDSet(fd_set *rfdp, fd_set *wfdp, int f_keepalive_check,
  int f_unchoke_check, btPeer **UNCHOKER)
{
  PEERNODE *p, *pp;
  btPeer *peer;
  int maxfd = -1, f_idle = 0;
  SOCKET sk = INVALID_SOCKET;

  m_f_limitu = BandwidthLimitUp(Self.LateUL());
  m_f_limitd = BandwidthLimitDown(Self.LateDL());
  if( cfg_cache_size && !m_f_pause )
    f_idle = IsIdle();

 again:
  pp = (PEERNODE *)0;
  m_seeds_count = m_conn_count = m_downloads = 0;
  dt_count_t interested_count = 0;
  for( p = m_head; p; ){
    peer = p->peer;
    sk = peer->stream.GetSocket();
    if( PEER_IS_FAILED(peer) ){
      if( sk != INVALID_SOCKET ){
        FD_CLR(sk, rfdp);
        FD_CLR(sk, wfdp);
      }
      if( peer->CanReconnect() ){  // connect to this peer again
        if(arg_verbose) CONSOLE.Debug("Adding %p for reconnect", peer);
        peer->Retry();
        struct sockaddr_in addr;
        peer->GetAddress(&addr);
        IPQUEUE.Add(&addr);
      }
      if( pp ) pp->next = p->next;
      else m_head = p->next;
      if( peer->TotalDL() || peer->TotalUL() ){  // keep stats
        peer->SetLastTimestamp();
        p->next = m_dead;
        m_dead = p;
      }else{
        delete peer;
        delete p;
      }
      m_peers_count--;
      p = pp ? pp->next : m_head;
      continue;
    }else{
      if( !PEER_IS_SUCCESS(peer) ) m_conn_count++;
      else{
        if( peer->bitfield.IsFull() ) m_seeds_count++;
        if( peer->Is_Local_Interested() ){
          interested_count++;
          if( peer->Is_Remote_Unchoked() ) m_downloads++;
        }
      }
      if( f_keepalive_check ){
        if( 3 * KEEPALIVE_INTERVAL <= now - peer->GetLastTimestamp() ){
          if(arg_verbose) CONSOLE.Debug("close: keepalive expired");
          peer->CloseConnection();
          goto skip_continue;
        }
        if( PEER_IS_SUCCESS(peer) &&
            KEEPALIVE_INTERVAL <= now - peer->GetLastTimestamp() &&
            peer->AreYouOK() < 0 ){
          if(arg_verbose) CONSOLE.Debug("close: keepalive death");
          peer->CloseConnection();
          goto skip_continue;
        }
      }
      if( f_unchoke_check && PEER_IS_SUCCESS(peer) ){
        if( peer->Is_Remote_Interested() && peer->Need_Local_Data() ){
          if( UNCHOKER && UnchokeCheck(peer, UNCHOKER) < 0 )
            goto skip_continue;
        }else if( peer->SetLocal(BT_MSG_CHOKE) < 0 ){
          if(arg_verbose) CONSOLE.Debug("close: Can't choke peer");
          peer->CloseConnection();
          goto skip_continue;
        }
      }

      if( PEER_IS_FAILED(peer) ) goto skip_continue;  // failsafe
      if( maxfd < sk ) maxfd = sk;
      if( !FD_ISSET(sk, rfdp) && peer->NeedRead((int)m_f_limitd) )
        FD_SET(sk, rfdp);
      if( !FD_ISSET(sk, wfdp) && peer->NeedWrite((int)m_f_limitu) )
        FD_SET(sk, wfdp);

      if( cfg_cache_size && !m_f_pause && f_idle && peer->NeedPrefetch() ){
        if( peer->Prefetch(m_unchoke_check_timestamp + m_unchoke_interval) ){
            SetIdled();
            f_idle = IsIdle();
        }
      }

    skip_continue:
      if( PEER_IS_FAILED(peer) ){
        FD_CLR(sk, rfdp);
        FD_CLR(sk, wfdp);
      }
      pp = p;
      p = p->next;
    }
  }  // end for
  if( (m_f_limitu && !(m_f_limitu = BandwidthLimitUp(Self.LateUL()))) ||
      (m_f_limitd && !(m_f_limitd = BandwidthLimitDown(Self.LateDL()))) ){
    goto again;
  }

  if( 0==interested_count ) Self.StopDLTimer();

  if( INVALID_SOCKET != m_listen_sock && m_peers_count < cfg_max_peers ){
    FD_SET(m_listen_sock, rfdp);
    if( maxfd < m_listen_sock ) maxfd = m_listen_sock;
  }

  if( f_unchoke_check && UNCHOKER ){
    m_unchoke_check_timestamp = now;  // time of the last unchoke check
    if( !m_opt_timestamp ) m_opt_timestamp = now;

    if( !UNCHOKER[0] ) Self.StopULTimer();

    for( dt_count_t i = 0; i < m_max_unchoke + 1; i++ ){
      if( !UNCHOKER[i] ) break;

      if( PEER_IS_FAILED(UNCHOKER[i]) ) continue;

      sk = UNCHOKER[i]->stream.GetSocket();

      if( UNCHOKER[i]->SetLocal(BT_MSG_UNCHOKE) < 0 ){
        if(arg_verbose) CONSOLE.Debug("close: Can't unchoke peer");
        UNCHOKER[i]->CloseConnection();
        FD_CLR(sk, rfdp);
        FD_CLR(sk, wfdp);
        continue;
      }

      if( !FD_ISSET(sk, wfdp) && UNCHOKER[i]->NeedWrite((int)m_f_limitu) ){
        FD_SET(sk, wfdp);
        if( maxfd < sk ) maxfd = sk;
      }
    }  // end for
    delete []UNCHOKER;
  }

  return maxfd;
}

void PeerList::SetUnchokeIntervals()
{
  time_t old_unchoke_int = m_unchoke_interval, old_opt_int = m_opt_interval;

  // Unchoke peers long enough to have a chance at getting some data.
  if( BandwidthLimitUp() && BTCONTENT.Seeding() ){
    dt_count_t optx = (dt_count_t)(1 / (1 - (double)MIN_UNCHOKE_INTERVAL *
                                   cfg_max_bandwidth_up / cfg_req_slice_size));
    if( optx < 0 ) optx = 0;
    if( optx < MIN_OPT_CYCLE ){
      optx = MIN_OPT_CYCLE;
      double interval = cfg_req_slice_size /
           (cfg_max_bandwidth_up * MIN_OPT_CYCLE / (double)(MIN_OPT_CYCLE-1));
      m_unchoke_interval = (time_t)interval;
      if( interval - (int)interval > 0 ) m_unchoke_interval++;
      if( m_unchoke_interval < MIN_UNCHOKE_INTERVAL )
        m_unchoke_interval = MIN_UNCHOKE_INTERVAL;
    }else{
      // Allow each peer at least 60 seconds unchoked.
      m_unchoke_interval = MIN_UNCHOKE_INTERVAL;
      if( m_max_unchoke+1 < (dt_count_t)(60 / m_unchoke_interval) ){
        dt_count_t maxopt = (dt_count_t)(1 / (1 - (double)(m_max_unchoke+1) *
                                                  m_unchoke_interval / 60));
        if( maxopt > MIN_OPT_CYCLE && optx > maxopt ) optx = maxopt;
      }
      if( optx > m_max_unchoke+2 ) optx = m_max_unchoke+2;
    }
    m_opt_interval = optx * m_unchoke_interval;
  }else if( BandwidthLimitUp() && !BTCONTENT.Seeding() ){
    // Need to be able to upload a slice per interval.
    double interval = cfg_req_slice_size / (double)cfg_max_bandwidth_up;
    m_unchoke_interval = (time_t)interval;
    if( interval - (int)interval > 0 ) m_unchoke_interval++;
    if( m_unchoke_interval < MIN_UNCHOKE_INTERVAL )
      m_unchoke_interval = MIN_UNCHOKE_INTERVAL;
    m_opt_interval = MIN_OPT_CYCLE * m_unchoke_interval;
  }else{
    m_unchoke_interval = MIN_UNCHOKE_INTERVAL;
    m_opt_interval = MIN_OPT_CYCLE * MIN_UNCHOKE_INTERVAL;
  }
  m_prev_limit_up = cfg_max_bandwidth_up;
  m_interval_timestamp = now;
  if( arg_verbose && (m_unchoke_interval != old_unchoke_int ||
                      m_opt_interval != old_opt_int) ){
    CONSOLE.Debug("ulimit %d, unchoke interval %d, opt interval %d",
      (int)cfg_max_bandwidth_up, (int)m_unchoke_interval, (int)m_opt_interval);
  }
}

btPeer *PeerList::Who_Can_Abandon(btPeer *proposer)
{
  PEERNODE *p;
  btPeer *peer = (btPeer *)0;
  PSLICE ps;
  bt_index_t idx;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer == proposer ||
        p->peer->request_q.IsEmpty() ){
      continue;
    }
    if( (peer && p->peer->NominalDL() < peer->NominalDL()) ||
        (!peer && p->peer->NominalDL() * 1.5 < proposer->NominalDL()) ){
      idx = p->peer->request_q.GetRequestIdx();
      if( proposer->bitfield.IsSet(idx) && !proposer->request_q.HasIdx(idx) )
        peer = p->peer;
      else{
        ps = p->peer->request_q.GetHead();
        for( ; ps; ps = ps->next ){
          if( idx == ps->index ) continue;
          idx = ps->index;
          if( proposer->bitfield.IsSet(idx) &&
              !proposer->request_q.HasIdx(idx) ){
            peer = p->peer;
            break;
          }
        }
      }
    }
  }  // end for
  if( peer && arg_verbose )
    CONSOLE.Debug("Abandoning %p (%d B/s) for %p (%d B/s)",
      peer, peer->NominalDL(), proposer, proposer->NominalDL());
  return peer;
}

/* This takes an index parameter to facilitate modification of the function to
   allow targeting of a specific piece.  It's currently only used as a flag to
   specify endgame or initial-piece mode though. */
bt_index_t PeerList::What_Can_Duplicate(Bitfield &bf, const btPeer *proposer,
  bt_index_t idx)
{
  struct qdata {
    bt_index_t idx;
    dt_count_t qlen, count;
  };
  struct qdata *data;
  int endgame, pass;
  PEERNODE *p;
  PSLICE ps;
  bt_index_t piece;
  dt_count_t slots, qsize, i, mark;
  double work, best;

  endgame = idx < BTCONTENT.GetNPieces();  // else initial-piece mode
  slots = endgame ? BTCONTENT.GetNPieces() - BTCONTENT.pBF->Count() :
                    m_downloads * 2;
  if( slots < m_dup_req_pieces + 2 ) slots = m_dup_req_pieces + 2;
  data = new struct qdata[slots];
#ifndef WINDOWS
  if( !data ) return BTCONTENT.GetNPieces();
#endif

  /* In initial mode, only dup a piece with trade value.
     In endgame mode, dup any if there are no pieces with trade value. */
  FindValuedPieces(bf, proposer, !endgame);
  if( bf.IsEmpty() ){
    if( endgame ) bf = proposer->bitfield;
    else return BTCONTENT.GetNPieces();
  }

  // initialize
  data[0].idx = BTCONTENT.GetNPieces();
  data[0].qlen = 0;
  data[0].count = 0;
  for( i = 1; i < slots; i++ )
    memcpy(data + i, data, sizeof(struct qdata));

  // measure applicable piece request queues
  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer == proposer ||
        p->peer->request_q.IsEmpty() ){
      continue;
    }
    piece = BTCONTENT.GetNPieces();
    ps = p->peer->request_q.GetHead();
    for( ; ps; ps = ps->next ){
      if( piece == ps->index ||
          !bf.IsSet(ps->index) || proposer->request_q.HasIdx(ps->index) ){
        continue;
      }
      piece = ps->index;
      qsize = p->peer->request_q.Qlen(piece);

      // insert queue data into array at (idx % slots)
      pass = 0;
      i = piece % slots;
      while( data[i].idx < BTCONTENT.GetNPieces() && pass < 2 ){
        if( piece == data[i].idx ) break;
        i++;
        if( i >= slots ){
          i = 0;
          pass++;
        }
      }
      if( pass < 2 ){
        if( data[i].idx == BTCONTENT.GetNPieces() ){
          data[i].idx = piece;
          data[i].qlen = qsize;
        }
        data[i].count++;
      }
    }
  }  // end of measurement loop

  /* Find the best workload for initial/endgame.
     In endgame mode, request the piece that should take the longest.
     In initial mode, request the piece that should complete the fastest. */
  best = endgame ? 0 : BTCONTENT.GetPieceLength() / cfg_req_slice_size + 2;
  mark = slots;
  for( i = 0; i < slots; i++ ){
    if( data[i].idx == BTCONTENT.GetNPieces() ) continue;
    work = data[i].qlen / (double)data[i].count;
    if( work > 1 && (endgame ? work > best : work < best) ){
      best = work;
      mark = i;
    }
  }
  if( mark < slots && data[mark].count == 1 ) m_dup_req_pieces++;
  CONSOLE.Debug("%d dup req pieces", (int)m_dup_req_pieces);
  piece = ( mark < slots ) ? data[mark].idx : BTCONTENT.GetNPieces();
  delete []data;
  return piece;
}

void PeerList::FindValuedPieces(Bitfield &bf, const btPeer *proposer,
  int initial) const
{
  PEERNODE *p;
  Bitfield bf_all_have = bf, bf_int_have = bf,
    bf_others_have, bf_only_he_has = bf, *pbf_prefer;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer == proposer ) continue;
    if( p->peer->Need_Remote_Data() )
      bf_int_have.And(p->peer->bitfield);
    bf_all_have.And(p->peer->bitfield);
    if( !initial && !p->peer->bitfield.IsFull() )
      bf_only_he_has.Except(p->peer->bitfield);
    else bf_others_have.Or(p->peer->bitfield);
  }
  /* bf_all_have is now pertinent pieces that all peers have
     bf_int_have is pertinent pieces that all peers in which I'm interested have
     We prefer to get pieces that those peers need, if we can.  Otherwise go
     for pieces that any peer needs in hopes of future reciprocation. */
  if( !bf_int_have.IsFull() )
    bf_all_have = bf_int_have;
  bf_all_have.Invert();
  bf.And(bf_all_have);  // bf is now pertinent pieces that not everyone has

  pbf_prefer = initial ? &bf_others_have : &bf_only_he_has;

  Bitfield tmpBitfield = bf;
  tmpBitfield.And(*pbf_prefer);
  /* If initial mode, tmpBitfield is now pertinent pieces that more than one
     peer has, but not everyone.
     Otherwise, it's pertinent pieces that only the proposer has (not
     considering what other seeders have).
     In either case if there are no such pieces, revert to the simple answer.*/
  if( !tmpBitfield.IsEmpty() ) bf = tmpBitfield;
}

/* Find a peer with the given piece in its request queue.
   Duplicating a request queue that's in progress rather than creating a new
   one helps avoid requesting slices that we already have. */
btPeer *PeerList::WhoHas(bt_index_t idx) const
{
  PEERNODE *p;
  btPeer *peer = (btPeer *)0;

  for( p = m_head; p; p = p->next ){
    if( p->peer->request_q.HasIdx(idx) ){
      peer = p->peer;
      break;
    }
  }
  return peer;
}

int PeerList::HasSlice(bt_index_t idx, bt_offset_t off, bt_length_t len) const
{
  PEERNODE *p;

  for( p = m_head; p; p = p->next ){
    if( p->peer->request_q.HasSlice(idx, off, len) )
      break;
  }
  return p ? 1 : 0;
}

/* If another peer has the same slice requested first, move the proposer's
   slice to the last position for the piece. */
void PeerList::CompareRequest(btPeer *proposer, bt_index_t idx)
{
  PSLICE ps, qs;
  PEERNODE *p;
  dt_count_t qlen, count=0;

  ps = proposer->request_q.GetHead();
  for( ; ps && idx != ps->index; ps = ps->next );
  if( !ps ) return;

  qlen = proposer->request_q.Qlen(idx);

  do{
    for( p = m_head; p; p = p->next ){
      if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty() ) continue;
      qs = p->peer->request_q.GetHead();
      for( ; qs && idx != qs->index; qs = qs->next );
      if( qs && ps->index == qs->index && ps->offset == qs->offset &&
          ps->length == qs->length ){
        qs = ps->next;
        proposer->request_q.MoveLast(ps);
        ps = qs;
        break;
      }
    }
  }while( p && ++count < qlen );
}

int PeerList::CancelSlice(bt_index_t idx, bt_offset_t off, bt_length_t len)
{
  PEERNODE *p;
  int t, r=0;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    t = p->peer->CancelSliceRequest(idx, off, len);
    if( t ){
      r = 1;
      if( t < 0 ){
        if(arg_verbose) CONSOLE.Debug("close: CancelSlice");
        p->peer->CloseConnection();
      }
    }
  }
  return r;
}

int PeerList::CancelPiece(bt_index_t idx)
{
  PEERNODE *p;
  int t, r=0;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    t = p->peer->CancelPiece(idx);
    if( t ){
      r = 1;
      if( t < 0 ){
        if(arg_verbose) CONSOLE.Debug("close: CancelPiece");
        p->peer->CloseConnection();
      }
    }
  }
  return r;
}

// Cancel one peer's request for a specific piece.
void PeerList::CancelOneRequest(bt_index_t idx)
{
  PEERNODE *p;
  PSLICE ps;
  btPeer *peer = (btPeer *)0;
  int pending = 0;
  dt_count_t count, max = 0, dupcount = 0;

  if( PENDINGQUEUE.Exist(idx) ){
    pending = 1;
    dupcount++;
  }
  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    // select the peer with the most requests ahead of the target piece
    count = 0;
    ps = p->peer->request_q.GetHead();
    for( ; ps; ps = ps->next ){
      if( ps->index == idx ) break;
      else count++;
    }
    if( ps ){
      dupcount++;
      // in a tie, select the slower peer
      if( count > max || !peer ||
          (!pending && count == max &&
            p->peer->NominalDL() < peer->NominalDL()) ){
        peer = p->peer;
        max = count;
      }
    }
  }
  if( peer && dupcount > peer->request_q.Qlen(idx) ){
    if( pending ) PENDINGQUEUE.Delete(idx);
    else{
      CONSOLE.Debug("Cancel #%d on %p (%d B/s)", (int)idx, peer,
        (int)peer->NominalDL());
      peer->CancelPiece(idx);
    }
    if( dupcount == 2 ){  // was 2, now only 1
      m_dup_req_pieces--;
      CONSOLE.Debug("%d dup req pieces", (int)m_dup_req_pieces);
    }
  }
}

void PeerList::RecalcDupReqs()
{
  PEERNODE *p;
  PSLICE ps;
  bt_index_t idx;
  Bitfield rqbf, dupbf;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty() ) continue;
    ps = p->peer->request_q.GetHead();
    idx = BTCONTENT.GetNPieces();
    for( ; ps; ps = ps->next ){
      if( idx == ps->index ) continue;
      idx = ps->index;
      if( rqbf.IsSet(idx) ) dupbf.Set(idx);
      else{
        rqbf.Set(idx);
        if( PENDINGQUEUE.Exist(idx) ) dupbf.Set(idx);
      }
    }
  }
  m_dup_req_pieces = dupbf.Count();
  CONSOLE.Debug("recalc: %d dup req pieces", (int)m_dup_req_pieces);
}

void PeerList::Tell_World_I_Have(bt_index_t idx)
{
  PEERNODE *p;
  btPeer *peer;
  int r = 0, f_seed = 0;

  if( BTCONTENT.Seeding() ) f_seed = 1;

  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) ) continue;

    peer = p->peer;

    /* Send HAVE now to:
         all if we're now seeding or it's our first piece
         non-interested peers who need the piece
       Otherwise queue the HAVE to send later.
    */
    if( f_seed || BTCONTENT.pBF->Count() == 1 ||
        (!peer->Is_Remote_Interested() && !peer->bitfield.IsSet(idx)) ){
      if( f_seed ) r = (int)peer->SendHaves();
      if( r >= 0 )
        r = (int)peer->stream.Send_Have(idx);
    }else r = peer->QueueHave(idx);

    if( r < 0 )
      peer->CloseConnection();
    else if( f_seed ){
      // request queue is emptied by setting not-interested state
      if( peer->SetLocal(BT_MSG_NOT_INTERESTED) < 0 ){
        if(arg_verbose)
          CONSOLE.Debug("close: Can't set self not interested (T_W_I_H)");
        peer->CloseConnection();
      }
    }
  }  // end for
}

int PeerList::Accepter()
{
  SOCKET newsk;
  socklen_t addrlen;
  struct sockaddr_in addr;
  addrlen = sizeof(struct sockaddr_in);
  newsk = accept(m_listen_sock, (struct sockaddr *)&addr, &addrlen);
//  CONSOLE.Debug("incoming! %s:%hu",
//    inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

  if( INVALID_SOCKET == newsk ) return -1;

  if( AF_INET != addr.sin_family || addrlen != sizeof(struct sockaddr_in) ){
    CLOSE_SOCKET(newsk);
    return -1;
  }

  if( Tracker.IsQuitting() ){
    CLOSE_SOCKET(newsk);
    return -1;
  }

  return NewPeer(addr, newsk);
}

int PeerList::Initial_ListenPort()
{
  int r = 0;
  struct sockaddr_in lis_addr;
  memset(&lis_addr, 0, sizeof(sockaddr_in));
  lis_addr.sin_family = AF_INET;
  lis_addr.sin_addr.s_addr = INADDR_ANY;
  strcpy(m_listen, "n/a");

  m_listen_sock = socket(AF_INET, SOCK_STREAM, 0);

  if( INVALID_SOCKET == m_listen_sock ) return -1;

  if( cfg_listen_ip != 0 )
    lis_addr.sin_addr.s_addr = cfg_listen_ip;

  if( cfg_listen_port ){
    int opt = 1;
    setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    lis_addr.sin_port = htons(cfg_listen_port);
    if( bind(m_listen_sock, (struct sockaddr *)&lis_addr,
             sizeof(struct sockaddr_in)) == 0 ){
      r = 1;
    }else{
      opt = 0;
      setsockopt(m_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
      CONSOLE.Warning(2, "warn, couldn't bind on specified port %d:  %s",
        cfg_listen_port, strerror(errno));
    }
  }

  if( !r && (!cfg_listen_port || cfg_listen_port > 1025) ){
    r = -1;
    if( cfg_listen_port ){
      cfg_min_listen_port = cfg_listen_port -
                            (cfg_max_listen_port - cfg_min_listen_port);
      if( cfg_min_listen_port < 1025 ) cfg_min_listen_port = 1025;
      cfg_max_listen_port = cfg_listen_port;
    }
    cfg_listen_port = cfg_max_listen_port;
    while( r != 0 ){
      lis_addr.sin_port = htons(cfg_listen_port);
      r = bind(m_listen_sock, (struct sockaddr *)&lis_addr,
        sizeof(struct sockaddr_in));
      if( r != 0 ){
        cfg_listen_port--;
        if( cfg_listen_port < cfg_min_listen_port ){
          CLOSE_SOCKET(m_listen_sock);
          CONSOLE.Warning(1, "error, couldn't bind port from %d to %d:  %s",
            cfg_min_listen_port, cfg_max_listen_port, strerror(errno));
          return -1;
        }
      }
    }  // end while
  }

  if( listen(m_listen_sock, 5) == -1 ){
    CLOSE_SOCKET(m_listen_sock);
    CONSOLE.Warning(1, "error, couldn't listen on port %d: %s",
      cfg_listen_port, strerror(errno));
    return -1;
  }

  if( setfd_nonblock(m_listen_sock) < 0 ){
    CLOSE_SOCKET(m_listen_sock);
    CONSOLE.Warning(1, "error, couldn't set socket to nonblock mode.");
    return -1;
  }

  snprintf(m_listen, sizeof(m_listen), "%s:%hu",
    inet_ntoa(lis_addr.sin_addr), ntohs(lis_addr.sin_port));
  CONSOLE.Print("Listening on %s", m_listen);

  return 0;
}

bt_index_t PeerList::Pieces_I_Can_Get(Bitfield *ptmpBitfield) const
{
  if( !ptmpBitfield ){
    ptmpBitfield = new Bitfield(BTCONTENT.GetNPieces());
    if( !ptmpBitfield ) return 0;
  }

  if( m_seeds_count > 0 || BTCONTENT.IsFull() )
    ptmpBitfield->SetAll();
  else{
    PEERNODE *p;

    *ptmpBitfield = *BTCONTENT.pBF;

    for( p = m_head; p && !ptmpBitfield->IsFull(); p = p->next ){
      if( PEER_IS_SUCCESS(p->peer) )
        ptmpBitfield->Or(p->peer->bitfield);
    }
  }
  return ptmpBitfield->Count();
}

int PeerList::AlreadyRequested(bt_index_t idx) const
{
  PEERNODE *p;
  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty() ) continue;
    if( p->peer->request_q.HasIdx(idx) ) return 1;
  }
  return 0;
}

void PeerList::CheckBitfield(Bitfield &bf) const
{
  PEERNODE *p;
  PSLICE ps;
  bt_index_t idx;
  for( p = m_head; p; p = p->next ){
    if( !PEER_IS_SUCCESS(p->peer) || p->peer->request_q.IsEmpty() ) continue;
    ps = p->peer->request_q.GetHead();
    idx = BTCONTENT.GetNPieces();
    for( ; ps; ps = ps->next ){
      if( ps->index != idx ){
        bf.UnSet(ps->index);
        idx = ps->index;
      }
    }
  }
}

void PeerList::PrintOut() const
{
  PEERNODE *p = m_head;
  CONSOLE.Print("PEER LIST");
  for( ; p; p = p->next ){
    if( PEER_IS_FAILED(p->peer) ) continue;
    p->peer->Dump();
  }
}

void PeerList::AnyPeerReady(fd_set *rfdp, fd_set *wfdp, int *nready,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  PEERNODE *p, *pp = (PEERNODE *)0, *pnext;
  btPeer *peer = (btPeer *)0;
  SOCKET sk;
  int pready, pmoved;
  dt_count_t pcount = 0;

  if( FD_ISSET(m_listen_sock, rfdp) ){
    (*nready)--;
    if( !Self.OntimeDL() && !Self.OntimeUL() ){
      FD_CLR(m_listen_sock, rfdnextp);
      Accepter();
    }
  }

  if( !Self.OntimeDL() && (peer = GetNextUL()) &&
      !FD_ISSET(peer->stream.GetSocket(), wfdp) &&
      !BandwidthLimitUp(Self.LateUL()) ){
    if(arg_verbose) CONSOLE.Debug("%p is not write-ready", peer);
    peer->CheckSendStatus();
  }

  for( p = m_head;
       p && *nready;
       pp = pmoved ? pp : p, p = pnext ){
    pnext = p->next;
    pmoved = pready = 0;
    pcount++;
    if( PEER_IS_FAILED(p->peer) ) continue;

    peer = p->peer;
    sk = peer->stream.GetSocket();

    if( DT_PEER_SUCCESS == peer->GetStatus() ){
      if( FD_ISSET(sk, rfdp) ){
        (*nready)--;
        if( !Self.OntimeUL() ){
          pready = 1;
          m_readycnt++;
          FD_CLR(sk, rfdnextp);
          if( peer->RecvModule() < 0 ){
            if(arg_verbose) CONSOLE.Debug("close: receive");
            peer->CloseConnection();
          }else{
            if( pcount > Rank(peer) )
              pmoved = 1;
            peer->readycnt = m_readycnt;
          }
        }else if( m_head != p ) pmoved = 1;
        if( pmoved ){
          pp->next = p->next;
          p->next = m_head;
          m_head = p;
        }
      }
      if( !Self.OntimeDL() && !Self.OntimeUL() &&
          DT_PEER_SUCCESS == peer->GetStatus() && peer->HealthCheck() < 0 ){
        if(arg_verbose) CONSOLE.Debug("close: unhealthy");
        peer->CloseConnection();
      }
      if( PEER_IS_FAILED(peer) ){
        if( FD_ISSET(sk, wfdp) ) (*nready)--;
        FD_CLR(sk, wfdnextp);
      }
    }
    if( DT_PEER_SUCCESS == peer->GetStatus() ){
      if( FD_ISSET(sk, wfdp) ){
        (*nready)--;
        if( !Self.OntimeDL() ){
          if( !pready ) m_readycnt++;
          FD_CLR(sk, wfdnextp);
          if( peer->SendModule() < 0 ){
            if(arg_verbose) CONSOLE.Debug("close: send");
            peer->CloseConnection();
            FD_CLR(sk, rfdnextp);
          }else if( !pready ){
            if( pcount > Rank(peer) )
              pmoved = 1;
            peer->readycnt = m_readycnt;
          }
        }else if( m_head != p ) pmoved = 1;
        if( pmoved && m_head != p ){
          pp->next = p->next;
          p->next = m_head;
          m_head = p;
        }
      }
    }
    else if( DT_PEER_HANDSHAKE == peer->GetStatus() ){
      if( FD_ISSET(sk, rfdp) ){
        (*nready)--;
        if( !Self.OntimeDL() && !Self.OntimeUL() ){
          FD_CLR(sk, rfdnextp);
          if( peer->HandShake() < 0 ){
            if(arg_verbose) CONSOLE.Debug("close: receiving handshake");
            peer->CloseConnection();
            FD_CLR(sk, wfdnextp);
          }
        }
      }
      if( FD_ISSET(sk, wfdp) ){
        (*nready)--;
        if( !Self.OntimeDL() && !Self.OntimeUL() ){
          FD_CLR(sk, wfdnextp);
          if( peer->SendModule() < 0 ){
            if(arg_verbose) CONSOLE.Debug("close: flushing handshake");
            peer->CloseConnection();
            FD_CLR(sk, rfdnextp);
          }
        }
      }
    }
    else if( DT_PEER_CONNECTING == peer->GetStatus() ){
      if( FD_ISSET(sk, wfdp) ){
        (*nready)--;
        if( !Self.OntimeDL() && !Self.OntimeUL() ){
          FD_CLR(sk, wfdnextp);
          if( peer->Send_ShakeInfo() < 0 ){
            if(arg_verbose) CONSOLE.Debug("close: sending handshake");
            peer->CloseConnection();
            FD_CLR(sk, rfdnextp);
          }else peer->SetStatus(DT_PEER_HANDSHAKE);
        }
        if( FD_ISSET(sk, rfdp) ) (*nready)--;
      }else if( FD_ISSET(sk, rfdp) ){  // connect failed.
        (*nready)--;
        if( !Self.OntimeDL() && !Self.OntimeUL() ){
          FD_CLR(sk, rfdnextp);
          if(arg_verbose) CONSOLE.Debug("close: connect failed");
          peer->CloseConnection();
          FD_CLR(sk, wfdnextp);
        }
      }
    }
  }  // end for

  if( !m_ul_limited && !BandwidthLimitUp() ) m_missed_count++;
}

void PeerList::CloseAllConnectionToSeed()
{
  PEERNODE *p = m_head;
  for( ; p; p = p->next ){
    if( p->peer->bitfield.IsFull() ||
        /* Drop peers who remain uninterested, but keep recent connections.
           Peers who connected recently will resolve by bitfield exchange. */
        (PEER_IS_SUCCESS(p->peer) && !p->peer->Is_Remote_Interested() &&
          BTCONTENT.GetSeedTime() - now >= 300 &&
          !p->peer->ConnectedWhileSeed()) ){
      p->peer->DontWantAgain();
      if(arg_verbose) CONSOLE.Debug("close: seed<->seed");
      p->peer->CloseConnection();
    }
    else p->peer->stream.in_buffer.SetSize(BUF_DEF_SIZ);
  }
}

/* Find my 3 or 4 fastest peers.
   The m_max_unchoke+1 (4th) slot is for the optimistic unchoke when it
   happens. */
int PeerList::UnchokeCheck(btPeer *peer, btPeer *peer_array[])
{
  dt_count_t i = 0, cancel_idx = 0;
  btPeer *loser = (btPeer *)0;
  int f_seed = BTCONTENT.Seeding();
  int no_opt = 0;
  int retval = 0;

  if( m_opt_timestamp ) no_opt = 1;
  if( f_seed ) no_opt = 1 - no_opt;

  // Find a slot for the candidate--the slowest peer, or an available slot.
  for( cancel_idx = i = 0; i < m_max_unchoke + no_opt; i++ ){
    if( (btPeer *)0 == peer_array[i] ||
        PEER_IS_FAILED(peer_array[i]) ){
      cancel_idx = i;
      break;
    }else{
      if( cancel_idx == i ) continue;

      if( f_seed ){
        // compare time unchoked
        if( (!peer_array[i]->Is_Local_Unchoked() &&
            (peer_array[cancel_idx]->Is_Local_Unchoked() ||
              peer_array[cancel_idx]->GetLastUnchokeTime() <
                peer_array[i]->GetLastUnchokeTime())) ||
            (peer_array[i]->Is_Local_Unchoked() &&
             peer_array[cancel_idx]->Is_Local_Unchoked() &&
             peer_array[i]->GetLastUnchokeTime() <
               peer_array[cancel_idx]->GetLastUnchokeTime()) ){
          cancel_idx = i;
        }
      }else{
        // compare download rate.
        if( peer_array[cancel_idx]->RateDL() > peer_array[i]->RateDL() ||
          // If equal reciprocate to the peer we've sent less to, proportionally
            (peer_array[cancel_idx]->RateDL() == peer_array[i]->RateDL() &&
             peer_array[cancel_idx]->TotalUL() /
                 (peer_array[cancel_idx]->TotalDL()+.001) <
               peer_array[i]->TotalUL() / (peer_array[i]->TotalDL()+.001)) ){
          cancel_idx = i;
        }
      }
    }
  }  // end for

  if( (btPeer *)0 != peer_array[cancel_idx] &&
      PEER_IS_SUCCESS(peer_array[cancel_idx]) ){
    if( f_seed ){
      if( (!peer_array[cancel_idx]->Is_Local_Unchoked() &&
           (peer->Is_Local_Unchoked() ||
             peer->GetLastUnchokeTime() <
               peer_array[cancel_idx]->GetLastUnchokeTime())) ||
           (peer_array[cancel_idx]->Is_Local_Unchoked() &&
            peer->Is_Local_Unchoked() &&
            peer_array[cancel_idx]->GetLastUnchokeTime() <
              peer->GetLastUnchokeTime()) ){
        loser = peer_array[cancel_idx];
        peer_array[cancel_idx] = peer;
      }else
        loser = peer;
    }else{
      if( peer->RateDL() > peer_array[cancel_idx]->RateDL() ||
        // If equal reciprocate to the peer we've sent less to, proportionally
          (peer_array[cancel_idx]->RateDL() == peer->RateDL() &&
           peer_array[cancel_idx]->TotalUL() /
               (peer_array[cancel_idx]->TotalDL()+.001) >
             peer->TotalUL() / (peer->TotalDL()+.001)) ){
        loser = peer_array[cancel_idx];
        peer_array[cancel_idx] = peer;
      }else
        loser = peer;
    }

    // opt unchoke
    if( no_opt ){
      if( loser->SetLocal(BT_MSG_CHOKE) < 0 ){
        loser->CloseConnection();
        if( peer==loser ) retval = -1;
      }
    }
    else
    // The last slot is for the optimistic unchoke.
    if( (btPeer *)0 == peer_array[m_max_unchoke] ||
        PEER_IS_FAILED(peer_array[m_max_unchoke]) ){
      peer_array[m_max_unchoke] = loser;
    }else{
      // If loser is empty and current is not, loser gets 75% chance.
      if( loser->IsEmpty() && !peer_array[m_max_unchoke]->IsEmpty() &&
          RandBits(2) ){
        btPeer *tmp = peer_array[m_max_unchoke];
        peer_array[m_max_unchoke] = loser;
        loser = tmp;
      }else
        /* This mess chooses the loser:
           if loser is choked and current is not
           OR if both are choked and loser has waited longer
           OR if both are unchoked and loser has had less time unchoked. */
      if( (!loser->Is_Local_Unchoked() &&
            ( peer_array[m_max_unchoke]->Is_Local_Unchoked() ||
              loser->GetLastUnchokeTime() <
                peer_array[m_max_unchoke]->GetLastUnchokeTime() )) ||
          (loser->Is_Local_Unchoked() &&
            peer_array[m_max_unchoke]->Is_Local_Unchoked() &&
            peer_array[m_max_unchoke]->GetLastUnchokeTime() <
              loser->GetLastUnchokeTime()) ){
        /* If current is empty and loser is not, loser gets 25% chance;
              else loser wins.
           transformed to: if loser is empty or current isn't, or 25% chance,
              then loser wins. */
        if( !peer_array[m_max_unchoke]->IsEmpty() || loser->IsEmpty() ||
            !RandBits(2) ){
          btPeer *tmp = peer_array[m_max_unchoke];
          peer_array[m_max_unchoke] = loser;
          loser = tmp;
        }
      }
      if( loser->SetLocal(BT_MSG_CHOKE) < 0 ){
        loser->CloseConnection();
        if( peer==loser ) retval = -1;
      }
    }
  }else  // else if( (btPeer *)0 != peer_array[cancel_idx] ...)
    peer_array[cancel_idx] = peer;

  return retval;
}

/* When we change what we're going after, we need to evaluate & set our
   interest with each peer appropriately. */
void PeerList::CheckInterest()
{
  PEERNODE *p = m_head;
  for( ; p; p = p->next ){
    /* Don't shortcut by checking Is_Local_Interested(), as we need to let
       SetLocal() reset the m_standby flag. */
    if( p->peer->Need_Remote_Data() ){
      if( p->peer->SetLocal(BT_MSG_INTERESTED) < 0 )
        p->peer->CloseConnection();
    }else{
      if( p->peer->SetLocal(BT_MSG_NOT_INTERESTED) < 0 )
        p->peer->CloseConnection();
    }
  }
}

btPeer *PeerList::GetNextPeer(const btPeer *peer) const
{
  static PEERNODE *p = m_head;

  if( !peer ) p = m_head;
  else if( p && p->peer == peer ){
    p = p->next;
  }else{
    for( p = m_head; p && (p->peer != peer); p = p->next );
    if( p ) p = p->next;
    else p = m_head;
  }
  for( ; p; p = p->next )
    if( p->peer && PEER_IS_SUCCESS(p->peer) ) break;

  if( p ) return p->peer;
  else return (btPeer *)0;
}

int PeerList::Endgame()
{
  Bitfield tmpBitfield;
  int endgame = 0;

  tmpBitfield = *BTCONTENT.pBF;
  tmpBitfield.Invert();                       // what I don't have...
  tmpBitfield.Except(BTCONTENT.GetFilter());  // ...that I want
  if( tmpBitfield.Count() > 0 &&
      tmpBitfield.Count() < m_peers_count - m_conn_count ){
    endgame = 1;
  }else{
    Pieces_I_Can_Get(&tmpBitfield);             // what's available...
    tmpBitfield.Except(BTCONTENT.GetFilter());  // ...that I want...
    tmpBitfield.Except(*BTCONTENT.pBF);         // ...that I don't have
    if( tmpBitfield.Count() > 0 &&
        tmpBitfield.Count() < m_peers_count - m_conn_count){
      endgame = 1;
    }
  }

  if( endgame && !m_endgame ){
    if(arg_verbose) CONSOLE.Debug("Endgame (dup request) mode");
    UnStandby();
  }else if( !endgame && m_endgame ){
    if(arg_verbose) CONSOLE.Debug("Normal (non dup request) mode");
    RecalcDupReqs();  // failsafe
  }

  m_endgame = endgame;
  return endgame;
}

void PeerList::UnStandby()
{
  PEERNODE *p = m_head;
  for( ; p; p = p->next ){
    if( PEER_IS_SUCCESS(p->peer) ) p->peer->UnStandby();
  }
}

void PeerList::Pause()
{
  PEERNODE *p = m_head;

  m_f_pause = 1;
  StopDownload();
  for( ; p; p = p->next ){
    if( p->peer->Is_Local_Unchoked() && p->peer->SetLocal(BT_MSG_CHOKE) < 0 )
      p->peer->CloseConnection();
  }
}

void PeerList::Resume()
{
  m_f_pause = 0;
  CheckInterest();
}

void PeerList::StopDownload()
{
  PEERNODE *p = m_head;

  for( ; p; p = p->next ){
    if( p->peer->SetLocal(BT_MSG_NOT_INTERESTED) < 0 ){
      p->peer->CloseConnection();
    }else p->peer->PutPending();
  }
}

dt_count_t PeerList::GetUnchoked() const
{
  PEERNODE *p;
  dt_count_t count = 0;

  for( p = m_head; p; p = p->next ){
    if( PEER_IS_SUCCESS(p->peer) && p->peer->Is_Local_Unchoked() ){
      count++;
      if( count > m_max_unchoke ) break;
    }
  }
  return count;
}

/* This function returns 0 if it could not find an upload faster than the
   minimum and all peer upload rates are known (not zero). */
dt_rate_t PeerList::GetSlowestUp(dt_rate_t minimum) const
{
  PEERNODE *p;
  dt_rate_t slowest = 0, rate;
  int zero = 0;
  dt_count_t unchoked = 0;

  for( p = m_head; p; p = p->next ){
    if( PEER_IS_SUCCESS(p->peer) && p->peer->Is_Local_Unchoked() ){
      unchoked++;
      rate = p->peer->RateUL();
      if( 0==rate ) zero = 1;
      else if( rate >= minimum && (rate < slowest || 0==slowest) )
        slowest = rate;
    }
  }
  if( slowest > (rate = Self.RateUL()) ) slowest = rate;

  // We're looking for slow, so guess low when we must guess a rate.
  if( slowest ){
    if( zero ) return minimum ? minimum : ((slowest+1)/2);
    else return slowest;
  }else{
    if( 0==unchoked ) unchoked = 1;  // safeguard
    if( zero ) return minimum ? minimum :
                             ((rate = Self.RateUL()) ? rate / unchoked : 1);
    else return 0;
  }
}

int PeerList::BandwidthLimited(double lasttime, bt_length_t lastsize,
  dt_rate_t limit, double grace) const
{
  int limited = 0;
  double nexttime;

  if( limit == 0 ) return 0;

  nexttime = lasttime + (double)lastsize / limit - grace;
  if( nexttime >= (double)(now + 1) ) limited = 1;
  else if( nexttime <= (double)now ) limited = 0;
  else if( nexttime <= PreciseTime() ) limited = 0;
  else limited = 1;

  return limited;
}

dt_idle_t PeerList::IdleState() const
{
  dt_idle_t idle;
  double dnext, unext, rightnow;
  int dlimnow, dlimthen, ulimnow, ulimthen;

  if( cfg_max_bandwidth_down == 0 && cfg_max_bandwidth_up == 0 )  // no limits
    return DT_IDLE_POLLING;

  rightnow = PreciseTime();

  if( cfg_max_bandwidth_down > 0 ){
    dnext = Self.LastRecvTime() +
            (double)Self.LastSizeRecv() / cfg_max_bandwidth_down;
    dlimnow = (dnext > rightnow);
    dlimthen = (dnext > rightnow + Self.LateDL());
  }else dlimnow = dlimthen = 0;

  if( cfg_max_bandwidth_up > 0 ){
    unext = Self.LastSendTime() +
            (double)Self.LastSizeSent() / cfg_max_bandwidth_up;
    ulimnow = (unext > rightnow);
    ulimthen = (unext > rightnow + Self.LateUL());
  }else ulimnow = ulimthen = 0;

  if( dlimthen && ulimthen )
    idle = DT_IDLE_IDLE;
  else if( (dlimnow && !dlimthen) || (ulimnow && !ulimthen) )
    idle = DT_IDLE_NOTIDLE;
  else idle = DT_IDLE_POLLING;

  return idle;
}

int PeerList::IsIdle() const
{
  switch( IdleState() ){
  case DT_IDLE_NOTIDLE:
    return 0;
  case DT_IDLE_IDLE:
    return 1;
  default:
    return m_f_idled ? 0 : 1;
  }
}

void PeerList::SetIdled()
{
  m_f_idled = 1;
  time(&now);
}

// How long must we wait for bandwidth to become available in either direction?
double PeerList::WaitBW() const
{
  double rightnow, late;
  double maxwait = 0, nextwake = 0;
  double nextup = 0, nextdn = 0;
  int use_up = 0, use_dn = 0;

  if( cfg_max_bandwidth_up > 0){
    nextup = Self.LastSendTime() +
             (double)Self.LastSizeSent() / cfg_max_bandwidth_up;
  }
  if( cfg_max_bandwidth_down > 0 ){
    nextdn = Self.LastRecvTime() +
             (double)Self.LastSizeRecv() / cfg_max_bandwidth_down;
  }

  // could optimize away the clock call when maxwait will be > MAX_SLEEP
  if( now <= (time_t)nextup || now <= (time_t)nextdn )
    rightnow = PreciseTime();
  else rightnow = (double)now;

  if( nextup >= rightnow ){
    if( nextdn < rightnow ) use_up = 1;
    else if( nextdn < nextup ) use_dn = 1;
    else use_up = 1;
  }else if( nextdn >= rightnow ) use_dn = 1;

  if( use_up ){
    nextwake = nextup;
    late = Self.LateUL();
  }else if( use_dn ){
    nextwake = nextdn;
    late = Self.LateDL();
  }else{
    nextwake = late = 0;
  }

  if( (m_f_limitd && nextdn <= rightnow + Self.LateDL()) ||
      (m_f_limitu && nextup <= rightnow + Self.LateUL()) ){
    // socket setup is outdated; send a problem indicator value back
    Self.OntimeUL(0);
    Self.OntimeDL(0);
    maxwait = -100;
  }else if( nextwake > rightnow ){
    maxwait = nextwake - rightnow - late;
    if( maxwait < 0 ){
      use_up = use_dn = 0;
    }
    Self.OntimeUL(use_up);
    Self.OntimeDL(use_dn);
//  CONSOLE.Debug("waitbw %f at %f", maxwait, rightnow);
  }else{
    Self.OntimeUL(0);
    Self.OntimeDL(0);
//  CONSOLE.Debug("nextwake %f at %f", nextwake, rightnow);
  }
  return maxwait;
}

void PeerList::UnchokeIfFree(btPeer *peer)
{
  PEERNODE *p;
  dt_count_t count = 0;

  if( m_f_pause ) return;
  for( p = m_head; p; p = p->next ){
    if( PEER_IS_SUCCESS(p->peer) && p->peer->Is_Local_Unchoked() &&
        p->peer->Is_Remote_Interested() ){
      count++;
      if( m_max_unchoke < count ) return;
    }
  }
  if( peer->SetLocal(BT_MSG_UNCHOKE) < 0 ) peer->CloseConnection();
}

void PeerList::AdjustPeersCount()
{
  Tracker.AdjustPeersCount();
}

void PeerList::WaitBWQueue(PEERNODE **queue, btPeer *peer)
{
  PEERNODE *p = *queue, *pp = (PEERNODE *)0, *node;

  for( ; p; pp = p, p = p->next ){
    if( peer == p->peer ) return;
  }
  if( (node = new PEERNODE) ){
    node->next = (PEERNODE *)0;
    node->peer = peer;
    if( pp ) pp->next = node;
    else *queue = node;
  }
}

void PeerList::BWReQueue(PEERNODE **queue, btPeer *peer)
{
  PEERNODE *p = *queue, *pp = (PEERNODE *)0, *node = (PEERNODE *)0;

  for( ; p; pp = p, p = p->next ){
    if( node ) continue;
    if( peer == p->peer ){
      if( !p->next ) return;
      if( pp ) pp->next = p->next;
      else *queue = p->next;
      p->next = (PEERNODE *)0;
      node = p;
    }
  }
  pp->next = node;
}

void PeerList::DontWaitBWQueue(PEERNODE **queue, const btPeer *peer)
{
  PEERNODE *p = *queue, *pp = (PEERNODE *)0;

  for( ; p; pp = p, p = p->next ){
    if( peer == p->peer ){
      if( pp ) pp->next = p->next;
      else *queue = p->next;
      delete p;
      return;
    }
  }
}

