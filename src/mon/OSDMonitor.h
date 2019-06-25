// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013,2014 Cloudwatt <libre.licensing@cloudwatt.com>
 *
 * Author: Loic Dachary <loic@dachary.org>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

/* Object Store Device (OSD) Monitor
 */

#ifndef CEPH_OSDMONITOR_H
#define CEPH_OSDMONITOR_H

#include <map>
#include <set>
using namespace std;

#include "include/types.h"
#include "common/simple_cache.hpp"
#include "msg/Messenger.h"

#include "osd/OSDMap.h"
#include "osd/OSDMapMapping.h"

#include "CreatingPGs.h"
#include "PaxosService.h"

class Monitor;
class PGMap;
class MonSession;
class MOSDMap;

#include "erasure-code/ErasureCodeInterface.h"
#include "mon/MonOpRequest.h"

/// information about a particular peer's failure reports for one osd
struct failure_reporter_t {
  utime_t failed_since;     ///< when they think it failed
  MonOpRequestRef op;       ///< failure op request

  failure_reporter_t() {}
  explicit failure_reporter_t(utime_t s) : failed_since(s) {}
  ~failure_reporter_t() { }
};

/// information about all failure reports for one osd
struct failure_info_t {
  map<int, failure_reporter_t> reporters;  ///< reporter -> failed_since etc
  utime_t max_failed_since;                ///< most recent failed_since

  failure_info_t() {}

  utime_t get_failed_since() {
    if (max_failed_since == utime_t() && !reporters.empty()) {
      // the old max must have canceled; recalculate.
      for (map<int, failure_reporter_t>::iterator p = reporters.begin();
	   p != reporters.end();
	   ++p)
	if (p->second.failed_since > max_failed_since)
	  max_failed_since = p->second.failed_since;
    }
    return max_failed_since;
  }

  // set the message for the latest report.  return any old op request we had,
  // if any, so we can discard it.
  MonOpRequestRef add_report(int who, utime_t failed_since,
			     MonOpRequestRef op) {
    map<int, failure_reporter_t>::iterator p = reporters.find(who);
    if (p == reporters.end()) {
      if (max_failed_since < failed_since)
	max_failed_since = failed_since;
      p = reporters.insert(map<int, failure_reporter_t>::value_type(who, failure_reporter_t(failed_since))).first;
    }

    MonOpRequestRef ret = p->second.op;
    p->second.op = op;
    return ret;
  }

  void take_report_messages(list<MonOpRequestRef>& ls) {
    for (map<int, failure_reporter_t>::iterator p = reporters.begin();
	 p != reporters.end();
	 ++p) {
      if (p->second.op) {
	ls.push_back(p->second.op);
        p->second.op.reset();
      }
    }
  }

  MonOpRequestRef cancel_report(int who) {
    map<int, failure_reporter_t>::iterator p = reporters.find(who);
    if (p == reporters.end())
      return MonOpRequestRef();
    MonOpRequestRef ret = p->second.op;
    reporters.erase(p);
    return ret;
  }
};


class LastEpochClean {
  struct Lec {
    vector<epoch_t> epoch_by_pg;
    ps_t next_missing = 0;
    epoch_t floor = std::numeric_limits<epoch_t>::max();
    void report(ps_t pg, epoch_t last_epoch_clean);
  };
  std::map<uint64_t, Lec> report_by_pool;
public:
  void report(const pg_t& pg, epoch_t last_epoch_clean);
  void remove_pool(uint64_t pool);
  epoch_t get_lower_bound(const OSDMap& latest) const;
};


class OSDMonitor : public PaxosService {
  CephContext *cct;

public:
  OSDMap osdmap;

  // [leader]
  OSDMap::Incremental pending_inc;
  map<int, bufferlist> pending_metadata;
  set<int>             pending_metadata_rm;
  map<int, failure_info_t> failure_info;
  map<int,utime_t>    down_pending_out;  // osd down -> out

  map<int,double> osd_weight;

  SimpleLRU<version_t, bufferlist> inc_osd_cache;
  SimpleLRU<version_t, bufferlist> full_osd_cache;

