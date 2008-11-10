#include "btcontent.h"

#ifdef WINDOWS
#include <direct.h>
#include <io.h>
#include <memory.h>
#else
#include <unistd.h>
#include <sys/param.h>
#endif

#if defined(USE_STANDALONE_SHA1)
#include "sha1.h"
#elif defined(HAVE_OPENSSL_SHA_H)
#include <openssl/sha.h>
#elif defined(HAVE_SSL_SHA_H)
#include <ssl/sha.h>
#elif defined(HAVE_SHA_H)
#include <sha.h>
#endif

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

#include "ctorrent.h"
#include "btconfig.h"
#include "bencode.h"
#include "peer.h"
#include "httpencode.h"
#include "tracker.h"
#include "peerlist.h"
#include "ctcs.h"
#include "console.h"
#include "bttime.h"
#include "util.h"

#define MAX_METAINFO_FILESIZ (4*1024*1024)
#define FLUSH_RETRY_INTERVAL 300  // seconds to retry after disk write error

#define meta_str(keylist, pstr, psiz) \
  decode_query(b, flen, (keylist), (pstr), (psiz), (int64_t *)0, DT_QUERY_STR)
#define meta_int(keylist, pint) \
  decode_query(b, flen, (keylist), (const char **)0, (size_t *)0, (pint), \
    DT_QUERY_INT)
#define meta_pos(keylist) \
  decode_query(b, flen, (keylist), (const char **)0, (size_t *)0, \
    (int64_t *) 0, DT_QUERY_POS)

// Does "ca" overlap the data that lies from roff to rlen?
#define CACHE_FIT(ca, roff, rlen) \
  (max_datalen((ca)->bc_off, (roff)) <= \
   min_datalen(((ca)->bc_off + (ca)->bc_len - 1), (roff + rlen - 1)))


btContent BTCONTENT;


static void Sha1(char *ptr, size_t len, unsigned char *dm)
{
#if defined(USE_STANDALONE_SHA1)
  SHA1_CTX context;
  SHA1Init(&context);
  SHA1Update(&context, (unsigned char *)ptr, len);
  SHA1Final(dm, &context);
#else
#ifdef WINDOWS
  ;
#else
  SHA_CTX context;
  SHA1_Init(&context);
  SHA1_Update(&context, (unsigned char *)ptr, len);
  SHA1_Final(dm, &context);
#endif
#endif
}

btContent::btContent()
{
  m_announce = global_piece_buffer = (char *)0;
  global_buffer_size = 0;
  memset(m_announcelist, 0, 9*sizeof(char *));
  m_hash_table = (unsigned char *)0;
  m_create_date = m_seed_timestamp = (time_t)0;
  m_private = 0;
  m_comment = m_created_by = (char *)0;

  pBF = (Bitfield *)0;
  pBMasterFilter = (Bitfield *)0;
  pBRefer = (Bitfield *)0;
  pBChecked = (Bitfield *)0;
  pBMultPeer = (Bitfield *)0;
  time(&m_start_timestamp);
  m_cache_oldest = m_cache_newest = (BTCACHE *)0;
  m_cache_size = m_cache_used = 0;
  m_flush_failed = 0;
  m_flush_tried = (time_t)0;
  m_check_piece = 0;
  m_flushq = (BTFLUSH *)0;
  m_filters = m_current_filter = (BFNODE *)0;
  m_prevdlrate = 0;
  m_updated_remain = (time_t)0;
}

int btContent::CreateMetainfoFile(const char *mifn, const char *comment,
  bool isprivate)
{
  FILE *fp;
  fp = fopen(mifn, "r");
  if( fp ){
    CONSOLE.Warning(1, "error, file \"%s\" already exists.", mifn);
    return -1;
  }else if( ENOENT != errno ){
    CONSOLE.Warning(1, "error, couldn't create \"%s\".", mifn);
    return -1;
  }

  fp = fopen(mifn, "w");

  if( !fp ){
    CONSOLE.Warning(1, "error, open \"%s\" failed:  %s", mifn, strerror(errno));
    return -1;
  }
  if( bencode_begin_dict(fp) != 1 ) goto err;

  // Entries in dictionary must be sorted by key!

  // announce
  if( bencode_str("announce", fp) != 1 ) goto err;
  if( bencode_str(m_announce, fp) != 1 ) goto err;

  // comment
  if( comment ){
    if( bencode_str("comment", fp) != 1 ) goto err;
    if( bencode_str(comment, fp) != 1 ) goto err;
  }

  // created by
  if( bencode_str("created by", fp) != 1 ) goto err;
  if( bencode_str(*cfg_user_agent, fp) != 1 ) goto err;

  // creation date
  if( bencode_str("creation date", fp) != 1 ) goto err;
  if( bencode_int(m_create_date, fp) != 1 ) goto err;

  // info dict
  if( bencode_str("info", fp) != 1 ) goto err;
  if( bencode_begin_dict(fp) != 1 ) goto err;

  { // Entries in dictionary must be sorted by key!
    // files & name, or length & name
    if( m_btfiles.FillMetaInfo(fp) != 1 ) goto err;

    // piece length
    if( bencode_str("piece length", fp) != 1 ) goto err;
    if( bencode_int(m_piece_length, fp) != 1 ) goto err;

    // pieces (hash table)
    if( bencode_str("pieces", fp) != 1 ) goto err;
    if( bencode_buf((const char *)m_hash_table, m_hashtable_length, fp) != 1 )
      goto err;

    // private
    if( isprivate ){
      if( bencode_str("private", fp) != 1 ) goto err;
      if( bencode_int(1, fp) != 1 ) goto err;
    }

    if( bencode_end_dict_list(fp) != 1 ) goto err;  // end info
  }

  if( bencode_end_dict_list(fp) != 1 ) goto err;  // end torrent

  fclose(fp);
  return 0;
 err:
  if( fp ) fclose(fp);
  return -1;
}

