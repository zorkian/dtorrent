#ifndef PEER_H
#define PEER_H

#include "./def.h"

#ifdef WINDOWS
#include <Winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#endif

#include <time.h>

#include "btrequest.h"
#include "btstream.h"
#include "bitfield.h"
#include "rate.h"
#include "btconfig.h"

#define P_CONNECTING (unsigned char) 0		// connecting
#define P_HANDSHAKE  (unsigned char) 1		// handshaking
#define P_SUCCESS (unsigned char) 2		// successful
#define P_FAILED (unsigned char) 3		// failed

typedef struct _btstatus{
  unsigned char remote_choked:1;
  unsigned char remote_interested:1;
  unsigned char local_choked:1;
  unsigned char local_interested:1;

  unsigned char reserved:4;		/* unused */
}BTSTATUS;

size_t get_nl(char *from);
void set_nl(char *to, size_t from);

int TextPeerID(unsigned char *peerid, char *txtid);

class btBasic
{
private:
  Rate rate_dl;
  Rate rate_ul;
  size_t m_current_dl, m_current_ul;
  unsigned char m_use_current:1;
  unsigned char m_reserved:7;
public:
  struct sockaddr_in m_sin;

  btBasic() { m_use_current = 0; }

  //IP地址相关函数
  int IpEquiv(struct sockaddr_in addr);
  void SetIp(struct sockaddr_in addr);
  void SetAddress(struct sockaddr_in addr);
  void GetAddress(struct sockaddr_in *psin) const {
    memcpy(psin,&m_sin,sizeof(struct sockaddr_in));
  }

  // 速率相关函数
  Rate GetDLRate() const { return rate_dl; }
  Rate GetULRate() const { return rate_ul; }
  
  u_int64_t TotalDL() const { return rate_dl.Count(); }
  u_int64_t TotalUL() const { return rate_ul.Count(); }

  void DataRecved(size_t nby) { rate_dl.CountAdd(nby); }
  void DataSended(size_t nby) { rate_ul.CountAdd(nby); }

  size_t RateDL() const { return m_use_current ? m_current_dl : rate_dl.RateMeasure(); }
  size_t RateUL() const { return m_use_current ? m_current_ul : rate_ul.RateMeasure(); }
  void SetCurrentRates();
  void ClearCurrentRates() { m_use_current = 0; }

  void StartDLTimer() { rate_dl.StartTimer(); }
  void StartULTimer() { rate_ul.StartTimer(); }
  void StopDLTimer() { rate_dl.StopTimer(); }
  void StopULTimer() { rate_ul.StopTimer(); }
  void ResetDLTimer() { rate_dl.Reset(); }
  void ResetULTimer() { rate_ul.Reset(); }
};

class btPeer:public btBasic
{
 private:
  time_t m_last_timestamp, m_unchoke_timestamp;

  unsigned char m_f_keepalive:1;
  unsigned char m_status:4;
  unsigned char m_bad_health:1;
  unsigned char m_standby:1;
  unsigned char m_want_again:1;  // attempt reconnect if lost

  unsigned char m_connect:1;     // we initiated the connection
  unsigned char m_connect_seed:1; // connected while I am seed
  unsigned char m_reserved:6;

  BTSTATUS m_state;

  size_t m_cached_idx;
  size_t m_err_count;
  size_t m_req_send;  // target number of outstanding requests
  size_t m_req_out;   // actual number of outstanding requests
  size_t m_latency;
  size_t m_prev_dlrate;
  time_t m_latency_timestamp;
  time_t m_health_time, m_receive_time;
  char m_lastmsg;
  time_t m_choketime;
  
  int PieceDeliver(size_t mlen);
  int ReportComplete(size_t idx);
  int RequestCheck();
  int SendRequest();
  int CancelPiece();
  int CancelRequest(PSLICE ps);
  int ReponseSlice();
  int RequestPiece();
  int MsgDeliver();
  int CouldReponseSlice();

  int BandWidthLimit();
  int BandWidthLimitUp();
  int BandWidthLimitDown();
 public:
  unsigned char id[PEER_ID_LEN];
  BitField bitfield;
  btStream stream;
  RequestQueue request_q;
  RequestQueue reponse_q;

  btPeer();

  int RecvModule();
  int SendModule();
  int HealthCheck(time_t now);
  void CheckSendStatus();

  time_t SetLastTimestamp() { return time(&m_last_timestamp); }
  time_t GetLastTimestamp() const { return m_last_timestamp; }
  time_t SetLastUnchokeTime() { return time(&m_unchoke_timestamp); }
  time_t GetLastUnchokeTime() const { return m_unchoke_timestamp; }

  int Is_Remote_Interested() const { return m_state.remote_interested ? 1 : 0; }
  int Is_Remote_UnChoked() const { return m_state.remote_choked ? 0 : 1; }
  int Is_Local_Interested() const { return m_state.local_interested ? 1 : 0;}
  int Is_Local_UnChoked() const { return m_state.local_choked ? 0 : 1; }
  int SetLocal(unsigned char s);

  int IsEmpty() const;

  int CancelSliceRequest(size_t idx, size_t off, size_t len);
  
  void SetStatus(unsigned char s){ m_status = s; }
  unsigned char GetStatus() const { return m_status; }
  int NeedWrite();
  int NeedRead();

  
  void CloseConnection();
  int WantAgain() const { return (m_connect && m_want_again) ? 1 : 0; }
  void DontWantAgain() { m_want_again = 0; }
  void SetConnect() { m_connect = 1; }

  int ConnectedWhileSeed() { return m_connect_seed; }
  
  int AreYouOK();
  int Send_ShakeInfo();
  int HandShake();

  int Need_Remote_Data();
  int Need_Local_Data();

  int PutPending();

  void dump();
};

extern btBasic Self;

#endif
