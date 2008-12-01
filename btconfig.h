#ifndef BTCONFIG_H
#define BTCONFIG_H

// btconfig.h:  Copyright 2008 Dennis Holmes  (dholmes@rahul.net)

#include <time.h>
#include <stdlib.h>  // atof(), strtoll(), getenv()
#include <limits.h>  // strtoll()
#include <string.h>
#include <stdio.h>   // snprintf()

#include <sys/types.h>   // in_addr_t, inet_addr(), inet_ntoa(), stat()
#include <sys/socket.h>  // in_addr_t, inet_addr(), inet_ntoa()
#include <netinet/in.h>  // in_addr_t, inet_addr(), inet_ntoa()
#include <arpa/inet.h>   // in_addr_t, inet_addr(), inet_ntoa()

#include "bttypes.h"
#include "console.h"

/* According to specs the max slice size is 128K.  While most clients now do
   not accept a value that large, we want max compatibility. */
#define MAX_SLICE_SIZE (128*1024)
#define MIN_SLICE_SIZE (1024)
#define DEFAULT_SLICE_SIZE (16*1024)

#define PEER_ID_LEN 20


/* Is this a secondary process which should not disturb the terminal, existing
   connections, etc? */
extern bool g_secondary_process;
extern bool g_config_only;

extern bool arg_flg_force_seed_mode;
extern bool arg_flg_check_only;
extern bool arg_flg_exam_only;


/****************************************************************************
  Configuration schema
  See these classes for the interface capabilities:
  Base ("generic") class:  ConfigGen
  Template derived class:  Config<type>
  List container class:  Configuration

  The dt_config_t enumerated type describes the fundamental data type of a
  config value (the kind of data, not the specific type) and is returned by
  the Type() query.

  Config may be instantiated from any basic data type except <char *>.  String
  configs must use type <const char *>.  (C++ string class is not used.)

  The base class interface for setting and accessing values uses a string
  representation, since the use is typically to represent an arbitrary type
  to the user.  The derived classes offer a convenience notation of "=" for
  assignment and "*" for interfacing to the stored value, as well as Set()
  and Value() for cases where this would be syntactically more clear or
  determination of success is required.  Thus:
    Config<int> foo = 54;  // initialization, no handler function is used
    foo = 42;              // equivalent to:  foo.Set(42);
    int bar = *foo;        // equivalent to:  int bar = foo.Value();

  In general the classes are responsible for their own memory use.  C strings
  (char *) passed in are copied, so the caller must clean up its own copy (if
  it was allocated).  Likewise C strings returned are managed by the class and
  must *not* be freed/deleted by the caller.

  Configs should typically *not* be dynamically allocated, as Configuration
  neither copies nor deallocates them.

  If a null or empty string is used to assign a string Config, it will revert
  to the default value.

  A typical post-definition/initialization setup would use Init(), Setup(),
  and Add().

  Tags are used by the Configuration class and not intended to be manipulated
  except via Add().  The tag is a unique identifier which can be passed among
  modules and external facilities (such as CTCS or DMS agents) so that values
  can be queried and set using common functions.  For maximum compatibility
  tags should be composed of alphanumerics, "." and "_".  A "." is treated as
  a category separator (e.g. "console.out_debug") for grouping and sorting of
  the list.

  Iteration example to display current user-visible configuration values:
    for( ConfigGen *config = CONFIG.First();
         config;
         config = CONFIG.Next(config) ){
      if( !config->Hidden() ){
        CONSOLE.Print("%s:  %s", config->Desc(), config->Sval());
      }
    }
****************************************************************************/


enum dt_config_t {
  DT_CONFIG_STRING,
  DT_CONFIG_INT,
  DT_CONFIG_FLOAT,
  DT_CONFIG_BOOL    // any binary type:  true/false, yes/no, 1/0
};


class ConfigGen
{
 private:
  char *m_tag;      // identifier for programmatic use
  char *m_desc;     // descriptive name/identification
  size_t m_maxlen;
  bool m_hidden, m_locked;
  bool m_save;

 protected:
  char *m_info;     // additional brief info/description
  char *m_svalbuf, *m_sminbuf, *m_smaxbuf, *m_sdefbuf;