  bool check_failures(utime_t now);
  bool check_failure(utime_t now, int target_osd, failure_info_t& fi);
  void force_failure(int target_osd, int by);

  // the time of last msg(MSG_ALIVE and MSG_PGTEMP) proposed without delay
  utime_t last_attempted_minwait_time;

  bool _have_pending_crush();
  CrushWrapper &_get_stable_crush();
  void _get_pending_crush(CrushWrapper& newcrush);

  enum FastReadType {
    FAST_READ_OFF,
    FAST_READ_ON,
    FAST_READ_DEFAULT
  };

  // svc
public:  
  void create_initial() override;
  void get_store_prefixes(std::set<string>& s) override;

private:
  void update_from_paxos(bool *need_bootstrap) override;
  void create_pending() override;  // prepare a new pending
  void encode_pending(MonitorDBStore::TransactionRef t) override;
  void on_active() override;
  void on_restart() override;
  void on_shutdown() override;
  /**
   * we haven't delegated full version stashing to paxosservice for some time
   * now, making this function useless in current context.
   */
  void encode_full(MonitorDBStore::TransactionRef t) override { }
  /**
   * do not let paxosservice periodically stash full osdmaps, or we will break our
   * locally-managed full maps.  (update_from_paxos loads the latest and writes them
   * out going forward from there, but if we just synced that may mean we skip some.)
   */
  bool should_stash_full() override {
    return false;
  }

  /**
   * hook into trim to include the oldest full map in the trim transaction
   *
   * This ensures that anyone post-sync will have enough to rebuild their
   * full osdmaps.
   */
  void encode_trim_extra(MonitorDBStore::TransactionRef tx, version_t first) override;

  void update_msgr_features();
  int check_cluster_features(uint64_t features, stringstream &ss);
  /**
   * check if the cluster supports the features required by the
   * given crush map. Outputs the daemons which don't support it
   * to the stringstream.
   *
   * @returns true if the map is passable, false otherwise
   */
  bool validate_crush_against_features(const CrushWrapper *newcrush,
                                      stringstream &ss);
  void check_osdmap_subs();
  void share_map_with_random_osd();

  Mutex prime_pg_temp_lock = {"OSDMonitor::prime_pg_temp_lock"};
  struct PrimeTempJob : public ParallelPGMapper::Job {
    OSDMonitor *osdmon;
    PrimeTempJob(const OSDMap& om, OSDMonitor *m)
      : ParallelPGMapper::Job(&om), osdmon(m) {}
    void process(int64_t pool, unsigned ps_begin, unsigned ps_end) override {
      for (unsigned ps = ps_begin; ps < ps_end; ++ps) {
	pg_t pgid(ps, pool);
	osdmon->prime_pg_temp(*osdmap, pgid);
      }
    }
    void complete() override {}
  };
  void maybe_prime_pg_temp();
  void prime_pg_temp(const OSDMap& next, pg_t pgid);

  ParallelPGMapper mapper;                        ///< for background pg work
  OSDMapMapping mapping;                          ///< pg <-> osd mappings
  unique_ptr<ParallelPGMapper::Job> mapping_job;  ///< background mapping job
  void start_mapping();

  void update_logger();

  void handle_query(PaxosServiceMessage *m);
  bool preprocess_query(MonOpRequestRef op) override;  // true if processed.
  bool prepare_update(MonOpRequestRef op) override;
  bool should_propose(double &delay) override;

  version_t get_trim_to() override;

  bool can_mark_down(int o);
  bool can_mark_up(int o);
  bool can_mark_out(int o);
  bool can_mark_in(int o);

  // ...
  MOSDMap *build_latest_full();
  MOSDMap *build_incremental(epoch_t first, epoch_t last);
  void send_full(MonOpRequestRef op);
  void send_incremental(MonOpRequestRef op, epoch_t first);
public:
  // @param req an optional op request, if the osdmaps are replies to it. so
  //            @c Monitor::send_reply() can mark_event with it.
  void send_incremental(epoch_t first, MonSession *session, bool onetime,
			MonOpRequestRef req = MonOpRequestRef());

private:
  void print_utilization(ostream &out, Formatter *f, bool tree) const;

