// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_MOSDOP_H
#define CEPH_MOSDOP_H

#include "MOSDFastDispatchOp.h"
#include "include/ceph_features.h"
#include "common/hobject.h"
#include <atomic>

/*
 * OSD op
 *
 * oid - object id
 * op  - OSD_OP_DELETE, etc.
 *
 */

class OSD;

class MOSDOp : public MOSDFastDispatchOp {

  static const int HEAD_VERSION = 8;
  static const int COMPAT_VERSION = 3;

private:
  uint32_t client_inc = 0;
  __u32 osdmap_epoch = 0;
  __u32 flags = 0;
  utime_t mtime;
  int32_t retry_attempt = -1;   // 0 is first attempt.  -1 if we don't know.

  hobject_t hobj;
  spg_t pgid;
  bufferlist::iterator p;
  // Decoding flags. Decoding is only needed for messages catched by pipe reader.
  // Transition from true -> false without locks being held
  // Can never see final_decode_needed == false and partial_decode_needed == true
  atomic<bool> partial_decode_needed;
  atomic<bool> final_decode_needed;
  //
public:
  vector<OSDOp> ops;
private:
  snapid_t snap_seq;
  vector<snapid_t> snaps;

  uint64_t features;

  osd_reqid_t reqid; // reqid explicitly set by sender

public:
  friend class MOSDOpReply;

  ceph_tid_t get_client_tid() { return header.tid; }
  void set_snapid(const snapid_t& s) {
    hobj.snap = s;
  }
  void set_snaps(const vector<snapid_t>& i) {
    snaps = i;
  }
  void set_snap_seq(const snapid_t& s) { snap_seq = s; }
  void set_reqid(const osd_reqid_t rid) {
    reqid = rid;
  }
  void set_spg(spg_t p) {
    pgid = p;
  }

  // Fields decoded in partial decoding
  pg_t get_pg() const {
    assert(!partial_decode_needed);
    return pgid.pgid;
  }
  spg_t get_spg() const override {
    assert(!partial_decode_needed);
    return pgid;
  }
  pg_t get_raw_pg() const {
    assert(!partial_decode_needed);
    return pg_t(hobj.get_hash(), pgid.pgid.pool());
  }
  epoch_t get_map_epoch() const override {
    assert(!partial_decode_needed);
    return osdmap_epoch;
  }
  int get_flags() const {
    assert(!partial_decode_needed);
    return flags;
  }
  osd_reqid_t get_reqid() const {
    assert(!partial_decode_needed);
    if (reqid.name != entity_name_t() || reqid.tid != 0) {
      return reqid;
    } else {
      if (!final_decode_needed)
	assert(reqid.inc == (int32_t)client_inc);  // decode() should have done this
      return osd_reqid_t(get_orig_source(),
                         reqid.inc,
			 header.tid);
    }
  }

  // Fields decoded in final decoding
  int get_client_inc() const {
    assert(!final_decode_needed);
    return client_inc;
  }
  utime_t get_mtime() const {
    assert(!final_decode_needed);
    return mtime;
  }
  object_locator_t get_object_locator() const {
    assert(!final_decode_needed);
    if (hobj.oid.name.empty())
      return object_locator_t(hobj.pool, hobj.nspace, hobj.get_hash());
    else
      return object_locator_t(hobj);
  }
  const object_t& get_oid() const {
    assert(!final_decode_needed);
    return hobj.oid;
  }
  const hobject_t &get_hobj() const {
    return hobj;
  }
  snapid_t get_snapid() const {
    assert(!final_decode_needed);
    return hobj.snap;
  }
  const snapid_t& get_snap_seq() const {
    assert(!final_decode_needed);
    return snap_seq;
  }
  const vector<snapid_t> &get_snaps() const {
    assert(!final_decode_needed);
    return snaps;
  }

  /**
   * get retry attempt
   *
   * 0 is the first attempt.
   *
   * @return retry attempt, or -1 if we don't know
   */
  int get_retry_attempt() const {
    return retry_attempt;
  }
  uint64_t get_features() const {
    if (features)
      return features;
    return get_connection()->get_features();
  }

