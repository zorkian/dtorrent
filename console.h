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
#include "registry.h"

class ConfigGen;

// Number of status line formats
#define STATUSLINES 2

// Output channel labels
#define DT_NCHANNELS 5  // number of I/O channels
enum dt_conchan_t{
  DT_CHAN_NORMAL   = 0,
  DT_CHAN_INTERACT = 1,
  DT_CHAN_WARNING  = 2,
  DT_CHAN_DEBUG    = 3,
  DT_CHAN_INPUT    = 4
};

// Console input modes
enum dt_conmode_t{
  DT_CONMODE_NONE,
  DT_CONMODE_CHARS,
  DT_CONMODE_LINES
};


class ConStream;
class ConChannel;


/* ConDevice is a collection of information about the underlying device, not
   an interface to the device. */
class ConDevice
{
 private:
  dt_conmode_t m_inputmode;
  bool m_newline;
  int m_lines;
  dev_t m_st_dev;
  ino_t m_st_ino;
  bool m_tty;
  dt_conchan_t m_lastuser;
  Registry<const ConStream *> m_streams;

 public:
  ConDevice(const struct stat *sb, bool tty);

  bool Newline() const { return m_newline; }
  void ClearNewline() { m_newline = false; }
  void AddLine() { m_newline = true; m_lines++; }
  int Lines() const { return m_lines; }
  void Clear() { m_lines = 0; }

  dt_conmode_t GetInputMode() const { return m_inputmode; }
  void SetInputMode(dt_conmode_t keymode){ m_inputmode = keymode; }

  bool SameDev(const struct stat *sb) const {
    return (sb->st_dev == m_st_dev && sb->st_ino == m_st_ino);
  }
  bool IsTTY() const { return m_tty; }

  void SetUser(dt_conchan_t channel){ m_lastuser = channel; }
  dt_conchan_t LastUser() const { return m_lastuser; }

  int Register(const ConStream *stream){ return m_streams.Register(stream); }
  bool Deregister(const ConStream *stream){
    return m_streams.Deregister(stream);
  }
};


class ConStream
{
 private:
  FILE *m_file;
  char *m_name;
  ConDevice *m_device;
  Registry<const ConChannel *> m_channels;

  unsigned char m_permanent:1;
  unsigned char m_suspend:1;
  unsigned char m_read:1;
  unsigned char m_write:1;
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
  int GetSize(bool getrows) const;
  void SetMode(bool reading, bool writing){
    m_read = reading; m_write = writing;
    if( !m_read && !m_write ) Suspend();
  }

 public:
  ConStream();
  ~ConStream();

  void Close();
  void Disassociate();
  int Associate(FILE *stream, const char *name, bool reading, bool writing);
  int Reassociate();
  FILE *Open(const char *name, int mode);

  const char *GetName() const { return m_name; }
  bool CanRead() const { return m_read ? true : false; }
  bool CanWrite() const { return m_write ? true : false; }
  int Fileno() const { return m_file ? fileno(m_file) : -1; }
  void Suspend(){ m_suspend = 1; SetInputMode(DT_CONMODE_NONE); }
  void Resume(){ m_suspend = 0; SetInputMode(DT_CONMODE_CHARS); }
  bool IsSuspended() const { return m_suspend ? true : false; }
  void SetPermanent(){ m_permanent = 1; }
  bool IsPermanent() const { return m_permanent ? true : false; }

  dt_conmode_t GetInputMode() const {
    return m_device ? m_device->GetInputMode() : DT_CONMODE_NONE;
  }
  void SetInputMode(dt_conmode_t keymode);
  void PreserveMode();
  void RestoreMode();

  bool SameStream(const char *streamname) const;
  bool SameDev(const ConStream *that) const {
    return (that && that->SameDev(m_device));
  }
  bool SameDev(const ConDevice *other) const { return (m_device == other); }

  bool IsTTY() const { return (m_device && m_device->IsTTY()); }
  int Rows() const { return GetSize(true); }
  int Cols() const { return GetSize(false); }

  void Output(dt_conchan_t channel, const char *message, va_list ap);
  void Output_n(dt_conchan_t channel, const char *message, va_list ap);
  void Update(dt_conchan_t channel, const char *message, va_list ap);
  char *Input(char *field, size_t length);
  bool Newline() const { return m_device ? m_device->Newline() : true; }
  int Lines() const { return m_device ? m_device->Lines() : 0; }
  void Clear() { if( m_device ) m_device->Clear(); }

  bool Eof() const { return feof(m_file) ? true : false; }

  int Register(const ConChannel *channel){
    return m_channels.Register(channel);
  }
  bool Deregister(const ConChannel *channel){
    return (m_channels.Deregister(channel) && !IsPermanent());
  }
};


