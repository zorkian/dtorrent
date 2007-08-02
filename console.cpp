#include <stdlib.h>     // atoi()
#include <sys/types.h>  // fstat(), FD_SET()
#include <sys/stat.h>   // fstat()
#include <unistd.h>     // isatty()
#include <string.h>
#include <errno.h>
#include <ctype.h>      // isdigit()
#include <signal.h>

#include "btconfig.h"
#include "console.h"
#include "ctcs.h"
#include "btcontent.h"
#include "tracker.h"
#include "peer.h"
#include "peerlist.h"
#include "bttime.h"
#include "sigint.h"

// console.cpp:  Copyright 2007 Dennis Holmes  (dholmes@rahul.net)

// input mode definitions
#define K_CHARS 0
#define K_LINES 1

const char LIVE_CHAR[4] = {'-', '\\','|','/'};

Console CONSOLE;


//===========================================================================
// ConStream class functions


ConStream::ConStream()
{
  m_stream = (FILE *)0;
  m_name = (char *)0;
  m_newline = 1;
  m_suspend = 0;
  m_inputmode = K_LINES;
}


ConStream::~ConStream()
{
  if( !m_suspend ) _newline();
  if( m_stream ) fclose(m_stream);
  if( m_name ) delete []m_name;
}


void ConStream::Associate(FILE *stream, const char *name)
{
  m_stream = stream;
  if( m_name = new char[strlen(name)+1] )
    strcpy(m_name, name);
  else CONSOLE.Warning(1, "Failed to allocate memory for output filename.");
}


int ConStream::SameDev(ConStream *master) const
{
  struct stat sbone, sbtwo;

  if( master == this ) return 1;
  if( !fstat(Fileno(), &sbone) && !fstat(master->Fileno(), &sbtwo) )
    return (sbone.st_dev==sbtwo.st_dev && sbone.st_ino==sbtwo.st_ino) ? 1 : 0;
  else return 0;
}


void ConStream::PreserveMode()
{
  if( !isatty(Fileno()) ) return;

#if defined(USE_TERMIOS)
  tcgetattr(Fileno(), &m_original);
#elif defined(USE_TERMIO)
  ioctl(Fileno(), TCGETA, &m_original);
#elif defined(USE_SGTTY)
  gtty(Fileno(), &m_original);
#endif
}


void ConStream::RestoreMode()
{
  if( !isatty(Fileno()) ) return;

#if defined(USE_TERMIOS)
  tcsetattr(Fileno(), TCSANOW, &m_original);
#elif defined(USE_TERMIO)
  ioctl(Fileno(), TCSETA, &m_original);
#elif defined(USE_SGTTY)
  stty(Fileno(), &m_original);
#endif
}


void ConStream::SetInputMode(int keymode)
{
  if( m_suspend || !isatty(Fileno()) ) return;

#if defined(USE_TERMIOS)
  struct termios termset;
  tcgetattr(Fileno(), &termset);
#elif defined(USE_TERMIO)
  struct termio termset;
  ioctl(Fileno(), TCGETA, &termset);
#elif defined(USE_SGTTY)
  struct sgttyb termset;
  gtty(Fileno(), &termset);
#endif

  switch(keymode) {
  case K_CHARS:     // read a char at a time, no echo
#if defined(USE_TERMIOS)
    termset.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(Fileno(), TCSANOW, &termset);
#elif defined(USE_TERMIO)
    termset.c_lflag &= ~(ICANON | ECHO);
    ioctl(Fileno(), TCSETA, &termset);
#elif defined(USE_SGTTY)
    termset.sg_flags |= CBREAK;
    termset.sg_flags &= ~ECHO;
    stty(Fileno(), &termset);
#endif
    break;

  case K_LINES:     // read a line at a time (allow terminal editing)
#if defined(USE_TERMIOS)
    termset.c_lflag |= (ICANON | ECHO);
    tcsetattr(Fileno(), TCSANOW, &termset);
#elif defined(USE_TERMIO)
    termset.c_lflag |= (ICANON | ECHO);
    ioctl(Fileno(), TCSETA, &termset);
#elif defined(USE_SGTTY)
    termset.sg_flags &= ~CBREAK;
    termset.sg_flags |= ECHO;
    stty(Fileno(), &termset);
#endif
    break;

  default:
    break;
  }
  m_inputmode = keymode;
}


int ConStream::Output(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  _newline();
  _convprintf(message, ap);
  _newline();
  fflush(m_stream);
  return (old_newline==m_newline) ? 0 : 1;
}


