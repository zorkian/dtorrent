#include "config.h"
#include "bitfield.h"

#ifdef WINDOWS
#include <io.h>
#include <memory.h>
#else
#include <unistd.h>
#include <sys/param.h>
#endif

#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef HAVE_RANDOM
#include "compat.h"
#endif

const unsigned char BIT_HEX[] = {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};

#define _isset(idx)     (b[(idx) / 8 ] & BIT_HEX[(idx) % 8])
#define _isempty()      (nset == 0)
#define _isempty_sp(sp) ((sp).nset == 0)
#define _isfull()       (nset >= nbits)
#define _isfull_sp(sp)  ((sp).nset >= nbits)

bt_index_t Bitfield::nbytes = 0;
bt_index_t Bitfield::nbits = 0;

Bitfield::Bitfield()
{
  b = new unsigned char[nbytes];
#ifndef WINDOWS
  if( !b ) throw 9;
#endif

  memset(b, 0, nbytes);
  nset = 0;
}

Bitfield::Bitfield(bt_index_t npcs)
{
  nbits = npcs;
  nbytes = nbits / 8;
  if( nbits % 8 ) nbytes++;

  b = new unsigned char[nbytes];
#ifndef WINDOWS
  if( !b ) throw 9;
#endif

  memset(b, 0, nbytes);
  nset = 0;
}

Bitfield::Bitfield(const Bitfield &bf)
{
  nset = bf.nset;
  if( _isfull_sp(bf) ) b = (unsigned char *)0;
  else{
    b = new unsigned char[nbytes];
#ifndef WINDOWS
    if( !b ) throw 9;
#endif
    memcpy(b, bf.b, nbytes);
  }
}

void Bitfield::operator=(const Bitfield &bf)
{
  nset = bf.nset;
  if( _isfull_sp(bf) ){
    if( b ){
      delete []b;
      b = (unsigned char *)0;
    }
  }else{
    if( !b ){
      b = new unsigned char[nbytes];
#ifndef WINDOWS
      if( !b ) throw 9;
#endif
    }
    memcpy(b, bf.b, nbytes);
  }
}

/* _set() sets the bit but doesn't increment nset or set the isfull case.
   Use instead of Set() when you know nset is incorrect and will be corrected
   afterward (as in Invert or by _recalc),
   and either bitfield won't get full or you'll _recalc() afterward to fix it.
*/
inline void Bitfield::_set(bt_index_t idx)
{
  if( idx < nbits && !_isfull() && !_isset(idx) )
    b[idx / 8] |= BIT_HEX[idx % 8];
}

inline void Bitfield::_setall(unsigned char *buf)
{
  memset(buf, 0xff, nbytes - 1);

  if( nbits % 8 ){
    buf[nbytes - 1] = ~(BIT_HEX[nbits % 8 - 1] - 1);
  }else
    buf[nbytes - 1] = (unsigned char) 0xFF;
}

// Compute a new value for nset from the actual bitfield data.
inline void Bitfield::_recalc()
{
  static unsigned char BITS[256] = {0xff};
  bt_index_t i;

  if( BITS[0] ){  // initialize bitcounts
    bt_index_t j, exp, x;
    BITS[0] = 0;
    x = 0;
    for( i = 0; i < 8; i++ ){
      exp = 1<<i;
      for( j = 0; j < exp; j++ )
        BITS[++x] = BITS[j] + 1;
    }
  }

  for( nset = 0, i = 0; i < nbytes; i++ )
    nset += BITS[b[i]];
  if( _isfull() && b ){
    delete []b;
    b = (unsigned char *)0;
  }
}

void Bitfield::SetAll()
{
  if( b ){
    delete []b;
    b = (unsigned char *)0;
  }
  nset = nbits;
}

void Bitfield::Clear()
{
  if( _isfull() ){
    b = new unsigned char[nbytes];
#ifndef WINDOWS
    if( !b ) throw 9;
#endif
  }
  memset(b, 0, nbytes);
  nset = 0;
}

int Bitfield::IsSet(bt_index_t idx) const
{
  if( idx >= nbits ) return 0;
  return _isfull() ? 1 : _isset(idx);
}

void Bitfield::Set(bt_index_t idx)
{
  if( idx >= nbits ) return;

  if( !_isfull() && !_isset(idx) ){
    b[idx / 8] |= BIT_HEX[idx % 8];
    nset++;
    if( _isfull() && b ){
      delete []b;
      b = (unsigned char *)0;
    }
  }
}

