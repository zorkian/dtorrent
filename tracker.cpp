#include "tracker.h"

#include <string.h>
#include <errno.h>

#include "peerlist.h"
#include "peer.h"
#include "httpencode.h"
#include "bencode.h"
#include "setnonblock.h"
#include "connect_nonb.h"
#include "btcontent.h"
#include "iplist.h"

#include "btconfig.h"
#include "ctcs.h"
#include "console.h"
#include "bttime.h"
#include "util.h"

#if !defined(HAVE_SNPRINTF) || !defined(HAVE_HTONL) || !defined(HAVE_HTONS)
#include "compat.h"
#endif


MultiTracker TRACKER;

btTracker::btTracker(const char *url)
{
  if( (m_spec = new tracker_spec) ){
    if( (m_spec->url = new char[strlen(url) + 1]) )
      strcpy(m_spec->url, url);
    else fprintf(stderr, "%s\n", strerror(errno = ENOMEM));
    m_spec->port = 80;
  }else fprintf(stderr, "%s\n", strerror(errno = ENOMEM));

  m_redirect = (tracker_spec *)0;

  memset(m_trackerid, 0, PEER_ID_LEN + 1);

  m_sock = INVALID_SOCKET;
  m_status = DT_TRACKER_FREE;
  m_result = DT_NORMAL;
  m_f_started = m_f_stop = m_f_completed = m_f_restart = 0;

  m_interval = 15;
  m_default_interval = 30 * 60;  // default default is 30 minutes
  m_peers_count = m_seeds_count = 0;

  m_ok_click = m_refuse_click = 0;
  m_last_timestamp = (time_t)0;
  m_prevpeers = 0;

  m_report_time = (time_t)0;
  m_report_dl = m_report_ul = 0;

  m_request_buffer.MaxSize(2048);
  m_response_buffer.MaxSize(256 * 1024);
}

btTracker::~btTracker()
{
  if( m_sock != INVALID_SOCKET ){
    if( !g_secondary_process )
      shutdown(m_sock, SHUT_RDWR);
    CLOSE_SOCKET(m_sock);
  }
  if( m_redirect ) delete m_redirect;
  if( m_spec ) delete m_spec;
}

void btTracker::Reset(time_t new_interval)
{
  m_interval = new_interval ? new_interval : m_default_interval;

  if( INVALID_SOCKET != m_sock ){
    if( *cfg_verbose && DT_TRACKER_READY==m_status )
      CONSOLE.Debug("Disconnected from tracker at %s", m_spec->url);
    if( !g_secondary_process )
      shutdown(m_sock, SHUT_RDWR);
    CLOSE_SOCKET(m_sock);
    m_sock = INVALID_SOCKET;
  }

  if( m_redirect ){
    delete m_spec;
    m_spec = m_redirect;
    m_redirect = (tracker_spec *)0;
  }

  m_request_buffer.Reset();
  m_response_buffer.Reset();
  if( now < m_last_timestamp ) m_last_timestamp = now;  // time reversed

  if( m_f_stop ){
    if( m_f_restart )
      m_status = m_f_started ? DT_TRACKER_FREE : DT_TRACKER_FINISHED;
    else m_status = DT_TRACKER_FINISHED;

    if( DT_TRACKER_FINISHED == m_status ){
      m_f_started = 0;
      if( m_f_restart ) Restart();
    }
  }
  else m_status = DT_TRACKER_FREE;
}

int btTracker::_IPsin(const char *h, int p, struct sockaddr_in *psin)
{
  psin->sin_family = AF_INET;
  psin->sin_port = htons(p);
  psin->sin_addr.s_addr = inet_addr(h);
  return ( psin->sin_addr.s_addr == INADDR_NONE ) ? -1 : 0;
}

int btTracker::_s2sin(const char *h, int p, struct sockaddr_in *psin)
{
  psin->sin_family = AF_INET;
  psin->sin_port = htons(p);
  if( h ){
    psin->sin_addr.s_addr = inet_addr(h);
    if( psin->sin_addr.s_addr == INADDR_NONE ){
      struct hostent *ph = gethostbyname(h);
      if( !ph  || ph->h_addrtype != AF_INET ){
        memset(psin, 0, sizeof(struct sockaddr_in));
        return -1;
      }
      memcpy(&psin->sin_addr, ph->h_addr_list[0], sizeof(struct in_addr));
    }
  }else psin->sin_addr.s_addr = htonl(INADDR_ANY);
  return 0;
}

