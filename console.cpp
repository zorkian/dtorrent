#include "console.h"  // def.h

#include <stdlib.h>     // atoi()
#include <sys/types.h>  // fstat(), FD_SET(), fork()
#include <sys/stat.h>   // fstat()
#include <unistd.h>     // isatty(), fork(), setsid()
#include <string.h>
#include <errno.h>
#include <ctype.h>      // isdigit()
#include <signal.h>
#include <fcntl.h>      // open()
#include <time.h>       // clock()

#if defined(HAVE_IOCTL_H)
#include <ioctl.h>      // ioctl()
#elif defined(HAVE_SYS_IOCTL_H)
#include <sys/ioctl.h>
#endif

#include "btconfig.h"
#include "ctcs.h"
#include "btcontent.h"
#include "tracker.h"
#include "peer.h"
#include "peerlist.h"
#include "bitfield.h"
#include "bttime.h"
#include "sigint.h"

#if !defined(HAVE_VSNPRINTF) || !defined(HAVE_SNPRINTF) || \
    !defined(HAVE_STRCASECMP)
#include "compat.h"
#endif

// console.cpp:  Copyright 2007-2008 Dennis Holmes  (dholmes@rahul.net)

const char LIVE_CHAR[4] = { '-', '\\', '|', '/' };

Console CONSOLE;
static bool g_console_ready = false;
static bool g_daemon_parent = false;


//===========================================================================
// ConStream class functions


ConStream::ConStream()
{
  m_stream = (FILE *)0;
  m_name = (char *)0;
  m_restore = 0;
  m_newline = 1;
  m_suspend = 0;
  m_inputmode = DT_CONMODE_LINES;
}


ConStream::~ConStream()
{
  if( !g_secondary_process || g_daemon_parent ){
    if( m_restore ) RestoreMode();
    if( !m_suspend ) _newline();
    if( m_stream ) fclose(m_stream);
  }
  if( m_name ) delete []m_name;
}


void ConStream::Close()
{
  if( m_stream ){
    fclose(m_stream);
    m_stream = (FILE *)0;
  }
  m_suspend = 1;
}


void ConStream::Associate(FILE *stream, const char *name, int mode)
{
  m_stream = stream;
  m_filemode = mode;
  if( (m_name = new char[strlen(name)+1]) )
    strcpy(m_name, name);
  else Error(1, "Failed to allocate memory for output filename.");
}


int ConStream::SameDev(const ConStream *master) const
{
  struct stat sbone, sbtwo;

  if( master == this || Fileno() == master->Fileno() ) return 1;
  else if( Fileno() < 0 || master->Fileno() < 0 ) return 0;

  if( !fstat(Fileno(), &sbone) && !fstat(master->Fileno(), &sbtwo) )
    return ( sbone.st_dev==sbtwo.st_dev && sbone.st_ino==sbtwo.st_ino ) ? 1 : 0;
  else return 0;
}


inline int ConStream::IsTTY() const
{
  return ( Fileno() >= 0 ) ? isatty(Fileno()) : 0;
}


int ConStream::GetSize(bool getrows) const
{
  int rows = 0, cols = 0;

  if( IsTTY() ){
#ifdef TIOCGWINSZ
    struct winsize tsize;
    if( ioctl(Fileno(), TIOCGWINSZ, &tsize) >= 0 ){
      rows = tsize.ws_row;
      cols = tsize.ws_col;
    }
#else
#ifdef TIOCGSIZE
    struct ttysize tsize;
    if( ioctl(Fileno(), TIOCGSIZE, &tsize) >= 0 ){
      rows = tsize.ts_lines;
      cols = tsize.ts_cols;
    }
#else
#ifdef WIOCGETD
    struct uwdata tsize;
    if( ioctl(Fileno(), WIOCGETD, &tsize) >= 0 ){
      rows = tsize.uw_height / tsize.uw_vs;
      cols = tsize.uw_width / tsize.uw_hs;
    }
#endif
#endif
#endif
  }
  return getrows ? rows : cols;
}


void ConStream::PreserveMode()
{
  int r;

  if( !IsTTY() ) return;

#if defined(USE_TERMIOS)
  r = tcgetattr(Fileno(), &m_original);
#elif defined(USE_TERMIO)
  r = ioctl(Fileno(), TCGETA, &m_original);
#elif defined(USE_SGTTY)
  r = gtty(Fileno(), &m_original);
#endif
  if( r < 0 ){
    Error(1, "Error preserving terminal mode on fd %d:  %s", Fileno(),
      strerror(errno));
  }else m_restore = 1;
}


void ConStream::RestoreMode()
{
  int r;

  if( !IsTTY() ) return;

#if defined(USE_TERMIOS)
  r = tcsetattr(Fileno(), TCSANOW, &m_original);
#elif defined(USE_TERMIO)
  r = ioctl(Fileno(), TCSETA, &m_original);
#elif defined(USE_SGTTY)
  r = stty(Fileno(), &m_original);
#endif
  if( r < 0 ){
    Error(1, "Error restoring terminal mode on fd %d:  %s", Fileno(),
      strerror(errno));
  }
}


