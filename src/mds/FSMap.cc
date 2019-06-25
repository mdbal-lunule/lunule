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


#include "FSMap.h"

#include <sstream>
using std::stringstream;

#include "mon/health_check.h"


void Filesystem::dump(Formatter *f) const
{
  f->open_object_section("mdsmap");
  mds_map.dump(f);
  f->close_section();
  f->dump_int("id", fscid);
}

void FSMap::dump(Formatter *f) const
{
  f->dump_int("epoch", epoch);

  f->open_object_section("compat");
  compat.dump(f);
  f->close_section();

  f->open_object_section("feature_flags");
  f->dump_bool("enable_multiple", enable_multiple);
  f->dump_bool("ever_enabled_multiple", ever_enabled_multiple);
  f->close_section();

  f->open_array_section("standbys");
  for (const auto &i : standby_daemons) {
    f->open_object_section("info");
    i.second.dump(f);
    f->dump_int("epoch", standby_epochs.at(i.first));
    f->close_section();
  }
  f->close_section();

  f->open_array_section("filesystems");
  for (const auto &fs : filesystems) {
    f->open_object_section("filesystem");
    fs.second->dump(f);
    f->close_section();
  }
  f->close_section();
}

void FSMap::generate_test_instances(list<FSMap*>& ls)
{
  FSMap *m = new FSMap();

  std::list<MDSMap*> mds_map_instances;
  MDSMap::generate_test_instances(mds_map_instances);

  int k = 20;
  for (auto i : mds_map_instances) {
    auto fs = std::make_shared<Filesystem>();
    fs->fscid = k++;
    fs->mds_map = *i;
    delete i;
    m->filesystems[fs->fscid] = fs;
  }
  mds_map_instances.clear();

  ls.push_back(m);
}

void FSMap::print(ostream& out) const
{
  out << "e" << epoch << std::endl;
  out << "enable_multiple, ever_enabled_multiple: " << enable_multiple << ","
      << ever_enabled_multiple << std::endl;
  out << "compat: " << compat << std::endl;
  out << "legacy client fscid: " << legacy_client_fscid << std::endl;
  out << " " << std::endl;

  if (filesystems.empty()) {
    out << "No filesystems configured" << std::endl;
    return;
  }

  for (const auto &fs : filesystems) {
    fs.second->print(out);
    out << " " << std::endl << " " << std::endl;  // Space out a bit
  }

  if (!standby_daemons.empty()) {
    out << "Standby daemons:" << std::endl << " " << std::endl;
  }

  for (const auto &p : standby_daemons) {
    p.second.print_summary(out);
    out << std::endl;
  }
}



void FSMap::print_summary(Formatter *f, ostream *out) const
{
  map<mds_role_t,string> by_rank;
  map<string,int> by_state;

  if (f) {
    f->dump_unsigned("epoch", get_epoch());
    for (auto i : filesystems) {
      auto fs = i.second;
      f->dump_unsigned("id", fs->fscid);
      f->dump_unsigned("up", fs->mds_map.up.size());
      f->dump_unsigned("in", fs->mds_map.in.size());
      f->dump_unsigned("max", fs->mds_map.max_mds);
    }
  } else {
    for (auto i : filesystems) {
      auto fs = i.second;
      *out << fs->mds_map.fs_name << "-" << fs->mds_map.up.size() << "/"
	   << fs->mds_map.in.size() << "/" << fs->mds_map.max_mds << " up ";
    }
  }

  if (f) {
    f->open_array_section("by_rank");
  }

  const auto all_info = get_mds_info();
  for (const auto &p : all_info) {
    const auto &info = p.second;
    string s = ceph_mds_state_name(info.state);
    if (info.laggy()) {
      s += "(laggy or crashed)";
    }

    const fs_cluster_id_t fscid = mds_roles.at(info.global_id);

    if (info.rank != MDS_RANK_NONE &&
        info.state != MDSMap::STATE_STANDBY_REPLAY) {
      if (f) {
        f->open_object_section("mds");
        f->dump_unsigned("filesystem_id", fscid);
        f->dump_unsigned("rank", info.rank);
        f->dump_string("name", info.name);
        f->dump_string("status", s);
        f->close_section();
      } else {
        by_rank[mds_role_t(fscid, info.rank)] = info.name + "=" + s;
      }
    } else {
      by_state[s]++;
    }
  }

  if (f) {
    f->close_section();
  } else {
    if (!by_rank.empty()) {
      if (filesystems.size() > 1) {
        // Disambiguate filesystems
        std::map<std::string, std::string> pretty;
        for (auto i : by_rank) {
          const auto &fs_name = filesystems.at(i.first.fscid)->mds_map.fs_name;
          std::ostringstream o;
          o << "[" << fs_name << ":" << i.first.rank << "]";
          pretty[o.str()] = i.second;
        }
        *out << " " << pretty;
      } else {
        // Omit FSCID in output when only one filesystem exists
        std::map<mds_rank_t, std::string> shortened;
        for (auto i : by_rank) {
          shortened[i.first.rank] = i.second;
        }
        *out << " " << shortened;
      }
    }
  }

  for (map<string,int>::reverse_iterator p = by_state.rbegin(); p != by_state.rend(); ++p) {
    if (f) {
      f->dump_unsigned(p->first.c_str(), p->second);
    } else {
      *out << ", " << p->second << " " << p->first;
    }
  }

  size_t failed = 0;
  size_t damaged = 0;
  for (auto i : filesystems) {
    auto fs = i.second;
    failed += fs->mds_map.failed.size();
    damaged += fs->mds_map.damaged.size();
  }

  if (failed > 0) {
    if (f) {
      f->dump_unsigned("failed", failed);
    } else {
      *out << ", " << failed << " failed";
    }
  }

  if (damaged > 0) {
    if (f) {
      f->dump_unsigned("damaged", damaged);
    } else {
      *out << ", " << damaged << " damaged";
    }
  }
  //if (stopped.size())
  //out << ", " << stopped.size() << " stopped";
}