dt_result_t btTracker::ParseResponse(const char *buf, size_t bufsiz)
{
  char tmphost[MAXHOSTNAMELEN];
  const char *ps;
  size_t i, pos, tmpport;
  int64_t bint;
  dt_count_t cnt = 0;

  struct sockaddr_in addr;

  if( decode_query(buf, bufsiz, "failure reason", &ps, &i, (int64_t *)0,
                   DT_QUERY_STR) ){
    char failreason[1024];
    if( i < 1024 ){
      memcpy(failreason, ps, i);
      failreason[i] = '\0';
    }else{
      memcpy(failreason, ps, 1000);
      failreason[1000] = '\0';
      strcat(failreason, "...");
    }
    CONSOLE.Warning(1, "TRACKER FAILURE from %s:  %s", m_spec->url, failreason);
    return DT_FAILURE;
  }
  if( decode_query(buf, bufsiz, "warning message", &ps, &i, (int64_t *)0,
                   DT_QUERY_STR) ){
    char warnmsg[1024];
    if( i < 1024 ){
      memcpy(warnmsg, ps, i);
      warnmsg[i] = '\0';
    }else{
      memcpy(warnmsg, ps, 1000);
      warnmsg[1000] = '\0';
      strcat(warnmsg, "...");
    }
    CONSOLE.Warning(2, "TRACKER WARNING from %s:  %s", m_spec->url, warnmsg);
  }

  m_peers_count = m_seeds_count = 0;

  if( decode_query(buf, bufsiz, "tracker id", &ps, &i, (int64_t *)0,
                   DT_QUERY_STR) ){
    if( i <= PEER_ID_LEN ){
      memcpy(m_trackerid, ps, i);
      m_trackerid[i] = '\0';
    }else{
      memcpy(m_trackerid, ps, PEER_ID_LEN);
      m_trackerid[PEER_ID_LEN] = '\0';
    }
  }

  if( decode_query(buf, bufsiz, "interval", (const char **)0, NULL, &bint,
                   DT_QUERY_INT) ){
    m_interval = m_default_interval = (time_t)bint;
  }else{
    CONSOLE.Debug("Tracker at %s did not specify interval.", m_spec->url);
    m_interval = m_default_interval;
  }

  if( decode_query(buf, bufsiz, "complete", (const char **)0, NULL, &bint,
                   DT_QUERY_INT) ){
    m_seeds_count = bint;
  }
  if( decode_query(buf, bufsiz, "incomplete", (const char **)0, NULL, &bint,
                   DT_QUERY_INT) ){
    m_peers_count = m_seeds_count + bint;
  }else{
    if( *cfg_verbose && 0==m_seeds_count )
      CONSOLE.Debug("Tracker at %s did not supply peers count.", m_spec->url);
    m_peers_count = m_seeds_count;
  }

  pos = decode_query(buf, bufsiz, "peers", (const char **)0, (size_t *)0,
                     (int64_t *)0, DT_QUERY_POS);
  if( !pos ){
    CONSOLE.Debug("Tracker at %s did not supply peers.", m_spec->url);
    return DT_FAILURE;
  }
  if( 4 > bufsiz - pos ){
    CONSOLE.Debug("Tracker at %s supplied an invalid peers list.", m_spec->url);
    return DT_FAILURE;
  }

  buf += (pos + 1);
  bufsiz -= (pos + 1);

  ps = buf-1;
  if( *ps != 'l' ){  // binary peers section if not 'l'
    addr.sin_family = AF_INET;
    i = 0;
    while( *ps != ':' ) i = i * 10 + (*ps++ - '0');
    i /= 6;
    ps++;
    while( i-- > 0 ){
      memcpy(&addr.sin_addr, ps, sizeof(struct in_addr));
      memcpy(&addr.sin_port, ps+sizeof(struct in_addr), sizeof(unsigned short));
      if( !Self.IpEquiv(addr) ){
        cnt++;
        IPQUEUE.Add(&addr);
      }
      ps += 6;
    }
  }else for( ; bufsiz && *buf != 'e'; buf += pos, bufsiz -= pos ){
    pos = decode_dict(buf, bufsiz, (char *)0);
    if( !pos ) break;
    if( !decode_query(buf, pos, "ip", &ps, &i, (int64_t *)0, DT_QUERY_STR) ||
        MAXHOSTNAMELEN < i ){
      continue;
    }
    memcpy(tmphost, ps, i);
    tmphost[i] = '\0';

    if( !decode_query(buf, pos, "port", (const char **)0, NULL, &bint,
                      DT_QUERY_INT) ){
      continue;
    }
    tmpport = bint;

    if( !decode_query(buf, pos, "peer id", &ps, &i, (int64_t *)0,
                      DT_QUERY_STR) && i != 20 ){
      continue;
    }

    if( _IPsin(tmphost, tmpport, &addr) < 0 ){
      CONSOLE.Warning(3, "warn, detected invalid ip address %s.", tmphost);
      continue;
    }

    if( !Self.IpEquiv(addr) ){
      cnt++;
      IPQUEUE.Add(&addr);
    }
  }

  if( 0==m_peers_count ){
    m_peers_count = cnt + 1;  // include myself
    m_f_boguspeercnt = 1;
  }else m_f_boguspeercnt = 0;
  if(*cfg_verbose) CONSOLE.Debug("new peers=%d; next check in %d sec [%s]",
    (int)cnt, (int)m_interval, m_spec->url);
  return DT_SUCCESS;
}