  MOSDOp()
    : MOSDFastDispatchOp(CEPH_MSG_OSD_OP, HEAD_VERSION, COMPAT_VERSION),
      partial_decode_needed(true),
      final_decode_needed(true) { }
  MOSDOp(int inc, long tid, const hobject_t& ho, spg_t& _pgid,
	 epoch_t _osdmap_epoch,
	 int _flags, uint64_t feat)
    : MOSDFastDispatchOp(CEPH_MSG_OSD_OP, HEAD_VERSION, COMPAT_VERSION),
      client_inc(inc),
      osdmap_epoch(_osdmap_epoch), flags(_flags), retry_attempt(-1),
      hobj(ho),
      pgid(_pgid),
      partial_decode_needed(false),
      final_decode_needed(false),
      features(feat) {
    set_tid(tid);

    // also put the client_inc in reqid.inc, so that get_reqid() can
    // be used before the full message is decoded.
    reqid.inc = inc;
  }
private:
  ~MOSDOp() override {}

public:
  void set_mtime(utime_t mt) { mtime = mt; }
  void set_mtime(ceph::real_time mt) {
    mtime = ceph::real_clock::to_timespec(mt);
  }

  // ops
  void add_simple_op(int o, uint64_t off, uint64_t len) {
    OSDOp osd_op;
    osd_op.op.op = o;
    osd_op.op.extent.offset = off;
    osd_op.op.extent.length = len;
    ops.push_back(osd_op);
  }
  void write(uint64_t off, uint64_t len, bufferlist& bl) {
    add_simple_op(CEPH_OSD_OP_WRITE, off, len);
    data.claim(bl);
    header.data_off = off;
  }
  void writefull(bufferlist& bl) {
    add_simple_op(CEPH_OSD_OP_WRITEFULL, 0, bl.length());
    data.claim(bl);
    header.data_off = 0;
  }
  void zero(uint64_t off, uint64_t len) {
    add_simple_op(CEPH_OSD_OP_ZERO, off, len);
  }
  void truncate(uint64_t off) {
    add_simple_op(CEPH_OSD_OP_TRUNCATE, off, 0);
  }
  void remove() {
    add_simple_op(CEPH_OSD_OP_DELETE, 0, 0);
  }

  void read(uint64_t off, uint64_t len) {
    add_simple_op(CEPH_OSD_OP_READ, off, len);
  }
  void stat() {
    add_simple_op(CEPH_OSD_OP_STAT, 0, 0);
  }

  bool has_flag(__u32 flag) const { return flags & flag; };

  bool is_retry_attempt() const { return flags & CEPH_OSD_FLAG_RETRY; }
  void set_retry_attempt(unsigned a) { 
    if (a)
      flags |= CEPH_OSD_FLAG_RETRY;
    else
      flags &= ~CEPH_OSD_FLAG_RETRY;
    retry_attempt = a;
  }

