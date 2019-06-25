// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_MOSDPGRECOVERYDELETE_H
#define CEPH_MOSDPGRECOVERYDELETE_H

#include "MOSDFastDispatchOp.h"
#include "include/ceph_features.h"

/*
 * instruct non-primary to remove some objects during recovery
 */

struct MOSDPGRecoveryDelete : public MOSDFastDispatchOp {

  static const int HEAD_VERSION = 2;
  static const int COMPAT_VERSION = 1;

  pg_shard_t from;
  spg_t pgid;            ///< target spg_t
  epoch_t map_epoch, min_epoch;
  list<pair<hobject_t, eversion_t> > objects;    ///< objects to remove

private:
  uint64_t cost;

public:
  int get_cost() const override {
    return cost;
  }

  epoch_t get_map_epoch() const override {
    return map_epoch;
  }
  epoch_t get_min_epoch() const override {
    return min_epoch;
  }
  spg_t get_spg() const override {
    return pgid;
  }

  void set_cost(uint64_t c) {
    cost = c;
  }

  MOSDPGRecoveryDelete()
    : MOSDFastDispatchOp(MSG_OSD_PG_RECOVERY_DELETE, HEAD_VERSION,
			 COMPAT_VERSION), cost(0) {}

  MOSDPGRecoveryDelete(pg_shard_t from, spg_t pgid, epoch_t map_epoch,
		       epoch_t min_epoch)
    : MOSDFastDispatchOp(MSG_OSD_PG_RECOVERY_DELETE, HEAD_VERSION,
			 COMPAT_VERSION),
      from(from),
      pgid(pgid),
      map_epoch(map_epoch),
      min_epoch(min_epoch),
      cost(0) {}

private:
  ~MOSDPGRecoveryDelete() {}

public:
  const char *get_type_name() const { return "recovery_delete"; }
  void print(ostream& out) const {
    out << "MOSDPGRecoveryDelete(" << pgid << " e" << map_epoch << ","
	<< min_epoch << " " << objects << ")";
  }

  void encode_payload(uint64_t features) {
    ::encode(from, payload);
    ::encode(pgid, payload);
    ::encode(map_epoch, payload);
    if (HAVE_FEATURE(features, SERVER_LUMINOUS)) {
      ::encode(min_epoch, payload);
    }
    ::encode(cost, payload);
    ::encode(objects, payload);
  }
  void decode_payload() {
    bufferlist::iterator p = payload.begin();
    ::decode(from, p);
    ::decode(pgid, p);
    ::decode(map_epoch, p);
    if (header.version == 1 &&
	!HAVE_FEATURE(get_connection()->get_features(), SERVER_LUMINOUS)) {
      min_epoch = map_epoch;
    } else {
      ::decode(min_epoch, p);
    }
    ::decode(cost, p);
    ::decode(objects, p);
  }
};



#endif
