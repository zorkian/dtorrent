#ifndef BTFILES_H
#define BTFILES_H

#include "def.h"
#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <time.h>

#include "bttypes.h"
#include "bitfield.h"
#include "btconfig.h"

enum dt_alloc_t{
  DT_ALLOC_SPARSE = 0,
  DT_ALLOC_FULL   = 1,
  DT_ALLOC_NONE   = 2
};

typedef struct _btfile{
  char *bf_filename;         // full path of file
  FILE *bf_fp;
  char *bf_buffer;           // IO buffer
  dt_datalen_t bf_length;    // final size of file
  dt_datalen_t bf_offset;    // torrent offset of file start
  dt_datalen_t bf_size;      // current size of file
  time_t bf_last_timestamp;  // last IO timestamp
  bt_index_t bf_npieces;     // number of pieces contained

  unsigned char bf_flag_opened:1;
  unsigned char bf_flag_readonly:1;
  unsigned char bf_flag_staging:1;
  unsigned char bf_reserved:5;

  struct _btfile *bf_next;
  struct _btfile *bf_nextreal;  // next non-staging file

  _btfile(){
    bf_flag_opened = bf_flag_readonly = bf_flag_staging = 0;
    bf_filename = (char *)0;
    bf_fp = (FILE *)0;
    bf_length = bf_offset = bf_size = 0;
    bf_buffer = (char *)0;
    bf_last_timestamp = (time_t)0;
    bf_npieces = 0;
    bf_next = bf_nextreal = (struct _btfile *)0;
  }

  ~_btfile(){
    if( bf_fp && bf_flag_opened ) fclose( bf_fp );
    if( bf_filename ) delete []bf_filename;
    if( bf_buffer ) delete []bf_buffer;
    bf_filename = (char *)0;
    bf_buffer = (char *)0;
    bf_next = bf_nextreal = (struct _btfile *)0;
  }
}BTFILE;


class btFiles
{
 private:
  BTFILE *m_btfhead;
  char *m_directory;
  dt_datalen_t m_total_files_length;
  dt_count_t m_total_opened;  // already opened
  dt_count_t m_nfiles;
  size_t m_fsizelen;
  BTFILE **m_file;
  char *m_torrent_id;         // unique torrent identifier (ASCII)
  char *m_staging_path;       // main staging directory
  char *m_stagedir;           // current staging subdir for new files
  int m_stagecount;           // count of files in staging subdir

  uint8_t m_flag_automanage:1;
  uint8_t m_need_merge:1;
  uint8_t m_flag_reserved:6;

  int _btf_close_oldest();
  int _btf_close(BTFILE *pbf);
  int _btf_open(BTFILE *pbf, const int iotype);
  int ExtendFile(BTFILE *pbf);
  int _btf_ftruncate(int fd, dt_datalen_t length);
  int _btf_destroy();
  int _btf_recurses_directory(const char *cur_path, BTFILE **plastnode);
  int ConvertFilename(char *dst, const char *src, int size);
  int MergeStaging(BTFILE *dst);
  int MergeAny(){ return FindAndMerge(0, 1); }
  int FindAndMerge(int findall, int dostaging=0);
  int ExtendAll();
  int MkPath(const char *pathname);

 public:
  Bitfield *pBFPieces;

  int SetupFiles(const char *torrentid);
  int CreateFiles();
  void CloseFile(dt_count_t nfile);

  btFiles();
  ~btFiles();

  int BuildFromFS(const char *pathname);
  int BuildFromMI(const char *metabuf, const size_t metabuf_len,
                  const char *saveas);

  const char *GetDataName() const;
  dt_datalen_t GetTotalLength() const { return m_total_files_length; }
  int IO(char *rbuf, const char *wbuf, dt_datalen_t off, bt_length_t len);
  int FillMetaInfo(FILE *fp);

  void SetFilter(dt_count_t nfile, Bitfield *pFilter, bt_length_t pieceLength);

  dt_count_t GetNFiles() const { return m_nfiles; }
  const char *GetFileName(dt_count_t nfile) const;
  dt_datalen_t GetFileSize(dt_count_t nfile) const;
  bt_index_t GetFilePieces(dt_count_t nfile) const;

  int NeedMerge() const { return m_need_merge ? 1 : 0; }
  int MergeNext(){ return FindAndMerge(0); }
  int MergeAll(){ return FindAndMerge(1); }
  bt_index_t ChoosePiece(const Bitfield &choices, const Bitfield &available,
    bt_index_t preference) const;

  void PrintOut() const;
};

#endif  // BTFILES_H