  // marshalling
  void encode_payload(uint64_t features) override {

    OSDOp::merge_osd_op_vector_in_data(ops, data);

    if ((features & CEPH_FEATURE_OBJECTLOCATOR) == 0) {
      // here is the old structure we are encoding to: //
#if 0
struct ceph_osd_request_head {
	__le32 client_inc;                 /* client incarnation */
	struct ceph_object_layout layout;  /* pgid */
	__le32 osdmap_epoch;               /* client's osdmap epoch */

	__le32 flags;

	struct ceph_timespec mtime;        /* for mutations only */
	struct ceph_eversion reassert_version; /* if we are replaying op */

	__le32 object_len;     /* length of object name */

	__le64 snapid;         /* snapid to read */
	__le64 snap_seq;       /* writer's snap context */
	__le32 num_snaps;

	__le16 num_ops;
	struct ceph_osd_op ops[];  /* followed by ops[], obj, ticket, snaps */
} __attribute__ ((packed));
#endif
      header.version = 1;

      ::encode(client_inc, payload);

      __u32 su = 0;
      ::encode(get_raw_pg(), payload);
      ::encode(su, payload);

      ::encode(osdmap_epoch, payload);
      ::encode(flags, payload);
      ::encode(mtime, payload);
      ::encode(eversion_t(), payload);  // reassert_version

      __u32 oid_len = hobj.oid.name.length();
      ::encode(oid_len, payload);
      ::encode(hobj.snap, payload);
      ::encode(snap_seq, payload);
      __u32 num_snaps = snaps.size();
      ::encode(num_snaps, payload);
      
      //::encode(ops, payload);
      __u16 num_ops = ops.size();
      ::encode(num_ops, payload);
      for (unsigned i = 0; i < ops.size(); i++)
	::encode(ops[i].op, payload);

      ::encode_nohead(hobj.oid.name, payload);
      ::encode_nohead(snaps, payload);
    } else if ((features & CEPH_FEATURE_NEW_OSDOP_ENCODING) == 0) {
      header.version = 6;
      ::encode(client_inc, payload);
      ::encode(osdmap_epoch, payload);
      ::encode(flags, payload);
      ::encode(mtime, payload);
      ::encode(eversion_t(), payload); // reassert_version
      ::encode(get_object_locator(), payload);
      ::encode(get_raw_pg(), payload);

      ::encode(hobj.oid, payload);

      __u16 num_ops = ops.size();
      ::encode(num_ops, payload);
      for (unsigned i = 0; i < ops.size(); i++)
        ::encode(ops[i].op, payload);

      ::encode(hobj.snap, payload);
      ::encode(snap_seq, payload);
      ::encode(snaps, payload);

      ::encode(retry_attempt, payload);
      ::encode(features, payload);
      if (reqid.name != entity_name_t() || reqid.tid != 0) {
	::encode(reqid, payload);
      } else {
	// don't include client_inc in the reqid for the legacy v6
	// encoding or else we'll confuse older peers.
	::encode(osd_reqid_t(), payload);
      }
    } else if (!HAVE_FEATURE(features, RESEND_ON_SPLIT)) {
      // reordered, v7 message encoding
      header.version = 7;
      ::encode(get_raw_pg(), payload);
      ::encode(osdmap_epoch, payload);
      ::encode(flags, payload);
      ::encode(eversion_t(), payload); // reassert_version
      ::encode(reqid, payload);
      ::encode(client_inc, payload);
      ::encode(mtime, payload);
      ::encode(get_object_locator(), payload);
      ::encode(hobj.oid, payload);

      __u16 num_ops = ops.size();
      ::encode(num_ops, payload);
      for (unsigned i = 0; i < ops.size(); i++)
	::encode(ops[i].op, payload);

      ::encode(hobj.snap, payload);
      ::encode(snap_seq, payload);
      ::encode(snaps, payload);

      ::encode(retry_attempt, payload);
      ::encode(features, payload);
    } else {
      // latest v8 encoding with hobject_t hash separate from pgid, no
      // reassert version
      header.version = HEAD_VERSION;
      ::encode(pgid, payload);
      ::encode(hobj.get_hash(), payload);
      ::encode(osdmap_epoch, payload);
      ::encode(flags, payload);
      ::encode(reqid, payload);
      encode_trace(payload, features);

      // -- above decoded up front; below decoded post-dispatch thread --

      ::encode(client_inc, payload);
      ::encode(mtime, payload);
      ::encode(get_object_locator(), payload);
      ::encode(hobj.oid, payload);

      __u16 num_ops = ops.size();
      ::encode(num_ops, payload);
      for (unsigned i = 0; i < ops.size(); i++)
	::encode(ops[i].op, payload);

      ::encode(hobj.snap, payload);
      ::encode(snap_seq, payload);
      ::encode(snaps, payload);

      ::encode(retry_attempt, payload);
      ::encode(features, payload);
    }
  }