dt_result_t btTracker::CheckResponse()
{
  const char *pdata;
  ssize_t r;
  size_t q, hlen, dlen;

  r = m_response_buffer.FeedIn(m_sock);
  m_last_timestamp = now;

  if( r > 0 ){
    // connection is still open; may have more data coming
    return DT_NORMAL;
  }

  q = m_response_buffer.Count();

  if( !q ){
    int error = 0;
    socklen_t n = sizeof(error);
    if( getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &error, &n) < 0 )
      error = errno;
    if( error ){
      CONSOLE.Warning(2, "warn, received nothing from tracker at %s:  %s",
        m_spec->url, strerror(error));
    }else{
      CONSOLE.Warning(2, "warn, received nothing from tracker at %s",
        m_spec->url);
    }
    Reset();  // try again
    return DT_FAILURE;
  }

  hlen = HttpSplit(m_response_buffer.BasePointer(), q, &pdata, &dlen);

  if( !hlen ){
    CONSOLE.Warning(2, "warn, no HTTP header in response from tracker at %s",
      m_spec->url);
    return DT_FAILURE;
  }

  r = HttpGetStatusCode(m_response_buffer.BasePointer(), hlen);
  if( r != 200 ){
    if( r == 301 || r == 302 || r == 303 || r == 307 ){
      char *tmpurl = (char *)0, *c;
      tracker_spec *tmpspec;

      if( HttpGetHeader(m_response_buffer.BasePointer(), hlen, "Location",
                        &tmpurl) < 0 ){
        if( errno ){
          CONSOLE.Warning(2,
            "Error parsing redirect response from tracker at %s:  %s",
            m_spec->url, strerror(errno));
        }else{
          CONSOLE.Warning(2,
            "warn, redirect with no location from tracker at %s", m_spec->url);
        }
        if( tmpurl ) delete []tmpurl;
        return DT_FAILURE;
      }

      if( !(tmpspec = new tracker_spec) ){
        CONSOLE.Warning(1,
          "warn, could not allocate memory for tracker redirect from %s",
          m_spec->url);
        errno = ENOMEM;
        delete []tmpurl;
        return DT_FAILURE;
      }

      if( (c = strstr(tmpurl, "?info_hash=")) ||
          (c = strstr(tmpurl, "&info_hash=")) ){
        *c = '\0';
      }
      if( UrlSplit(tmpurl, &tmpspec->host, &tmpspec->port,
                   &tmpspec->request) < 0 ){
        CONSOLE.Warning(1,
          "warn, error parsing redirected tracker URL from %s (%s):  %s",
          m_spec->url, tmpurl, errno ? strerror(errno) : "invalid format");
        delete tmpspec;
        delete []tmpurl;
        return DT_FAILURE;
      }else{
        CONSOLE.Debug("tracker at %s redirected%s to %s",
          m_spec->url, (r == 301) ? " permanently" : "", tmpurl);
        if( (tmpspec->url = new char[strlen(tmpurl) + 1]) )
          strcpy(tmpspec->url, tmpurl);
        else{
          CONSOLE.Warning(1,
            "warn, could not allocate memory for tracker redirect from %s",
            m_spec->url);
          errno = ENOMEM;
          delete tmpspec;
          delete []tmpurl;
          return DT_FAILURE;
        }
        delete []tmpurl;
        Reset();
        m_redirect = m_spec;
        m_spec = tmpspec;
        if( BuildBaseRequest() < 0 ){
          delete m_spec;
          m_spec = m_redirect;
          m_redirect = (tracker_spec *)0;
          return DT_FAILURE;
        }else if( r == 301 ){  // moved permanently
          delete m_redirect;
          m_redirect = (tracker_spec *)0;
        }
      }

      if( Connect() < 0 ){
        Reset();
        delete m_spec;
        m_spec = m_redirect;
        m_redirect = (tracker_spec *)0;
        return DT_FAILURE;
      }else return DT_NORMAL;
    }else if( r >= 400 ){
      CONSOLE.Warning(2, "Tracker response code %d from %s%s", r, m_spec->url,
        (r >= 500) ? "" :
          "; the torrent is not registered on this tracker"
          " or may have been removed.");
      if( pdata && dlen ){  // write(STDERR_FILENO, pdata, dlen);
        char data[dlen + 1];
        memcpy(data, pdata, dlen);
        data[dlen] = '\0';
        CONSOLE.Warning(0, "Tracker response data DUMP:");
        CONSOLE.Warning(0, "%s", data);
        CONSOLE.Warning(0, "== DUMP OVER==");
      }
      m_interval = (m_default_interval > 300) ? m_default_interval : 300;
      m_f_started = 0;
      return DT_FAILURE;
    }else{
      m_f_started = 0;
      return DT_FAILURE;
    }
  }

  m_f_started = m_f_stop ? 0 : 1;
  m_refuse_click = 0;
  m_ok_click++;

  if( !pdata ){
    CONSOLE.Warning(2, "warn, received no response data from tracker at %s",
      m_spec->url);
    return DT_FAILURE;
  }
  return ParseResponse(pdata, dlen);
}

int btTracker::Initial()
{
  if( UrlSplit(m_spec->url, &m_spec->host, &m_spec->port,
               &m_spec->request) < 0 ){
    CONSOLE.Warning(1, "Error parsing tracker URL (%s):  %s", m_spec->url,
      errno ? strerror(errno) : "invalid format");
    return -1;
  }

  char chars[37] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for( int i=0; i<8; i++ )
    m_key[i] = chars[RandBits(6) % 36];
  m_key[8] = 0;

  if( BuildBaseRequest() < 0 ) return -1;

  return 0;
}

