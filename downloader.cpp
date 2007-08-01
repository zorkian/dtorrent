#include <sys/types.h>

#include <time.h>

#ifdef WINDOWS
#include <winsock2.h>

#else
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "peerlist.h"
#include "tracker.h"
#include "btcontent.h"
#include "ctcs.h"
#include "btconfig.h"
#include "bttime.h"

time_t now = (time_t) 0;

void Downloader()
{
  int nfds,maxfd,r;
  struct timeval timeout;
  fd_set rfd;
  fd_set wfd;
  int stopped = 0;

  time(&now);
  do{
    if( !stopped &&
        ( BTCONTENT.SeedTimeout(&now) ||
          (( cfg_exit_zero_peers || Tracker.IsQuitting() ) &&
            !WORLD.TotalPeers()) ) ){
        Tracker.SetStoped();
        stopped = 1;
        if( arg_ctcs ) CTCS.Send_Status();
    }
    
    FD_ZERO(&rfd); FD_ZERO(&wfd);
    maxfd = Tracker.IntervalCheck(&now,&rfd, &wfd);
    if( arg_ctcs ){
      r = CTCS.IntervalCheck(&now,&rfd, &wfd);
      if( r > maxfd ) maxfd = r;
    }
    r = WORLD.FillFDSET(&now,&rfd,&wfd);
    if( r > maxfd ) maxfd = r;

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    nfds = select(maxfd + 1,&rfd,&wfd,(fd_set*) 0,&timeout);
    time(&now);

    if(nfds > 0){
      if(T_FREE != Tracker.GetStatus()) Tracker.SocketReady(&rfd,&wfd,&nfds);
      if(nfds > 0 && T_FREE != CTCS.GetStatus())
        CTCS.SocketReady(&rfd,&wfd,&nfds);
      if(nfds > 0) WORLD.AnyPeerReady(&rfd,&wfd,&nfds);
    }
  } while(Tracker.GetStatus() != T_FINISHED || Tracker.IsPaused());
}