int ConStream::Output_n(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  if( !*message ) _newline();
  else _convprintf(message, ap);
  fflush(m_stream);
  return (old_newline==m_newline) ? 0 : 1;
}


int ConStream::Update(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  if( !m_newline)
    fprintf(m_stream, isatty(Fileno()) ? "\r" : "\n");
  _convprintf(message, ap);
  fflush(m_stream);
  return (old_newline==m_newline) ? 0 : 1;
}


char *ConStream::Input(char *field, size_t length)
{
  if( m_suspend ) return (char *)0;

  m_newline = 1;
  return fgets(field, length, m_stream);
}


int ConStream::CharIn()
{
  if( m_suspend ) return 0;

  return fgetc(m_stream);
}


inline void ConStream::_newline()
{
  if( !m_newline ){
    fprintf(m_stream, "\n");
    m_newline = 1;
  }
}


inline int ConStream::_convprintf(const char *format, va_list ap)
{
  int r = ( '\n' == format[strlen(format)-1] );
  m_newline = r;
  return vfprintf(m_stream, format, ap);
}



//===========================================================================
// Console class functions


Console::Console()
{
  m_skip_status = m_status_last = 0;
  m_live_idx = 0;

  m_status_format = 0;
  int i = 0;
  m_statusline[i++] = &Console::StatusLine0;
  m_statusline[i++] = &Console::StatusLine1;
  if( STATUSLINES > i ){
    fprintf(stderr, "Unassigned status line in Console() constructor!\n");
    exit(1);
  }else if ( STATUSLINES < i ){
    fprintf(stderr, "Value of STATUSLINES is too small!\n");
    exit(1);
  }

  m_stdout.Associate(stdout, "stdout");
  m_stderr.Associate(stderr, "stderr");
  m_stdin.Associate(stdin, "stdin");
  m_streams[O_NORMAL] = &m_stdout;
  m_streams[O_WARNING] = &m_stderr;
  m_streams[O_DEBUG] = &m_stderr;
  m_streams[O_INTERACT] = &m_stdout;
  m_streams[O_INPUT] = &m_stdin;

  m_streams[O_INPUT]->PreserveMode();
  m_streams[O_INPUT]->SetInputMode(K_CHARS);
  m_conmode = K_CHARS;
}


Console::~Console()
{
  m_streams[O_INPUT]->RestoreMode();
}


int Console::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
   Status(0);
   FD_SET(m_streams[O_INPUT]->Fileno(), rfdp);
   return m_streams[O_INPUT]->Fileno();
}


