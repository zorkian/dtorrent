#include "ctcs.h"  // def.h

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#include <ctype.h>

#include "btcontent.h"
#include "setnonblock.h"
#include "connect_nonb.h"
#include "peerlist.h"
#include "peer.h"
#include "btconfig.h"
#include "bttime.h"
#include "console.h"

#if !defined(HAVE_SNPRINTF) || !defined(HAVE_HTONS)
#include "compat.h"
#endif


#define CTCS_PROTOCOL 3

#define compset(a, member) \
  ( (a.member==member) ? 0 : ((a.member = member)||1) )

Ctcs CTCS;


Ctcs::Ctcs()
{
  memset(m_host, 0, MAXHOSTNAMELEN);

  m_sock = INVALID_SOCKET;
  m_port = 2780;
  m_status = DT_TRACKER_FREE;
  m_interval = 5;
  m_protocol = CTCS_PROTOCOL;

  m_last_timestamp = m_sent_ctstatus_time = m_statustime = (time_t) 0;
  m_sent_ctstatus = 0;
  m_sent_ctbw = 0;

  in_buffer.MaxSize(256 * 1024);
  out_buffer.MaxSize(256 * 1024);
}


Ctcs::~Ctcs()
{
  if( m_sock != INVALID_SOCKET ){
    if( !g_secondary_process )
      shutdown(m_sock, SHUT_RDWR);
    CLOSE_SOCKET(m_sock);
  }
}


void Ctcs::Reset(time_t new_interval)
{
  int warn = 0;

  if( new_interval ) m_interval = new_interval;

  if( INVALID_SOCKET != m_sock ){
    if( DT_TRACKER_READY==m_status ) warn = 1;
    if( !g_secondary_process )
      shutdown(m_sock, SHUT_RDWR);
    CLOSE_SOCKET(m_sock);
    m_sock = INVALID_SOCKET;
  }

  in_buffer.Reset();
  out_buffer.Reset();
  m_last_timestamp = now;
  m_sent_ctstatus = 0;
  m_sent_ctbw = 0;
  m_status = DT_TRACKER_FREE;

  if( warn ) CONSOLE.Warning(2, "Connection to CTCS closed");
}


// borrowed from tracker.cpp (with changes)
int Ctcs::_s2sin(const char *h, int p, struct sockaddr_in *psin)
{
  psin->sin_family = AF_INET;
  psin->sin_port = htons(p);
  psin->sin_addr.s_addr = inet_addr(h);
  if( psin->sin_addr.s_addr == INADDR_NONE ){
    struct hostent *ph = gethostbyname(h);
    if( !ph  || ph->h_addrtype != AF_INET ){
      memset(psin, 0, sizeof(struct sockaddr_in));
      return -1;
    }
    memcpy(&psin->sin_addr, ph->h_addr_list[0], sizeof(struct in_addr));
  }
  return ( psin->sin_addr.s_addr == INADDR_NONE ) ? -1 : 0;
}