int btContent::InitialFromFS(const char *pathname, char *ann_url,
  bt_length_t piece_length)
{
  bt_index_t n, percent;

  // piece length
  m_piece_length = piece_length;
  if( m_piece_length % 65536 ){
    m_piece_length /= 65536;
    m_piece_length *= 65536;
  }

  // This is really just a sanity check on the piece length to create.
  if( !m_piece_length || m_piece_length > 4096*1024 )
    m_piece_length = 262144;

  m_metainfo_file = pathname;
  m_announce = ann_url;
  m_create_date = time((time_t *)0);

  if( m_btfiles.BuildFromFS(pathname) < 0 ) return -1;

  global_piece_buffer = new char[m_piece_length];
#ifndef WINDOWS
  if( !global_piece_buffer ) return -1;
#endif
  global_buffer_size = m_piece_length;

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

int btContent::PrintOut() const
{
  CONSOLE.Print("META INFO");
  CONSOLE.Print("Announce: %s", m_announce);
  if( m_announcelist[0] ){
    CONSOLE.Print("Alternates:");
    for( int n=0; n < 9 && m_announcelist[n]; n++ )
      CONSOLE.Print(" %d. %s", n+1, m_announcelist[n]);
  }
  if( m_create_date ){
    char s[42];
#ifdef HAVE_CTIME_R_3
    ctime_r(&m_create_date, s, sizeof(s));
#else
    ctime_r(&m_create_date, s);
#endif
    if( s[strlen(s)-1] == '\n' ) s[strlen(s)-1] = '\0';
    CONSOLE.Print("Created On: %s", s);
  }
  CONSOLE.Print("Piece length: %lu", (unsigned long)m_piece_length);
  if( m_private ) CONSOLE.Print("Private: %s", m_private ? "Yes" : "No");
  if( m_comment ){
    char *s = new char[strlen(m_comment)+1];
    if( s ){
      strcpy(s, m_comment);
      for( char *t=s; *t; t++ )
        if( !isprint(*t) && !strchr("\t\r\n", *t) ) *t = '?';
      CONSOLE.Print("Comment: %s", s);
      delete []s;
    }
  }
  if( m_created_by ) CONSOLE.Print("Created with: %s", m_created_by);
  m_btfiles.PrintOut();
  return 0;
}

int btContent::InitialFromMI(const char *metainfo_fname, const char *saveas,
  const char *announce)
{
  unsigned char *ptr = m_shake_buffer;
  char *b, *tmpstr;
  const char *s;
  size_t flen, q, bsiz;
  int64_t bint;
  int check_pieces = 0;
  char torrentid[41];

  m_cache_hit = m_cache_miss = m_cache_pre = 0;
  time(&m_cache_eval_time);

  m_metainfo_file = metainfo_fname;
  b = _file2mem(metainfo_fname, &flen);
  if( !b ) return -1;

  // announce
  if( !meta_str("announce", &s, &bsiz) ) goto err;
  if( bsiz > MAXPATHLEN ) goto err;
  tmpstr = new char [bsiz + 1];
  memcpy(tmpstr, s, bsiz);
  tmpstr[bsiz] = '\0';
  m_announce = tmpstr;

  // announce-list
  if( (bsiz = meta_pos("announce-list")) ){
    const char *sptr;
    size_t slen;
    int n = 0;
    if( (q = decode_list(b+bsiz, flen-bsiz, (char *)0)) ){
      size_t alend = bsiz + q;
      bsiz++;  // 'l'
      while( bsiz < alend && *(b+bsiz) != 'e' && n < 9 ){  // each list
        if( !(q = decode_list(b+bsiz, alend-bsiz, (char *)0)) ) break;
        bsiz++;  // 'l'
        while( bsiz < alend && n < 9 ){  // each value
          if( !(q = buf_str(b+bsiz, alend-bsiz, &sptr, &slen)) )
            break;  // next list
          bsiz += q;
          if( strncasecmp(m_announce, sptr, slen) ){
            m_announcelist[n] = new char[slen+1];
            memcpy(m_announcelist[n], sptr, slen);
            (m_announcelist[n])[slen] = '\0';
            n++;
          }
        }
        bsiz++;  // 'e'
      }
    }
  }

  if( meta_int("creation date", &bint) ) m_create_date = (time_t)bint;
  if( meta_str("comment", &s, &bsiz) && bsiz ){
    if( (m_comment = new char[bsiz + 1]) ){
      memcpy(m_comment, s, bsiz);
      m_comment[bsiz] = '\0';
    }
  }
  if( meta_str("created by", &s, &bsiz) && bsiz ){
    if( (m_created_by = new char[bsiz + 1]) ){
      memcpy(m_created_by, s, bsiz);
      m_created_by[bsiz] = '\0';
    }
  }

  // infohash
  if( !(bsiz = meta_pos("info")) ) goto err;
  if( !(q = decode_dict(b + bsiz, flen - bsiz, (char *) 0)) ) goto err;
  Sha1(b + bsiz, q, m_shake_buffer + 28);

  // private flag
  if( meta_int("info|private", &bint) ) m_private = bint;

  // hash table
  if( !meta_str("info|pieces", &s, &bsiz) || m_hashtable_length % 20 != 0 )
    goto err;
  m_hashtable_length = bsiz;

  if( !arg_flg_exam_only ){
    m_hash_table = new unsigned char[m_hashtable_length];
#ifndef WINDOWS
    if( !m_hash_table ) goto err;
#endif
    memcpy(m_hash_table, s, m_hashtable_length);
  }

  if( !meta_int("info|piece length", &bint) ) goto err;
  m_piece_length = (bt_length_t)bint;
  m_npieces = m_hashtable_length / 20;

  if( m_piece_length < *cfg_req_slice_size )
    cfg_req_slice_size = m_piece_length;

  if( m_btfiles.BuildFromMI(b, flen, saveas) < 0 ) goto err;

  delete []b;
  b = (char *)0;

  if( arg_flg_exam_only ){
    PrintOut();
    return 0;
  }else{
    arg_flg_exam_only = true;
    PrintOut();
    arg_flg_exam_only = false;
  }

  for( int i=0; i < 20; i++ ){
    sprintf(torrentid + i*2, "%.2x", (int)m_shake_buffer[28+i]);
  }
  if( (check_pieces = m_btfiles.SetupFiles(torrentid)) < 0 ) goto err;

  global_piece_buffer = new char[DEFAULT_SLICE_SIZE];
#ifndef WINDOWS
  if( !global_piece_buffer ) goto err;
#endif
  global_buffer_size = DEFAULT_SLICE_SIZE;

  pBF = new Bitfield(m_npieces);
#ifndef WINDOWS
  if( !pBF ) goto err;
#endif

  pBRefer = new Bitfield(m_npieces);
#ifndef WINDOWS
  if( !pBRefer ) goto err;
#endif

  pBChecked = new Bitfield(m_npieces);
#ifndef WINDOWS
  if( !pBChecked ) goto err;
#endif

  pBMultPeer = new Bitfield(m_npieces);
#ifndef WINDOWS
  if( !pBMultPeer ) goto err;
#endif

  // create the file filter
  pBMasterFilter = new Bitfield(m_npieces);
#ifndef WINDOWS
  if( !pBMasterFilter ) goto err;
#endif
  if( *cfg_file_to_download ) SetFilter();

  check_pieces *= m_btfiles.CreateFiles();

  m_left_bytes = m_btfiles.GetTotalLength() / m_piece_length;
  if( m_btfiles.GetTotalLength() % m_piece_length ) m_left_bytes++;
  if( m_left_bytes != m_npieces ) goto err;

  m_left_bytes = m_btfiles.GetTotalLength();

  if( arg_flg_check_only ){
    struct stat sb;
    if( *cfg_bitfield_file && stat(*cfg_bitfield_file, &sb) == 0 ){
      if( remove(*cfg_bitfield_file) < 0 ){
        CONSOLE.Warning(2, "warn, couldn't delete bit field file \"%s\":  %s",
          *cfg_bitfield_file, strerror(errno));
      }
    }
    if( check_pieces ){
      if( CheckExist() < 0 ) goto err;
      if( !pBF->IsEmpty() )
        m_btfiles.PrintOut(); // show file completion
    }
    CONSOLE.Print("Already/Total: %d/%d (%d%%)", (int)pBF->Count(),
      (int)m_npieces, (int)(100 * pBF->Count() / m_npieces));
    if( !arg_flg_force_seed_mode ){
      if( *cfg_completion_exit ) CompletionCommand();
      return 0;
    }
  }else if( check_pieces ){  // files exist already
    if( *cfg_bitfield_file && pBRefer->SetReferFile(*cfg_bitfield_file) < 0 ){
      if( !arg_flg_force_seed_mode ){
        CONSOLE.Warning(2,
          "warn, couldn't set bit field refer file \"%s\":  %s",
          *cfg_bitfield_file, strerror(errno));
        CONSOLE.Warning(2, "This is normal if you are seeding.");
      }
      pBRefer->SetAll();  // need to check all pieces
    }else{
      CONSOLE.Interact("Found bit field file; %s previous state.",
        arg_flg_force_seed_mode ? "resuming download from" : "verifying");
      if( *cfg_bitfield_file && remove(*cfg_bitfield_file) < 0 ){
        CONSOLE.Warning(2, "warn, couldn't delete bit field file \"%s\":  %s",
          *cfg_bitfield_file, strerror(errno));
      }
      // Mark missing pieces as "checked" (eligible for download).
    }
    pBRefer->And(m_btfiles.pBFPieces);
    *pBChecked = *pBRefer;
    pBChecked->Invert();
  }
  if( !check_pieces ){  // don't hash-check if the files were just created
    m_check_piece = m_npieces;
    pBChecked->SetAll();
    if( arg_flg_force_seed_mode ){
      CONSOLE.Warning(2, "Files were not present; overriding force mode!");
    }
  }else if( arg_flg_force_seed_mode && !arg_flg_check_only ){
    bt_index_t idx = 0;
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
  }
  delete pBRefer;

  m_cache = new BTCACHE *[m_npieces];
  if( !m_cache ){
    CONSOLE.Warning(1, "error, allocate cache index failed");
    goto err;
  }
  memset(m_cache, 0, m_npieces * sizeof(BTCACHE *));
  CacheConfigure();

  *ptr++ = (unsigned char)19;              // protocol string length
  memcpy(ptr, "BitTorrent protocol", 19);  // protocol string
  ptr += 19;
  memset(ptr, 0, 8);                       // reserved bytes

  { // peer id
    const char *sptr = *cfg_peer_prefix;
    char *dptr = (char *)m_shake_buffer + 48;
    char *eptr = dptr + PEER_ID_LEN;
    while( *sptr ) *dptr++ = *sptr++;
    while( dptr < eptr )
      *dptr++ = (unsigned char)RandBits(sizeof(unsigned char) * 8);
    cfg_peer_prefix.Lock();
  }

  if( announce ){
    int n;
    delete []m_announce;
    if( (n = atoi(announce)) && n <= 9 && m_announcelist[n-1] ){
      m_announce = m_announcelist[n-1];
      delete []announce;
    }
    else m_announce = announce;
    CONSOLE.Print("Using announce URL:  %s", m_announce);
  }

  return 0;

 err:
  if( b ) delete []b;
  return -1;
}

btContent::~btContent()
{
  if( m_hash_table ) delete []m_hash_table;
  if( m_announce ) delete []m_announce;
  if( global_piece_buffer ) delete []global_piece_buffer;
  if( pBF ) delete pBF;
  if( m_metainfo_file ) delete []m_metainfo_file;
}

// returns <0 if error; if using cache: 1 if read from disk, 0 otherwise
int btContent::ReadSlice(char *buf, bt_index_t idx, bt_offset_t off,
  bt_length_t len)
{
  int retval = 0;
  dt_datalen_t offset = (dt_datalen_t)idx * (dt_datalen_t)m_piece_length + off;

  if( !m_cache_size ) return buf ? FileIO(buf, NULL, offset, len) : 0;
  else{
    bt_length_t len2;
    BTCACHE *p;

    p = m_cache[idx];
    while( len && p ){
      while( p && offset + len > p->bc_off && !CACHE_FIT(p, offset, len) ){
        p = p->bc_next;
      }
      if( !p || !CACHE_FIT(p, offset, len) ) break;
      if( offset < p->bc_off ){
        len2 = p->bc_off - offset;
        if( CacheIO(buf, NULL, offset, len2, 0) < 0 ) return -1;
        retval = 1;
        if( buf ) m_cache_miss += len2 / DEFAULT_SLICE_SIZE +
                                  ((len2 % DEFAULT_SLICE_SIZE) ? 1 : 0);
        else m_cache_pre += len2 / DEFAULT_SLICE_SIZE +
                            ((len2 % DEFAULT_SLICE_SIZE) ? 1 : 0);
        p = m_cache[idx];  // p may not be valid after CacheIO
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
          m_cache_hit += len2 / DEFAULT_SLICE_SIZE +
                         ((len2 % DEFAULT_SLICE_SIZE) ? 1 : 0);
        }else{  // prefetch only, update the age
          if( m_cache_newest != p ){
            if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
            else p->age_prev->age_next = p->age_next;
            p->age_next->age_prev = p->age_prev;
            m_cache_newest->age_next = p;
            p->age_next = (BTCACHE *)0;
            p->age_prev = m_cache_newest;
            m_cache_newest = p;
          }
        }
        p = p->bc_next;
      }

      if( buf ) buf += len2;
      offset += len2;
      len -= len2;
    }  // end while

    if( len ){
      if( buf ) m_cache_miss += len / DEFAULT_SLICE_SIZE +
                                ((len % DEFAULT_SLICE_SIZE) ? 1 : 0);
      else m_cache_pre += len / DEFAULT_SLICE_SIZE +
                          ((len % DEFAULT_SLICE_SIZE) ? 1 : 0);
      retval = CacheIO(buf, NULL, offset, len, 0);
      return (retval < 0) ? retval : 1;
    }
  }
  return retval;
}


