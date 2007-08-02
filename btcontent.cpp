#include "btcontent.h"

#ifdef WINDOWS
#include <direct.h>
#include <io.h>
#include <memory.h>
// include windows sha1 header here.

#else
#include <unistd.h>
#include <sys/param.h>

#if defined(USE_STANDALONE_SHA1)
#include "sha1.h"
#elif defined(HAVE_OPENSSL_SHA_H)
#include <openssl/sha.h>
#elif defined(HAVE_SSL_SHA_H)
#include <ssl/sha.h>
#elif defined(HAVE_SHA_H)
#include <sha.h>
#endif

#endif

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "btconfig.h"
#include "bencode.h"
#include "peer.h"
#include "httpencode.h"
#include "tracker.h"
#include "peerlist.h"
#include "ctcs.h"
#include "console.h"
#include "bttime.h"

#define meta_str(keylist,pstr,pint) decode_query(b,flen,(keylist),(pstr),(pint),(int64_t*) 0,QUERY_STR)
#define meta_int(keylist,pint) decode_query(b,flen,(keylist),(const char**) 0,(pint),(int64_t*) 0,QUERY_INT)
#define meta_pos(keylist) decode_query(b,flen,(keylist),(const char**) 0,(size_t*) 0,(int64_t*) 0,QUERY_POS)

#define CACHE_FIT(ca,roff,rlen)	\
(max_uint64_t((ca)->bc_off,(roff)) <= \
 min_uint64_t(((ca)->bc_off + (ca)->bc_len - 1),(roff + rlen - 1)))

#define MAX_OPEN_FILES 20

btContent BTCONTENT;

static void Sha1(char *ptr,size_t len,unsigned char *dm)
{
#if defined(USE_STANDALONE_SHA1)
  SHA1_CTX context;
  SHA1Init(&context);
  SHA1Update(&context,(unsigned char*)ptr,len);
  SHA1Final(dm,&context);
#else
#ifdef WINDOWS
  ;
#else
  SHA_CTX context;
  SHA1_Init(&context);
  SHA1_Update(&context,(unsigned char*)ptr,len);
  SHA1_Final(dm,&context);
#endif
#endif
}

btContent::btContent()
{
  m_announce = global_piece_buffer = (char*) 0;
  m_hash_table = (unsigned char *) 0;
  pBF = (BitField*) 0;
  pBFilter = (BitField*) 0;
  pBRefer = (BitField*) 0;
  pBChecked = (BitField*) 0;
  m_create_date = m_seed_timestamp = (time_t) 0;
  time(&m_start_timestamp);
  m_cache_oldest = m_cache_newest = (BTCACHE*) 0;
  m_cache_size = m_cache_used = 0;
  m_flush_failed = (time_t) 0;
  m_check_piece = 0;
}

int btContent::CreateMetainfoFile(const char *mifn)
{
  FILE *fp;
  fp = fopen(mifn, "r");
  if( fp ){
    CONSOLE.Warning(1, "error, file %s already exist.", mifn);
    return -1;
  }else if( ENOENT != errno ){
    CONSOLE.Warning(1, "error, couldn't create %s.", mifn);
    return -1;
  }

  fp = fopen(mifn, "w");

  if( !fp ){
    CONSOLE.Warning(1, "error, open %s failed. %s", mifn, strerror(errno));
    return -1;
  }
  if( bencode_begin_dict(fp) != 1 ) goto err;

  // announce
  if( bencode_str("announce", fp) != 1 ) goto err;
  if( bencode_str(m_announce, fp) != 1 ) goto err;
  // create date
  if( bencode_str("creation date", fp) != 1 ) goto err;
  if( bencode_int(m_create_date, fp) != 1 ) goto err;

  // info dict
  if( bencode_str("info", fp) != 1 ) goto err;
  if( bencode_begin_dict(fp) != 1 ) goto err;

  if( m_btfiles.FillMetaInfo(fp) != 1 ) goto err;

  // piece length
  if( bencode_str("piece length", fp) != 1 ) goto err;
  if( bencode_int(m_piece_length, fp) != 1 ) goto err;
  
  // hash table;
  if( bencode_str("pieces", fp) != 1 ) goto err;
  if( bencode_buf((const char*) m_hash_table, m_hashtable_length, fp) != 1 )
    goto err;

  if( bencode_end_dict_list(fp) != 1 ) goto err; // end info
  if( bencode_end_dict_list(fp) != 1 ) goto err; // end torrent

  fclose(fp);
  return 0;
 err:
  if( fp ) fclose(fp);
  return -1;
}