void ConStream::SetInputMode(dt_conmode_t keymode)
{
  if( m_suspend ) return;

  m_inputmode = keymode;
  if( !IsTTY() ) return;

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

  switch( keymode ){
  case DT_CONMODE_CHARS:  // read a char at a time, no echo
#if defined(USE_TERMIOS)
    termset.c_lflag &= ~(ICANON | ECHO);
    termset.c_cc[VMIN] = 1;
    termset.c_cc[VTIME] = 0;
    tcsetattr(Fileno(), TCSANOW, &termset);
#elif defined(USE_TERMIO)
    termset.c_lflag &= ~(ICANON | ECHO);
    termset.c_cc[VMIN] = 1;
    termset.c_cc[VTIME] = 0;
    ioctl(Fileno(), TCSETA, &termset);
#elif defined(USE_SGTTY)
    termset.sg_flags |= CBREAK;
    termset.sg_flags &= ~ECHO;
    stty(Fileno(), &termset);
#endif
    break;

  case DT_CONMODE_LINES:  // read a line at a time (allow terminal editing)
#if defined(USE_TERMIOS)
    termset.c_lflag |= (ICANON | ECHO);
    termset.c_cc[VMIN] = 1;
    termset.c_cc[VTIME] = 0;
    tcsetattr(Fileno(), TCSANOW, &termset);
#elif defined(USE_TERMIO)
    termset.c_lflag |= (ICANON | ECHO);
    termset.c_cc[VMIN] = 1;
    termset.c_cc[VTIME] = 0;
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
}


int ConStream::Output(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  _newline();
  _convprintf(message, ap);
  _newline();
  fflush(m_stream);
  return ( old_newline==m_newline ) ? 0 : 1;
}


int ConStream::Output_n(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  if( !message || !*message ) _newline();
  else _convprintf(message, ap);
  fflush(m_stream);
  return ( old_newline==m_newline ) ? 0 : 1;
}


int ConStream::Update(const char *message, va_list ap)
{
  if( m_suspend ) return 0;

  int old_newline = m_newline;
  if( !m_newline ) fprintf(m_stream, IsTTY() ? "\r" : "\n");
  _convprintf(message, ap);
  fflush(m_stream);
  return ( old_newline==m_newline ) ? 0 : 1;
}


char *ConStream::Input(char *field, size_t length)
{
  char *result, *tmp;

  if( m_suspend ) return (char *)0;

  m_newline = 1;
  if( (result = fgets(field, length, m_stream)) &&
      (tmp = strpbrk(field, "\r\n")) ){
    *tmp = '\0';
  }
  return result;
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


/* ConStream functions need to call Error instead of CONSOLE.Warning, because
   CONSOLE may not be initialized yet (or may have been destroyed already).
*/
void ConStream::Error(int sev, const char *message, ...)
{
  va_list ap;

  va_start(ap, message);
  /* Note the call to Warning sends only the literal message--a limitation to
     deal with later. */
  if( g_console_ready ) CONSOLE.Warning(sev, message);
  else{
    vfprintf(stderr, message, ap);
    fflush(stderr);
  }
  va_end(ap);
}



//===========================================================================
// Console class functions


Console::Console()
{
  m_skip_status = m_status_last = 0;
  m_live_idx = 0;
  m_oldfd = -1;

  int i = 0;
  m_statusline[i++] = &Console::StatusLine0;
  m_statusline[i++] = &Console::StatusLine1;
  if( STATUSLINES > i ){
    fprintf(stderr, "Unassigned status line in Console() constructor!\n");
    exit(1);
  }else if( STATUSLINES < i ){
    fprintf(stderr, "Value of STATUSLINES is too small!\n");
    exit(1);
  }

  m_status_len = 80;

  opermenu.mode = configmenu.mode = 0;

  m_stdout.Associate(stdout, "stdout", 1);
  m_stderr.Associate(stderr, "stderr", 1);
  m_stdin.Associate(stdin, "stdin", 0);
  m_off.Associate(NULL, "off", 1);
  m_off.Suspend();
  m_streams[O_NORMAL] = &m_stdout;
  m_streams[O_WARNING] = &m_stderr;
  m_streams[O_DEBUG] = &m_stderr;
  m_streams[O_INTERACT] = &m_stdout;
  m_streams[O_INPUT] = &m_stdin;

  m_streams[O_INPUT]->PreserveMode();
  m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
  m_conmode = DT_CONMODE_CHARS;

  if( this == &CONSOLE ) g_console_ready = true;
}


Console::~Console()
{
  if( this == &CONSOLE ) g_console_ready = false;
}


void Console::Init()
{
  // Activate config defaults
  ChangeChannel(O_NORMAL, *cfg_channel_normal, 0);
  ChangeChannel(O_WARNING, *cfg_channel_error, 0);
  ChangeChannel(O_DEBUG, *cfg_channel_debug, 0);
  ChangeChannel(O_INTERACT, *cfg_channel_interact, 0);
  ChangeChannel(O_INPUT, *cfg_channel_input, 0);
}


int Console::IntervalCheck(fd_set *rfdp, fd_set *wfdp)
{
  Status(0);

  if( m_oldfd >= 0 ){
    FD_CLR(m_oldfd, rfdp);
    m_oldfd = -1;
  }

  if( !m_streams[O_INPUT]->IsSuspended() ){
    FD_SET(m_streams[O_INPUT]->Fileno(), rfdp);
    return m_streams[O_INPUT]->Fileno();
  }else{
    if( m_streams[O_INPUT]->Fileno() >= 0 )
      FD_CLR(m_streams[O_INPUT]->Fileno(), rfdp);
    return -1;
  }
}


void Console::User(fd_set *rfdp, fd_set *wfdp, int *nready,
  fd_set *rfdnextp, fd_set *wfdnextp)
{
  static char pending = '\0';
  static int inc, count;
  char c, param[MAXPATHLEN], *s;

  if( m_streams[O_INPUT]->Fileno() >= 0 &&
      FD_ISSET(m_streams[O_INPUT]->Fileno(), rfdp) ){
    FD_CLR(m_streams[O_INPUT]->Fileno(), rfdnextp);
    (*nready)--;
    if( DT_CONMODE_LINES == m_streams[O_INPUT]->GetInputMode() ){  // cmd param
      char *got = m_streams[O_INPUT]->Input(param, sizeof(param));
      SyncNewlines(O_INPUT);
      if( got ){
        if( (s = strchr(param, '\n')) ) *s = '\0';
        if( '0'==pending ){
          if( OperatorMenu(param) ) pending = '\0';
        }else{
          m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
          if( *param ) switch( pending ){
          case 'n':  // get1file
            cfg_file_to_download = param;
            break;
          case 'S':  // CTCS server
            if( !cfg_ctcs.Valid(param) )
              Interact("Invalid input");
            else cfg_ctcs = param;
            break;
          case 'X':  // completion command (user exit)
            cfg_completion_exit = param;
            break;
          case 'Q':  // quit
            if( 'y'==*param || 'Y'==*param ){
              Tracker.ClearRestart();
              Tracker.SetStoped();
            }
            break;
          default:
            Interact("Input mode error");
          }
        }
      }else{
        if( m_streams[O_INPUT]->Eof() ){
          Interact("End of input reached.");
          if( ChangeChannel(O_INPUT, "off") < 0 )
            m_streams[O_INPUT]->Suspend();
        }else if( errno ){
          if( ENODEV==errno || ENOTTY==errno ) m_streams[O_INPUT]->Suspend();
          else Interact("Input error:  %s", strerror(errno));
        }else Interact("Input error!");
      }
      if( '0' != pending ){
          m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
          Status(1);
      }

    }else{  // command character received

      m_skip_status = 1;
      if( (c = m_streams[O_INPUT]->CharIn()) == EOF ){
        if( m_streams[O_INPUT]->Eof() ){
          Interact("End of input reached.");
          if( ChangeChannel(O_INPUT, "off") < 0 )
            m_streams[O_INPUT]->Suspend();
        }else if( errno ){
          if( ENODEV==errno || ENOTTY==errno ) m_streams[O_INPUT]->Suspend();
          else Interact("Input error:  %s", strerror(errno));
        }else Interact("Input error!");
        return;
      }
      if( c!='+' && c!='-' ) pending = c;
      switch( c ){
      case 'h':  // help
      case '?':  // help
        Interact("Available commands:");
        Interact(" %-9s%-30s %-9s%s", "[Esc/0]", "Operator menu",
          "m[+/-]", "Adjust min peers count");
        Interact(" %-9s%-30s %-9s%s", "d[+/-]", "Adjust download limit",
          "M[+/-]", "Adjust max peers count");
        Interact(" %-9s%-30s %-9s%s", "u[+/-]", "Adjust upload limit",
          "C[+/-]", "Adjust max cache size");
        Interact(" %-9s%-30s %-9s%s", "n", "Download specific files",
          "S", "Set/change CTCS server");
        Interact(" %-9s%-30s %-9s%s", "e[+/-]", "Adjust seed exit time",
          "v", "Toggle verbose mode");
        Interact(" %-9s%-30s %-9s%s", "E[+/-]", "Adjust seed exit ratio",
          "Q", "Quit");
        Interact(" %-9s%-30s %-9s%s", "X", "Completion command",
          "", "");
        break;
      case 'd':  // download bw limit
      case 'u':  // upload bw limit
        if(*cfg_ctcs) Interact("Note, changes may be overridden by CTCS.");
      case 'e':  // seed time
      case 'E':  // seed ratio
      case 'm':  // min peers
      case 'M':  // max peers
      case 'C':  // max cache size
        inc = 1;
        count = 0;
        Interact_n("");
        break;
      case 'n':  // get1file
        if( BTCONTENT.IsFull() )
          Interact("Download is already complete.");
        else{
          m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
          ShowFiles();
          Interact("Enter 0 or * for all files (normal behavior).");
          if( *cfg_file_to_download )
            Interact_n("Get file number/list (currently %s): ",
              *cfg_file_to_download);
          else Interact_n("Get file number/list: ");
        }
        break;
      case 'S':  // CTCS server
        m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
        Interact_n("");
        if( *cfg_ctcs ){
          Interact("Enter ':' to stop using CTCS.");
          Interact_n("CTCS server:port (currently %s): ", *cfg_ctcs);
        }
        else Interact_n("CTCS server:port: ");
        break;
      case 'X':  // completion command (user exit)
        if( BTCONTENT.IsFull() )
          Interact("Download is already complete.");
        else{
          m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
          Interact("Enter a command to run upon download completion.");
          if( *cfg_completion_exit )
            Interact("Currently: %s", *cfg_completion_exit);
          Interact_n(">");
        }
        break;
      case 'v':  // verbose
        cfg_verbose = !*cfg_verbose;
        if( !m_streams[O_INTERACT]->SameDev(m_streams[O_DEBUG]) )
          Interact("Verbose output %s", *cfg_verbose ? "on" : "off");
        break;
      case 'Q':  // quit
        if( !Tracker.IsQuitting() ){
          m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
          Interact_n("");
          Interact_n("Quit:  Are you sure? ");
        }
        break;
      case '+':  // increase value
      case '-':  // decrease value
        if( ('+'==c && inc<0) || ('-'==c && inc>0) ) inc *= -1;
        switch( pending ){
        case 'd':
          if( inc < 0 && (unsigned int)abs(inc) > *cfg_max_bandwidth_down ){
            cfg_max_bandwidth_down = 0;
            break;
          }
          if( inc > 0 &&
              *cfg_max_bandwidth_down + inc < *cfg_max_bandwidth_down ){
            cfg_max_bandwidth_down = cfg_max_bandwidth_down.Max();
            break;
          }
          cfg_max_bandwidth_down +=
            (*cfg_max_bandwidth_down * (abs(inc)/100.0) < 1) ? inc :
              (*cfg_max_bandwidth_down * (inc/100.0));
          break;
        case 'u':
          if( inc < 0 && (unsigned int)abs(inc) > *cfg_max_bandwidth_up ){
            cfg_max_bandwidth_up = 0;
            break;
          }
          if( inc > 0 &&
              *cfg_max_bandwidth_up + inc < *cfg_max_bandwidth_up ){
            cfg_max_bandwidth_up = cfg_max_bandwidth_up.Max();
            break;
          }
          cfg_max_bandwidth_up +=
            (*cfg_max_bandwidth_up * (abs(inc)/100.0) < 1) ? inc :
              (*cfg_max_bandwidth_up * (inc/100.0));
          break;
        case 'e':
          if( cfg_seed_remain.Hidden() ) cfg_seed_hours += inc;
          else cfg_seed_remain += inc;
          break;
        case 'E':
          cfg_seed_ratio += inc / 10.0;
          break;
        case 'm':
          cfg_min_peers += inc;
          break;
        case 'M':
          cfg_max_peers += inc;
          break;
        case 'C':
          if( inc < 0 && (unsigned int)abs(inc) > *cfg_cache_size ){
            cfg_cache_size = 0;
            break;
          }
          if( inc > 0 && *cfg_cache_size + inc < *cfg_cache_size ){
            cfg_cache_size = cfg_cache_size.Max();
            break;
          }
          cfg_cache_size += inc;
          break;
        default:
          Status(1);
          break;
        }
        if( 10==++count ) inc *= 2;
        else if( 5==count ) inc *= 5;
        if( *cfg_ctcs && 'd' != pending && 'u' != pending ) CTCS.Send_Config();
        break;
      case '0':  // operator menu
      case 0x1b:  // Escape key
        pending = '0';
        OperatorMenu();
        break;
      default:
        Status(1);
        break;
      }

      switch( pending ){
      case 'd':
        InteractU("DL Limit: %u B/s ", (unsigned int)*cfg_max_bandwidth_down);
        break;
      case 'u':
        InteractU("UL Limit: %u B/s ", (unsigned int)*cfg_max_bandwidth_up);
        break;
      case 'e':
        InteractU("Seed time: %.1f hours%s ",
          cfg_seed_remain.Hidden() ? (double)*cfg_seed_hours : *cfg_seed_remain,
          cfg_seed_remain.Hidden() ? "" : " remaining");
        break;
      case 'E':
        InteractU("Seed ratio: %.2f ", *cfg_seed_ratio);
        break;
      case 'm':
        InteractU("Minimum peers: %d ", (int)*cfg_min_peers);
        break;
      case 'M':
        InteractU("Maximum peers: %d ", (int)*cfg_max_peers);
        break;
      case 'C':
        InteractU("Maximum cache: %d MB ", (int)*cfg_cache_size);
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
  if( 0==opermenu.mode ){
    Interact("Operator Menu");
    opermenu.n_opt = 0;
    Interact(" Console Channels:");
    Interact(" %2d) Normal/status:  %s", ++opermenu.n_opt,
                                         m_streams[O_NORMAL]->GetName());
    Interact(" %2d) Interactive:    %s", ++opermenu.n_opt,
                                         m_streams[O_INTERACT]->GetName());
    Interact(" %2d) Error/warning:  %s", ++opermenu.n_opt,
                                         m_streams[O_WARNING]->GetName());
    Interact(" %2d) Debug/verbose:  %s", ++opermenu.n_opt,
                                         m_streams[O_DEBUG]->GetName());
    Interact(" %2d) Input:          %s", ++opermenu.n_opt,
                                         m_streams[O_INPUT]->GetName());
    char buffer[80];
    Interact(" Status Line Formats:");
    for( int i=0; i < STATUSLINES; i++ ){
      (this->*m_statusline[i])(buffer, sizeof(buffer));
      Interact(" %c%d) %s",
        (i==*cfg_status_format) ? '*' : ' ', ++opermenu.n_opt, buffer);
    }
    Interact(" Other options:");
    Interact(" %2d) View detailed status", ++opermenu.n_opt);
    if( WORLD.IsPaused() )
      Interact(" %2d) Resume (continue upload/download)", ++opermenu.n_opt);
    else Interact(" %2d) Pause (suspend upload/download)", ++opermenu.n_opt);
    if( !*cfg_daemon )
      Interact(" %2d) Become daemon (fork to background)", ++opermenu.n_opt);
    Interact(" %2d) Update tracker stats & get peers", ++opermenu.n_opt);
    Interact(" %2d) Restart (recover) the tracker session", ++opermenu.n_opt);
    Interact(" %2d) Configuration", ++opermenu.n_opt);
    Interact_n("Enter selection: ");
    m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
    opermenu.mode = 1;
    return 0;
  }
  else if( 1==opermenu.mode ){
    if( !param || !*param ){
      opermenu.mode = 0;
      Interact("Exiting menu");
      return 1;
    }
    int sel = atoi(param);
    if( sel < 1 || sel > opermenu.n_opt ){
      Interact_n("Enter selection: ");
      return 0;
    }
    if( sel <= O_NCHANNELS+1 ){  // change i/o channel
      opermenu.channel = (dt_conchan_t)(sel - 1);
      Interact("Possible values are:");
      Interact(" %s", m_stdout.GetName());
      Interact(" %s", m_stderr.GetName());
      Interact(" %s", m_off.GetName());
      Interact(" a filename");
      Interact_n("Enter a destination: ");
      m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
      opermenu.mode = 2;
      return 0;
    }else if( sel <= O_NCHANNELS+1 + STATUSLINES ){
      cfg_status_format = sel - (O_NCHANNELS+1) - 1;
      opermenu.mode = 0;
      return OperatorMenu();
    }else if( sel == 1 + O_NCHANNELS+1 + STATUSLINES ){  // detailed status
      m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
      Interact("");
      Interact("Torrent: %s", BTCONTENT.GetMetainfoFile());
      ShowFiles();
      if( *cfg_file_to_download && !BTCONTENT.Seeding() )
        Interact("Downloading: %s", *cfg_file_to_download);
      Interact("");
      Interact("Download rate: %dB/s   Limit: %dB/s   Total: %llu",
        (int)Self.RateDL(), (int)*cfg_max_bandwidth_down,
        (unsigned long long)Self.TotalDL());
      Interact("  Upload rate: %dB/s   Limit: %dB/s   Total: %llu",
        (int)Self.RateUL(), (int)*cfg_max_bandwidth_up,
        (unsigned long long)Self.TotalUL());
      time_t t = Tracker.GetReportTime();
      if( t ){
        char s[42];
#ifdef HAVE_CTIME_R_3
        ctime_r(&t, s, sizeof(s));
#else
        ctime_r(&t, s);
#endif
        if( s[strlen(s)-1] == '\n' ) s[strlen(s)-1] = '\0';
        Interact("Reported to tracker: %llu up",
          (unsigned long long)Tracker.GetReportUL());
        Interact("                     %llu down at %s",
          (unsigned long long)Tracker.GetReportDL(), s);
      }
      Interact("Failed hashes: %d    Dup blocks: %d    Unwanted blocks: %d",
        (int)BTCONTENT.GetHashFailures(), (int)BTCONTENT.GetDupBlocks(),
        (int)BTCONTENT.GetUnwantedBlocks());
      Interact("");
      Interact("Peers: %d   Min: %d   Max: %d",
        (int)WORLD.GetPeersCount(), (int)*cfg_min_peers, (int)*cfg_max_peers);
      Interact("Listening on: %s", WORLD.GetListen());
      Interact("Announce URL: %s", BTCONTENT.GetAnnounce());
      Interact("");
      Interact("Ratio: %.2f   Seed time: %luh   Seed ratio: %.2f",
        (double)Self.TotalUL() / ( Self.TotalDL() ? Self.TotalDL() :
                                     BTCONTENT.GetTotalFilesLength() ),
        (unsigned long)*cfg_seed_hours, *cfg_seed_ratio);
      Interact("Cache in use: %dKB  Wants: %dKB  Max: %dMB",
        (int)(BTCONTENT.CacheUsed()/1024), (int)(BTCONTENT.CacheSize()/1024),
        (int)*cfg_cache_size);
      if(*cfg_ctcs) Interact("CTCS Server: %s", *cfg_ctcs);
      if(*cfg_verbose) cpu();
      opermenu.mode = 0;
      return 1;
    }else if( sel == 2 + O_NCHANNELS+1 + STATUSLINES ){  // pause/resume
      cfg_pause = !*cfg_pause;
      opermenu.mode = 0;
      return 1;
    }else if( sel == 3 + O_NCHANNELS+1 + STATUSLINES ){  // daemon
      cfg_daemon = true;
      opermenu.mode = 0;
      return 1;
    }else if( sel == 4 + O_NCHANNELS+1 + STATUSLINES ){  // update tracker
      if( Tracker.GetStatus() == DT_TRACKER_FREE ) Tracker.Reset(15);
      else Interact("Already connecting, please be patient...");
      opermenu.mode = 0;
      return 1;
    }else if( sel == 5 + O_NCHANNELS+1 + STATUSLINES ){  // update tracker
      Tracker.RestartTracker();
      opermenu.mode = 0;
      return 1;
    }else if( sel == 6 + O_NCHANNELS+1 + STATUSLINES ){  // configuration
      configmenu.mode = 0;  // safeguard
      Configure();
      opermenu.mode = 3;
      return 0;
    }
  }
  else if( 2==opermenu.mode ){
    if( !*param ){
      opermenu.mode = 0;
      return OperatorMenu();
    }
    ChangeChannel(opermenu.channel, param);
    opermenu.mode = 0;
    return OperatorMenu();
  }
  else if( 3==opermenu.mode ){
    if( Configure(param) ){
      opermenu.mode = 0;
      return OperatorMenu();
    }else return 0;
  }

  Interact("Exiting menu");
  opermenu.mode = 0;
  return 1;
}


// Return non-zero to exit configuration menu.
int Console::Configure(const char *param)
{
  ConfigGen *config;
  int ttyrows = 0, lines = 0, configcount = 0, nextopt = 0;
  bool more = false;

  if( 0==configmenu.mode ||
      (99==configmenu.mode && 0==configmenu.next_start) ){
    configmenu.next_start = 0;
    configmenu.n_opt = 0;
    Interact("Configuration Menu");
    lines++;
    if( !g_config_only ){
      Interact("Some options are not configurable while running.  To set and");
      Interact("save these values, run the client without a torrent file.");
      lines += 2;
    }
    configmenu.mode = 99;
  }
  if( 99==configmenu.mode ){  // continue the menu
    int otherlines = 4;
    configmenu.current_start = configmenu.next_start;
    configmenu.start_opt = configmenu.n_opt + 1;
    ttyrows = m_streams[O_INTERACT]->Rows() - 1;
    configcount = 0;
    for( config = CONFIG.First(); config; config = CONFIG.Next(config) ){
      if( configcount >= configmenu.next_start ){
        if( ttyrows > otherlines && lines >= ttyrows - otherlines ){
          if( !config->Hidden() ){
            more = true;
            if( !config->Locked() ) nextopt++;
          }
          continue;
        }
        if( !config->Hidden() ){
          Interact_n(" %c", config->Saving() ? '*' : ' ');
          if( !config->Locked() ) Interact_n("%2d)", ++configmenu.n_opt);
          else Interact_n("   ");
          Interact_n(" %s:  %s  %s", config->Desc(), config->Sval(),
            config->Info());
          Interact_n("");
          lines++;
        }
      }
      configcount++;
    }
    if( more || configmenu.next_start ){
      Interact("   M) More options (%d-%d)",
        more ? configmenu.n_opt + 1 : 1, configmenu.n_opt + nextopt);
    }
    configmenu.next_start = more ? configcount : 0;
    Interact("   S) Save marked options (*) to file");
    lines++;
    Interact("   X) Exit");
    configmenu.mode = 98;
  }
  if( 98==configmenu.mode ){  // selection prompt
    if( lines <= 1 )
      Interact("Configuration Menu (Press Enter to redisplay menu)");
    Interact_n("Enter selection: ");
    m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
    configmenu.mode = 1;
    return 0;
  }
  else if( 1==configmenu.mode ){  // option selected
    if( !param || !*param ){  // redisplay menu
      configmenu.next_start = configmenu.current_start;
      configmenu.n_opt = configmenu.start_opt - 1;
      configmenu.mode = 99;
      return Configure();
    }
    if( *param == 'x' || *param == 'X' ){
      configmenu.mode = 0;
      Interact("Exiting menu");
      return 1;
    }
    if( *param == 'm' || *param == 'M' ){
      configmenu.mode = 99;
      return Configure();
    }
    if( *param == 's' || *param == 'S' ){
      Interact_n("Filename (default %s): ", *cfg_config_file);
      m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
      configmenu.mode = 4;
      return 0;
    }
    int sel = atoi(param);
    if( sel < 1 || sel > configmenu.n_opt ){
      Interact_n("Enter selection: ");
      return 0;
    }

    // Find the selected Config and show manipulation options.
    int count = 0;
    for( config = CONFIG.First(); config; config = CONFIG.Next(config) ){
      if( !config->Hidden() && !config->Locked() ) count++;
      if( count == sel ) break;
    }
    Interact("%s:  %s  %s", config->Desc(), config->Sval(), config->Info());
    configmenu.selected = config;

    Interact(" C) Change value");
    Interact(" R) Reset to default (%s)", configmenu.selected->Sdefault());
    Interact(" S) %s for save",
      configmenu.selected->Saving() ? "Unmark" : "Mark");
    if( configmenu.selected->Type() == DT_CONFIG_STRING )
      Interact(" Z) Null value");
    Interact_n("Enter selection: ");
    m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
    configmenu.mode = 2;
    return 0;
  }
  else if( 2==configmenu.mode ){  // manipulate option
    if( !param || !*param ){
      configmenu.mode = 98;
      return Configure();
    }
    switch( *param ){
      case 'c':
      case 'C':
        Interact_n("New value: ");
        m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
        configmenu.mode = 3;
        return 0;
      case 'r':
      case 'R':
        configmenu.selected->Reset();
        break;
      case 's':
      case 'S':
        if( configmenu.selected->Saving() ) configmenu.selected->Unsave();
        else configmenu.selected->Save();
        break;
      case 'z':
      case 'Z':
        if( configmenu.selected->Type() == DT_CONFIG_STRING )
          configmenu.selected->Scan("");
        configmenu.selected->Save();
        break;
      default:
        break;
    }
    return Configure();
  }
  else if( 3==configmenu.mode ){  // change value
    if( param && *param ){
      ConfigGen *config;
      if( configmenu.selected->Scan(param) )
        configmenu.selected->Save();
      config = configmenu.selected;
      Interact("%s:  %s  %s", config->Desc(), config->Sval(), config->Info());
    }
    else Interact("Not changed");
    configmenu.mode = 98;
    return Configure();
  }
  else if( 4==configmenu.mode ){  // save to file
    if( param && *param ) cfg_config_file = param;
    if( CONFIG.Save(*cfg_config_file) )
      Interact("Configuration saved");
    configmenu.mode = 98;
    return Configure();
  }

  Interact("Exiting menu");
  configmenu.mode = 0;
  return 1;
}
 

int Console::ChangeChannel(dt_conchan_t channel, const char *param, int notify)
{
  ConStream *dest = (ConStream *)0;

  if( !param ) return -1;
  if( 0==strcasecmp(param, m_stdout.GetName()) ) dest = &m_stdout;
  else if( 0==strcasecmp(param, m_stderr.GetName()) ) dest = &m_stderr;
  else if( 0==strcasecmp(param, m_stdin.GetName()) ) dest = &m_stdin;
  else if( 0==strcasecmp(param, m_off.GetName()) ) dest = &m_off;
  else{
    for( int i=0; i <= O_NCHANNELS; i++ ){
      if( channel != i && 0==strcmp(param, m_streams[i]->GetName()) &&
          m_streams[i]->GetMode() == ((channel==O_INPUT) ? 0 : 1) ){
        dest = m_streams[i];
        break;
      }
    }
    if( !dest ){
      FILE *stream;
      if( (dest = new ConStream) ){
        if( 0==strcmp(param, m_streams[channel]->GetName()) ){
          delete m_streams[channel];
          m_streams[channel] = &m_off;
        }
        if( (stream = fopen(param, (channel==O_INPUT) ? "r" : "a")) )
          dest->Associate(stream, param, (channel==O_INPUT) ? 0 : 1);
        else{
          Interact("Error opening file: %s", strerror(errno));
          delete dest;
          dest = (ConStream *)0;
        }
      }else Interact("Failed to allocate memory.");
    }
  }
  if( dest ){
    if( O_INPUT==channel ) m_oldfd = m_streams[channel]->Fileno();
    if( m_streams[channel] != &m_stdout && m_streams[channel] != &m_stderr &&
        m_streams[channel] != &m_stdin && m_streams[channel] != &m_off ){
      int in_use = 0;
      for( int i=0; i <= O_NCHANNELS; i++ ){
        if( channel != i && m_streams[channel] == m_streams[i] ) in_use = 1;
      }
      if( !in_use ) delete m_streams[channel];
      else if( O_INPUT==channel ) m_streams[O_INPUT]->RestoreMode();
    }
    if( notify && (!*cfg_daemon || !m_streams[channel]->IsTTY()) ){
      switch( channel ){
      case O_NORMAL:
        Print("Output channel is now %s", dest->GetName());
        break;
      case O_DEBUG:
        Debug("Debug channel is now %s", dest->GetName());
        break;
      case O_INTERACT:
        Interact("Interactive output channel is now %s", dest->GetName());
        break;
      case O_INPUT:
        Interact("Input channel is now %s", dest->GetName());
        break;
      default:
        break;
      }
    }
    m_streams[channel] = dest;
    if( O_INPUT==channel ){
      m_streams[O_INPUT]->PreserveMode();
      m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
    }
    return 0;
  }else return -1;
}


void Console::ShowFiles()
{
  Bitfield tmpFilter;
  dt_count_t n = 0;

  Interact("Files in this torrent:");
  while( ++n <= BTCONTENT.GetNFiles() ){
    BTCONTENT.SetTmpFilter(n, &tmpFilter);
    Bitfield tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(tmpFilter);
    Interact("%d) %s [%llu] %d%%", (int)n, BTCONTENT.GetFileName(n),
      (unsigned long long)BTCONTENT.GetFileSize(n),
      BTCONTENT.GetFilePieces(n) ?
        (int)(100 * tmpBitfield.Count() / BTCONTENT.GetFilePieces(n)) : 0);
  }
}


void Console::Status(int immediate)
{
  static char buffer[80];

  if( immediate ) m_skip_status = 0;
  if( m_pre_dlrate.TimeUsed() || immediate ){
    if( m_skip_status ) m_skip_status = 0;
    else if( !m_streams[O_NORMAL]->IsSuspended() ||
             (*cfg_verbose && !m_streams[O_DEBUG]->IsSuspended()) ){
      // optimized to generate the status line only if it will be output
      (this->*m_statusline[*cfg_status_format])(buffer, sizeof(buffer));

      if( !m_status_last ) Print_n("");
      int tmplen = m_streams[O_NORMAL]->Cols() - 1;
      if( tmplen > 79 ) tmplen = 79;
      int len = strlen(buffer);
      if( 0==tmplen ) tmplen = (len < m_status_len) ? m_status_len : len;
      m_status_len = len;
      Update("%*.*s", -tmplen, tmplen, buffer);
      m_status_last = 1;

      if(*cfg_verbose)
        Debug("Cache: %dK/%dM  Hits: %d  Miss: %d  %d%%  Pre: %d/%d",
          (int)(BTCONTENT.CacheUsed()/1024), (int)*cfg_cache_size,
          (int)BTCONTENT.CacheHits(), (int)BTCONTENT.CacheMiss(),
          BTCONTENT.CacheHits() ? (int)(100 * BTCONTENT.CacheHits() /
            (BTCONTENT.CacheHits()+BTCONTENT.CacheMiss())) : 0,
          (int)BTCONTENT.CachePre(),
            (int)(Self.TotalUL() / DEFAULT_SLICE_SIZE));
    }

    m_pre_dlrate = Self.GetDLRate();
    m_pre_ulrate = Self.GetULRate();
  }
}


const char *Console::StatusLine()
{
  static char buffer[80];
  (this->*m_statusline[*cfg_status_format])(buffer, sizeof(buffer));
  return buffer;
}


void Console::StatusLine0(char buffer[], size_t length)
{
  char partial[30] = "";
  if( BTCONTENT.GetFilter() && !BTCONTENT.GetFilter()->IsEmpty() ){
    Bitfield tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(BTCONTENT.GetFilter());
    sprintf( partial, "P:%d/%d ",
      (int)tmpBitfield.Count(),
      (int)(BTCONTENT.GetNPieces() - BTCONTENT.GetFilter()->Count()) );
  }

  char checked[14] = "";
  if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
    sprintf( checked, "Checking: %d%%",
      (int)(100 * BTCONTENT.CheckedPieces() / BTCONTENT.GetNPieces()) );
  }

  snprintf(buffer, length,
    "%c %d/%d/%d [%d/%d/%d] %lluMB,%lluMB | %d,%dK/s | %d,%dK E:%d,%d %s%s",
    LIVE_CHAR[m_live_idx++],

    (int)WORLD.GetSeedsCount(),
    (int)(WORLD.GetPeersCount() - WORLD.GetSeedsCount()),
    (int)Tracker.GetPeersCount(),

    (int)BTCONTENT.pBF->Count(),
    (int)BTCONTENT.GetNPieces(),
    (int)WORLD.Pieces_I_Can_Get(),

    (unsigned long long)(Self.TotalDL() >> 20),
    (unsigned long long)(Self.TotalUL() >> 20),

    (int)(Self.RateDL() >> 10), (int)(Self.RateUL() >> 10),

    (int)(m_pre_dlrate.RateMeasure(Self.GetDLRate()) >> 10),
    (int)(m_pre_ulrate.RateMeasure(Self.GetULRate()) >> 10),

    (int)Tracker.GetRefuseClick(),
    (int)Tracker.GetOkClick(),

    partial,

    (Tracker.GetStatus()==DT_TRACKER_CONNECTING) ? "Connecting" :
      ( (Tracker.GetStatus()==DT_TRACKER_READY) ? "Connected" :
          (Tracker.IsRestarting() ? "Restarting" :
            (Tracker.IsQuitting() ? "Quitting" :
              (WORLD.IsPaused() ? "Paused" : checked))) )
  );
}


void Console::StatusLine1(char buffer[], size_t length)
{
  char partial[30] = "";
  if( BTCONTENT.GetFilter() && !BTCONTENT.GetFilter()->IsEmpty() ){
    bt_index_t have, avail, all;
    long premain = -1;
    char ptime[20] = "";
    Bitfield tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(BTCONTENT.GetFilter());
    have = tmpBitfield.Count();

    WORLD.Pieces_I_Can_Get(&tmpBitfield);
    tmpBitfield.Except(BTCONTENT.GetFilter());
    avail = tmpBitfield.Count();

    all = BTCONTENT.GetNPieces() - BTCONTENT.GetFilter()->Count();

    if( Self.RateDL() ){
      premain = (all - have) * BTCONTENT.GetPieceLength() / Self.RateDL() / 60;
      if( premain < 60000 ){  // 1000 hours
        snprintf(ptime, sizeof(ptime), " %d:%2.2d",
          (int)(premain / 60), (int)(premain % 60));
      }
    }
    sprintf(partial, "P:%d/%d%%%s ",
      (int)(100 * have / all), (int)(100 * avail / all), ptime);
  }


  char checked[14] = "";
  if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
    sprintf(checked, "Checking: %d%%",
      (int)(100 * BTCONTENT.CheckedPieces() / BTCONTENT.GetNPieces()));
  }

  char complete[8];
  if( BTCONTENT.IsFull() )
    sprintf(complete, "seeding");
  else if( BTCONTENT.Seeding() ){
    sprintf(complete, "seed%d%%",
      (int)(100 * BTCONTENT.pBF->Count() / BTCONTENT.GetNPieces()));
  }else{
    int have, avail, all;
    Bitfield tmpBitfield = *BTCONTENT.pBF;
    tmpBitfield.Except(*BTCONTENT.pBMasterFilter);
    have = tmpBitfield.Count();

    WORLD.Pieces_I_Can_Get(&tmpBitfield);
    tmpBitfield.Except(*BTCONTENT.pBMasterFilter);
    avail = tmpBitfield.Count();

    all = BTCONTENT.GetNPieces() - BTCONTENT.pBMasterFilter->Count();
    sprintf(complete, "%d/%d%%",
      (int)(100 * have / all), (int)(100 * avail / all));
  }

  long remain = -1;
  char timeleft[20];
  if( !BTCONTENT.Seeding() || BTCONTENT.FlushFailed() ){  // downloading
    if( Self.RateDL() ){
      // don't overflow remain
      if( BTCONTENT.GetLeftBytes() < (dt_datalen_t)Self.RateDL() << 22 )
        remain = BTCONTENT.GetLeftBytes() / Self.RateDL() / 60;
      else remain = 99999;
    }
  }else{  // seeding
    if( *cfg_seed_time )
      remain = (long)( (*cfg_seed_time / 60) -
                       (now - BTCONTENT.GetSeedTime()) / 60 );
    else if( Self.RateUL() ){
      // don't overflow remain
      if( *cfg_seed_ratio *
          (Self.TotalDL() ? Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) -
          Self.TotalUL() < (dt_datalen_t)Self.RateUL() << 22 ){
        remain = (long)( *cfg_seed_ratio *
          (Self.TotalDL() ? Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) -
          Self.TotalUL() ) / Self.RateUL() / 60;
      }else remain = 99999;
    }
  }
  if( remain >= 0 ){
    if( remain < 60000 ){  // 1000 hours
      snprintf(timeleft, sizeof(timeleft), "%d:%2.2d",
        (int)(remain / 60), (int)(remain % 60));
    }else strcpy(timeleft, ">999hr");
  }else if( BTCONTENT.CheckedPieces() < BTCONTENT.GetNPieces() ){
    // Don't say stalled if still checking and nothing to download yet.
    Bitfield tmpBitfield = *BTCONTENT.pBChecked;
    tmpBitfield.Except(BTCONTENT.pBF);
    if( tmpBitfield.IsEmpty() ) strcpy(timeleft, "unknown");
    else strcpy(timeleft, "stalled");
  }else strcpy(timeleft, "stalled");

  snprintf(buffer, length,
    "%c S:%d/%d L:%d/%d C:%d  R=%.2f D=%d U=%d K/s  %s %s  %s%s",
    LIVE_CHAR[m_live_idx++],

    (int)WORLD.GetSeedsCount(),
    (int)(Tracker.GetSeedsCount() - (BTCONTENT.IsFull() ? 1 : 0)),

    (int)(WORLD.GetPeersCount() - WORLD.GetSeedsCount() - WORLD.GetConnCount()),
    (int)(Tracker.GetPeersCount() - Tracker.GetSeedsCount() -
          (!BTCONTENT.IsFull() ? 1 : 0)),

    (int)WORLD.GetConnCount(),


    (double)Self.TotalUL() / (Self.TotalDL() ? Self.TotalDL() :
                               BTCONTENT.GetTotalFilesLength()),

    (int)(Self.RateDL() >> 10), (int)(Self.RateUL() >> 10),

    complete, timeleft,

    partial,

    (Tracker.GetStatus()==DT_TRACKER_CONNECTING) ? "Connecting" :
      ( (Tracker.GetStatus()==DT_TRACKER_READY) ? "Connected" :
          (Tracker.IsRestarting() ? "Restarting" :
            (Tracker.IsQuitting() ? "Quitting" :
              (WORLD.IsPaused() ? "Paused" : checked))) )
  );
}


void Console::Print(const char *message, ...)
{
  va_list ap;

  if( DT_CONMODE_LINES != m_streams[O_INPUT]->GetInputMode() ||
      m_streams[O_INPUT]->IsSuspended() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    va_start(ap, message);
    if( m_streams[O_NORMAL]->Output(message, ap) )
      SyncNewlines(O_NORMAL);
    va_end(ap);
  }
  if( *cfg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    va_start(ap, message);
    if( m_streams[O_DEBUG]->Output(message, ap) )
      SyncNewlines(O_DEBUG);
    va_end(ap);
  }
}


/* Print message without a terminating newline
   With a null message, start/insure a new line
*/
void Console::Print_n(const char *message, ...)
{
  va_list ap;

  if( m_status_last && message && *message ) Print_n("");
  m_status_last = 0;

  if( DT_CONMODE_LINES != m_streams[O_INPUT]->GetInputMode() ||
      m_streams[O_INPUT]->IsSuspended() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    va_start(ap, message);
    if( m_streams[O_NORMAL]->Output_n(message, ap) )
      SyncNewlines(O_NORMAL);
    va_end(ap);
  }
  if( *cfg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    va_start(ap, message);
    if( m_streams[O_DEBUG]->Output_n(message, ap) )
      SyncNewlines(O_DEBUG);
    va_end(ap);
  }
}


/* Update (replace) the current line (no terminating newline)
*/
void Console::Update(const char *message, ...)
{
  va_list ap;

  m_status_last = 0;

  if( DT_CONMODE_LINES != m_streams[O_INPUT]->GetInputMode() ||
      m_streams[O_INPUT]->IsSuspended() ||
      (!m_streams[O_NORMAL]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_NORMAL]->SameDev(m_streams[O_INPUT])) ){
    va_start(ap, message);
    if( m_streams[O_NORMAL]->Update(message, ap) )
      SyncNewlines(O_NORMAL);
    va_end(ap);
  }
  if( *cfg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
    va_start(ap, message);
    if( m_streams[O_DEBUG]->Update(message, ap) )
      SyncNewlines(O_DEBUG);
    va_end(ap);
  }
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
  va_end(ap);
  if( *cfg_verbose && !m_streams[O_DEBUG]->SameDev(m_streams[O_WARNING]) ){
    va_start(ap, message);
    if( m_streams[O_DEBUG]->Output(message, ap) )
      SyncNewlines(O_DEBUG);
    va_end(ap);
  }

  if( sev && *cfg_ctcs ){
    char cmsg[CTCS_BUFSIZE];
    va_start(ap, message);
    vsnprintf(cmsg, CTCS_BUFSIZE, message, ap);
    CTCS.Send_Info(sev, cmsg);
    va_end(ap);
  }
}


void Console::Debug(const char *message, ...)
{
  static char buffer[80];
  if( !*cfg_verbose ) return;

  char *format = (char *)0;
  size_t buflen;
  va_list ap;

  if( DT_CONMODE_LINES != m_streams[O_INPUT]->GetInputMode() ||
      m_streams[O_INPUT]->IsSuspended() ||
      (!m_streams[O_DEBUG]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_DEBUG]->SameDev(m_streams[O_INPUT])) ){
    size_t need = strlen(message)+1 + 10*sizeof(unsigned long)/4;
    if( need > sizeof(buffer) && (format = new char[need]) ) buflen = need;
    else{
      format = buffer;
      buflen = sizeof(buffer);
    }

    snprintf(format, buflen, "%lu %s", (unsigned long)now, message);

    va_start(ap, message);
    if( m_streams[O_DEBUG]->Output(format, ap) )
      SyncNewlines(O_DEBUG);
    va_end(ap);

    if( format && format != buffer ) delete []format;
  }
}


/* Print debug message without a terminating newline
   With a null message, start/insure a new line
*/
void Console::Debug_n(const char *message, ...)
{
  static char buffer[80];
  static int f_new_line = 1;
  if( !*cfg_verbose ) return;

  va_list ap;

  if( DT_CONMODE_LINES != m_streams[O_INPUT]->GetInputMode() ||
      m_streams[O_INPUT]->IsSuspended() ||
      (!m_streams[O_DEBUG]->SameDev(m_streams[O_INTERACT]) &&
       !m_streams[O_DEBUG]->SameDev(m_streams[O_INPUT])) ){
    if( m_streams[O_DEBUG]->SameDev(m_streams[O_NORMAL]) ){
      if( m_status_last && message && *message ) Debug_n("");
      m_status_last = 0;
    }
    if( f_new_line && message && *message ){
      char *format = (char *)0;
      size_t buflen;
      size_t need = strlen(message)+1 + 10*sizeof(unsigned long)/4;
      if( need > sizeof(buffer) && (format = new char[need]) ) buflen = need;
      else{
        format = buffer;
        buflen = sizeof(buffer);
      }

      snprintf(format, buflen, "%lu %s", (unsigned long)now, message);

      va_start(ap, message);
      if( m_streams[O_DEBUG]->Output_n(format, ap) )
        SyncNewlines(O_DEBUG);
      va_end(ap);
      if( format && format != buffer ) delete []format;
    }else{
      va_start(ap, message);
      if( m_streams[O_DEBUG]->Output_n(message, ap) )
        SyncNewlines(O_DEBUG);
      va_end(ap);
    }

    if( message && *message ) f_new_line = 0;
    else f_new_line = 1;
  }
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

  if( m_streams[O_INTERACT]->SameDev(m_streams[O_NORMAL]) ){
    if( m_status_last && message && *message ) Interact_n("");
    m_status_last = 0;
  }
  va_start(ap, message);
  if( m_streams[O_INTERACT]->Output_n(message, ap) )
    SyncNewlines(O_INTERACT);
  va_end(ap);
}


/* Update (replace) the current interactive line (no terminating newline)
*/
void Console::InteractU(const char *message, ...)
{
  va_list ap;

  if( m_streams[O_INTERACT]->SameDev(m_streams[O_NORMAL]) ){
    if( m_status_last ) Interact_n("");
    m_status_last = 0;
  }
  va_start(ap, message);
  if( m_streams[O_INTERACT]->Update(message, ap) )
    SyncNewlines(O_INTERACT);
  va_end(ap);
}


// Avoid using this during normal operation, as it blocks for input!
char *Console::Input(const char *prompt, char *field, size_t length)
{
  char *retval;

  if( prompt && *prompt ){
    Interact_n("");
    Interact_n("%s", prompt);
  }
  m_streams[O_INPUT]->SetInputMode(DT_CONMODE_LINES);
  retval = m_streams[O_INPUT]->Input(field, length);
  SyncNewlines(O_INPUT);
  m_streams[O_INPUT]->SetInputMode(DT_CONMODE_CHARS);
  return retval;
}


void Console::SyncNewlines(int master)
{
  for( int i=0; i < O_NCHANNELS; i++ ){
    if( i != master && m_streams[i]->SameDev(m_streams[master]) )
      m_streams[i]->SyncNewline(m_streams[master]);
  }
}


void Console::cpu()
{
  if(*cfg_verbose)
    Debug( "%.2f CPU seconds used; %lu seconds elapsed (%.2f%% usage)",
      clock() / (double)CLOCKS_PER_SEC,
      (unsigned long)(time((time_t *)0) - BTCONTENT.GetStartTime()),
      clock() / (double)CLOCKS_PER_SEC /
        (time((time_t *)0) - BTCONTENT.GetStartTime()) * 100 );
}


RETSIGTYPE Console::Signal(int sig_no)
{
  switch( sig_no ){
  case SIGTTOU:
    for( int i=0; i < O_NCHANNELS; i++ )
      if( m_streams[i]->IsTTY() ) m_streams[i]->Suspend();
    m_conmode = m_streams[O_INPUT]->GetInputMode();
    break;
  case SIGTTIN:
    if( m_streams[O_INPUT]->IsTTY() ) m_streams[O_INPUT]->Suspend();
    m_conmode = m_streams[O_INPUT]->GetInputMode();
    break;
  case SIGCONT:
    for( int i=0; i <= O_NCHANNELS; i++ )
      if( m_streams[i]->IsTTY() ) m_streams[i]->Resume();
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


void Console::Daemonize()
{
#ifdef HAVE_WORKING_FORK
  dt_mem_t orig_cache_size = 0;
  pid_t r;
  int nullfd = -1;

  if( *cfg_cache_size && BTCONTENT.CacheUsed() ){
    orig_cache_size = *cfg_cache_size;
    cfg_cache_size /= 2;
  }

  if( (r = fork()) < 0 ){
    Warning(2, "warn, fork to background failed:  %s", strerror(errno));
    cfg_daemon = false;
    goto restorecache;
  }else if( r ){
    g_secondary_process = g_daemon_parent = true;
    exit(EXIT_SUCCESS);
  }

  for( int i=0; i <= O_NCHANNELS; i++ ){
    if( m_streams[i]->IsTTY() && ChangeChannel((dt_conchan_t)i, "off", 0) < 0 )
      m_streams[i]->Suspend();
  }
  nullfd = OpenNull(nullfd, &m_stdin, 0);
  nullfd = OpenNull(nullfd, &m_stdout, 1);
  nullfd = OpenNull(nullfd, &m_stderr, 2);

  if( setsid() < 0 ){
    Warning(2,
      "warn, failed to create new session (continuing in background):  %s",
      strerror(errno));
    goto restorecache;
  }

  if( (r = fork()) < 0 ){
    Warning(2,
      "warn, final fork failed (continuing in background):  %s",
      strerror(errno));
    goto restorecache;
  }else if( r ){
    g_secondary_process = true;
    exit(EXIT_SUCCESS);
  }
  else if(*cfg_verbose) Debug("Running in daemon (background) mode.");

 restorecache:
  if( orig_cache_size ){
    cfg_cache_size = orig_cache_size;
  }
#endif
}


int Console::OpenNull(int nullfd, ConStream *stream, int sfd)
{
  if( stream->IsTTY() || !*cfg_redirect_io ){
    int mfd = stream->Fileno();
    if( mfd < 0 ) mfd = sfd;
    stream->Close();
    if( nullfd < 0 ) nullfd = open("/dev/null", O_RDWR);
    if( nullfd >= 0 && nullfd != mfd ) dup2(nullfd, mfd);
  }
  return nullfd;
}