void FSMap::create_filesystem(boost::string_view name,
                              int64_t metadata_pool, int64_t data_pool,
                              uint64_t features)
{
  auto fs = std::make_shared<Filesystem>();
  fs->mds_map.fs_name = std::string(name);
  fs->mds_map.max_mds = 1;
  fs->mds_map.data_pools.push_back(data_pool);
  fs->mds_map.metadata_pool = metadata_pool;
  fs->mds_map.cas_pool = -1;
  fs->mds_map.max_file_size = g_conf->mds_max_file_size;
  fs->mds_map.compat = compat;
  fs->mds_map.created = ceph_clock_now();
  fs->mds_map.modified = ceph_clock_now();
  fs->mds_map.session_timeout = g_conf->mds_session_timeout;
  fs->mds_map.session_autoclose = g_conf->mds_session_autoclose;
  fs->mds_map.enabled = true;
  if (features & CEPH_FEATURE_SERVER_JEWEL) {
    fs->fscid = next_filesystem_id++;
    // ANONYMOUS is only for upgrades from legacy mdsmaps, we should
    // have initialized next_filesystem_id such that it's never used here.
    assert(fs->fscid != FS_CLUSTER_ID_ANONYMOUS);
  } else {
    // Use anon fscid because this will get thrown away when encoding
    // as legacy MDSMap for legacy mons.
    assert(filesystems.empty());
    fs->fscid = FS_CLUSTER_ID_ANONYMOUS;
  }
  filesystems[fs->fscid] = fs;

  // Created first filesystem?  Set it as the one
  // for legacy clients to use
  if (filesystems.size() == 1) {
    legacy_client_fscid = fs->fscid;
  }
}

void FSMap::reset_filesystem(fs_cluster_id_t fscid)
{
  auto fs = get_filesystem(fscid);
  auto new_fs = std::make_shared<Filesystem>();

  // Populate rank 0 as existing (so don't go into CREATING)
  // but failed (so that next available MDS is assigned the rank)
  new_fs->mds_map.in.insert(mds_rank_t(0));
  new_fs->mds_map.failed.insert(mds_rank_t(0));

  // Carry forward what makes sense
  new_fs->fscid = fs->fscid;
  new_fs->mds_map.inline_data_enabled = fs->mds_map.inline_data_enabled;
  new_fs->mds_map.max_mds = 1;
  new_fs->mds_map.data_pools = fs->mds_map.data_pools;
  new_fs->mds_map.metadata_pool = fs->mds_map.metadata_pool;
  new_fs->mds_map.cas_pool = fs->mds_map.cas_pool;
  new_fs->mds_map.fs_name = fs->mds_map.fs_name;
  new_fs->mds_map.max_file_size = g_conf->mds_max_file_size;
  new_fs->mds_map.compat = compat;
  new_fs->mds_map.created = ceph_clock_now();
  new_fs->mds_map.modified = ceph_clock_now();
  new_fs->mds_map.session_timeout = g_conf->mds_session_timeout;
  new_fs->mds_map.session_autoclose = g_conf->mds_session_autoclose;
  new_fs->mds_map.standby_count_wanted = fs->mds_map.standby_count_wanted;
  new_fs->mds_map.enabled = true;

  // Remember mds ranks that have ever started. (They should load old inotable
  // instead of creating new one if they start again.)
  new_fs->mds_map.stopped.insert(fs->mds_map.in.begin(), fs->mds_map.in.end());
  new_fs->mds_map.stopped.insert(fs->mds_map.stopped.begin(), fs->mds_map.stopped.end());
  new_fs->mds_map.stopped.erase(mds_rank_t(0));

  // Persist the new FSMap
  filesystems[new_fs->fscid] = new_fs;
}