int Ctcs::CheckMessage()
{
  ssize_t r;
  size_t q;

  r = in_buffer.FeedIn(m_sock);

  // This differs from tracker.cpp since we maintain a persistent connection.
  if( r == 0 ) return 0;  // no data (should return an error)

  q = in_buffer.Count();

  if( !q ){
    int error = 0;
    socklen_t n = sizeof(error);
    if( getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &error, &n) < 0 )
      error = errno;
    if( error != 0 ) CONSOLE.Warning(2,
      "warn, received nothing from CTCS:  %s", strerror(error));
    Reset(5);
    return -1;
  }

  const char *s, *p, *msgbuf;
  while( in_buffer.Count() ){
    msgbuf = in_buffer.BasePointer();
    p = (const char *)memchr(msgbuf, '\r', in_buffer.Count());
    s = (const char *)memchr(msgbuf, '\n', in_buffer.Count());
    if( p && s > p ) s = p;
    if( !s ) break;

    if( *cfg_verbose && s != msgbuf )
      CONSOLE.Debug("CTCS: %.*s", (int)(s - msgbuf), msgbuf);
    if( !strncmp("SETDLIMIT", msgbuf, 9) ){
      dt_rate_t arg = (dt_rate_t)strtod(msgbuf+10, NULL);
      if( !BTCONTENT.IsFull() || arg < *cfg_max_bandwidth_down ){
        cfg_max_bandwidth_down = arg;
      }
    }else if( !strncmp("SETULIMIT", msgbuf, 9) ){
      cfg_max_bandwidth_up = (dt_rate_t)strtod(msgbuf+10, NULL);
    }else if( !strncmp("SENDPEERS", msgbuf, 9) ){
      Send_Peers();
    }else if( !strncmp("SENDSTATUS", msgbuf, 10) ){
      Send_Status();
    }else if( !strncmp("SENDCONF", msgbuf, 8) ){
      Send_Config();
    }else if( !strncmp("CTCONFIG", msgbuf, 8) ){
      Set_Config(msgbuf);
    }else if( !strncmp("SENDDETAIL", msgbuf, 10) ){
      Send_Detail();
    }else if( !strncmp("CTQUIT", msgbuf, 6) ){
      CONSOLE.Print("CTCS sent Quit command");
      Tracker.ClearRestart();
      Tracker.SetStoped();
    }else if( !strncmp("CTRESTART", msgbuf, 9) ){
      Tracker.RestartTracker();
    }else if( !strncmp("CTUPDATE", msgbuf, 8) ){
      Tracker.Reset(15);
    }else if( !strncmp("PROTOCOL", msgbuf, 8) ){
      int proto = atoi(msgbuf+9);
      if( proto <= CTCS_PROTOCOL ) m_protocol = proto;
      else m_protocol = CTCS_PROTOCOL;
    }else if( s != msgbuf ){
      if(*cfg_verbose)
        CONSOLE.Debug("unknown CTCS message: %.*s", (int)(s - msgbuf), msgbuf);
    }
    in_buffer.PickUp(s - msgbuf + 1);
  }
  m_last_timestamp = now;
  return 0;
}


int Ctcs::SendMessage(const char *message)
{
  int len, r=0;
  char buf[CTCS_BUFSIZE];

  if( m_status == DT_TRACKER_READY ){
    len = strlen(message);
    strncpy(buf, message, CTCS_BUFSIZE);
    if( len+1 < CTCS_BUFSIZE ){
      buf[len] = '\n';
      buf[len+1] = '\0';
    }else{
      buf[CTCS_BUFSIZE-2] = '\n';
      buf[CTCS_BUFSIZE-1] = '\0';
    }
    r = out_buffer.PutFlush(m_sock, buf, len+1);
    if( r < 0 ) Reset(5);
    else m_last_timestamp = now;
  }
  return ( r < 0 ) ? r : 0;
}


int Ctcs::Send_Auth()
{
  char message[CTCS_BUFSIZE];

  if( !*m_pass ) return 0;
  snprintf(message, CTCS_BUFSIZE, "AUTH %s", m_pass);
  return SendMessage(message);
}


int Ctcs::Send_Protocol()
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "PROTOCOL %04d", CTCS_PROTOCOL);
  return SendMessage(message);
}


int Ctcs::Send_Torrent(const unsigned char *peerid, const char *torrent)
{
  char message[CTCS_BUFSIZE];
  char txtid[PEER_ID_LEN*2+3];

  TextPeerID(peerid, txtid);

  snprintf(message, CTCS_BUFSIZE, "CTORRENT %s %ld %ld %s", txtid,
    (long)BTCONTENT.GetStartTime(), (long)now, torrent);
  return SendMessage(message);
}


int Ctcs::Report_Status()
{
  int changebw;
  dt_rate_t dlrate, ulrate, dlimit, ulimit;

  if( DT_TRACKER_READY != m_status ) return 0;

  dlrate = Self.RateDL();
  ulrate = Self.RateUL();
  dlimit = *cfg_max_bandwidth_down;
  ulimit = *cfg_max_bandwidth_up;

  changebw = (
    compset(m_ctstatus, dlrate) |
    compset(m_ctstatus, ulrate) |
    compset(m_ctstatus, dlimit) |
    compset(m_ctstatus, ulimit) );

  m_statustime = now;

  if( !m_sent_ctstatus ||
      (Tracker.GetStatus() && now > m_sent_ctstatus_time+30) ){
    return Send_Status();
  }else return ( changebw || !m_sent_ctbw ) ? Send_bw() : 0;
}


