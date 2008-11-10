#ifndef CONSOLE_H
#define CONSOLE_H

#include "def.h"
#include <sys/types.h>  // fd_set, struct stat, fork()
#include <sys/stat.h>   // fstat()
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

class ConfigGen;

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


class ConStream;
struct stream_node{
  stream_node *next;
  const ConStream *stream;
};

class ConDevice
{
 private:
  bool m_newline;
  int m_lines;
  dev_t m_st_dev;
  ino_t m_st_ino;
  stream_node *m_streams;

 public:
  ConDevice(const struct stat *sb);
  ~ConDevice();

  bool Newline() const { return m_newline; }
  void ClearNewline() { m_newline = false; }
  void AddLine() { m_newline = true; m_lines++; }
  int Lines() const { return m_lines; }
  void Clear() { m_lines = 0; }

  bool SameDev(const struct stat *sb) const {
    return (sb->st_dev == m_st_dev && sb->st_ino == m_st_ino);
  }
  int Register(const ConStream *stream);
  bool Deregister(const ConStream *stream);
};


class ConStream
{
 private:
  FILE *m_stream;
  char *m_name;
  dt_conmode_t m_inputmode;
  ConDevice *m_device;

  unsigned char m_suspend:1;
  unsigned char m_filemode:1;  // 0=read, 1=write
  unsigned char m_restore:1;
  unsigned char m_reserved:5;

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
  int GetSize(bool getrows) const;
  const ConDevice *GetDevice() const { return m_device; }

 public:
  ConStream();
  ~ConStream();

  void Close();
  int Associate(FILE *stream, const char *name, int mode);
  const char *GetName() const { return m_name; }
  int GetMode() const { return m_filemode ? 1 : 0; }
  int Fileno() const { return m_stream ? fileno(m_stream) : -1; }
  void Suspend(){ m_suspend = 1; }
  void Resume(){ m_suspend = 0; }
  bool IsSuspended() const { return m_suspend ? true : false; }

  bool SameDev(const ConStream *master) const {
    return (m_device == master->GetDevice());
  }
  dt_conmode_t GetInputMode() const { return m_inputmode; }
  void SetInputMode(dt_conmode_t keymode);
  void PreserveMode();
  void RestoreMode();
  int IsTTY() const;
  int Rows() const { return GetSize(true); }
  int Cols() const { return GetSize(false); }

  void Output(const char *message, va_list ap);
  void Output_n(const char *message, va_list ap);
  void Update(const char *message, va_list ap);
  char *Input(char *field, size_t length);
  int CharIn();
  bool Newline() const { return m_device->Newline(); }
  int Lines() const { return m_device->Lines(); }
  void Clear() { m_device->Clear(); }

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
  char m_buffer[80], m_debug_buffer[80];

  struct{
    int mode, n_opt;
    dt_conchan_t channel;
  }opermenu;

  struct{
    int mode, n_opt, start_opt, current_start, next_start;
    ConfigGen *selected;
  }configmenu;

  struct{
    char pending;
    int inc, count;
  }m_user;

  typedef void (Console::*statuslinefn)(char buffer[], size_t length);
  statuslinefn m_statusline[STATUSLINES];

  Rate m_pre_dlrate, m_pre_ulrate;

  ConStream m_stdout, m_stderr, m_stdin, m_off;
  ConStream *m_streams[O_NCHANNELS+1];

  int OpenNull(int nullfd, ConStream *stream, int sfd);
  int OperatorMenu(const char *param=(char *)0);
  void ShowFiles();
  void StatusLine0(char buffer[], size_t length);
  void StatusLine1(char buffer[], size_t length);

 public:
  Console();
  ~Console();

  void Init();
  void Shutdown();
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
  const char *StatusLine(int format=-1);

  void cpu();

  RETSIGTYPE Signal(int sig_no);
  void Daemonize();

  int Configure(const char *param=(char *)0);
};

extern Console CONSOLE;

#endif  // CONSOLE_H

