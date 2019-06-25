// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include "mdstypes.h"
#include "MDSContext.h"
#include "common/Formatter.h"

const mds_gid_t MDS_GID_NONE = mds_gid_t(0);
const mds_rank_t MDS_RANK_NONE = mds_rank_t(-1);


/*
 * frag_info_t
 */

void frag_info_t::encode(bufferlist &bl) const
{
  ENCODE_START(3, 2, bl);
  ::encode(version, bl);
  ::encode(mtime, bl);
  ::encode(nfiles, bl);
  ::encode(nsubdirs, bl);
  ::encode(change_attr, bl);
  ENCODE_FINISH(bl);
}

void frag_info_t::decode(bufferlist::iterator &bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(3, 2, 2, bl);
  ::decode(version, bl);
  ::decode(mtime, bl);
  ::decode(nfiles, bl);
  ::decode(nsubdirs, bl);
  if (struct_v >= 3)
    ::decode(change_attr, bl);
  else
    change_attr = 0;
  DECODE_FINISH(bl);
}

void frag_info_t::dump(Formatter *f) const
{
  f->dump_unsigned("version", version);
  f->dump_stream("mtime") << mtime;
  f->dump_unsigned("num_files", nfiles);
  f->dump_unsigned("num_subdirs", nsubdirs);
}

void frag_info_t::generate_test_instances(list<frag_info_t*>& ls)
{
  ls.push_back(new frag_info_t);
  ls.push_back(new frag_info_t);
  ls.back()->version = 1;
  ls.back()->mtime = utime_t(2, 3);
  ls.back()->nfiles = 4;
  ls.back()->nsubdirs = 5;
}

ostream& operator<<(ostream &out, const frag_info_t &f)
{
  if (f == frag_info_t())
    return out << "f()";
  out << "f(v" << f.version;
  if (f.mtime != utime_t())
    out << " m" << f.mtime;
  if (f.nfiles || f.nsubdirs)
    out << " " << f.size() << "=" << f.nfiles << "+" << f.nsubdirs;
  out << ")";
  return out;
}


/*
 * nest_info_t
 */

void nest_info_t::encode(bufferlist &bl) const
{
  ENCODE_START(3, 2, bl);
  ::encode(version, bl);
  ::encode(rbytes, bl);
  ::encode(rfiles, bl);
  ::encode(rsubdirs, bl);
  {
    // removed field
    int64_t ranchors = 0;
    ::encode(ranchors, bl);
  }
  ::encode(rsnaprealms, bl);
  ::encode(rctime, bl);
  ENCODE_FINISH(bl);
}

void nest_info_t::decode(bufferlist::iterator &bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(3, 2, 2, bl);
  ::decode(version, bl);
  ::decode(rbytes, bl);
  ::decode(rfiles, bl);
  ::decode(rsubdirs, bl);
  {
    int64_t ranchors;
    ::decode(ranchors, bl);
  }
  ::decode(rsnaprealms, bl);
  ::decode(rctime, bl);
  DECODE_FINISH(bl);
}

void nest_info_t::dump(Formatter *f) const
{
  f->dump_unsigned("version", version);
  f->dump_unsigned("rbytes", rbytes);
  f->dump_unsigned("rfiles", rfiles);
  f->dump_unsigned("rsubdirs", rsubdirs);
  f->dump_unsigned("rsnaprealms", rsnaprealms);
  f->dump_stream("rctime") << rctime;
}

void nest_info_t::generate_test_instances(list<nest_info_t*>& ls)
{
  ls.push_back(new nest_info_t);
  ls.push_back(new nest_info_t);
  ls.back()->version = 1;
  ls.back()->rbytes = 2;
  ls.back()->rfiles = 3;
  ls.back()->rsubdirs = 4;
  ls.back()->rsnaprealms = 6;
  ls.back()->rctime = utime_t(7, 8);
}

ostream& operator<<(ostream &out, const nest_info_t &n)
{
  if (n == nest_info_t())
    return out << "n()";
  out << "n(v" << n.version;
  if (n.rctime != utime_t())
    out << " rc" << n.rctime;
  if (n.rbytes)
    out << " b" << n.rbytes;
  if (n.rsnaprealms)
    out << " sr" << n.rsnaprealms;
  if (n.rfiles || n.rsubdirs)
    out << " " << n.rsize() << "=" << n.rfiles << "+" << n.rsubdirs;
  out << ")";    
  return out;
}