inline void btContent::CacheClean(bt_length_t need)
{
  CacheClean(need, m_npieces);
}

/* idx is a piece we wish to avoid expiring */
void btContent::CacheClean(bt_length_t need, bt_index_t idx)
{
  BTCACHE *p, *pnext;
  int f_flush = 0;

  if( m_flush_failed && now >= m_flush_tried + FLUSH_RETRY_INTERVAL && need )
    FlushCache();  // try again

  again:
  for( p=m_cache_oldest; p && m_cache_size < m_cache_used + need; p=pnext ){
    pnext = p->age_next;
    if( f_flush && p->bc_f_flush && !m_flush_failed ){
      if( FlushPiece(p->bc_off / m_piece_length) ){
        pnext = m_cache_oldest;
        continue;
      }
    }
    if( !p->bc_f_flush ){
      if( !f_flush && idx == p->bc_off / m_piece_length ) continue;
      if(*cfg_verbose)
        CONSOLE.Debug("Expiring %d/%d/%d", (int)(p->bc_off / m_piece_length),
          (int)(p->bc_off % m_piece_length), (int)p->bc_len);

      if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
      else p->age_prev->age_next = p->age_next;
      if( m_cache_newest == p ) m_cache_newest = p->age_prev;
      else p->age_next->age_prev = p->age_prev;

      if( p->bc_prev ) p->bc_prev->bc_next = p->bc_next;
      else m_cache[p->bc_off / m_piece_length] = p->bc_next;
      if( p->bc_next ) p->bc_next->bc_prev = p->bc_prev;

      m_cache_used -= p->bc_len;
      delete []p->bc_buf;
      delete p;
    }
  }
  if( m_cache_size < m_cache_used + need ){  // still not enough
    if( m_cache_size < (*cfg_cache_size)*1024U*1024U ){  // can alloc more
      m_cache_size = (m_cache_used + need > (*cfg_cache_size)*1024U*1024U) ?
        (*cfg_cache_size)*1024U*1024U : (m_cache_used + need);
    }
    if( m_cache_size < m_cache_used + need && m_cache_used && !f_flush ){
      if(*cfg_verbose) CONSOLE.Debug("CacheClean flushing to obtain space");
      f_flush = 1;
      goto again;
    }  // else we tried...
  }
}