void Console::User(fd_set *rfdp, fd_set *wfdp, int *nready,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  static char pending = '\0';
  static int inc, count;
  char c, param[256], *s;

  if( FD_ISSET(m_streams[O_INPUT]->Fileno(), rfdp) ){
    FD_CLR(m_streams[O_INPUT]->Fileno(), rfdnextp);
    (*nready)--;
    if( K_LINES==m_streams[O_INPUT]->GetInputMode() ){  // command parameter
      SyncNewlines(O_INPUT);
      if( m_streams[O_INPUT]->Input(param, 256) ){
        if( s = strchr(param, '\n') ) *s = '\0';
        if( '0'==pending ){
          if( OperatorMenu(param) ) pending = '\0';
        }else{
          if( *param ) switch( pending ){
          case 'n':				// get1file
            if( isdigit(*param) ){
              arg_file_to_download = atoi(param);
              BTCONTENT.SetFilter();
            }
            break;
          case 'S':				// CTCS server
            if( !strchr(param, ':') )
              Interact("Invalid input");
            else{
              if( arg_ctcs ) delete []arg_ctcs;
              arg_ctcs = new char[strlen(param) + 1];
              strcpy(arg_ctcs, param);
              CTCS.Initial();
              CTCS.Reset(1);
            }
            break;
          case 'Q':				// quit
            if( 'y'==*param || 'Y'==*param ) Tracker.SetStoped();
            break;
          default:
            Interact("Input error!");
          }
        }
      }else{
        if(errno) Interact("Input error: %s", strerror(errno));
        m_streams[O_INPUT]->SetInputMode(K_CHARS);
        Status(1);
      }
      if( '0' != pending ){
          m_streams[O_INPUT]->SetInputMode(K_CHARS);
          Status(1);
      }

    }else{     // command character received

      m_skip_status = 1;
      c = m_streams[O_INPUT]->CharIn();
      if( c!='+' && c!='-' ) pending = c;
      switch( c ){
      case 'h':				// help
      case '?':				// help
        Interact("Available commands:");
        Interact(" %-9s%-30s %-9s%s", "[Esc/0]", "Operator menu",
          "m[+/-]", "Adjust min peers count");
        Interact(" %-9s%-30s %-9s%s", "d[+/-]", "Adjust download limit",
          "M[+/-]", "Adjust max peers count");
        Interact(" %-9s%-30s %-9s%s", "u[+/-]", "Adjust upload limit",
          "C[+/-]", "Adjust max cache size");
        Interact(" %-9s%-30s %-9s%s", "n", "Download specific file",
          "S", "Set/change CTCS server");
        Interact(" %-9s%-30s %-9s%s", "e[+/-]", "Adjust seed exit time",
          "v", "Toggle verbose mode");
        Interact(" %-9s%-30s %-9s%s", "E[+/-]", "Adjust seed exit ratio",
          "Q", "Quit");
        break;
      case 'd':				// download bw limit
      case 'u':				// upload bw limit
      case 'e':				// seed time
      case 'E':				// seed ratio
      case 'm':				// min peers
      case 'M':				// max peers
      case 'C':				// max cache size
        inc = 1; count = 0;
        Interact_n("");
        break;
      case 'n':{				// get1file
        m_streams[O_INPUT]->SetInputMode(K_LINES);
        ShowFiles();
        Interact("Enter 0 for all files (normal behavior).");
        Interact_n("Get file number (currently %d): ",
          (int)arg_file_to_download);
        }break;
      case 'S':				// CTCS server
        m_streams[O_INPUT]->SetInputMode(K_LINES);
        Interact_n("");
        if( arg_ctcs )
          Interact_n("CTCS server:port (currently %s): ", arg_ctcs);
        else Interact_n("CTCS server:port: ");
        break;
      case 'v':				// verbose
        if( arg_verbose && m_streams[O_INPUT] != m_streams[O_DEBUG] )
          Debug("Verbose output off");
        arg_verbose = !arg_verbose;
        Interact("Verbose output %s", arg_verbose ? "on" : "off");
        break;
      case 'Q':				// quit
        if( !Tracker.IsQuitting() ){
          m_streams[O_INPUT]->SetInputMode(K_LINES);
          Interact_n("");
          Interact_n("Quit:  Are you sure? ");
        }
        break;
      case '+':				// increase value
      case '-':				// decrease value
        if( ('+'==c && inc<0) || ('-'==c && inc>0) ) inc *= -1;
        switch( pending ){
          int value;
        case 'd': cfg_max_bandwidth_down +=
            ( (cfg_max_bandwidth_down * (abs(inc)/100.0) < 1) ? inc :
                (int)(cfg_max_bandwidth_down * (inc/100.0)) );
          if( cfg_max_bandwidth_down < 0 ) cfg_max_bandwidth_down = 0;
          break;
        case 'u': cfg_max_bandwidth_up +=
            ( (cfg_max_bandwidth_up * (abs(inc)/100.0) < 1) ? inc :
                (int)(cfg_max_bandwidth_up * (inc/100.0)) );
          if( cfg_max_bandwidth_up < 0 ) cfg_max_bandwidth_up = 0;
          break;
        case 'e': cfg_seed_hours += inc;
          if( cfg_seed_hours < 0 ) cfg_seed_hours = 0;
          break;
        case 'E': cfg_seed_ratio += inc / 10.0;
          if( cfg_seed_ratio < 0 ) cfg_seed_ratio = 0;
          break;
        case 'm': value = (int)cfg_min_peers; value += inc;
          cfg_min_peers = (value < 1) ? 1 : (size_t)value;
          if( cfg_min_peers > cfg_max_peers ) cfg_min_peers = cfg_max_peers;
          break;
        case 'M': value = (int)cfg_max_peers; value += inc;
          cfg_max_peers = (value < (int)cfg_min_peers) ?
                          cfg_min_peers : (size_t)value;
          if( cfg_max_peers > 1000 ) cfg_max_peers = 1000;
          break;
        case 'C': value = (int)cfg_cache_size; value += inc;
          cfg_cache_size = (value < 0) ? 0 : (size_t)value;
          BTCONTENT.CacheConfigure();
          break;
        default:
          Status(1);
          break;
        }
        if( 10==++count ) inc *= 2;
        else if( 5==count ) inc *= 5;
        if( arg_ctcs ){
          if( 'd'==pending || 'u'==pending ) CTCS.Send_bw();
          else CTCS.Send_Config();
        }
        break;
      case '0':				// operator menu
      case 0x1b:				// Escape key
        pending = '0';
        OperatorMenu("");
        break;
      default:
        Status(1);
        break;
      }

      switch( pending ){
      case 'd': InteractU("DL Limit: %d B/s ", (int)cfg_max_bandwidth_down);
        break;
      case 'u': InteractU("UL Limit: %d B/s ", (int)cfg_max_bandwidth_up);
        break;
      case 'e': InteractU("Seed time: %.1f hours ", BTCONTENT.GetSeedTime() ?
          cfg_seed_hours - (now - BTCONTENT.GetSeedTime())/(double)3600 :
          (double)cfg_seed_hours);
        break;
      case 'E': InteractU("Seed ratio: %.2f ", (double)cfg_seed_ratio);
        break;
      case 'm': InteractU("Minimum peers: %d ", (int)cfg_min_peers);
        break;
      case 'M': InteractU("Maximum peers: %d ", (int)cfg_max_peers);
        break;
      case 'C': InteractU("Maximum cache: %d MB ", (int)cfg_cache_size);
        break;
      default:
        break;
      }
    }
  }
}