int btTracker::BuildBaseRequest()
{
  char ih_buf[20 * 3 + 1], pi_buf[20 * 3 + 1], *tmppath, *tmpreq = (char *)0;

  if( !(tmppath = new char[strlen(m_spec->request) + 1]) ||
      !(tmpreq = new char[strlen(m_spec->request) + 256]) ){
    errno = ENOMEM;
    CONSOLE.Warning(1, "Error building tracker request:  %s", strerror(errno));
    if( tmppath ) delete []tmppath;
    return -1;
  }
  strcpy(tmppath, m_spec->request);
  delete []m_spec->request;
  sprintf(tmpreq,
          "GET %s%cinfo_hash=%s&peer_id=%s&key=%s",
          tmppath,
          strchr(tmppath, '?') ? '&' : '?',
          urlencode(ih_buf, BTCONTENT.GetInfoHash(), 20),
          urlencode(pi_buf, BTCONTENT.GetPeerId(), PEER_ID_LEN),
          m_key);

  if( *cfg_listen_port > 0 ){
    strcat(tmpreq, "&port=");
    sprintf(tmpreq + strlen(tmpreq), "%d", (int)*cfg_listen_port);
  }

  cfg_public_ip.Lock();
  if( *cfg_public_ip ){
    strcat(tmpreq, "&ip=");
    strcat(tmpreq, *cfg_public_ip);
  }else{
    struct sockaddr_in addr;
    Self.GetAddress(&addr);
    if( !IsPrivateAddress(addr.sin_addr.s_addr) ){
      strcat(tmpreq, "&ip=");
      strcat(tmpreq, inet_ntoa(addr.sin_addr));
    }
  }

  if( !(m_spec->request = new char[strlen(tmpreq) + 1]) ){
    errno = ENOMEM;
    CONSOLE.Warning(1, "Error building tracker request:  %s", strerror(errno));
    delete []tmpreq;
    return -1;
  }
  strcpy(m_spec->request, tmpreq);
  delete []tmpreq;
  return 0;
}

int btTracker::Connect()
{
  struct sockaddr_in sin;
  ssize_t r;

  m_result = DT_NORMAL;
  m_last_timestamp = now;

  if( _s2sin(m_spec->host, m_spec->port, &sin) < 0 ){
    CONSOLE.Warning(2, "warn, get tracker's ip address failed.");
    return -1;
  }

  m_sock = socket(AF_INET, SOCK_STREAM, 0);
  if( INVALID_SOCKET == m_sock ){
    CONSOLE.Warning(2, "warn, tracker socket allocation failed:  %s",
      strerror(errno));
    return -1;
  }

  /* we only need to bind if we have specified an ip
     we need it to bind here before the connect! */
  if( *cfg_listen_ip != 0 ){
    struct sockaddr_in addr;
    // clear the struct as requested in the manpages
    memset(&addr, 0, sizeof(sockaddr_in));
    // set the type
    addr.sin_family = AF_INET;
    // we want the system to choose port
    addr.sin_port = 0;
    // set the defined ip from the commandline
    addr.sin_addr.s_addr = *cfg_listen_ip;
    // bind it or return...
    if( bind(m_sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in))
          != 0 ){
      CONSOLE.Warning(1, "warn, can't set up tracker connection:  %s",
        strerror(errno));
      return -1;
    }
  }

  if( setfd_nonblock(m_sock) < 0 ){
    CONSOLE.Warning(2, "warn, tracker socket setup failed:  %s",
      strerror(errno));
    CLOSE_SOCKET(m_sock);
    m_sock = INVALID_SOCKET;
    return -1;
  }

  r = connect_nonb(m_sock, (struct sockaddr *)&sin);
  if( r == -1 ){
    CONSOLE.Warning(2, "warn, connect to tracker at %s failed:  %s",
      m_spec->url, strerror(errno));
    CLOSE_SOCKET(m_sock);
    m_sock = INVALID_SOCKET;
    return -1;
  }else if( r == -2 ){
    if(*cfg_verbose) CONSOLE.Debug("Connecting to tracker at %s", m_spec->url);
    m_status = DT_TRACKER_CONNECTING;
  }else{
    if(*cfg_verbose) CONSOLE.Debug("Connected to tracker at %s", m_spec->url);
    if( 0 == SendRequest() ) m_status = DT_TRACKER_READY;
    else{
      CLOSE_SOCKET(m_sock);
      m_sock = INVALID_SOCKET;
      return -1;
    }
  }
  return 0;
}

