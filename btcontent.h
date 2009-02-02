#ifndef BTCONTENT_H
#define BTCONTENT_H

#include "def.h"
#include <inttypes.h>
#include <sys/types.h>

#include <time.h>
#include "bttypes.h"
#include "bitfield.h"
#include "btfiles.h"
#include "tracker.h"

typedef struct _btcache{
  dt_datalen_t bc_off;
  bt_length_t bc_len;

  unsigned char bc_f_flush:1;
  unsigned char bc_f_reserved:7;

  char *bc_buf;

  struct _btcache *bc_next;
  struct _btcache *bc_prev;
  struct _btcache *age_next;
  struct _btcache *age_prev;
}BTCACHE;

typedef struct _btflush{
  bt_index_t idx;
  struct _btflush *next;
}BTFLUSH;

typedef struct _bfnode{
  char *name;
  Bitfield bitfield;
  struct _bfnode *next;

  _bfnode(){
    name = (char *)0;
    next = (struct _bfnode *)0;
  }
  ~_bfnode(){
    if(name) delete []name;
  }
}BFNODE;

class btContent
{
 private:
  const char *m_metainfo_file;
  // torrent metainfo data
  const char *m_announce;
  unsigned char *m_hash_table;
  unsigned char m_shake_buffer[68];
  char *m_comment, *m_created_by;
  time_t m_create_date;
  int m_private;

  bt_index_t m_hashtable_length;
  bt_length_t m_piece_length;
  bt_index_t m_npieces, m_check_piece;
  time_t m_seed_timestamp, m_start_timestamp;
  dt_datalen_t m_left_bytes;

  btFiles m_btfiles;

  unsigned char m_flush_failed:1;
  unsigned char m_reserved:7;

  time_t m_flush_tried;

  BTCACHE **m_cache, *m_cache_oldest, *m_cache_newest;
  dt_mem_t m_cache_size, m_cache_used;
  dt_count_t m_cache_hit, m_cache_miss, m_cache_pre;
  time_t m_cache_eval_time;
  BTFLUSH *m_flushq;

  BFNODE *m_filters, *m_current_filter;

  dt_rate_t m_prevdlrate;
  dt_count_t m_hash_failures, m_dup_blocks, m_unwanted_blocks;

  time_t m_updated_remain;

  char *_file2mem(const char *fname, size_t *psiz);

  void ReleaseHashTable(){
    if( m_hash_table ){
      delete []m_hash_table;
      m_hash_table = (unsigned char *)0;
    }
  }

  int CheckExist();
  void CacheClean(bt_length_t need);
  void CacheClean(bt_length_t need, bt_index_t idx);
  void CacheEval();
  dt_datalen_t max_datalen(dt_datalen_t a, dt_datalen_t b){
    return (a > b) ? a : b;
  }
  dt_datalen_t min_datalen(dt_datalen_t a, dt_datalen_t b){
    return (a > b) ? b : a;
  }
  int ReadPiece(char *buf, bt_index_t idx);
  int CacheIO(char *rbuf, const char *wbuf, dt_datalen_t off, bt_length_t len,
    int method);
  int FileIO(char *rbuf, const char *wbuf, dt_datalen_t off, bt_length_t len);
  void FlushEntry(BTCACHE *p);
  int WriteFail();

 public:
  Bitfield *pBF;
  Bitfield *pBMasterFilter;
  Bitfield *pBRefer;
  Bitfield *pBChecked;
  Bitfield *pBMultPeer;
  char *global_piece_buffer;
  bt_length_t global_buffer_size;

  btContent();
  ~btContent();

  void CacheConfigure();
  void FlushCache();
  int FlushPiece(bt_index_t idx);
  void Uncache(bt_index_t idx);
  void FlushQueue();
  int NeedFlush() const;
  int FlushFailed() const { return m_flush_failed ? 1 : 0; }
  int NeedMerge() const { return m_btfiles.NeedMerge(); }
  void MergeNext();
  void MergeAll(){ m_btfiles.MergeAll(); }
  bt_index_t ChoosePiece(const Bitfield &choices, const Bitfield &available,
    bt_index_t preference) const;

