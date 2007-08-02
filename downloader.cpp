#include <sys/types.h>

#include <sys/time.h>
#include <time.h>

#ifdef WINDOWS
#include <Winsock2.h>
#else
#include <stdio.h>   // autoconf manual: Darwin + others prereq for stdlib.h
#include <stdlib.h>  // autoconf manual: Darwin prereq for sys/socket.h
#include <sys/socket.h>
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
#include "console.h"

#define MAX_SLEEP 1

time_t now = (time_t) 0;

void Downloader()
{
  int nfds = 0, prevnfds = 0, maxfd, r;
  struct timeval timeout;
  fd_set rfd, rfdnext;
  fd_set wfd, wfdnext;
  int stopped = 0;
  struct timespec nowspec;
  double maxsleep, prevsleep = 0;

  FD_ZERO(&rfdnext); FD_ZERO(&wfdnext);

  time(&now);
  do{
    if( !stopped ){
      if( !Tracker.IsQuitting() && BTCONTENT.SeedTimeout() )
        Tracker.SetStoped();
      if( Tracker.IsQuitting() ){
        stopped = 1;
        if( arg_ctcs ) CTCS.Send_Status();
      }
    }

    prevsleep = maxsleep;
    maxsleep = -1;

    rfd = rfdnext;
    wfd = wfdnext;

    if( 0==prevnfds && prevsleep > 0 && prevsleep < MAX_SLEEP ){
      FD_ZERO(&rfd); FD_ZERO(&wfd);
      maxsleep = 0;  // waited for bandwidth--poll now
    }
    else if( WORLD.IsIdle() ){
      if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
        if( BTCONTENT.CheckNextPiece() < 0 ){
          CONSOLE.Warning(1, "Error while checking piece %d of %d",
            (int)(BTCONTENT.CheckedPieces()), (int)(BTCONTENT.GetNPieces()));
          Tracker.SetStoped();
          maxsleep = 2;
        }else maxsleep = 0;
      }
      maxfd = Tracker.IntervalCheck(&rfd, &wfd);
      if( arg_ctcs ){
        r = CTCS.IntervalCheck(&rfd, &wfd);
        if( r > maxfd ) maxfd = r;
      }
      r = CONSOLE.IntervalCheck(&rfd, &wfd);
      if( r > maxfd ) maxfd = r;
    }
    r = WORLD.FillFDSET(&rfd, &wfd);
    if( r > maxfd ) maxfd = r;

    rfdnext = rfd;
    wfdnext = wfd;

   again:
    if( maxsleep < 0 ){
      maxsleep = WORLD.WaitBW();  // must do after intervalchecks/fillfdset!
      if( maxsleep <= 0 || maxsleep > MAX_SLEEP ) maxsleep = MAX_SLEEP;
    }

    timeout.tv_sec = (long)maxsleep;
    timeout.tv_usec = (long)( (maxsleep-(long)maxsleep) * 1000000 );

    if( maxfd < 0 ) maxfd = 0;
    nfds = select(maxfd + 1,&rfd,&wfd,(fd_set*) 0,&timeout);

    if( nfds > 0 && maxsleep > 0 ) WORLD.DontWaitBW();
    else if( 0==nfds && 0==maxsleep && prevsleep > 0 ){
      prevsleep = maxsleep;
      maxsleep = -1;
      goto again;
    }

    time(&now);

    prevnfds = nfds;
    if( (maxsleep > 0 || 0==prevsleep) && nfds > 0 ){
      if(T_FREE != Tracker.GetStatus())
        Tracker.SocketReady(&rfd,&wfd,&nfds,&rfdnext,&wfdnext);
      if(nfds > 0 && T_FREE != CTCS.GetStatus())
        CTCS.SocketReady(&rfd,&wfd,&nfds,&rfdnext,&wfdnext);
      if(nfds > 0)
        CONSOLE.User(&rfd,&wfd,&nfds,&rfdnext,&wfdnext);
    }
    if(nfds > 0)
      WORLD.AnyPeerReady(&rfd,&wfd,&nfds,&rfdnext,&wfdnext);
  } while(Tracker.GetStatus() != T_FINISHED || Tracker.IsPaused());
}