int btTracker::SendRequest()
{
  enum{
    DT_NONE,
    DT_STARTED,
    DT_STOPPED,
    DT_COMPLETED
  } event = DT_NONE;
  const char *event_str = (char *)0;
  char *req_buffer;
  struct sockaddr_in addr;
  dt_datalen_t totaldl, totalul;
  int r;

  if( !(req_buffer = new char[strlen(m_spec->request) + strlen(m_spec->host) +
                              strlen(*cfg_user_agent) + 256]) ){
    errno = ENOMEM;
    CONSOLE.Warning(1, "Error constructing tracker request:  %s",
      strerror(errno));
    return -1;
  }

  if( m_f_stop ) event = DT_STOPPED;
  else if( !m_f_started ){
    if( BTCONTENT.IsFull() ) m_f_completed = 1;
    event = DT_STARTED;
  }else if( BTCONTENT.IsFull() && !m_f_completed ){
    if( Self.TotalDL() > 0 ) event = DT_COMPLETED;
    m_f_completed = 1;  // only send download complete once
  }

  totaldl = TRACKER.GetTotalDL();
  totalul = TRACKER.GetTotalUL();
  sprintf(req_buffer,
          "%s&uploaded=%llu&downloaded=%llu&left=%llu&compact=1&numwant=%d",
          m_spec->request,
          (unsigned long long)totalul,
          (unsigned long long)totaldl,
          (unsigned long long)BTCONTENT.GetLeftBytes(),
          (int)*cfg_max_peers);
  if( DT_NONE != event ){
    strcat(req_buffer, "&event=");
    event_str = (DT_STARTED == event ? "started" :
                 (DT_STOPPED == event ? "stopped" :
                 (DT_COMPLETED == event ? "completed" : "update")));
    strcat(req_buffer, event_str);
  }
  if( *m_trackerid ){
    strcat(req_buffer, "&trackerid=");
    strcat(req_buffer, m_trackerid);
  }
  strcat(req_buffer, " HTTP/1.0" CRLF);

  // If we have a tracker hostname (not just an IP), send a Host: header
  if( _IPsin(m_spec->host, m_spec->port, &addr) < 0 ){
    strcat(req_buffer, "Host: ");
    strcat(req_buffer, m_spec->host);
    strcat(req_buffer, CRLF);
  }

  strcat(req_buffer, "User-Agent: ");
  strcat(req_buffer, *cfg_user_agent);
  strcat(req_buffer, CRLF);

  strcat(req_buffer, CRLF);
//CONSOLE.Warning(0, "SendRequest: %s", req_buffer);

  r = m_request_buffer.PutFlush(m_sock, req_buffer, strlen(req_buffer));
  delete []req_buffer;
  if( r < 0 ){
    CONSOLE.Warning(2, "warn, send request to tracker at %s failed:  %s",
      m_spec->url, strerror(errno));
    if( event == DT_COMPLETED )
      m_f_completed = 0;  // failed sending completion event
    return -1;
  }else{
    if( 0==m_request_buffer.Count() )
      shutdown(m_sock, SHUT_WR);
    m_report_time = now;
    m_report_dl = totaldl;
    m_report_ul = totalul;
    if(*cfg_verbose){
      CONSOLE.Debug("Reported to tracker:  %llu uploaded, %llu downloaded (%s)",
        (unsigned long long)m_report_ul, (unsigned long long)m_report_dl,
        event_str ? event_str : "update");
    }
  }
  return 0;
}

int btTracker::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
  if( BTCONTENT.IsFull() && !m_f_completed && Self.TotalDL() > 0 ){
    // need to report completion
    m_interval = 15;
  }

  if( DT_TRACKER_FREE == m_status ){
    if( INVALID_SOCKET != m_sock ){
      FD_CLR(m_sock, rfdp);
      FD_CLR(m_sock, wfdp);
    }
    if( now - m_last_timestamp >= m_interval ||
        // Connect to tracker early if we run low on peers.
        (WORLD.GetPeersCount() < *cfg_min_peers &&
          m_prevpeers >= *cfg_min_peers && now - m_last_timestamp >= 15) ){
      m_prevpeers = WORLD.GetPeersCount();

      if( Connect() < 0 ){
        Reset();
        m_result = DT_FAILURE;
        return -1;
      }

      FD_SET(m_sock, rfdp);
      if( m_status == DT_TRACKER_CONNECTING ) FD_SET(m_sock, wfdp);
    }else if( now < m_last_timestamp ) m_last_timestamp = now;  // time reversed
  }else if( DT_TRACKER_CONNECTING == m_status ){
    FD_SET(m_sock, rfdp);
    FD_SET(m_sock, wfdp);
  }else if( INVALID_SOCKET != m_sock ){
    FD_SET(m_sock, rfdp);
    if( m_request_buffer.Count() ) FD_SET(m_sock, wfdp);
  }
  return m_sock;
}

int btTracker::SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  if( DT_TRACKER_FREE == m_status ) return 0;
  if( DT_TRACKER_FINISHED == m_status && m_f_stop && !m_f_restart ) return 0;

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
    if( error ){
      if( ECONNREFUSED == error ){
        CONSOLE.Debug("Tracker connection refused at %s", m_spec->url);
        m_refuse_click++;
      }else{
        CONSOLE.Warning(2, "warn, connect to tracker at %s failed:  %s",
          m_spec->url, strerror(error));
      }
      Reset();
      m_result = DT_FAILURE;
      return -1;
    }else{
      if(*cfg_verbose) CONSOLE.Debug("Connected to tracker at %s", m_spec->url);
      if( SendRequest() == 0 ) m_status = DT_TRACKER_READY;
      else{
        Reset();
        m_result = DT_FAILURE;
        return -1;
      }
    }
  }else if( DT_TRACKER_CONNECTING == m_status && FD_ISSET(m_sock, rfdp) ){
    int error = 0;
    socklen_t n = sizeof(error);
    (*nfds)--;
    FD_CLR(m_sock, rfdnextp);
    if( FD_ISSET(m_sock, wfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, wfdnextp);
    }
    if( getsockopt(m_sock, SOL_SOCKET, SO_ERROR, &error, &n) < 0 )
      error = errno;
    CONSOLE.Warning(2, "warn, connect to tracker at %s failed:  %s",
      m_spec->url, strerror(error));
    Reset();
    m_result = DT_FAILURE;
    return -1;
  }else if( INVALID_SOCKET != m_sock ){
    if( FD_ISSET(m_sock, rfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, rfdnextp);
      SOCKET tmp_sock = m_sock;
      m_result = CheckResponse();
      if( DT_SUCCESS == m_result ) Reset(0);
      else if( DT_FAILURE == m_result ) Reset();
      if( INVALID_SOCKET == m_sock ){
        if( FD_ISSET(tmp_sock, wfdp) ){
          (*nfds)--;
          FD_CLR(tmp_sock, wfdnextp);
        }
        return (DT_FAILURE == m_result) ? -1 : 0;
      }
    }
    if( FD_ISSET(m_sock, wfdp) ){
      (*nfds)--;
      FD_CLR(m_sock, wfdnextp);
      if( m_request_buffer.Count() && m_request_buffer.FlushOut(m_sock) < 0 ){
        Reset();
        m_result = DT_FAILURE;
        return -1;
      }
      if( 0==m_request_buffer.Count() )
        shutdown(m_sock, SHUT_WR);
    }
  }else{  // failsafe
    Reset();
    return -1;
  }
  return 0;
}

