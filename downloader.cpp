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
#include "bttime.h"

#define MAX_SLEEP 1

time_t now = time((time_t *)0);
bool g_disk_access = false;

void Downloader()
{
  int nfds = 0, maxfd;
  int maxfd_tracker, maxfd_ctcs, maxfd_console, maxfd_peer;
  struct timeval timeout;
  fd_set rfd, rfdnext;
  fd_set wfd, wfdnext;
  int stopped = 0, f_poll = 0;
  double maxsleep;

  FD_ZERO(&rfdnext);
  FD_ZERO(&wfdnext);

  do{
    g_disk_access = 0;
    UpdateTime();

    if( !stopped ){
      if( !TRACKER.IsQuitting() && BTCONTENT.SeedTimeout() )
        TRACKER.Stop();
      if( TRACKER.IsQuitting() ){
        stopped = 1;
        WORLD.Pause();
        if( *cfg_ctcs ) CTCS.Send_Status();
      }
    }

    maxfd = -1;
    maxsleep = -1;
    rfd = rfdnext;
    wfd = wfdnext;
    maxfd_tracker = maxfd_ctcs = maxfd_console = -1;

    if( f_poll ){
      FD_ZERO(&rfd);
      FD_ZERO(&wfd);  // remove non-peers from sets
      maxsleep = 0;  // waited for bandwidth--poll now
    }else{
      WORLD.DontWaitBW();
      maxfd_tracker = TRACKER.IntervalCheck(&rfd, &wfd);
      if( maxfd_tracker > maxfd ) maxfd = maxfd_tracker;
      if( *cfg_ctcs ){
        maxfd_ctcs = CTCS.IntervalCheck(&rfd, &wfd);
        if( maxfd_ctcs > maxfd ) maxfd = maxfd_ctcs;
      }
      maxfd_console = CONSOLE.IntervalCheck(&rfd, &wfd);
      if( maxfd_console > maxfd ) maxfd = maxfd_console;
      if( WORLD.IsIdle() ){
        if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() &&
            !BTCONTENT.NeedFlush() ){
          if( BTCONTENT.CheckNextPiece() < 0 ){
            CONSOLE.Warning(1, "Error while checking piece %d of %d",
              (int)BTCONTENT.CheckedPieces(), (int)BTCONTENT.GetNPieces());
            TRACKER.Stop();
            maxsleep = 2;
          }
          CheckTime();
        }
      }
    }
    maxfd_peer = WORLD.IntervalCheck(&rfd, &wfd);
    if( maxfd_peer > maxfd ) maxfd = maxfd_peer;

    if( !f_poll ){
      UpdateTime();
      while( BTCONTENT.NeedFlush() && WORLD.IsIdle() ){
        BTCONTENT.FlushQueue();
        CheckTime();
      }
      while( BTCONTENT.NeedMerge() && WORLD.IsIdle() ){
        BTCONTENT.MergeNext();
        CheckTime();
      }
    }

    rfdnext = rfd;
    wfdnext = wfd;

    if( g_disk_access && WORLD.IdleState() == DT_IDLE_POLLING ){
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

    UpdateTime();

    if( !f_poll && nfds > 0 ){
      if( maxfd_tracker >= 0 )
        TRACKER.SocketReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
      if( nfds > 0 && maxfd_ctcs >= 0 )
        CTCS.SocketReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
      if( nfds > 0 && maxfd_console >= 0 )
        CONSOLE.User(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
    }
    if( nfds > 0 && maxfd_peer >= 0 )
      WORLD.AnyPeerReady(&rfd, &wfd, &nfds, &rfdnext, &wfdnext);
  }while( TRACKER.GetStatus() != DT_TRACKER_FINISHED ||
          TRACKER.IsRestarting() );
}