  bool check_source(PaxosServiceMessage *m, uuid_d fsid);
 
  bool preprocess_get_osdmap(MonOpRequestRef op);

  bool preprocess_mark_me_down(MonOpRequestRef op);

  friend class C_AckMarkedDown;
  bool preprocess_failure(MonOpRequestRef op);
  bool prepare_failure(MonOpRequestRef op);
  bool prepare_mark_me_down(MonOpRequestRef op);
  void process_failures();
  void take_all_failures(list<MonOpRequestRef>& ls);

  bool preprocess_full(MonOpRequestRef op);
  bool prepare_full(MonOpRequestRef op);

  bool preprocess_boot(MonOpRequestRef op);
  bool prepare_boot(MonOpRequestRef op);
  void _booted(MonOpRequestRef op, bool logit);

  void update_up_thru(int from, epoch_t up_thru);
  bool preprocess_alive(MonOpRequestRef op);
  bool prepare_alive(MonOpRequestRef op);
  void _reply_map(MonOpRequestRef op, epoch_t e);

  bool preprocess_pgtemp(MonOpRequestRef op);
  bool prepare_pgtemp(MonOpRequestRef op);

  bool preprocess_pg_created(MonOpRequestRef op);
  bool prepare_pg_created(MonOpRequestRef op);

  int _check_remove_pool(int64_t pool_id, const pg_pool_t &pool, ostream *ss);
  bool _check_become_tier(
      int64_t tier_pool_id, const pg_pool_t *tier_pool,
      int64_t base_pool_id, const pg_pool_t *base_pool,
      int *err, ostream *ss) const;
  bool _check_remove_tier(
      int64_t base_pool_id, const pg_pool_t *base_pool, const pg_pool_t *tier_pool,
      int *err, ostream *ss) const;

  int _prepare_remove_pool(int64_t pool, ostream *ss, bool no_fake);
  int _prepare_rename_pool(int64_t pool, string newname);

  bool preprocess_pool_op (MonOpRequestRef op);
  bool preprocess_pool_op_create (MonOpRequestRef op);
  bool prepare_pool_op (MonOpRequestRef op);
  bool prepare_pool_op_create (MonOpRequestRef op);
  bool prepare_pool_op_delete(MonOpRequestRef op);
  int crush_rename_bucket(const string& srcname,
			  const string& dstname,
			  ostream *ss);
  void check_legacy_ec_plugin(const string& plugin, 
			      const string& profile) const;
  int normalize_profile(const string& profilename, 
			ErasureCodeProfile &profile,
			bool force,
			ostream *ss);
  int crush_rule_create_erasure(const string &name,
				const string &profile,
				int *rule,
				ostream *ss);
  int get_crush_rule(const string &rule_name,
			int *crush_rule,
			ostream *ss);
  int get_erasure_code(const string &erasure_code_profile,
		       ErasureCodeInterfaceRef *erasure_code,
		       ostream *ss) const;
  int prepare_pool_crush_rule(const unsigned pool_type,
				 const string &erasure_code_profile,
				 const string &rule_name,
				 int *crush_rule,
				 ostream *ss);
  bool erasure_code_profile_in_use(
    const mempool::osdmap::map<int64_t, pg_pool_t> &pools,
    const string &profile,
    ostream *ss);
  int parse_erasure_code_profile(const vector<string> &erasure_code_profile,
				 map<string,string> *erasure_code_profile_map,
				 ostream *ss);
  int prepare_pool_size(const unsigned pool_type,
			const string &erasure_code_profile,
			unsigned *size, unsigned *min_size,
			ostream *ss);
  int prepare_pool_stripe_width(const unsigned pool_type,
				const string &erasure_code_profile,
				unsigned *stripe_width,
				ostream *ss);
  int check_pg_num(int64_t pool, int pg_num, int size, ostream* ss);
  int prepare_new_pool(string& name, uint64_t auid,
		       int crush_rule,
		       const string &crush_rule_name,
                       unsigned pg_num, unsigned pgp_num,
		       const string &erasure_code_profile,
                       const unsigned pool_type,
                       const uint64_t expected_num_objects,
                       FastReadType fast_read,
		       ostream *ss);
  int prepare_new_pool(MonOpRequestRef op);

