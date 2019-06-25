// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <stdlib.h>
#include <limits.h>
#include <sstream>
#include <boost/regex.hpp>

#include "include/assert.h"
#include "include/stringify.h"

#include "mon/Monitor.h"
#include "mon/HealthService.h"
#include "mon/HealthMonitor.h"
#include "mon/DataHealthService.h"

#include "messages/MMonHealth.h"
#include "messages/MMonHealthChecks.h"

#include "common/Formatter.h"

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, this)
static ostream& _prefix(std::ostream *_dout, const Monitor *mon,
                        const HealthMonitor *hmon) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name() << ").health ";
}

HealthMonitor::HealthMonitor(Monitor *m, Paxos *p, const string& service_name)
  : PaxosService(m, p, service_name) {
}

void HealthMonitor::init()
{
  dout(10) << __func__ << dendl;
}

void HealthMonitor::create_initial()
{
  dout(10) << __func__ << dendl;
}

void HealthMonitor::update_from_paxos(bool *need_bootstrap)
{
  version = get_last_committed();
  dout(10) << __func__ << dendl;
  load_health();

  bufferlist qbl;
  mon->store->get(service_name, "quorum", qbl);
  if (qbl.length()) {
    auto p = qbl.begin();
    ::decode(quorum_checks, p);
  } else {
    quorum_checks.clear();
  }

  bufferlist lbl;
  mon->store->get(service_name, "leader", lbl);
  if (lbl.length()) {
    auto p = lbl.begin();
    ::decode(leader_checks, p);
  } else {
    leader_checks.clear();
  }

  dout(20) << "dump:";
  JSONFormatter jf(true);
  jf.open_object_section("health");
  jf.open_object_section("quorum_health");
  for (auto& p : quorum_checks) {
    string s = string("mon.") + stringify(p.first);
    jf.dump_object(s.c_str(), p.second);
  }
  jf.close_section();
  jf.dump_object("leader_health", leader_checks);
  jf.close_section();
  jf.flush(*_dout);
  *_dout << dendl;
}

void HealthMonitor::create_pending()
{
  dout(10) << " " << version << dendl;
}

void HealthMonitor::encode_pending(MonitorDBStore::TransactionRef t)
{
  ++version;
  dout(10) << " " << version << dendl;
  put_last_committed(t, version);

  bufferlist qbl;
  ::encode(quorum_checks, qbl);
  t->put(service_name, "quorum", qbl);
  bufferlist lbl;
  ::encode(leader_checks, lbl);
  t->put(service_name, "leader", lbl);

  health_check_map_t pending_health;

  // combine per-mon details carefully...
  map<string,set<string>> names; // code -> <mon names>
  for (auto p : quorum_checks) {
    for (auto q : p.second.checks) {
      names[q.first].insert(mon->monmap->get_name(p.first));
    }
    pending_health.merge(p.second);
  }
  for (auto &p : pending_health.checks) {
    p.second.summary = boost::regex_replace(
      p.second.summary,
      boost::regex("%hasorhave%"),
      names[p.first].size() > 1 ? "have" : "has");
    p.second.summary = boost::regex_replace(
      p.second.summary,
      boost::regex("%names%"), stringify(names[p.first]));
    p.second.summary = boost::regex_replace(
      p.second.summary,
      boost::regex("%plurals%"),
      names[p.first].size() > 1 ? "s" : "");
    p.second.summary = boost::regex_replace(
      p.second.summary,
      boost::regex("%isorare%"),
      names[p.first].size() > 1 ? "are" : "is");
  }

  pending_health.merge(leader_checks);
  encode_health(pending_health, t);
}

version_t HealthMonitor::get_trim_to()
{
  // we don't actually need *any* old states, but keep a few.
  if (version > 5) {
    return version - 5;
  }
  return 0;
}

bool HealthMonitor::preprocess_query(MonOpRequestRef op)
{
  return false;
}