 public:
  ConfigGen();
  virtual ~ConfigGen();

  void SetTag(const char *tag);
  bool Init(const char *desc, const char *info=(char *)0, size_t maxlen=0);
  void SetInfo(const char *info);
  virtual bool Scan(const char *valstr) = 0;  // set value from a string
  virtual void Reset() = 0;  // set value to default
  void Lock(){ m_locked = true; }
  void Unlock(){ m_locked = false; }
  void Hide(){ m_hidden = true; }
  void Unhide(){ m_hidden = false; }
  void Save() { m_save = true; }
  void Unsave() { m_save = false; }

  virtual dt_config_t Type() const = 0;

  // String representations of content fields
  virtual const char *Sval() = 0;
  virtual const char *Smin() = 0;
  virtual const char *Smax() = 0;
  virtual const char *Sdefault() = 0;

  const char *Tag() const { return m_tag; }
  const char *Desc() const { return m_desc; }
  virtual const char *Info() { return m_info; }
  size_t MaxLen() const { return m_maxlen; }
  bool Hidden() const { return m_hidden; }
  bool Locked() const { return m_locked; }
  bool Saving() const { return m_save; }

  bool Match(const char *query) const { return (0==strcmp(m_tag, query)); }
};


template<class T> class Config:public ConfigGen
{
 private:
  T m_current, m_default, m_minval, m_maxval;

  /* The handler performs required actions upon changing the value.
     If it adjusts the value (via = or Set), it will be called again
     and so must be reentrant!
  */
  void (*m_handler)(Config<T> *config);

  /* The validator can use extended criteria to determine whether a
     value is acceptable.
  */
  bool (*m_validator)(const Config<T> *config, T value);

  /* The infomaker can update the info data at the time it is requested
     (for dynamic info).
  */
  void (*m_infomaker)(Config<T> *config);

  void Handler(){ if( m_handler && !g_config_only ) (*m_handler)(this); }
  void Infomaker(){ if( m_infomaker && !g_config_only ) (*m_infomaker)(this); }

  const char *Sprint(char **buffer, T value);

 public:
  Config(T init=0);
  ~Config();

  dt_config_t Type() const;

  /* Operator = returns the assigned (resulting) value.
     Set() returns an indication of success (true) or failure (false).
  */
  T operator=(T newval){ Set(newval); return m_current; }
  bool Set(T newval);
  bool Scan(const char *valstr);
  void Override(T newval);
  void Reset() { Set(m_default); }

  /* Operator * provides convenient access to the value. */
  T operator*() const { return m_current; }
  T Value() const { return m_current; }
  const char *Info() { Infomaker(); return m_info; }
  const char *Sval(){ return Sprint(&m_svalbuf, m_current); }
  const char *Smin(){ return Sprint(&m_sminbuf, m_minval); }
  const char *Smax(){ return Sprint(&m_smaxbuf, m_maxval); }
  const char *Sdefault(){ return Sprint(&m_sdefbuf, m_default); }
  T Min() const { return m_minval; }
  T Max() const { return m_maxval; }

  void SetMin(T newmin);
  void SetMax(T newmax);
  void SetDefault(T newdef);
  void SetHandler(void (*func)(Config<T> *)){ m_handler = func; }
  void SetValidator(bool (*func)(const Config<T> *, T)){ m_validator = func; }
  void SetInfomaker(void (*func)(Config<T> *)){ m_infomaker = func; }
  void Setup(void (*hfunc)(Config<T> *), bool (*vfunc)(const Config<T> *, T)=0,
    void (*ifunc)(Config<T> *)=0, T newmin=0, T newmax=0);

  bool Valid(T value) const;

  T operator++();
  T operator++(int);
  T operator--();
  T operator--(int);
  T operator+=(int arg);
  T operator-=(int arg);
  T operator*=(int arg);
  T operator/=(int arg);
  T operator+=(double arg);
  T operator-=(double arg);
  T operator*=(double arg);
  T operator/=(double arg);
};


//===========================================================================
// Template functions

static int DoNothing() { return 0; }


template<> Config<const char *>::Config(const char *init);
template<class T> Config<T>::Config(T init)
{
  m_current = m_default = init;
  m_minval = m_maxval = (T)0; 
  m_handler = (void (*)(Config<T> *))0;
  m_validator = (bool (*)(const Config<T> *, T))0;
  m_infomaker = (void (*)(Config<T> *))0;
}


