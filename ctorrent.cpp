#include "def.h"
#include <sys/types.h>

#ifdef WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

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
int param_check(int argc, char **argv);


bt_length_t arg_piece_length = 256 * 1024;
char *arg_save_as = (char *)0;
bool arg_flg_make_torrent = false;
char *arg_metainfo_file = (char *)0;  // will be owned by BTCONTENT
char *arg_announce = (char *)0;
char *arg_comment = (char *)0;
bool arg_flg_private = false;

// temporary config values
bool arg_daemon = *cfg_daemon;


int main(int argc, char **argv)
{
  RandomInit();
  CONSOLE.Init();
  InitConfig();

  if( argc < 2 ){
    usage();
    exit(1);
  }else if( param_check(argc, argv) < 0 ){
    exit(1);
  }

  if( *cfg_verbose ) CONFIG.Dump();

  if( arg_flg_make_torrent ){
    if( !arg_announce ){
      CONSOLE.Warning(1, "Please use -u to specify an announce URL!");
      exit(1);
    }
    if( !arg_save_as ){
      CONSOLE.Warning(1, "Please use -s to specify a metainfo file name!");
      exit(1);
    }
    if( BTCONTENT.InitialFromFS(arg_metainfo_file, arg_announce,
                                arg_piece_length) < 0 ||
        BTCONTENT.CreateMetainfoFile(arg_save_as, arg_comment, arg_flg_private)
          < 0 ){
      CONSOLE.Warning(1, "create metainfo failed.");
      exit(1);
    }
    CONSOLE.Print("Create metainfo file %s successful.", arg_save_as);
    exit(0);
  }

  cfg_daemon = arg_daemon;  // triggers action

  if( BTCONTENT.InitialFromMI(arg_metainfo_file, arg_save_as, arg_announce)
        < 0 ){
    CONSOLE.Warning(1, "error, initial meta info failed.");
    exit(1);
  }

  if( !arg_flg_exam_only && (!arg_flg_check_only || arg_flg_force_seed_mode) ){
    if( WORLD.Init() < 0 ){
      CONSOLE.Warning(2, "warn, you can't accept connections.");
    }

    if( Tracker.Initial() < 0 ){
      CONSOLE.Warning(1, "error, tracker setup failed.");
      exit(1);
    }

    sig_setup();  // setup signal handling
    CONSOLE.Interact(
      "Press 'h' or '?' for help (display/control client options)." );

    Downloader();
    WORLD.CloseAll();
    if( *cfg_cache_size ) BTCONTENT.FlushCache();
    if( BTCONTENT.NeedMerge() ){
      CONSOLE.Interact_n("");
      CONSOLE.Interact_n("Merging staged data");
      BTCONTENT.MergeAll();
    }
  }
  if( !arg_flg_exam_only ) BTCONTENT.SaveBitfield();

  if(*cfg_verbose) CONSOLE.cpu();
  exit(0);
}