void FSMap::get_health(list<pair<health_status_t,string> >& summary,
			list<pair<health_status_t,string> > *detail) const
{
  mds_rank_t standby_count_wanted = 0;
  for (const auto &i : filesystems) {
    const auto &fs = i.second;

    // TODO: move get_health up into here so that we can qualify
    // all the messages with what filesystem they're talking about
    fs->mds_map.get_health(summary, detail);

    standby_count_wanted = std::max(standby_count_wanted, fs->mds_map.get_standby_count_wanted((mds_rank_t)standby_daemons.size()));
  }

  if (standby_count_wanted) {
    std::ostringstream oss;
    oss << "insufficient standby daemons available: have " << standby_daemons.size() << "; want " << standby_count_wanted << " more";
    summary.push_back(make_pair(HEALTH_WARN, oss.str()));
  }
}

bool FSMap::check_health(void)
{
  bool changed = false;
  for (auto &i : filesystems) {
    changed |= i.second->mds_map.check_health((mds_rank_t)standby_daemons.size());
  }
  return changed;
}

void FSMap::get_health_checks(health_check_map_t *checks) const
{
  mds_rank_t standby_count_wanted = 0;
  for (const auto &i : filesystems) {
    const auto &fs = i.second;
    health_check_map_t fschecks;

    fs->mds_map.get_health_checks(&fschecks);

    // Some of the failed ranks might be transient (i.e. there are standbys
    // ready to replace them).  We will report only on "stuck" failed, i.e.
    // ranks which are failed and have no standby replacement available.
    std::set<mds_rank_t> stuck_failed;

    for (const auto &rank : fs->mds_map.failed) {
      const mds_gid_t replacement = find_replacement_for(
          {fs->fscid, rank}, {}, g_conf->mon_force_standby_active);
      if (replacement == MDS_GID_NONE) {
        stuck_failed.insert(rank);
      }
    }

    // FS_WITH_FAILED_MDS
    if (!stuck_failed.empty()) {
      health_check_t& fscheck = checks->get_or_add(
        "FS_WITH_FAILED_MDS", HEALTH_WARN,
        "%num% filesystem%plurals% %hasorhave% a failed mds daemon");
      ostringstream ss;
      ss << "fs " << fs->mds_map.fs_name << " has " << stuck_failed.size()
         << " failed mds" << (stuck_failed.size() > 1 ? "s" : "");
      fscheck.detail.push_back(ss.str()); }

    checks->merge(fschecks);
    standby_count_wanted = std::max(
      standby_count_wanted,
      fs->mds_map.get_standby_count_wanted((mds_rank_t)standby_daemons.size()));
  }

  // MDS_INSUFFICIENT_STANDBY
  if (standby_count_wanted) {
    std::ostringstream oss, dss;
    oss << "insufficient standby MDS daemons available";
    auto& d = checks->get_or_add("MDS_INSUFFICIENT_STANDBY", HEALTH_WARN, oss.str());
    dss << "have " << standby_daemons.size() << "; want " << standby_count_wanted
	<< " more";
    d.detail.push_back(dss.str());
  }
}

void FSMap::encode(bufferlist& bl, uint64_t features) const
{
  if (features & CEPH_FEATURE_SERVER_JEWEL) {
    ENCODE_START(7, 6, bl);
    ::encode(epoch, bl);
    ::encode(next_filesystem_id, bl);
    ::encode(legacy_client_fscid, bl);
    ::encode(compat, bl);
    ::encode(enable_multiple, bl);
    std::vector<Filesystem> fs_list;
    for (auto i : filesystems) {
      fs_list.push_back(*(i.second));
    }
    ::encode(fs_list, bl, features);
    ::encode(mds_roles, bl);
    ::encode(standby_daemons, bl, features);
    ::encode(standby_epochs, bl);
    ::encode(ever_enabled_multiple, bl);
    ENCODE_FINISH(bl);
  } else {
    if (filesystems.empty()) {
      MDSMap disabled_map;
      disabled_map.epoch = epoch;
      disabled_map.encode(bl, features);
    } else {
      // MDSMonitor should never have created multiple filesystems
      // until the quorum features indicated Jewel
      assert(filesystems.size() == 1);
      auto fs = filesystems.begin()->second;

      // Take the MDSMap for the enabled filesystem, and populated its
      // mds_info with the standbys to get a pre-jewel-style mon MDSMap.
      MDSMap full_mdsmap = fs->mds_map;
      full_mdsmap.epoch = epoch;
      for (const auto &p : standby_daemons) {
        full_mdsmap.mds_info[p.first] = p.second;
      }

      // Old MDSMaps don't set rank on standby replay daemons
      for (auto &i : full_mdsmap.mds_info) {
        auto &info = i.second;
        if (info.state == MDSMap::STATE_STANDBY_REPLAY) {
          info.rank = MDS_RANK_NONE;
        }
      }

      full_mdsmap.encode(bl, features);
    }
  }
}

