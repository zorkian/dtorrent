#ifndef WINDOWS

#include <sys/types.h>
#include <signal.h>

#include "btcontent.h"
#include "tracker.h"
#include "peerlist.h"
#include "btconfig.h"
#include "sigint.h"

void sig_catch(int sig_no)
{
  if(SIGINT == sig_no || SIGTERM == sig_no){
    if( Tracker.IsPaused() ) Tracker.ClearPause();
    Tracker.SetStoped();
    signal(sig_no,sig_catch2);
  }
}

static void sig_catch2(int sig_no)
{
  if(SIGINT == sig_no || SIGTERM == sig_no){
    if( cfg_cache_size ) BTCONTENT.FlushCache();
    if( arg_bitfield_file ) BTCONTENT.pBF->WriteToFile(arg_bitfield_file);
    WORLD.CloseAll();
    signal(sig_no,SIG_DFL);
    raise(sig_no);
  }
}

#endif