int param_check(int argc, char **argv)
{
  const char *opts;
  int c;
  size_t l;

  if( 0==strncmp(argv[1], "-t", 2) )
    opts = "tc:l:ps:u:";
  else opts = "aA:b:cC:dD:e:E:fi:I:M:m:n:P:p:s:S:Tu:U:vxX:z:hH";

  while( (c=getopt(argc, argv, opts)) != -1 )
    switch( c ){
    case 'a':  // change allocation mode
      cfg_allocate++;
      break;

    case 'b':  // set bitfield file
      if( *cfg_bitfield_file ) return -1;  // specified twice
      cfg_bitfield_file = optarg;
      break;

    case 'i':  // listen on given IP address
      cfg_listen_addr = optarg;
      break;

    case 'I':  // set public IP address
      cfg_public_ip = optarg;
      break;

    case 'p':  // listen on given port
      if( arg_flg_make_torrent ) arg_flg_private = true;
      else cfg_listen_port.Scan(optarg);
      break;

    case 's':  // save as given file/dir name
      if( arg_save_as ) return -1;  // specified twice
      arg_save_as = new char[strlen(optarg) + 1];
#ifndef WINDOWS
      if( !arg_save_as ) return -1;
#endif
      strcpy(arg_save_as, optarg);
      break;

    case 'e':  // seed timeout (exit time)
      cfg_seed_hours = strtoul(optarg, NULL, 10);
      break;

    case 'E':  // target seed ratio
      cfg_seed_ratio = atof(optarg);
      break;

    case 'c':  // check piece hashes only
      if( arg_flg_make_torrent ){
        arg_comment = new char[strlen(optarg) + 1];
        if( !arg_comment ) return -1;
        strcpy(arg_comment, optarg);
      }else arg_flg_check_only = true;
      break;

    case 'C':  // max cache size
      cfg_cache_size = atoi(optarg);
      break;

    case 'M':  // max peers
      if( cfg_max_peers.Valid(atoi(optarg)) )
        cfg_max_peers = atoi(optarg);
      else{
        CONSOLE.Warning(1, "-%c argument must be between %s and %s", c,
          cfg_max_peers.Smin(), cfg_max_peers.Smax());
        return -1;
      }
      break;

    case 'm':  // min peers
      if( cfg_min_peers.Valid(atoi(optarg)) )
        cfg_min_peers = atoi(optarg);
      else{
        CONSOLE.Warning(1, "-%c argument must be between %s and %s", c,
          cfg_min_peers.Smin(), cfg_min_peers.Smax());
        return -1;
      }
      break;

    case 'z':  // slice size for requests
      if( cfg_req_slice_size.Valid(atoi(optarg) * 1024) )
        cfg_req_slice_size = atoi(optarg) * 1024;
      else{
        CONSOLE.Warning(1, "-%c argument must be between %d and %d", c,
          cfg_req_slice_size.Min() / 1024, cfg_req_slice_size.Max() / 1024);
        return -1;
      }
      break;

    case 'n':  // file download list
      if( *cfg_file_to_download ) return -1;  // specified twice
      cfg_file_to_download = optarg;
      break;

    case 'f':  // force bitfield accuracy or seeding, skip initial hash check
      arg_flg_force_seed_mode = true;
      break;

    case 'D':  // download bandwidth limit
      cfg_max_bandwidth_down = (dt_rate_t)(strtod(optarg, NULL) * 1024);
      break;

    case 'U':  // upload bandwidth limit
      cfg_max_bandwidth_up = (dt_rate_t)(strtod(optarg, NULL) * 1024);
      break;

    case 'P':  // peer ID prefix
      l = strlen(optarg);
      if( l > cfg_peer_prefix.MaxLen() ){
        CONSOLE.Warning(1, "-P arg must be %d or less characters",
          cfg_peer_prefix.MaxLen());
        return -1;
      }
      if( l == 1 && *optarg == '-' ) cfg_peer_prefix = "";
      else cfg_peer_prefix = optarg;
      break;

    case 'A':  // HTTP user-agent header string
      cfg_user_agent = optarg;
      break;

    case 'T':  // convert foreign filenames to printable text
      cfg_convert_filenames = true;
      break;

     // BELOW OPTIONS USED FOR CREATE TORRENT.
    case 'u':  // tracker announce URL
      if( arg_announce ) return -1;  // specified twice
      arg_announce = new char[strlen(optarg) + 1];
#ifndef WINDOWS
      if( !arg_announce ) return -1;
#endif
      strcpy(arg_announce, optarg);
      break;

    case 't':  // make torrent
      arg_flg_make_torrent = true;
      CONSOLE.ChangeChannel(O_INPUT, "off", 0);
      break;

    case 'l':  // piece length
      arg_piece_length = atol(optarg);
      if( arg_piece_length < 65536 || arg_piece_length > 4096*1024 ){
        CONSOLE.Warning(1, "-%c argument must be between 65536 and %d",
          c, 4096*1024);
        return -1;
      }
      break;
     // ABOVE OPTIONS USED FOR CREATE TORRENT.

    case 'x':  // print torrent information only
      arg_flg_exam_only = true;
      CONSOLE.ChangeChannel(O_INPUT, "off", 0);
      break;

    case 'S':  // CTCS server
      if( *cfg_ctcs ) return -1;  // specified twice
      if( !cfg_ctcs.Valid(optarg) ){
        CONSOLE.Warning(1, "-%c argument must be in the format host:port", c);
        return -1;
      }
      cfg_ctcs = optarg;
      break;

    case 'X':  // "user exit" on download completion
      if( *cfg_completion_exit ) return -1;  // specified twice
#ifndef HAVE_SYSTEM
      CONSOLE.Warning(1, "-X is not supported on your system");
      return -1;
#endif
#ifndef HAVE_WORKING_FORK
      CONSOLE.Warning(2,
        "No working fork function; be sure the -X command is brief!");
#endif
      cfg_completion_exit = optarg;
      break;

    case 'v':  // verbose output
      cfg_verbose = true;
      break;

    case 'd':  // daemon mode (fork to background)
      if( arg_daemon ) cfg_redirect_io = true;
      else arg_daemon = true;
      break;

    case 'h':
    case 'H':  // help
      usage();
      return -1;

    default:
      // unknown option.
      CONSOLE.Warning(1, "Use -h for help/usage.");
      return -1;
    }

  argc -= optind;
  argv += optind;
  if( argc != 1 ){
    if( arg_flg_make_torrent )
      CONSOLE.Warning(1,
        "Must specify torrent contents (one file or directory)");
    else CONSOLE.Warning(1, "Must specify one torrent file");
    return -1;
  }
  arg_metainfo_file = new char[strlen(*argv) + 1];
#ifndef WINDOWS
  if( !arg_metainfo_file ) return -1;
#endif
  strcpy(arg_metainfo_file, *argv);

  char *tmp = new char[strlen(arg_metainfo_file) + 4];
  if( tmp ){
    strcpy(tmp, arg_metainfo_file);
    strcat(tmp, ".bf");
    cfg_bitfield_file.SetDefault(tmp);
    delete []tmp;
    if( !*cfg_bitfield_file ) cfg_bitfield_file.Reset();
  }

  return 0;
}


void usage()
{
  CONSOLE.ChangeChannel(O_INPUT, "off", 0);
  fprintf(stderr, "%s   Original code Copyright: YuHong(992126018601033)\n",
    PACKAGE_STRING);
  fprintf(stderr,
    "WARNING: THERE IS NO WARRANTY FOR CTorrent. USE AT YOUR OWN RISK!!!\n");
  fprintf(stderr, "\nGeneral Options:\n");
  fprintf(stderr, "%-15s %s\n", "-h/-H", "Show this message");
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
  fprintf(stderr, "%-15s %s\n", "-u num or URL",
    "Use an alternate announce (tracker) URL");
  fprintf(stderr, "%-15s %s\n", "-s filename",
    "Download (\"save as\") to a different file or directory");
  fprintf(stderr, "%-15s %s %sMB)\n", "-C cache_size",
    "Cache size, unit MB (default", cfg_cache_size.Sdefault());
  fprintf(stderr, "%-15s %s\n", "-f",
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
  fprintf(stderr, "%-15s %s\n", "-u URL", "Tracker's URL");
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