int Ctcs::Send_Status()
{
  char message[CTCS_BUFSIZE];
  dt_count_t seeders, leechers;
  dt_mem_t cacheused;
  bt_index_t nhave, ntotal;
  dt_datalen_t dltotal, ultotal;
  dt_rate_t dlrate, ulrate, dlimit, ulimit;

  if( DT_TRACKER_READY != m_status ) return 0;

  seeders = WORLD.GetSeedsCount();
  leechers = WORLD.GetPeersCount() - seeders - WORLD.GetConnCount();
  nhave = BTCONTENT.pBF->Count();
  ntotal = BTCONTENT.GetNPieces();
  dlrate = m_ctstatus.dlrate;
  ulrate = m_ctstatus.ulrate;
  dltotal = Self.TotalDL();
  ultotal = Self.TotalUL();
  dlimit = *cfg_max_bandwidth_down;
  ulimit = *cfg_max_bandwidth_up;
  cacheused = BTCONTENT.CacheUsed()/1024;

  if( m_protocol == 1 )
    snprintf( message, CTCS_BUFSIZE,
      "CTSTATUS %d/%d %d/%d/%d %d,%d %llu,%llu %d,%d",
      (int)seeders, (int)leechers,
      (int)nhave, (int)ntotal, (int)WORLD.Pieces_I_Can_Get(),
      (int)dlrate, (int)ulrate,
      (unsigned long long)dltotal, (unsigned long long)ultotal,
      (int)dlimit, (int)ulimit );
  else
    snprintf( message, CTCS_BUFSIZE,
      "CTSTATUS %d:%d/%d:%d/%d %d/%d/%d %d,%d %llu,%llu %d,%d %d",
      (int)seeders,
        (int)(Tracker.GetSeedsCount() - (BTCONTENT.IsFull() ? 1 : 0)),
      (int)leechers,
        (int)(Tracker.GetPeersCount() - Tracker.GetSeedsCount() -
              (!BTCONTENT.IsFull() ? 1 : 0)),
      (int)WORLD.GetConnCount(),
      (int)nhave, (int)ntotal, (int)WORLD.Pieces_I_Can_Get(),
      (int)dlrate, (int)ulrate,
      (unsigned long long)dltotal, (unsigned long long)ultotal,
      (int)dlimit, (int)ulimit,
      (int)cacheused );
  m_sent_ctstatus = m_sent_ctbw = 1;
  m_sent_ctstatus_time = now;
  return SendMessage(message);
}


int Ctcs::Send_bw()
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "CTBW %d,%d %d,%d",
    (int)m_ctstatus.dlrate, (int)m_ctstatus.ulrate,
    (int)m_ctstatus.dlimit, (int)m_ctstatus.ulimit);
  m_sent_ctbw = 1;
  return SendMessage(message);
}