// Return non-zero to exit operator menu mode.
int Console::OperatorMenu(const char *param)
{
  static int oper_mode = 0;
  static int channel, n_opt;

  if( 0==oper_mode ){
    Interact("Operator Menu");
    n_opt = 0;
    Interact(" Output Channels:");
    Interact(" %2d) Normal/status:  %s", ++n_opt,
                                         m_streams[O_NORMAL]->GetName());
    Interact(" %2d) Interactive:    %s", ++n_opt,
                                         m_streams[O_INTERACT]->GetName());
    Interact(" %2d) Error/warning:  %s", ++n_opt,
                                         m_streams[O_WARNING]->GetName());
    Interact(" %2d) Debug/verbose:  %s", ++n_opt,
                                         m_streams[O_DEBUG]->GetName());
    char buffer[80];
    Interact(" Status Line Formats:");
    for( int i=0; i < STATUSLINES; i++ ){
      (CONSOLE.*m_statusline[i])(buffer, sizeof(buffer));
      Interact(" %c%d) %s", (i==m_status_format) ? '*' : ' ', ++n_opt, buffer);
    }
    Interact(" Other options:");
    Interact(" %2d) View detailed status", ++n_opt);
    if( WORLD.IsPaused() )
      Interact(" %2d) Resume (continue upload/download)", ++n_opt);
    else Interact(" %2d) Pause (suspend upload/download)", ++n_opt);
    Interact_n("Enter selection: ");
    m_streams[O_INPUT]->SetInputMode(K_LINES);
    oper_mode = 1;
    return 0;
  }
  else if( 1==oper_mode ){
    if( !*param ){ oper_mode = 0; Interact("Exiting menu"); return 1; }
    int sel = atoi(param);
    if( sel < 1 || sel > n_opt ){ 
      Interact_n("Enter selection: ");
      return 0;
    }
    if( sel <= O_NCHANNELS ){  // change output channel
      channel = sel - 1;
      Interact("Possible values are:");
      Interact(" %s", m_stdout.GetName());
      Interact(" %s", m_stderr.GetName());
      Interact(" a filename");
      Interact_n("Enter a destination: ");
      m_streams[O_INPUT]->SetInputMode(K_LINES);
      oper_mode = 2;
      return 0;
    }else if( sel <= O_NCHANNELS + STATUSLINES ){
      m_status_format = sel - (O_NCHANNELS+1);
      oper_mode = 0;
      return OperatorMenu("");
    }else if( sel == 1 + O_NCHANNELS + STATUSLINES ){  // detailed status
      Interact("");
      Interact("Torrent: %s", arg_metainfo_file);
      ShowFiles();
      Interact("");
      Interact("Download rate: %dB/s   Limit: %dB/s   Total: %llu",
        (int)(Self.RateDL()), (int)cfg_max_bandwidth_down,
        (unsigned long long)(Self.TotalDL()));
      Interact("  Upload rate: %dB/s   Limit: %dB/s   Total: %llu",
        (int)(Self.RateUL()), (int)cfg_max_bandwidth_up,
        (unsigned long long)(Self.TotalUL()));
      Interact("Peers: %d   Min: %d   Max: %d",
        (int)(WORLD.GetPeersCount()), (int)cfg_min_peers, (int)cfg_max_peers);
      Interact("Listening on: %s", WORLD.GetListen());
      Interact("");
      Interact("Ratio: %.2f   Seed time: %luh   Seed ratio: %.2f",
        (double)(Self.TotalUL()) / ( Self.TotalDL() ? Self.TotalDL() :
                                     BTCONTENT.GetTotalFilesLength() ),
        (unsigned long)cfg_seed_hours, cfg_seed_ratio);
      Interact("Cache in use: %dKB  Wants: %dKB  Max: %dMB",
        (int)(BTCONTENT.CacheUsed()/1024), (int)(BTCONTENT.CacheSize()/1024),
        (int)cfg_cache_size);
      if(arg_ctcs) Interact("CTCS Server: %s", arg_ctcs);
      oper_mode = 0;
      return 1;
    }else if( sel == 2 + O_NCHANNELS + STATUSLINES ){  // pause/resume
      if( WORLD.IsPaused() ) WORLD.Resume();
      else WORLD.Pause();
      oper_mode = 0;
      return 1;
    }
  }
  else if( 2==oper_mode ){
    if( !*param ){
      oper_mode = 0;
      return OperatorMenu("");
    }
    ChangeChannel(channel, param);
    oper_mode = 0;
    return OperatorMenu("");
  }

  Interact("Exiting menu");
  return 1;
}