int btContent::InitialFromFS(const char *pathname, char *ann_url, size_t piece_length)
{
  size_t n, percent;

  // piece length
  m_piece_length = piece_length;
  if( m_piece_length % 65536 ){ 
    m_piece_length /= 65536;
    m_piece_length *= 65536;
  }

  // This is really just a sanity check on the piece length to create.
  if( !m_piece_length || m_piece_length > 4096*1024 )
    m_piece_length = 262144;
  
  m_announce = ann_url;
  m_create_date = time((time_t*) 0);

  if(m_btfiles.BuildFromFS(pathname) < 0) return -1;

  global_piece_buffer = new char[m_piece_length];
#ifndef WINDOWS
  if( !global_piece_buffer ) return -1;
#endif
  
  // n pieces
  m_npieces = m_btfiles.GetTotalLength() / m_piece_length;
  if( m_btfiles.GetTotalLength() % m_piece_length ) m_npieces++;

  // create hash table.
  m_hashtable_length = m_npieces * 20;
  m_hash_table = new unsigned char[m_hashtable_length];
#ifndef WINDOWS
  if( !m_hash_table ) return -1;
#endif

  percent = m_npieces / 100;
  if( !percent ) percent = 1;

  CONSOLE.Interact_n("");
  for( n = 0; n < m_npieces; n++ ){
    if( GetHashValue(n, m_hash_table + n * 20) < 0 ) return -1;
    if( n % percent == 0 || n == m_npieces-1 ){
      CONSOLE.InteractU("Create hash table: %d/%d", (int)n+1, (int)m_npieces);
    }
  }
  return 0;
}

int btContent::PrintOut()
{
  CONSOLE.Print("META INFO");
  CONSOLE.Print("Announce: %s", m_announce);
  if( m_create_date ){
    char s[42];
    ctime_r(&m_create_date, s);
    if( s[strlen(s)-1] == '\n' ) s[strlen(s)-1] = '\0';
    CONSOLE.Print("Created On: %s", s);
  }
  CONSOLE.Print("Piece length: %lu", (unsigned long)m_piece_length);
  m_btfiles.PrintOut();
  return 0;
}

int btContent::PrintFiles()
{
  m_btfiles.PrintOut();
  return 0;
}

