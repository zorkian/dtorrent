#include <sys/types.h>
#include <string.h>

#include "btconfig.h"
#include "bttime.h"
#include "btcontent.h"
#include "peerlist.h"
#include "ctcs.h"

// btconfig.cpp:  Copyright 2008 Dennis Holmes  (dholmes@rahul.net)

#define MAX_PEER_PREFIX_LEN 8
#define DEFAULT_PEER_PREFIX "-CD0303-"


Configuration CONFIG;


//===========================================================================
// ConfigGen class functions

ConfigGen::ConfigGen()
{
  m_tag = m_desc = m_info = (char *)0;
  m_svalbuf = m_sminbuf = m_smaxbuf = m_sdefbuf = (char *)0;
  m_maxlen = 0;
  m_hidden = m_locked = false;
}


ConfigGen::~ConfigGen()
{
  if( m_tag ){
    delete []m_tag;
    m_tag = (char *)0;
  }
  if( m_desc ){
    delete []m_desc;
    m_desc = (char *)0;
  }
  if( m_info ){
    delete []m_info;
    m_info = (char *)0;
  }
  if( m_svalbuf ){
    delete []m_svalbuf;
    m_svalbuf = (char *)0;
  }
  if( m_sminbuf ){
    delete []m_sminbuf;
    m_sminbuf = (char *)0;
  }
  if( m_smaxbuf ){
    delete []m_smaxbuf;
    m_smaxbuf = (char *)0;
  }
  if( m_sdefbuf ){
    delete []m_sdefbuf;
    m_sdefbuf = (char *)0;
  }
}

 
bool ConfigGen::Init(const char *desc, const char *info, size_t maxlen)
{
  if( m_desc ) delete []m_desc;
  if( (m_desc = new char[(desc ? strlen(desc) : 0) + 1]) ){
    if( desc ) strcpy(m_desc, desc);
    else *m_desc = '\0';
  }

  SetInfo(info);

  m_maxlen = maxlen;

  return true;
}


void ConfigGen::SetInfo(const char *info)
{
  if( m_info ) delete []m_info;
  if( (m_info = new char[(info ? strlen(info) : 0) + 1]) ){
    if( info ) strcpy(m_info, info);
    else *m_info = '\0';
  }
}


void ConfigGen::SetTag(const char *tag)
{
  if( m_tag ) delete []m_tag;
  if( (m_tag = new char[(tag ? strlen(tag) : 0) + 1]) ){
    if( tag ) strcpy(m_tag, tag);
    else *m_tag = '\0';
  }
}


//===========================================================================
// Config class functions

template<> Config<const char *>::Config(const char *init)
{
  char *tmp;

  m_current = m_default = (const char *)0;

  if( init && (tmp = new char[strlen(init) + 1]) ){
    strcpy(tmp, init);
    m_current = tmp;

    if( (tmp = new char[strlen(init) + 1]) ){
      strcpy(tmp, init);
      m_default = tmp;
    }
  }

  m_minval = m_maxval = (const char *)0;
  m_handler = (void (*)(Config<const char *> *))0;
  m_validator = (bool (*)(const Config<const char *> *, const char *))0;
  m_infomaker = (void (*)(Config<const char *> *))0;
}


template<> Config<const char *>::~Config()
{
  if( m_current ){
    delete []m_current;
    m_current = (const char *)0;
  }
  if( m_default ){
    delete []m_default;
    m_default = (const char *)0;
  }
} 


template<> dt_config_t Config<bool>::Type() const { return DT_CONFIG_BOOL; }
template<> dt_config_t Config<double>::Type() const { return DT_CONFIG_FLOAT; }
template<> dt_config_t Config<float>::Type() const { return DT_CONFIG_FLOAT; }
template<> dt_config_t Config<const char *>::Type() const
{
  return DT_CONFIG_STRING;
}


template<> void Config<const char *>::Override(const char *newval)
{
  char *tmp;

  if( m_current == newval )
    return;
  if( m_current && newval && 0==strcmp(newval, m_current) )
    return;

  if( m_current ){
    delete []m_current;
    m_current = (const char *)0;
  }
  if( !newval ) return;

  if( (tmp = new char[strlen(newval) + 1]) ){
    strcpy(tmp, newval);
    m_current = tmp;
  }
  return;
}