void Console::ChangeChannel(int channel, const char *param)
{
  ConStream *dest = (ConStream *)0;

  if( !strcmp(param, m_stdout.GetName()) ) dest = &m_stdout;
  else if( !strcmp(param, m_stderr.GetName()) ) dest = &m_stderr;
  else{
    for( int i=0; i <= O_NCHANNELS; i++ ){
      if( channel != i && !strcmp(param, m_streams[i]->GetName()) ){
        dest = m_streams[i];
        break;
      }
    }
    if( !dest ){
      FILE *stream;
      if( dest = new ConStream ){
        if( !strcmp(param, m_streams[channel]->GetName()) )
          delete m_streams[channel];
        if( stream = fopen(param, "a") ) dest->Associate(stream, param);
        else{
          Interact("Error opening file: %s", strerror(errno));
          delete dest;
          dest = (ConStream *)0;
        }
      }else Interact("Failed to allocate memory.");
    }
  }
  if( dest ){
    if( m_streams[channel] != &m_stdout && m_streams[channel] != &m_stderr ){
      int in_use = 0;
      for( int i=0; i <= O_NCHANNELS; i++ ){
        if( channel != i && m_streams[channel] == m_streams[i] ) in_use = 1;
      }
      if( !in_use) delete m_streams[channel];
    }
    m_streams[channel] = dest;
  }
}


void Console::ShowFiles()
{
  BTFILE *file = 0;
  BitField tmpFilter;
  int n = 0;

  Interact("Files in this torrent:");
  while( file = BTCONTENT.GetNextFile(file) ){
    n++;
    BTCONTENT.SetTmpFilter(n, &tmpFilter);
    BitField tmpBitField = *BTCONTENT.pBF;
    tmpBitField.Except(tmpFilter);
    Interact("%d) %s [%llu] %d%%", n, file->bf_filename,
      (unsigned long long)(file->bf_length),
      (int)(100 * tmpBitField.Count() / BTCONTENT.getFilePieces(n)));
  }
}


void Console::Status(int immediate)
{
  static char buffer[80];

  if( immediate ) m_skip_status = 0;
  if( m_pre_dlrate.TimeUsed() || immediate ){
    if( m_skip_status ) m_skip_status = 0;
    else{
      (CONSOLE.*m_statusline[m_status_format])(buffer, sizeof(buffer));

      if( !m_status_last ) Print_n("");
      Update("%*s", -(int)sizeof(buffer)+1, buffer);
      m_status_last = 1;

      if(arg_verbose) Debug("Cache: %dK/%dM  Hits: %d  Miss: %d  %d%%",
        (int)(BTCONTENT.CacheUsed()/1024), (int)cfg_cache_size,
        (int)(BTCONTENT.CacheHits()), (int)(BTCONTENT.CacheMiss()),
        BTCONTENT.CacheHits() ? (int)(100 * BTCONTENT.CacheHits() /
          (BTCONTENT.CacheHits()+BTCONTENT.CacheMiss())) : 0);
    }

    m_pre_dlrate = Self.GetDLRate();
    m_pre_ulrate = Self.GetULRate();
  }
}


