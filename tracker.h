#ifndef TRACKER_H
#define TRACKER_H

#include "def.h"
#include <sys/types.h>
#include <inttypes.h>

#include <unistd.h>
#include <netdb.h>   // Solaris defines MAXHOSTNAMELEN here.
#include <stdio.h>   // autoconf manual: Darwin + others prereq for stdlib.h
#include <stdlib.h>  // autoconf manual: Darwin prereq for sys/socket.h
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>

#include <time.h>

#include "btconfig.h"
#include "bttypes.h"
#include "bufio.h"

enum dt_trackerstatus_t{
  DT_TRACKER_FREE,
  DT_TRACKER_CONNECTING,
  DT_TRACKER_READY,
  DT_TRACKER_FINISHED
};

class btTracker
{
  struct tracker_spec{
    char *url, *host, *request;
    int port;
    tracker_spec(){ url = host = request = (char *)0; }
    ~tracker_spec(){
      if( url ) delete []url;
      if( host ) delete []host;
      if( request ) delete []request;
    }
  };
 private:
  tracker_spec *m_spec;
  tracker_spec *m_redirect;  // preserves original spec when redirected

  char m_key[9];
  char m_trackerid[PEER_ID_LEN+1];
  dt_trackerstatus_t m_status;
  dt_result_t m_result;

  unsigned char m_f_started:1;
  unsigned char m_f_completed:1;
  unsigned char m_f_stop:1;
  unsigned char m_f_restart:1;
  unsigned char m_f_boguspeercnt:1;
  unsigned char m_reserved:3;

  time_t m_interval;          // time from previous to next tracker contact
  time_t m_default_interval;  // interval that the tracker tells us to wait
  time_t m_last_timestamp;    // time of last tracker contact attempt

  dt_count_t m_refuse_click;  // connection-refused counter
  dt_count_t m_ok_click;      // tracker ok response counter
  dt_count_t m_peers_count;   // total number of peers
  dt_count_t m_seeds_count;   // total number of seeds
  dt_count_t m_prevpeers;     // number of peers previously seen
  time_t m_report_time;       // stats last sent to tracker
  dt_datalen_t m_report_dl, m_report_ul;

  SOCKET m_sock;
  BufIo m_request_buffer, m_response_buffer;

  int _IPsin(const char *h, int p, struct sockaddr_in *psin);
  int _s2sin(const char *h, int p, struct sockaddr_in *psin);

  int BuildBaseRequest();
  int Connect();
  int SendRequest();
  dt_result_t ParseResponse(const char *buf, size_t bufsiz);
  dt_result_t CheckResponse();
  void Restart();

 public:
  btTracker(const char *url);
  ~btTracker();

  int Initial();

  void Reset(time_t new_interval=15);
  void Update(){ m_interval = 15; }

  const char *GetURL() const { return m_spec->url; }
  dt_trackerstatus_t GetStatus() const { return m_status; }
  dt_result_t GetResult() const { return m_result; }
  void ClearResult() { m_result = DT_NORMAL; }

  SOCKET GetSocket() const { return m_sock; }

  void RestartTracker();
  int IsRestarting() const { return m_f_restart; }
  int IsQuitting() const {
    return (m_f_stop && DT_TRACKER_FINISHED != m_status);
  }
  void Stop();

  int IntervalCheck(fd_set *rfdp, fd_set *wfdp);
  int SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds,
    fd_set *rfdnextp, fd_set *wfdnextp);

  dt_count_t GetRefuseClick() const { return m_refuse_click; }
  dt_count_t GetOkClick() const { return m_ok_click; }
  dt_count_t GetPeersCount() const;
  dt_count_t GetSeedsCount() const;
  void AdjustPeersCount(){
    if( m_f_boguspeercnt && m_peers_count ) m_peers_count--;
  }
  time_t GetInterval() const { return m_default_interval; }

  time_t GetReportTime() const { return m_report_time; }
  dt_datalen_t GetReportDL() const { return m_report_dl; }
  dt_datalen_t GetReportUL() const { return m_report_ul; }
};


class MultiTracker
{
 private:
  struct tier_node{
    tier_node *next, *next_tier;
    btTracker *tracker;
  }*m_trackers;

  int m_ntiers;
  mutable tier_node *m_tier_iter;
  btTracker *m_tracker;
  char *m_primary_url;
  dt_datalen_t m_totaldl;
  int m_nset;

  dt_result_t m_result;
  time_t m_report_time;       // stats last sent to tracker
  dt_datalen_t m_report_dl, m_report_ul;

  bool m_stop;

 public:
  MultiTracker();
  ~MultiTracker();

  int AddTracker(const char *url, bool new_tier);
  int AddOneTracker(const char *url, bool new_tier);
  int Initial();
  void Update();

  dt_trackerstatus_t GetStatus() const;
  dt_result_t GetResult() const;
  const char *GetURL() const {
    return m_tracker ? m_tracker->GetURL() : m_primary_url;
  }

  int GetNTiers() const { return m_ntiers; }
  const char *GetPrimaryURL() const { return m_primary_url; }
  const btTracker *GetTier(int num) const;
  const btTracker *GetNext(const btTracker *tracker) const;

  void RestartTracker();
  bool IsRestarting() const;
  bool IsQuitting() const { return m_stop; }
  void Stop();

  int IntervalCheck(fd_set *rfdp, fd_set *wfdp);
  void SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds,
    fd_set *rfdnextp, fd_set *wfdnextp);

  dt_count_t GetRefuseClick() const {
    return m_tracker ? m_tracker->GetRefuseClick() : 0;
  }
  dt_count_t GetOkClick() const {
    return m_tracker ? m_tracker->GetOkClick() : 0;
  }
  dt_count_t GetPeersCount() const {
    return m_tracker ? m_tracker->GetPeersCount() : 1;
  }
  dt_count_t GetSeedsCount() const;
  void AdjustPeersCount(){ if( m_tracker ) m_tracker->AdjustPeersCount(); }

  time_t GetInterval() const {
    return m_tracker ? m_tracker->GetInterval() : 15;
  }

  time_t GetReportTime() const {
    return m_tracker ? m_tracker->GetReportTime() : m_report_time;
  }
  dt_datalen_t GetReportDL() const {
    return m_tracker ? m_tracker->GetReportDL() : m_report_dl;
  }
  dt_datalen_t GetReportUL() const {
    return m_tracker ? m_tracker->GetReportUL() : m_report_ul;
  }

  void CountDL(bt_length_t nbytes){ m_totaldl += nbytes; }
  dt_datalen_t GetTotalDL();
  dt_datalen_t GetTotalUL() const;

  void PrintTiers() const;
};


extern MultiTracker TRACKER;


#endif  // TRACKER_H