int Ctcs::Send_Config()
{
  char message[CTCS_BUFSIZE];

  if( m_protocol >= 3 ){
    int r = 0;
    const char *cfgtype, *info;
    char range[32], tmpinfo[CTCS_BUFSIZE];
    const char *value;

    if( (r=SendMessage("CTCONFIGSTART")) < 0 ) return r;

    for( ConfigGen *config = CONFIG.First();
         config;
         config = CONFIG.Next(config) ){
      if( !config->Hidden() ){
        switch( config->Type() ){
          case DT_CONFIG_INT: cfgtype = "I"; break;
          case DT_CONFIG_FLOAT: cfgtype = "F"; break;
          case DT_CONFIG_BOOL: cfgtype = "B"; break;
          default:
          case DT_CONFIG_STRING: cfgtype = "S";
        }

        if( config->Type() == DT_CONFIG_STRING )
          sprintf(range, "%d", (int)config->MaxLen());
        else if( 0 != strcmp(config->Smax(), config->Smin()) )
          sprintf(range, "%s-%s", config->Smin(), config->Smax());
        else sprintf(range, "0");

        if( config->Locked() ){
          snprintf(tmpinfo, CTCS_BUFSIZE, "%s [Locked]", config->Info());
          info = tmpinfo;
        }else info = config->Info();

        value = config->Sval();
        if( m_protocol == 3 && DT_CONFIG_BOOL == config->Type() )
          value = strchr("1TtYy", *value) ? "1" : "0";

        if( (r=SendMessage(ConfigMsg(config->Tag(), cfgtype, range, value,
                 config->Desc(), info))) < 0 ){
          return r;
        }
      }
    }

    sprintf(message, "CTCONFIGDONE");
  }
  else if( m_protocol == 2 )
    snprintf(message, CTCS_BUFSIZE, "CTCONFIG %d %lu %f %d %d %d %d %d",
      (int)*cfg_verbose, (unsigned long)*cfg_seed_hours, *cfg_seed_ratio,
      (int)*cfg_max_peers, (int)*cfg_min_peers,
      BTCONTENT.GetFilter() ? atoi(BTCONTENT.GetFilterName()) : 0,
      (int)*cfg_cache_size, WORLD.IsPaused());
  else  // m_protocol == 1
    snprintf(message, CTCS_BUFSIZE, "CTCONFIG %d %lu %f %d %d %d %d %d %d",
      (int)*cfg_verbose, (unsigned long)*cfg_seed_hours, *cfg_seed_ratio,
      (int)*cfg_max_peers, (int)*cfg_min_peers,
      BTCONTENT.GetFilter() ? atoi(BTCONTENT.GetFilterName()) : 0,
      0, WORLD.IsPaused(), 0);

  return SendMessage(message);
}

char *Ctcs::ConfigMsg(const char *name, const char *type, const char *range,
  const char *value, const char *short_desc, const char *long_desc)
{
  static char *message = (char *)0;

  if( !message ){
    message = new char[CTCS_BUFSIZE];
    if( !message ){
      CONSOLE.Warning(1, "error, failed to allocate memory for CTCS message");
      return (char *)0;
    }
  }
  snprintf(message, CTCS_BUFSIZE, "CTCONFIG %s %s %s %d:%s %d:%s %d:%s",
    name, type, range, (int)strlen(value), value,
    (int)strlen(short_desc), short_desc, (int)strlen(long_desc), long_desc);

  return message;
}

