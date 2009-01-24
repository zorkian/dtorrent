#ifndef MSGLIST_H
#define MSGLIST_H

#include <time.h>


struct dt_message{
  dt_message *next;
  time_t timestamp;
  int severity;
  char *text;

  dt_message(){ text = (char *)0; next = (dt_message *)0; }
  ~dt_message(){ if( text ) delete []text; }
};


class MessageList
{
 private:
  dt_message *m_firstmsg;
  dt_message *m_lastmsg;
  int m_count;

 public:
  MessageList(){ m_firstmsg = m_lastmsg = (dt_message *)0; m_count = 0; }
  ~MessageList(){ Clear(); }

  const dt_message *GetMessages() const { return m_firstmsg; }
  const dt_message *GetBrief() const;
  const char *AddMessage(int sev, const char *message);
  const char *AddMessage(size_t len, int sev, const char *format, va_list ap);
  void Expire();
  void Clear(dt_message *last=(dt_message *)0);
};

#endif  // MSGLIST_H