// Don't call this function if cfg_cache_size==0 !
void btContent::CacheEval()
{
  BTCACHE *p = m_cache_oldest;
  time_t interval;
  dt_mem_t unflushed = 0, dlnext, upadd = 0, upmax = 0, upmin = 0, total;

  dt_rate_t rateup = Self.RateUL();
  dt_rate_t ratedn = Self.RateDL();
  dt_count_t unchoked = WORLD.GetUnchoked();

  // Time until next cache size eval: unchoke interval or time to dl a piece.
  if( ratedn ){
    interval = (time_t)(m_piece_length / ratedn);
    if( interval > WORLD.GetUnchokeInterval() )
      interval = WORLD.GetUnchokeInterval();
    else if( 0==interval ) interval = 1;
  }else interval = WORLD.GetUnchokeInterval();

  /* Download: total unflushed data + data to dl before next eval
     Hold the first piece a bit to let uploading begin. */
  if( pBF->IsFull() ) dlnext = 0;
  else{
    if( pBF->Count() < 2 ) unflushed = m_cache_used;
    else for( ; p; p = p->age_next )
      if( p->bc_f_flush ) unflushed += p->bc_len;
    /* Make sure we can read back and check a completed piece.
       But free some cache if download has completely stalled. */
    dlnext = ratedn ? (ratedn * interval + m_piece_length) : 0;
  }

  // Upload: need enough to hold read/dl'd data until it can be sent
  upmin = DEFAULT_SLICE_SIZE * unchoked;
  upmax = (*cfg_cache_size)*1024U*1024U;
  if( pBF->IsFull() ){
    /* Seed mode.  All cache data is prefetched, and we don't normally need to
       keep prefetched data longer than 2.5 unchoke intervals. */
    if( rateup && unchoked ){
      // A very slow peer can't possibly benefit from cache--don't grow for it.
      dt_rate_t slowest = (dt_rate_t)( 1 + DEFAULT_SLICE_SIZE /
                           ((double)(*cfg_cache_size)*1024*1024 / rateup) );
      /* Lead cache: data we need to cache to keep the slowest up's data cached
         Add a slice per up for timing uncertainty */
      if( (slowest = WORLD.GetSlowestUp(slowest)) )
        upadd = DEFAULT_SLICE_SIZE * ( rateup / slowest + unchoked-1 );
      else upadd = DEFAULT_SLICE_SIZE * unchoked;

      upmin = DEFAULT_SLICE_SIZE * unchoked;
      upmax = (dt_mem_t)(DEFAULT_SLICE_SIZE * (unchoked-1) +
                         rateup * 2.5 * WORLD.GetUnchokeInterval());
    }
  }else{
    if( rateup > ratedn ){
      dt_rate_t slowest = (dt_rate_t)( 1 +
        *cfg_req_slice_size * ((double)ratedn / (*cfg_cache_size)*1024*1024) +
         DEFAULT_SLICE_SIZE * ((double)rateup / (*cfg_cache_size)*1024*1024) );
      if( (slowest = WORLD.GetSlowestUp(slowest)) )
        /* lead cache is how much we'll use while uploading a slice to slowest
           (default_slice_size / slowest) * (ratedn + rateup) */
        upadd = (dt_mem_t)( ((double)DEFAULT_SLICE_SIZE / slowest) *
                            (ratedn + rateup + 1) );
      else upadd = m_piece_length * unchoked;
    }
    else if( rateup ){
      // same as m_piece_length / (cfg_cache_size*1024*1024 / (double)ratedn)
      dt_rate_t slowest = (dt_rate_t)( 1 +
        ratedn * ((double)m_piece_length / ((*cfg_cache_size)*1024*1024)) );
      if( (slowest = WORLD.GetSlowestUp(slowest)) ){
        /* m_piece_length / (double)slowest * ratedn
           optimize, then round up a piece and add a piece */
        upadd = m_piece_length * (ratedn / slowest + 2);
      }else{  // gimme 10 seconds worth (unchoke interval)
        /* Can't keep pieces in cache long enough to upload them.
           Rely on prefetching slices from disk instead. */
        upadd = ratedn * WORLD.GetUnchokeInterval() +
                DEFAULT_SLICE_SIZE * unchoked;
      }
    }
  }

  if( upadd < upmin ) upadd = upmin;

  // Add a slice to round up
  total = unflushed + dlnext + upadd + *cfg_req_slice_size;

  // Limit to max configured size
  if( total > (*cfg_cache_size)*1024U*1024U )
    total = (*cfg_cache_size)*1024U*1024U;

  // Don't decrease cache size if flush failed.
  if( !m_flush_failed || total > m_cache_size ) m_cache_size = total;

  if(*cfg_verbose)
    CONSOLE.Debug("DL need: %dK  UL need: %dK  Cache: %dK  Used: %dK",
      (int)((unflushed+dlnext)/1024), (int)(upadd/1024),
      (int)(m_cache_size/1024), (int)(m_cache_used/1024));
  m_cache_eval_time = now + interval;
}

void btContent::CacheConfigure()
{
  if( *cfg_cache_size ){
    if( *cfg_cache_size > GetTotalFilesLength()/1024/1024 )
      cfg_cache_size = (GetTotalFilesLength()+1024*1024-1)/1024/1024;
    else CacheEval();
  }else m_cache_size = 0;

  if( m_cache_size < m_cache_used && !m_flush_failed ) CacheClean(0);
}

int btContent::NeedFlush() const
{
  if( m_flush_failed ){
    return (now >= m_flush_tried + FLUSH_RETRY_INTERVAL) ? 1 : 0;
  }else{
    return (m_flushq ||
            (m_cache_oldest && m_cache_oldest->bc_f_flush &&
             m_cache_used >=
               (*cfg_cache_size)*1024U*1024U - *cfg_req_slice_size + 1)) ?
           1 : 0;
  }
}

void btContent::FlushCache()
{
  if(*cfg_verbose) CONSOLE.Debug("Flushing all cache");
  for( bt_index_t i=0; i < m_npieces; i++ ){
    if( m_cache[i] ) FlushPiece(i);
    if( m_flush_failed ) break;
  }
  if( !NeedMerge() && !m_flushq && Seeding() ) CloseAllFiles();
}

// Returns 1 if cache aging changed.
int btContent::FlushPiece(bt_index_t idx)
{
  BTCACHE *p;
  int retval = 0;

  if(*cfg_verbose){
    if( pBF->IsSet(idx) )
      CONSOLE.Debug("Writing piece #%d to disk", (int)idx);
    else CONSOLE.Debug("Flushing piece #%d", (int)idx);
  }
  p = m_cache[idx];

  for( ; p; p = p->bc_next ){
    /* Update the age if piece is complete, as this should mean we've just
       completed the piece and made it available. */
    if( pBF->IsSet(idx) && m_cache_newest != p ){
      if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
      else p->age_prev->age_next = p->age_next;
      p->age_next->age_prev = p->age_prev;
      m_cache_newest->age_next = p;
      p->age_next = (BTCACHE *)0;
      p->age_prev = m_cache_newest;
      m_cache_newest = p;
      retval = 1;
    }
    if( p->bc_f_flush ) FlushEntry(p);
  }

  return retval;
}

void btContent::FlushEntry(BTCACHE *p)
{
  if( m_flush_failed && now < m_flush_tried + FLUSH_RETRY_INTERVAL ){
    // Delay until next retry.
    return;
  }
  if( p->bc_f_flush && FileIO(NULL, p->bc_buf, p->bc_off, p->bc_len) == 0 ){
    p->bc_f_flush = 0;
    if( m_flush_failed ){
      m_flush_failed = 0;
      CONSOLE.Warning(3, "Flushing cache succeeded%s.",
        Seeding() ? "" : "; resuming download");
      CacheConfigure();
      WORLD.CheckInterest();
    }
  }
}