/*
 * quota_info_t
 */
void quota_info_t::dump(Formatter *f) const
{
  f->dump_int("max_bytes", max_bytes);
  f->dump_int("max_files", max_files);
}

void quota_info_t::generate_test_instances(list<quota_info_t *>& ls)
{
  ls.push_back(new quota_info_t);
  ls.push_back(new quota_info_t);
  ls.back()->max_bytes = 16;
  ls.back()->max_files = 16;
}

ostream& operator<<(ostream &out, const quota_info_t &n)
{
  out << "quota("
      << "max_bytes = " << n.max_bytes
      << " max_files = " << n.max_files
      << ")";
  return out;
}

/*
 * client_writeable_range_t
 */

void client_writeable_range_t::encode(bufferlist &bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(range.first, bl);
  ::encode(range.last, bl);
  ::encode(follows, bl);
  ENCODE_FINISH(bl);
}

void client_writeable_range_t::decode(bufferlist::iterator& bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
  ::decode(range.first, bl);
  ::decode(range.last, bl);
  ::decode(follows, bl);
  DECODE_FINISH(bl);
}

void client_writeable_range_t::dump(Formatter *f) const
{
  f->open_object_section("byte range");
  f->dump_unsigned("first", range.first);
  f->dump_unsigned("last", range.last);
  f->close_section();
  f->dump_unsigned("follows", follows);
}

void client_writeable_range_t::generate_test_instances(list<client_writeable_range_t*>& ls)
{
  ls.push_back(new client_writeable_range_t);
  ls.push_back(new client_writeable_range_t);
  ls.back()->range.first = 123;
  ls.back()->range.last = 456;
  ls.back()->follows = 12;
}

ostream& operator<<(ostream& out, const client_writeable_range_t& r)
{
  return out << r.range.first << '-' << r.range.last << "@" << r.follows;
}

/*
 * inline_data_t
 */
void inline_data_t::encode(bufferlist &bl) const
{
  ::encode(version, bl);
  if (blp)
    ::encode(*blp, bl);
  else
    ::encode(bufferlist(), bl);
}
void inline_data_t::decode(bufferlist::iterator &p)
{
  ::decode(version, p);
  uint32_t inline_len;
  ::decode(inline_len, p);
  if (inline_len > 0)
    ::decode_nohead(inline_len, get_data(), p);
  else
    free_data();
}


/*
 * fnode_t
 */
void fnode_t::encode(bufferlist &bl) const
{
  ENCODE_START(4, 3, bl);
  ::encode(version, bl);
  ::encode(snap_purged_thru, bl);
  ::encode(fragstat, bl);
  ::encode(accounted_fragstat, bl);
  ::encode(rstat, bl);
  ::encode(accounted_rstat, bl);
  ::encode(damage_flags, bl);
  ::encode(recursive_scrub_version, bl);
  ::encode(recursive_scrub_stamp, bl);
  ::encode(localized_scrub_version, bl);
  ::encode(localized_scrub_stamp, bl);
  ENCODE_FINISH(bl);
}

void fnode_t::decode(bufferlist::iterator &bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(3, 2, 2, bl);
  ::decode(version, bl);
  ::decode(snap_purged_thru, bl);
  ::decode(fragstat, bl);
  ::decode(accounted_fragstat, bl);
  ::decode(rstat, bl);
  ::decode(accounted_rstat, bl);
  if (struct_v >= 3) {
    ::decode(damage_flags, bl);
  }
  if (struct_v >= 4) {
    ::decode(recursive_scrub_version, bl);
    ::decode(recursive_scrub_stamp, bl);
    ::decode(localized_scrub_version, bl);
    ::decode(localized_scrub_stamp, bl);
  }
  DECODE_FINISH(bl);
}

void fnode_t::dump(Formatter *f) const
{
  f->dump_unsigned("version", version);
  f->dump_unsigned("snap_purged_thru", snap_purged_thru);

  f->open_object_section("fragstat");
  fragstat.dump(f);
  f->close_section();

  f->open_object_section("accounted_fragstat");
  accounted_fragstat.dump(f);
  f->close_section();

  f->open_object_section("rstat");
  rstat.dump(f);
  f->close_section();

  f->open_object_section("accounted_rstat");
  accounted_rstat.dump(f);
  f->close_section();
}

