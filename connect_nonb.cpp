#include "connect_nonb.h"  // def.h

#include <errno.h>

/* Returns:
    0 on successful connection
   -1 on failure
   -2 if connection is delayed (in progress)
*/
int connect_nonb(SOCKET sk, const struct sockaddr *psa)
{
  int r;
  r = connect(sk, psa, sizeof(struct sockaddr));
  if( r < 0 && EINPROGRESS == errno ) r = -2;
  return r;
}
