// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#ifndef __MDS_ADSL_MDSTYPES_H__
#define __MDS_ADSL_MDSTYPES_H__

#include "include/encoding.h"

struct dirfrag_pot_load_t {
  double value;
  int last_epoch;
  dirfrag_pot_load_t() : value(0.0), last_epoch(-1) {}
  void _update_epoch(int epoch);
  void inc(int epoch);
  void adjust(double adj, int epoch);
  void add(dirfrag_pot_load_t & anotherpot);

  double pot_load(int epoch = -1);
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator & bl);
};
WRITE_CLASS_ENCODER(dirfrag_pot_load_t)

// --- DEPRECATED ---
/*
struct lunule_mds_load_t {
  mds_load_t pop_load;
  dirfrag_pot_load_t lul_load;
  double alpha;
  double beta;
	
  explicit lunule_mds_load_t(mds_load_t & pop_load, dirfrag_pot_load_t lul_load, double alpha, double beta) {}

  double mds_load();
  void encode(bufferlist& bl) const;
  void decode(bufferlist::iterator & bl);
};
WRITE_CLASS_ENCODER(lunule_mds_load_t)
*/

#endif /* mds/adsl/mdstypes.h */