template<> Config<const char *>::~Config();
template<class T> Config<T>::~Config(){ DoNothing(); }

template<> dt_config_t Config<bool>::Type() const;
template<> dt_config_t Config<double>::Type() const;
template<> dt_config_t Config<float>::Type() const;
template<> dt_config_t Config<const char *>::Type() const;
template<class T> dt_config_t Config<T>::Type() const { return DT_CONFIG_INT; }

template<> void Config<const char *>::Override(const char *newval);
template<class T> void Config<T>::Override(T newval){ m_current = newval; }

template<> void Config<const char *>::SetMin(const char *newmin);
template<class T> void Config<T>::SetMin(T newmin){ m_minval = newmin; }

template<> void Config<const char *>::SetMax(const char *newmin);
template<class T> void Config<T>::SetMax(T newmax){ m_maxval = newmax; }

template<> void Config<const char *>::SetDefault(const char *newdef);
template<class T> void Config<T>::SetDefault(T newdef){ m_default = newdef; }


template<class T> void Config<T>::Setup(void (*hfunc)(Config<T> *),
  bool (*vfunc)(const Config<T> *, T), void (*ifunc)(Config<T> *), T newmin,
  T newmax)
{
  m_handler = hfunc;
  m_validator = vfunc;
  m_infomaker = ifunc;
  m_minval = newmin;
  m_maxval = newmax;
}


template<> bool Config<const char *>::Set(const char *newval);
template<> bool Config<bool>::Set(bool newval);
template<class T> bool Config<T>::Set(T newval)
{
  T useval = newval;

  if( m_minval <= m_maxval && m_maxval > 0 ){
    if( useval > m_maxval ) useval = m_maxval;
    else if( useval < m_minval ) useval = m_minval;
  }

  if( useval == m_current )
    return true;
  if( !Locked() && Valid(newval) ){
    m_current = useval;
    Handler();
    CONSOLE.Debug("Config: %s = %s", Tag(), Sval());
    return true;
  }
  return false;
}


template<> bool Config<const char *>::Scan(const char *valstr);
template<> bool Config<bool>::Scan(const char *valstr);
template<> bool Config<double>::Scan(const char *valstr);
template<class T> bool Config<T>::Scan(const char *valstr)
{
  return Set((T)strtoll(valstr, (char **)0, 10));
}


template<> const char *Config<const char *>::Sprint(char **buffer,
  const char *value);
template<> const char *Config<bool>::Sprint(char **buffer, bool value);
template<> const char *Config<double>::Sprint(char **buffer, double value);
template<class T> const char *Config<T>::Sprint(char **buffer, T value)
{
  if( !*buffer ){
    *buffer = new char[32];
  }
  if( *buffer ) snprintf(*buffer, 32, "%lld", (long long)value);
  return *buffer ? *buffer : "Out of memory";
}


template<class T> bool Config<T>::Valid(T value) const
{
  bool valid = true;

  if( m_minval <= m_maxval && m_maxval > 0 )
    if( value < m_minval || value > m_maxval ) valid = false;
  if( valid && m_validator && !g_config_only )
    valid = (*m_validator)(this, value);
  return valid;
}