void btTracker::Restart()
{
  m_f_stop = m_f_restart = 0;

  if( DT_TRACKER_FINISHED == m_status ){
    m_status = DT_TRACKER_FREE;
    m_f_started = 0;
    m_interval = 15;
    m_result = DT_NORMAL;
  }
}

void btTracker::Stop()
{
  m_f_stop = 0;
  m_f_restart = 0;
  Reset();
  m_f_stop = 1;
  if( !m_f_started ) m_status = DT_TRACKER_FINISHED;
  m_result = DT_NORMAL;
}

void btTracker::RestartTracker()
{
  // Stop/finish the tracker, then call Restart() after DT_TRACKER_FINISHED.
  Stop();
  m_f_restart = 1;
  if( DT_TRACKER_FINISHED == m_status ) Restart();
}

dt_count_t btTracker::GetPeersCount() const
{
  // includes seeds, so must always be >= 1 (myself!)
  return ( m_peers_count > m_seeds_count ) ? m_peers_count :
           (GetSeedsCount() + (BTCONTENT.IsFull() ? 0 : 1));
}

dt_count_t btTracker::GetSeedsCount() const
{
  return m_seeds_count ? m_seeds_count : (BTCONTENT.IsFull() ? 1 : 0);
}



//===========================================================================
// MultiTracker class functions
// Copyright 2008 Dennis Holmes  (dholmes@rahul.net)

MultiTracker::MultiTracker()
{
  m_trackers = (tier_node *)0;
  m_ntiers = 0;
  m_tracker = (btTracker *)0;
  m_primary_url = (char *)0;
  m_totaldl = 0;
  m_stop = false;
  m_nset = 0;
  m_report_time = (time_t)0;
  m_report_dl = m_report_ul = 0;
  m_result = DT_NORMAL;
}


MultiTracker::~MultiTracker()
{
  tier_node *p;

  if( m_primary_url ) delete []m_primary_url;

  while( m_trackers ){
    while( (p = m_trackers->next) ){
      m_trackers->next = p->next;
      if( p->tracker ) delete p->tracker;
      delete p;
    }
    p = m_trackers;
    m_trackers = m_trackers->next_tier;
    if( p->tracker ) delete p->tracker;
    delete p;
  }
}


// url may be a comma-separated URL list
int MultiTracker::AddTracker(const char *url, bool new_tier)
{
  if( strchr(url, ',') ){
    char *urls, *upos;
    int result;

    if( !(urls = new char[strlen(url) + 1]) ){
      CONSOLE.Warning(1, "error, failed to allocate memory for tracker list");
      errno = ENOMEM;
      return -1;
    }
    strcpy(urls, url);
    for( upos = urls; (upos = strchr(upos, ',')); upos++ ){
      if( upos > urls && '\\' == *(upos - 1) ){
        *upos = 0xff;
        memmove(upos - 1, upos, strlen(upos) + 1);
      }
    }

    upos = strtok(urls, ",");
    do{
      for( char *cpos = upos; (cpos = strchr(cpos, 0xff)); ) *cpos = ',';
      result = AddOneTracker(upos, new_tier);
      if( result < 0 ) break;
      new_tier = false;
    }while( (upos = strtok(NULL, ",")) );
    delete []urls;
    return result;
  }else return AddOneTracker(url, new_tier);
}


// url must be a single URL
int MultiTracker::AddOneTracker(const char *url, bool new_tier)
{
  int tier = 0;
  tier_node *p = m_trackers, *pp = (tier_node *)0, *node;

  if( !(node = new tier_node) || !(node->tracker = new btTracker(url)) ){
    CONSOLE.Warning(1, "error, failed to allocate memory for tracker");
    errno = ENOMEM;
    if( node ) delete node;
    return -1;
  }
  node->next = node->next_tier = (tier_node *)0;

  if( !m_trackers ){
    m_trackers = node;
    if( (m_primary_url = new char[strlen(node->tracker->GetURL()) + 1]) )
      strcpy(m_primary_url, node->tracker->GetURL());
  }else{
    for( tier = 0; p && (new_tier || p->next_tier); tier++ ){
      pp = p;
      p = p->next_tier;
    }
    if( !p ) pp->next_tier = node;
    else{
      node->next_tier = p->next_tier;
      node->next = p->next;
      p->next = node;
      // shuffle the list
      pp = p;  // head of the tier
      for( p = p->next; p; p = p->next ){
        if( RandBits(1) ){
          btTracker *tmp = pp->tracker;
          pp->tracker = p->tracker;
          p->tracker = tmp;
        }
      }
    }
  }
  if( *cfg_verbose ) CONSOLE.Debug("Tier %d tracker at %s", tier, url);
  if( new_tier ) m_ntiers++;
  return 0;
}


dt_result_t MultiTracker::GetResult() const
{
  tier_node *tier, *p;

  if( DT_NORMAL == m_result ){
    for( tier = m_trackers; tier; tier = tier->next_tier ){
      for( p = tier; p; p = p->next ){
        if( DT_FAILURE == p->tracker->GetResult() )
          return DT_FAILURE;
      }
    }
  }
  return m_result;
}