void FSMap::decode(bufferlist::iterator& p)
{
  // The highest MDSMap encoding version before we changed the
  // MDSMonitor to store an FSMap instead of an MDSMap was
  // 5, so anything older than 6 is decoded as an MDSMap,
  // and anything newer is decoded as an FSMap.
  DECODE_START_LEGACY_COMPAT_LEN_16(7, 4, 4, p);
  if (struct_v < 6) {
    // Because the mon used to store an MDSMap where we now
    // store an FSMap, FSMap knows how to decode the legacy
    // MDSMap format (it never needs to encode it though).
    MDSMap legacy_mds_map;

    // Decoding an MDSMap (upgrade)
    ::decode(epoch, p);
    ::decode(legacy_mds_map.flags, p);
    ::decode(legacy_mds_map.last_failure, p);
    ::decode(legacy_mds_map.root, p);
    ::decode(legacy_mds_map.session_timeout, p);
    ::decode(legacy_mds_map.session_autoclose, p);
    ::decode(legacy_mds_map.max_file_size, p);
    ::decode(legacy_mds_map.max_mds, p);
    ::decode(legacy_mds_map.mds_info, p);
    if (struct_v < 3) {
      __u32 n;
      ::decode(n, p);
      while (n--) {
        __u32 m;
        ::decode(m, p);
        legacy_mds_map.data_pools.push_back(m);
      }
      __s32 s;
      ::decode(s, p);
      legacy_mds_map.cas_pool = s;
    } else {
      ::decode(legacy_mds_map.data_pools, p);
      ::decode(legacy_mds_map.cas_pool, p);
    }

    // kclient ignores everything from here
    __u16 ev = 1;
    if (struct_v >= 2)
      ::decode(ev, p);
    if (ev >= 3)
      ::decode(legacy_mds_map.compat, p);
    else
      legacy_mds_map.compat = get_mdsmap_compat_set_base();
    if (ev < 5) {
      __u32 n;
      ::decode(n, p);
      legacy_mds_map.metadata_pool = n;
    } else {
      ::decode(legacy_mds_map.metadata_pool, p);
    }
    ::decode(legacy_mds_map.created, p);
    ::decode(legacy_mds_map.modified, p);
    ::decode(legacy_mds_map.tableserver, p);
    ::decode(legacy_mds_map.in, p);
    std::map<mds_rank_t,int32_t> inc;  // Legacy field, parse and drop
    ::decode(inc, p);
    ::decode(legacy_mds_map.up, p);
    ::decode(legacy_mds_map.failed, p);
    ::decode(legacy_mds_map.stopped, p);
    if (ev >= 4)
      ::decode(legacy_mds_map.last_failure_osd_epoch, p);
    if (ev >= 6) {
      if (ev < 10) {
	// previously this was a bool about snaps, not a flag map
	bool flag;
	::decode(flag, p);
	legacy_mds_map.ever_allowed_features = flag ?
	  CEPH_MDSMAP_ALLOW_SNAPS : 0;
	::decode(flag, p);
	legacy_mds_map.explicitly_allowed_features = flag ?
	  CEPH_MDSMAP_ALLOW_SNAPS : 0;
	if (legacy_mds_map.max_mds > 1) {
	  legacy_mds_map.set_multimds_allowed();
	}
      } else {
	::decode(legacy_mds_map.ever_allowed_features, p);
	::decode(legacy_mds_map.explicitly_allowed_features, p);
      }
    } else {
      legacy_mds_map.ever_allowed_features = CEPH_MDSMAP_ALLOW_CLASSICS;
      legacy_mds_map.explicitly_allowed_features = 0;
      if (legacy_mds_map.max_mds > 1) {
	legacy_mds_map.set_multimds_allowed();
      }
    }
    if (ev >= 7)
      ::decode(legacy_mds_map.inline_data_enabled, p);

    if (ev >= 8) {
      assert(struct_v >= 5);
      ::decode(legacy_mds_map.enabled, p);
      ::decode(legacy_mds_map.fs_name, p);
    } else {
      legacy_mds_map.fs_name = "default";
      if (epoch > 1) {
        // If an MDS has ever been started, epoch will be greater than 1,
        // assume filesystem is enabled.
        legacy_mds_map.enabled = true;
      } else {
        // Upgrading from a cluster that never used an MDS, switch off
        // filesystem until it's explicitly enabled.
        legacy_mds_map.enabled = false;
      }
    }

    if (ev >= 9) {
      ::decode(legacy_mds_map.damaged, p);
    }

    // We're upgrading, populate filesystems from the legacy fields
    filesystems.clear();
    standby_daemons.clear();
    standby_epochs.clear();
    mds_roles.clear();
    compat = legacy_mds_map.compat;
    enable_multiple = false;

    // Synthesise a Filesystem from legacy_mds_map, if enabled
    if (legacy_mds_map.enabled) {
      // Construct a Filesystem from the legacy MDSMap
      auto migrate_fs = std::make_shared<Filesystem>(); 
      migrate_fs->fscid = FS_CLUSTER_ID_ANONYMOUS;
      migrate_fs->mds_map = legacy_mds_map;
      migrate_fs->mds_map.epoch = epoch;
      filesystems[migrate_fs->fscid] = migrate_fs;

      // List of GIDs that had invalid states
      std::set<mds_gid_t> drop_gids;

      // Construct mds_roles, standby_daemons, and remove
      // standbys from the MDSMap in the Filesystem.
      for (auto &p : migrate_fs->mds_map.mds_info) {
        if (p.second.state == MDSMap::STATE_STANDBY_REPLAY) {
          // In legacy MDSMap, standby replay daemons don't have
          // rank set, but since FSMap they do.
          p.second.rank = p.second.standby_for_rank;
        }
        if (p.second.rank == MDS_RANK_NONE) {
          if (p.second.state != MDSMap::STATE_STANDBY) {
            // Old MDSMaps can have down:dne here, which
            // is invalid in an FSMap (#17837)
            drop_gids.insert(p.first);
          } else {
            insert(p.second); // into standby_daemons
          }
        } else {
          mds_roles[p.first] = migrate_fs->fscid;
        }
      }
      for (const auto &p : standby_daemons) {
        // Erase from this Filesystem's MDSMap, because it has
        // been copied into FSMap::Standby_daemons above
        migrate_fs->mds_map.mds_info.erase(p.first);
      }
      for (const auto &gid : drop_gids) {
        // Throw away all info for this MDS because it was identified
        // as having invalid state above.
        migrate_fs->mds_map.mds_info.erase(gid);
      }

      legacy_client_fscid = migrate_fs->fscid;
    } else {
      legacy_client_fscid = FS_CLUSTER_ID_NONE;
    }
  } else {
    ::decode(epoch, p);
    ::decode(next_filesystem_id, p);
    ::decode(legacy_client_fscid, p);
    ::decode(compat, p);
    ::decode(enable_multiple, p);
    std::vector<Filesystem> fs_list;
    ::decode(fs_list, p);
    filesystems.clear();
    for (std::vector<Filesystem>::const_iterator fs = fs_list.begin(); fs != fs_list.end(); ++fs) {
      filesystems[fs->fscid] = std::make_shared<Filesystem>(*fs);
    }

    ::decode(mds_roles, p);
    ::decode(standby_daemons, p);
    ::decode(standby_epochs, p);
    if (struct_v >= 7) {
      ::decode(ever_enabled_multiple, p);
    }
  }

  DECODE_FINISH(p);
}