template<> const char *Config<const char *>::operator++();
template<class T> T Config<T>::operator++(){
  Set(m_current + 1); return m_current;
}
template<> const char *Config<const char *>::operator++(int);
template<class T> T Config<T>::operator++(int){
  int orig = m_current; Set(m_current + 1); return orig;
}
template<> const char *Config<const char *>::operator--();
template<class T> T Config<T>::operator--(){
  Set(m_current - 1); return m_current;
}
template<> const char *Config<const char *>::operator--(int);
template<class T> T Config<T>::operator--(int){
  int orig = m_current; Set(m_current - 1); return orig;
}
template<> const char *Config<const char *>::operator+=(int arg);
template<class T> T Config<T>::operator+=(int arg){
  Set((T)(m_current + arg)); return m_current;
}
template<> const char *Config<const char *>::operator-=(int arg);
template<class T> T Config<T>::operator-=(int arg){
  Set((T)(m_current - arg)); return m_current;
}
template<> const char *Config<const char *>::operator*=(int arg);
template<class T> T Config<T>::operator*=(int arg){
  Set((T)(m_current * arg)); return m_current;
}
template<> const char *Config<const char *>::operator/=(int arg);
template<class T> T Config<T>::operator/=(int arg){
  Set((T)(m_current / arg)); return m_current;
}
template<> const char *Config<const char *>::operator+=(double arg);
template<class T> T Config<T>::operator+=(double arg){
  Set((T)(m_current + arg)); return m_current;
}
template<> const char *Config<const char *>::operator-=(double arg);
template<class T> T Config<T>::operator-=(double arg){
  Set((T)(m_current - arg)); return m_current;
}
template<> const char *Config<const char *>::operator*=(double arg);
template<class T> T Config<T>::operator*=(double arg){
  Set((T)(m_current * arg)); return m_current;
}
template<> const char *Config<const char *>::operator/=(double arg);
template<class T> T Config<T>::operator/=(double arg){
  Set((T)(m_current / arg)); return m_current;
}


//===========================================================================
// Configuration class

struct confignode{
  confignode *next;
  ConfigGen *item;
};


class Configuration
{
 private:
  confignode *m_head;
  mutable confignode *m_iter;  // cached iterator

  confignode *FindNode(const char *tag) const;

 public:
  Configuration(){ m_head = (confignode *)0; }
  ~Configuration();

  bool Add(const char *tag, ConfigGen &config);
  ConfigGen *Find(const char *tag) const;
  ConfigGen *operator[](const char *tag){ return Find(tag); }
  bool Set(const char *tag, const char *value);
  const char *Get(const char *tag);

  ConfigGen *First() const;
  ConfigGen *Next(const ConfigGen *current) const;

  bool Save(const char *filename) const;
  bool Load(const char *filename);

  void Dump() const;
};


extern Configuration CONFIG;


//===========================================================================
// Global declarations

extern Config<unsigned int> cfg_cache_size;  // megabytes

extern Config<dt_count_t> cfg_max_peers;
extern Config<dt_count_t> cfg_min_peers;

extern Config<const char *> cfg_channel_normal;
extern Config<const char *> cfg_channel_interact;
extern Config<const char *> cfg_channel_error;
extern Config<const char *> cfg_channel_debug;
extern Config<const char *> cfg_channel_input;

extern Config<dt_rate_t> cfg_max_bandwidth_down;
extern Config<dt_rate_t> cfg_max_bw_down_k;
extern Config<dt_rate_t> cfg_max_bandwidth_up;
extern Config<dt_rate_t> cfg_max_bw_up_k;

extern Config<time_t> cfg_seed_time;
extern Config<uint32_t> cfg_seed_hours;
extern Config<double> cfg_seed_remain;
extern Config<double> cfg_seed_ratio;

extern Config<const char *> cfg_peer_prefix;  // BT peer ID prefix

extern Config<const char *> cfg_file_to_download;

extern Config<bool> cfg_verbose;

extern Config<const char *> cfg_ctcs;

extern Config<const char *> cfg_completion_exit;

extern Config<bool> cfg_pause;

extern Config<const char *> cfg_user_agent;  // HTTP header

extern Config<const char *> cfg_bitfield_file;

extern Config<bool> cfg_daemon;
extern Config<bool> cfg_redirect_io;

extern Config<unsigned char> cfg_allocate;
extern Config<const char *> cfg_staging_dir;

extern Config<const char *> cfg_public_ip;
extern Config<in_addr_t> cfg_listen_ip;
extern Config<const char *> cfg_listen_addr;
extern Config<uint16_t> cfg_listen_port;
extern Config<uint16_t> cfg_default_port;

extern Config<bt_length_t> cfg_req_slice_size;
extern Config<bt_length_t> cfg_req_slice_size_k;

extern Config<bool> cfg_convert_filenames;

extern Config<int> cfg_status_format;
extern Config<time_t> cfg_status_snooze;

extern Config<const char *> cfg_config_file;


void CfgAllocate(Config<unsigned char> *config);
void CfgCTCS(Config<const char *> *config);
void InitConfig();


#endif  // BTCONFIG_H