template<> void Config<const char *>::SetMin(const char *newmin){ DoNothing(); }

template<> void Config<const char *>::SetMax(const char *newmax){ DoNothing(); }

template<> void Config<const char *>::SetDefault(const char *newdef)
{
  if( m_default ){
    delete []m_default;
    m_default = (char *)0;
  }
  if( newdef ){
    char *tmp;
    if( (tmp = new char[strlen(newdef) + 1]) ){
      strcpy(tmp, newdef);
      m_default = tmp;
    }
  }
}


template<> bool Config<const char *>::Set(const char *newval)
{ 
  if( m_current == newval )
    return true;
  if( m_current && newval && 0==strcmp(newval, m_current) )
    return true;

  if( !Locked() && Valid(newval) ){
    size_t len;
    char *tmp;
    if( m_current ){
      delete []m_current;
      m_current = (const char *)0;
    }
    if( newval ){
      len = strlen(newval);
      if( MaxLen() && len > MaxLen() ) len = MaxLen();
      if( !(tmp = new char[len + 1]) ) return false;
      strncpy(tmp, newval, len);
      tmp[len] = '\0';
      m_current = tmp;
    }
    Handler();
    if(*cfg_verbose)
      CONSOLE.Debug("Config: %s = \"%s\"", Tag(), m_current ? m_current : "");
    return true;
  }
  return false;
}


template<> bool Config<bool>::Set(bool newval)
{
  if( newval == m_current )
    return true;
  if( !Locked() && Valid(newval) ){
    m_current = newval;
    Handler();
    if(*cfg_verbose) CONSOLE.Debug("Config: %s = %s", Tag(), Sval());
    return true;
  }
  return false;
}


template<> bool Config<const char *>::Scan(const char *valstr)
{
  return Set(valstr);
}


template<> bool Config<bool>::Scan(const char *valstr)
{
  if( !valstr || !*valstr || *valstr == '0' ||
      *valstr == 'f' || *valstr == 'F' ||
      *valstr == 'n' || *valstr == 'N' ){
    return Set(false);
  }else return Set(true);
}


template<> bool Config<double>::Scan(const char *valstr)
{
  return Set(atof(valstr));
}


template<> const char *Config<const char *>::Sprint(char ** buffer,
  const char *value)
{
  const char *val = value;

  if( !value ) val = "";

  if( *buffer ){
    if( 0==strcmp(val, *buffer) ) return *buffer;
    else delete []*buffer;
  }
  if( (*buffer = new char[strlen(val) + 1]) )
    strcpy(*buffer, val);
  return *buffer ? *buffer : "Out of memory";
}


template<> const char *Config<bool>::Sprint(char **buffer, bool value)
{
  if( !*buffer ){
    if( (*buffer = new char[2]) )
      (*buffer)[1] = '\0';
  }
  if( *buffer ) (*buffer)[0] = value ? '1' : '0';
  return *buffer ? *buffer : "Out of memory";
}


template<> const char *Config<double>::Sprint(char **buffer, double value)
{
  if( !*buffer ){
    *buffer = new char[32];
  }
  if( *buffer ) snprintf(*buffer, 32, "%.2f", value);
  return *buffer ? *buffer : "Out of memory";
}


// Mathematical operators don't make sense for string Configs.

template<> const char *Config<const char *>::operator++(){ return m_current; }
template<> const char *Config<const char *>::operator++(int){return m_current;}
template<> const char *Config<const char *>::operator--(){ return m_current; }
template<> const char *Config<const char *>::operator--(int){return m_current;}