int Ctcs::Set_Config(const char *origmsg)
{
  char *msgbuf, *msgptr;
  size_t msglen;

  msglen = strpbrk(origmsg, "\r\n") - origmsg;
  msgbuf = new char[msglen + 1];
  if( !msgbuf ){
    CONSOLE.Warning(1, "error, failed to allocate memory for config");
    return -1;
  }
  strncpy(msgbuf, origmsg, msglen);
  msgbuf[msglen] = '\0';
  msgptr = msgbuf;

  if( m_protocol >= 3 ){
    char *name, *valstr;
    ConfigGen *config;
    if( !(name = strtok(strchr(msgbuf, ' '), " ")) ||
        strlen(name) >= strlen(origmsg) - (name - msgbuf) ){
      goto err;
    }
    valstr = name + strlen(name);
    for( ++valstr; *valstr==' '; valstr++ );

    if( (config = CONFIG[name]) ){
      if( !config->Scan(valstr) ) goto err;
    }else{
      CONSOLE.Warning(2, "Unknown config option %s", name);
      goto err;
    }
  }else{  // m_protocol <= 2
    if( msgptr[9] != '.' ){
      int arg = atoi(msgptr+9);
      cfg_verbose = arg;
    }
    if( msgptr[11] != '.' ) cfg_seed_hours = strtoul(msgptr+11, NULL, 10);
    if( !(msgptr = strchr(msgptr+11, ' ')) ) goto err;
    if( *++msgptr != '.' ) cfg_seed_ratio = atof(msgptr);
    if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
    if( *++msgptr != '.' ) cfg_max_peers = (dt_count_t)atoi(msgptr);
    if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
    if( *++msgptr != '.' ) cfg_min_peers = (dt_count_t)atoi(msgptr);
    if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
    if( *++msgptr != '.' ){
      char *tmp, *p = strchr(msgptr, ' ');
      if( !p ) goto err;
      tmp = new char[p - msgptr + 2 + 1];
      if( !tmp )
        CONSOLE.Warning(1, "error, failed to allocate memory for option");
      else{
        strncpy(tmp, msgptr, p - msgptr);
        tmp[p - msgptr] = '\0';
        strcat(tmp, ",*");  // mock old behavior
      }
      cfg_file_to_download = tmp;
      if( tmp ) delete []tmp;
    }
    if( m_protocol >= 2 ){
      if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
      if( *++msgptr != '.' ){
        cfg_cache_size = atoi(msgptr);
      }
    }
    if( m_protocol == 1 ){
      if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
      ++msgptr;
      // old cfg_exit_zero_peers option
    }
    if( !(msgptr = strchr(msgptr, ' ')) ) goto err;
    if( *++msgptr != '.' ){
      cfg_pause = atoi(msgptr);
    }
  }

  delete []msgbuf;
  return 0;

 err:
  const char *p, *s;
  p = (const char *)memchr(origmsg, '\r', in_buffer.Count());
  s = (const char *)memchr(origmsg, '\n', in_buffer.Count());
  if( p && s > p ) s = p;
  CONSOLE.Warning(2, "Malformed or invalid input from CTCS: %.*s",
    (int)(s - origmsg), origmsg);
  delete []msgbuf;
  return -1;
}


int Ctcs::Send_Detail()
{
  char message[CTCS_BUFSIZE];
  int r=0, priority, current=0;
  dt_count_t n=0;
  Bitfield tmpBitfield, fileFilter, availbf, tmpavail, allFilter, tmpFilter;
  const Bitfield *pfilter;

  snprintf(message, CTCS_BUFSIZE, "CTDETAIL %lld %d %ld %ld",
    (unsigned long long)BTCONTENT.GetTotalFilesLength(),
    (int)BTCONTENT.GetPieceLength(), (long)now,
    (long)BTCONTENT.GetSeedTime());
  r = SendMessage(message);

  if( r==0 ) r = SendMessage((m_protocol >= 3) ? "CTFILESTART" : "CTFILES");

  if( m_protocol >= 3 ){  // determine current download priority
    pfilter = (Bitfield *)0;
    while( pfilter != BTCONTENT.GetFilter() ){
      current++;
      pfilter = BTCONTENT.GetNextFilter(pfilter);
    }
  }

  WORLD.Pieces_I_Can_Get(&availbf);

  while( r==0 && ++n <= BTCONTENT.GetNFiles() ){
    tmpBitfield = *BTCONTENT.pBF;
    BTCONTENT.SetTmpFilter(n, &fileFilter);
    tmpBitfield.Except(fileFilter);  // the pieces of this file that I have
    tmpavail = availbf;
    tmpavail.Except(fileFilter);     // the available pieces of this file

    if( m_protocol >= 3 ){
      priority = 0;
      if( BTCONTENT.GetFilter() ){
        fileFilter.Invert();
        allFilter.SetAll();
        pfilter = (Bitfield *)0;
        while( (pfilter = BTCONTENT.GetNextFilter(pfilter)) ){
          priority++;
          allFilter.And(*pfilter);    // cumulation of filters
          tmpFilter = allFilter;
          tmpFilter.Invert();         // what's included by the filters...
          tmpFilter.And(fileFilter);  // ...that's also in this file
          if( tmpFilter.Count() >= fileFilter.Count() ) break;
        }
        if( !pfilter ) priority = 0;
      }
      snprintf( message, CTCS_BUFSIZE, "CTFILE %d %d %d %d %d %d %llu %s",
        (int)n, priority, current, (int)BTCONTENT.GetFilePieces(n),
        (int)tmpBitfield.Count(), (int)tmpavail.Count(),
        (unsigned long long)BTCONTENT.GetFileSize(n),
        BTCONTENT.GetFileName(n) );
    }
    else if( m_protocol == 2 )
      snprintf(message, CTCS_BUFSIZE, "CTFILE %d %d %d %d %llu %s",
        (int)n, (int)BTCONTENT.GetFilePieces(n),
        (int)tmpBitfield.Count(), (int)tmpavail.Count(),
        (unsigned long long)BTCONTENT.GetFileSize(n),
        BTCONTENT.GetFileName(n));
    else  // m_protocol == 1
      snprintf(message, CTCS_BUFSIZE, "CTFILE %d %d %d %llu %s",
        (int)n, (int)BTCONTENT.GetFilePieces(n),
        (int)tmpBitfield.Count(),
        (unsigned long long)BTCONTENT.GetFileSize(n),
        BTCONTENT.GetFileName(n));

    r = SendMessage(message);
  }
  if( r==0 ) r = SendMessage((m_protocol >= 3) ? "CTFILESDONE" : "CTFDONE");
  return r;
}


