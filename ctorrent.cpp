#include "def.h"
#include <sys/types.h>

#ifdef WINDOWS
#include <windows.h>
#else
#include <signal.h>
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include "ctorrent.h"
#include "btconfig.h"
#include "btcontent.h"
#include "downloader.h"
#include "peerlist.h"
#include "tracker.h"
#include "ctcs.h"
#include "console.h"

#include "config.h"
#include "util.h"

#ifndef WINDOWS
#include "sigint.h"
#endif

void usage();
int CheckOptions(int argc, const char *const *argv);
int GetOpts(int argc, const char *const *argv, bool checkonly=false);


bt_length_t arg_piece_length = 256 * 1024;
char *arg_save_as = (char *)0;
bool arg_flg_make_torrent = false;
char *arg_metainfo_file = (char *)0;  // will be owned by BTCONTENT
char *arg_comment = (char *)0;
bool arg_flg_private = false;
bool arg_flg_force_seed_mode = false;
bool arg_flg_check_only = false;
bool arg_flg_exam_only = false;  

// temporary config values
bool arg_daemon = *cfg_daemon;
bool arg_config_mode = false;


int main(int argc, char **argv)
{
  RandomInit();
  CONSOLE.Init();
  InitConfig();

  if( CheckOptions(argc, argv) < 0 ) Exit(EXIT_FAILURE);
  CONFIG.Load(*cfg_config_file);
  if( GetOpts(argc, argv) < 0 ) Exit(EXIT_FAILURE);

  if( g_config_only ){
    char param[MAXPATHLEN] = "";

    while( CONSOLE.Configure(param) == 0 &&
           CONSOLE.Input("", param, sizeof(param)) );
    Exit(EXIT_SUCCESS);
  }

  if( arg_flg_make_torrent ){
    if( 0==TRACKER.GetNTiers() ){
      CONSOLE.Warning(1, "Please use -u to specify an announce URL.");
      Exit(EXIT_FAILURE);
    }
    if( !arg_save_as ){
      CONSOLE.Warning(1, "Please use -s to specify a metainfo file name.");
      Exit(EXIT_FAILURE);
    }
    if( BTCONTENT.InitialFromFS(arg_metainfo_file, arg_piece_length) < 0 ||
        BTCONTENT.CreateMetainfoFile(arg_save_as, arg_comment, arg_flg_private)
          < 0 ){
      CONSOLE.Warning(1, "Failed creating torrent metainfo file.");
      Exit(EXIT_FAILURE);
    }
    CONSOLE.Print("Create metainfo file %s successful.", arg_save_as);
    Exit(EXIT_SUCCESS);
  }

  if( *cfg_verbose ) CONFIG.Dump();
  cfg_daemon = arg_daemon;  // triggers action

  if( BTCONTENT.InitialFromMI(arg_metainfo_file, arg_save_as,
        arg_flg_force_seed_mode, arg_flg_check_only, arg_flg_exam_only) < 0 ){
    CONSOLE.Warning(1, "Failed during initial torrent setup.");
    Exit(EXIT_FAILURE);
  }

  if( !arg_flg_exam_only && (!arg_flg_check_only || arg_flg_force_seed_mode) ){
    if( WORLD.Init() < 0 ){
      CONSOLE.Warning(2, "warn, you can't accept connections.");
    }

    if( TRACKER.Initial() < 0 ){
      CONSOLE.Warning(1, "Failed during tracker setup.");
      Exit(EXIT_FAILURE);
    }

    sig_setup();  // setup signal handling
    CONSOLE.Interact(
      "Press 'h' or '?' for help (display/control client options)." );

    Downloader();
    WORLD.CloseAll();
    if( *cfg_cache_size ) BTCONTENT.FlushCache();
    if( BTCONTENT.NeedMerge() ){
      CONSOLE.Interact_n();
      CONSOLE.Interact_n("Merging staged data");
      BTCONTENT.MergeAll();
    }
  }
  if( !arg_flg_exam_only ){
    BTCONTENT.SaveBitfield();
    if(*cfg_verbose) CONSOLE.cpu();
  }
  Exit(EXIT_SUCCESS);
}