int btContent::InitialFromMI(const char *metainfo_fname,const char *saveas)
{
#define ERR_RETURN()	{if(b) delete []b; return -1;}
  unsigned char *ptr = m_shake_buffer;
  char *b;
  const char *s;
  size_t flen, q, r;

  m_cache_hit = m_cache_miss = 0;
  time(&m_cache_eval_time);

  b = _file2mem(metainfo_fname,&flen);
  if ( !b ) return -1;

  // announce
  if( !meta_str("announce",&s,&r) ) ERR_RETURN();
  if( r > MAXPATHLEN ) ERR_RETURN();
  m_announce = new char [r + 1];
  memcpy(m_announce, s, r);
  m_announce[r] = '\0';
  
  // infohash
  if( !(r = meta_pos("info")) ) ERR_RETURN();
  if( !(q = decode_dict(b + r, flen - r, (char *) 0)) ) ERR_RETURN();
  Sha1(b + r, q, m_shake_buffer + 28);

  if( meta_int("creation date",&r) ) m_create_date = (time_t) r;
 
  // hash table
  if( !meta_str("info|pieces",&s,&m_hashtable_length) ||
      m_hashtable_length % 20 != 0 ) ERR_RETURN();

  m_hash_table = new unsigned char[m_hashtable_length];

#ifndef WINDOWS
  if( !m_hash_table ) ERR_RETURN();
#endif
  memcpy(m_hash_table, s, m_hashtable_length);

  if( !meta_int("info|piece length",&m_piece_length) ) ERR_RETURN();
  m_npieces = m_hashtable_length / 20;

  cfg_req_queue_length = (m_piece_length / cfg_req_slice_size) * 2 - 1;

  if( m_piece_length < cfg_req_slice_size )
    cfg_req_slice_size = m_piece_length;
  
  if( m_btfiles.BuildFromMI(b, flen, saveas) < 0 ) ERR_RETURN();

  delete []b;
  b = (char *)0;
  
  if( arg_flg_exam_only ){
    PrintOut();
    return 0;
  }else{
    arg_flg_exam_only = 1;
    PrintOut();
    arg_flg_exam_only = 0;
  }

  if( (r = m_btfiles.CreateFiles()) < 0 ) ERR_RETURN();

  global_piece_buffer = new char[m_piece_length];
#ifndef WINDOWS
  if( !global_piece_buffer ) ERR_RETURN();
#endif

  pBF = new BitField(m_npieces);
#ifndef WINDOWS
  if( !pBF ) ERR_RETURN();
#endif

  pBRefer = new BitField(m_npieces);
#ifndef WINDOWS
  if( !pBRefer ) ERR_RETURN();
#endif

  pBChecked = new BitField(m_npieces);
#ifndef WINDOWS
  if( !pBChecked ) ERR_RETURN();
#endif

  //create the file filter
  pBFilter = new BitField(m_npieces);
#ifndef WINDOWS
   if( !pBFilter ) ERR_RETURN();
#endif
  if(arg_file_to_download>0){
    m_btfiles.SetFilter(arg_file_to_download,pBFilter,m_piece_length, 1);
  }

  m_left_bytes = m_btfiles.GetTotalLength() / m_piece_length;
  if( m_btfiles.GetTotalLength() % m_piece_length ) m_left_bytes++;
  if( m_left_bytes != m_npieces ) ERR_RETURN();
  
  m_left_bytes = m_btfiles.GetTotalLength();

  if( arg_flg_check_only ){
    if( r ) CheckExist();
    m_btfiles.PrintOut(); // show file completion
    CONSOLE.Print("Already/Total: %d/%d (%d%%)", (int)(pBF->Count()),
      (int)m_npieces, (int)(100 * pBF->Count() / m_npieces));
    if( !arg_flg_force_seed_mode ){
      SaveBitfield();
      exit(0);
    }
  }else if( pBRefer->SetReferFile(arg_bitfield_file) < 0 ){
    CONSOLE.Warning(2, "warn, couldn't set bit field refer file %s:  %s",
      arg_bitfield_file, strerror(errno));
    CONSOLE.Warning(2, "This is normal if you are starting or seeding.");
    pBRefer->SetAll();  // need to check all pieces
  }else if( unlink(arg_bitfield_file) < 0 ){
    CONSOLE.Warning(2, "warn, couldn't delete bit field file %s:  %s",
      arg_bitfield_file, strerror(errno));
  }
  if( !r ){  // don't hash-check if the files were just created
    m_check_piece = m_npieces;
    pBChecked->SetAll();
    delete pBRefer;
    if( arg_flg_force_seed_mode ){
      CONSOLE.Warning(2, "Files were not present; overriding force mode!");
    }
  }else if( arg_flg_force_seed_mode && !arg_flg_check_only ){
    size_t idx = 0;
    *pBF = *pBRefer;
    if( pBF->IsFull() ){
      CONSOLE.Interact("Skipping hash checks and forcing seed mode.");
      CONSOLE.Interact(
       "-----> STOP NOW if you have not downloaded the whole torrent! <-----");
      m_left_bytes = 0;
    }else for( ; idx < m_npieces; idx++ ){
      if( pBF->IsSet(idx) )
        m_left_bytes -= GetPieceLength(idx);
    }
    m_check_piece = m_npieces;
    pBChecked->SetAll();
    delete pBRefer;
  }

  m_cache = new BTCACHE *[m_npieces];
  if( !m_cache ){
    CONSOLE.Warning(1, "error, allocate cache index failed");
    ERR_RETURN();
  }
  memset(m_cache, 0, m_npieces * sizeof(BTCACHE*));
  CacheConfigure();

  *ptr = (unsigned char) 19; ptr++; // protocol string length
  memcpy(ptr,"BitTorrent protocol",19); ptr += 19; //  protocol string
  memset(ptr,0,8);		// reserved set zero.

  {				// peer id
        char *sptr = arg_user_agent;
        char *dptr = (char *)m_shake_buffer + 48;
        char *eptr = dptr + PEER_ID_LEN;
        while (*sptr) *dptr++ = *sptr++;
        while (dptr < eptr) *dptr++ = (unsigned char)random();
  }
  return 0;
}

btContent::~btContent()
{
  if(m_hash_table) delete []m_hash_table;
  if(m_announce) delete []m_announce;
  if(global_piece_buffer) delete []global_piece_buffer;
  if(pBF) delete pBF;
}

void btContent::_Set_InfoHash(unsigned char buf[20]) 
{ 
  memcpy(m_shake_buffer + 28, buf, 20);
}