void fnode_t::generate_test_instances(list<fnode_t*>& ls)
{
  ls.push_back(new fnode_t);
  ls.push_back(new fnode_t);
  ls.back()->version = 1;
  ls.back()->snap_purged_thru = 2;
  list<frag_info_t*> fls;
  frag_info_t::generate_test_instances(fls);
  ls.back()->fragstat = *fls.back();
  ls.back()->accounted_fragstat = *fls.front();
  list<nest_info_t*> nls;
  nest_info_t::generate_test_instances(nls);
  ls.back()->rstat = *nls.front();
  ls.back()->accounted_rstat = *nls.back();
}


/*
 * old_rstat_t
 */
void old_rstat_t::encode(bufferlist& bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(first, bl);
  ::encode(rstat, bl);
  ::encode(accounted_rstat, bl);
  ENCODE_FINISH(bl);
}

void old_rstat_t::decode(bufferlist::iterator& bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
  ::decode(first, bl);
  ::decode(rstat, bl);
  ::decode(accounted_rstat, bl);
  DECODE_FINISH(bl);
}

void old_rstat_t::dump(Formatter *f) const
{
  f->dump_unsigned("snapid", first);
  f->open_object_section("rstat");
  rstat.dump(f);
  f->close_section();
  f->open_object_section("accounted_rstat");
  accounted_rstat.dump(f);
  f->close_section();
}

void old_rstat_t::generate_test_instances(list<old_rstat_t*>& ls)
{
  ls.push_back(new old_rstat_t());
  ls.push_back(new old_rstat_t());
  ls.back()->first = 12;
  list<nest_info_t*> nls;
  nest_info_t::generate_test_instances(nls);
  ls.back()->rstat = *nls.back();
  ls.back()->accounted_rstat = *nls.front();
}

/*
 * session_info_t
 */
void session_info_t::encode(bufferlist& bl, uint64_t features) const
{
  ENCODE_START(6, 3, bl);
  ::encode(inst, bl, features);
  ::encode(completed_requests, bl);
  ::encode(prealloc_inos, bl);   // hacky, see below.
  ::encode(used_inos, bl);
  ::encode(client_metadata, bl);
  ::encode(completed_flushes, bl);
  ::encode(auth_name, bl);
  ENCODE_FINISH(bl);
}

void session_info_t::decode(bufferlist::iterator& p)
{
  DECODE_START_LEGACY_COMPAT_LEN(6, 2, 2, p);
  ::decode(inst, p);
  if (struct_v <= 2) {
    set<ceph_tid_t> s;
    ::decode(s, p);
    while (!s.empty()) {
      completed_requests[*s.begin()] = inodeno_t();
      s.erase(s.begin());
    }
  } else {
    ::decode(completed_requests, p);
  }
  ::decode(prealloc_inos, p);
  ::decode(used_inos, p);
  prealloc_inos.insert(used_inos);
  used_inos.clear();
  if (struct_v >= 4) {
    ::decode(client_metadata, p);
  }
  if (struct_v >= 5) {
    ::decode(completed_flushes, p);
  }
  if (struct_v >= 6) {
    ::decode(auth_name, p);
  }
  DECODE_FINISH(p);
}

void session_info_t::dump(Formatter *f) const
{
  f->dump_stream("inst") << inst;

  f->open_array_section("completed_requests");
  for (map<ceph_tid_t,inodeno_t>::const_iterator p = completed_requests.begin();
       p != completed_requests.end();
       ++p) {
    f->open_object_section("request");
    f->dump_unsigned("tid", p->first);
    f->dump_stream("created_ino") << p->second;
    f->close_section();
  }
  f->close_section();

  f->open_array_section("prealloc_inos");
  for (interval_set<inodeno_t>::const_iterator p = prealloc_inos.begin();
       p != prealloc_inos.end();
       ++p) {
    f->open_object_section("ino_range");
    f->dump_unsigned("start", p.get_start());
    f->dump_unsigned("length", p.get_len());
    f->close_section();
  }
  f->close_section();

  f->open_array_section("used_inos");
  for (interval_set<inodeno_t>::const_iterator p = prealloc_inos.begin();
       p != prealloc_inos.end();
       ++p) {
    f->open_object_section("ino_range");
    f->dump_unsigned("start", p.get_start());
    f->dump_unsigned("length", p.get_len());
    f->close_section();
  }
  f->close_section();

  for (map<string, string>::const_iterator i = client_metadata.begin();
      i != client_metadata.end(); ++i) {
    f->dump_string(i->first.c_str(), i->second);
  }
}

