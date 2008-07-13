#ifndef PEER_H
#define PEER_H

#include "def.h"

#ifdef WINDOWS
#include <Winsock2.h>
#else
#include <unistd.h>
#include <stdio.h>   // autoconf manual: Darwin + others prereq for stdlib.h
#include <stdlib.h>  // autoconf manual: Darwin prereq for sys/socket.h
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <inttypes.h>
#include <time.h>
#include <string.h>

#include "bttypes.h"
#include "btrequest.h"
#include "btstream.h"
#include "bitfield.h"
#include "rate.h"
#include "btconfig.h"

enum dt_peerstatus_t{
  DT_PEER_CONNECTING,
  DT_PEER_HANDSHAKE,
  DT_PEER_SUCCESS,
  DT_PEER_FAILED
};

#define HAVEQ_SIZE 5

typedef struct _btstatus{
  unsigned char remote_choked:1;
  unsigned char remote_interested:1;
  unsigned char local_choked:1;
  unsigned char local_interested:1;
  unsigned char reserved:4;  // unused
}BTSTATUS;

int TextPeerID(const unsigned char *peerid, char *txtid);

class btBasic
{
protected:
  struct sockaddr_in m_sin;
  Rate rate_dl;
  Rate rate_ul;

private:

public:
  int IpEquiv(struct sockaddr_in addr);
  void SetIp(struct sockaddr_in addr);
  void SetAddress(struct sockaddr_in addr);
  void GetAddress(struct sockaddr_in *psin) const {
    memcpy(psin, &m_sin, sizeof(struct sockaddr_in));
  }

  const Rate &GetDLRate() const { return rate_dl; }
  const Rate &GetULRate() const { return rate_ul; }
  void SetDLRate(Rate rate){ rate_dl = rate; StopDLTimer(); }
  void SetULRate(Rate rate){ rate_ul = rate; StopULTimer(); }

  dt_datalen_t TotalDL() const { return rate_dl.Count(); }
  dt_datalen_t TotalUL() const { return rate_ul.Count(); }

  void DataRecd(bt_length_t nby){ rate_dl.CountAdd(nby); }
  void DataUnRec(bt_length_t nby){ rate_dl.UnCount(nby); }
  void DataSent(bt_length_t nby, double timestamp){
    rate_ul.CountAdd(nby);
    rate_ul.RateAdd(nby, cfg_max_bandwidth_up, timestamp);
  }

  dt_rate_t CurrentDL(){ return rate_dl.CurrentRate(); }
  dt_rate_t CurrentUL(){ return rate_ul.CurrentRate(); }
  dt_rate_t RateDL(){ return rate_dl.RateMeasure(); }
  dt_rate_t RateUL(){ return rate_ul.RateMeasure(); }

  dt_rate_t NominalDL(){ return rate_dl.NominalRate(); }
  dt_rate_t NominalUL(){ return rate_ul.NominalRate(); }

  void StartDLTimer(){ rate_dl.StartTimer(); }
  void StartULTimer(){ rate_ul.StartTimer(); }
  void StopDLTimer(){ rate_dl.StopTimer(); }
  void StopULTimer(){ rate_ul.StopTimer(); }
  void ResetDLTimer(){ rate_dl.Reset(); }
  void ResetULTimer(){ rate_ul.Reset(); }

  double LastSendTime() const { return rate_ul.LastRealtime(); }
  double LastRecvTime() const { return rate_dl.LastRealtime(); }
  bt_length_t LastSizeSent() const { return rate_ul.LastSize(); }
  bt_length_t LastSizeRecv() const { return rate_dl.LastSize(); }

  Rate *DLRatePtr(){ return &rate_dl; }
  Rate *ULRatePtr(){ return &rate_ul; }

  double LateDL() const { return rate_dl.Late(); }
  double LateUL() const { return rate_ul.Late(); }
  int OntimeDL() const { return rate_dl.Ontime(); }
  int OntimeUL() const { return rate_ul.Ontime(); }
  void OntimeDL(int yn){ rate_dl.Ontime(yn); }
  void OntimeUL(int yn){ rate_ul.Ontime(yn); }
};

class btPeer:public btBasic
{
 private:
  time_t m_last_timestamp, m_unchoke_timestamp;
  dt_peerstatus_t m_status;