bool HealthMonitor::prepare_update(MonOpRequestRef op)
{
  Message *m = op->get_req();
  dout(7) << "prepare_update " << *m
	  << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_HEALTH:
    {
      MMonHealth *hm = static_cast<MMonHealth*>(op->get_req());
      int service_type = hm->get_service_type();
      if (services.count(service_type) == 0) {
	dout(1) << __func__ << " service type " << service_type
		<< " not registered -- drop message!" << dendl;
	return false;
      }
      return services[service_type]->service_dispatch(op);
    }
  case MSG_MON_HEALTH_CHECKS:
    return prepare_health_checks(op);
  default:
    return false;
  }
}

bool HealthMonitor::prepare_health_checks(MonOpRequestRef op)
{
  MMonHealthChecks *m = static_cast<MMonHealthChecks*>(op->get_req());
  // no need to check if it's changed, the peon has done so
  quorum_checks[m->get_source().num()] = std::move(m->health_checks);
  return true;
}

void HealthMonitor::tick()
{
  if (!is_active()) {
    return;
  }
  dout(10) << __func__ << dendl;
  bool changed = false;
  if (check_member_health()) {
    changed = true;
  }
  if (!mon->is_leader()) {
    return;
  }
  if (check_leader_health()) {
    changed = true;
  }
  if (changed) {
    propose_pending();
  }
}

bool HealthMonitor::check_member_health()
{
  dout(20) << __func__ << dendl;
  bool changed = false;

  // snapshot of usage
  DataStats stats;
  get_fs_stats(stats.fs_stats, g_conf->mon_data.c_str());
  map<string,uint64_t> extra;
  uint64_t store_size = mon->store->get_estimated_size(extra);
  assert(store_size > 0);
  stats.store_stats.bytes_total = store_size;
  stats.store_stats.bytes_sst = extra["sst"];
  stats.store_stats.bytes_log = extra["log"];
  stats.store_stats.bytes_misc = extra["misc"];
  stats.last_update = ceph_clock_now();
  dout(10) << __func__ << " avail " << stats.fs_stats.avail_percent << "%"
	   << " total " << prettybyte_t(stats.fs_stats.byte_total)
	   << ", used " << prettybyte_t(stats.fs_stats.byte_used)
	   << ", avail " << prettybyte_t(stats.fs_stats.byte_avail) << dendl;

  // MON_DISK_{LOW,CRIT,BIG}
  health_check_map_t next;
  if (stats.fs_stats.avail_percent <= g_conf->mon_data_avail_crit) {
    stringstream ss, ss2;
    ss << "mon%plurals% %names% %isorare% very low on available space";
    auto& d = next.add("MON_DISK_CRIT", HEALTH_ERR, ss.str());
    ss2 << "mon." << mon->name << " has " << stats.fs_stats.avail_percent
	<< "% avail";
    d.detail.push_back(ss2.str());
  } else if (stats.fs_stats.avail_percent <= g_conf->mon_data_avail_warn) {
    stringstream ss, ss2;
    ss << "mon%plurals% %names% %isorare% low on available space";
    auto& d = next.add("MON_DISK_LOW", HEALTH_WARN, ss.str());
    ss2 << "mon." << mon->name << " has " << stats.fs_stats.avail_percent
	<< "% avail";
    d.detail.push_back(ss2.str());
  }
  if (stats.store_stats.bytes_total >= g_conf->mon_data_size_warn) {
    stringstream ss, ss2;
    ss << "mon%plurals% %names% %isorare% using a lot of disk space";
    auto& d = next.add("MON_DISK_BIG", HEALTH_WARN, ss.str());
    ss2 << "mon." << mon->name << " is "
	<< prettybyte_t(stats.store_stats.bytes_total)
	<< " >= mon_data_size_warn ("
	<< prettybyte_t(g_conf->mon_data_size_warn) << ")";
    d.detail.push_back(ss2.str());
  }

  // OSD_NO_DOWN_OUT_INTERVAL
  {
    // Warn if 'mon_osd_down_out_interval' is set to zero.
    // Having this option set to zero on the leader acts much like the
    // 'noout' flag.  It's hard to figure out what's going wrong with clusters
    // without the 'noout' flag set but acting like that just the same, so
    // we report a HEALTH_WARN in case this option is set to zero.
    // This is an ugly hack to get the warning out, but until we find a way
    // to spread global options throughout the mon cluster and have all mons
    // using a base set of the same options, we need to work around this sort
    // of things.
    // There's also the obvious drawback that if this is set on a single
    // monitor on a 3-monitor cluster, this warning will only be shown every
    // third monitor connection.
    if (g_conf->mon_warn_on_osd_down_out_interval_zero &&
        g_conf->mon_osd_down_out_interval == 0) {
      ostringstream ss, ds;
      ss << "mon%plurals% %names% %hasorhave% mon_osd_down_out_interval set to 0";
      auto& d = next.add("OSD_NO_DOWN_OUT_INTERVAL", HEALTH_WARN, ss.str());
      ds << "mon." << mon->name << " has mon_osd_down_out_interval set to 0";
      d.detail.push_back(ds.str());
    }
  }

  auto p = quorum_checks.find(mon->rank);
  if (p == quorum_checks.end()) {
    if (next.empty()) {
      return false;
    }
  } else {
    if (p->second == next) {
      return false;
    }
  }

  if (mon->is_leader()) {
    // prepare to propose
    quorum_checks[mon->rank] = next;
    changed = true;
  } else {
    // tell the leader
    mon->messenger->send_message(new MMonHealthChecks(next),
                                 mon->monmap->get_inst(mon->get_leader()));
  }

  return changed;
}