  void decode_payload() override {
    assert(partial_decode_needed && final_decode_needed);
    p = payload.begin();

    // Always keep here the newest version of decoding order/rule
    if (header.version == HEAD_VERSION) {
      ::decode(pgid, p);      // actual pgid
      uint32_t hash;
      ::decode(hash, p); // raw hash value
      hobj.set_hash(hash);
      ::decode(osdmap_epoch, p);
      ::decode(flags, p);
      ::decode(reqid, p);
      decode_trace(p);
    } else if (header.version == 7) {
      ::decode(pgid.pgid, p);      // raw pgid
      hobj.set_hash(pgid.pgid.ps());
      ::decode(osdmap_epoch, p);
      ::decode(flags, p);
      eversion_t reassert_version;
      ::decode(reassert_version, p);
      ::decode(reqid, p);
    } else if (header.version < 2) {
      // old decode
      ::decode(client_inc, p);

      old_pg_t opgid;
      ::decode_raw(opgid, p);
      pgid.pgid = opgid;

      __u32 su;
      ::decode(su, p);

      ::decode(osdmap_epoch, p);
      ::decode(flags, p);
      ::decode(mtime, p);
      eversion_t reassert_version;
      ::decode(reassert_version, p);

      __u32 oid_len;
      ::decode(oid_len, p);
      ::decode(hobj.snap, p);
      ::decode(snap_seq, p);
      __u32 num_snaps;
      ::decode(num_snaps, p);
      
      //::decode(ops, p);
      __u16 num_ops;
      ::decode(num_ops, p);
      ops.resize(num_ops);
      for (unsigned i = 0; i < num_ops; i++)
	::decode(ops[i].op, p);

      decode_nohead(oid_len, hobj.oid.name, p);
      decode_nohead(num_snaps, snaps, p);

      // recalculate pgid hash value
      pgid.pgid.set_ps(ceph_str_hash(CEPH_STR_HASH_RJENKINS,
				     hobj.oid.name.c_str(),
				     hobj.oid.name.length()));
      hobj.pool = pgid.pgid.pool();
      hobj.set_hash(pgid.pgid.ps());

      retry_attempt = -1;
      features = 0;
      OSDOp::split_osd_op_vector_in_data(ops, data);

      // we did the full decode
      final_decode_needed = false;

      // put client_inc in reqid.inc for get_reqid()'s benefit
      reqid = osd_reqid_t();
      reqid.inc = client_inc;
    } else if (header.version < 7) {
      ::decode(client_inc, p);
      ::decode(osdmap_epoch, p);
      ::decode(flags, p);
      ::decode(mtime, p);
      eversion_t reassert_version;
      ::decode(reassert_version, p);

      object_locator_t oloc;
      ::decode(oloc, p);

      if (header.version < 3) {
	old_pg_t opgid;
	::decode_raw(opgid, p);
	pgid.pgid = opgid;
      } else {
	::decode(pgid.pgid, p);
      }

      ::decode(hobj.oid, p);

      //::decode(ops, p);
      __u16 num_ops;
      ::decode(num_ops, p);
      ops.resize(num_ops);
      for (unsigned i = 0; i < num_ops; i++)
        ::decode(ops[i].op, p);

      ::decode(hobj.snap, p);
      ::decode(snap_seq, p);
      ::decode(snaps, p);

      if (header.version >= 4)
        ::decode(retry_attempt, p);
      else
        retry_attempt = -1;

      if (header.version >= 5)
        ::decode(features, p);
      else
	features = 0;

      if (header.version >= 6)
	::decode(reqid, p);
      else
	reqid = osd_reqid_t();

      hobj.pool = pgid.pgid.pool();
      hobj.set_key(oloc.key);
      hobj.nspace = oloc.nspace;
      hobj.set_hash(pgid.pgid.ps());

      OSDOp::split_osd_op_vector_in_data(ops, data);

      // we did the full decode
      final_decode_needed = false;

      // put client_inc in reqid.inc for get_reqid()'s benefit
      if (reqid.name == entity_name_t() && reqid.tid == 0)
	reqid.inc = client_inc;
    }

    partial_decode_needed = false;
  }

  bool finish_decode() {
    assert(!partial_decode_needed); // partial decoding required
    if (!final_decode_needed)
      return false; // Message is already final decoded
    assert(header.version >= 7);

    ::decode(client_inc, p);
    ::decode(mtime, p);
    object_locator_t oloc;
    ::decode(oloc, p);
    ::decode(hobj.oid, p);

    __u16 num_ops;
    ::decode(num_ops, p);
    ops.resize(num_ops);
    for (unsigned i = 0; i < num_ops; i++)
      ::decode(ops[i].op, p);

    ::decode(hobj.snap, p);
    ::decode(snap_seq, p);
    ::decode(snaps, p);

    ::decode(retry_attempt, p);

    ::decode(features, p);

    hobj.pool = pgid.pgid.pool();
    hobj.set_key(oloc.key);
    hobj.nspace = oloc.nspace;

    OSDOp::split_osd_op_vector_in_data(ops, data);

    final_decode_needed = false;
    return true;
  }

  void clear_buffers() override {
    OSDOp::clear_data(ops);
  }

  const char *get_type_name() const override { return "osd_op"; }
  void print(ostream& out) const override {
    out << "osd_op(";
    if (!partial_decode_needed) {
      out << get_reqid() << ' ';
      out << pgid;
      if (!final_decode_needed) {
	out << ' ';
	out << hobj
	    << " " << ops
	    << " snapc " << get_snap_seq() << "=" << snaps;
	if (is_retry_attempt())
	  out << " RETRY=" << get_retry_attempt();
      } else {
	out << " " << get_raw_pg() << " (undecoded)";
      }
      out << " " << ceph_osd_flag_string(get_flags());
      out << " e" << osdmap_epoch;
    }
    out << ")";
  }
};


#endif