// returns <0 if error; if using cache: 1 if read from disk, 0 otherwise
ssize_t btContent::ReadSlice(char *buf,size_t idx,size_t off,size_t len)
{
  ssize_t retval = 0;
  uint64_t offset = (uint64_t)idx * (uint64_t)m_piece_length + off;

  if( !m_cache_size ) return buf ? m_btfiles.IO(buf, offset, len, 0) : 0;
  else{
    size_t len2;
    int flg_rescan;
    BTCACHE *p;

    p = m_cache[idx];
    for( ; p && offset + len > p->bc_off && !CACHE_FIT(p,offset,len);
        p = p->bc_next );

    for( ; len && p && CACHE_FIT(p, offset, len); ){
      flg_rescan = 0;
      if( offset < p->bc_off ){
        len2 = p->bc_off - offset;
        if( CacheIO(buf, offset, len2, 0) < 0 ) return -1;
        retval = 1;
        if(buf) m_cache_miss += len2 / cfg_req_slice_size;
        flg_rescan = 1;
      }else{
        char *src;
        if( offset > p->bc_off ){
          len2 = p->bc_off + p->bc_len - offset;
          if( len2 > len ) len2 = len;
          src = p->bc_buf + offset - p->bc_off;
        }else{
          len2 = (len > p->bc_len) ? p->bc_len : len;
          src = p->bc_buf;
        }
        if( buf ){
          memcpy(buf, src, len2);
          m_cache_hit += len2 / cfg_req_slice_size;
        }else{  // prefetch only, update the age
          if( m_cache_newest != p ){
            if( p->age_prev ) p->age_prev->age_next = p->age_next;
            else if( p->age_next ) m_cache_oldest = p->age_next;
            if( p->age_next ) p->age_next->age_prev = p->age_prev;
            m_cache_newest->age_next = p;
            p->age_next = (BTCACHE *)0;
            p->age_prev = m_cache_newest;
            m_cache_newest = p;
          }
        }
      }

      if( buf ) buf += len2;
      offset += len2;
      len -= len2;

      if( len ){
        if( flg_rescan ){
          for( p = m_cache[idx];
               p && (offset + len) > p->bc_off && !CACHE_FIT(p,offset,len);
               p = p->bc_next );
        }else{
          p = p->bc_next;
        }
      }
    }// end for;
  
    if( len ){
      if(buf) m_cache_miss += len / cfg_req_slice_size;
      retval = CacheIO(buf, offset, len, 0);
      return (retval < 0) ? retval : 1;
    }
  }
  return retval;
}

void btContent::CacheClean(size_t need)
{
  BTCACHE *p, *pnext;
  int f_flushed = 0;

  if( m_flush_failed ) FlushCache();  // try again

  again:
  for( p=m_cache_oldest; p && m_cache_size < m_cache_used + need; p=pnext ){
    pnext = p->age_next;
    if( !p->bc_f_flush ){
      if( p->age_prev ) p->age_prev->age_next = p->age_next;
      else m_cache_oldest = p->age_next;
      if( p->age_next ) p->age_next->age_prev = p->age_prev;
      else m_cache_newest = p->age_prev;

      if( p->bc_prev ) p->bc_prev->bc_next = p->bc_next;
      if( p->bc_next ) p->bc_next->bc_prev = p->bc_prev;

      size_t idx = p->bc_off / m_piece_length;
      if( p->bc_next && p->bc_next->bc_off / m_piece_length == idx )
        m_cache[idx] = p->bc_next;
      else m_cache[idx] = (BTCACHE *)0;

      m_cache_used -= p->bc_len;
      delete []p->bc_buf;
      delete p;
    }
  }
  if( m_cache_size < m_cache_used + need ){  // still not enough
    if( m_cache_size < cfg_cache_size*1024*1024 ){  // can alloc more
      m_cache_size = (m_cache_used + need > cfg_cache_size*1024*1024) ?
        cfg_cache_size*1024*1024 : m_cache_used + need;
    }
    if( m_cache_size < m_cache_used + need && m_cache_used && !f_flushed ){
      // flush and try again
      if(arg_verbose) CONSOLE.Debug("CacheClean flushing to obtain space");
      FlushCache();
      f_flushed = 1;
      goto again;
    }  // else we tried...
  }
}