const btTracker *MultiTracker::GetTier(int num) const
{
  tier_node *tier = m_trackers;

  for( int i = 0; i < num && tier; i++ )
    tier = tier->next_tier;
  m_tier_iter = tier;
  return tier ? tier->tracker : (btTracker *)0;
}


const btTracker *MultiTracker::GetNext(const btTracker *tracker) const
{
  if( m_tier_iter && tracker == m_tier_iter->tracker ){
    m_tier_iter = m_tier_iter->next;
    return m_tier_iter ? m_tier_iter->tracker : (btTracker *)0;
  }else{
    tier_node *tier, *p = (tier_node *)0;
    for( tier = m_trackers; tier; tier = tier->next_tier ){
      for( p = tier; p; p = p->next ){
        if( tracker == p->tracker ) break;
      }
    }
    m_tier_iter = p;
    return p ? p->tracker : (btTracker *)0;
  }
}


int MultiTracker::Initial()
{
  struct sockaddr_in addr;
  tier_node *tier, *p, *pt, *pp, *tnext, *pnext;
  int result;

  // Determine local IP address
  if( *cfg_public_ip ){  // Get specified public address.
    if( (addr.sin_addr.s_addr = inet_addr(*cfg_public_ip)) == INADDR_NONE ){
      struct hostent *h;
      h = gethostbyname(*cfg_public_ip);
      memcpy(&addr.sin_addr, h->h_addr, sizeof(struct in_addr));
    }
    Self.SetIp(addr);
    goto next_step;
  }
  if( *cfg_listen_ip ){  // Get specified listen address.
    addr.sin_addr.s_addr = *cfg_listen_ip;
    Self.SetIp(addr);
    if( !IsPrivateAddress(*cfg_listen_ip) ) goto next_step;
  }
  { // Try to get address corresponding to the hostname.
    struct hostent *h;
    char hostname[MAXHOSTNAMELEN];

    if( gethostname(hostname, MAXHOSTNAMELEN) >= 0 ){
//    CONSOLE.Debug("hostname: %s", hostname);
      if( (h = gethostbyname(hostname)) ){
//      CONSOLE.Debug("Host name: %s", h->h_name);
//      CONSOLE.Debug("Address: %s", inet_ntoa(*((struct in_addr *)h->h_addr)));
        if( !IsPrivateAddress(((struct in_addr *)h->h_addr)->s_addr) ||
            !*cfg_listen_ip ){
          memcpy(&addr.sin_addr, h->h_addr, sizeof(struct in_addr));
          Self.SetIp(addr);
        }
      }
    }
  }
 next_step:

  // Call each btTracker's Initial()
  result = -1;
  pt = (tier_node *)0;
  for( tier = m_trackers; tier; pt = tier, tier = tnext ){
    tnext = tier->next_tier;
    pp = (tier_node *)0;
    for( p = tier; p; pp = p, p = pnext ){
      pnext = p->next;
      if( p->tracker->Initial() == 0 ) result = 0;
      else{  // remove bad tracker
        if( pp ){
          pp->next = pnext;
          delete p->tracker;
          delete p;
          p = pp;
        }else{
          if( pnext ){
            delete p->tracker;
            p->tracker = pnext->tracker;
            p->next = pnext->next;
            delete pnext;
            pnext = p;
            p = (tier_node *)0;
          }else{
            pt->next_tier = p->next_tier;
            delete p->tracker;
            delete p;
            if( pt ) tier = pt;
            else{
              m_trackers = tnext;
              tier = (tier_node *)0;
            }
          }
        }
      }
    }
  }

  return result;
}


dt_trackerstatus_t MultiTracker::GetStatus() const
{
  tier_node *tier, *p;
  dt_trackerstatus_t status, tmp;

  if( !m_stop && m_tracker && DT_FAILURE != m_tracker->GetResult() )
    return m_tracker->GetStatus();

  status = DT_TRACKER_FINISHED;
  for( tier = m_trackers; tier; tier = tier->next_tier ){
    for( p = tier; p; p = p->next ){
      switch( (tmp = p->tracker->GetStatus()) ){
        case DT_TRACKER_READY:
          return tmp;
        case DT_TRACKER_CONNECTING:
          status = tmp;
          break;
        case DT_TRACKER_FREE:
          if( DT_TRACKER_FINISHED == status )
            status = tmp;
          break;
        default:
          break;
      }
    }
  }
  return status;
}


void MultiTracker::Update()
{
  tier_node *tier, *p;

  for( tier = m_trackers; tier; tier = tier->next_tier ){
    for( p = tier; p; p = p->next ){
      p->tracker->Update();
    }
  }
}


void MultiTracker::RestartTracker()
{
  tier_node *tier, *p;

  m_tracker = (btTracker *)0;
  m_result = DT_NORMAL;
  for( tier = m_trackers; tier; tier = tier->next_tier ){
    for( p = tier; p; p = p->next ){
      p->tracker->RestartTracker();
    }
  }
}


bool MultiTracker::IsRestarting() const
{
  tier_node *tier, *p;

  if( m_tracker ) return m_tracker->IsRestarting();

  for( tier = m_trackers; tier; tier = tier->next_tier ){
    for( p = tier; p; p = p->next ){
      if( p->tracker->IsRestarting() ) return true;
    }
  }
  return false;
}