bool HealthMonitor::check_leader_health()
{
  dout(20) << __func__ << dendl;
  bool changed = false;

  // prune quorum_health
  {
    auto& qset = mon->get_quorum();
    auto p = quorum_checks.begin();
    while (p != quorum_checks.end()) {
      if (qset.count(p->first) == 0) {
	p = quorum_checks.erase(p);
	changed = true;
      } else {
	++p;
      }
    }
  }

  health_check_map_t next;

  // MON_DOWN
  {
    int max = mon->monmap->size();
    int actual = mon->get_quorum().size();
    if (actual < max) {
      ostringstream ss;
      ss << (max-actual) << "/" << max << " mons down, quorum "
	 << mon->get_quorum_names();
      auto& d = next.add("MON_DOWN", HEALTH_WARN, ss.str());
      set<int> q = mon->get_quorum();
      for (int i=0; i<max; i++) {
	if (q.count(i) == 0) {
	  ostringstream ss;
	  ss << "mon." << mon->monmap->get_name(i) << " (rank " << i
	     << ") addr " << mon->monmap->get_addr(i)
	     << " is down (out of quorum)";
	  d.detail.push_back(ss.str());
	}
      }
    }
  }

  // MON_CLOCK_SKEW
  if (!mon->timecheck_skews.empty()) {
    list<string> warns;
    list<string> details;
    for (map<entity_inst_t,double>::iterator i = mon->timecheck_skews.begin();
	 i != mon->timecheck_skews.end(); ++i) {
      entity_inst_t inst = i->first;
      double skew = i->second;
      double latency = mon->timecheck_latencies[inst];
      string name = mon->monmap->get_name(inst.addr);
      ostringstream tcss;
      health_status_t tcstatus = mon->timecheck_status(tcss, skew, latency);
      if (tcstatus != HEALTH_OK) {
	warns.push_back(name);
	ostringstream tmp_ss;
	tmp_ss << "mon." << name
	       << " addr " << inst.addr << " " << tcss.str()
	       << " (latency " << latency << "s)";
	details.push_back(tmp_ss.str());
      }
    }
    if (!warns.empty()) {
      ostringstream ss;
      ss << "clock skew detected on";
      while (!warns.empty()) {
	ss << " mon." << warns.front();
	warns.pop_front();
	if (!warns.empty())
	  ss << ",";
      }
      auto& d = next.add("MON_CLOCK_SKEW", HEALTH_WARN, ss.str());
      d.detail.swap(details);
    }
  }

  if (next != leader_checks) {
    changed = true;
    leader_checks = next;
  }
  return changed;
}