  void set_pool_flags(int64_t pool_id, uint64_t flags);
  void clear_pool_flags(int64_t pool_id, uint64_t flags);
  bool update_pools_status();

  bool prepare_set_flag(MonOpRequestRef op, int flag);
  bool prepare_unset_flag(MonOpRequestRef op, int flag);

  void _pool_op_reply(MonOpRequestRef op,
                      int ret, epoch_t epoch, bufferlist *blp=NULL);

  struct C_Booted : public C_MonOp {
    OSDMonitor *cmon;
    bool logit;
    C_Booted(OSDMonitor *cm, MonOpRequestRef op_, bool l=true) :
      C_MonOp(op_), cmon(cm), logit(l) {}
    void _finish(int r) override {
      if (r >= 0)
	cmon->_booted(op, logit);
      else if (r == -ECANCELED)
        return;
      else if (r == -EAGAIN)
        cmon->dispatch(op);
      else
	assert(0 == "bad C_Booted return value");
    }
  };

  struct C_ReplyMap : public C_MonOp {
    OSDMonitor *osdmon;
    epoch_t e;
    C_ReplyMap(OSDMonitor *o, MonOpRequestRef op_, epoch_t ee)
      : C_MonOp(op_), osdmon(o), e(ee) {}
    void _finish(int r) override {
      if (r >= 0)
	osdmon->_reply_map(op, e);
      else if (r == -ECANCELED)
        return;
      else if (r == -EAGAIN)
	osdmon->dispatch(op);
      else
	assert(0 == "bad C_ReplyMap return value");
    }    
  };
  struct C_PoolOp : public C_MonOp {
    OSDMonitor *osdmon;
    int replyCode;
    int epoch;
    bufferlist reply_data;
    C_PoolOp(OSDMonitor * osd, MonOpRequestRef op_, int rc, int e, bufferlist *rd=NULL) :
      C_MonOp(op_), osdmon(osd), replyCode(rc), epoch(e) {
      if (rd)
	reply_data = *rd;
    }
    void _finish(int r) override {
      if (r >= 0)
	osdmon->_pool_op_reply(op, replyCode, epoch, &reply_data);
      else if (r == -ECANCELED)
        return;
      else if (r == -EAGAIN)
	osdmon->dispatch(op);
      else
	assert(0 == "bad C_PoolOp return value");
    }
  };

  bool preprocess_remove_snaps(MonOpRequestRef op);
  bool prepare_remove_snaps(MonOpRequestRef op);

  OpTracker op_tracker;

  int load_metadata(int osd, map<string, string>& m, ostream *err);
  void count_metadata(const string& field, Formatter *f);
public:
  void count_metadata(const string& field, map<string,int> *out);
protected:
  int get_osd_objectstore_type(int osd, std::string *type);
  bool is_pool_currently_all_bluestore(int64_t pool_id, const pg_pool_t &pool,
				       ostream *err);

  // when we last received PG stats from each osd
  map<int,utime_t> last_osd_report;
  // TODO: use last_osd_report to store the osd report epochs, once we don't
  //       need to upgrade from pre-luminous releases.
  map<int,epoch_t> osd_epochs;
  LastEpochClean last_epoch_clean;
  bool preprocess_beacon(MonOpRequestRef op);
  bool prepare_beacon(MonOpRequestRef op);
  epoch_t get_min_last_epoch_clean() const;

  friend class C_UpdateCreatingPGs;
  std::map<int, std::map<epoch_t, std::set<pg_t>>> creating_pgs_by_osd_epoch;
  std::vector<pg_t> pending_created_pgs;
  // the epoch when the pg mapping was calculated
  epoch_t creating_pgs_epoch = 0;
  creating_pgs_t creating_pgs;
  mutable std::mutex creating_pgs_lock;

