#include "tracker.h"	// for its includes/defines

#define CTCS_PROTOCOL "0001"

#define CTCS_BUFSIZE 200
#define CTCS_PASS_SIZE 21

#define C_TORRENT	1
#define C_STATUS	2
#define C_PEERS		3

struct ctstatus {
 public:
  size_t seeders, leechers, nhave, ntotal, navail, dlrate, ulrate,
    dlimit, ulimit;
  u_int64_t dltotal, ultotal;
  ctstatus() {
    seeders=leechers=nhave=ntotal=navail=dlrate=ulrate=dltotal=
    ultotal=dlimit=ulimit = 0;
  }
};

class Ctcs
{
 private:
  char m_host[MAXHOSTNAMELEN];
  int m_port;
  char m_pass[CTCS_PASS_SIZE];

  struct sockaddr_in m_sin;

  unsigned char m_status:2;

  time_t m_interval;
  time_t m_last_timestamp;
  time_t m_sent_ctstatus_time;

  SOCKET m_sock;
  BufIo in_buffer;
  BufIo out_buffer;
  struct ctstatus m_ctstatus;
  int m_sent_ctstatus;
  int m_sent_ctbw;

  int SendMessage(char *buf);

 public:
  Ctcs();
  ~Ctcs();

  void Reset(time_t new_interval);
  int _s2sin(char *h,int p,struct sockaddr_in *psin);
  int Initial();
  int Connect();
  int CheckMessage();
  int Send_Protocol();
  int Send_Auth();
  int Send_Torrent(unsigned char *peerid, char *torrent);
  int Report_Status(size_t seeders, size_t leechers, size_t nhave,
    size_t ntotal, size_t navail, size_t dlrate, size_t ulrate,
    u_int64_t dltotal, u_int64_t ultotal, size_t dlimit, size_t ulimit);
  int Send_Status();
  int Send_bw();
  int Send_Config();
  int Set_Config(char *msgbuf);
  int Send_Detail();
  int Send_Peers();
  int Send_Info(int sev, const char *info);
  int IntervalCheck(const time_t *pnow, fd_set *rfdp, fd_set *wfdp);
  int SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds);
  void RestartTracker();

  SOCKET GetSocket() { return m_sock; }
  unsigned char GetStatus() { return m_status;}
};

extern Ctcs CTCS;

