#ifndef BENCODE_H
#define BENCODE_H

#include <inttypes.h>
#include <sys/types.h>
#include <stdio.h>

#include "bttypes.h"

#define KEY_SP '|'	//the keyname list's delimiters
#define KEYNAME_SIZ 32
#define KEYNAME_LISTSIZ 256

#define MAX_INT_SIZ 64

enum dt_query_t{
  DT_QUERY_STR,
  DT_QUERY_INT,
  DT_QUERY_POS,
};

size_t buf_long(const char *b,size_t len,char beginchar,char endchar,int64_t *pi);
size_t buf_int(const char *b,size_t len,char beginchar,char endchar,size_t *pi);
size_t buf_str(const char *b,size_t len,const char **pstr,size_t* slen);
size_t decode_int(const char *b,size_t len);
size_t decode_str(const char *b,size_t len);
size_t decode_dict(const char *b,size_t len,const char *keylist);
size_t decode_list(const char *b,size_t len,const char *keylist);
size_t decode_rev(const char *b,size_t len,const char *keylist);
size_t decode_query(const char *b,size_t len,const char *keylist,const char **ps,size_t *pz,int64_t *pi,dt_query_t method);
size_t decode_list2path(const char *b, size_t n, char *pathname);
int bencode_buf(const char *str,size_t len,FILE *fp);
int bencode_str(const char *str, FILE *fp);
int bencode_int(const int64_t integer, FILE *fp);
int bencode_begin_dict(FILE *fp);
int bencode_begin_list(FILE *fp);
int bencode_end_dict_list(FILE *fp);
int bencode_path2list(const char *pathname, FILE *fp);

#endif
