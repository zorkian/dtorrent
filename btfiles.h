#ifndef BTFILES_H
#define BTFILES_H

#include "./def.h"
#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>
#include <time.h>

#include "bitfield.h"
#include "btconfig.h"

typedef struct _btfile{
  char *bf_filename;         // full path of file
  FILE *bf_fp;
  char *bf_buffer;           // IO buffer
  uint64_t bf_length;        // final size of file
  uint64_t bf_offset;        // torrent offset of file start
  uint64_t bf_size;          // current size of file
  time_t bf_last_timestamp;  // last IO timestamp
  size_t bf_npieces;         // number of pieces contained

  unsigned char bf_flag_opened:1;
  unsigned char bf_flag_readonly:1;
  unsigned char bf_flag_staging:1;
  unsigned char bf_reserved:5;

  struct _btfile *bf_next;
  struct _btfile *bf_nextreal;  // next non-staging file

  _btfile(){
    bf_flag_opened = bf_flag_readonly = bf_flag_staging = 0;
    bf_filename = (char*)0;
    bf_fp = (FILE*)0;
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
    bf_filename = (char*)0;
    bf_buffer = (char *)0;
    bf_next = bf_nextreal = (struct _btfile *)0;
  }
}BTFILE;


class btFiles
{
 private:
  
  BTFILE *m_btfhead;
  char *m_directory;
  uint64_t m_total_files_length;
  size_t m_total_opened;	// already opened
  size_t m_nfiles;
  int m_fsizelen;
  BTFILE **m_file;
  char *m_torrent_id;      // unique torrent identifier (ASCII)
  char *m_staging_path;    // main staging directory
  char *m_stagedir;        // current staging subdir for new files
  int m_stagecount;        // count of files in staging subdir

  uint8_t m_flag_automanage:1;
  uint8_t m_need_merge:1;
  uint8_t m_flag_reserved:6;

  int _btf_close_oldest();
  int _btf_close(BTFILE *pbf);
  int _btf_open(BTFILE *pbf, const int iotype);
  int ExtendFile(BTFILE *pbf);
  int _btf_ftruncate(int fd, uint64_t length);
  int _btf_destroy();
  int _btf_recurses_directory(const char *cur_path, BTFILE **plastnode);
  int ConvertFilename(char *dst, const char *src, int size);
  int MergeStaging(BTFILE *dst);
  int MergeAny() { return FindAndMerge(0, 1); }
  int FindAndMerge(int findall, int dostaging=0);
  int ExtendAll();
  int MkPath(const char *pathname);

 public:
  BitField *pBFPieces;

  int SetupFiles(const char *torrentid);
  int CreateFiles();
  void CloseFile(size_t nfile);

  btFiles();
  ~btFiles();
  
  int BuildFromFS(const char *pathname);
  int BuildFromMI(const char *metabuf, const size_t metabuf_len,
                  const char *saveas);

  char *GetDataName() const;
  uint64_t GetTotalLength() const { return m_total_files_length; }
  ssize_t IO(char *buf, uint64_t off, size_t len, const int iotype);
  size_t FillMetaInfo(FILE* fp);

  void SetFilter(int nfile, BitField *pFilter, size_t pieceLength);

  size_t GetNFiles() const { return m_nfiles; }
  char *GetFileName(size_t nfile) const;
  uint64_t GetFileSize(size_t nfile) const;
  size_t GetFilePieces(size_t nfile) const;

  int NeedMerge() const { return m_need_merge ? 1 : 0; }
  int MergeNext() { return FindAndMerge(0); }
  int MergeAll() { return FindAndMerge(1); }

  void PrintOut();
};

#endif