void FSMap::sanitize(std::function<bool(int64_t pool)> pool_exists)
{
  for (auto &fs : filesystems) {
    fs.second->mds_map.sanitize(pool_exists);
  }
}

void Filesystem::encode(bufferlist& bl, uint64_t features) const
{
  ENCODE_START(1, 1, bl);
  ::encode(fscid, bl);
  bufferlist mdsmap_bl;
  mds_map.encode(mdsmap_bl, features);
  ::encode(mdsmap_bl, bl);
  ENCODE_FINISH(bl);
}

void Filesystem::decode(bufferlist::iterator& p)
{
  DECODE_START(1, p);
  ::decode(fscid, p);
  bufferlist mdsmap_bl;
  ::decode(mdsmap_bl, p);
  bufferlist::iterator mdsmap_bl_iter = mdsmap_bl.begin();
  mds_map.decode(mdsmap_bl_iter);
  DECODE_FINISH(p);
}

int FSMap::parse_filesystem(
      boost::string_view ns_str,
      std::shared_ptr<const Filesystem> *result
      ) const
{
  std::string ns_err;
  std::string s(ns_str);
  fs_cluster_id_t fscid = strict_strtol(s.c_str(), 10, &ns_err);
  if (!ns_err.empty() || filesystems.count(fscid) == 0) {
    for (auto &fs : filesystems) {
      if (fs.second->mds_map.fs_name == s) {
        *result = std::const_pointer_cast<const Filesystem>(fs.second);
        return 0;
      }
    }
    return -ENOENT;
  } else {
    *result = get_filesystem(fscid);
    return 0;
  }
}

