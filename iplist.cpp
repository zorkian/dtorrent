#include "iplist.h"  // def.h
#include <string.h>

IpList IPQUEUE;

void IpList::_Empty()
{
  IPLIST *node = ipl_head;
  while( ipl_head ){
    node = ipl_head;
    ipl_head = node->next;
    delete node;
  }
  count = 0;
}

int IpList::Add(const struct sockaddr_in *psin)
{
  IPLIST *node = ipl_head;
  for( ; node; node = node->next ){
    if( memcmp(psin, &node->address, sizeof(struct sockaddr_in)) == 0 ){
      // already have a connection to this address
      return -1;
    }
  }

  node = new IPLIST;
#ifndef WINDOWS
  if( !node ) return -1;
#endif
  count++;
  memcpy(&node->address, psin, sizeof(struct sockaddr_in));
  node->next = ipl_head;
  ipl_head = node;
  return 0;
}

int IpList::Pop(struct sockaddr_in *psin)
{
  IPLIST *node = (IPLIST *)0;
  if( !ipl_head ) return -1;
  node = ipl_head;
  ipl_head = ipl_head->next;

  count--;
  memcpy(psin, &node->address, sizeof(struct sockaddr_in));
  delete node;
  return 0;
}