// Returns -1 for convenience (can return this function's value)
int btContent::WriteFail()
{
  m_flush_tried = now;
  if( !m_flush_failed )
    m_cache_size += *cfg_req_slice_size * WORLD.GetDownloads() * 2;
  CONSOLE.Warning(1, "warn, write file failed while flushing data.");
  CONSOLE.Warning(1,
    "You need to have at least %llu bytes free on this filesystem!",
    (unsigned long long)(m_left_bytes + m_cache_used));
  CONSOLE.Warning(1,
    "This could also be caused by a conflict or disk error.");
  if( !IsFull() ||
      (!m_flush_failed && m_cache_size > (*cfg_cache_size)*1024U*1024U) ){
    CONSOLE.Warning(1, "Temporarily %s%s...",
      IsFull() ? "" : "suspending download",
      (!m_flush_failed && m_cache_size > (*cfg_cache_size)*1024U*1024U) ?
        (IsFull() ? "increasing cache" : " and increasing cache") : "");
  }
  m_flush_failed = 1;
  WORLD.StopDownload();
  return -1;
}

void btContent::Uncache(bt_index_t idx)
{
  BTCACHE *p, *pnext;

  p = m_cache[idx];
  for( ; p; p = pnext ){
    pnext = p->bc_next;
     if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
     else p->age_prev->age_next = p->age_next;
     if( m_cache_newest == p ) m_cache_newest = p->age_prev;
     else p->age_next->age_prev = p->age_prev;

     m_cache_used -= p->bc_len;
     delete []p->bc_buf;
     delete p;
  }
  m_cache[idx] = (BTCACHE *)0;
}

void btContent::FlushQueue()
{
  if( m_flushq ){
    FlushPiece(m_flushq->idx);
    if( !m_flush_failed ){
      BTFLUSH *goner = m_flushq;
      m_flushq = m_flushq->next;
      delete goner;
    }
  }else FlushPiece(m_cache_oldest->bc_off / m_piece_length);

  if( !NeedMerge() && !m_flushq && Seeding() ){
      CloseAllFiles();
      CONSOLE.Print("Finished flushing data.");
  }
}

/* Prepare for prefetching a whole piece.
   return -1:  do not prefetch (problem or not needed)
   return  0:  already ready (no time used)
   return  1:  data was flushed (time used)
*/
int btContent::CachePrep(bt_index_t idx)
{
  int retval = 0;
  BTCACHE *p, *pnext;
  bt_length_t need = GetPieceLength(idx);

  if( m_cache_size < m_cache_used + need ){
    for( p=m_cache[idx]; p; p=p->bc_next ) need -= p->bc_len;
    if( 0==need ) retval = -1;  // don't need to prefetch
    for( p=m_cache_oldest; p && m_cache_size < m_cache_used + need; p=pnext ){
      pnext = p->age_next;
      if( p->bc_off / m_piece_length == idx ) continue;
      if( p->bc_f_flush && !m_flush_failed ){
        retval = 1;
        if( FlushPiece(p->bc_off / m_piece_length) ){
          pnext = m_cache_oldest;
          continue;
        }
      }
      if(*cfg_verbose)
        CONSOLE.Debug("Expiring %d/%d/%d", (int)(p->bc_off / m_piece_length),
          (int)(p->bc_off % m_piece_length), (int)p->bc_len);
      if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
      else p->age_prev->age_next = p->age_next;
      if( m_cache_newest == p ) m_cache_newest = p->age_prev;
      else p->age_next->age_prev = p->age_prev;

      if( p->bc_prev ) p->bc_prev->bc_next = p->bc_next;
      else m_cache[p->bc_off / m_piece_length] = p->bc_next;
      if( p->bc_next ) p->bc_next->bc_prev = p->bc_prev;

      m_cache_used -= p->bc_len;
      delete []p->bc_buf;
      delete p;
    }
  }
  return retval;
}

int btContent::WriteSlice(const char *buf, bt_index_t idx, bt_offset_t off,
  bt_length_t len)
{
  dt_datalen_t offset = (dt_datalen_t)idx * (dt_datalen_t)m_piece_length + off;

  if( !m_cache_size && FileIO(NULL, buf, offset, len) == 0 ){
    return 0;
    // save it in cache if write failed
  }else{
    bt_length_t len2;
    BTCACHE *p;

    p = m_cache[idx];
    while( len && p ){
      while( p && offset + len > p->bc_off && !CACHE_FIT(p, offset, len) ){
        p = p->bc_next;
      }
      if( !p || !CACHE_FIT(p, offset, len) ) break;
      if( offset < p->bc_off ){
        len2 = p->bc_off - offset;
        if( CacheIO(NULL, buf, offset, len2, 1) < 0 ) return -1;
        p = m_cache[idx];  // p may not be valid after CacheIO
      }else{
        if( offset > p->bc_off ){
          len2 = p->bc_off + p->bc_len - offset;
          if( len2 > len ) len2 = len;
          memcpy(p->bc_buf + (offset - p->bc_off), buf, len2);
        }else{
          len2 = (len > p->bc_len) ? p->bc_len : len;
          memcpy(p->bc_buf, buf, len2);
        }
        p->bc_f_flush = 1;
        // re-received this data, make it new again
        if( m_cache_newest != p ){
          if( m_cache_oldest == p ) m_cache_oldest = p->age_next;
          else p->age_prev->age_next = p->age_next;
          p->age_next->age_prev = p->age_prev;
          m_cache_newest->age_next = p;
          p->age_next = (BTCACHE *)0;
          p->age_prev = m_cache_newest;
          m_cache_newest = p;
        }
        p = p->bc_next;
      }

      buf += len2;
      offset += len2;
      len -= len2;
    }  // end while

    if( len ) return CacheIO(NULL, buf, offset, len, 1);
  }
  return 0;
}

