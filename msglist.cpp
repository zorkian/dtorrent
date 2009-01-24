#include "config.h"
#include <stdarg.h>
#include <stdio.h>

#include "msglist.h"
#include "bttime.h"
#include "btconfig.h"

#if !defined(HAVE_VSNPRINTF)
#include "compat.h"
#endif


const dt_message *MessageList::GetBrief() const
{
  dt_message *p, *first = m_firstmsg;
  int count = 0;

  for( p = m_firstmsg; p; p = p->next ){
    if( ++count > 5 ) first = first->next;
  }
  return first;
}


const char *MessageList::AddMessage(int sev, const char *message)
{
  dt_message *msg;

  if( m_lastmsg && 0==strcmp(m_lastmsg->text, message) ){
    m_lastmsg->timestamp = now;
    if( sev > 0 && sev < m_lastmsg->severity )
      m_lastmsg->severity = sev;
    return m_lastmsg->text;
  }
  if( m_count > 99 ){
    msg = m_firstmsg;
    m_firstmsg = m_firstmsg->next;
    delete msg;
    m_count--;
  }
  if( (msg = new dt_message) && (msg->text = new char[strlen(message) + 1]) ){
    msg->timestamp = now;
    msg->severity = sev;
    strcpy(msg->text, message);
    if( m_lastmsg ){
      msg->next = m_lastmsg->next;
      m_lastmsg->next = msg;
      m_lastmsg = msg;
    }else{
      m_firstmsg = m_lastmsg = msg;
    }
    m_count++;
    return msg->text;
  }else if( msg ) delete []msg;
  return (char *)0;
}


const char *MessageList::AddMessage(size_t msglen, int sev, const char *format,
  va_list ap)
{
  char *message = (char *)0;
  const char *result = (char *)0;

  if( msglen > 0 ){
    msglen++;
    if( (message = new char[msglen]) ){
      vsnprintf(message, msglen, format, ap);
    }
  }else if( *format ){
    int ret;
    msglen = 81;
    while( (message = new char[msglen]) ){
      ret = vsnprintf(message, msglen, format, ap);
      if( ret > 0 && ret < (int)msglen ) break;
      delete []message;
      if( ret > (int)msglen ) msglen = ret;
      else msglen += 80;
    }
  }
  if( message ){
    result = AddMessage(sev, message);
    delete []message;
  }
  return result;
}


void MessageList::Expire()
{
  dt_message *p;

  if( 0==*cfg_msg_expiration ) return;

  while( (p = m_firstmsg) ){
    if( p->timestamp + *cfg_msg_expiration * 3600 < now ){
      m_firstmsg = m_firstmsg->next;
      delete p;
      m_count--;
    }else break;
  }
  if( !m_firstmsg ){
    m_lastmsg = (dt_message *)0;
    m_count = 0;  // failsafe
  }
}


void MessageList::Clear(dt_message *last)
{
  dt_message *p;
  bool done = false;

  while( !done && (p = m_firstmsg) ){
    if( last == m_firstmsg ) done = true;
    m_firstmsg = m_firstmsg->next;
    delete p;
    m_count--;
  }
  if( !m_firstmsg ){
    m_lastmsg = (dt_message *)0;
    m_count = 0;  // failsafe
  }
}