// Don't call this function if cfg_cache_size==0 !
void btContent::CacheEval()
{
  BTCACHE *p = m_cache_oldest;
  size_t interval;
  size_t unflushed = 0, dlnext, upadd = 0, upmax = 0, upmin = 0, total;

  size_t rateup = Self.RateUL();
  size_t ratedn = Self.RateDL();
  size_t unchoked = WORLD.GetUnchoked();

  // Time until next cache size eval: unchoke interval or time to dl a piece.
  if( ratedn ){
    interval = m_piece_length / ratedn;
    if( interval > WORLD.GetUnchokeInterval() )
      interval = WORLD.GetUnchokeInterval();
    else if( 0==interval ) interval = 1;
  }else interval = WORLD.GetUnchokeInterval();

  // Download: total unflushed data + data to dl before next eval
  // Hold the first piece a bit to let uploading begin.
  if( pBF->IsFull() ) dlnext = 0;
  else{
    if( pBF->Count() < 2 ) unflushed = m_cache_used;
    else for( ; p; p = p->age_next )
      if( p->bc_f_flush ) unflushed += p->bc_len;
    // Make sure we can read back and check a completed piece.
    dlnext = ratedn * interval + m_piece_length;
    // Set a shorter interval if DL cache need is very high.
    if( ratedn ){
      size_t max =
        (cfg_cache_size*1024*1024 - unflushed - m_piece_length) / ratedn;
      if( interval > max ) interval = max;
    }
  }

  // Upload: need enough to hold read/dl'd data until it can be sent
  upmin = DEFAULT_SLICE_SIZE * unchoked;
  upmax = cfg_cache_size*1024*1024;
  if( pBF->IsFull() ){
    // Seed mode.  All cache data is prefetched, and we don't normally need to
    // keep prefetched data longer than 2.5 unchoke intervals.
    if( rateup && unchoked ){
      // A very slow peer can't possibly benefit from cache--don't grow for it.
      size_t slowest = (size_t)( 1 + DEFAULT_SLICE_SIZE /
                                 ((double)cfg_cache_size*1024*1024 / rateup) );
      // Lead cache: data we need to cache to keep the slowest up's data cached
      // Add a slice per up for timing uncertainty
      if( slowest = WORLD.GetSlowestUp(slowest) )
        upadd = DEFAULT_SLICE_SIZE * ( rateup / slowest + unchoked-1 );
      else upadd = DEFAULT_SLICE_SIZE * unchoked;

      upmin = DEFAULT_SLICE_SIZE * unchoked;
      upmax = (size_t)( DEFAULT_SLICE_SIZE * (unchoked-1) +
        rateup * 2.5 * WORLD.GetUnchokeInterval() );
    }
  }else{
    if( rateup > ratedn ){
      size_t slowest = (size_t)( 1 +
        cfg_req_slice_size * ((double)ratedn / cfg_cache_size*1024*1024) +
        DEFAULT_SLICE_SIZE * ((double)rateup / cfg_cache_size*1024*1024) );
      if( slowest = WORLD.GetSlowestUp(slowest) )
        // lead cache is how much we'll use while uploading a slice to slowest
        // (default_slice_size / slowest) * (ratedn + rateup)
        upadd = (size_t)( ((double)DEFAULT_SLICE_SIZE / slowest) *
                          (ratedn + rateup + 1) );
      else upadd = m_piece_length * unchoked;
    }
    else if( rateup ){
      // same as m_piece_length / (cfg_cache_size*1024*1024 / (double)ratedn)
      size_t slowest = (size_t)( 1 +
        ratedn * ((double)m_piece_length / (cfg_cache_size*1024*1024)) );
      if( slowest = WORLD.GetSlowestUp(slowest) ){
        // m_piece_length / (double)slowest * ratedn
        // optimize, then round up a piece and add a piece
        upadd = m_piece_length * (ratedn / slowest + 2);
      }else{  // gimme 10 seconds worth (unchoke interval)
        // Can't keep pieces in cache long enough to upload them.
        // Rely on prefetching slices from disk instead.
        upadd = ratedn * WORLD.GetUnchokeInterval() +
                DEFAULT_SLICE_SIZE * unchoked;
      }
    }
  }

  if( upadd < upmin ) upadd = upmin;

  // Add a slice to round up
  total = unflushed + dlnext + upadd + cfg_req_slice_size;

  // Limit to max configured size
  if( total > cfg_cache_size*1024*1024 ) total = cfg_cache_size*1024*1024;

  // Don't decrease cache size if flush failed.
  if( !m_flush_failed || total > m_cache_size ) m_cache_size = total;

  if(arg_verbose)
    CONSOLE.Debug("DL need: %dK  UL need: %dK  Cache: %dK  Used: %dK",
    (int)((unflushed+dlnext)/1024), (int)(upadd/1024),
    (int)(m_cache_size/1024), (int)(m_cache_used/1024));
  m_cache_eval_time = now + interval;
}

void btContent::CacheConfigure()
{
  if( cfg_cache_size ){
    if( cfg_cache_size > GetTotalFilesLength()/1024/1024 )
      cfg_cache_size = (GetTotalFilesLength()+1024*1024-1)/1024/1024;
    CacheEval();
  }else m_cache_size = 0;

  if( m_cache_size < m_cache_used && !m_flush_failed ) CacheClean(0);
}