void Filesystem::print(std::ostream &out) const
{
  out << "Filesystem '" << mds_map.fs_name
      << "' (" << fscid << ")" << std::endl;
  mds_map.print(out);
}

mds_gid_t FSMap::find_standby_for(mds_role_t role, boost::string_view name) const
{
  mds_gid_t result = MDS_GID_NONE;

  // First see if we have a STANDBY_REPLAY
  auto fs = get_filesystem(role.fscid);
  for (const auto &i : fs->mds_map.mds_info) {
    const auto &info = i.second;
    if (info.rank == role.rank && info.state == MDSMap::STATE_STANDBY_REPLAY) {
      return info.global_id;
    }
  }

  // See if there are any STANDBY daemons available
  for (const auto &i : standby_daemons) {
    const auto &gid = i.first;
    const auto &info = i.second;
    assert(info.state == MDSMap::STATE_STANDBY);
    assert(info.rank == MDS_RANK_NONE);

    if (info.laggy()) {
      continue;
    }

    // The mds_info_t may or may not tell us exactly which filesystem
    // the standby_for_rank refers to: lookup via legacy_client_fscid
    mds_role_t target_role = {
      info.standby_for_fscid == FS_CLUSTER_ID_NONE ?
        legacy_client_fscid : info.standby_for_fscid,
      info.standby_for_rank};

    if ((target_role.rank == role.rank && target_role.fscid == role.fscid)
        || (name.length() && info.standby_for_name == name)) {
      // It's a named standby for *me*, use it.
      return gid;
    } else if (
        info.standby_for_rank < 0 && info.standby_for_name.length() == 0 &&
        (info.standby_for_fscid == FS_CLUSTER_ID_NONE ||
         info.standby_for_fscid == role.fscid)) {
        // It's not a named standby for anyone, use it if we don't find
        // a named standby for me later, unless it targets another FSCID.
        result = gid;
      }
  }

  return result;
}

mds_gid_t FSMap::find_unused_for(mds_role_t role,
				 bool force_standby_active) const {
  for (const auto &i : standby_daemons) {
    const auto &gid = i.first;
    const auto &info = i.second;
    assert(info.state == MDSMap::STATE_STANDBY);

    if (info.laggy() || info.rank >= 0)
      continue;

    if (info.standby_for_fscid != FS_CLUSTER_ID_NONE &&
        info.standby_for_fscid != role.fscid)
      continue;
    if (info.standby_for_rank != MDS_RANK_NONE &&
        info.standby_for_rank != role.rank)
      continue;

    // To be considered 'unused' a daemon must either not
    // be selected for standby-replay or the force_standby_active
    // setting must be enabled to use replay daemons anyway.
    if (!info.standby_replay || force_standby_active) {
      return gid;
    }
  }
  return MDS_GID_NONE;
}

mds_gid_t FSMap::find_replacement_for(mds_role_t role, boost::string_view name,
                               bool force_standby_active) const {
  const mds_gid_t standby = find_standby_for(role, name);
  if (standby)
    return standby;
  else
    return find_unused_for(role, force_standby_active);
}

void FSMap::sanity() const
{
  if (legacy_client_fscid != FS_CLUSTER_ID_NONE) {
    assert(filesystems.count(legacy_client_fscid) == 1);
  }

  for (const auto &i : filesystems) {
    auto fs = i.second;
    assert(fs->mds_map.compat.compare(compat) == 0);
    assert(fs->fscid == i.first);
    for (const auto &j : fs->mds_map.mds_info) {
      assert(j.second.rank != MDS_RANK_NONE);
      assert(mds_roles.count(j.first) == 1);
      assert(standby_daemons.count(j.first) == 0);
      assert(standby_epochs.count(j.first) == 0);
      assert(mds_roles.at(j.first) == i.first);
      if (j.second.state != MDSMap::STATE_STANDBY_REPLAY) {
        assert(fs->mds_map.up.at(j.second.rank) == j.first);
        assert(fs->mds_map.failed.count(j.second.rank) == 0);
        assert(fs->mds_map.damaged.count(j.second.rank) == 0);
      }
    }

    for (const auto &j : fs->mds_map.up) {
      mds_rank_t rank = j.first;
      assert(fs->mds_map.in.count(rank) == 1);
      mds_gid_t gid = j.second;
      assert(fs->mds_map.mds_info.count(gid) == 1);
    }
  }

  for (const auto &i : standby_daemons) {
    assert(i.second.state == MDSMap::STATE_STANDBY);
    assert(i.second.rank == MDS_RANK_NONE);
    assert(i.second.global_id == i.first);
    assert(standby_epochs.count(i.first) == 1);
    assert(mds_roles.count(i.first) == 1);
    assert(mds_roles.at(i.first) == FS_CLUSTER_ID_NONE);
  }

  for (const auto &i : standby_epochs) {
    assert(standby_daemons.count(i.first) == 1);
  }

  for (const auto &i : mds_roles) {
    if (i.second == FS_CLUSTER_ID_NONE) {
      assert(standby_daemons.count(i.first) == 1);
    } else {
      assert(filesystems.count(i.second) == 1);
      assert(filesystems.at(i.second)->mds_map.mds_info.count(i.first) == 1);
    }
  }
}