void session_info_t::generate_test_instances(list<session_info_t*>& ls)
{
  ls.push_back(new session_info_t);
  ls.push_back(new session_info_t);
  ls.back()->inst = entity_inst_t(entity_name_t::MDS(12), entity_addr_t());
  ls.back()->completed_requests.insert(make_pair(234, inodeno_t(111222)));
  ls.back()->completed_requests.insert(make_pair(237, inodeno_t(222333)));
  ls.back()->prealloc_inos.insert(333, 12);
  ls.back()->prealloc_inos.insert(377, 112);
  // we can't add used inos; they're cleared on decode
}


/*
 * string_snap_t
 */
void string_snap_t::encode(bufferlist& bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(name, bl);
  ::encode(snapid, bl);
  ENCODE_FINISH(bl);
}

void string_snap_t::decode(bufferlist::iterator& bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
  ::decode(name, bl);
  ::decode(snapid, bl);
  DECODE_FINISH(bl);
}

void string_snap_t::dump(Formatter *f) const
{
  f->dump_string("name", name);
  f->dump_unsigned("snapid", snapid);
}

void string_snap_t::generate_test_instances(list<string_snap_t*>& ls)
{
  ls.push_back(new string_snap_t);
  ls.push_back(new string_snap_t);
  ls.back()->name = "foo";
  ls.back()->snapid = 123;
  ls.push_back(new string_snap_t);
  ls.back()->name = "bar";
  ls.back()->snapid = 456;
}


/*
 * MDSCacheObjectInfo
 */
void MDSCacheObjectInfo::encode(bufferlist& bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(ino, bl);
  ::encode(dirfrag, bl);
  ::encode(dname, bl);
  ::encode(snapid, bl);
  ENCODE_FINISH(bl);
}

void MDSCacheObjectInfo::decode(bufferlist::iterator& p)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, p);
  ::decode(ino, p);
  ::decode(dirfrag, p);
  ::decode(dname, p);
  ::decode(snapid, p);
  DECODE_FINISH(p);
}

void MDSCacheObjectInfo::dump(Formatter *f) const
{
  f->dump_unsigned("ino", ino);
  f->dump_stream("dirfrag") << dirfrag;
  f->dump_string("name", dname);
  f->dump_unsigned("snapid", snapid);
}

void MDSCacheObjectInfo::generate_test_instances(list<MDSCacheObjectInfo*>& ls)
{
  ls.push_back(new MDSCacheObjectInfo);
  ls.push_back(new MDSCacheObjectInfo);
  ls.back()->ino = 1;
  ls.back()->dirfrag = dirfrag_t(2, 3);
  ls.back()->dname = "fooname";
  ls.back()->snapid = CEPH_NOSNAP;
  ls.push_back(new MDSCacheObjectInfo);
  ls.back()->ino = 121;
  ls.back()->dirfrag = dirfrag_t(222, 0);
  ls.back()->dname = "bar foo";
  ls.back()->snapid = 21322;
}

/*
 * mds_table_pending_t
 */
void mds_table_pending_t::encode(bufferlist& bl) const
{
  ENCODE_START(2, 2, bl);
  ::encode(reqid, bl);
  ::encode(mds, bl);
  ::encode(tid, bl);
  ENCODE_FINISH(bl);
}

void mds_table_pending_t::decode(bufferlist::iterator& bl)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
  ::decode(reqid, bl);
  ::decode(mds, bl);
  ::decode(tid, bl);
  DECODE_FINISH(bl);
}

void mds_table_pending_t::dump(Formatter *f) const
{
  f->dump_unsigned("reqid", reqid);
  f->dump_unsigned("mds", mds);
  f->dump_unsigned("tid", tid);
}

void mds_table_pending_t::generate_test_instances(list<mds_table_pending_t*>& ls)
{
  ls.push_back(new mds_table_pending_t);
  ls.push_back(new mds_table_pending_t);
  ls.back()->reqid = 234;
  ls.back()->mds = 2;
  ls.back()->tid = 35434;
}


/*
 * inode_load_vec_t
 */
void inode_load_vec_t::encode(bufferlist &bl) const
{
  ENCODE_START(2, 2, bl);
  for (const auto &i : vec) {
    ::encode(i, bl);
  }
  ENCODE_FINISH(bl);
}

void inode_load_vec_t::decode(const utime_t &t, bufferlist::iterator &p)
{
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, p);
  for (auto &i : vec) {
    ::decode(i, t, p);
  }
  DECODE_FINISH(p);
}