void Bitfield::UnSet(bt_index_t idx)
{
  if( idx >= nbits ) return;

  if( _isfull() ){
    b = new unsigned char[nbytes];
#ifndef WINDOWS
    if( !b ) throw 9;
#endif
    _setall(b);
    b[idx / 8] &= (~BIT_HEX[idx % 8]);
    nset = nbits - 1;
  }else{
    if( _isset(idx) ){
      b[idx / 8] &= (~BIT_HEX[idx % 8]);
      nset--;
    }
  }
}

void Bitfield::Invert()
{
  if( _isempty() ){
    SetAll();
  }else if( _isfull() ){
    Clear();
  }else{
    bt_index_t i = 0;
    bt_index_t s = nset;
    for( ; i < nbytes - 1; i++ ) b[i] = ~b[i];

    if( nbits % 8 ){
      for( i = 8 * (nbytes - 1); i < nbits; i++ ){
        if( _isset(i) ) UnSet(i);
        else _set(i);
      }
    }else b[nbytes - 1] = ~b[nbytes - 1];

    nset = nbits - s;
  }
}

void Bitfield::Or(const Bitfield &bf)
{
  if( !_isempty_sp(bf) && !_isfull() ){
    if( _isfull_sp(bf) ){
      SetAll();
    }else if( _isempty() ){
      memcpy(b, bf.b, nbytes);
      nset = bf.nset;
    }else{
      bt_index_t i;
      for( i = 0; i < nbytes; i++ )
        b[i] |= bf.b[i];
      _recalc();
    }
  }
}

void Bitfield::Except(const Bitfield &bf)
{
  if( !_isempty_sp(bf) && !_isempty() ){
    if( _isfull_sp(bf) ){
      Clear();
    }else{
      if( _isfull() ){
        b = new unsigned char[nbytes];
#ifndef WINDOWS
        if( !b ) throw 9;
#endif
        _setall(b);
      }
      for( bt_index_t i = 0; i < nbytes; i++ )
        b[i] &= ~bf.b[i];
      _recalc();
    }
  }
}

void Bitfield::And(const Bitfield &bf)
{
  if( !_isfull_sp(bf) && !_isempty() ){
    if( _isempty_sp(bf) ){
      Clear();
    }else{
      if( _isfull() ){
        b = new unsigned char[nbytes];
#ifndef WINDOWS
        if( !b ) throw 9;
#endif
        memcpy(b, bf.b, nbytes);
        nset = bf.nset;
      }else{
        for( bt_index_t i = 0; i < nbytes; i++ )
          b[i] &= bf.b[i];
        _recalc();
      }
    }
  }
}

bt_index_t Bitfield::Random() const
{
  bt_index_t idx;

  if( _isfull() ) idx = random() % nbits;
  else{
    bt_index_t i = random() % nset + 1;
    for( idx = 0; idx < nbits && i; idx++ )
      if( _isset(idx) ) i--;
    idx--;
  }
  return idx;
}

void Bitfield::SetReferBuffer(char *buf)
{
  if( !b ){
    b = new unsigned char[nbytes];
#ifndef WINDOWS
    if( !b ) throw 9;
#endif
  }
  memcpy(b, buf, nbytes);
  if( nbits % 8 )
    b[nbytes - 1] &= ~(BIT_HEX[nbits % 8 - 1] - 1);
  _recalc();
}

void Bitfield::WriteToBuffer(char *buf)
{
  if( _isfull() )
    _setall((unsigned char *)buf);
  else
    memcpy(buf, (char *)b, nbytes);
}

int Bitfield::SetReferFile(const char *fname)
{
  FILE *fp;
  struct stat sb;
  char *bitbuf = (char *)0;

  if( stat(fname, &sb) < 0 ) return -1;
  if( sb.st_size != nbytes ) return -1;

  fp = fopen(fname, "r");
  if( !fp ) return -1;

  bitbuf = new char[nbytes];
#ifndef WINDOWS
  if( !bitbuf ) goto fclose_err;
#endif

  if( fread(bitbuf, nbytes, 1, fp) != 1 ) goto fclose_err;

  fclose(fp);

  SetReferBuffer(bitbuf);

  delete []bitbuf;
  return 0;
 fclose_err:
  if( bitbuf ) delete []bitbuf;
  fclose(fp);
  return -1;
}

int Bitfield::WriteToFile(const char *fname)
{
  FILE *fp;
  char *bitbuf = (char *)0;

  fp = fopen(fname, "w");
  if( !fp ) return -1;

  bitbuf = new char[nbytes];
#ifndef WINDOWS
  if( !bitbuf ) goto fclose_err;
#endif

  WriteToBuffer(bitbuf);

  if( fwrite(bitbuf, nbytes, 1, fp) != 1 ) goto fclose_err;

  delete []bitbuf;
  fclose(fp);
  return 0;
 fclose_err:
  if( bitbuf ) delete []bitbuf;
  fclose(fp);
  return -1;
}