void FSMap::promote(
    mds_gid_t standby_gid,
    const std::shared_ptr<Filesystem> &filesystem,
    mds_rank_t assigned_rank)
{
  assert(gid_exists(standby_gid));
  bool is_standby_replay = mds_roles.at(standby_gid) != FS_CLUSTER_ID_NONE;
  if (!is_standby_replay) {
    assert(standby_daemons.count(standby_gid));
    assert(standby_daemons.at(standby_gid).state == MDSMap::STATE_STANDBY);
  }

  MDSMap &mds_map = filesystem->mds_map;

  // Insert daemon state to Filesystem
  if (!is_standby_replay) {
    mds_map.mds_info[standby_gid] = standby_daemons.at(standby_gid);
  } else {
    assert(mds_map.mds_info.count(standby_gid));
    assert(mds_map.mds_info.at(standby_gid).state == MDSMap::STATE_STANDBY_REPLAY);
    assert(mds_map.mds_info.at(standby_gid).rank == assigned_rank);
  }
  MDSMap::mds_info_t &info = mds_map.mds_info[standby_gid];

  if (mds_map.stopped.erase(assigned_rank)) {
    // The cluster is being expanded with a stopped rank
    info.state = MDSMap::STATE_STARTING;
  } else if (!mds_map.is_in(assigned_rank)) {
    // The cluster is being expanded with a new rank
    info.state = MDSMap::STATE_CREATING;
  } else {
    // An existing rank is being assigned to a replacement
    info.state = MDSMap::STATE_REPLAY;
    mds_map.failed.erase(assigned_rank);
  }
  info.rank = assigned_rank;
  info.inc = epoch;
  mds_roles[standby_gid] = filesystem->fscid;

  // Update the rank state in Filesystem
  mds_map.in.insert(assigned_rank);
  mds_map.up[assigned_rank] = standby_gid;

  // Remove from the list of standbys
  if (!is_standby_replay) {
    standby_daemons.erase(standby_gid);
    standby_epochs.erase(standby_gid);
  }

  // Indicate that Filesystem has been modified
  mds_map.epoch = epoch;
}

void FSMap::assign_standby_replay(
    const mds_gid_t standby_gid,
    const fs_cluster_id_t leader_ns,
    const mds_rank_t leader_rank)
{
  assert(mds_roles.at(standby_gid) == FS_CLUSTER_ID_NONE);
  assert(gid_exists(standby_gid));
  assert(!gid_has_rank(standby_gid));
  assert(standby_daemons.count(standby_gid));

  // Insert to the filesystem
  auto fs = filesystems.at(leader_ns);
  fs->mds_map.mds_info[standby_gid] = standby_daemons.at(standby_gid);
  fs->mds_map.mds_info[standby_gid].rank = leader_rank;
  fs->mds_map.mds_info[standby_gid].state = MDSMap::STATE_STANDBY_REPLAY;
  mds_roles[standby_gid] = leader_ns;

  // Remove from the list of standbys
  standby_daemons.erase(standby_gid);
  standby_epochs.erase(standby_gid);

  // Indicate that Filesystem has been modified
  fs->mds_map.epoch = epoch;
}