int CheckOptions(int argc, const char *const *argv)
{
  return GetOpts(argc, argv, true);
}


int GetOpts(int argc, const char *const *argv, bool checkonly)
{
  const char *options, *multiopts, *nonegopts, *argpos, *optarg, *pos;
  char opt;
  int argn, noptions, optn;
  bool negate, *foundopt = (bool *)0, error = false;
  size_t len;
  char *tmp = (char *)0;

  errno = 0;
  if( argc < 2 ){
    arg_config_mode = g_config_only = true;
    return 0;
  }

  if( 0==strncmp(argv[1], "-t", 2) )
    options = "tc:l:ps:u:v";
  else options = "aA:b:cC:dD:e:E:f:Fi:I:M:m:n:P:p:s:S:Tu:U:vxX:z:hH";

  // Options which may be given more than once.
  multiopts = "adu";
  // Options which cannot be negated.
  nonegopts = "cfFlsu";

  noptions = strlen(options);
  if( !(foundopt = new bool[noptions]) ){
    errno = ENOMEM;
    return -1;
  }
  for( int i = 0; i < noptions; i++ ) foundopt[i] = false;

  for( argn = 1; argn < argc && *argv[argn] == '-' && !error; argn++ ){
    argpos = &argv[argn][1];
    optarg = (char *)0;
    while( !optarg && (opt = *argpos++) && !error ){
      negate = false;
      if( '-' == opt ){  // negate the option
        negate = true;
        opt = *argpos++;
        if( strchr(nonegopts, opt) ){
          CONSOLE.Warning(1, "Option -%c cannot be negated.", opt);
          error = true; break;
        }
      }
      if( !(pos = strchr(options, opt)) ){  // unknown option
        CONSOLE.Warning(1, "Use -h for help/usage.");
        error = true; break;
      }
      optn = pos - options;
      if( foundopt[optn] && !strchr(multiopts, opt) ){
        CONSOLE.Warning(1, "Usage error:  -%c was specified twice.", opt);
        error = true; break;
      }

      if( !negate && ':' == *(pos + 1) )
        optarg = *argpos ? argpos : argv[++argn];
      else optarg = (char *)0;

      switch( opt ){
        case 'a':  // change allocation mode
          if( !checkonly ){
            if( negate ){
              cfg_allocate.Reset();
              if( arg_config_mode ) cfg_allocate.Unsave();
            }else{
              if( *cfg_allocate == cfg_allocate.Max() )
                cfg_allocate = cfg_allocate.Min();
              else cfg_allocate++;
              if( arg_config_mode ) cfg_allocate.Save();
            }
          }
          break;

        case 'A':  // HTTP user-agent header string
          if( !checkonly ){
            if( negate ){
              cfg_user_agent.Reset();
              if( arg_config_mode ) cfg_user_agent.Unsave();
            }else{
              cfg_user_agent = optarg;
              if( arg_config_mode ) cfg_user_agent.Save();
            }
          }
          break;

        case 'b':  // set bitfield file
          if( !checkonly ){
            if( negate ){
              cfg_bitfield_file.Reset();
              if( arg_config_mode ) cfg_bitfield_file.Unsave();
            }else{
              cfg_bitfield_file = optarg;
              if( arg_config_mode ) cfg_bitfield_file.Save();
            }
          }
          break;

        case 'c':  // check piece hashes only
          if( arg_flg_make_torrent ){
            if( !(arg_comment = new char[strlen(optarg) + 1]) ){
              errno = ENOMEM;
              error = true; break;
            }
            strcpy(arg_comment, optarg);
          }else arg_flg_check_only = true;
          break;
    
        case 'C':  // max cache size
          if( !checkonly ){
            if( negate ){
              cfg_cache_size.Reset();
              if( arg_config_mode ) cfg_cache_size.Unsave();
            }else{
              cfg_cache_size = atoi(optarg);
              if( arg_config_mode ) cfg_cache_size.Save();
            }
          }
          break;

        case 'd':  // daemon mode (fork to background)
          if( !checkonly ){
            if( negate ){
              arg_daemon = false;
              cfg_daemon.Reset();
              cfg_redirect_io.Reset();
              if( arg_config_mode ){
                cfg_daemon.Unsave();
                cfg_redirect_io.Unsave();
              }
            }else if( arg_daemon ){
              cfg_redirect_io = true;
              if( arg_config_mode ) cfg_redirect_io.Save();
            }else{
              arg_daemon = true;
              if( arg_config_mode ) cfg_daemon.Save();
            }
          }
          break;

        case 'D':  // download bandwidth limit
          if( !checkonly ){
            if( negate ){
              cfg_max_bandwidth_down.Reset();
              if( arg_config_mode ) cfg_max_bandwidth_down.Unsave();
            }else{
              cfg_max_bandwidth_down = (dt_rate_t)(strtod(optarg, NULL) * 1024);
              if( arg_config_mode ) cfg_max_bandwidth_down.Save();
            }
          }
          break;

        case 'e':  // seed timeout (exit time)
          if( !checkonly ){
            if( negate ){
              cfg_seed_hours.Reset();
              if( arg_config_mode ) cfg_seed_hours.Unsave();
            }else{
              cfg_seed_hours = strtoul(optarg, NULL, 10);
              if( arg_config_mode ) cfg_seed_hours.Save();
            }
          }
          break;

        case 'E':  // target seed ratio
          if( !checkonly ){
            if( negate ){
              cfg_seed_ratio.Reset();
              if( arg_config_mode ) cfg_seed_ratio.Unsave();
            }else{
              cfg_seed_ratio = atof(optarg);
              if( arg_config_mode ) cfg_seed_ratio.Save();
            }
          }
          break;

        case 'f':  // configuration file
          cfg_config_file = optarg;
          break;

        case 'F':  // force bitfield accuracy or seeding, skip hash check
          arg_flg_force_seed_mode = true;
          break;

        case 'h':  // help
        case 'H':  // help
          usage();
          error = true; break;

        case 'i':  // listen on given IP address
          if( !checkonly ){
            if( negate ){
              cfg_listen_addr.Reset();
              if( arg_config_mode ) cfg_listen_addr.Unsave();
            }else{
              cfg_listen_addr = optarg;
              if( arg_config_mode ) cfg_listen_addr.Save();
            }
          }
          break;

        case 'I':  // set public IP address
          if( !checkonly ){
            if( negate ){
              cfg_public_ip.Reset();
              if( arg_config_mode ) cfg_public_ip.Unsave();
            }else{
              cfg_public_ip = optarg;
              if( arg_config_mode ) cfg_public_ip.Save();
            }
          }
          break;

        case 'l':  // piece length
          arg_piece_length = atol(optarg);
          if( arg_piece_length < 65536 || arg_piece_length > 4096*1024 ){
            CONSOLE.Warning(1,
              "Option -%c argument must be between 65536 and %d.", opt,
              4096*1024);
            error = true; break;
          }
          break;

        case 'm':  // min peers
          if( !negate && !cfg_min_peers.Valid(atoi(optarg)) ){
            CONSOLE.Warning(1, "Option -%c argument must be between %s and %s.",
              opt, cfg_min_peers.Smin(), cfg_min_peers.Smax());
            error = true; break;
          }
          if( !checkonly ){
            if( negate ){
              cfg_min_peers.Reset();
              if( arg_config_mode ) cfg_min_peers.Unsave();
            }else{
              cfg_min_peers = atoi(optarg);
              if( arg_config_mode ) cfg_min_peers.Save();
            }
          }
          break;

        case 'M':  // max peers
          if( !negate && !cfg_max_peers.Valid(atoi(optarg)) ){
            CONSOLE.Warning(1, "Option -%c argument must be between %s and %s.",
              opt, cfg_max_peers.Smin(), cfg_max_peers.Smax());
            error = true; break;
          }
          if( !checkonly ){
            if( negate ){
              cfg_max_peers.Reset();
              if( arg_config_mode ) cfg_max_peers.Unsave();
            }else{
              cfg_max_peers = atoi(optarg);
              if( arg_config_mode ) cfg_max_peers.Save();
            }
          }
          break;

        case 'n':  // file download list
          if( !checkonly ){
            if( negate ){
              cfg_file_to_download.Reset();
              if( arg_config_mode ) cfg_file_to_download.Unsave();
            }else{
              cfg_file_to_download = optarg;
              if( arg_config_mode ) cfg_file_to_download.Save();
            }
          }
          break;

        case 'p':  // listen on given port
          if( arg_flg_make_torrent ) arg_flg_private = true;
          else if( !checkonly ){
            if( negate ){
              cfg_listen_port.Reset();
              if( arg_config_mode ) cfg_listen_port.Unsave();
            }else{
              cfg_listen_port.Scan(optarg);
              if( arg_config_mode ) cfg_listen_port.Save();
            }
          }
          break;

        case 'P':  // peer ID prefix
          if( !negate && (len = strlen(optarg)) > cfg_peer_prefix.MaxLen() ){
            CONSOLE.Warning(1,
              "Option -%c argument must be %d or less characters.", opt,
              cfg_peer_prefix.MaxLen());
            error = true; break;
          }
          if( !checkonly ){
            if( negate ){
              cfg_peer_prefix.Reset();
              if( arg_config_mode ) cfg_peer_prefix.Unsave();
            }else{
              if( len == 1 && *optarg == '-' ) cfg_peer_prefix = "";
              else cfg_peer_prefix = optarg;
              if( arg_config_mode ) cfg_peer_prefix.Save();
            }
          }
          break;

        case 's':  // save as given file/dir name
          if( !*optarg ){
            errno = EINVAL;
            error = true; break;
          }
          if( !checkonly ){
            if( !(arg_save_as = new char[strlen(optarg) + 1]) ){
              errno = ENOMEM;
              error = true; break;
            }
            strcpy(arg_save_as, optarg);
          }
          break;

        case 'S':  // CTCS server
          if( !negate && !cfg_ctcs.Valid(optarg) ){
            CONSOLE.Warning(1,
              "Option -%c argument must be in the format host:port.", opt);
            error = true; break;
          }
          if( !checkonly ){
            if( negate ){
              cfg_ctcs.Reset();
              if( arg_config_mode ) cfg_ctcs.Unsave();
            }else{
              cfg_ctcs = optarg;
              if( arg_config_mode ) cfg_ctcs.Save();
            }
          }
          break;

        case 't':  // make torrent
          arg_flg_make_torrent = true;
          CONSOLE.NoInput();
          break;

        case 'T':  // convert foreign filenames to printable text
          if( !checkonly ){
            if( negate ){
              cfg_convert_filenames.Reset();
              if( arg_config_mode ) cfg_convert_filenames.Unsave();
            }else{
              cfg_convert_filenames = true;
              if( arg_config_mode ) cfg_convert_filenames.Save();
            }
          }
          break;

        case 'u':  // tracker announce URL
          if( !*optarg ){
            errno = EINVAL;
            error = true; break;
          }
          if( !checkonly && !arg_flg_exam_only &&
              TRACKER.AddTracker(optarg, true) < 0 )
            error = true; break;
          break;

        case 'U':  // upload bandwidth limit
          if( !checkonly ){
            if( negate ){
              cfg_max_bandwidth_up.Reset();
              if( arg_config_mode ) cfg_max_bandwidth_up.Unsave();
            }else{
              cfg_max_bandwidth_up = (dt_rate_t)(strtod(optarg, NULL) * 1024);
              if( arg_config_mode ) cfg_max_bandwidth_up.Save();
            }
          }
          break;

        case 'v':  // verbose output
          if( negate ){
            cfg_verbose.Reset();
            if( arg_config_mode ) cfg_verbose.Unsave();
          }else{
            cfg_verbose = true;
            if( arg_config_mode ) cfg_verbose.Save();
          }
          break;

        case 'x':  // print torrent information only
          arg_flg_exam_only = true;
          CONSOLE.NoInput();
          break;

        case 'X':  // "user exit" (command) on download completion
          if( !negate && !*optarg ){
            errno = EINVAL;
            error = true; break;
          }
#ifndef HAVE_SYSTEM
          CONSOLE.Warning(1, "Option -%c is not supported on your system.",
            opt);
          error = true; break;
#endif
#ifndef HAVE_WORKING_FORK
          CONSOLE.Warning(2,
            "No working fork function found; be sure the -%c command is brief!",
            opt);
#endif
          if( !checkonly ){
            if( negate ){
              cfg_completion_exit.Reset();
              if( arg_config_mode ) cfg_completion_exit.Unsave();
            }else{
              cfg_completion_exit = optarg;
              if( arg_config_mode ) cfg_completion_exit.Save();
            }
          }
          break;

        case 'z':  // slice size for requests
          if( !negate && !cfg_req_slice_size.Valid(atoi(optarg) * 1024) ){
            CONSOLE.Warning(1, "Option -%c argument must be between %d and %d.",
              opt, cfg_req_slice_size.Min() / 1024,
              cfg_req_slice_size.Max() / 1024);
            error = true; break;
          }
          if( !checkonly ){
            if( negate ){
              cfg_req_slice_size.Reset();
              if( arg_config_mode ) cfg_req_slice_size.Unsave();
            }else{
              cfg_req_slice_size = atoi(optarg) * 1024;
              if( arg_config_mode ) cfg_req_slice_size.Save();
            }
          }
          break;

        default:  // can't happen
          break;
      }
      if( !negate ) foundopt[optn] = true;
    }
  }
  if( foundopt ){
    delete []foundopt;
    foundopt = (bool *)0;
  }
  if( error ) goto err;

  if( argc - argn != 1 || !*argv[argn] ){
    if( arg_flg_make_torrent ){
      CONSOLE.Warning(1,
        "Must specify torrent contents (one file or directory)");
      goto err;
    }else if( argc - argn > 1 ){
      CONSOLE.Warning(1, "Must specify exactly one torrent file");
      goto err;
    }else{
      if( checkonly ) arg_config_mode = true;
      else{
        CONSOLE.Warning(1,
          "No torrent file specified--entering configuration mode");
        CONSOLE.Warning(1, "Use -h for help/usage.");
        g_config_only = true;
      }
      return 0;
    }
  }
  if( checkonly ) return 0;

  if( !(arg_metainfo_file = new char[strlen(argv[argn]) + 1]) ){
    errno = ENOMEM;
    goto err;
  }
  strcpy(arg_metainfo_file, argv[argn]);

  if( (tmp = new char[strlen(arg_metainfo_file) + 4]) ){
    strcpy(tmp, arg_metainfo_file);
    strcat(tmp, ".bf");
    cfg_bitfield_file.SetDefault(tmp);
    delete []tmp;
    if( !*cfg_bitfield_file ) cfg_bitfield_file.Reset();
  }
  return 0;

 err:
  if( foundopt ) delete []foundopt;
  if( errno )
    CONSOLE.Warning(1, "Error while parsing options:  %s", strerror(errno));
  return -1;
}


