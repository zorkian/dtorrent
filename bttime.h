#ifndef BTTIME_H
#define BTTIME_H

#include <time.h>

extern time_t now;
extern bool g_disk_access;  // recently accessed the disk

inline void UpdateTime(){
  time_t then = now;
  time(&now);
  if( now == then - 1 ) now = then;
}

/* DiskAccess flags system storage accesses to indicate that the clock ("now")
   may need to be updated.  This does not directly update the clock since it
   would be updated more frequently than necessary.
   Console (and potentially other similar mechanisms) doesn't use DiskAccess
   since it may cause frequent disk access (essentially as a side effect) and
   setting g_disk_access might prevent performing any idle-time tasks.  Instead
   it may use UpdateTime when appropriate.
*/
inline void DiskAccess(){ g_disk_access = true; }

/* Debug version, needs to insure that logging here doesn't trigger a
   recursion loop.
#include "console.h"
inline void DiskAccess(){
  static bool recursionblock = false;
  if( !recursionblock ){
    recursionblock = true;
    CONSOLE.Debug("DiskAccess");
    g_disk_access = true;
  }
  recursionblock = false;
}
*/

inline void CheckTime(){ if( g_disk_access ) UpdateTime(); }

#endif  // BTTIME_H

