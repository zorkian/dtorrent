#ifndef BITFIELD_H
#define BITFIELD_H

#include <sys/types.h>
#include "bttypes.h"

class Bitfield
{
 private:
  static bt_index_t nbits;
  static bt_index_t nbytes;

  unsigned char *b;
  bt_index_t nset;

  void _recalc();
  void _setall(unsigned char *buf);
  void _set(bt_index_t idx);

 public:
  Bitfield();
  Bitfield(bt_index_t n_bits);
  Bitfield(const Bitfield &bf);
  ~Bitfield(){ if(b) delete []b; }

  void operator=(const Bitfield &bf);

  void SetAll();
  void Clear();
  void Set(bt_index_t idx);
  void UnSet(bt_index_t idx);

  int IsSet(bt_index_t idx) const;
  int IsFull() const { return (nset >= nbits) ? 1 : 0; }
  int IsEmpty() const { return (nset == 0) ? 1 : 0; }

  bt_index_t Count() const { return nset; }
  bt_index_t NBytes() const { return nbytes; }
  bt_index_t NBits() const { return nbits; }
  bt_index_t Random() const;

  void Or(const Bitfield &bf);
  void Or(const Bitfield *pbf){ if(pbf) Or(*pbf); }
  void Except(const Bitfield &bf);
  void Except(const Bitfield *pbf){ if(pbf) Except(*pbf); }
  void And(const Bitfield &bf);
  void And(const Bitfield *pbf){ if(pbf) And(*pbf); }
  void Invert();

  void SetReferBuffer(const char *buf);
  void WriteToBuffer(char *buf);
  int SetReferFile(const char *fname);
  int WriteToFile(const char *fname);
};

#endif  // BITFIELD_H