void FSMap::erase(mds_gid_t who, epoch_t blacklist_epoch)
{
  if (mds_roles.at(who) == FS_CLUSTER_ID_NONE) {
    standby_daemons.erase(who);
    standby_epochs.erase(who);
  } else {
    auto &fs = filesystems.at(mds_roles.at(who));
    const auto &info = fs->mds_map.mds_info.at(who);
    if (info.state != MDSMap::STATE_STANDBY_REPLAY) {
      if (info.state == MDSMap::STATE_CREATING) {
        // If this gid didn't make it past CREATING, then forget
        // the rank ever existed so that next time it's handed out
        // to a gid it'll go back into CREATING.
        fs->mds_map.in.erase(info.rank);
      } else {
        // Put this rank into the failed list so that the next available
        // STANDBY will pick it up.
        fs->mds_map.failed.insert(info.rank);
      }
      assert(fs->mds_map.up.at(info.rank) == info.global_id);
      fs->mds_map.up.erase(info.rank);
    }
    fs->mds_map.mds_info.erase(who);
    fs->mds_map.last_failure_osd_epoch = blacklist_epoch;
    fs->mds_map.epoch = epoch;
  }

  mds_roles.erase(who);
}

void FSMap::damaged(mds_gid_t who, epoch_t blacklist_epoch)
{
  assert(mds_roles.at(who) != FS_CLUSTER_ID_NONE);
  auto fs = filesystems.at(mds_roles.at(who));
  mds_rank_t rank = fs->mds_map.mds_info[who].rank;

  erase(who, blacklist_epoch);
  fs->mds_map.failed.erase(rank);
  fs->mds_map.damaged.insert(rank);

  assert(fs->mds_map.epoch == epoch);
}

/**
 * Update to indicate that the rank `rank` is to be removed
 * from the damaged list of the filesystem `fscid`
 */
bool FSMap::undamaged(const fs_cluster_id_t fscid, const mds_rank_t rank)
{
  auto fs = filesystems.at(fscid);

  if (fs->mds_map.damaged.erase(rank)) {
    fs->mds_map.failed.insert(rank);
    fs->mds_map.epoch = epoch;
    return true;
  } else {
    return false;
  }
}

void FSMap::insert(const MDSMap::mds_info_t &new_info)
{
  assert(new_info.state == MDSMap::STATE_STANDBY);
  assert(new_info.rank == MDS_RANK_NONE);
  mds_roles[new_info.global_id] = FS_CLUSTER_ID_NONE;
  standby_daemons[new_info.global_id] = new_info;
  standby_epochs[new_info.global_id] = epoch;
}

std::list<mds_gid_t> FSMap::stop(mds_gid_t who)
{
  assert(mds_roles.at(who) != FS_CLUSTER_ID_NONE);
  auto fs = filesystems.at(mds_roles.at(who));
  const auto &info = fs->mds_map.mds_info.at(who);
  fs->mds_map.up.erase(info.rank);
  fs->mds_map.in.erase(info.rank);
  fs->mds_map.stopped.insert(info.rank);

  // Also drop any standby replays that were following this rank
  std::list<mds_gid_t> standbys;
  for (const auto &i : fs->mds_map.mds_info) {
    const auto &other_gid = i.first;
    const auto &other_info = i.second;
    if (other_info.rank == info.rank
        && other_info.state == MDSMap::STATE_STANDBY_REPLAY) {
      standbys.push_back(other_gid);
      erase(other_gid, 0);
    }
  }

  fs->mds_map.mds_info.erase(who);
  mds_roles.erase(who);

  fs->mds_map.epoch = epoch;

  return standbys;
}


/**
 * Given one of the following forms:
 *   <fs name>:<rank>
 *   <fs id>:<rank>
 *   <rank>
 *
 * Parse into a mds_role_t.  The rank-only form is only valid
 * if legacy_client_ns is set.
 */
int FSMap::parse_role(
    boost::string_view role_str,
    mds_role_t *role,
    std::ostream &ss) const
{
  size_t colon_pos = role_str.find(":");
  size_t rank_pos;
  std::shared_ptr<const Filesystem> fs;
  if (colon_pos == std::string::npos) {
    if (legacy_client_fscid == FS_CLUSTER_ID_NONE) {
      ss << "No filesystem selected";
      return -ENOENT;
    }
    fs = get_filesystem(legacy_client_fscid);
    rank_pos = 0;
  } else {
    if (parse_filesystem(role_str.substr(0, colon_pos), &fs) < 0) {
      ss << "Invalid filesystem";
      return -ENOENT;
    }
    rank_pos = colon_pos+1;
  }

  mds_rank_t rank;
  std::string err;
  std::string rank_str(role_str.substr(rank_pos));
  long rank_i = strict_strtol(rank_str.c_str(), 10, &err);
  if (rank_i < 0 || !err.empty()) {
    ss << "Invalid rank '" << rank_str << "'";
    return -EINVAL;
  } else {
    rank = rank_i;
  }

  if (fs->mds_map.in.count(rank) == 0) {
    ss << "Rank '" << rank << "' not found";
    return -ENOENT;
  }

  *role = {fs->fscid, rank};

  return 0;
}
