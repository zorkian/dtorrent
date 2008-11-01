#ifndef PEERLIST_H
#define PEERLIST_H

#include "def.h"
#include <sys/types.h>
#include <time.h>

#include "bttypes.h"
#include "peer.h"
#include "rate.h"

enum dt_idle_t{
  DT_IDLE_NOTIDLE,
  DT_IDLE_IDLE,
  DT_IDLE_POLLING
};

typedef struct _peernode{
  btPeer *peer;
  struct _peernode *next;
}PEERNODE;

class PeerList
{
 private:
  SOCKET m_listen_sock;
  PEERNODE *m_head, *m_dead;
  PEERNODE *m_next_dl, *m_next_ul;  // UL/DL rotation queues
  dt_count_t m_peers_count, m_seeds_count, m_conn_count, m_downloads;
  dt_count_t m_max_unchoke;
  time_t m_unchoke_check_timestamp, m_keepalive_check_timestamp,
         m_last_progress_timestamp, m_opt_timestamp, m_interval_timestamp;
  time_t m_unchoke_interval, m_opt_interval;
  dt_count_t m_defer_count, m_missed_count, m_upload_count, m_up_opt_count;
  dt_count_t m_dup_req_pieces;
  dt_rate_t m_prev_limit_up;
  char m_listen[22];
  dt_count_t m_readycnt;      // cumulative count of ready peers
  uint16_t m_max_listen_port;
  uint16_t m_min_listen_port;

  unsigned char m_ul_limited:1;
  unsigned char m_f_pause:1;
  unsigned char m_f_limitd:1;
  unsigned char m_f_limitu:1;
  unsigned char m_endgame:1;
  unsigned char m_f_idled:1;     // idle action taken this cycle
  unsigned char m_reserved:2;

  int InitialListenPort();
  int Accepter();
  int UnchokeCheck(btPeer *peer, btPeer *peer_array[]);
  int FillFDSet(fd_set *rfd, fd_set *wfd, int f_keepalive_check,
    int f_unchoke_check, btPeer **UNCHOKER);
  void WaitBWQueue(PEERNODE **queue, btPeer *peer);
  void BWReQueue(PEERNODE **queue, btPeer *peer);
  void DontWaitBWQueue(PEERNODE **queue, const btPeer *peer);
  dt_count_t Rank(const btPeer *peer) const {
    return (m_readycnt > peer->readycnt) ? m_readycnt - peer->readycnt : 3;
  }

 public:
  PeerList();
  ~PeerList();

  // TotalPeers() is now GetPeersCount() for consistency
  int Init();
  const char *GetListen() const { return m_listen; }

  int IsEmpty() const { return m_peers_count ? 0 : 1; }

  void PrintOut() const;

  int NewPeer(struct sockaddr_in addr, SOCKET sk);

  void CloseAllConnectionToSeed();
  void CloseAll();

  int IntervalCheck(fd_set *rfd, fd_set *wfd);

  void SetUnchokeIntervals();
  void AnyPeerReady(fd_set *rfdp, fd_set *wfdp, int *nready,
    fd_set *rfdnextp, fd_set *wfdnextp);

  int BandwidthLimitUp(double grace=0) const {
    return BandwidthLimited(Self.LastSendTime(), Self.LastSizeSent(),
                            *cfg_max_bandwidth_up, grace);
  }
  int BandwidthLimitDown(double grace=0) const {
    return BandwidthLimited(Self.LastRecvTime(), Self.LastSizeRecv(),
                            *cfg_max_bandwidth_down, grace);
  }
  int BandwidthLimited(double lasttime, bt_length_t lastsize, dt_rate_t limit,
    double grace) const;
  double WaitBW() const;
  void DontWaitBW(){ Self.OntimeUL(0); Self.OntimeDL(0); }

  // Rotation queue management
  void WaitDL(btPeer *peer){ WaitBWQueue(&m_next_dl, peer); }
  void WaitUL(btPeer *peer){ WaitBWQueue(&m_next_ul, peer); }
  int IsNextDL(const btPeer *peer) const {
    return (!m_next_dl || peer == m_next_dl->peer) ? 1 : 0;
  }
  int IsNextUL(const btPeer *peer) const {
    return (!m_next_ul || peer == m_next_ul->peer) ? 1 : 0;
  }
  btPeer *GetNextDL(){ return m_next_dl ? m_next_dl->peer : (btPeer *)0; }
  btPeer *GetNextUL(){ return m_next_ul ? m_next_ul->peer : (btPeer *)0; }
  void ReQueueDL(btPeer *peer){ BWReQueue(&m_next_dl, peer); }
  void ReQueueUL(btPeer *peer){ BWReQueue(&m_next_ul, peer); }
  void DontWaitDL(const btPeer *peer){ DontWaitBWQueue(&m_next_dl, peer); }
  void DontWaitUL(const btPeer *peer){ DontWaitBWQueue(&m_next_ul, peer); }

  void Tell_World_I_Have(bt_index_t idx);
  btPeer *Who_Can_Abandon(btPeer *proposer);
  bt_index_t What_Can_Duplicate(Bitfield &bf, const btPeer *proposer,
    bt_index_t idx);
  void FindValuedPieces(Bitfield &bf, const btPeer *proposer, int initial)
    const;
  btPeer *WhoHas(bt_index_t idx) const;
  int HasSlice(bt_index_t idx, bt_offset_t off, bt_length_t len) const;
  void CompareRequest(btPeer *proposer, bt_index_t idx);
  int CancelSlice(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int CancelPiece(bt_index_t idx);
  void CancelOneRequest(bt_index_t idx);

  void CheckBitfield(Bitfield &bf) const;
  int AlreadyRequested(bt_index_t idx) const;
  bt_index_t Pieces_I_Can_Get(Bitfield *ptmpBitfield=(Bitfield *)0) const;
  void CheckInterest();
  btPeer *GetNextPeer(const btPeer *peer) const;
  int Endgame();
  void UnStandby();

  dt_count_t GetDupReqs() const { return m_dup_req_pieces; }
  void RecalcDupReqs();

  dt_count_t GetSeedsCount() const { return m_seeds_count; }
  dt_count_t GetPeersCount() const { return m_peers_count; }
  dt_count_t GetConnCount() const { return m_conn_count; }
  void AdjustPeersCount();  // passthrough to tracker function

  dt_count_t GetUnchoked() const;
  dt_rate_t GetSlowestUp(dt_rate_t minimum) const;
  dt_count_t GetDownloads() const { return m_downloads; }
  time_t GetUnchokeInterval() const { return m_unchoke_interval; }

  void Defer(){ m_defer_count++; }
  void Upload(){ m_upload_count++; }

  dt_idle_t IdleState() const;
  int IsIdle() const;
  void SetIdled();
  void ClearIdled(){ m_f_idled = 0; }
  int Idled() const { return m_f_idled ? 1 : 0; }
  void Pause();
  void Resume();
  int IsPaused() const { return m_f_pause ? 1 : 0; }
  void StopDownload();

  void UnchokeIfFree(btPeer *peer);
};

extern PeerList WORLD;

#endif  // PEERLIST_H

