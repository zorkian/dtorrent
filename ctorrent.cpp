#include "./def.h"
#include <sys/types.h>

#ifdef WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "ctorrent.h"
#include "btconfig.h"
#include "btcontent.h"
#include "downloader.h"
#include "peerlist.h"
#include "tracker.h"
#include "ctcs.h"

#include "./config.h"

#ifndef WINDOWS
#include "sigint.h"
#endif

void usage();
int param_check(int argc, char **argv);

#ifdef WINDOWS

int APIENTRY WinMain(HINSTANCE hInstance,
                     HINSTANCE hPrzevInstance,
                     LPSTR     lpCmdLine,
                     int       nCmdShow)
{
}

#else

void Random_init()
{
  struct timeval tv; 
  gettimeofday(&tv,(struct timezone*) 0);
  unsigned int seed = tv.tv_usec + tv.tv_sec + getpid();
  srandom(seed);
}

int main(int argc, char **argv)
{

  Random_init();
  arg_user_agent = new char[MAX_PF_LEN];
  strcpy(arg_user_agent,PEER_PFX);
    
  if( argc < 2 || param_check(argc,argv) < 0 ){
    usage();
    exit(1);
  }

  if( arg_flg_make_torrent ){
    if( !arg_announce ){ fprintf(stderr,"please use -u to specify a announce url!\n"); exit(1);}
    if( !arg_save_as ){ fprintf(stderr,"please use -s to specify a metainfo file name!\n"); exit(1);}
    if( BTCONTENT.InitialFromFS(arg_metainfo_file, arg_announce,  arg_piece_length) < 0 ||
        BTCONTENT.CreateMetainfoFile(arg_save_as) < 0){
      fprintf(stderr,"create metainfo failed.\n");
      exit(1);
    }
    printf("create metainfo file %s successful.\n", arg_save_as);
    exit(0);
  }
  
  if( BTCONTENT.InitialFromMI(arg_metainfo_file, arg_save_as) < 0){
    fprintf(stderr,"error,initial meta info failed.\n");
    exit(1);
  }

  if( !arg_flg_exam_only && !arg_flg_check_only){
    if(WORLD.Initial_ListenPort() < 0){
      fprintf(stderr,"warn, you couldn't accept connection.\n");
    }

    if( arg_ctcs ) CTCS.Initial();
        Tracker.Initial();

        signal(SIGPIPE,SIG_IGN);
    signal(SIGINT,sig_catch);
    signal(SIGTERM,sig_catch);
    Downloader();
  }
  if( cfg_cache_size ) BTCONTENT.FlushCache();
  if( arg_bitfield_file ) BTCONTENT.pBF->WriteToFile(arg_bitfield_file);
  WORLD.CloseAll();

  exit(0);
}

#endif

int param_check(int argc, char **argv)
{
  int c, l;
  char *s;
  while ( ( c = getopt(argc,argv,"b:cC:D:e:E:fi:l:M:m:n:P:p:s:S:tu:U:vxz:hH")) != -1)
    switch( c ){
    case 'b':
      arg_bitfield_file = new char[strlen(optarg) + 1];
#ifndef WINDOWS
      if( !arg_bitfield_file ) return -1;
#endif
      strcpy(arg_bitfield_file, optarg);
      break;

    case 'i':                  // listen on ip XXXX
      cfg_listen_ip = inet_addr(optarg);
      break;

    case 'p':			// listen on Port XXXX
      cfg_listen_port = atoi(optarg);
      break;

    case 's':			// Save as FILE/DIR NAME
      if( arg_save_as ) return -1;
      arg_save_as = new char[strlen(optarg) + 1];
#ifndef WINDOWS
      if( !arg_save_as ) return -1;
#endif
      strcpy(arg_save_as,optarg);
      break;

    case 'e':			// Exit while complete
      cfg_seed_hours = atoi(optarg);
      break;

    case 'E':			// target seed ratio
      cfg_seed_ratio = atof(optarg);
      break;

    case 'c':			// Check exist only
      arg_flg_check_only = 1;
      break;

    case 'C':
      cfg_cache_size = atoi(optarg);
      break;
      
    case 'M':			// Max peers
      cfg_max_peers = atoi(optarg);
      if( cfg_max_peers > 1000 ||
          cfg_max_peers < 20){
        return -1;
      }
      break;
      
    case 'm':			// Min peers
      cfg_min_peers = atoi(optarg);
      if( cfg_min_peers > 1000 ||
          cfg_min_peers < 1){
        return -1;
      }
      break;

    case 'z':			// slice size
      cfg_req_slice_size = atoi(optarg) * 1024;
      if( cfg_req_slice_size < 1024 || cfg_req_slice_size > 128*1024 )
        return -1;
      break;

    case 'n':                  // Which file download
      arg_file_to_download = atoi(optarg);
    break;


    case 'f':			// force seed mode, skip sha1 check when startup.
      arg_flg_force_seed_mode = 1;
      break;
      
    case 'D':
      cfg_max_bandwidth_down = (int)(strtod(optarg, NULL) * 1024);
      break;

    case 'U':
      cfg_max_bandwidth_up = (int)(strtod(optarg, NULL) * 1024);
      break;

    case 'P':
      l = strlen(optarg);
      if (l > MAX_PF_LEN) {printf("-P arg must be 8 or less characters\n"); exit(1);}
      if (l == 1 && *optarg == '-') *arg_user_agent = (char) 0;
      else strcpy(arg_user_agent,optarg);
      break;

     // BELLOW OPTIONS USED FOR CREATE TORRENT.
    case 'u':			// Announce url
      if( arg_announce ) return -1;
      arg_announce = new char[strlen(optarg) + 1];
      strcpy(arg_announce, optarg);
      break;

    case 't':			// make Torrent
      arg_flg_make_torrent = 1;
      break;

    case 'l':			// piece Length (default 262144)
      arg_piece_length = atoi(optarg);
      if( arg_piece_length < 65536 ||
          arg_piece_length > 1310720 ){
        // warn message:
        // piece length range is 65536 =>> 1310720
        return -1;
      }
      break;
     // ABOVE OPTIONS USED FOR CREATE TORRENT.

    case 'x':
      arg_flg_exam_only = 1;
      break;

    case 'S':			// CTCS server
      if( arg_ctcs ) return -1;
      arg_ctcs = new char[strlen(optarg) + 1];
      if( !strchr(optarg, ':') ) return -1;
      strcpy(arg_ctcs, optarg);
      break;

    case 'v':
      arg_verbose = 1;
      break;

    case 'h':
    case 'H':
    default:
      //unknow option.
      return -1;
    }

  argc -= optind; argv += optind;
  if( cfg_min_peers >= cfg_max_peers ) cfg_min_peers = cfg_max_peers - 1;
  if( argc != 1 ) return -1;
  arg_metainfo_file = new char[strlen(*argv) + 1];
  
#ifndef WINDOWS
  if( !arg_metainfo_file ) return -1;
#endif
  strcpy(arg_metainfo_file, *argv);
  return 0;
}