  int CreateMetainfoFile(const char *mifn, const char *comment, bool isprivate);
  int InitialFromFS(const char *pathname, bt_length_t piece_length);
  int InitialFromMI(const char *metainfo_fname, const char *saveas,
    bool force_seed, bool check_only, bool exam_only);

  int CheckNextPiece();
  bt_index_t CheckedPieces() const { return m_check_piece; }

  const char *GetMetainfoFile() const { return m_metainfo_file; }

  const unsigned char *GetShakeBuffer() const { return m_shake_buffer; }
  const unsigned char *GetInfoHash() const { return (m_shake_buffer + 28); }
  const unsigned char *GetPeerId() const { return (m_shake_buffer + 48); }

  bt_length_t GetPieceLength(bt_index_t idx) const;
  bt_length_t GetPieceLength() const { return m_piece_length; }
  bt_index_t GetNPieces() const { return m_npieces; }

  dt_datalen_t GetTotalFilesLength() const {
    return m_btfiles.GetTotalLength(); }
  dt_datalen_t GetLeftBytes() const { return m_left_bytes; }
  dt_datalen_t GetNeedBytes() const;

  int APieceComplete(bt_index_t idx);
  int GetHashValue(bt_index_t idx, unsigned char *md);

  bool CachePrep(bt_index_t idx);
  int ReadSlice(char *buf, bt_index_t idx, bt_offset_t off, bt_length_t len);
  int WriteSlice(const char *buf, bt_index_t idx, bt_offset_t off,
    bt_length_t len);

  int PrintOut() const;
  int SeedTimeout();
  void CompletionCommand();
  void SaveBitfield();

  void CheckFilter();
  void SetFilter();
  const Bitfield *GetFilter() const {
    return m_current_filter ? &(m_current_filter->bitfield) : (Bitfield *)0;
  }
  const Bitfield *GetNextFilter() const { return GetNextFilter((Bitfield *)0); }
  const Bitfield *GetNextFilter(const Bitfield *pfilter) const;
  const char *GetFilterName() const { return m_current_filter->name; }
  void SetTmpFilter(int nfile, Bitfield *pFilter){
    m_btfiles.SetFilter(nfile, pFilter, m_piece_length);
  }

  dt_count_t GetNFiles() const { return m_btfiles.GetNFiles(); }
  const char *GetFileName(dt_count_t nfile) const {
    return m_btfiles.GetFileName(nfile);
  }
  dt_datalen_t GetFileSize(dt_count_t nfile) const {
    return m_btfiles.GetFileSize(nfile);
  }
  bt_index_t GetFilePieces(dt_count_t nfile) const {
    return m_btfiles.GetFilePieces(nfile);
  }

  time_t GetStartTime() const { return m_start_timestamp; }
  time_t GetSeedTime() const { return m_seed_timestamp; }

  dt_count_t GetHashFailures() const { return m_hash_failures; }
  dt_count_t GetDupBlocks() const { return m_dup_blocks; }
  dt_count_t GetUnwantedBlocks() const { return m_unwanted_blocks; }
  void CountHashFailure(){ m_hash_failures++; }
  void CountDupBlock(bt_length_t len);
  void CountUnwantedBlock(){ m_unwanted_blocks++; }

  int IsFull() const { return pBF->IsFull(); }
  int Seeding() const;

  dt_count_t CacheHits() const { return m_cache_hit; }
  // "miss" does not count prefetch reads from disk
  dt_count_t CacheMiss() const { return m_cache_miss; }
  // instead, "pre" does
  dt_count_t CachePre() const { return m_cache_pre; }
  dt_mem_t CacheSize() const { return m_cache_size; }
  dt_mem_t CacheUsed() const { return m_cache_used; }

  void CloseAllFiles();

  void DumpCache() const;
};

extern btContent BTCONTENT;


#endif  // BTCONTENT_H