void MultiTracker::Stop()
{
  tier_node *tier, *p;

  m_stop = true;
  for( tier = m_trackers; tier; tier = tier->next_tier ){
    for( p = tier; p; p = p->next ){
      p->tracker->Stop();
    }
  }
}


int MultiTracker::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
  int max_sock = -1, tmp;
  tier_node *tier, *p, *pp;
  bool ok;

  for( tier = m_trackers; tier; tier = tier->next_tier ){
    pp = (tier_node *)0;
    ok = false;
    for( p = tier; p; pp = p, p = p->next ){
      if( DT_FAILURE == p->tracker->GetResult() ||
          DT_TRACKER_FINISHED == p->tracker->GetStatus() ){
        continue;
      }
      if( DT_TRACKER_READY == p->tracker->GetStatus() ||
          DT_TRACKER_CONNECTING == p->tracker->GetStatus() ||
          m_tracker == p->tracker ||
          p->tracker->IsQuitting() ||
          (!m_tracker && !pp) ||
          (pp && DT_NORMAL == p->tracker->GetResult() &&
            DT_FAILURE == pp->tracker->GetResult()) ){
        if( (tmp = p->tracker->IntervalCheck(rfdp, wfdp)) > max_sock )
          max_sock = tmp;
        if( tmp >= 0 ){
          if( FD_ISSET(tmp, rfdp) ) m_nset++;
          if( FD_ISSET(tmp, wfdp) ) m_nset++;
        }
        if( DT_FAILURE != p->tracker->GetResult() )
          ok = true;
      }
    }
    if( !ok ){  // tier exhausted
      for( p = tier; p; p = p->next ){
        p->tracker->ClearResult();
      }
      if( !m_tracker || tier->tracker == m_tracker ){
        m_tracker = (btTracker *)0;  // search all tiers
        m_result = DT_FAILURE;
      }
    }
  }
  return max_sock;
}


void MultiTracker::SocketReady(fd_set *rfdp, fd_set *wfdp, int *nfds,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  tier_node *tier, *p, *pp, *pnext;
  int ntmp;

  for( tier = m_trackers; tier && *nfds && m_nset; tier = tier->next_tier ){
    pp = (tier_node *)0;
    for( p = tier; p && *nfds && m_nset; pp = p, p = pnext ){
      pnext = p->next;
      if( DT_TRACKER_FREE == p->tracker->GetStatus() ) continue;
      ntmp = *nfds;
      if( 0==p->tracker->SocketReady(rfdp, wfdp, nfds, rfdnextp, wfdnextp) ){
        if( DT_SUCCESS == p->tracker->GetResult() ){
          m_report_time = p->tracker->GetReportTime();
          m_report_dl = p->tracker->GetReportDL();
          m_report_ul = p->tracker->GetReportUL();
          if( DT_TRACKER_FINISHED != p->tracker->GetStatus() ){
            if( p != tier ){
              btTracker *tmpt;
              pp->next = p->next;
              p->next = tier->next;
              tier->next = p;
              tmpt = tier->tracker;
              tier->tracker = p->tracker;
              p->tracker = tmpt;
              for( tier_node *q = p; q; q = q->next ){
                // Reset tier results except for the successful tracker.
                q->tracker->ClearResult();
              }
            }
            if( !m_tracker || DT_FAILURE == m_tracker->GetResult() ||
                (p->tracker->GetPeersCount() > 0 &&
                  p->tracker->GetPeersCount() >=
                    m_tracker->GetPeersCount() * 1.5) ){
              m_tracker = p->tracker;
              m_result = DT_SUCCESS;
              CONSOLE.Debug("Using tracker %s", m_tracker->GetURL());
            }
          }
        }
      }
      m_nset -= (ntmp - *nfds);
    }
  }
}


dt_count_t MultiTracker::GetSeedsCount() const
{
  if( m_tracker ) return m_tracker->GetSeedsCount();
  else return BTCONTENT.IsFull() ? 1 : 0;
}


dt_datalen_t MultiTracker::GetTotalDL()
{
  if( BTCONTENT.IsFull() ) m_totaldl = Self.TotalDL();
  return m_totaldl;
}


dt_datalen_t MultiTracker::GetTotalUL() const
{
  return Self.TotalUL();
}


const char *MultiTracker::StatusInfo()
{
  dt_trackerstatus_t status;

  status = GetStatus();
  snprintf(m_status_info, sizeof(m_status_info), "%s%s%s",
    DT_SUCCESS == m_result ? "OK" :
      (DT_FAILURE == m_result ? "Failed" : "Unknown"),
    DT_TRACKER_FREE == status ? "" : ", ",
    DT_TRACKER_CONNECTING == status ? "connecting" :
      (DT_TRACKER_READY == status ? "connected" :
        (DT_TRACKER_FINISHED == status ? "finished" :
          (m_stop ? "quitting" :
            (IsRestarting() ? "restarting" : "")))));
  m_status_info[sizeof(m_status_info) - 1] = '\0';
  return m_status_info;
}


void MultiTracker::PrintTiers() const
{
  tier_node *tier, *p;
  int n = 0;

  if( !m_trackers || (!m_trackers->next && !m_trackers->next_tier) ) return;

  for( tier = m_trackers; tier; tier = tier->next_tier, n++ ){
    CONSOLE.Print("Announce tier %d:", n);
    for( p = tier; p; p = p->next ){
      CONSOLE.Print("  %s", p->tracker->GetURL());
    }
  }
}