int Ctcs::Send_Peers()
{
  btPeer *peer=0;
  char message[CTCS_BUFSIZE];
  char txtid[PEER_ID_LEN*2+3];
  struct sockaddr_in psin;
  int r=0;

  r=SendMessage((m_protocol >= 3) ? "CTPEERSTART" : "CTPEERS");
  while( r==0 && (peer = WORLD.GetNextPeer(peer)) ){
    TextPeerID(peer->GetPeerID(), txtid);
     peer->GetAddress(&psin);

     snprintf(message, CTCS_BUFSIZE, "CTPEER %s %s %c%c%c%c %d %d %llu %llu %d",
       txtid, inet_ntoa(psin.sin_addr),
       peer->Is_Remote_Unchoked() ? 'U' : 'C',
       peer->Is_Local_Interested() ? 'i' : 'n',
       peer->Is_Local_Unchoked() ? 'U' : 'C',
       peer->Is_Remote_Interested() ? 'i' : 'n',
       (int)peer->RateDL(), (int)peer->RateUL(),
       (unsigned long long)peer->TotalDL(),
         (unsigned long long)peer->TotalUL(),
       (int)peer->bitfield.Count());
     r = SendMessage(message);
  }
  if( r==0 ) r = SendMessage((m_protocol >= 3) ? "CTPEERSDONE" : "CTPDONE");
  return r;
}


int Ctcs::Send_Info(int sev, const char *info)
{
  char message[CTCS_BUFSIZE];

  snprintf(message, CTCS_BUFSIZE, "CTINFO %d %s", sev, info);
  return SendMessage(message);
}


int Ctcs::Connect()
{
  ssize_t r;
  m_last_timestamp = now;

  if( _s2sin(m_host, m_port, &m_sin) < 0 ){
    CONSOLE.Warning(2, "warn, get CTCS ip address failed.");
    return -1;
  }

  m_sock = socket(AF_INET, SOCK_STREAM, 0);
  if( INVALID_SOCKET == m_sock ) return -1;

  if( setfd_nonblock(m_sock) < 0 ){
    CLOSE_SOCKET(m_sock);
    return -1;
  }

  r = connect_nonb(m_sock, (struct sockaddr *)&m_sin);
  if( r == -1 ){
    CLOSE_SOCKET(m_sock);
    return -1;
  }else if( r == -2 ){
    m_status = DT_TRACKER_CONNECTING;
  }else{
    m_status = DT_TRACKER_READY;
    if(*cfg_verbose) CONSOLE.Debug("Connected to CTCS");
    if( Send_Protocol() != 0 && errno != EINPROGRESS ){
      CONSOLE.Warning(2, "warn, send protocol to CTCS failed:  %s",
        strerror(errno));
      return -1;
    }
    if( Send_Auth() != 0 && errno != EINPROGRESS ){
      CONSOLE.Warning(2, "warn, send password to CTCS failed:  %s",
        strerror(errno));
      return -1;
    }
    if( Send_Torrent(BTCONTENT.GetPeerId(), BTCONTENT.GetMetainfoFile()) < 0 &&
        errno != EINPROGRESS ){
      CONSOLE.Warning(2, "warn, send torrent to CTCS failed:  %s",
        strerror(errno));
      return -1;
    }
  }
  return 0;
}


