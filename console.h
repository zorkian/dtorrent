#ifndef CONSOLE_H
#define CONSOLE_H

#include "def.h"
#include <sys/types.h>  // fd_set
#include <stdarg.h>
#include <stdio.h>

#if defined(USE_TERMIOS)
#include <termios.h>
#elif defined(USE_TERMIO)
#include <termio.h>
#elif defined(USE_SGTTY)
#include <sgtty.h>
#endif

#include "bttypes.h"
#include "rate.h"

// Number of status line formats
#define STATUSLINES 2

// Output channel labels
#define O_NCHANNELS 4  // number of output channels
enum dt_conchan_t{
  O_NORMAL   = 0,
  O_INTERACT = 1,
  O_WARNING  = 2,
  O_DEBUG    = 3,
  O_INPUT    = 4       // not an output!  do not include in above count.
};

// Console input modes
enum dt_conmode_t{
  DT_CONMODE_CHARS,
  DT_CONMODE_LINES
};


class ConStream
{
 private:
  FILE *m_stream;
  char *m_name;
  dt_conmode_t m_inputmode;

  unsigned char m_newline:1;
  unsigned char m_suspend:1;
  unsigned char m_filemode:1;
  unsigned char m_restore:1;
  unsigned char m_reserved:4;

#if defined(USE_TERMIOS)
  struct termios m_original;
#elif defined(USE_TERMIO)
  struct termio m_original;
#elif defined(USE_SGTTY)
  struct sgttyb m_original;
#endif

  void _newline();
  int _convprintf(const char *format, va_list ap);
  void Error(int sev, const char *message, ...);

 public:
  ConStream();
  ~ConStream();

  void Close();
  void Associate(FILE *stream, const char *name, int mode);
  const char *GetName() const { return m_name; }
  int GetMode() const { return m_filemode ? 1 : 0; }
  int Fileno() const { return m_stream ? fileno(m_stream) : -1; }
  int GetNewline() const { return m_newline ? 1 : 0; }
  void SyncNewline(ConStream *master){ m_newline = master->GetNewline(); }
  void Suspend(){ m_suspend = 1; }
  void Resume(){ m_suspend = 0; }
  int IsSuspended(){ return m_suspend ? 1 : 0; }

  int SameDev(ConStream *master) const;
  dt_conmode_t GetInputMode() const { return m_inputmode; }
  void SetInputMode(dt_conmode_t keymode);
  void PreserveMode();
  void RestoreMode();
  int IsTTY() const;

  int Output(const char *message, va_list ap);
  int Output_n(const char *message, va_list ap);
  int Update(const char *message, va_list ap);
  char *Input(char *field, size_t length);
  int CharIn();

  int Eof() const { return feof(m_stream); }
};


class Console
{
 private:
  dt_conmode_t m_conmode;

  unsigned char m_live_idx:2;
  unsigned char m_skip_status:1;
  unsigned char m_status_last:1;
  unsigned char m_reserved:4;

  int m_status_format;
  int m_oldfd;
  int m_status_len;

  typedef void (Console::*statuslinefn)(char buffer[], size_t length);
  statuslinefn m_statusline[STATUSLINES];

  Rate m_pre_dlrate, m_pre_ulrate;

  ConStream m_stdout, m_stderr, m_stdin, m_off;
  ConStream *m_streams[O_NCHANNELS+1];

  int OpenNull(int nullfd, ConStream *stream, int sfd);
  void SyncNewlines(int master);
  int OperatorMenu(const char *param);
  void ShowFiles();
  void StatusLine0(char buffer[], size_t length);
  void StatusLine1(char buffer[], size_t length);

 public:
  Console();
  ~Console();

  void Init();
  int IntervalCheck(fd_set *rfdp, fd_set *wfdp);
  void User(fd_set *rfdp, fd_set *wfdp, int *nready,
    fd_set *rfdnextp, fd_set *wfdnextp);
  void Status(int immediate);

  void Print(const char *message, ...);
  void Print_n(const char *message, ...);
  void Update(const char *message, ...);
  void Warning(int sev, const char *message, ...);
  void Debug(const char *message, ...);
  void Debug_n(const char *message, ...);
  void Interact(const char *message, ...);
  void Interact_n(const char *message, ...);
  void InteractU(const char *message, ...);
  char *Input(const char *prompt, char *field, size_t length);

  const char *GetChannel(dt_conchan_t channel) const {
    return m_streams[channel]->GetName();
  }
  int ChangeChannel(dt_conchan_t channel, const char *param, int notify = 1);
  const char *StatusLine();

  void cpu();

  RETSIGTYPE Signal(int sig_no);
  void Daemonize();
};

extern Console CONSOLE;

#endif  // CONSOLE_H

