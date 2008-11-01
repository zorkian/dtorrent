#include "def.h"

#include <sys/types.h>

#if !defined(HAVE_SYS_TIME_H) || defined(TIME_WITH_SYS_TIME)
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "peerlist.h"
#include "tracker.h"
#include "btcontent.h"
#include "ctcs.h"
#include "btconfig.h"
#include "console.h"

#define MAX_SLEEP 1

time_t now = time((time_t *)0);

void Downloader()
{
  int nfds = 0, maxfd, r;
  struct timeval timeout;
  fd_set rfd, rfdnext;
  fd_set wfd, wfdnext;
  int stopped = 0, f_poll = 0;
  double maxsleep;
  time_t then;

  FD_ZERO(&rfdnext);
  FD_ZERO(&wfdnext);

  do{
    WORLD.ClearIdled();
    time(&now);

    if( !stopped ){
      if( !Tracker.IsQuitting() && BTCONTENT.SeedTimeout() )
        Tracker.SetStoped();
      if( Tracker.IsQuitting() && !Tracker.IsRestarting() ){
        stopped = 1;
        WORLD.Pause();
        if( *cfg_ctcs ) CTCS.Send_Status();
      }
    }

    maxfd = -1;
    maxsleep = -1;
    rfd = rfdnext;
    wfd = wfdnext;

    if( f_poll ){
      FD_ZERO(&rfd);
      FD_ZERO(&wfd);  // remove non-peers from sets
      maxsleep = 0;  // waited for bandwidth--poll now
    }else{
      WORLD.DontWaitBW();
      r = Tracker.IntervalCheck(&rfd, &wfd);
      if( r > maxfd ) maxfd = r;
      if( *cfg_ctcs ){
        r = CTCS.IntervalCheck(&rfd, &wfd);
        if( r > maxfd ) maxfd = r;
      }
      r = CONSOLE.IntervalCheck(&rfd, &wfd);
      if( r > maxfd ) maxfd = r;
      if( WORLD.IsIdle() ){
        if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() &&
            !BTCONTENT.NeedFlush() ){
          if( BTCONTENT.CheckNextPiece() < 0 ){
            CONSOLE.Warning(1, "Error while checking piece %d of %d",
              (int)BTCONTENT.CheckedPieces(), (int)BTCONTENT.GetNPieces());
            Tracker.SetStoped();
            maxsleep = 2;
          }
          WORLD.SetIdled();
        }
      }
    }
    r = WORLD.IntervalCheck(&rfd, &wfd);
    if( r > maxfd ) maxfd = r;

    if( !f_poll ){
      time(&now);
      while( BTCONTENT.NeedFlush() && WORLD.IsIdle() ){
        BTCONTENT.FlushQueue();
        WORLD.SetIdled();
      }
      while( BTCONTENT.NeedMerge() && WORLD.IsIdle() ){
        BTCONTENT.MergeNext();
        WORLD.SetIdled();
      }
    }

    rfdnext = rfd;
    wfdnext = wfd;

    if( WORLD.Idled() && WORLD.IdleState() == DT_IDLE_POLLING ){
      maxsleep = 0;
    }else if( maxsleep < 0 ){  // not yet set
      maxsleep = WORLD.WaitBW();  // must do after intervalchecks!
      if( maxsleep <= -100 ) maxsleep = 0;
      else if( maxsleep <= 0 || maxsleep > MAX_SLEEP ) maxsleep = MAX_SLEEP;
    }

    timeout.tv_sec = (long)maxsleep;
    timeout.tv_usec = (long)((maxsleep - (long)maxsleep) * 1000000);

    nfds = select(maxfd + 1, &rfd, &wfd, (fd_set *)0, &timeout);
    if( nfds < 0 ){
      CONSOLE.Debug("Error from select:  %s", strerror(errno));
      FD_ZERO(&rfdnext);
      FD_ZERO(&wfdnext);
      nfds = 0;
    }

    if( f_poll ) f_poll = 0;
    else if( nfds > 0 ) WORLD.DontWaitBW();
    else if( maxsleep > 0 && maxsleep < MAX_SLEEP ) f_poll = 1;

    then = now;
    time(&now);
    if( now == then-1 ) now = then;

    if( !f_poll && nfds > 0 ){
      if( DT_TRACKER_FREE != Tracker.GetStatus() )
        Tracker.SocketReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
      if( nfds > 0 && DT_TRACKER_FREE != CTCS.GetStatus() )
        CTCS.SocketReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
      if( nfds > 0 )
        CONSOLE.User(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
    }
    if( nfds > 0 )
      WORLD.AnyPeerReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
  }while( Tracker.GetStatus() != DT_TRACKER_FINISHED ||
          Tracker.IsRestarting() );
}
