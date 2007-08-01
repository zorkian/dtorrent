#ifndef WINDOWS
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <netdb.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <ctype.h>

#include "ctcs.h"
#include "btcontent.h"
#include "setnonblock.h"
#include "connect_nonb.h"
#include "tracker.h"
#include "peerlist.h"
#include "peer.h"
#include "btconfig.h"
#include "bttime.h"

#define compset(a,member)  ( (a.member==member)? 0 : ((a.member = member)||1) )

Ctcs CTCS;


Ctcs::Ctcs()
{
  memset(m_host,0,MAXHOSTNAMELEN);

  m_sock = INVALID_SOCKET;
  m_port = 2780;
  m_status = T_FREE;
  m_interval = 1;

  m_last_timestamp = m_sent_ctstatus_time = (time_t) 0;
  m_sent_ctstatus = 0;
  m_sent_ctbw = 0;
}


Ctcs::~Ctcs()
{
  if( m_sock != INVALID_SOCKET) CLOSE_SOCKET(m_sock);
}


void Ctcs::Reset(time_t new_interval)
{
  if(new_interval) m_interval = new_interval;

  if( INVALID_SOCKET != m_sock ){
    CLOSE_SOCKET(m_sock);
    m_sock = INVALID_SOCKET;
  }

  in_buffer.Reset();
  out_buffer.Reset();
  m_last_timestamp = now;
  m_sent_ctstatus = 0;
  m_sent_ctbw = 0;
  m_status = T_FREE;
}


// borrowed from tracker.cpp (with changes)
int Ctcs:: _s2sin(char *h,int p,struct sockaddr_in *psin)
{
  psin->sin_family = AF_INET;
  psin->sin_port = htons(p);
  psin->sin_addr.s_addr = inet_addr(h);
  if(psin->sin_addr.s_addr == INADDR_NONE){
    struct hostent *ph = gethostbyname(h);
    if( !ph  || ph->h_addrtype != AF_INET){
      memset(psin,0,sizeof(struct sockaddr_in));
      return -1;
    }
    memcpy(&psin->sin_addr,ph->h_addr_list[0],sizeof(struct in_addr));
  }
  return ( psin->sin_addr.s_addr == INADDR_NONE ) ? -1 : 0;
}


int Ctcs::CheckMessage()
{
  ssize_t r;
  size_t q;

  r = in_buffer.FeedIn(m_sock);

  if( r == 0 ) return 0;	// no data
  if( r < 0 ){ Reset(1); return -1; }	// error

  q = in_buffer.Count();

  if( !q ){
    int error = 0;
    socklen_t n = sizeof(error);
    if(getsockopt(m_sock, SOL_SOCKET,SO_ERROR,&error,&n) < 0 ||
       error != 0 ){
      fprintf(stderr,"warn, received nothing from CTCS! %s\n",strerror(error));
    }
    Reset(0);
    return -1;
  }

  char *s, *msgbuf;
  while(in_buffer.Count() && (s=strchr(msgbuf=in_buffer.BasePointer(), '\n'))){
//  msgbuf = in_buffer.BasePointer();
//  if( s=strchr(msgbuf, '\n') ){	//have a complete message
    *s = '\0';
    if(arg_verbose) fprintf(stderr, "CTCS: %s\n", msgbuf);
    if( !strncmp("SETDLIMIT",msgbuf,9) ){
      cfg_max_bandwidth_down = (int)(strtod(msgbuf+10, NULL));
      if(arg_verbose) fprintf(stderr, "DLimit=%d\n", cfg_max_bandwidth_down);
    }else if( !strncmp("SETULIMIT",msgbuf,9) ){
      cfg_max_bandwidth_up = (int)(strtod(msgbuf+10, NULL));
      if(arg_verbose) fprintf(stderr, "ULimit=%d\n", cfg_max_bandwidth_up);
    }else if( !strncmp("SENDPEERS",msgbuf,9) ){
      Send_Peers();
    }else if( !strncmp("SENDSTATUS",msgbuf,10) ){
      Send_Status();
    }else if( !strncmp("SENDCONF",msgbuf,8) ){
      Send_Config();
    }else if( !strncmp("CTCONFIG",msgbuf,8) ){
      Set_Config(msgbuf);
    }else if( !strncmp("SENDDETAIL",msgbuf,10) ){
      Send_Detail();
    }else if( !strncmp("CTQUIT",msgbuf,6) ){
      printf("CTCS sent Quit command\n");
      Tracker.SetStoped();
    }else if( !strncmp("CTRESTART",msgbuf,9) ){
      RestartTracker();
    }else if( !strncmp("CTUPDATE",msgbuf,8) ){
      Tracker.Reset(1);
    }else if( !strncmp("PROTOCOL",msgbuf,8) ){
      // nothing yet
    }else{
      if(arg_verbose) fprintf(stderr, "unknown CTCS message: %s", msgbuf);
    }
    in_buffer.PickUp(s-msgbuf + 1);
  }
  return 0;
}