void Console::StatusLine0(char buffer[], size_t length)
{
  char partial[30] = "";
  if(arg_file_to_download){
    BitField tmpBitField =  *BTCONTENT.pBF;
    tmpBitField.Except(*BTCONTENT.pBFilter);
    sprintf( partial, "P:%d/%d ",
      (int)(tmpBitField.Count()),
      (int)(BTCONTENT.getFilePieces(arg_file_to_download)) );
  }

  char checked[14] = "";
  if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
    sprintf( checked, "Checking: %d%%",
      100 * BTCONTENT.CheckedPieces() / BTCONTENT.GetNPieces() );
  }

  snprintf(buffer, length,
    "%c %d/%d/%d [%d/%d/%d] %lluMB,%lluMB | %d,%dK/s | %d,%dK E:%d,%d %s%s",
    LIVE_CHAR[m_live_idx++],

    (int)(WORLD.GetSeedsCount()),
    (int)(WORLD.GetPeersCount()) - WORLD.GetSeedsCount(),
    (int)(Tracker.GetPeersCount()),

    (int)(BTCONTENT.pBF->Count()),
    (int)(BTCONTENT.GetNPieces()),
    (int)(WORLD.Pieces_I_Can_Get()),

    (unsigned long long)(Self.TotalDL()) >> 20,
    (unsigned long long)(Self.TotalUL()) >> 20,

    (int)(Self.RateDL() >> 10), (int)(Self.RateUL() >> 10),

    (int)(m_pre_dlrate.RateMeasure(Self.GetDLRate()) >> 10),
    (int)(m_pre_ulrate.RateMeasure(Self.GetULRate()) >> 10),

    (int)(Tracker.GetRefuseClick()),
    (int)(Tracker.GetOkClick()),

    partial,

    (Tracker.GetStatus()==T_CONNECTING) ? "Connecting" :
      ( (Tracker.GetStatus()==T_READY) ? "Connected" :
          (Tracker.IsQuitting() ? "Quitting" :
           (WORLD.IsPaused() ? "Paused" : checked)) )
  );
}


void Console::StatusLine1(char buffer[], size_t length)
{
  char partial[30] = "";
  if(arg_file_to_download){
    BitField tmpBitField =  *BTCONTENT.pBF;
    tmpBitField.Except(*BTCONTENT.pBFilter);
    sprintf( partial, "P:%d/%d ",
      (int)(tmpBitField.Count()),
      (int)(BTCONTENT.getFilePieces(arg_file_to_download)) );
  }

  char checked[14] = "";
  if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
    sprintf( checked, "Checking: %d%%",
      100 * BTCONTENT.CheckedPieces() / BTCONTENT.GetNPieces() );
  }

  char complete[8];
  if( BTCONTENT.pBF->IsFull() )
    sprintf(complete, "seeding");
  else sprintf(complete, "%d/%d%%",
    100 * BTCONTENT.pBF->Count() / BTCONTENT.GetNPieces(),
    100 * WORLD.Pieces_I_Can_Get() / BTCONTENT.GetNPieces());

  long remain = -1;
  char timeleft[20];
  size_t rate;
  if( !BTCONTENT.pBF->IsFull() ){  // downloading
    if( rate = Self.RateDL() ){
      // don't overflow remain
      if( BTCONTENT.GetLeftBytes() < (uint64_t)rate << 22 )
        remain = BTCONTENT.GetLeftBytes() / rate / 60;
      else remain = 99999;
    }
  }else{  //seeding
    if( cfg_seed_hours )
      remain = cfg_seed_hours * 60 - (now - BTCONTENT.GetSeedTime()) / 60;
    else if( rate = Self.RateUL() ){
      // don't overflow remain
      if( cfg_seed_ratio *
          (Self.TotalDL() ? Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) -
          Self.TotalUL() < (uint64_t)rate << 22 )
        remain = (long)( cfg_seed_ratio *
          (Self.TotalDL() ? Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) -
          Self.TotalUL() ) / rate / 60;
      else remain = 99999;
    }
  }
  if( remain >= 0 ){
    if( remain < 60000 )  // 1000 hours
      snprintf(timeleft, sizeof(timeleft), "%d:%2.2d",
        (int)(remain / 60), (int)(remain % 60));
    else strcpy(timeleft, ">999hr");
  }else strcpy(timeleft, "stalled");

  snprintf(buffer, length,
    "%c S:%d/%d L:%d/%d C:%d  R=%.2f D=%d U=%d K/s  %s %s  %s%s",
    LIVE_CHAR[m_live_idx++],

    (int)(WORLD.GetSeedsCount()),
    (int)(Tracker.GetSeedsCount()) -
      ((BTCONTENT.pBF->IsFull() && Tracker.GetSeedsCount() > 0) ? 1 : 0),

    (int)(WORLD.GetPeersCount()) - WORLD.GetSeedsCount() - WORLD.GetConnCount(),
    (int)(Tracker.GetPeersCount()) - Tracker.GetSeedsCount() -
      ((!BTCONTENT.pBF->IsFull() && Tracker.GetPeersCount() > 0) ? 1 : 0),

    (int)(WORLD.GetConnCount()),


    (double)(Self.TotalUL()) / ( Self.TotalDL() ? Self.TotalDL() :
                                 BTCONTENT.GetTotalFilesLength() ),

    (int)(Self.RateDL() >> 10), (int)(Self.RateUL() >> 10),

    complete, timeleft,

    partial,

    (Tracker.GetStatus()==T_CONNECTING) ? "Connecting" :
      ( (Tracker.GetStatus()==T_READY) ? "Connected" :
          (Tracker.IsQuitting() ? "Quitting" :
           (WORLD.IsPaused() ? "Paused" : checked)) )
  );
}