void btContent::FlushCache(size_t idx)
{
  BTCACHE *p;

  p = m_cache[idx];
  if( idx == m_npieces ) p = m_cache_oldest;
  for( ; p; p = (idx < m_npieces) ? p->bc_next : p->age_next ){
    if( idx == p->bc_off / m_piece_length ||
        (p->bc_f_flush && idx == m_npieces) ||
        idx == (p->bc_off+p->bc_len-1) / m_piece_length ){
      // update the age--flushing the entry or its piece
      if( m_cache_newest != p ){
        if( p->age_prev ) p->age_prev->age_next = p->age_next;
        else if( p->age_next ) m_cache_oldest = p->age_next;
        if( p->age_next ) p->age_next->age_prev = p->age_prev;
        m_cache_newest->age_next = p;
        p->age_next = (BTCACHE *)0;
        p->age_prev = m_cache_newest;
        m_cache_newest = p;
      }
      if( p->bc_f_flush ){
        if( m_btfiles.IO(p->bc_buf, p->bc_off, p->bc_len, 1) < 0 ){
          if( now >= m_flush_failed + 300 ){
            if( !m_flush_failed )
              m_cache_size += cfg_req_slice_size * WORLD.GetDownloads() * 2;
            CONSOLE.Warning(1, "warn, write file failed while flushing cache.");
            CONSOLE.Warning(1,
              "You need to have at least %llu bytes free on this filesystem!",
              (unsigned long long)(m_left_bytes + m_cache_used));
            CONSOLE.Warning(1, "This can also be caused by a disk error.");
            CONSOLE.Warning(1, "Temporarily%s suspending download...",
              (!m_flush_failed && m_cache_size > cfg_cache_size*1024*1024) ?
              " increasing cache size and" : "");
            m_flush_failed = now;
            WORLD.SeedOnly(1);
          }
          return;
        }else{
          p->bc_f_flush = 0;
          if(m_flush_failed){
            CONSOLE.Warning(3, "Flushing cache succeeded, resuming download");
            m_flush_failed = 0;
            CacheConfigure();
            WORLD.SeedOnly(0);
          }
        }
      }
    }else if( idx < p->bc_off / m_piece_length ) break;
  }
}

void btContent::Uncache(size_t idx)
{
  BTCACHE *p, *pnext;

  p = m_cache[idx];
  for( ; p; p = pnext ){
    pnext = p->bc_next;
    if( idx == p->bc_off / m_piece_length ){
      if( p->age_prev ) p->age_prev->age_next = p->age_next;
      else m_cache_oldest = p->age_next;
      if( p->age_next ) p->age_next->age_prev = p->age_prev;
      else m_cache_newest = p->age_prev;

      if( p->bc_prev ) p->bc_prev->bc_next = p->bc_next;
      if( p->bc_next ) p->bc_next->bc_prev = p->bc_prev;

      m_cache_used -= p->bc_len;
      delete []p->bc_buf;
      delete p;
    }else break;
  }
  m_cache[idx] = (BTCACHE *)0;
}

ssize_t btContent::WriteSlice(char *buf,size_t idx,size_t off,size_t len)
{
  uint64_t offset = (uint64_t)idx * (uint64_t)m_piece_length + off;

  //CONSOLE.Debug("Offset-write: %llu - Piece:%lu",
  //  (unsigned long long)offset, (unsigned long)idx);

  if( !m_cache_size ) return m_btfiles.IO(buf, offset, len, 1);
  else{
    size_t len2;
    int flg_rescan;
    BTCACHE *p;

    p = m_cache[idx];
    for( ; p && (offset + len) > p->bc_off && !CACHE_FIT(p,offset,len);
        p = p->bc_next );

    for( ; len && p && CACHE_FIT(p, offset, len); ){
      flg_rescan = 0;
      if( offset < p->bc_off ){
        len2 = p->bc_off - offset;
        if( CacheIO(buf, offset, len2, 1) < 0 ) return -1;
        flg_rescan = 1;
      }else{
        if( offset > p->bc_off ){
          len2 = p->bc_off + p->bc_len - offset;
          if( len2 > len ) len2 = len;
          memcpy(p->bc_buf + offset - p->bc_off, buf, len2);
          p->bc_f_flush = 1;
        }else{
          len2 = (len > p->bc_len) ? p->bc_len : len;
          memcpy(p->bc_buf, buf, len2);
          p->bc_f_flush = 1;
        }
        // re-received this data, make it new again
        if( m_cache_newest != p ){
          if( p->age_prev ) p->age_prev->age_next = p->age_next;
          else if( p->age_next ) m_cache_oldest = p->age_next;
          if( p->age_next ) p->age_next->age_prev = p->age_prev;
          m_cache_newest->age_next = p;
          p->age_next = (BTCACHE *)0;
          p->age_prev = m_cache_newest;
          m_cache_newest = p;
        }
      }

      buf += len2;
      offset += len2;
      len -= len2;

      if( len ){
        if( flg_rescan ){
          for( p = m_cache[idx];
               p && (offset + len) > p->bc_off && !CACHE_FIT(p,offset,len);
               p = p->bc_next );
        }else{
          p = p->bc_next;
        }
      }
    }// end for;
  
    if( len ) return CacheIO(buf, offset, len, 1);
  }
  return 0;
}