int Ctcs::SendMessage(char *message)
{
  int len, r=0;
  char buf[CTCS_BUFSIZE];

  if( m_status == T_READY ){
    len = strlen(message);
    strncpy(buf, message, len);
    if( len+1 < CTCS_BUFSIZE ){
      buf[len] = '\n';
      buf[len+1] = '\0';
    }else{
      buf[CTCS_BUFSIZE-2] = '\n';
      buf[CTCS_BUFSIZE-1] = '\0';
    }
    r = out_buffer.PutFlush(m_sock, buf, len+1);
    if( r<0 ) Reset(1);
  }
  return r;
}


int Ctcs::Send_Auth()
{
  char message[CTCS_BUFSIZE];

  if(!*m_pass) return 0;
  snprintf(message, CTCS_BUFSIZE, "AUTH %s", m_pass);
  return SendMessage(message);
}


int Ctcs::Send_Protocol()
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "PROTOCOL %s", CTCS_PROTOCOL);
  return SendMessage(message);
}


int Ctcs::Send_Torrent(unsigned char *peerid, char *torrent)
{
  char message[CTCS_BUFSIZE];
  char txtid[PEER_ID_LEN*2+3];

  TextPeerID(peerid, txtid);

  snprintf(message, CTCS_BUFSIZE, "CTORRENT %s %ld %ld %s", txtid,
    (long)(BTCONTENT.GetStartTime()), (long)now, torrent);
  return SendMessage(message);
}


int Ctcs::Report_Status(size_t seeders, size_t leechers, size_t nhave,
  size_t ntotal, size_t navail, size_t dlrate, size_t ulrate,
  u_int64_t dltotal, u_int64_t ultotal, size_t dlimit, size_t ulimit)
{
  int changebw=0,change=0;
  int r;
  size_t nhad;

  if( T_READY != m_status ) return 0;

  nhad = m_ctstatus.nhave;

  changebw = (
    compset(m_ctstatus, dlrate)		|
    compset(m_ctstatus, ulrate)		|
    compset(m_ctstatus, dlimit)		|
    compset(m_ctstatus, ulimit)		);
  change = ( changebw |
    compset(m_ctstatus, seeders)	|
    compset(m_ctstatus, leechers)	|
    compset(m_ctstatus, nhave)		|
    compset(m_ctstatus, ntotal)		|
    compset(m_ctstatus, navail)		|
    compset(m_ctstatus, dltotal)	|
    compset(m_ctstatus, ultotal)	);

  if( ( !m_sent_ctstatus || (nhad<nhave && nhave==ntotal) ||
        (Tracker.GetStatus() && m_sent_ctstatus_time+30 > now) ) &&
      (r=Send_Status()) != 0 ) return r;
  else return (changebw || !m_sent_ctbw) ? Send_bw() : 0;
}


int Ctcs::Send_Status()
{
  char message[CTCS_BUFSIZE];

  if( m_sent_ctstatus_time + 1 > now ) {
    m_sent_ctstatus = 0;
    return 0;
  }
  snprintf(message, CTCS_BUFSIZE, "CTSTATUS %u/%u %u/%u/%u %u,%u %llu,%llu %u,%u",
    m_ctstatus.seeders, m_ctstatus.leechers,
    m_ctstatus.nhave, m_ctstatus.ntotal, m_ctstatus.navail,
    m_ctstatus.dlrate, m_ctstatus.ulrate,
    m_ctstatus.dltotal, m_ctstatus.ultotal,
    m_ctstatus.dlimit, m_ctstatus.ulimit );
  m_sent_ctstatus = 1;
  m_sent_ctstatus_time = now;
  return SendMessage(message);
}


int Ctcs::Send_bw()
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "CTBW %u,%u %u,%u",
    m_ctstatus.dlrate, m_ctstatus.ulrate,
    m_ctstatus.dlimit, m_ctstatus.ulimit );
  m_sent_ctbw = 1;
  return SendMessage(message);
}


int Ctcs::Send_Config()
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "CTCONFIG %d %d %f %d %d %d %d %d %d",
    (int)arg_verbose, (int)cfg_seed_hours, cfg_seed_ratio,
    (int)cfg_max_peers, (int)cfg_min_peers, (int)arg_file_to_download,
    (int)cfg_exit_zero_peers, Tracker.IsPaused(), Tracker.IsQuitting());
  return SendMessage(message);
}