int Ctcs::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
  if( DT_TRACKER_FREE == m_status ){
    if( INVALID_SOCKET != m_sock ){
      FD_CLR(m_sock, rfdp);
      FD_CLR(m_sock, wfdp);
    }
    if( now - m_last_timestamp >= m_interval ){
      if( Connect() < 0 ){
        Reset(15);
        return -1;
      }
      FD_SET(m_sock, rfdp);
      if( m_status == DT_TRACKER_CONNECTING ) FD_SET(m_sock, wfdp);
    }else if( now < m_last_timestamp ) m_last_timestamp = now;
  }else if( DT_TRACKER_CONNECTING == m_status ){
    FD_SET(m_sock, rfdp);
    FD_SET(m_sock, wfdp);
  }else if( INVALID_SOCKET != m_sock ){
    if( now > m_statustime ) Report_Status();
    FD_SET(m_sock, rfdp);
    if( out_buffer.Count() ) FD_SET(m_sock, wfdp);
  }
  return m_sock;
}


int Ctcs::SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  if( DT_TRACKER_FREE == m_status ) return 0;

  if( DT_TRACKER_CONNECTING == m_status && FD_ISSET(m_sock, wfdp) ){
    int error = 0;
    socklen_t n = sizeof(error);
    (*nfds)--;
    FD_CLR(m_sock, wfdnextp);
    if( FD_ISSET(m_sock, rfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, rfdnextp);
    }
    if( getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &error, &n) < 0 )
      error = errno;
    if( error != 0 ){
      if( ECONNREFUSED != error )
        CONSOLE.Warning(2, "warn, connect to CTCS failed:  %s",
          strerror(error));
      Reset(15);
      return -1;
    }else{
      m_status = DT_TRACKER_READY;
      if(*cfg_verbose) CONSOLE.Debug("Connected to CTCS");
      if( Send_Protocol() != 0 && errno != EINPROGRESS ){
        CONSOLE.Warning(2, "warn, send protocol to CTCS failed:  %s",
          strerror(errno));
        return -1;
      }
      if( Send_Auth() != 0 && errno != EINPROGRESS ){
        CONSOLE.Warning(2, "warn, send password to CTCS failed:  %s",
          strerror(errno));
        return -1;
      }
      if( Send_Torrent(BTCONTENT.GetPeerId(),
            BTCONTENT.GetMetainfoFile()) < 0 && errno != EINPROGRESS ){
        CONSOLE.Warning(2, "warn, send torrent to CTCS failed:  %s",
          strerror(errno));
        return -1;
      }
    }
  }else if( DT_TRACKER_CONNECTING == m_status && FD_ISSET(m_sock, rfdp) ){
    int error = 0;
    socklen_t n = sizeof(error);
    (*nfds)--;
    FD_CLR(m_sock, rfdnextp);
    if( getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &error, &n) < 0 )
      error = errno;
    CONSOLE.Warning(2, "warn, connect to CTCS failed:  %s", strerror(error));
    Reset(15);
    return -1;
  }else if( INVALID_SOCKET != m_sock ){
    if( FD_ISSET(m_sock, rfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, rfdnextp);
      SOCKET tmp_sock = m_sock;
      int r = CheckMessage();
      if( INVALID_SOCKET == m_sock ){
        if( FD_ISSET(tmp_sock, wfdp) ){
          (*nfds)--;
          FD_CLR(tmp_sock, wfdnextp);
        }
        return r;
      }
    }
    if( FD_ISSET(m_sock, wfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, wfdnextp);
      if( out_buffer.Count() && out_buffer.FlushOut(m_sock) < 0 ){
        Reset(5);
        return -1;
      }
    }
  }else{  // failsafe
    Reset(5);
    return -1;
  }
  return 0;
}