  unsigned char m_f_keepalive:1;
  unsigned char m_bad_health:1;
  unsigned char m_standby:1;              // nothing to request at this time
  unsigned char m_want_again:1;           // attempt reconnect if lost
  unsigned char m_reserved:4;

  unsigned char m_connect:1;              // we initiated the connection
  unsigned char m_retried:1;              // already retried connecting
  unsigned char m_connect_seed:1;         // connected while I am seed
  unsigned char m_requested:1;            // received a request since unchoke
  unsigned char m_prefetch_completion:2;  // prefetched for piece completion
  unsigned char m_deferred_dl:1;          // peer has deferred DL to me
  unsigned char m_deferred_ul:1;          // peer has deferred UL to me

  BTSTATUS m_state;

  bt_index_t m_cached_idx;
  int m_err_count;
  dt_count_t m_req_send;  // target number of outstanding requests
  dt_count_t m_req_out;   // actual number of outstanding requests
  time_t m_latency;
  dt_rate_t m_prev_dlrate;
  time_t m_latency_timestamp;
  time_t m_health_time, m_receive_time, m_next_send_time;
  bt_msg_t m_lastmsg;
  time_t m_choketime;
  time_t m_prefetch_time;
  time_t m_cancel_time;
  bt_index_t m_last_req_piece;
  bt_index_t m_haveq[HAVEQ_SIZE];  // need to send HAVE for these pieces

  int PieceDeliver(bt_int_t mlen);
  int ReportComplete(bt_index_t idx, bt_length_t len);
  int RequestCheck();
  int SendRequest();
  int RespondSlice();
  int RequestPiece();
  int MsgDeliver();
  int CouldRespondSlice();
  int RequestSlice(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int PeerError(int weight, const char *message);

 public:
  unsigned char id[PEER_ID_LEN];
  Bitfield bitfield;
  btStream stream;
  RequestQueue request_q;
  RequestQueue respond_q;
  unsigned long readycnt;  // copy of peerlist m_readycnt at last ready time

  btPeer();

  void CopyStats(btPeer *peer);

  int RecvModule();
  int SendModule();
  int HealthCheck();
  void CheckSendStatus();
  void UnStandby(){ m_standby = 0; }

  void SetLastTimestamp(){ time(&m_last_timestamp); }
  time_t GetLastTimestamp() const { return m_last_timestamp; }
  time_t GetLastUnchokeTime() const { return m_unchoke_timestamp; }

  int Is_Remote_Interested() const { return m_state.remote_interested ? 1 : 0; }
  int Is_Remote_Unchoked() const { return m_state.remote_choked ? 0 : 1; }
  int Is_Local_Interested() const { return m_state.local_interested ? 1 : 0; }
  int Is_Local_Unchoked() const { return m_state.local_choked ? 0 : 1; }
  int SetLocal(bt_msg_t s);

  int IsEmpty() const;

  int CancelRequest();
  int CancelSliceRequest(bt_index_t idx, bt_offset_t off, bt_length_t len);
  int CancelPiece(bt_index_t idx);
  bt_index_t FindLastCommonRequest(Bitfield &proposerbf);

  void SetStatus(dt_peerstatus_t s){ m_status = s; }
  dt_peerstatus_t GetStatus() const { return m_status; }
  int NeedWrite(int limited);
  int NeedRead(int limited);

  void CloseConnection();
  int CanReconnect() const {
    return (m_connect && m_want_again && !m_retried) ? 1 : 0;
  }
  int WantAgain() const { return m_want_again ? 1 : 0; }
  void DontWantAgain(){ m_want_again = 0; }
  void SetConnect(){ m_connect = 1; }
  void Retry(){ m_retried = 1; }
  int Retried() const { return m_retried ? 1 : 0; }

  int ConnectedWhileSeed() const { return m_connect_seed ? 1 : 0; }

  int AreYouOK();
  int Send_ShakeInfo();
  int HandShake();

  int Need_Remote_Data() const;
  int Need_Local_Data() const;

  void PutPending();

  int NeedPrefetch() const;
  int Prefetch(time_t deadline);

  void DeferDL(){ m_deferred_dl = 1; }
  void DeferUL(){ m_deferred_ul = 1; }

  int SendHaves();
  int QueueHave(bt_index_t idx);

  void dump();
};

extern btBasic Self;

#endif  // PEER_H