int Ctcs::Set_Config(char *msgbuf)
{
  unsigned char foo;

  if(msgbuf[9] != '.') arg_verbose = atoi(msgbuf+9);
  if(msgbuf[11] != '.') cfg_seed_hours = atoi(msgbuf+11);
  msgbuf = strchr(msgbuf+11, ' ') + 1;
  if(msgbuf[0] != '.') cfg_seed_ratio = atof(msgbuf);
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.') cfg_max_peers = atoi(msgbuf);
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.') cfg_min_peers = atoi(msgbuf);
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.'){
    foo = atoi(msgbuf);
    if(foo != arg_file_to_download){
      arg_file_to_download = foo;
      BTCONTENT.FlushCache();
      BTCONTENT.SetFilter();
      WORLD.CheckInterest();
    }
  }
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.') cfg_exit_zero_peers = atoi(msgbuf);
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.'){
    if(atoi(msgbuf)){
      if( !Tracker.IsPaused() ) Tracker.SetPause();
    }else if( Tracker.IsPaused() ) Tracker.Resume();
  }
  msgbuf = strchr(msgbuf, ' ') + 1;
  if(msgbuf[0] != '.'){
    if(atoi(msgbuf)) Tracker.SoftQuit();
    else Tracker.DontQuit();
    if(atoi(msgbuf)){
      if( !Tracker.IsQuitting() ) Tracker.SoftQuit();
    }else if( Tracker.IsQuitting() ) Tracker.DontQuit();
  }

  return 0;
}


int Ctcs::Send_Detail()
{
  char message[CTCS_BUFSIZE];
  int r=0;
  size_t n=0;
  BTFILE *file=0;
  BitField tmpFilter;

  snprintf( message, CTCS_BUFSIZE, "CTDETAIL %lld %d %ld %ld",
    BTCONTENT.GetTotalFilesLength(),
    (int)(BTCONTENT.GetPieceLength()), (long)now,
    (long)(BTCONTENT.GetSeedTime()) );
  r = SendMessage(message);

  if(r==0) r = SendMessage("CTFILES");

  while( r==0 && (file = BTCONTENT.GetNextFile(file)) ){
    ++n;
    BTCONTENT.SetTmpFilter(n, &tmpFilter);
    BitField tmpBitField = *BTCONTENT.pBF;
    tmpBitField.Except(tmpFilter);

    snprintf( message, CTCS_BUFSIZE, "CTFILE %u %d %d %llu %s",
      n, (int)(BTCONTENT.getFilePieces(n)),
      (int)(tmpBitField.Count()),
      file->bf_length, file->bf_filename );
    r = SendMessage(message);
  }
  if(r==0) r = SendMessage("CTFDONE");
  return r;
}


int Ctcs::Send_Peers()
{
  btPeer *peer=0;
  char message[CTCS_BUFSIZE];
  char txtid[PEER_ID_LEN*2+3];
  struct sockaddr_in psin;
  int r=0;

  r=SendMessage("CTPEERS");
  while( r==0 && (peer = WORLD.GetNextPeer(peer)) ){
    TextPeerID(peer->id, txtid);
     peer->GetAddress(&psin);

     snprintf(message, CTCS_BUFSIZE, "CTPEER %s %s %c%c%c%c %d %d %llu %llu %d",
       txtid, inet_ntoa(psin.sin_addr),
       peer->Is_Remote_UnChoked() ? 'U' : 'C',
       peer->Is_Local_Interested() ? 'i' : 'n',
       peer->Is_Local_UnChoked() ? 'U' : 'C',
       peer->Is_Remote_Interested() ? 'i' : 'n',
       peer->RateDL(), peer->RateUL(),
       peer->TotalDL(), peer->TotalUL(),
       peer->bitfield.Count() );
     r = SendMessage(message);
  }
  if(r==0) r = SendMessage("CTPDONE");
  return r;
}


int Ctcs::Send_Info(int sev, const char *info)
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "CTINFO %d %s", sev, info);
  return SendMessage(message);
}


int Ctcs::Initial()
{
  char *s;

  strncpy(m_host, arg_ctcs, MAXHOSTNAMELEN-1);
  m_host[MAXHOSTNAMELEN-1] = '\0';
  if( s = strchr(m_host, ':') ) *s='\0';
  m_port = atoi(s=(strchr(arg_ctcs, ':')+1));
  if(strchr(s, ':')){
    printf("Enter CTCS password: "); fflush(stdout);
    fgets(m_pass, CTCS_PASS_SIZE, stdin);
  } else *m_pass = '\0';

  return 0;
}