  creating_pgs_t update_pending_pgs(const OSDMap::Incremental& inc,
				    const OSDMap& nextmap);
  void trim_creating_pgs(creating_pgs_t *creating_pgs,
			 const ceph::unordered_map<pg_t,pg_stat_t>& pgm);
  unsigned scan_for_creating_pgs(
    const mempool::osdmap::map<int64_t,pg_pool_t>& pools,
    const mempool::osdmap::set<int64_t>& removed_pools,
    utime_t modified,
    creating_pgs_t* creating_pgs) const;
  pair<int32_t, pg_t> get_parent_pg(pg_t pgid) const;
  void update_creating_pgs();
  void check_pg_creates_subs();
  epoch_t send_pg_creates(int osd, Connection *con, epoch_t next) const;

  int32_t _allocate_osd_id(int32_t* existing_id);

public:
  OSDMonitor(CephContext *cct, Monitor *mn, Paxos *p, const string& service_name);

  void tick() override;  // check state, take actions

  void get_health(list<pair<health_status_t,string> >& summary,
		  list<pair<health_status_t,string> > *detail,
		  CephContext *cct) const override;
  bool preprocess_command(MonOpRequestRef op);
  bool prepare_command(MonOpRequestRef op);
  bool prepare_command_impl(MonOpRequestRef op, map<string,cmd_vartype>& cmdmap);

  int validate_osd_create(
      const int32_t id,
      const uuid_d& uuid,
      const bool check_osd_exists,
      int32_t* existing_id,
      stringstream& ss);
  int prepare_command_osd_create(
      const int32_t id,
      const uuid_d& uuid,
      int32_t* existing_id,
      stringstream& ss);
  void do_osd_create(const int32_t id, const uuid_d& uuid,
		     const string& device_class,
		     int32_t* new_id);
  int prepare_command_osd_purge(int32_t id, stringstream& ss);
  int prepare_command_osd_destroy(int32_t id, stringstream& ss);
  int _prepare_command_osd_crush_remove(
      CrushWrapper &newcrush,
      int32_t id,
      int32_t ancestor,
      bool has_ancestor,
      bool unlink_only);
  void do_osd_crush_remove(CrushWrapper& newcrush);
  int prepare_command_osd_crush_remove(
      CrushWrapper &newcrush,
      int32_t id,
      int32_t ancestor,
      bool has_ancestor,
      bool unlink_only);
  int prepare_command_osd_remove(int32_t id);
  int prepare_command_osd_new(
      MonOpRequestRef op,
      const map<string,cmd_vartype>& cmdmap,
      const map<string,string>& secrets,
      stringstream &ss,
      Formatter *f);

  int prepare_command_pool_set(map<string,cmd_vartype> &cmdmap,
                               stringstream& ss);
  int prepare_command_pool_application(const string &prefix,
                                       map<string,cmd_vartype> &cmdmap,
                                       stringstream& ss);

  bool handle_osd_timeouts(const utime_t &now,
			   std::map<int,utime_t> &last_osd_report);

  void send_latest(MonOpRequestRef op, epoch_t start=0);
  void send_latest_now_nodelete(MonOpRequestRef op, epoch_t start=0) {
    op->mark_osdmon_event(__func__);
    send_incremental(op, start);
  }

  int get_version(version_t ver, bufferlist& bl) override;
  int get_version_full(version_t ver, bufferlist& bl) override;

  epoch_t blacklist(const entity_addr_t& a, utime_t until);

  void dump_info(Formatter *f);
  int dump_osd_metadata(int osd, Formatter *f, ostream *err);
  void print_nodes(Formatter *f);

  void check_osdmap_sub(Subscription *sub);
  void check_pg_creates_sub(Subscription *sub);

  void do_application_enable(int64_t pool_id, const std::string &app_name);

  void add_flag(int flag) {
    if (!(osdmap.flags & flag)) {
      if (pending_inc.new_flags < 0)
	pending_inc.new_flags = osdmap.flags;
      pending_inc.new_flags |= flag;
    }
  }

  void remove_flag(int flag) {
    if(osdmap.flags & flag) {
      if (pending_inc.new_flags < 0)
	pending_inc.new_flags = osdmap.flags;
      pending_inc.new_flags &= ~flag;
    }
  }
};

#endif