template<> const char *Config<const char *>::operator+=(int arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator-=(int arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator*=(int arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator/=(int arg)
{
  return m_current;
}

template<> const char *Config<const char *>::operator+=(double arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator-=(double arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator*=(double arg)
{
  return m_current;
}
template<> const char *Config<const char *>::operator/=(double arg)
{
  return m_current;
}


//===========================================================================
// Configuration class functions

Configuration::~Configuration()
{
  confignode *p;

  while( m_head ){
    p = m_head;
    m_head = m_head->next;
    delete p;
  }
}


bool Configuration::Add(const char *tag, ConfigGen &config)
{
  confignode *p;

  if( tag && *tag ){
    if( (p = FindNode(tag)) ){
      if( 0 != strcmp(p->item->Desc(), config.Desc()) )
        return false;  // description matches--already exists
      else{  // different description--add with "_1" appended to the tag
        char *newtag;
        bool retval;
        if( !(newtag = new char[strlen(tag) + 3]) ) return false;
        strcpy(newtag, tag);
        strcat(newtag, "_1");
        retval = Add(newtag, config);
        delete []newtag;
        return retval;
      }
    }else{
      const char *dot = strchr(tag, '.'), *item_dot;
      size_t len;
      int res;
      confignode *pp = (confignode *)0;

      for( p = m_head; p; pp = p, p = p->next ){
        if( !dot ){
          if( strchr(p->item->Tag(), '.') || strcmp(tag, p->item->Tag()) < 0 )
            break;
          else continue;
        }else{
          item_dot = strchr(p->item->Tag(), '.');
          if( !item_dot ) continue; 
          len = (dot - tag > item_dot - p->item->Tag()) ?
                 dot - tag : item_dot - p->item->Tag();
          if( (res = strncmp(tag, p->item->Tag(), len)) > 0 ) continue;
          else if( res == 0 ){
            // found category, now compare rest of the tag
            if( strcmp(dot, item_dot) > 0 ) continue;
            else break;
          }else break;
        }
      }

      // insert before p (after pp)
      if( !(p = new confignode) ) return false;
      config.SetTag(tag);
      p->item = &config;
      if( pp ){
        p->next = pp->next;
        pp->next = p;
      }else{
        p->next = m_head;
        m_head = p;
      }
      return true;
    }
  }
  return false;
}


confignode *Configuration::FindNode(const char *tag) const
{
  confignode *p;

  for( p = m_head; p; p = p->next ){
    if( p->item->Match(tag) ) break;
  }
  return p;
}


ConfigGen *Configuration::Find(const char *tag) const
{
  confignode *p;

  p = FindNode(tag);
  return p ? p->item : (ConfigGen *)0;
}


bool Configuration::Set(const char *tag, const char *value)
{
  ConfigGen *p;

  p = Find(tag);
  return p ? p->Scan(value) : false;
}


const char *Configuration::Get(const char *tag)
{
  ConfigGen *p;

  p = Find(tag);
  return p ? p->Sval() : (char *)0;
}


ConfigGen *Configuration::First() const
{
  m_iter = m_head;
  return m_iter ? m_iter->item : (ConfigGen *)0;
}


ConfigGen *Configuration::Next(const ConfigGen *current) const
{
  if( m_iter ){
    if( current != m_iter->item )
      m_iter = FindNode(current->Tag());
    m_iter = m_iter->next;
  }
  return m_iter ? m_iter->item : (ConfigGen *)0;
}


void Configuration::Dump() const
{
  if( !*cfg_verbose ) return;

  CONSOLE.Debug("CONFIGURATION DUMP");
  for( ConfigGen *config = First();
       config;
       config = Next(config) ){
    CONSOLE.Debug("  %s:  %s", config->Tag(), config->Sval());
  }
}


//===========================================================================
// Global flags

bool g_secondary_process = false;

bool arg_flg_force_seed_mode = false;
bool arg_flg_check_only = false;
bool arg_flg_exam_only = false;


//===========================================================================
// Global Configs

Config<unsigned int> cfg_cache_size = 16;

static void CfgCacheSize(Config<unsigned int> *config)
{
  BTCONTENT.CacheConfigure();
}

static void InfoCfgCacheSize(Config<unsigned int> *config)
{
  char info[32];
  snprintf(info, 32, "MB; %dKB now in use", (int)(BTCONTENT.CacheUsed()/1024));
  config->SetInfo(info);
}

//---------------------------------------------------------------------------

Config<dt_count_t> cfg_max_peers = 100;
Config<dt_count_t> cfg_min_peers = 1;

static void CfgMinPeers(Config<dt_count_t> *config)
{
  if( *cfg_min_peers > *cfg_max_peers )
    cfg_max_peers = *cfg_min_peers;
}

static void CfgMaxPeers(Config<dt_count_t> *config)
{
  if( *cfg_max_peers < *cfg_min_peers )
    cfg_min_peers = *cfg_max_peers;
}

static void InfoCfgPeers(Config<dt_count_t> *config)
{
  char info[32];   
  snprintf(info, 32, "Current peers: %d", (int)WORLD.GetPeersCount());
  config->SetInfo(info);
}

//---------------------------------------------------------------------------

Config<const char *> cfg_channel_normal = "stdout";
Config<const char *> cfg_channel_interact = "stdout";
Config<const char *> cfg_channel_error = "stderr";
Config<const char *> cfg_channel_debug = "stderr";
Config<const char *> cfg_channel_input = "stdin";

static void CfgChannel(Config<const char *> *config)
{
  if( strstr(config->Tag(), "normal") )
    CONSOLE.ChangeChannel(O_NORMAL, config->Value());
  else if( strstr(config->Tag(), "interact") )
    CONSOLE.ChangeChannel(O_INTERACT, config->Value());
  else if( strstr(config->Tag(), "error") )
    CONSOLE.ChangeChannel(O_WARNING, config->Value());
  else if( strstr(config->Tag(), "debug") )
    CONSOLE.ChangeChannel(O_DEBUG, config->Value());
  else if( strstr(config->Tag(), "input") )
    CONSOLE.ChangeChannel(O_INPUT, config->Value());
}

static bool ValCfgChannel(const Config<const char *> *config, const char *value)
{
  return (value && *value);
}

//---------------------------------------------------------------------------

Config<dt_rate_t> cfg_max_bandwidth_down = 0;
Config<dt_rate_t> cfg_max_bw_down_k = 0;
Config<dt_rate_t> cfg_max_bandwidth_up = 0;
Config<dt_rate_t> cfg_max_bw_up_k = 0;

static void CfgMaxBWDown(Config<dt_rate_t> *config)
{
  cfg_max_bw_down_k.Override(*cfg_max_bandwidth_down / 1024);
  if( *cfg_ctcs ) CTCS.Send_bw();
}

static void CfgMaxBWDownK(Config<dt_rate_t> *config)
{
  cfg_max_bandwidth_down = *cfg_max_bw_down_k * 1024;
}

static void CfgMaxBWUp(Config<dt_rate_t> *config)
{
  cfg_max_bw_up_k.Override(*cfg_max_bandwidth_up / 1024);
  if( *cfg_ctcs ) CTCS.Send_bw();
}
 
static void CfgMaxBWUpK(Config<dt_rate_t> *config)
{
  cfg_max_bandwidth_up = *cfg_max_bw_up_k * 1024;
}
    
//---------------------------------------------------------------------------

Config<time_t> cfg_seed_time;

static void CfgSeedTime(Config<time_t> *config)
{
  cfg_seed_remain.Override( (*cfg_seed_time -
                                   (now - BTCONTENT.GetSeedTime())) / 3600.0 );
  cfg_seed_hours.Override(*cfg_seed_time / 3600);
}

static bool ValCfgSeedTime(const Config<time_t> *config, time_t value)
{
  if( value < 0 ) return false;

  // Don't allow a value that would cause an immediate exit.
  if( 0==BTCONTENT.GetSeedTime() || value > now - BTCONTENT.GetSeedTime() )
    return true;
  if( 0==value &&
      *cfg_seed_ratio > (double)Self.TotalUL() /
       (Self.TotalDL() ?
        Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) ){
    return true;
  }
  return false;
}

//---------------------------------------------------------------------------

Config<uint32_t> cfg_seed_hours = 72;

static void CfgSeedHours(Config<uint32_t> *config)
{
  cfg_seed_time = *cfg_seed_hours * 3600;
  cfg_seed_hours.Override(*cfg_seed_time / 3600);
}

static bool ValCfgSeedHours(const Config<uint32_t> *config, uint32_t value)
{
  return ValCfgSeedTime(&cfg_seed_time, value * 3600);
}

//---------------------------------------------------------------------------

Config<double> cfg_seed_remain;

static void CfgSeedRemain(Config<double> *config)
{
  cfg_seed_time = (time_t)(*cfg_seed_remain * 3600) +
    (BTCONTENT.GetSeedTime() ? (now - BTCONTENT.GetSeedTime()) : 0);

  // If cfg_seed_time didn't actually change, cfg_seed_remain wasn't adjusted.
  cfg_seed_remain.Override( (*cfg_seed_time -
                                   (now - BTCONTENT.GetSeedTime())) / 3600.0 );
}

static bool ValCfgSeedRemain(const Config<double> *config, double value)
{
  return (value >= .01);
}

//---------------------------------------------------------------------------

Config<double> cfg_seed_ratio = 0;
  
static bool ValCfgSeedRatio(const Config<double> *config, double value)
{
  if( value < 0 ) return false;

  // Don't allow a value that would cause an immediate exit.
  if( 0==BTCONTENT.GetSeedTime() ||
      value > (double)Self.TotalUL() /
        (Self.TotalDL() ?
         Self.TotalDL() : BTCONTENT.GetTotalFilesLength()) ){
    return true;
  }
  if( 0==value && *cfg_seed_time > now - BTCONTENT.GetSeedTime() )
    return true;

  return false;
}

//---------------------------------------------------------------------------

Config<const char *> cfg_peer_prefix = DEFAULT_PEER_PREFIX;

//---------------------------------------------------------------------------

Config<const char *> cfg_file_to_download;

static void CfgFileToDownload(Config<const char *> *config)
{
  BTCONTENT.SetFilter();
}

//---------------------------------------------------------------------------

Config<bool> cfg_verbose = false;

static void CfgVerbose(Config<bool> *config)
{
  bool tmp = *cfg_verbose;
  cfg_verbose.Override(true);
  CONSOLE.Debug("Verbose output %s", tmp ? "on" : "off");
  cfg_verbose.Override(tmp);
}

//---------------------------------------------------------------------------

Config <const char *> cfg_ctcs;

void CfgCTCS(Config<const char *> *config)
{
  char *s;

  if( !*cfg_ctcs || !**cfg_ctcs || 0==strcmp(":", *cfg_ctcs) ){
    if( *cfg_ctcs ) cfg_ctcs.Override((const char *)0);
  }else{
    strncpy(CTCS.m_host, *cfg_ctcs, MAXHOSTNAMELEN-1);
    CTCS.m_host[MAXHOSTNAMELEN-1] = '\0';
    if( (s = strchr(CTCS.m_host, ':')) ) *s='\0';
    CTCS.m_port = atoi(s = (strchr(*cfg_ctcs, ':')+1));
    if( strchr(s, ':') )
      CONSOLE.Input("Enter CTCS password: ", CTCS.m_pass, CTCS_PASS_SIZE);
    else *CTCS.m_pass = '\0';
  }
  CTCS.Reset(1);
}


static bool ValCfgCTCS(const Config<const char *> *config, const char *value)
{
  const char *s;
  if( !value || !*value ) return true;
  if( 0==strcmp(":", value) ) return true;
  return ( (s = strchr(value, ':')) > value && atoi(s+1) > 0 );
}

//---------------------------------------------------------------------------
 
Config <const char *> cfg_completion_exit;

//---------------------------------------------------------------------------

Config <bool> cfg_pause = false;

static void CfgPause(Config<bool> *config)
{
  if( *cfg_pause ){
    if( !WORLD.IsPaused() ) WORLD.Pause();
  }else if( WORLD.IsPaused() ) WORLD.Resume();   
}

//---------------------------------------------------------------------------

Config<const char *> cfg_user_agent = PACKAGE_NAME "/" PACKAGE_VERSION;

static void CfgUserAgent(Config<const char *> *config)
{
  char *s, *tmp_user_agent;

  if( strchr(*cfg_user_agent, ' ') ){
    if( (tmp_user_agent = new char[strlen(*cfg_user_agent) + 1]) ){
      strcpy(tmp_user_agent, *cfg_user_agent);
      while( (s = strchr(tmp_user_agent, ' ')) ) *s = '-';
      cfg_user_agent = tmp_user_agent;
      delete []tmp_user_agent;
    }
  }
}

static bool ValCfgUserAgent(const Config<const char *> *config,
  const char *value)
{
  return (value && *value);
}

//---------------------------------------------------------------------------

Config<const char *>cfg_bitfield_file;

//---------------------------------------------------------------------------

Config<bool> cfg_daemon = false;
Config<bool> cfg_redirect_io = false;

static void CfgDaemon(Config<bool> *config)
{
  if( *cfg_daemon ) CONSOLE.Daemonize();
  if( *cfg_daemon ){
    cfg_daemon.Lock();  // cannot un-daemonize
    cfg_redirect_io.Lock();
  }
  config->SetInfo(*cfg_daemon ? "Running in background" : "Use caution!");
}

//---------------------------------------------------------------------------

Config<unsigned char> cfg_allocate = 0;

static void CfgAllocate(Config<unsigned char> *config)
{
  const char *info;
  char tmp[32];

  switch( *cfg_allocate ){
    case DT_ALLOC_SPARSE:
      info = "Sparse (if supported)";
      break;
    case DT_ALLOC_FULL:
      info = "Full (preallocated)";
      break;
    case DT_ALLOC_NONE:
      info = "None (staged)";
      break;
    default:
      sprintf(tmp, "Unknown (%d)", (int)*cfg_allocate);
      info = tmp;
  }
  config->SetInfo(info);
}

//---------------------------------------------------------------------------

Config<const char *> cfg_public_ip;

//---------------------------------------------------------------------------

Config<in_addr_t> cfg_listen_ip = 0;
Config<const char *> cfg_listen_addr;

static void CfgListenAddr(Config<const char *> *config)
{
  cfg_listen_ip = inet_addr(*cfg_listen_addr);
}

static void CfgListenIp(Config<in_addr_t> *config)
{
  in_addr_t ip = *cfg_listen_ip;
  cfg_listen_addr.Override(inet_ntoa(*(struct in_addr *)&ip));
}

//---------------------------------------------------------------------------

Config<uint16_t> cfg_listen_port = 0;
Config<uint16_t> cfg_default_port = 2706;

//---------------------------------------------------------------------------

Config<bt_length_t> cfg_req_slice_size = DEFAULT_SLICE_SIZE;
Config<bt_length_t> cfg_req_slice_size_k = DEFAULT_SLICE_SIZE / 1024;

static void CfgReqSliceSize(Config<bt_length_t> *config)
{
  cfg_req_slice_size_k.Override(*cfg_req_slice_size / 1024);
}

static void CfgReqSliceSizeK(Config<bt_length_t> *config)
{
  cfg_req_slice_size = *cfg_req_slice_size_k * 1024;
  cfg_req_slice_size_k.Override(*cfg_req_slice_size / 1024);
}

static bool ValCfgReqSliceSize(const Config<bt_length_t> *config,
  bt_length_t value)
{
  return (value % 1024 == 0 && value <= MIN_SLICE_SIZE &&
          value <= MAX_SLICE_SIZE);
}

//---------------------------------------------------------------------------

Config<bool> cfg_convert_filenames = false;

//---------------------------------------------------------------------------

Config<int> cfg_status_format = 1;

static void CfgStatusFormat(Config<int> *config)
{
  CONSOLE.Status(1);
}

static void InfoCfgStatusFormat(Config<int> *config)
{
  char info[32];   
  strncpy(info, CONSOLE.StatusLine(), sizeof(info));
  info[sizeof(info) - 4] = '\0';
  strcat(info, "...");
  config->SetInfo(info);
}

//===========================================================================

void InitConfig()
{
  cfg_cache_size.Init("Cache size [-C]", "megabytes max");
  cfg_cache_size.Setup(CfgCacheSize, 0, InfoCfgCacheSize);
  cfg_cache_size.SetMax((unsigned int)-1);
  CONFIG.Add("cache_size", cfg_cache_size);

  cfg_min_peers.Init("Min peers [-m]");
  cfg_min_peers.Setup(CfgMinPeers, 0, InfoCfgPeers, 1, 1000);
  CONFIG.Add("peers_min", cfg_min_peers);
  
  cfg_max_peers.Init("Max peers [-M]");
  cfg_max_peers.Setup(CfgMaxPeers, 0, InfoCfgPeers, 20, 1000);
  CONFIG.Add("peers_max", cfg_max_peers);
  
  cfg_channel_normal.Init("Normal/status output");
  cfg_channel_normal.Setup(CfgChannel, ValCfgChannel);
  CONFIG.Add("channel.normal", cfg_channel_normal);

  cfg_channel_interact.Init("Interactive output");
  cfg_channel_interact.Setup(CfgChannel, ValCfgChannel);
  CONFIG.Add("channel.interact", cfg_channel_interact);

  cfg_channel_error.Init("Error/warning output");
  cfg_channel_error.Setup(CfgChannel, ValCfgChannel);
  CONFIG.Add("channel.error", cfg_channel_error);
  
  cfg_channel_debug.Init("Debug/verbose output");
  cfg_channel_debug.Setup(CfgChannel, ValCfgChannel);
  CONFIG.Add("channel.debug", cfg_channel_debug);

  cfg_channel_input.Init("Console input");
  cfg_channel_input.Setup(CfgChannel, ValCfgChannel);
  CONFIG.Add("channel.input", cfg_channel_input);

  cfg_max_bandwidth_down.Init("Max DL bandwidth [-d]", "B/s");
  cfg_max_bandwidth_down.Setup(CfgMaxBWDown);
  cfg_max_bandwidth_down.SetMax((dt_rate_t)-1);
  CONFIG.Add("bw_down_max", cfg_max_bandwidth_down);
  cfg_max_bandwidth_down.Hide();
  
  cfg_max_bw_down_k.Init("Max DL bandwidth [-d]", "K/s");
  cfg_max_bw_down_k.Setup(CfgMaxBWDownK);
  CONFIG.Add("bw_down_max_k", cfg_max_bw_down_k);

  cfg_max_bandwidth_up.Init("Max UL bandwidth [-u]", "B/s");
  cfg_max_bandwidth_up.Setup(CfgMaxBWUp);
  cfg_max_bandwidth_up.SetMax((dt_rate_t)-1);
  CONFIG.Add("bw_up_max", cfg_max_bandwidth_up);
  cfg_max_bandwidth_up.Hide();

  cfg_max_bw_up_k.Init("Max UL bandwidth [-u]", "K/s");
  cfg_max_bw_up_k.Setup(CfgMaxBWUpK);
  CONFIG.Add("bw_up_max_k", cfg_max_bw_up_k);

  cfg_seed_hours.Init("Seed time [-e]", "hours");
  cfg_seed_hours.Setup(CfgSeedHours, ValCfgSeedHours);
  cfg_seed_hours.SetMax(500000);
  CONFIG.Add("seed_hours", cfg_seed_hours);

  cfg_seed_time.Init("Seed time [-e]", "seconds");
  cfg_seed_time.Setup(CfgSeedTime, ValCfgSeedTime);
  cfg_seed_time.Override(*cfg_seed_hours * 3600);
  cfg_seed_time.Hide();
  CONFIG.Add("seed_time", cfg_seed_time);

  cfg_seed_remain.Init("Seed remaining", "hours");
  cfg_seed_remain.Setup(CfgSeedRemain, ValCfgSeedRemain);
  cfg_seed_remain.Hide();
  CONFIG.Add("seed_remain", cfg_seed_remain);

  cfg_seed_ratio.Init("Seed ratio [-E]", "Upload:Download");
  cfg_seed_ratio.SetValidator(ValCfgSeedRatio);
  CONFIG.Add("seed_ratio", cfg_seed_ratio);

  cfg_peer_prefix.Init("Peer ID prefix [-P]", "", MAX_PEER_PREFIX_LEN);
  CONFIG.Add("peer_prefix", cfg_peer_prefix);
   
  cfg_file_to_download.Init("Download files [-n]");
  cfg_file_to_download.Setup(CfgFileToDownload);
  CONFIG.Add("file_list", cfg_file_to_download);

  cfg_verbose.Init("Verbose output [-v]", "For debugging");
  cfg_verbose.Setup(CfgVerbose);
  CONFIG.Add("verbose", cfg_verbose);

  cfg_ctcs.Init("CTCS server [-S]");
  cfg_ctcs.Setup(CfgCTCS, ValCfgCTCS);
  CONFIG.Add("ctcs_server", cfg_ctcs);

  cfg_completion_exit.Init("Completion command [-X]");
  CONFIG.Add("user_exit", cfg_completion_exit);

  cfg_pause.Init("Pause torrent", "Stop upload/download");
  cfg_pause.Setup(CfgPause);
  CONFIG.Add("pause", cfg_pause);

  cfg_user_agent.Init("HTTP User-Agent [-A]");
  cfg_user_agent.Setup(CfgUserAgent, ValCfgUserAgent);
  CfgUserAgent(&cfg_user_agent);  // normalize the initial value
  cfg_user_agent.SetDefault(*cfg_user_agent);
  CONFIG.Add("user_agent", cfg_user_agent); 

  cfg_bitfield_file.Init("Bitfield save file [-b]");
  CONFIG.Add("bitfield_file", cfg_bitfield_file);

  cfg_daemon.Init("Daemon mode [-d]", "Use caution!");
  cfg_daemon.Setup(CfgDaemon);
  CONFIG.Add("daemon.mode", cfg_daemon);

  cfg_redirect_io.Init("I/O redirection [-dd]", "For daemon mode");
  CONFIG.Add("daemon.redirect", cfg_redirect_io);

  cfg_allocate.Init("File allocation mode [-a]");
  CfgAllocate(&cfg_allocate);  // set default info
  cfg_allocate.Setup(CfgAllocate);
  cfg_allocate.SetMax(2);
  CONFIG.Add("allocate", cfg_allocate);

  cfg_public_ip.Init("Public IP [-I]");
  CONFIG.Add("listen.public_ip", cfg_public_ip);

  cfg_listen_ip.Init("Listen IP [-i]");
  cfg_listen_ip.Setup(CfgListenIp);
  cfg_listen_ip.Hide();
  CONFIG.Add("listen.listen_ip", cfg_listen_ip);

  cfg_listen_addr.Init("Listen Address [-i]");
  cfg_listen_addr.Setup(CfgListenAddr);
  CONFIG.Add("listen.listen_addr", cfg_listen_addr);

  cfg_listen_port.Init("Listen port [-p]");
  cfg_listen_port.SetMax(65535);
  CONFIG.Add("listen.listen_port", cfg_listen_port);

  cfg_default_port.Init("Default port");
  cfg_default_port.SetMax(65535);
  CONFIG.Add("listen.default_port", cfg_default_port);

  cfg_req_slice_size.Init("Request block size [-z]", "Bytes");
  cfg_req_slice_size.Setup(CfgReqSliceSize, ValCfgReqSliceSize);
  cfg_req_slice_size.SetMin(MIN_SLICE_SIZE);
  cfg_req_slice_size.SetMax(MAX_SLICE_SIZE);
  cfg_req_slice_size.Hide();
  CONFIG.Add("req_size", cfg_req_slice_size);

  cfg_req_slice_size_k.Init("Request block size [-z]", "KB");
  cfg_req_slice_size_k.Setup(CfgReqSliceSizeK);
  cfg_req_slice_size_k.SetMax(MAX_SLICE_SIZE / 1024);
  CONFIG.Add("req_size_k", cfg_req_slice_size_k);

  cfg_convert_filenames.Init("Convert foreign filenames [-T]");
  CONFIG.Add("convert_names", cfg_convert_filenames);

  cfg_status_format.Init("Status line format");
  cfg_status_format.Setup(CfgStatusFormat, 0, InfoCfgStatusFormat);
  cfg_status_format.SetMax(STATUSLINES - 1);
  CONFIG.Add("status.format", cfg_status_format);
}