ssize_t btContent::CacheIO(char *buf, uint64_t off, size_t len, int method)
{
  BTCACHE *p;
  BTCACHE *pp = (BTCACHE*) 0;
  BTCACHE *pnew = (BTCACHE*) 0;

  if(arg_verbose && 0==method)
    CONSOLE.Debug("Read to %s %d/%d/%d", buf?"buffer":"cache",
      (int)(off / m_piece_length), (int)(off % m_piece_length), (int)len);

  if( m_cache_size < m_cache_used + len ) CacheClean(len);
  // Note, there is no failure code from CacheClean().  If nothing can be done
  // to increase the cache size, we allocate what we need anyway.
  
  if( 0==method && buf && m_btfiles.IO(buf, off, len, method ) < 0) return -1;
  
  pnew = new BTCACHE;
#ifndef WINDOWS
  if( !pnew )
    return (method && buf) ? m_btfiles.IO(buf, off, len, method) : 0;
#endif

  pnew->bc_buf = new char[len];
#ifndef WINDOWS
  if( !(pnew->bc_buf) ){ 
    delete pnew; 
    return (method && buf) ? m_btfiles.IO(buf, off, len, method) : 0;
  }
#endif

  if( buf ) memcpy(pnew->bc_buf, buf, len);
  else if( 0==method && m_btfiles.IO(pnew->bc_buf, off, len, method) < 0 )
    return -1;
  pnew->bc_off = off;
  pnew->bc_len = len;
  pnew->bc_f_flush = method;
  m_cache_used += len;
  pnew->age_next = (BTCACHE *)0;
  if( m_cache_newest ){
    pnew->age_prev = m_cache_newest;
    m_cache_newest->age_next = pnew;
  }else{
    pnew->age_prev = (BTCACHE *)0;
    m_cache_oldest = pnew;
  }
  m_cache_newest = pnew;

  // find insert point: after pp, before p.
  size_t idx = off / m_piece_length;
  p = m_cache[idx];
  if( p ) pp = p->bc_prev;
  for( ; p && off > p->bc_off; pp = p, p = pp->bc_next );

  pnew->bc_next = p;
  pnew->bc_prev = pp;
  if( pp ) pp->bc_next = pnew;
  if( p ) p->bc_prev = pnew;
  if( !m_cache[idx] || off < m_cache[idx]->bc_off )
    m_cache[idx] = pnew;

  return 0;
}

ssize_t btContent::ReadPiece(char *buf,size_t idx)
{
  return ReadSlice(buf, idx, 0, GetPieceLength(idx));
}

size_t btContent::GetPieceLength(size_t idx)
{
  return (idx == m_btfiles.GetTotalLength() / m_piece_length) ?
    (size_t)(m_btfiles.GetTotalLength() % m_piece_length) 
    :m_piece_length;
}

int btContent::CheckExist()
{
  size_t idx = 0;
  size_t percent = GetNPieces() / 100;
  unsigned char md[20];

  if( !percent ) percent = 1;

  CONSOLE.Interact_n("");
  for( ; idx < m_npieces; idx++ ){
    if( GetHashValue(idx, md) == 0 &&
         memcmp(md, m_hash_table + idx * 20, 20) == 0 ){
       m_left_bytes -= GetPieceLength(idx);
       pBF->Set(idx);
    }
    if( idx % percent == 0 || idx == m_npieces-1 )
      CONSOLE.InteractU("Check exist: %d/%d", idx+1, m_npieces);
  }
  m_check_piece = m_npieces;
  pBChecked->SetAll();
  delete pBRefer;
  return 0;
}

int btContent::CheckNextPiece()
{
  size_t idx = m_check_piece;
  unsigned char md[20];
  int f_checkint = 0;

  if( idx >= m_npieces ) return 0;
  if( !pBRefer->IsSet(idx) ){
    while( idx < m_npieces && !pBRefer->IsSet(idx) ){
      pBChecked->Set(idx);
      ++idx;
    }
    f_checkint = 1;
    m_check_piece = idx;
  }
  if( idx < m_npieces ){
    // Don't use the cache for this (looks a bit ugly but helps performance).
    size_t tmp_cache_size = m_cache_size;
    m_cache_size = 0;
    int r = GetHashValue(idx, md);
    m_cache_size = tmp_cache_size;
    if( r < 0 ) return -1;

    pBChecked->Set(idx);  // need to set before CheckInterest below
    if( memcmp(md, m_hash_table + idx * 20, 20) == 0 ){
      m_left_bytes -= GetPieceLength(idx);
      pBF->Set(idx);
      WORLD.Tell_World_I_Have(idx);
      if( pBF->IsFull() ){
        WORLD.CloseAllConnectionToSeed();
      }
    } else f_checkint = 1;
    m_check_piece = idx + 1;
  }
  if( f_checkint ) WORLD.CheckInterest();

  if( m_check_piece >= m_npieces ){
    CONSOLE.Print("Checking completed.");
    m_btfiles.PrintOut();  // show file completion
    delete pBRefer;
  }
  return 0;
}