void Console::Print(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( K_LINES != m_streams[O_INPUT]->GetInputMode() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    if( m_streams[O_NORMAL]->Output(message, ap) )
      SyncNewlines(O_NORMAL);
  }
  if( arg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    if( m_streams[O_DEBUG]->Output(message, ap) )
      SyncNewlines(O_DEBUG);
  }

  va_end(ap);
}


/* Print message without a terminating newline
   With a null message, start/insure a new line
*/
void Console::Print_n(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( m_status_last && *message ) Print_n("");
  m_status_last = 0;

  if( K_LINES != m_streams[O_INPUT]->GetInputMode() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    if( m_streams[O_NORMAL]->Output_n(message, ap) )
      SyncNewlines(O_NORMAL);
  }
  if( arg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    if( m_streams[O_DEBUG]->Output_n(message, ap) )
      SyncNewlines(O_DEBUG);
  }

  va_end(ap);
}


/* Update (replace) the current line (no terminating newline)
*/
void Console::Update(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  m_status_last = 0;

  if( K_LINES != m_streams[O_INPUT]->GetInputMode() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    if( m_streams[O_NORMAL]->Update(message, ap) )
      SyncNewlines(O_NORMAL);
  }
  if( arg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    if( m_streams[O_DEBUG]->Update(message, ap) )
      SyncNewlines(O_DEBUG);
  }

  va_end(ap);
}


/* "sev" indicates the severity of the message.
   0: will be printed but not sent to CTCS
   1: extremely urgent/important
   2: less important
   3: no problem
*/
void Console::Warning(int sev, const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( m_streams[O_WARNING]->Output(message, ap) )
    SyncNewlines(O_WARNING);
  if( arg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_WARNING]) ){
    if( m_streams[O_DEBUG]->Output(message, ap) )
      SyncNewlines(O_DEBUG);
  }

  if(sev && arg_ctcs){
    char cmsg[CTCS_BUFSIZE];
    vsnprintf(cmsg, CTCS_BUFSIZE, message, ap);
    CTCS.Send_Info(sev, cmsg);
  }

  va_end(ap);
}


void Console::Debug(const char *message, ...)
{
  static char buffer[80];
  if( !arg_verbose ) return;

  char *format = (char *)0;
  size_t buflen;
  va_list ap;
  va_start(ap, message);

  if( K_LINES != m_streams[O_INPUT]->GetInputMode() ||
      (!m_streams[O_DEBUG]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_DEBUG]->SameDev(m_streams[O_INPUT])) ){
    size_t need = strlen(message)+1 + 10*sizeof(unsigned long)/4;
    if( need > sizeof(buffer) && (format = new char[need]) ) buflen = need;
    else{ format = buffer; buflen = sizeof(buffer); }

    snprintf(format, buflen, "%lu %s", (unsigned long)now, message);

    if( m_streams[O_DEBUG]->Output(format, ap) )
      SyncNewlines(O_DEBUG);

    if( format && format != buffer ) delete []format;
  }
  va_end(ap);
}