void inode_load_vec_t::dump(Formatter *f)
{
  f->open_array_section("Decay Counters");
  for (const auto &i : vec) {
    f->open_object_section("Decay Counter");
    i.dump(f);
    f->close_section();
  }
  f->close_section();
}

void inode_load_vec_t::generate_test_instances(list<inode_load_vec_t*>& ls)
{
  utime_t sample;
  ls.push_back(new inode_load_vec_t(sample));
}


/*
 * dirfrag_load_vec_t
 */
void dirfrag_load_vec_t::dump(Formatter *f) const
{
  f->open_array_section("Decay Counters");
  for (const auto &i : vec) {
    f->open_object_section("Decay Counter");
    i.dump(f);
    f->close_section();
  }
  f->close_section();
}

void dirfrag_load_vec_t::generate_test_instances(list<dirfrag_load_vec_t*>& ls)
{
  utime_t sample;
  ls.push_back(new dirfrag_load_vec_t(sample));
}

/*
 * mds_load_t
 */
void mds_load_t::encode(bufferlist &bl) const {
  ENCODE_START(2, 2, bl);
  ::encode(auth, bl);
  ::encode(all, bl);
  ::encode(req_rate, bl);
  ::encode(cache_hit_rate, bl);
  ::encode(queue_len, bl);
  ::encode(cpu_load_avg, bl);
  ENCODE_FINISH(bl);
}

void mds_load_t::decode(const utime_t &t, bufferlist::iterator &bl) {
  DECODE_START_LEGACY_COMPAT_LEN(2, 2, 2, bl);
  ::decode(auth, t, bl);
  ::decode(all, t, bl);
  ::decode(req_rate, bl);
  ::decode(cache_hit_rate, bl);
  ::decode(queue_len, bl);
  ::decode(cpu_load_avg, bl);
  DECODE_FINISH(bl);
}

void mds_load_t::dump(Formatter *f) const
{
  f->dump_float("request rate", req_rate);
  f->dump_float("cache hit rate", cache_hit_rate);
  f->dump_float("queue length", queue_len);
  f->dump_float("cpu load", cpu_load_avg);
  f->open_object_section("auth dirfrag");
  auth.dump(f);
  f->close_section();
  f->open_object_section("all dirfrags");
  all.dump(f);
  f->close_section();
}

void mds_load_t::generate_test_instances(list<mds_load_t*>& ls)
{
  utime_t sample;
  ls.push_back(new mds_load_t(sample));
}

/*
 * cap_reconnect_t
 */
void cap_reconnect_t::encode(bufferlist& bl) const {
  ENCODE_START(2, 1, bl);
  encode_old(bl); // extract out when something changes
  ::encode(snap_follows, bl);
  ENCODE_FINISH(bl);
}

void cap_reconnect_t::encode_old(bufferlist& bl) const {
  ::encode(path, bl);
  capinfo.flock_len = flockbl.length();
  ::encode(capinfo, bl);
  ::encode_nohead(flockbl, bl);
}

void cap_reconnect_t::decode(bufferlist::iterator& bl) {
  DECODE_START(1, bl);
  decode_old(bl); // extract out when something changes
  if (struct_v >= 2)
    ::decode(snap_follows, bl);
  DECODE_FINISH(bl);
}

void cap_reconnect_t::decode_old(bufferlist::iterator& bl) {
  ::decode(path, bl);
  ::decode(capinfo, bl);
  ::decode_nohead(capinfo.flock_len, flockbl, bl);
}

void cap_reconnect_t::dump(Formatter *f) const
{
  f->dump_string("path", path);
  f->dump_int("cap_id", capinfo.cap_id);
  f->dump_string("cap wanted", ccap_string(capinfo.wanted));
  f->dump_string("cap issued", ccap_string(capinfo.issued));
  f->dump_int("snaprealm", capinfo.snaprealm);
  f->dump_int("path base ino", capinfo.pathbase);
  f->dump_string("has file locks", capinfo.flock_len ? "true" : "false");
}

void cap_reconnect_t::generate_test_instances(list<cap_reconnect_t*>& ls)
{
  ls.push_back(new cap_reconnect_t);
  ls.back()->path = "/test/path";
  ls.back()->capinfo.cap_id = 1;
}

ostream& operator<<(ostream &out, const mds_role_t &role)
{
  out << role.fscid << ":" << role.rank;
  return out;
}