void usage()
{
  CONSOLE.NoInput();
  fprintf(stderr, "%s   Original code Copyright: YuHong(992126018601033)\n",
    PACKAGE_STRING);
  fprintf(stderr,
    "WARNING: THERE IS NO WARRANTY FOR CTorrent. USE AT YOUR OWN RISK!!!\n");
  fprintf(stderr, "\nGeneral Options:\n");
  fprintf(stderr, "%-15s %s\n", "-h/-H", "Show this message");
  fprintf(stderr, "%-15s %s %s)\n", "-f filename",
    "Configuration file (default", cfg_config_file.Sdefault());
  fprintf(stderr, "%-15s %s\n", "-x",
    "Decode metainfo (torrent) file only, don't download");
  fprintf(stderr, "%-15s %s\n", "-c", "Check pieces only, don't download");
  fprintf(stderr, "%-15s %s\n", "-v", "Verbose output (for debugging)");

  fprintf(stderr, "\nDownload Options:\n");
  fprintf(stderr, "%-15s %s %s hours)\n", "-e int",
    "Exit while seed <int> hours later (default", cfg_seed_hours.Sdefault());
  fprintf(stderr, "%-15s %s\n", "-E num",
    "Exit after seeding to <num> ratio (UL:DL)");
  fprintf(stderr, "%-15s %s\n", "-i ip",
    "Listen for connections on specific IP address (default all/any)");
  fprintf(stderr, "%-15s %s %d on down)\n", "-p port",
    "Listen port (default", (int)*cfg_default_port);
  fprintf(stderr, "%-15s %s\n", "-I ip",
    "Specify public/external IP address for peer connections");
  fprintf(stderr, "%-15s %s\n", "-u URL",
    "Specify additional announce (tracker) URLs");
  fprintf(stderr, "%-15s %s\n", "-s filename",
    "Download (\"save as\") to a different file or directory");
  fprintf(stderr, "%-15s %s %sMB)\n", "-C cache_size",
    "Cache size, unit MB (default", cfg_cache_size.Sdefault());
  fprintf(stderr, "%-15s %s\n", "-F",
    "Force saved bitfield or seed mode (skip initial hash check)");
  fprintf(stderr, "%-15s %s\n", "-b filename",
    "Specify bitfield save file (default is torrent+\".bf\")");
  fprintf(stderr, "%-15s %s %s)\n", "-M max_peers",
    "Max peers count (default", cfg_max_peers.Sdefault());
  fprintf(stderr, "%-15s %s %s)\n", "-m min_peers",
    "Min peers count (default", cfg_min_peers.Sdefault());
  fprintf(stderr, "%-15s %s %s, max %s)\n", "-z slice_size",
    "Download slice/block size, unit KB (default",
    cfg_req_slice_size_k.Sdefault(), cfg_req_slice_size_k.Smax());
  fprintf(stderr, "%-15s %s\n", "-n file_list",
    "Specify file number(s) to download");
  fprintf(stderr, "%-15s %s\n", "-D rate", "Max bandwidth down (unit KB/s)");
  fprintf(stderr, "%-15s %s\n", "-U rate", "Max bandwidth up (unit KB/s)");
  fprintf(stderr, "%-15s %s%s\")\n", "-P peer_id",
    "Set Peer ID prefix (default \"", cfg_peer_prefix.Sdefault());
  fprintf(stderr, "%-15s %s%s\")\n", "-A user_agent",
    "Set User-Agent header (default \"", cfg_user_agent.Sdefault());
  fprintf(stderr, "%-15s %s\n", "-S host:port",
    "Use CTCS server at host:port");
  fprintf(stderr, "%-15s %s\n", "-a", "Preallocate files on disk");
  fprintf(stderr, "%-15s %s\n", "-aa",
    "Do not precreate/allocate files (anti-allocate)");
  fprintf(stderr, "%-15s %s\n", "-T",
    "Convert foreign filenames to printable text");
  fprintf(stderr, "%-15s %s\n", "-X command",
    "Run command upon download completion (\"user exit\")");
  fprintf(stderr, "%-15s %s\n", "-d", "Daemon mode (fork to background)");
  fprintf(stderr, "%-15s %s\n", "-dd", "Daemon mode with I/O redirection");

  fprintf(stderr, "\nMake metainfo (torrent) file options:\n");
  fprintf(stderr, "%-15s %s\n", "-t", "Create a new torrent file");
  fprintf(stderr, "%-15s %s\n", "-u URL", "Tracker's announce URL(s)");
  fprintf(stderr, "%-15s %s %d)\n", "-l piece_len",
    "Piece length (default", (int)arg_piece_length);
  fprintf(stderr, "%-15s %s\n", "-s filename", "Specify metainfo file name");
  fprintf(stderr, "%-15s %s\n", "-p", "Private (disable peer exchange)");
  fprintf(stderr, "%-15s %s\n", "-c comment", "Include a comment/description");

  fprintf(stderr, "\nExample:\n");
  fprintf(stderr,
    "ctorrent -s new_filename -e 12 -C 32 -p 6881 example.torrent\n");
  fprintf(stderr, "\nhome page: http://ctorrent.sourceforge.net/\n");
  fprintf(stderr, "see also: http://www.rahul.net/dholmes/ctorrent/\n");
  fprintf(stderr, "bug report: %s\n", PACKAGE_BUGREPORT);
  fprintf(stderr, "original author: bsdi@sina.com\n\n");
}


void Exit(int status)
{
  CONSOLE.Shutdown();
  exit(status);
}