/* Print debug message without a terminating newline
   With a null message, start/insure a new line
*/
void Console::Debug_n(const char *message, ...)
{
  static char buffer[80];
  static int f_new_line = 1;
  if( !arg_verbose ) return;

  va_list ap;
  va_start(ap, message);

  if( K_LINES != m_streams[O_INPUT]->GetInputMode() ||
      (!m_streams[O_DEBUG]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_DEBUG]->SameDev(m_streams[O_INPUT])) ){
    if( m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
      if( m_status_last && *message ) Debug_n("");
      m_status_last = 0;
    }
    if( f_new_line && *message ){
      char *format = (char *)0;
      size_t buflen;
      size_t need = strlen(message)+1 + 10*sizeof(unsigned long)/4;
      if( need > sizeof(buffer) && (format = new char[need]) ) buflen = need;
      else{ format = buffer; buflen = sizeof(buffer); }

      snprintf(format, buflen, "%lu %s", (unsigned long)now, message);

      if( m_streams[O_DEBUG]->Output_n(format, ap) )
        SyncNewlines(O_DEBUG);
      if( format && format != buffer ) delete []format;
    }
    else if( m_streams[O_DEBUG]->Output_n(message, ap) )
      SyncNewlines(O_DEBUG);

    if( *message ) f_new_line = 0;
    else f_new_line = 1;
  }
  va_end(ap);
}


void Console::Interact(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( m_streams[O_INTERACT]->Output(message, ap) )
    SyncNewlines(O_INTERACT);

  va_end(ap);
}


/* Print interactive message without a terminating newline
   With a null message, start/insure a new line
*/
void Console::Interact_n(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( m_streams[O_INTERACT]->SameDev(m_streams[O_NORMAL]) ){
    if( m_status_last && *message ) Interact_n("");
    m_status_last = 0;
  }
  if( m_streams[O_INTERACT]->Output_n(message, ap) )
    SyncNewlines(O_INTERACT);

  va_end(ap);
}


/* Update (replace) the current interactive line (no terminating newline)
*/
void Console::InteractU(const char *message, ...)
{
  va_list ap;
  va_start(ap, message);

  if( m_streams[O_INTERACT]->SameDev(m_streams[O_NORMAL]) ){
    if( m_status_last ) Interact_n("");
    m_status_last = 0;
  }
  if( m_streams[O_INTERACT]->Update(message, ap) )
    SyncNewlines(O_INTERACT);

  va_end(ap);
}


// Avoid using this during normal operation, as it blocks for input!
char *Console::Input(const char *prompt, char *field, size_t length)
{
  char *retval;
  m_streams[O_INPUT]->SetInputMode(K_LINES);
  Interact_n(0, "");
  Interact_n(0, "%s", prompt);
  retval = m_streams[O_INPUT]->Input(field, length);
  m_streams[O_INPUT]->SetInputMode(K_CHARS);
  return retval;
}


void Console::SyncNewlines(int master)
{
  for( int i=0; i < O_NCHANNELS; i++ ){
    if( i != master && m_streams[i]->SameDev(m_streams[master]) )
      m_streams[i]->SyncNewline(m_streams[master]);
  }
}


RETSIGTYPE Console::Signal(int sig_no)
{
  switch( sig_no ){
  case SIGTTOU:
    for( int i=0; i < O_NCHANNELS; i++ )
      if( isatty(m_streams[i]->Fileno()) ) m_streams[i]->Suspend();
    m_conmode = m_streams[O_INPUT]->GetInputMode();
    break;
  case SIGTTIN:
    if( isatty(m_streams[O_INPUT]->Fileno()) ) m_streams[O_INPUT]->Suspend();
    m_conmode = m_streams[O_INPUT]->GetInputMode();
    break;
  case SIGCONT:
    for( int i=0; i <= O_NCHANNELS; i++ )
      if( isatty(m_streams[i]->Fileno()) ) m_streams[i]->Resume();
    m_streams[O_INPUT]->SetInputMode(m_conmode);
    // restore my handler
    signal(SIGTSTP, signals);
    break;
  case SIGTSTP:
    m_conmode = m_streams[O_INPUT]->GetInputMode();
    m_streams[O_INPUT]->RestoreMode();
    // let the system default action proceed
    signal(sig_no, SIG_DFL);
    raise(sig_no);
    break;
  default:
    break;
  }
}