// Put data into the cache (receiving data, or need to read from disk).
int btContent::CacheIO(char *rbuf, const char *wbuf, dt_datalen_t off,
  bt_length_t len, int method)
{
  BTCACHE *p;
  BTCACHE *pp = (BTCACHE *)0;
  BTCACHE *pnew = (BTCACHE *)0;
  bt_index_t idx = off / m_piece_length;

  if( len >= (*cfg_cache_size)*1024U*768U ){  // 75% of cache limit
    if( rbuf || wbuf ) return FileIO(rbuf, wbuf, off, len);
    else return 0;
  }

  if( *cfg_verbose && 0==method )
    CONSOLE.Debug("Read to %s %d/%d/%d", rbuf ? "buffer" : "cache",
      (int)idx, (int)(off % m_piece_length), (int)len);

  if( m_cache_size < m_cache_used + len ){
    if( 0==method && !pBF->IsSet(idx) ) CacheClean(len, idx);
    else CacheClean(len);
    /* Note, there is no failure code from CacheClean().  If nothing can be
       done to increase the cache size, we allocate what we need anyway. */
  }

  if( 0==method && rbuf && FileIO(rbuf, wbuf, off, len) < 0 )
    return -1;

  pnew = new BTCACHE;
#ifndef WINDOWS
  if( !pnew )
    return (method && wbuf) ? FileIO(rbuf, wbuf, off, len) : 0;
#endif

  pnew->bc_buf = new char[len];
#ifndef WINDOWS
  if( !(pnew->bc_buf) ){
    delete pnew;
    return (method && wbuf) ? FileIO(rbuf, wbuf, off, len) : 0;
  }
#endif

  if( rbuf || wbuf ) memcpy(pnew->bc_buf, method ? wbuf : rbuf, len);
  else if( 0==method && FileIO(pnew->bc_buf, wbuf, off, len) < 0 ){
    delete []pnew->bc_buf;
    delete pnew;
    return -1;
  }
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

/* Perform file I/O, handling failures.
   Method (read/write) is determined by which buffer is given. */
inline int btContent::FileIO(char *rbuf, const char *wbuf, dt_datalen_t off,
  bt_length_t len)
{
  return ( m_btfiles.IO(rbuf, wbuf, off, len) < 0 ) ?
             (wbuf ? WriteFail() : -1) : 0;
}

inline int btContent::ReadPiece(char *buf, bt_index_t idx)
{
  return ReadSlice(buf, idx, 0, GetPieceLength(idx));
}

bt_length_t btContent::GetPieceLength(bt_index_t idx) const
{
  /* Slight optimization to avoid division in every call.  The second test is
     still needed in case the torrent size is exactly n pieces. */
  return (idx == m_npieces - 1 &&
          idx == m_btfiles.GetTotalLength() / m_piece_length) ?
    (bt_length_t)(m_btfiles.GetTotalLength() % m_piece_length) :
    m_piece_length;
}

int btContent::CheckExist()
{
  bt_index_t idx = 0;
  bt_index_t percent = GetNPieces() / 100;
  unsigned char md[20];

  if( !percent ) percent = 1;

  CONSOLE.Interact_n("");
  for( ; idx < m_npieces; idx++ ){
    if( m_btfiles.pBFPieces->IsSet(idx) ){
      if( GetHashValue(idx, md) < 0 ){
        CONSOLE.Warning(1, "Error while checking piece %d of %d",
          (int)idx+1, (int)m_npieces);
        return -1;
      }
      if( memcmp(md, m_hash_table + idx * 20, 20) == 0 ){
         m_left_bytes -= GetPieceLength(idx);
         pBF->Set(idx);
      }
    }
    if( idx % percent == 0 || idx == m_npieces-1 )
      CONSOLE.InteractU("Check exist: %d/%d", idx+1, m_npieces);
  }
  m_check_piece = m_npieces;
  pBChecked->SetAll();
  return 0;
}

int btContent::CheckNextPiece()
{
  bt_index_t idx = m_check_piece;
  unsigned char md[20];
  int f_checkint = 0;

  if( idx >= m_npieces ) return 0;
  if( pBChecked->IsSet(idx) ){
    while( idx < m_npieces && pBChecked->IsSet(idx) ){
      if(*cfg_verbose) CONSOLE.Debug("Check: %u skipped", idx);
      pBChecked->Set(idx);
      ++idx;
    }
    f_checkint = 1;
    m_check_piece = idx;
  }
  if( idx < m_npieces ){
    // Don't use the cache for this (looks a bit ugly but helps performance).
    dt_mem_t tmp_cache_size = m_cache_size;
    m_cache_size = 0;
    int r = GetHashValue(idx, md);
    m_cache_size = tmp_cache_size;
    if( r < 0 ) return -1;

    pBChecked->Set(idx);  // need to set before CheckInterest below
    if( memcmp(md, m_hash_table + idx * 20, 20) == 0 ){
      if(*cfg_verbose) CONSOLE.Debug("Check: %u ok", idx);
      m_left_bytes -= GetPieceLength(idx);
      pBF->Set(idx);
      WORLD.Tell_World_I_Have(idx);
    }else{
      if(*cfg_verbose) CONSOLE.Debug("Check: %u failed", idx);
      f_checkint = 1;
    }
    m_check_piece = idx + 1;
  }
  if( f_checkint ) WORLD.CheckInterest();

  if( m_check_piece >= m_npieces ){
    CONSOLE.Print("Checking completed.");
    if( !pBF->IsEmpty() )
      m_btfiles.PrintOut();  // show file completion
    if( pBF->IsFull() ){
      WORLD.CloseAllConnectionToSeed();
    }
  }
  return 0;
}

char *btContent::_file2mem(const char *fname, size_t *psiz)
{
  char *b = (char *)0;
  struct stat sb;
  FILE *fp;
  fp = fopen(fname, "r");
  if( !fp ){
    CONSOLE.Warning(1, "error, open \"%s\" failed:  %s", fname,
      strerror(errno));
    return (char *)0;
  }

  if( stat(fname, &sb) < 0 ){
    CONSOLE.Warning(1, "error, stat \"%s\" failed:  %s", fname,
      strerror(errno));
    return (char *)0;
  }

  if( sb.st_size > MAX_METAINFO_FILESIZ ){
    CONSOLE.Warning(1, "error, \"%s\" is really a metainfo file?", fname);
    return (char *)0;
  }

  b = new char[sb.st_size];
#ifndef WINDOWS
  if( !b ) return (char *)0;
#endif

  if( fread(b, sb.st_size, 1, fp) != 1 ){
    if( ferror(fp) ){
      delete []b;
      return (char *)0;
    }
  }
  fclose(fp);

  if( psiz ) *psiz = sb.st_size;
  return b;
}

int btContent::APieceComplete(bt_index_t idx)
{
  unsigned char md[20];
  if( pBF->IsSet(idx) ) return 1;
  if( GetHashValue(idx, md) < 0 ){
    // error reading data
    Uncache(idx);
    return -1;
  }

  if( memcmp(md, (m_hash_table + idx * 20), 20) != 0 ){
    CONSOLE.Warning(3, "warn, piece %d hash check failed.", idx);
    Uncache(idx);
    CountHashFailure();
    return 0;
  }

  pBF->Set(idx);
  m_left_bytes -= GetPieceLength(idx);
  Tracker.CountDL(GetPieceLength(idx));

  // Add the completed piece to the flush queue.
  if( *cfg_cache_size ){
    BTFLUSH *last = m_flushq;
    BTFLUSH *node = new BTFLUSH;
    if( !node ) FlushPiece(idx);
    else{
      node->idx = idx;
      node->next = (BTFLUSH *)0;
      if( last ){
        for( ; last->next; last = last->next);
        last->next = node;
      }else m_flushq = node;
    }
  }

  return 1;
}

int btContent::GetHashValue(bt_index_t idx, unsigned char *md)
{
  if( global_buffer_size < m_piece_length ){
    delete []global_piece_buffer;
    global_piece_buffer = new char[m_piece_length];
    global_buffer_size = global_piece_buffer ? m_piece_length : 0;
  }
  if( ReadPiece(global_piece_buffer, idx) < 0 ) return -1;
  Sha1(global_piece_buffer, GetPieceLength(idx), md);
  return 0;
}

// This is btcontent's "IntervalCheck()"
int btContent::SeedTimeout()
{
  dt_datalen_t dl;
  dt_rate_t oldrate = m_prevdlrate;

  if( Seeding() && (!m_flush_failed || IsFull()) &&
      (!*cfg_completion_exit || (!m_flushq && !NeedMerge())) ){
    if( !m_seed_timestamp ){
      if( IsFull() ){
        Tracker.Reset(15);
        ReleaseHashTable();
        cfg_file_to_download.Hide();
      }
      Self.ResetDLTimer();  // set/report dl rate = 0
      m_prevdlrate = 0;
      m_seed_timestamp = now;
      // Free global buffer prior to CompletionCommand fork (reallocate after).
      delete []global_piece_buffer;
      global_piece_buffer = (char *)0;
      if( Self.TotalDL() > 0 ){
        CONSOLE.Print("Download complete.");
        CONSOLE.Print("Total time used: %ld minutes.",
          (long)((now - m_start_timestamp) / 60));
        if(*cfg_verbose) CONSOLE.cpu();
        if( *cfg_completion_exit )
          CompletionCommand();
      }
      cfg_completion_exit.Hide();
      // Reallocate global buffer for uploading.
      global_piece_buffer = new char[DEFAULT_SLICE_SIZE];
      global_buffer_size = global_piece_buffer ? DEFAULT_SLICE_SIZE : 0;
      if(*cfg_ctcs) CTCS.Send_Status();
      CONSOLE.Print_n("Seed for others %lu hours",
        (unsigned long)*cfg_seed_hours);
      if( *cfg_seed_ratio )
        CONSOLE.Print_n(" or to ratio of %.2f", *cfg_seed_ratio);
      CONSOLE.Print("");
      cfg_seed_remain.Unhide();
    }else if( now < m_seed_timestamp ) m_seed_timestamp = now;
    dl = (Self.TotalDL() > 0) ? Self.TotalDL() : GetTotalFilesLength();
    if( (*cfg_seed_ratio == 0 && *cfg_seed_hours == 0) ||
        (*cfg_seed_hours > 0 &&
          (now - m_seed_timestamp) >= (time_t)(*cfg_seed_hours * 60 * 60)) ||
        (*cfg_seed_ratio > 0 &&
          *cfg_seed_ratio <= (double) Self.TotalUL() / dl) ){
      if( m_flush_failed ){
        if( !WORLD.IsPaused() ){
          CONSOLE.Warning(1,
            "Seeding completed but cache flush failed; pausing...");
          WORLD.Pause();
        }
      }else return 1;
    }
    if( now >= m_updated_remain + 1 ){
      cfg_seed_remain.Override( (*cfg_seed_time - (now - m_seed_timestamp)) /
                                3600.0 );
      m_updated_remain = now;
    }
  }else{
    m_prevdlrate = Self.RateDL();
    if( m_prevdlrate == 0 && oldrate > 0 &&
        global_buffer_size > DEFAULT_SLICE_SIZE ){
      delete []global_piece_buffer;
      global_piece_buffer = new char[DEFAULT_SLICE_SIZE];
      global_buffer_size = global_piece_buffer ? DEFAULT_SLICE_SIZE : 0;
    }
  }
  if( (*cfg_cache_size && now >= m_cache_eval_time) ||
      (oldrate == 0 && m_prevdlrate > 0) ){
    CacheEval();
  }
  return 0;
}


void btContent::CompletionCommand()
{
  const char *pt, *pd, *pw;
  char *cmdstr, wd[MAXPATHLEN];
  int nt=0, nd=0, nw=0;

  if( !*cfg_completion_exit || !**cfg_completion_exit ) return;

  pt = pd = pw = *cfg_completion_exit;
  while( (pt = strstr(pt, "&t")) ){
    nt++;
    pt += 2;
  }
  while( (pd = strstr(pd, "&d")) ){
    nd++;
    pd+=2;
  }
  while( (pw = strstr(pw, "&w")) ){
    nw++;
    pw+=2;
  }

  if( nw && !getcwd(wd, MAXPATHLEN) ){
    CONSOLE.Warning(2, "warn, couldn't get working directory:  %s",
      strerror(errno));
    return;
  }
  cmdstr = new char[1 + strlen(*cfg_completion_exit) +
    nt * (strlen(m_metainfo_file) - 2) +
    nd * (strlen(m_btfiles.GetDataName()) - 2) +
    nw * (strlen(wd) - 2)];
  if( !cmdstr ){
    CONSOLE.Warning(2,
      "warn, failed to allocate memory for completion command");
    return;
  }
  strcpy(cmdstr, *cfg_completion_exit);

  if( nt || nd || nw ){
    char *pt, *pd, *pw;
    const char *ptmp, *parg = *cfg_completion_exit;

    pt = strstr(cmdstr, "&t");
    pd = strstr(cmdstr, "&d");
    pw = strstr(cmdstr, "&w");
    while( pt || pd || pw ){
      if( pt && (!pd || pt < pd) && (!pw || pt < pw) ){
        strcpy(pt, m_metainfo_file);
        ptmp = cmdstr + strlen(cmdstr);
        parg = strstr(parg, "&t") + 2;
        strcat(pt, parg);
        pt = strstr(ptmp, "&t");
        if( pd ) pd = strstr(ptmp, "&d");
        if( pw ) pw = strstr(ptmp, "&w");
      }
      if( pd && (!pt || pd < pt) && (!pw || pd < pw) ){
        strcpy(pd, m_btfiles.GetDataName());
        ptmp = cmdstr + strlen(cmdstr);
        parg = strstr(parg, "&d") + 2;
        strcat(pd, parg);
        pd = strstr(ptmp, "&d");
        if( pt ) pt = strstr(ptmp, "&t");
        if( pw ) pw = strstr(ptmp, "&w");
      }
      if( pw && (!pt || pw < pt) && (!pd || pw < pd) ){
        strcpy(pw, wd);
        ptmp = cmdstr + strlen(cmdstr);
        parg = strstr(parg, "&w") + 2;
        strcat(pw, parg);
        pw = strstr(ptmp, "&w");
        if( pt ) pt = strstr(ptmp, "&t");
        if( pd ) pd = strstr(ptmp, "&d");
      }
    }
  }
  if(*cfg_verbose) CONSOLE.Debug("Executing: %s", cmdstr);
#ifdef HAVE_WORKING_FORK
  if( *cfg_cache_size ){  // maybe free some cache before forking
    CacheEval();
    if( m_cache_size < m_cache_used && !m_flush_failed ) CacheClean(0);
  }
  pid_t r;
  if( (r = fork()) < 0 ){
    CONSOLE.Warning(2, "warn, fork failed running completion command:  %s",
      strerror(errno));
  }else if( r==0 ){
    g_secondary_process = true;
    if( m_cache_used ){  // free the cache in the child process
      BTCACHE *p, *pnext;
      for( p=m_cache_oldest; p; p=pnext ){
        pnext = p->age_next;
        delete []p->bc_buf;
        delete p;
      }
    }
    WORLD.CloseAll();  // deallocate peers
#endif
    if( system(cmdstr) < 0 )
      CONSOLE.Warning(2, "warn, failure running completion command:  %s",
        strerror(errno));
#ifdef HAVE_WORKING_FORK
    Exit(EXIT_SUCCESS);
  }
#endif
  delete []cmdstr;
}


void btContent::CheckFilter()
{
  Bitfield tmpBitfield;
  BFNODE *original = m_current_filter;

  if( !m_filters ) return;
  if( !m_current_filter ) m_current_filter = m_filters;

  while( m_current_filter ){
    tmpBitfield = *pBF;           // what I have...
    tmpBitfield.Or(GetFilter());  // ...plus what I don't want
    if( !tmpBitfield.IsFull() ) break;
    m_current_filter = m_current_filter->next;
  }

  if( !m_current_filter ){
    if( !IsFull() ) CONSOLE.Print("End of download files list.");
    for( BFNODE *goner=m_filters; goner; goner=m_current_filter ){
      m_current_filter = goner->next;
      delete goner;
    }
    m_filters = (BFNODE *)0;
  }

  if( m_current_filter != original ){
    if( m_current_filter ){
      int last;
      tmpBitfield = *GetFilter();
      tmpBitfield.Invert();      // what I want...
      tmpBitfield.Except(*pBF);  // ...that I don't have
      last = tmpBitfield.IsSet(m_npieces-1) ? 1 : 0;
      if( GetFilter()->IsEmpty() )
        CONSOLE.Print("Downloading remaining files");
      else CONSOLE.Print("Downloading file(s): %s", m_current_filter->name);
      CONSOLE.Print( "Pieces: %d (%llu bytes)", (int)tmpBitfield.Count(),
        (unsigned long long)
          ((tmpBitfield.Count() - last) * (dt_datalen_t)m_piece_length +
           (last ? GetPieceLength(m_npieces-1) : 0)) );
    }
  }

  if( m_seed_timestamp && m_current_filter ){
    // was seeding, now downloading again
    m_seed_timestamp = (time_t)0;
    cfg_seed_remain.Hide();
    cfg_completion_exit.Unhide();
  }
}


void btContent::SetFilter()
{
  // Set up filter list
  char *list=(char *)0, *tok, *dash, *plus;
  dt_count_t start, end;
  Bitfield tmpFilter, *pfilter;
  BFNODE *node=m_filters, *pnode=(BFNODE *)0;

  if( *cfg_file_to_download ){
    pBMasterFilter->SetAll();
    list = new char[strlen(*cfg_file_to_download) + 1];
    if( !list ){
      CONSOLE.Warning(1, "error, failed to allocate memory for filter");
      return;
    }
    strcpy(list, *cfg_file_to_download);
    tok = strtok(list, ", ");
    while( tok ){
      if( !node ){
        node = new BFNODE;
        if( !node ){
          CONSOLE.Warning(1, "error, failed to allocate memory for filter");
          return;
        }
        if( pnode ) pnode->next = node;
        else m_filters = node;
      }

      if( node->name && strlen(node->name) < strlen(tok) ){
        delete []node->name;
        node->name = (char *)0;
      }
      if( !node->name ){
        node->name = new char[strlen(tok)+1];
        if( !node ){
          CONSOLE.Warning(1, "error, failed to allocate memory for filter");
          return;
        }
      }
      strcpy(node->name, tok);

      pfilter = &(node->bitfield);
      if( strstr(tok, "...") || strchr(tok, '*') ){
        pfilter->Clear();
        pBMasterFilter->Clear();
        pnode = node;
        node = node->next;
        break;
      }
      pfilter->SetAll();
      do{
        start = (dt_count_t)atoi(tok);
        m_btfiles.SetFilter(start, &tmpFilter, m_piece_length);
        pfilter->And(tmpFilter);

        plus = strchr(tok, '+');

        if( (dash = strchr(tok, '-')) && (!plus || dash < plus) ){
          end = (dt_count_t)atoi(dash + 1);
          while( ++start <= end ){
            m_btfiles.SetFilter(start, &tmpFilter, m_piece_length);
            pfilter->And(tmpFilter);
          }
        }

        tok = plus ? plus+1 : plus;
      }while( tok );

      pBMasterFilter->And(*pfilter);
      tok = strtok(NULL, ", ");
      pnode = node;
      node = node->next;
    }
    delete []list;
  }else  // no cfg_file_to_download
    pBMasterFilter->Clear();

  if( m_filters && m_filters->bitfield.IsEmpty() ){
    cfg_file_to_download = (const char *)0;
    pBMasterFilter->Clear();
    node = m_filters;
    pnode = (BFNODE *)0;
  }

  if( node ){
    if( m_filters == node ) m_filters = (BFNODE *)0;
    if( pnode ) pnode->next = (BFNODE *)0;
    for( BFNODE *goner=node; goner; goner=node ){
      node = goner->next;
      delete goner;
    }
  }

  m_current_filter = (BFNODE *)0;
  CheckFilter();
  WORLD.CheckInterest();
}


const Bitfield *btContent::GetNextFilter(const Bitfield *pfilter) const
{
  static BFNODE *p = m_filters;

  if( !pfilter ) p = m_filters;
  else if( p && &(p->bitfield) == pfilter ){
    p = p->next;
  }else{
    for( p=m_filters; p && (&(p->bitfield) != pfilter); p = p->next );
    if( p ) p = p->next;
    else p = m_filters;
  }

  if( p ) return &(p->bitfield);
  else return (Bitfield *)0;
}


int btContent::Seeding() const
{
  if( IsFull() || m_flush_failed ) return 1;
  if( *cfg_file_to_download && !m_current_filter ) return 1;
  return 0;
}


// Note, this function assumes the program is exiting.
void btContent::SaveBitfield()
{
  if( *cfg_bitfield_file ){
    if( m_check_piece < m_npieces ){  // still checking
      // Anything unchecked needs to be checked next time.
      pBChecked->Invert();
      pBF->Or(*pBChecked);
    }
    if( !pBF->IsFull() ){
      if( pBF->WriteToFile(*cfg_bitfield_file) < 0 ){
        CONSOLE.Warning(1, "error writing bitfield file %s:  %s",
          *cfg_bitfield_file, strerror(errno));
      }
    }
  }
}


void btContent::CountDupBlock(bt_length_t len)
{
  m_dup_blocks++;
  Tracker.CountDL(len);
}


void btContent::CloseAllFiles()
{
  for( dt_count_t n=1; n <= m_btfiles.GetNFiles(); n++ )
    m_btfiles.CloseFile(n);  // files will reopen read-only
}


void btContent::MergeNext()
{
  if( !m_flush_failed ){
    m_btfiles.MergeNext();
    if( !NeedMerge() && !m_flushq && Seeding() ){
      CloseAllFiles();
      CONSOLE.Print("Finished merging staged data.");
    }
  }
}


/* Opportunity to influence download piece selection by criteria unknown to
   the peer object.
*/
bt_index_t btContent::ChoosePiece(const Bitfield &choices,
  const Bitfield &available, bt_index_t preference) const
{
  return m_btfiles.ChoosePiece(choices, available, preference);
}


void btContent::DumpCache() const
{
  BTCACHE *p = m_cache_oldest;
  int count;

  CONSOLE.Debug("CACHE CONTENTS:");
  count = 0;
  for( ; p; p = p->age_next ){
    CONSOLE.Debug("  %p prev=%p %d/%d/%d %sflushed",
      p, p->age_prev,
      (int)(p->bc_off / m_piece_length), (int)(p->bc_off % m_piece_length),
      (int)p->bc_len,
      p->bc_f_flush ? "un" : "");
    count++;
  }
  CONSOLE.Debug("  count=%d", count);
  CONSOLE.Debug("  newest=%p", m_cache_newest);

  CONSOLE.Debug("BY PIECE:");
  count = 0;
  for( bt_index_t idx=0; idx < m_npieces; idx++ ){
    for( p=m_cache[idx]; p; p=p->bc_next ){
      CONSOLE.Debug("  %p prev=%p %d/%d/%d %sflushed",
        p, p->bc_prev,
        (int)(p->bc_off / m_piece_length), (int)(p->bc_off % m_piece_length),
        (int)p->bc_len,
        p->bc_f_flush ? "un" : "");
      count++;
    }
  }
  CONSOLE.Debug("  count=%d", count);
}