char* btContent::_file2mem(const char *fname, size_t *psiz)
{
  char *b = (char*) 0;
  struct stat sb;
  FILE* fp;
  fp = fopen(fname,"r");
  if( !fp ){
    CONSOLE.Warning(1, "error, open %s failed. %s",fname,strerror(errno));
    return (char*) 0;
  }

  if(stat(fname,&sb) < 0){
    CONSOLE.Warning(1, "error, stat %s failed, %s",fname,strerror(errno));
    return (char*) 0;
  }

  if( sb.st_size > MAX_METAINFO_FILESIZ ){
    CONSOLE.Warning(1, "error, %s is really a metainfo file???",fname);
    return (char*) 0;
  }

  b = new char[sb.st_size];
#ifndef WINDOWS
  if( !b ) return (char*) 0;
#endif

  if(fread(b, sb.st_size, 1, fp) != 1){
    if( ferror(fp) ){
      delete []b;
      return (char*) 0;
    }
  }
  fclose(fp);

  if(psiz) *psiz = sb.st_size;
  return b;
}

int btContent::APieceComplete(size_t idx)
{
  unsigned char md[20];
  if(pBF->IsSet(idx)) return 1;
  if( GetHashValue(idx, md) < 0 ){
    Uncache(idx);
    return -1;
  }

  if( memcmp(md,(m_hash_table + idx * 20), 20) != 0 ){
    CONSOLE.Warning(3, "warn, piece %d hash check failed.", idx);
    return 0;
  }
  pBF->Set(idx);
  m_left_bytes -= GetPieceLength(idx);
  FlushCache(idx);
  return 1;
}

int btContent::GetHashValue(size_t idx,unsigned char *md)
{
  if( ReadPiece(global_piece_buffer,idx) < 0 ) return -1;
  Sha1(global_piece_buffer,GetPieceLength(idx),md);
  return 0;
}

// This is btcontent's "IntervalCheck()"
int btContent::SeedTimeout()
{
  uint64_t dl;
  if( pBF->IsFull() ){
    if( !m_seed_timestamp ){
      Tracker.Reset(1);
      Self.ResetDLTimer();  // set/report dl rate = 0
      ReleaseHashTable();
      m_seed_timestamp = now;
      FlushCache();
      if( Self.TotalDL() > 0 ){
        CONSOLE.Print("Download complete.");
        CONSOLE.Print("Total time used: %ld minutes.",
          (long)((now - m_start_timestamp) / 60));
      }
      CONSOLE.Print_n("Seed for others %lu hours",
        (unsigned long)cfg_seed_hours);
      if( cfg_seed_ratio )
        CONSOLE.Print_n(" or to ratio of %.2f", cfg_seed_ratio);
      CONSOLE.Print("");
    }else if( now < m_seed_timestamp ) m_seed_timestamp = now;
    dl = (Self.TotalDL() > 0) ? Self.TotalDL() : GetTotalFilesLength();
    if( (cfg_seed_ratio == 0 && cfg_seed_hours == 0) ||
        (cfg_seed_hours > 0 &&
          (now - m_seed_timestamp) >= (cfg_seed_hours * 60 * 60)) ||
        (cfg_seed_ratio > 0 &&
          cfg_seed_ratio <= (double) Self.TotalUL() / dl) ) return 1;
  }
  if( cfg_cache_size && now >= m_cache_eval_time ) CacheEval();
  return 0;
}


size_t btContent::getFilePieces(size_t nfile)
{
   return m_btfiles.getFilePieces(nfile);
}


void btContent::SetFilter()
{
  FlushCache();
  m_btfiles.SetFilter(arg_file_to_download,pBFilter,m_piece_length, 1);
  WORLD.CheckInterest();
}


// Note, this functions assumes the program is exiting.
void btContent::SaveBitfield()
{
  if( arg_bitfield_file ){
    if( m_check_piece < m_npieces ){  // still checking
      pBChecked->Invert();
      pBRefer->And(*pBChecked);
      pBF->Comb(*pBRefer);
    }
    if( !pBF->IsFull() ) pBF->WriteToFile(arg_bitfield_file);
  }
}


void btContent::DumpCache()
{
  BTCACHE *p = m_cache_oldest;

  CONSOLE.Debug("CACHE CONTENTS:");
  for( ; p; p = p->age_next ){
    CONSOLE.Debug("  0x%llx: %d bytes %sflushed",
      p->bc_off, (int)(p->bc_len), p->bc_f_flush ? "un" : "");
  }
}

