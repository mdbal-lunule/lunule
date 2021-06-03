// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
#include "mdstypes.h" // local

void dirfrag_pot_load_t::_update_epoch(int epoch)
{
  if (epoch > last_epoch) {
    last_value = (epoch - last_epoch == 1) ? value : 0.0;
    value = value/4;
    last_epoch = epoch;
  }
}

void dirfrag_pot_load_t::inc(int epoch)
{
  _update_epoch(epoch);
  value += 1.0;
}

void dirfrag_pot_load_t::adjust(double adj, int epoch)
{
  _update_epoch(epoch);
  value += adj;
}

void dirfrag_pot_load_t::add(dirfrag_pot_load_t & anotherpot)
{
  if (last_epoch > anotherpot.last_epoch)	return;

  adjust(anotherpot.value, anotherpot.last_epoch);
}

void dirfrag_pot_load_t::clear(int epoch)
{
  _update_epoch(epoch);
  value = 0;
  last_value = 0;
}

double dirfrag_pot_load_t::pot_load(int epoch, bool use_current)
{
  if (epoch > 0)
    _update_epoch(epoch);
  return use_current ? value : last_value;
}

void dirfrag_pot_load_t::encode(bufferlist& bl) const
{
  ::encode(value, bl);
  ::encode(last_value, bl);
  ::encode(last_epoch, bl);
}

void dirfrag_pot_load_t::decode(bufferlist::iterator & bl)
{
  ::decode(value, bl);
  ::decode(last_value, bl);
  ::decode(last_epoch, bl);
}

// --- DEPRECATED ---
/*
double lunule_mds_load_t::mds_load()
{
  return alpha * pop_load.mds_load() + beta * lul_load.mds_load();
}

void lunule_mds_load_t::encode(bufferlist& bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(pop_load, bl);
  ::encode(lul_load, bl);
  ENCODE_FINISH(bl);
}

void lunule_mds_load_t::decode(bufferlist::iterator & bl)
{
  DECODE_START(2, 2, bl);
  ::decode(pop_load, bl);
  ::decode(lul_load, bl);
  DECODE_FINISH(bl);
}
*/