class ConChannel
{
 private:
  dt_conchan_t m_id;
  bool m_write;
  int m_oldfd;

  ConStream *m_stream;
  ConfigGen *m_cfg;

 public:
  ConChannel();

  void Init(dt_conchan_t channel, int mode, ConStream *stream, ConfigGen *cfg);
  void Associate(ConStream *stream, bool notify);
  int Oldfd();

  int GetMode() const { return m_write ? 1 : 0; }
  bool IsOutput() const { return m_write; }
  bool IsInput() const { return !m_write; }
  const char *GetName() const;
  const char *StreamName() const { return m_stream ? m_stream->GetName() : ""; }
  bool IsSuspended() const {
    return (!m_stream || m_stream->IsSuspended());
  }
  int Fileno() const { return m_stream ? m_stream->Fileno() : -1; }
  void Suspend(){ if( m_stream ) m_stream->Suspend(); }
  void Resume(){ if( m_stream ) m_stream->Resume(); }
  bool PermanentStream() const {
    return m_stream ? m_stream->IsPermanent() : true;
  }

  dt_conmode_t GetInputMode() const {
    return m_stream ? m_stream->GetInputMode() : DT_CONMODE_NONE;
  }
  void SetInputMode(dt_conmode_t keymode){
    if( m_stream ) m_stream->SetInputMode(keymode);
  }
  void PreserveMode(){ if( m_stream ) m_stream->PreserveMode(); }
  void RestoreMode(){ if( m_stream ) m_stream->RestoreMode(); }

  bool SameStream(const char *streamname) const {
    return (m_stream && m_stream->SameStream(streamname));
  }
  bool SameStream(const ConStream *stream) const {
    return (stream == m_stream);
  }
  bool SameDev(const ConChannel *that) const {
    return (that && that->SameDev(m_stream));
  }
  bool SameDev(const ConStream *other) const {
    return (m_stream == other || (other && other->SameDev(m_stream)));
  }
  bool IsTTY() const { return (m_stream && m_stream->IsTTY()); }
  int Rows() const { return m_stream ? m_stream->Rows() : 0; }
  int Cols() const { return m_stream ? m_stream->Cols() : 0; }

  void Print(const char *message, ...);
  void Print_n(const char *message="", ...);
  void Output(const char *message, va_list ap){
    if( m_stream ) m_stream->Output(m_id, message, ap);
  }
  void Output_n(const char *message, va_list ap){
    if( m_stream ) m_stream->Output_n(m_id, message, ap);
  }
  void Update(const char *message, va_list ap){
    if( m_stream ) m_stream->Update(m_id, message, ap);
  }
  char *Input(char *field, size_t length){
    return m_stream ? m_stream->Input(field, length) : (char *)0;
  }
  bool Newline() const { return m_stream ? m_stream->Newline() : true; }
  int Lines() const { return m_stream ? m_stream->Lines() : 0; }
  void Clear() { if( m_stream ) m_stream->Clear(); }

  bool Eof() const { return m_stream ? m_stream->Eof() : true; }

  bool ConfigChannel(const char *param);
  ConStream *SetupStream(ConStream *dest, const char *name);
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
  int m_status_len;
  char m_buffer[80], m_debug_buffer[80];
  time_t m_active;

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
  ConChannel m_channels[DT_NCHANNELS];

  void CharMode(){ m_channels[DT_CHAN_INPUT].SetInputMode(DT_CONMODE_CHARS); }
  void LineMode();
  bool WaitingInput(ConChannel *channel);
  int ReopenNull(int nullfd, ConStream *stream, int sfd);
  int OperatorMenu(const char *param=(char *)0);
  void ShowFiles();
  void StatusLine0(char buffer[], size_t length);
  void StatusLine1(char buffer[], size_t length);
  char *Input(char *buffer, size_t size);

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
  void Print_n(const char *message="", ...);
  void Update(const char *message, ...);
  void Warning(int sev, const char *message, ...);
  void Error(int sev, const char *message, va_list ap);
  void Debug(const char *message, ...);
  void Debug_n(const char *message="", ...);
  void VDebug_n(const char *message, va_list ap);
  void Dump(const char *data, size_t length, const char *message=(char *)0,
    ...);
  void Interact(const char *message, ...);
  void Interact_n(const char *message="", ...);
  void InteractU(const char *message, ...);
  char *Input(const char *prompt, char *field, size_t length);

  void NoInput(){ m_channels[DT_CHAN_INPUT].Associate(&m_off, false); }
  int ChangeChannel(dt_conchan_t channel, const char *name, bool notify=true);
  const char *StatusLine(int format=-1);

  void cpu();

  RETSIGTYPE Signal(int sig_no);
  void Daemonize();

  int Configure(const char *param=(char *)0);
};


extern Console CONSOLE;

#endif  // CONSOLE_H