void usage()
{
  fprintf(stderr,"%s	Original code Copyright: YuHong(992126018601033)",PACKAGE_STRING);
  fprintf(stderr,"\nWARNING: THERE IS NO WARRANTY FOR CTorrent. USE AT YOUR OWN RISK!!!\n");
  fprintf(stderr,"\nGeneral Options:\n");
  fprintf(stderr, "%-15s %s\n", "-h/-H", "Show this message.");
  fprintf(stderr, "%-15s %s\n", "-x",
    "Decode metainfo (torrent) file only, don't download.");
  fprintf(stderr, "%-15s %s\n", "-c", "Check pieces only, don't download.");
  fprintf(stderr, "%-15s %s\n", "-v", "Verbose output (for debugging).");

  fprintf(stderr,"\nDownload Options:\n");
  fprintf(stderr, "%-15s %s\n", "-e int",
    "Exit while seed <int> hours later. (default 72 hours)");
  fprintf(stderr, "%-15s %s\n", "-E num",
    "Exit after seeding to <num> ratio (UL:DL).");
  fprintf(stderr, "%-15s %s\n", "-i ip",
    "Listen for connections on ip. (default all IP's)");
  fprintf(stderr, "%-15s %s\n", "-p port",
    "Listen port. (default 2706 -> 2106)");
  fprintf(stderr, "%-15s %s\n", "-s filename",
    "Download (\"save as\") to a different file or directory.");
  fprintf(stderr, "%-15s %s\n", "-C cache_size",
    "Cache size, unit MB. (default 16MB)");
  fprintf(stderr, "%-15s %s\n", "-f",
    "Force seed mode (skip hash check at startup).");
  fprintf(stderr, "%-15s %s\n", "-b filename",
    "Bitfield filename. (use it carefully)");
  fprintf(stderr, "%-15s %s\n", "-M max_peers",
    "Max peers count. (default 100)");
  fprintf(stderr, "%-15s %s\n", "-m min_peers", "Min peers count. (default 1)");
  fprintf(stderr, "%-15s %s\n", "-z slice_size",
    "Download slice/block size, unit KB. (default 16, max 128).");
  fprintf(stderr, "%-15s %s\n", "-n file_number", "Specify file to download.");
  fprintf(stderr, "%-15s %s\n", "-D rate", "Max bandwidth down (unit KB/s)");
  fprintf(stderr, "%-15s %s\n", "-U rate", "Max bandwidth up (unit KB/s)");
  fprintf(stderr, "%-15s %s%s\")\n", "-P peer_id",
    "Set Peer ID prefix. (default \"", PEER_PFX);
  fprintf(stderr, "%-15s %s\n", "-S host:port",
    "Use CTCS server at host:port.");

  fprintf(stderr,"\nMake metainfo (torrent) file options:\n");
  fprintf(stderr, "%-15s %s\n", "-t", "Create a new torrent file.");
  fprintf(stderr, "%-15s %s\n", "-u url", "Tracker's url.");
  fprintf(stderr, "%-15s %s\n", "-l piece_len",
    "Piece length. (default 262144)");
  fprintf(stderr, "%-15s %s\n", "-s filename", "Specify metainfo file name.");

  fprintf(stderr,"\nExample:\n");
  fprintf(stderr,"ctorrent -s new_filename -e 12 -C 32 -p 6881 example.torrent\n");
  fprintf(stderr,"\nhome page: http://ctorrent.sourceforge.net/\n");
  fprintf(stderr,"see also: http://www.rahul.net/dholmes/ctorrent/\n");
  fprintf(stderr,"bug report: %s\n",PACKAGE_BUGREPORT);
  fprintf(stderr,"original author: bsdi@sina.com\n\n");
}

/* "sev" indicates the severity of the message.
   0: will be printed but not sent to CTCS
   1: extremely urgent/important
   2: less important
   3: no problem
*/
void warning (int sev, const char *message)
{
  fprintf(stderr, "%s\n", message);
  if(sev && arg_ctcs) CTCS.Send_Info(sev, message);
}