int Ctcs::Connect()
{
  ssize_t r;
  m_last_timestamp = now;

  if(_s2sin(m_host,m_port,&m_sin) < 0) {
    fprintf(stderr,"warn, get CTCS ip address failed.");
    return -1;
  }

  m_sock = socket(AF_INET,SOCK_STREAM,0);
  if(INVALID_SOCKET == m_sock) return -1;

  if(setfd_nonblock(m_sock) < 0) {CLOSE_SOCKET(m_sock); return -1; }

  r = connect_nonb(m_sock,(struct sockaddr*)&m_sin);

  if( r == -1 ){ CLOSE_SOCKET(m_sock); return -1;}
  else if( r == -2 ) m_status = T_CONNECTING;
  else{
    m_status = T_READY;
    if( Send_Protocol() != 0 && errno != EINPROGRESS ){
      fprintf(stderr,"warn, send protocol to CTCS failed. %s\n",strerror(errno));
      return -1;
    }
    if( Send_Auth() != 0 && errno != EINPROGRESS ) {
      fprintf(stderr,"warn, send password to CTCS failed. %s\n",strerror(errno));
      return -1;
    }
    if( Send_Torrent(BTCONTENT.GetPeerId(), arg_metainfo_file) != 0 &&
        errno != EINPROGRESS ){
      fprintf(stderr,"warn, send torrent to CTCS failed. %s\n",strerror(errno));
      return -1;
    }
  }
  return 0;
}


int Ctcs::IntervalCheck(const time_t *pnow, fd_set *rfdp, fd_set *wfdp)
{
  if( T_FREE == m_status ){
    if(*pnow - m_last_timestamp >= m_interval){
      if(Connect() < 0){ Reset(15); return -1; }

      if( m_status == T_CONNECTING ){
        FD_SET(m_sock, rfdp);
        FD_SET(m_sock, wfdp);
      }else{
        FD_SET(m_sock, rfdp);
      }
    }
  }else{
    if( m_status == T_CONNECTING ){
      FD_SET(m_sock, rfdp);
      FD_SET(m_sock, wfdp);
    }else if (INVALID_SOCKET != m_sock){
      FD_SET(m_sock, rfdp);
      if( out_buffer.Count() ) FD_SET(m_sock, wfdp);
    }
  }
  return m_sock;
}


int Ctcs::SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds)
{
  if( T_FREE == m_status ) return 0;

  if( T_CONNECTING == m_status && FD_ISSET(m_sock,wfdp) ){
    int error = 0;
    socklen_t n = sizeof(error);
    (*nfds)--;
    FD_CLR(m_sock, wfdp); 
    if(getsockopt(m_sock, SOL_SOCKET,SO_ERROR,&error,&n) < 0 ||
       error != 0 ){
      if( ECONNREFUSED != error )
        fprintf(stderr,"warn, connect to CTCS failed. %s\n",strerror(error));
      Reset(15);
      return -1;
    }else{
      m_status = T_READY; 
      if( Send_Protocol() != 0 && errno != EINPROGRESS ){
        fprintf(stderr,"warn, send protocol to CTCS failed. %s\n",strerror(errno));
        return -1;
      }
      if( Send_Auth() != 0 && errno != EINPROGRESS ) {
        fprintf(stderr,"warn, send password to CTCS failed. %s\n",strerror(errno));
        return -1;
      }
      if( Send_Torrent(BTCONTENT.GetPeerId(), arg_metainfo_file) == 0
          && errno != EINPROGRESS ){
        fprintf(stderr,"warn, send torrent to CTCS failed. %s\n",strerror(errno));
        return -1;
      }
    }
  }else if( T_CONNECTING == m_status && FD_ISSET(m_sock,rfdp) ){
    int error = 0;
    socklen_t n = sizeof(error);
    (*nfds)--;
    FD_CLR(m_sock, rfdp);
    getsockopt(m_sock, SOL_SOCKET,SO_ERROR,&error,&n);
    fprintf(stderr,"warn, connect to CTCS failed. %s\n",strerror(error));
    Reset(15);
    return -1;
  }else if( INVALID_SOCKET != m_sock ){
    if( FD_ISSET(m_sock, rfdp) ){
      (*nfds)--;
      FD_CLR(m_sock,rfdp);
      CheckMessage();
    }
    if( INVALID_SOCKET != m_sock && FD_ISSET(m_sock, wfdp) ){
      (*nfds)--;
      FD_CLR(m_sock,wfdp);
      if( out_buffer.Count() && out_buffer.FlushOut(m_sock) < 0){
        Reset(1);
        return -1;
      }
    }
  }
  return 0;
}


void Ctcs::RestartTracker()
{
  Tracker.SetPause();  // prevents downloader from exiting
  Tracker.SetStoped(); // finish the tracker
  // Now we need to wait until the tracker updates (T_FINISHED == m_status),
  // then Tracker.Resume().
  Tracker.SetRestart();
}

