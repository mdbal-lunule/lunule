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

#include "include/compat.h"
#include "mdstypes.h"

#include "MDBalancer.h"
#include "MDSRank.h"
#include "mon/MonClient.h"
#include "MDSMap.h"
#include "CInode.h"
#include "CDir.h"
#include "MDCache.h"
#include "Migrator.h"
#include "Mantle.h"
#include "Server.h"

#include "include/Context.h"
#include "msg/Messenger.h"
#include "messages/MHeartbeat.h"
#include "messages/MIFBeat.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <map>
#include <functional>

using std::map;
using std::vector;

#include "common/config.h"
#include "common/errno.h"

#include "adsl/PathUtil.h"

//#define MDS_MONITOR
#include <unistd.h>
#define MDS_COLDFIRST_BALANCER

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << mds->get_nodeid() << ".bal "
#undef dout
#define dout(lvl) \
  do {\
    auto subsys = ceph_subsys_mds;\
    if ((dout_context)->_conf->subsys.should_gather(ceph_subsys_mds_balancer, lvl)) {\
      subsys = ceph_subsys_mds_balancer;\
    }\
    dout_impl(dout_context, subsys, lvl) dout_prefix
#undef dendl
#define dendl dendl_impl; } while (0)

#define COLDSTART_MIGCOUNT 1000

#define MIN_LOAD    50   //  ??
#define MIN_REEXPORT 5  // will automatically reexport
#define MIN_OFFLOAD 0.1   // point at which i stop trying, close enough

#define COLDFIRST_DEPTH 2
#define MAX_EXPORT_SIZE 40

/* This function DOES put the passed message before returning */

#define LUNULE_DEBUG_LEVEL 0

int MDBalancer::proc_message(Message *m)
{
  switch (m->get_type()) {

  case MSG_MDS_HEARTBEAT:
    handle_heartbeat(static_cast<MHeartbeat*>(m));
    break;
  case MSG_MDS_IFBEAT:
    handle_ifbeat(static_cast<MIFBeat*>(m));
    break;
    
  default:
    derr << " balancer unknown message " << m->get_type() << dendl_impl;
    assert(0 == "balancer unknown message");
  }

  return 0;
}

void MDBalancer::handle_export_pins(void)
{
  auto &q = mds->mdcache->export_pin_queue;
  auto it = q.begin();
  dout(20) << "export_pin_queue size=" << q.size() << dendl;
  while (it != q.end()) {
    auto cur = it++;
    CInode *in = *cur;
    assert(in->is_dir());
    mds_rank_t export_pin = in->get_export_pin(false);

    bool remove = true;
    list<CDir*> dfls;
    in->get_dirfrags(dfls);
    for (auto dir : dfls) {
      if (!dir->is_auth())
	continue;

      if (export_pin == MDS_RANK_NONE) {
	if (dir->state_test(CDir::STATE_AUXSUBTREE)) {
	  if (dir->is_frozen() || dir->is_freezing()) {
	    // try again later
	    remove = false;
	    continue;
	  }
	  dout(10) << " clear auxsubtree on " << *dir << dendl;
	  dir->state_clear(CDir::STATE_AUXSUBTREE);
	  mds->mdcache->try_subtree_merge(dir);
	}
      } else if (export_pin == mds->get_nodeid()) {
	if (dir->state_test(CDir::STATE_CREATING) ||
	    dir->is_frozen() || dir->is_freezing()) {
	  // try again later
	  remove = false;
	  continue;
	}
	if (!dir->is_subtree_root()) {
	  dir->state_set(CDir::STATE_AUXSUBTREE);
	  mds->mdcache->adjust_subtree_auth(dir, mds->get_nodeid());
	  dout(10) << " create aux subtree on " << *dir << dendl;
	} else if (!dir->state_test(CDir::STATE_AUXSUBTREE)) {
	  dout(10) << " set auxsubtree bit on " << *dir << dendl;
	  dir->state_set(CDir::STATE_AUXSUBTREE);
	}
      } else {
	mds->mdcache->migrator->export_dir(dir, export_pin);
	remove = false;
      }
    }

    if (remove) {
      in->state_clear(CInode::STATE_QUEUEDEXPORTPIN);
      q.erase(cur);
    }
  }

  set<CDir *> authsubs;
  mds->mdcache->get_auth_subtrees(authsubs);
  for (auto &cd : authsubs) {
    mds_rank_t export_pin = cd->inode->get_export_pin();
    dout(10) << "auth tree " << *cd << " export_pin=" << export_pin << dendl;
    if (export_pin >= 0 && export_pin != mds->get_nodeid()) {
      dout(10) << "exporting auth subtree " << *cd->inode << " to " << export_pin << dendl;
      mds->mdcache->migrator->export_dir(cd, export_pin);
    }
  }
}

void MDBalancer::tick()
{
  static int num_bal_times = g_conf->mds_bal_max;
  static utime_t first = ceph_clock_now();
  utime_t now = ceph_clock_now();
  utime_t elapsed = now;
  elapsed -= first;

  if (g_conf->mds_bal_export_pin) {
    handle_export_pins();
  }

  // sample?
  if ((double)now - (double)last_sample > g_conf->mds_bal_sample_interval) {
    dout(15) << "tick last_sample now " << now << dendl;
    last_sample = now;
  }

  // balance?
  if (last_heartbeat == utime_t())
    last_heartbeat = now;
  if (mds->get_nodeid() == 0 &&
      g_conf->mds_bal_interval > 0 &&
      (num_bal_times ||
       (g_conf->mds_bal_max_until >= 0 &&
	elapsed.sec() > g_conf->mds_bal_max_until)) &&
      mds->is_active() &&
      now.sec() - last_heartbeat.sec() >= g_conf->mds_bal_interval) {
    last_heartbeat = now;
    send_heartbeat();
    
    //MDS0 will not send empty IF
    //send_ifbeat();
    
    num_bal_times--;
  }
}

class C_Bal_SendIFbeat : public MDSInternalContext {
  mds_rank_t target;
  double if_beate_value;
  vector<migration_decision_t> my_decision;
public:
  explicit C_Bal_SendIFbeat(MDSRank *mds_, mds_rank_t target, double if_beate_value, vector<migration_decision_t> migration_decision) : MDSInternalContext(mds_) 
  {
    this->target = target;
    this->if_beate_value = if_beate_value;
    this->my_decision.assign(migration_decision.begin(),migration_decision.end());
  }
  void finish(int f) override {
    mds->balancer->send_ifbeat(target,if_beate_value,my_decision);
  }
};

class C_Bal_SendHeartbeat : public MDSInternalContext {
public:
  explicit C_Bal_SendHeartbeat(MDSRank *mds_) : MDSInternalContext(mds_) { }
  void finish(int f) override {
    mds->balancer->send_heartbeat();
  }
};


double mds_load_t::mds_pop_load()
{
  switch(g_conf->mds_bal_mode) {
  case 0:
    return
      .8 * auth.meta_load() +
      .2 * all.meta_load() +
      req_rate +
      10.0 * queue_len;

  case 1:
    return req_rate + 10.0*queue_len;

  case 2:
    return cpu_load_avg;

  }
  ceph_abort();
  return 0;
}

double mds_load_t::mds_pot_load(bool auth, int epoch)
{
  if (auth) return pot_auth.pot_load(epoch);

  return
    .8 * pot_auth.pot_load(epoch) +
    .2 * pot_all.pot_load(epoch);
}

double mds_load_t::mds_load(double alpha, double beta, int epoch, bool is_auth, MDBalancer * bal)
{
  if (is_auth)
    //return alpha * auth.meta_load(bal->rebalance_time, bal->mds->mdcache->decayrate) + beta * pot_auth.pot_load(epoch);
    return alpha * auth.meta_load(bal->rebalance_time, bal->mds->mdcache->decayrate) + beta * pot_all.pot_load(epoch);
  else
    return alpha * mds_pop_load() + beta * mds_pot_load(epoch);
}

mds_load_t MDBalancer::get_load(utime_t now)
{
  mds_load_t load(now);

  if (mds->mdcache->get_root()) {
    list<CDir*> ls;
    mds->mdcache->get_root()->get_dirfrags(ls);
    for (list<CDir*>::iterator p = ls.begin();
	 p != ls.end();
	 ++p) {
      load.auth.add(now, mds->mdcache->decayrate, (*p)->pop_auth_subtree_nested);
      load.all.add(now, mds->mdcache->decayrate, (*p)->pop_nested);
      load.pot_auth.add((*p)->pot_auth);
      load.pot_all.add((*p)->pot_all);
    }
  } else {
    dout(20) << "get_load no root, no load" << dendl;
  }

  load.req_rate = mds->get_req_rate();
  load.queue_len = messenger->get_dispatch_queue_len();

  ifstream cpu(PROCPREFIX "/proc/loadavg");
  if (cpu.is_open())
    cpu >> load.cpu_load_avg;
  else
    derr << "input file " PROCPREFIX "'/proc/loadavg' not found" << dendl_impl;
  
  dout(15) << "get_load " << load << dendl;
  return load;
}

/*
 * Read synchronously from RADOS using a timeout. We cannot do daemon-local
 * fallbacks (i.e. kick off async read when we are processing the map and
 * check status when we get here) with the way the mds is structured.
 */
int MDBalancer::localize_balancer()
{
  /* reset everything */
  bool ack = false;
  int r = 0;
  bufferlist lua_src;
  Mutex lock("lock");
  Cond cond;

  /* we assume that balancer is in the metadata pool */
  object_t oid = object_t(mds->mdsmap->get_balancer());
  object_locator_t oloc(mds->mdsmap->get_metadata_pool());
  ceph_tid_t tid = mds->objecter->read(oid, oloc, 0, 0, CEPH_NOSNAP, &lua_src, 0,
                                       new C_SafeCond(&lock, &cond, &ack, &r));
  dout(15) << "launched non-blocking read tid=" << tid
           << " oid=" << oid << " oloc=" << oloc << dendl;

  /* timeout: if we waste half our time waiting for RADOS, then abort! */
  double t = ceph_clock_now() + g_conf->mds_bal_interval/2;
  utime_t timeout;
  timeout.set_from_double(t);
  lock.Lock();
  int ret_t = cond.WaitUntil(lock, timeout);
  lock.Unlock();

  /* success: store the balancer in memory and set the version. */
  if (!r) {
    if (ret_t == ETIMEDOUT) {
      mds->objecter->op_cancel(tid, -ECANCELED);
      return -ETIMEDOUT;
    }
    bal_code.assign(lua_src.to_str());
    bal_version.assign(oid.name);
    dout(10) << "localized balancer, bal_code=" << bal_code << dendl;
  }
  return r;
}

void MDBalancer::send_ifbeat(mds_rank_t target, double if_beate_value, vector<migration_decision_t>& migration_decision){
  utime_t now = ceph_clock_now();
  mds_rank_t whoami = mds->get_nodeid();
  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (0) Prepare to send ifbeat: " << if_beate_value << " from " << whoami << " to " << target << dendl;
  if (mds->is_cluster_degraded()) {
    dout(10) << "send_ifbeat degraded" << dendl;
    return;
  }

  if (!mds->mdcache->is_open()) {
    dout(5) << "not open" << dendl;
    vector<migration_decision_t> &waited_decision(migration_decision);
    //vector<migration_decision_t> &waited_decision1 = migration_decision;
    mds->mdcache->wait_for_open(new C_Bal_SendIFbeat(mds,target,if_beate_value, waited_decision));
    return;
  }

  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (1) OK, could send ifbeat" << dendl;
  
  set<mds_rank_t> up;
  mds->get_mds_map()->get_up_mds_set(up);
  
  //myload
  mds_load_t load = get_load(now);
  set<mds_rank_t>::iterator target_mds=up.find(target);

  if(target_mds==up.end()){
  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (1.1) ERR: Can't find MDS"<< *target_mds << dendl;
  return;
  }

  MIFBeat *ifm = new MIFBeat(load, beat_epoch, if_beate_value, migration_decision);
  messenger->send_message(ifm,
                            mds->mdsmap->get_inst(*target_mds));
}

void MDBalancer::send_heartbeat()
{
  utime_t now = ceph_clock_now();
  
  if (mds->is_cluster_degraded()) {
    dout(10) << "send_heartbeat degraded" << dendl;
    return;
  }

  if (!mds->mdcache->is_open()) {
    dout(5) << "not open" << dendl;
    mds->mdcache->wait_for_open(new C_Bal_SendHeartbeat(mds));
    return;
  }

  map<mds_rank_t, mds_load_t>::iterator it = mds_load.begin();
  #ifdef MDS_MONITOR
  while(it != mds_load.end()){
    dout(7) << " MDS_MONITOR " << __func__ << " (1) before send hearbeat, retain mds_load <" << it->first << "," << it->second << ">" << dendl;
    it++; 
  }
  #endif

  if (mds->get_nodeid() == 0) {
    beat_epoch++;

    req_tracer.switch_epoch();
   
    mds_load.clear();
  }

  // my load
  mds_load_t load = get_load(now);
  map<mds_rank_t, mds_load_t>::value_type val(mds->get_nodeid(), load);
  mds_load.insert(val);

  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (2) count my load, MDS." << mds->get_nodeid() << " Load " << load << dendl;
  #endif

  // import_map -- how much do i import from whom

  map<mds_rank_t, float> import_map;
  set<CDir*> authsubs;
  mds->mdcache->get_auth_subtrees(authsubs);
  for (set<CDir*>::iterator it = authsubs.begin();
       it != authsubs.end();
       ++it) {
    CDir *im = *it;
    mds_rank_t from = im->inode->authority().first;
    if (from == mds->get_nodeid()) continue;
    if (im->get_inode()->is_stray()) continue;
    import_map[from] += im->get_load(this);
  }
  mds_import_map[ mds->get_nodeid() ] = import_map;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (3) count imported directory meta_load" << dendl;
  map<mds_rank_t, float>::iterator it_import = import_map.begin();
  while(it_import != import_map.end()){
    dout(7) << " MDS_MONITOR " << __func__ << " (3) from mds " << it_import->first << " meta_load " << it_import->second << dendl;
    it_import++;
  }
  #endif

  // dout(5) << "mds." << mds->get_nodeid() << " epoch " << beat_epoch << " load " << load << dendl;
  // for (map<mds_rank_t, float>::iterator it = import_map.begin();
  //      it != import_map.end();
  //      ++it) {
  //   dout(5) << "  import_map from " << it->first << " -> " << it->second << dendl;
  // }


  set<mds_rank_t> up;
  mds->get_mds_map()->get_up_mds_set(up);
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (4) collect up mds" << dendl;
  set<mds_rank_t>::iterator it_up = up.begin();
  while( it_up != up.end()){
    dout(7) << " MDS_MONITOR " << __func__ << " (4) up mds." << *it_up << dendl;
    it_up++;
  } 
  #endif
  for (set<mds_rank_t>::iterator p = up.begin(); p != up.end(); ++p) {
    if (*p == mds->get_nodeid())
      continue;
    MHeartbeat *hb = new MHeartbeat(load, beat_epoch);
    hb->get_import_map() = import_map;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (5) send heartbeat to mds." << *p << dendl;
    #endif
    messenger->send_message(hb, mds->mdsmap->get_inst(*p));
  }
}

//handle imbalancer factor message
void MDBalancer::handle_ifbeat(MIFBeat *m){
  mds_rank_t who = mds_rank_t(m->get_source().num());
  mds_rank_t whoami = mds->get_nodeid();
  double simple_migration_amount = 0.1;
  double simple_if_threshold = g_conf->mds_bal_ifthreshold;

  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (1) get ifbeat " << m->get_beat() << " from " << who << " to " << whoami << " load: " << m->get_load() << " IF: " << m->get_IFvaule() << dendl;

  if (!mds->is_active())
    goto out;

  if (!mds->mdcache->is_open()) {
    dout(10) << "opening root on handle_ifbeat" << dendl;
    mds->mdcache->wait_for_open(new C_MDS_RetryMessage(mds, m));
    return;
  }

  if (mds->is_cluster_degraded()) {
    dout(10) << " degraded, ignoring" << dendl;
    goto out;
  }

  if(whoami == 0){
    // mds0 is responsible for calculating IF
    if(m->get_beat()!=beat_epoch){
    dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " ifbeat with wrong epoch: " << m->get_beat() << " from " << who << " to " << whoami << dendl;
      return;
    }else{
      // set mds_load[who]
      
      typedef map<mds_rank_t, mds_load_t> mds_load_map_t;
      mds_load_map_t::value_type val(who, m->get_load());
      mds_load.insert(val);
    }

    if(mds->get_mds_map()->get_num_in_mds()==mds_load.size()){
      //calculate IF
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2)  ifbeat: Try to calculate IF " << dendl;
      unsigned cluster_size = mds->get_mds_map()->get_num_in_mds();
      
      
      //vector < map<string, double> > metrics (cluster_size);
      vector <double> IOPSvector(cluster_size);
      vector <double> load_vector(cluster_size);
      for (mds_rank_t i=mds_rank_t(0);
       i < mds_rank_t(cluster_size);
       i++) {
        map<mds_rank_t, mds_load_t>::iterator it = mds_load.find(i);
        if(it==mds_load.end()){
          derr << " cant find target load of MDS." << i << dendl_impl;
          assert(0 == " cant find target load of MDS.");
        }

        if(old_req.find(i)!=old_req.end()){
          IOPSvector[i] = (it->second.req_rate - old_req[i])/g_conf->mds_bal_interval;
        }else{
          //MDS just started, so skip this time
          IOPSvector[i] = 0;
        }
        old_req[i] = it->second.req_rate;
        
        load_vector[i] = calc_mds_load(it->second, true);
        //load_vector[i] = it->second.auth.meta_load();
        /* mds_load_t &load(it->second);
        no need to get all info?
        metrics[i] = {{"auth.meta_load", load.auth.meta_load()},
                      {"all.meta_load", load.all.meta_load()},
                      {"req_rate", load.req_rate},
                      {"queue_len", load.queue_len},
                      {"cpu_load_avg", load.cpu_load_avg}};
        IOPSvector[i] = load.auth.meta_load();*/
        }

      //ok I know all IOPS, know get to calculateIF
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2) get IOPS: " << IOPSvector << " load: "<< load_vector << dendl;
      
      double avg_load = std::accumulate(std::begin(load_vector), std::end(load_vector), 0.0)/load_vector.size(); 
      double avg_IOPS = std::accumulate(std::begin(IOPSvector), std::end(IOPSvector), 0.0)/IOPSvector.size(); 
      double max_IOPS = *max_element(IOPSvector.begin(), IOPSvector.end());
      double sum_quadratic = 0.0;
      unsigned max_exporter_count = 5;
      double my_if_threshold = simple_if_threshold/min(cluster_size,max_exporter_count);
      int importer_count = 0;

      mds_rank_t max_pos = mds_rank_t(IOPSvector.begin() - max_element(IOPSvector.begin(), IOPSvector.end()));
      mds_rank_t min_pos = mds_rank_t(IOPSvector.begin() - min_element(IOPSvector.begin(), IOPSvector.end()));
      mds_rank_t temp_pos=mds_rank_t(0);
      vector<imbalance_summary_t> my_imbalance_vector(cluster_size);
      vector<imbalance_summary_t>::iterator my_if_it = my_imbalance_vector.begin();


      std::for_each (std::begin(IOPSvector), std::end(IOPSvector), [&](const double my_IOPS) {  
        sum_quadratic  += (my_IOPS-avg_IOPS)*(my_IOPS-avg_IOPS);  

        (*my_if_it).my_if = sqrt((my_IOPS-avg_IOPS)*(my_IOPS-avg_IOPS)/(cluster_size-1)) /(sqrt(cluster_size)*avg_IOPS);
        (*my_if_it).my_urgency = 1/(1+pow(exp(1), 5-10*(my_IOPS/g_conf->mds_bal_presetmax)));
        (*my_if_it).my_iops = my_IOPS ? my_IOPS :1;
        (*my_if_it).whoami = temp_pos;
        if(my_IOPS>avg_IOPS){
          (*my_if_it).is_bigger = true;
        }else{
          (*my_if_it).is_bigger = false;
          if((*my_if_it).my_if >= my_if_threshold){
            importer_count++;
          }
        }
        temp_pos++;
        my_if_it++;
      });
      
      importer_count = max(importer_count, 1);
      importer_count = min(importer_count, 5);

      //simple_migration_amount = simple_migration_total_amount / importer_count;
      simple_migration_amount = 0.1;

      double stdev_IOPS = sqrt(sum_quadratic/(IOPSvector.size()-1));
      double imbalance_degree = 0.0;
      
      double urgency = 1/(1+pow(exp(1), 5-10*(max_IOPS/g_conf->mds_bal_presetmax)));

      if(sqrt(IOPSvector.size())*avg_IOPS == 0){
        imbalance_degree = 0.0;
      }else{
        imbalance_degree = stdev_IOPS/(sqrt(IOPSvector.size())*avg_IOPS);
      }

      double imbalance_factor = imbalance_degree*urgency;
      
      //dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.1) avg_IOPS: " << avg_IOPS << " max_IOPS: " << max_IOPS << " stdev_IOPS: " << stdev_IOPS << " imbalance_degree: " << imbalance_degree << " presetmax: " << g_conf->mds_bal_presetmax << dendl;

      

      if(imbalance_factor>=simple_if_threshold){
        dout(0) << " MDS_IFBEAT " << __func__ << " (2.1) imbalance_factor is high enough: " << imbalance_factor << " imbalance_degree: " << imbalance_degree << " urgency: " << urgency << " start to send " << dendl;
        
        set<mds_rank_t> up;
        mds->get_mds_map()->get_up_mds_set(up);
        for (set<mds_rank_t>::iterator p = up.begin(); p != up.end(); ++p) {
	  if (*p == 0)continue;
        dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2.01) I'm: " <<  my_imbalance_vector[*p].whoami << " " <<  my_imbalance_vector[*p].my_if << " " << my_imbalance_vector[*p].is_bigger << dendl;
          vector<migration_decision_t> mds_decision;
          //std::sort (my_imbalance_vector.begin(), my_imbalance_vector.end(), sortImporter);
          if((max_pos == my_imbalance_vector[*p].whoami || my_imbalance_vector[*p].my_if>my_if_threshold) && my_imbalance_vector[*p].is_bigger){
          int max_importer_count = 0;
            for (vector<imbalance_summary_t>::iterator my_im_it = my_imbalance_vector.begin();my_im_it!=my_imbalance_vector.end() && (max_importer_count < max_exporter_count);my_im_it++){
              dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2.011), try match" << *p << ": " << " with " << (*my_im_it).whoami << " " <<  (*my_im_it).my_if << " " << (*my_im_it).is_bigger << dendl;
            if((*my_im_it).whoami != *p &&(*my_im_it).is_bigger == false && ((*my_im_it).my_if >=my_if_threshold  || (*my_im_it).whoami == min_pos )){
              migration_decision_t temp_decision = {(*my_im_it).whoami,static_cast<float>(simple_migration_amount*load_vector[*p]),static_cast<float>(simple_migration_amount*((my_imbalance_vector[*p].my_iops-(*my_im_it).my_iops)/my_imbalance_vector[*p].my_iops))};
              mds_decision.push_back(temp_decision);
              max_importer_count ++;
              dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2.1) decision: " << temp_decision.target_import_mds << " " << temp_decision.target_export_load  << temp_decision.target_export_percent<< dendl;
            }
          }
          send_ifbeat(*p, imbalance_factor, mds_decision);
          }
        }

        if( (max_pos == my_imbalance_vector[0].whoami || my_imbalance_vector[0].my_if>my_if_threshold) && my_imbalance_vector[0].is_bigger){
          vector<migration_decision_t> my_decision;
          int max_importer_count = 0;
          for (vector<imbalance_summary_t>::iterator my_im_it = my_imbalance_vector.begin();my_im_it!=my_imbalance_vector.end() && (max_importer_count < max_exporter_count);my_im_it++){
            if((*my_im_it).whoami != whoami &&(*my_im_it).is_bigger == false && ((*my_im_it).my_if >=(my_if_threshold) || (*my_im_it).whoami == min_pos )){
              //migration_decision_t temp_decision = {(*my_im_it).whoami,static_cast<float>(simple_migration_amount*load_vector[0])};
              dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2.011), try match" << 0 << ": " << " with " << (*my_im_it).whoami << " " <<  (*my_im_it).my_if << " " << (*my_im_it).is_bigger << dendl;
              migration_decision_t temp_decision = {(*my_im_it).whoami,static_cast<float>((load_vector[0]-avg_load)/importer_count),static_cast<float>(simple_migration_amount*(my_imbalance_vector[0].my_iops-(*my_im_it).my_iops)/my_imbalance_vector[0].my_iops)};
              my_decision.push_back(temp_decision);
              max_importer_count ++;
              dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2.2) decision of mds0: " << temp_decision.target_import_mds << " " << temp_decision.target_export_load  << temp_decision.target_export_percent<< dendl;
            }
          }
          simple_determine_rebalance(my_decision);
          
          if(urgency<=0.1){
            dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << "wird bug, dont clear" <<dendl;
          }else{
            dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << "new epoch, clear_export_queue" <<dendl;
            mds->mdcache->migrator->clear_export_queue();  
          }
        }
      }else{
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.2) imbalance_factor is low: " << imbalance_factor << " imbalance_degree: " << imbalance_degree << " urgency: " << urgency << dendl;
      //mds->mdcache->migrator->clear_export_queue();
      }
    }else{
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (3)  ifbeat: No enough MDSload to calculate IF, skip " << dendl;
    }
  }else{
    double get_if_value = m->get_IFvaule();
    if(get_if_value>=simple_if_threshold){
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (3.1) Imbalance Factor is high enough: " << m->get_IFvaule() << dendl;
      simple_determine_rebalance(m->get_decision());
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << "new epoch, clear_export_queue" <<dendl;
      mds->mdcache->migrator->clear_export_queue();  
    }else{
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (3.1) Imbalance Factor is low: " << m->get_IFvaule() << dendl;
      //mds->mdcache->migrator->clear_export_queue();
    }
  }

  // done
 out:
  m->put();
}

/* This function DOES put the passed message before returning */
void MDBalancer::handle_heartbeat(MHeartbeat *m)
{
  typedef map<mds_rank_t, mds_load_t> mds_load_map_t;

  mds_rank_t who = mds_rank_t(m->get_source().num());
  dout(25) << "=== got heartbeat " << m->get_beat() << " from " << m->get_source().num() << " " << m->get_load() << dendl;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (1) get heartbeat " << m->get_beat() << " from " << who << " load " << m->get_load() << dendl;
  #endif

  if (!mds->is_active())
    goto out;

  if (!mds->mdcache->is_open()) {
    dout(10) << "opening root on handle_heartbeat" << dendl;
    mds->mdcache->wait_for_open(new C_MDS_RetryMessage(mds, m));
    return;
  }

  if (mds->is_cluster_degraded()) {
    dout(10) << " degraded, ignoring" << dendl;
    goto out;
  }

  if (mds->get_nodeid() != 0 && m->get_beat() > beat_epoch) {
    dout(10) << "receive next epoch " << m->get_beat() << " from mds." << who << " before mds0" << dendl;

    beat_epoch = m->get_beat();
    // clear the mds load info whose epoch is less than beat_epoch 
    mds_load.clear();
  }

  if (who == 0) {
    dout(20) << " from mds0, new epoch " << m->get_beat() << dendl;
    if (beat_epoch != m->get_beat()) {
      mds_load.clear();
    }
    beat_epoch = m->get_beat();
    send_heartbeat();

    //req_tracer.switch_epoch();

    vector<migration_decision_t> empty_decision;
    send_ifbeat(0, -1, empty_decision);

    mds->mdcache->show_subtrees();
  }

  {
    // set mds_load[who]
    mds_load_map_t::value_type val(who, m->get_load());
    pair < mds_load_map_t::iterator, bool > rval (mds_load.insert(val));
    if (!rval.second) {
      rval.first->second = val.second;
    }
  }
  mds_import_map[ who ] = m->get_import_map();

  //if imbalance factor is enabled, won't use old migration
  
  if(g_conf->mds_bal_ifenable == 0){
    unsigned cluster_size = mds->get_mds_map()->get_num_in_mds();
    if (mds_load.size() == cluster_size) {
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (2) receive all mds heartbeats, now start balance" << dendl;
      #endif
      // let's go!
      //export_empties();  // no!

      /* avoid spamming ceph -w if user does not turn mantle on */
      if (mds->mdsmap->get_balancer() != "") {
        int r = mantle_prep_rebalance();
        if (!r) goto out;
	mds->clog->warn() << "using old balancer; mantle failed for "
                          << "balancer=" << mds->mdsmap->get_balancer()
                          << " : " << cpp_strerror(r);
      }
      prep_rebalance(m->get_beat());
    }
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (2) waiting other heartbeats..." << dendl;
    #endif
  }else{
    //use new migration
  }

  // done
 out:
  m->put();
}


void MDBalancer::export_empties()
{
  dout(5) << "export_empties checking for empty imports" << dendl;

  std::set<CDir *> subtrees;
  mds->mdcache->get_fullauth_subtrees(subtrees);
  for (auto &dir : subtrees) {
    if (dir->is_freezing() || dir->is_frozen())
      continue;

    if (!dir->inode->is_base() &&
	!dir->inode->is_stray() &&
	dir->get_num_head_items() == 0)
      mds->mdcache->migrator->export_empty_import(dir);
  }
}



double MDBalancer::try_match(balance_state_t& state, mds_rank_t ex, double& maxex,
                             mds_rank_t im, double& maxim)
{
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " Try Match BEGIN " << dendl;
  #endif
  if (maxex <= 0 || maxim <= 0) return 0.0;

  double howmuch = MIN(maxex, maxim);
  if (howmuch <= 0) return 0.0;

  dout(5) << "   - mds." << ex << " exports " << howmuch << " to mds." << im << dendl;

  if (ex == mds->get_nodeid())
    state.targets[im] += howmuch;

  state.exported[ex] += howmuch;
  state.imported[im] += howmuch;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (1) howmuch matched : "<< howmuch << dendl;
  #endif
  maxex -= howmuch;
  maxim -= howmuch;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " Try Match END " << dendl;
  #endif
  return howmuch;
}

void MDBalancer::queue_split(const CDir *dir, bool fast)
{
  dout(10) << __func__ << " enqueuing " << *dir
                       << " (fast=" << fast << ")" << dendl;

  assert(mds->mdsmap->allows_dirfrags());
  const dirfrag_t frag = dir->dirfrag();

  auto callback = [this, frag](int r) {
    if (split_pending.erase(frag) == 0) {
      // Someone beat me to it.  This can happen in the fast splitting
      // path, because we spawn two contexts, one with mds->timer and
      // one with mds->queue_waiter.  The loser can safely just drop
      // out.
      return;
    }

    CDir *split_dir = mds->mdcache->get_dirfrag(frag);
    if (!split_dir) {
      dout(10) << "drop split on " << frag << " because not in cache" << dendl;
      return;
    }
    if (!split_dir->is_auth()) {
      dout(10) << "drop split on " << frag << " because non-auth" << dendl;
      return;
    }

    // Pass on to MDCache: note that the split might still not
    // happen if the checks in MDCache::can_fragment fail.
    dout(10) << __func__ << " splitting " << *split_dir << dendl;
    mds->mdcache->split_dir(split_dir, g_conf->mds_bal_split_bits);
  };

  bool is_new = false;
  if (split_pending.count(frag) == 0) {
    split_pending.insert(frag);
    is_new = true;
  }

  if (fast) {
    // Do the split ASAP: enqueue it in the MDSRank waiters which are
    // run at the end of dispatching the current request
    mds->queue_waiter(new MDSInternalContextWrapper(mds, 
          new FunctionContext(callback)));
  } else if (is_new) {
    // Set a timer to really do the split: we don't do it immediately
    // so that bursts of ops on a directory have a chance to go through
    // before we freeze it.
    mds->timer.add_event_after(g_conf->mds_bal_fragment_interval,
                               new FunctionContext(callback));
  }
}

void MDBalancer::queue_merge(CDir *dir)
{
  const auto frag = dir->dirfrag();
  auto callback = [this, frag](int r) {
    assert(frag.frag != frag_t());

    // frag must be in this set because only one context is in flight
    // for a given frag at a time (because merge_pending is checked before
    // starting one), and this context is the only one that erases it.
    merge_pending.erase(frag);

    CDir *dir = mds->mdcache->get_dirfrag(frag);
    if (!dir) {
      dout(10) << "drop merge on " << frag << " because not in cache" << dendl;
      return;
    }
    assert(dir->dirfrag() == frag);

    if(!dir->is_auth()) {
      dout(10) << "drop merge on " << *dir << " because lost auth" << dendl;
      return;
    }

    dout(10) << "merging " << *dir << dendl;

    CInode *diri = dir->get_inode();

    frag_t fg = dir->get_frag();
    while (fg != frag_t()) {
      frag_t sibfg = fg.get_sibling();
      list<CDir*> sibs;
      bool complete = diri->get_dirfrags_under(sibfg, sibs);
      if (!complete) {
        dout(10) << "  not all sibs under " << sibfg << " in cache (have " << sibs << ")" << dendl;
        break;
      }
      bool all = true;
      for (list<CDir*>::iterator p = sibs.begin(); p != sibs.end(); ++p) {
        CDir *sib = *p;
        if (!sib->is_auth() || !sib->should_merge()) {
          all = false;
          break;
        }
      }
      if (!all) {
        dout(10) << "  not all sibs under " << sibfg << " " << sibs << " should_merge" << dendl;
        break;
      }
      dout(10) << "  all sibs under " << sibfg << " " << sibs << " should merge" << dendl;
      fg = fg.parent();
    }

    if (fg != dir->get_frag())
      mds->mdcache->merge_dir(diri, fg);
  };

  if (merge_pending.count(frag) == 0) {
    dout(20) << __func__ << " enqueued dir " << *dir << dendl;
    merge_pending.insert(frag);
    mds->timer.add_event_after(g_conf->mds_bal_fragment_interval,
        new FunctionContext(callback));
  } else {
    dout(20) << __func__ << " dir already in queue " << *dir << dendl;
  }
}

void MDBalancer::prep_rebalance(int beat)
{
  balance_state_t state;

  if (g_conf->mds_thrash_exports) {
    //we're going to randomly export to all the mds in the cluster
    set<mds_rank_t> up_mds;
    mds->get_mds_map()->get_up_mds_set(up_mds);
    for (const auto &rank : up_mds) {
      state.targets[rank] = 0.0;
    }
  } else {
    int cluster_size = mds->get_mds_map()->get_num_in_mds();
    mds_rank_t whoami = mds->get_nodeid();
    rebalance_time = ceph_clock_now();

    dout(5) << " prep_rebalance: cluster loads are" << dendl;

    mds->mdcache->migrator->clear_export_queue();

    // rescale!  turn my mds_load back into meta_load units
    double load_fac = 1.0;
    map<mds_rank_t, mds_load_t>::iterator m = mds_load.find(whoami);
    if ((m != mds_load.end()) && (calc_mds_load(m->second) > 0)) {
      double metald = calc_mds_load(m->second, true);
      double mdsld = calc_mds_load(m->second);
      load_fac = metald / mdsld;
      dout(7) << " load_fac is " << load_fac
	      << " <- " << m->second.auth << " " << metald
	      << " / " << mdsld
	      << dendl;
        #ifdef MDS_MONITOR
        dout(7) << " MDS_MONITOR " << __func__ << " (1) calculate my load factor meta_load " << metald << " mds_load " << mdsld << " load_factor " << load_fac << dendl;
        #endif
    }

    double total_load = 0.0;
    multimap<double,mds_rank_t> load_map;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (2) compute mds cluter load" << dendl;
    #endif
    for (mds_rank_t i=mds_rank_t(0); i < mds_rank_t(cluster_size); i++) {
      map<mds_rank_t, mds_load_t>::value_type val(i, mds_load_t(ceph_clock_now()));
      std::pair < map<mds_rank_t, mds_load_t>::iterator, bool > r(mds_load.insert(val));
      mds_load_t &load(r.first->second);

      double l = calc_mds_load(load) * load_fac;
      mds_meta_load[i] = l;
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (2) mds." << i << " load " << l << dendl;
      #endif
      if (whoami == 0)
	dout(5) << "  mds." << i
		<< " " << load
		<< " = " << calc_mds_load(load)
		<< " ~ " << l << dendl;

      if (whoami == i) my_load = l;
      total_load += l;

      load_map.insert(pair<double,mds_rank_t>( l, i ));
    }

    // target load
    target_load = total_load / (double)cluster_size;
    dout(5) << "prep_rebalance:  my load " << my_load
	    << "   target " << target_load
	    << "   total " << total_load
	    << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (3) total_load " << total_load << " cluster_size " << cluster_size << " target_load " << target_load << dendl;
    #endif

    // under or over?
    if (my_load < target_load * (1.0 + g_conf->mds_bal_min_rebalance)) {
      dout(5) << "  i am underloaded or barely overloaded, doing nothing." << dendl;
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (3) my_load is small, so doing nothing (" << my_load << " < " << target_load << " * " <<g_conf->mds_bal_min_rebalance << ")" << dendl;
      #endif
      last_epoch_under = beat_epoch;
      mds->mdcache->show_subtrees();
      return;
    }

    // am i over long enough?
    if (last_epoch_under && beat_epoch - last_epoch_under < 2) {
      dout(5) << "  i am overloaded, but only for " << (beat_epoch - last_epoch_under) << " epochs" << dendl;
      return;
    }

    dout(5) << "  i am sufficiently overloaded" << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (4) I am overloaded!!! Now need to migrate." << dendl;
    dout(7) << " MDS_MONITOR " << __func__ << " (4) decide importer and exporter!" << dendl;
    #endif


    // first separate exporters and importers
    multimap<double,mds_rank_t> importers;
    multimap<double,mds_rank_t> exporters;
    set<mds_rank_t>             importer_set;
    set<mds_rank_t>             exporter_set;

    for (multimap<double,mds_rank_t>::iterator it = load_map.begin();
	 it != load_map.end();
	 ++it) {
      if (it->first < target_load) {
	dout(15) << "   mds." << it->second << " is importer" << dendl;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (4) importer - mds." << it->second << " load " << it->first << " < " << target_load << "(target_load)" <<dendl;
  #endif
	importers.insert(pair<double,mds_rank_t>(it->first,it->second));
	importer_set.insert(it->second);
      } else {
	dout(15) << "   mds." << it->second << " is exporter" << dendl;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (4) exporter - mds." << it->second << " load " << it->first << " >= " << target_load << "(target_load)" <<dendl;
  #endif
	exporters.insert(pair<double,mds_rank_t>(it->first,it->second));
	exporter_set.insert(it->second);
      }
    }


    // determine load transfer mapping

    if (true) {
      // analyze import_map; do any matches i can
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (5) determine load transfer mapping " << dendl;
      dout(7) << " MDS_MONITOR " << __func__ << " ----------------------------------- " << dendl;
      dout(7) << " MDS_MONITOR " << __func__ << " (5) before determination, state elements " << dendl;
      map<mds_rank_t, double>::iterator it_target = state.targets.begin();
      while(it_target != state.targets.end()){
        dout(7) << " MDS_MONITOR " << __func__ << "(5) targets mds." << it_target->first << " load " << it_target->second << dendl;
        it_target++;
      }
      map<mds_rank_t, double>::iterator it_import = state.imported.begin();
      while(it_import != state.imported.end()){
        dout(7) << " MDS_MONITOR " << __func__ << "(5) imported mds." << it_import->first << " load " << it_import->second << dendl;
        it_import++;
      }
      map<mds_rank_t, double>::iterator it_export = state.exported.begin();
      while(it_export != state.exported.end()){
        dout(7) << " MDS_MONITOR " << __func__ << "(5) exported mds." << it_export->first << " load " << it_export->second << dendl;
        it_export++;
      }
      dout(7) << " MDS_MONITOR " << __func__ << " ----------------------------------- " << dendl;
      #endif
      dout(15) << "  matching exporters to import sources" << dendl;

      #ifdef MDS_MONITOR
      dout(7) << " BEGIN TO LIST EXPORTER " << dendl;
      #endif
      // big -> small exporters
      for (multimap<double,mds_rank_t>::reverse_iterator ex = exporters.rbegin();
	   ex != exporters.rend();
	   ++ex) {
	double maxex = get_maxex(state, ex->second);
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << "(5) list exporters: "<< maxex << dendl;
    #endif

	if (maxex <= .001) continue;

	// check importers. for now, just in arbitrary order (no intelligent matching).
	for (map<mds_rank_t, float>::iterator im = mds_import_map[ex->second].begin();
	     im != mds_import_map[ex->second].end();
	     ++im) {
	  double maxim = get_maxim(state, im->first);
	  if (maxim <= .001) continue;
	  try_match(state, ex->second, maxex, im->first, maxim);
	  if (maxex <= .001) break;
	}
      }
    }

    // old way
    if (beat % 2 == 1) {
      dout(15) << "  matching big exporters to big importers" << dendl;
      // big exporters to big importers
      multimap<double,mds_rank_t>::reverse_iterator ex = exporters.rbegin();
      multimap<double,mds_rank_t>::iterator im = importers.begin();
      while (ex != exporters.rend() &&
	     im != importers.end()) {
        double maxex = get_maxex(state, ex->second);
	double maxim = get_maxim(state, im->second);
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " before match: maxex: "<<maxex<<" maxim: "<<maxim << dendl;
  #endif
	if (maxex < .001 || maxim < .001) break;
	try_match(state, ex->second, maxex, im->second, maxim);
	if (maxex <= .001) ++ex;
	if (maxim <= .001) ++im;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " after match: maxex: "<<maxex<<" maxim: "<<maxim << dendl;
  #endif
      }
    } else { // new way
      dout(15) << "  matching small exporters to big importers" << dendl;
      // small exporters to big importers
      multimap<double,mds_rank_t>::iterator ex = exporters.begin();
      multimap<double,mds_rank_t>::iterator im = importers.begin();
      while (ex != exporters.end() &&
	     im != importers.end()) {
        double maxex = get_maxex(state, ex->second);
	double maxim = get_maxim(state, im->second);
	if (maxex < .001 || maxim < .001) break;
	try_match(state, ex->second, maxex, im->second, maxim);
	if (maxex <= .001) ++ex;
	if (maxim <= .001) ++im;
      }
    }
  }
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " ----------------------------------- " << dendl;
  dout(7) << " MDS_MONITOR " << __func__ << " (6) after determination, state elements " << dendl;
  map<mds_rank_t, double>::iterator it_target = state.targets.begin();
  while(it_target != state.targets.end()){
    dout(7) << " MDS_MONITOR " << __func__ << "(6) targets mds." << it_target->first << " load " << it_target->second << dendl;
    it_target++;
  }
  map<mds_rank_t, double>::iterator it_import = state.imported.begin();
  while(it_import != state.imported.end()){
    dout(7) << " MDS_MONITOR " << __func__ << "(6) imported mds." << it_import->first << " load " << it_import->second << dendl;
    it_import++;
  }
  map<mds_rank_t, double>::iterator it_export = state.exported.begin();
  while(it_export != state.exported.end()){
    dout(7) << " MDS_MONITOR " << __func__ << "(6) exported mds." << it_export->first << " load " << it_export->second << dendl;
    it_export++;
  }
  dout(7) << " MDS_MONITOR " << __func__ << " ----------------------------------- " << dendl;
  #endif
  try_rebalance(state);
}

int MDBalancer::mantle_prep_rebalance()
{
  balance_state_t state;

  /* refresh balancer if it has changed */
  if (bal_version != mds->mdsmap->get_balancer()) {
    bal_version.assign("");
    int r = localize_balancer();
    if (r) return r;

    /* only spam the cluster log from 1 mds on version changes */
    if (mds->get_nodeid() == 0)
      mds->clog->info() << "mantle balancer version changed: " << bal_version;
  }

  /* prepare for balancing */
  int cluster_size = mds->get_mds_map()->get_num_in_mds();
  rebalance_time = ceph_clock_now();
  mds->mdcache->migrator->clear_export_queue();

  /* fill in the metrics for each mds by grabbing load struct */
  vector < map<string, double> > metrics (cluster_size);
  for (mds_rank_t i=mds_rank_t(0);
       i < mds_rank_t(cluster_size);
       i++) {
    map<mds_rank_t, mds_load_t>::value_type val(i, mds_load_t(ceph_clock_now()));
    std::pair < map<mds_rank_t, mds_load_t>::iterator, bool > r(mds_load.insert(val));
    mds_load_t &load(r.first->second);

    metrics[i] = {{"auth.meta_load", load.auth.meta_load()},
                  {"all.meta_load", load.all.meta_load()},
                  {"req_rate", load.req_rate},
                  {"queue_len", load.queue_len},
                  {"cpu_load_avg", load.cpu_load_avg}};
  }

  /* execute the balancer */
  Mantle mantle;
  int ret = mantle.balance(bal_code, mds->get_nodeid(), metrics, state.targets);
  dout(2) << " mantle decided that new targets=" << state.targets << dendl;

  /* mantle doesn't know about cluster size, so check target len here */
  if ((int) state.targets.size() != cluster_size)
    return -EINVAL;
  else if (ret)
    return ret;

  try_rebalance(state);
  return 0;
}

void MDBalancer::simple_determine_rebalance(vector<migration_decision_t>& migration_decision){

  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (1) start to migration by simple policy "<< dendl;
  set<CDir*> already_exporting;
  rebalance_time = ceph_clock_now();
  
  int my_mds_load= calc_mds_load(get_load(rebalance_time), true);
  int sample_count = 0;
  set<CDir*> count_candidates;
  mds->mdcache->get_fullauth_subtrees(count_candidates);
  for (set<CDir*>::iterator pot = count_candidates.begin(); pot != count_candidates.end(); ++pot) {
      if ((*pot)->is_freezing() || (*pot)->is_frozen() || (*pot)->get_inode()->is_stray()) continue;
      sample_count += (*pot)->get_inode()->last_hit_amount();
  }

  if(sample_count <= g_conf->mds_bal_presetmax || my_mds_load <= 0.2* g_conf->mds_bal_presetmax ){
        dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (1.1) sample count " << sample_count << " or my load " << my_mds_load << " to less!" << dendl;
        return ;
  }



  for (auto &it : migration_decision){
    mds_rank_t target = it.target_import_mds;
    //double ex_load = it.target_export_load;
    
    double ex_load = it.target_export_percent * my_mds_load;

    dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2) want send " << it.target_export_percent << " * " <<  my_mds_load  << " load to " << target << dendl;
    
    set<CDir*> candidates;
    mds->mdcache->get_fullauth_subtrees(candidates);
    list<CDir*> exports;
    int count = 0;
    double have = 0.0;
    for (set<CDir*>::iterator pot = candidates.begin(); pot != candidates.end(); ++pot) {
      if ((*pot)->is_freezing() || (*pot)->is_frozen() || (*pot)->get_inode()->is_stray()) continue;
      
      //find_exports(*pot, ex_load, exports, have, already_exporting);
      find_exports_wrapper(*pot, ex_load, exports, have, already_exporting, target);
      if(have>= 0.8*ex_load )break;
      if(exports.size() - count>=MAX_EXPORT_SIZE)
      {
        count = exports.size();
  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << " find: " << exports.size() << " last: " << count << " leave " << target << exports << dendl;       
        break;
      }
      //if (have > amount-MIN_OFFLOAD)break;
    }

    dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (2.1) find this: " << exports << dendl;

    for (list<CDir*>::iterator it = exports.begin(); it != exports.end(); ++it) {
      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (3) exporting " << (*it)->pop_auth_subtree << "  " << (*it)->get_load(this) << " to mds." << target << " DIR " << **it <<dendl;
      mds->mdcache->migrator->export_dir_nicely(*it, target);
    }
  }
  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " (4) simple rebalance done" << dendl;
}


void MDBalancer::try_rebalance(balance_state_t& state)
{
  if (g_conf->mds_thrash_exports) {
    dout(5) << "mds_thrash is on; not performing standard rebalance operation!"
	    << dendl;
    return;
  }

  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (1) start" <<dendl;
  #endif

  // make a sorted list of my imports
  map<double,CDir*>    import_pop_map;
  multimap<mds_rank_t,CDir*>  import_from_map;
  set<CDir*> fullauthsubs;

  mds->mdcache->get_fullauth_subtrees(fullauthsubs);
  for (set<CDir*>::iterator it = fullauthsubs.begin();
       it != fullauthsubs.end();
       ++it) {
    CDir *im = *it;
    if (im->get_inode()->is_stray()) continue;

    double pop = im->get_load(this);
    #ifdef MDS_COLDFIRST_BALANCER
    
    #endif
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (2) Dir " << *im << " pop " << pop <<dendl;
    #endif

    if (g_conf->mds_bal_idle_threshold > 0 &&
	pop < g_conf->mds_bal_idle_threshold &&
	im->inode != mds->mdcache->get_root() &&
	im->inode->authority().first != mds->get_nodeid()) {
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (2) exporting idle (" << pop << " ) import " << *im
        << " back to mds. " << im->inode->authority().first <<dendl;
      #endif
      dout(5) << " exporting idle (" << pop << ") import " << *im
	      << " back to mds." << im->inode->authority().first
	      << dendl;
      mds->mdcache->migrator->export_dir_nicely(im, im->inode->authority().first);
      continue;
    }

    import_pop_map[ pop ] = im;
    mds_rank_t from = im->inode->authority().first;
    dout(15) << "  map: i imported " << *im << " from " << from << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (2) record import directory mds." << from << ", Dir " << *im << " pop " << pop <<dendl;
    #endif
    import_from_map.insert(pair<mds_rank_t,CDir*>(from, im));
  }



  // do my exports!
  set<CDir*> already_exporting;

  for (auto &it : state.targets) {
    mds_rank_t target = it.first;
    double amount = it.second;

    if (amount < MIN_OFFLOAD) continue;
    if (amount / target_load < .2) continue;

    dout(5) << "want to send " << amount << " to mds." << target
      //<< " .. " << (*it).second << " * " << load_fac
	    << " -> " << amount
	    << dendl;//" .. fudge is " << fudge << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (3) SELECT exported directory, send " << amount << " to mds." << target <<dendl;
    #endif


    double have = 0.0;


    mds->mdcache->show_subtrees();

    // search imports from target
    if (import_from_map.count(target)) {
      dout(5) << " aha, looking through imports from target mds." << target << dendl;
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (3) aha,  looking through imports from target mds." << target <<dendl;
      #endif
      pair<multimap<mds_rank_t,CDir*>::iterator, multimap<mds_rank_t,CDir*>::iterator> p =
	import_from_map.equal_range(target);
      while (p.first != p.second) {
	CDir *dir = (*p.first).second;
	dout(5) << "considering " << *dir << " from " << (*p.first).first << dendl;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " (3) considering " << *dir << " from " << (*p.first).first <<dendl;
  #endif
	multimap<mds_rank_t,CDir*>::iterator plast = p.first++;

	if (dir->inode->is_base() ||
	    dir->inode->is_stray())
	  continue;
	if (dir->is_freezing() || dir->is_frozen()) continue;  // export pbly already in progress
	double pop = dir->get_load(this);
	assert(dir->inode->authority().first == target);  // cuz that's how i put it in the map, dummy

	if (pop <= amount-have) {
	  dout(5) << "reexporting " << *dir
		  << " pop " << pop
		  << " back to mds." << target << dendl;
   
	  mds->mdcache->migrator->export_dir_nicely(dir, target);
	  have += pop;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (3) Have " << have << " reexporting " << *dir << " pop " << pop << " back to mds." << target <<dendl;
    #endif
	  import_from_map.erase(plast);
	  import_pop_map.erase(pop);
	} else {
	  dout(5) << "can't reexport " << *dir << ", too big " << pop << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (3) can't reexport " << *dir << ", too big " << pop << dendl;
    #endif
	}
	if (amount-have < MIN_OFFLOAD) break;
      }
    }


    if (amount-have < MIN_OFFLOAD) {
      continue;
    }

    // any other imports
    if (false)
      for (map<double,CDir*>::iterator import = import_pop_map.begin();
	   import != import_pop_map.end();
	   import++) {
	CDir *imp = (*import).second;
	if (imp->inode->is_base() ||
	    imp->inode->is_stray())
	  continue;

	double pop = (*import).first;
	if (pop < amount-have || pop < MIN_REEXPORT) {
	  dout(5) << "reexporting " << *imp
		  << " pop " << pop
		  << " back to mds." << imp->inode->authority()
		  << dendl;
	  have += pop;
	  mds->mdcache->migrator->export_dir_nicely(imp, imp->inode->authority().first);
	}
	if (amount-have < MIN_OFFLOAD) break;
      }


    if (amount-have < MIN_OFFLOAD) {
      //fudge = amount-have;
      continue;
    }

    // okay, search for fragments of my workload
     #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << " (4) searching directory workloads " <<dendl;
    #endif
    set<CDir*> candidates;
    mds->mdcache->get_fullauth_subtrees(candidates);

    list<CDir*> exports;

    for (set<CDir*>::iterator pot = candidates.begin();
	 pot != candidates.end();
	 ++pot) {
      if ((*pot)->get_inode()->is_stray()) continue;

      #ifdef MDS_COLDFIRST_BALANCER
      //find_exports_coldfirst_trigger(*pot, amount, exports, have, target, already_exporting);
      find_exports_coldfirst(*pot, amount, exports, have, already_exporting, target, COLDFIRST_DEPTH);
      #else
      find_exports(*pot, amount, exports, have, already_exporting);
      #endif

      #ifdef MDS_COLDFIRST_BALANCER
      dout(1) << " MDS_COLD " << __func__ << " 1: start to coldfirst migration " <<dendl;
      break;
      #endif

      #ifndef MDS_COLDFIRST_BALANCER
      if (have > amount-MIN_OFFLOAD){
        dout(1) << " MDS_COLD " << __func__ << " 2: start to coldfirst migration " <<dendl;
        break;
      }
      #endif
    }
    //fudge = amount - have;

    for (list<CDir*>::iterator it = exports.begin(); it != exports.end(); ++it) {
      dout(5) << "   - exporting "
	       << (*it)->pop_auth_subtree
	       << " "
	       << (*it)->get_load(this)
	       << " to mds." << target
	       << " " << **it
	       << dendl;
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << " (5) exporting " << (*it)->pop_auth_subtree << "  " << (*it)->get_load(this)
       << " to mds." << target << " DIR " << **it <<dendl;
      #endif
      mds->mdcache->migrator->export_dir_nicely(*it, target);
    }
  }

  dout(5) << "rebalance done" << dendl;
  mds->mdcache->show_subtrees();
}

#ifdef MDS_COLDFIRST_BALANCER
void MDBalancer::find_exports_coldfirst_trigger(CDir *dir,
                              double amount,
                              list<CDir*>& exports,
                              double& have,
                              mds_rank_t dest,
                              set<CDir*>& already_exporting)
{
  //bool have_dominator = false;
  double need = amount - have;

  list<CDir*> dominators;

  double dir_pop = dir->get_load(this);
  dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " find dominator in " << *dir << " pop " << dir_pop << " amount " << amount << " have " << have << " need " << need << dendl;
  
  if (dir_pop > amount*0.05 ) {
  find_exports_coldfirst(dir, amount, exports, have, already_exporting, dest, COLDFIRST_DEPTH);
  }


/*  for (auto it = dir->begin(); it != dir->end(); ++it) {
    CInode *in = it->second->get_linkage()->get_inode();
    if (!in) continue;
    if (!in->is_dir()) continue;

    list<CDir*> dfls;
    in->get_dirfrags(dfls);
      for (list<CDir*>::iterator p = dfls.begin();
    p != dfls.end();
    ++p) {
        CDir *subdir = *p;

        if (!subdir->is_auth()) continue;
        if (already_exporting.count(subdir)) continue;

        if (subdir->is_frozen() || dir->is_freezing() || subdir->get_inode()->is_stray()) continue;  // can't export this right now!

        // how popular?
        double pop = subdir->pop_auth_subtree.meta_load(rebalance_time, mds->mdcache->decayrate);
        //dout(1) << " subdir pop " << pop << " " << *subdir << dendl;

        list<CDir*> sub_dfls;
        subdir->get_inode()->get_dirfrags(sub_dfls);

        if (pop > need*0.05 && sub_dfls.size() > 1) {
          dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD_DOMINATOR " << __func__ << " find a dominator " << *((*it).second) << " pop: " << pop << dendl;
          //dominators.push_back(subdir);
          find_exports_coldfirst(subdir, amount, exports, have, already_exporting, dest, 10);
          have_dominator = true;
      }

    }

  }*/

  /*if(!have_dominator){
    for (list<CDir*>::iterator it = dominators.begin();
       it != dominators.end();
       ++it) {
    dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " descending into big " << **it << dendl;
    find_exports_coldfirst(*it, amount, exports, have, already_exporting, dest, 10);
    }
  }*/
  

}


void MDBalancer::find_exports_coldfirst(CDir *dir,
                              double amount,
                              list<CDir*>& exports,
                              double& have,
                              set<CDir*>& already_exporting, 
                              mds_rank_t dest,
                              int descend_depth)
{
  int cluster_size = mds->get_mds_map()->get_num_in_mds();
  double need = amount - have;
  double needmax = need * g_conf->mds_bal_need_max;
  double needmin = need * g_conf->mds_bal_need_min;

  multimap<double, CDir*> warm;
  int migcoldcount = 0;

  double dir_pop = dir->get_load(this);
  /*if (dir_pop < amount*0.01 ) {
  dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " my load is too less " << *dir << " pop " << dir_pop << " amount " << amount << " have " << have << " need " << need << dendl;
  return;
  }*/
  dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " hash in " << *dir << " pop " << dir_pop << " amount " << amount << " have " << have << " need " << need << " in (" << needmin << " - " << needmax << ")" << dendl;
  
  /*#ifdef MDS_MONITOR
  dout(1) << " MDS_MONITOR " << __func__ << " needmax " << needmax << " needmin " << needmin << " midchunk " << midchunk << " minchunk " << minchunk << dendl;
  dout(1) << " MDS_MONITOR " << __func__ << "(1) Find DIR " << *dir << " pop " << dir_pop << " amount " << amount << " have " << have << " need " << need << dendl;
  #endif  */

  double subdir_sum = 0;

  //hash frag to mds
  int frag_mod_dest = 0;
  unsigned int hash_frag = 0;
  std::hash<unsigned> hash_frag_func;

  for (auto it = dir->begin(); it != dir->end(); ++it) {
    CInode *in = it->second->get_linkage()->get_inode();
    if (!in) continue;
    if (!in->is_dir()) continue;

    list<CDir*> dfls;
    in->get_dirfrags(dfls);
    for (list<CDir*>::iterator p = dfls.begin();
   p != dfls.end();
   ++p) {
      CDir *subdir = *p;

      if (!subdir->is_auth()) continue;
      if (already_exporting.count(subdir)) continue;

      if (subdir->is_frozen()) continue;  // can't export this right now!

      // how popular?
      double pop = subdir->get_load(this);
      subdir_sum += pop;
      //dout(1) << " subdir pop " << pop << " " << *subdir << dendl;

      //frag_mod_dest = int(subdir->get_frag().value())%cluster_size;
      
      //frag hash
      //hash_frag = hash_frag_func(subdir->ino().val + subdir->get_frag().value());
      //directory hash
      hash_frag = hash_frag_func(subdir->ino().val);
      //frag_mod_dest = (hash_frag%(2*cluster_size-1) + 1)/2;
      frag_mod_dest = hash_frag%cluster_size;
      dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " subdir: " << *subdir << " frag: " << subdir->dirfrag() << " hash_frag: " << hash_frag << " frag_mod_dest: " << frag_mod_dest <<  " target: " << dest << dendl; 
      
      list<CDir*> sub_dfls;
      subdir->get_inode()->get_dirfrags(sub_dfls);
      dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " subdir: " << *subdir <<"   is " << sub_dfls.size() << dendl;

      if(sub_dfls.size() >= 1 && descend_depth > 0){
          dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " descending into " << *subdir << dendl;
          find_exports_coldfirst(subdir, amount, exports, have, already_exporting, dest, descend_depth-1);
      }

      if(frag_mod_dest!=mds->get_nodeid()){
          if(frag_mod_dest == dest){
          exports.push_back(subdir);
          already_exporting.insert(subdir);
          //have += pop;
          migcoldcount++;
          if(migcoldcount>=100){
            dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " find 100 frag enough " <<dendl;
            return;
          }
          }
      }

    }
  }
  dout(15) << "   sum " << subdir_sum << " / " << dir_pop << dendl;


  dout(LUNULE_DEBUG_LEVEL) << " MDS_COLD " << __func__ << " export " << migcoldcount << " small and cold, stop " <<dendl;
  //have += need/mds->get_mds_map()->get_num_in_mds();
}
#endif

void MDBalancer::find_exports(CDir *dir,
                              double amount,
                              list<CDir*>& exports,
                              double& have,
                              set<CDir*>& already_exporting,
			      mds_rank_t target)
{
  int my_exports = exports.size();
  dout(LUNULE_DEBUG_LEVEL) << " [WAN]: old export was happen to " << *dir << dendl;
  double need = amount - have;
  if (need < amount * g_conf->mds_bal_min_start)
    return;   // good enough!
  double needmax = need * g_conf->mds_bal_need_max;
  double needmin = need * g_conf->mds_bal_need_min;
  double midchunk = need * g_conf->mds_bal_midchunk;
  double minchunk = need * g_conf->mds_bal_minchunk;

  list<CDir*> bigger_rep, bigger_unrep;
  multimap<double, CDir*> smaller;

  double dir_pop = dir->get_load(this);
  dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " find in " << *dir << " pop: " << dir_pop << " Vel: " << dir->pop_auth_subtree.show_meta_vel() << " need " << need << " (" << needmin << " - " << needmax << ")" << dendl;
  //dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " Vel: " << dir->pop_auth_subtree.show_meta_vel()<<dendl;
  dout(7) << " find_exports in " << dir_pop << " " << *dir << " need " << need << " (" << needmin << " - " << needmax << ")" << dendl;
  #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << " needmax " << needmax << " needmin " << needmin << " midchunk " << midchunk << " minchunk " << minchunk << dendl;
  dout(7) << " MDS_MONITOR " << __func__ << "(1) Find DIR " << *dir << " expect_request " << dir_pop << 
  " amount " << amount << " have " << have << " need " << need << dendl;
  #endif  

  double subdir_sum = 0;
  for (auto it = dir->begin(); it != dir->end(); ++it) {
    CInode *in = it->second->get_linkage()->get_inode();
    if (!in) continue;
    if (!in->is_dir()) continue;

    list<CDir*> dfls;
    in->get_dirfrags(dfls);
    for (list<CDir*>::iterator p = dfls.begin();
	 p != dfls.end();
	 ++p) {
      CDir *subdir = *p;


      if (!subdir->is_auth()) continue;
      if (already_exporting.count(subdir)) continue;

      if (subdir->is_frozen() || subdir->is_freezing() || subdir->get_inode()->is_stray()) continue;  // can't export this right now!

      // how popular?
      double pop = subdir->get_load(this);
      subdir_sum += pop;
      
      /*if(pop<0){
      string s;
      subdir->get_inode()->make_path_string(s);
      pair<double, double> result = req_tracer.alpha_beta(s, subdir->get_num_dentries_auth_subtree_nested());
      dout(0) << " [Wrong!] minus pop " << pop << " " << *subdir << " potauth: " << subdir->pot_auth << " alpha: " << result.first << " beta: " << result.second << dendl;
      }*/

      dout(LUNULE_DEBUG_LEVEL) << " MDS_IFBEAT " << __func__ << " find in subdir " << *subdir << " pop: " << pop << " have " << have << " Vel: " << subdir->pop_auth_subtree.show_meta_vel() << dendl;

      dout(15) << "   subdir pop " << pop << " " << *subdir << dendl;
      #ifdef MDS_MONITOR
      dout(7) << " MDS_MONITOR " << __func__ << "(2) Searching DIR " << *subdir << " pop " << pop << dendl;
      #endif

      if (pop < minchunk) continue;

      if(exports.size() - my_exports>=MAX_EXPORT_SIZE)
      {
        dout(LUNULE_DEBUG_LEVEL) << " [WAN]: got" << exports.size() - my_exports << " targets, enough! " << *dir << dendl;
        return;
      }

      // lucky find?
      if (pop > needmin && pop < needmax) {
        #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << "(2) Lucky Find DIR " << *subdir << " pop " << pop << 
  " needmin~needmax " << needmin << " ~ " << needmax << " have " << have << " need " << need << dendl;
  #endif 
	exports.push_back(subdir);
	already_exporting.insert(subdir);

	have += pop;
	return;
      }

      if (pop > need) {
	if (subdir->is_rep())
	  bigger_rep.push_back(subdir);
	else
	  bigger_unrep.push_back(subdir);
      } else
	smaller.insert(pair<double,CDir*>(pop, subdir));
    }
  }
  dout(15) << "   sum " << subdir_sum << " / " << dir_pop << dendl;

  // grab some sufficiently big small items
  multimap<double,CDir*>::reverse_iterator it;
  for (it = smaller.rbegin();
       it != smaller.rend();
       ++it) {

    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << "(3) See smaller DIR " << *((*it).second) << " pop " << (*it).first << dendl;
    #endif

    if ((*it).first < midchunk)
      break;  // try later

    dout(7) << "   taking smaller " << *(*it).second << dendl;
    #ifdef MDS_MONITOR
    dout(7) << " MDS_MONITOR " << __func__ << "(3) taking smaller DIR " << *((*it).second) << " pop " << (*it).first << dendl;
    #endif
    exports.push_back((*it).second);
    already_exporting.insert((*it).second);
    have += (*it).first;
    if(exports.size() - my_exports>=MAX_EXPORT_SIZE)
  {
    dout(LUNULE_DEBUG_LEVEL) << " [WAN]: enough! " << *dir << dendl;
    return;
  }
    if (have > needmin)
      return;
  }

  // apprently not enough; drill deeper into the hierarchy (if non-replicated)
  /*for (list<CDir*>::iterator it = bigger_unrep.begin();
       it != bigger_unrep.end();
       ++it) {
  dynamically_fragment(*it, amount);
  }*/

  for (list<CDir*>::iterator it = bigger_unrep.begin();
       it != bigger_unrep.end();
       ++it) {
    #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << "(4) descending into bigger DIR " << **it << dendl;
  #endif
    dout(15) << "   descending into " << **it << dendl;
    find_exports(*it, amount, exports, have, already_exporting);
    //find_exports_wrapper(*it, amount, exports, have, already_exporting, target);
    if(exports.size() - my_exports>=MAX_EXPORT_SIZE)
  {
    dout(LUNULE_DEBUG_LEVEL) << " [WAN]: enough! " << *dir << dendl;
    return;
  }
    if (have > needmin)
      return;
  }

  // ok fine, use smaller bits
  for (;
       it != smaller.rend();
       ++it) {
    dout(7) << "   taking (much) smaller " << it->first << " " << *(*it).second << dendl;
    #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << "(5) taking (much) smaller DIR " << *((*it).second) << " pop " << (*it).first << dendl;
  #endif
    exports.push_back((*it).second);
    already_exporting.insert((*it).second);
    have += (*it).first;
    if(exports.size() - my_exports>=MAX_EXPORT_SIZE)
  {
    dout(LUNULE_DEBUG_LEVEL) << " [WAN]: enough! " << *dir << dendl;
    return;
  }
    if (have > needmin)
      return;
  }

  // ok fine, drill into replicated dirs
  for (list<CDir*>::iterator it = bigger_rep.begin();
       it != bigger_rep.end();
       ++it) {
    #ifdef MDS_MONITOR
  dout(7) << " MDS_MONITOR " << __func__ << "(6) descending into replicated DIR " << **it << dendl;
  #endif
    dout(7) << "   descending into replicated " << **it << dendl;
    find_exports(*it, amount, exports, have, already_exporting);
    //find_exports_wrapper(*it, amount, exports, have, already_exporting, target);
    if (have > needmin)
      return;
  }

}


void MDBalancer::dynamically_fragment(CDir *dir, double amount){
  if( amount <= 0.1){
    dout(LUNULE_DEBUG_LEVEL) << __func__ << " amount to low: " << amount << *dir << dendl;
    return;
  }

  if (dir->get_inode()->is_stray() || !dir->is_auth()) return;
  dout(LUNULE_DEBUG_LEVEL) << __func__ << " dynamically(0): " << *dir << dendl;
  double dir_pop = dir->get_load(this);
  if(dir_pop >amount*0.6){
    dout(LUNULE_DEBUG_LEVEL) << __func__ << " my_pop:  " << dir_pop << " is big enough for: " << amount << *dir << dendl;
    double sub_dir_total = 0;
    for (auto it = dir->begin(); it != dir->end(); ++it) {
    CInode *in = it->second->get_linkage()->get_inode();
    if (!in) continue;
    if (!in->is_dir()) continue;
    dout(LUNULE_DEBUG_LEVEL) << __func__ << " dynamically(1): I'm " << *in << dendl;

    list<CDir*> dfls;

    in->get_dirfrags(dfls);
    for (list<CDir*>::iterator p = dfls.begin();
   p != dfls.end();
   ++p) {
      CDir *subdir = *p;
      double sub_dir_pop = subdir->get_load(this);
      sub_dir_total +=sub_dir_pop;
      }
   }
  double file_hot_ratio = 1-sub_dir_total/dir_pop;
  if(file_hot_ratio >=0.5){
    dout(LUNULE_DEBUG_LEVEL) << __func__ << " dynamically (2) file_access_hotspot " << file_hot_ratio << " find in " << *dir << dendl;
    maybe_fragment(dir,true);
  }
  dout(LUNULE_DEBUG_LEVEL) << __func__ << " dynamically(3) I'm" << *dir << " my_pop: " << dir_pop << " my_sub_pop: " << sub_dir_total << dendl;


  }else{
    return;
  }

   

}

void MDBalancer::find_exports_wrapper(CDir *dir,
                    double amount,
                    list<CDir*>& exports,
                    double& have,
                    set<CDir*>& already_exporting,
		    mds_rank_t target)
{
  if(dir->inode->is_stray())return;
  double total_hot = 0;
  string s;
  dir->get_inode()->make_path_string(s);
  WorkloadType wlt = adsl::workload2type(adsl::g_matcher.match(s));
  dout(LUNULE_DEBUG_LEVEL) << __func__ << " path=" << s << " workload=" << adsl::g_matcher.match(s) << " type=" << wlt << dendl;
  
  //dynamically_fragment(dir, amount);
  
  switch (wlt) {
    /*
    case WLT_SCAN:
    if(mds->get_nodeid()==0){
      find_exports_coldfirst_trigger(dir, amount, exports, have, target, already_exporting);
    }else{
      find_exports_coldfirst_trigger(dir->inode->get_parent_dir(), amount, exports, have, target, already_exporting);
    }
      break;

    case WLT_ZIPF:
      if(mds->get_nodeid()==0){
      dout(LUNULE_DEBUG_LEVEL) << __func__ << " ZIPF: diving to " << *dir << dendl;
      find_exports(dir, amount, exports, have, already_exporting, target);
      }else{
      dout(LUNULE_DEBUG_LEVEL) << __func__ << " ZIPF: diving to parent: " << *(dir->inode->get_parent_dir()) << dendl;
        find_exports(dir->inode->get_parent_dir(), amount, exports, have, already_exporting, target);
      }
      break;
    */
    case WLT_ROOT:
      dout(LUNULE_DEBUG_LEVEL) << __func__ << " Root: diving to " << *dir << dendl;
      find_exports(dir, amount, exports, have, already_exporting, target);
    /*for (auto it = dir->begin(); it != dir->end(); ++it) {
      CInode *in = it->second->get_linkage()->get_inode();
      if (!in) continue;
      if (!in->is_dir()) continue;
      list<CDir*> root_sub_dfls;
      in->get_dirfrags(root_sub_dfls);
      for (list<CDir*>::iterator p = root_sub_dfls.begin();p != root_sub_dfls.end();++p) {
        CDir *subdir = *p;
        
        dout(LUNULE_DEBUG_LEVEL) << __func__ << " warrper descend: from root to" << *subdir << dendl;
        find_exports_wrapper(subdir, amount, exports, have, already_exporting, target);
      }
    }*/
      break;
    default:
      if(mds->get_nodeid()==0){
      dout(LUNULE_DEBUG_LEVEL) << __func__ << " Unknown-MDS0: diving to " << *dir << dendl;
      find_exports(dir, amount, exports, have, already_exporting, target);
      }else{
      if (dir->inode->get_parent_dir())
      {
        dout(LUNULE_DEBUG_LEVEL) << __func__ << " Unknown-MDS not 0: diving to parent: " << *(dir->inode->get_parent_dir()) << dendl;
        find_exports(dir->inode->get_parent_dir(), amount, exports, have, already_exporting, target);
      }else{
        dout(LUNULE_DEBUG_LEVEL) << __func__ << " Unknown-MDS not 0: no parent: " << *dir << dendl;
        find_exports(dir, amount, exports, have, already_exporting, target);
      }
      }
      break;
  }
}


void MDBalancer::hit_inode(utime_t now, CInode *in, int type, int who)
{
  // hit inode pop and count
  in->pop.get(type).hit(now, mds->mdcache->decayrate);
  int newold = in->hit(true, beat_epoch);

  if (in->get_parent_dn()) {
    hit_dir(now, in->get_parent_dn()->get_dir(), type, who, 1.0, newold);
  }
}


void MDBalancer::maybe_fragment(CDir *dir, bool hot)
{
  //dout(0) << __func__ << " split " << *dir << dendl;
  dout(20) << __func__ << dendl;
  // split/merge
  if (g_conf->mds_bal_frag && g_conf->mds_bal_fragment_interval > 0 &&
      !dir->inode->is_base() &&        // not root/base (for now at least)
      dir->is_auth()) {

    // split
    if (g_conf->mds_bal_split_size > 0 &&
	mds->mdsmap->allows_dirfrags() &&
	(dir->should_split() || hot))
    {
      dout(20) << __func__ << " mds_bal_split_size " << g_conf->mds_bal_split_size << " dir's frag size " << dir->get_frag_size() << dendl;
      if (split_pending.count(dir->dirfrag()) == 0) {
        queue_split(dir, false);
      } else {
        if (dir->should_split_fast()) {
          queue_split(dir, true);
        } else {
          dout(10) << __func__ << ": fragment already enqueued to split: "
                   << *dir << dendl;
        }
      }
    }

    // merge?
    if (dir->get_frag() != frag_t() && dir->should_merge() &&
	merge_pending.count(dir->dirfrag()) == 0) {
      queue_merge(dir);
    }
  }
}

//auto update_dir_pot_recur = [this] (CDir * dir, int level, double adj_auth_pot = 1.0, double adj_all_pot = 1.0) -> void {
void MDBalancer::update_dir_pot_recur(CDir * dir, int level, double adj_auth_pot, double adj_all_pot)
{
  string s = "/";
  if (dir->inode->get_parent_dn())
    dir->inode->make_path_string(s);
  dout(0) << __func__ << " path=" << s << " level=" << level << " adj_auth=" << adj_auth_pot << " adj_all=" << adj_all_pot << dendl;

  // adjust myself
  dir->pot_all.adjust(adj_all_pot, beat_epoch);
  if (dir->is_auth())
    dir->pot_auth.adjust(adj_auth_pot, beat_epoch);

  int brocount = 0;
  if (level <= 0)	goto finish;

  for (auto it = dir->begin(); it != dir->end(); it++)
    brocount += dir->get_authsubtree_size_slow(beat_epoch);
  for (auto it = dir->begin(); it != dir->end(); it++) {
    int my_subtree_size = dir->get_authsubtree_size_slow(beat_epoch);
    double my_adj_auth_pot = adj_auth_pot * my_subtree_size / brocount;
    double my_adj_all_pot = adj_all_pot * my_subtree_size / brocount;
    CDentry::linkage_t * de_l = it->second->get_linkage();
    if (de_l && de_l->is_primary()) {
      CInode * in = de_l->get_inode();
      list<CDir *> petals;
      in->get_dirfrags(petals);
      int brothers_count = 0;
      int brothers_auth_count = 0;
      for (CDir * petal : petals) {
        brothers_count += petal->get_num_any();
        if (petal->is_auth())
          brothers_auth_count += petal->get_num_any();
      }
      double adj_auth_single = brothers_auth_count ? (my_adj_auth_pot / brothers_auth_count) : 0.0;
      double adj_all_single = brothers_count ? (my_adj_all_pot / brothers_count) : 0.0;
      for (CDir * petal : petals) {
        update_dir_pot_recur(petal, level - 1, petal->get_num_any() * adj_auth_single, petal->get_num_any() * adj_all_single);
      }
    }
  }

finish:
  dout(0) << __func__ << " after adjust, path=" << s << " level=" << level << " pot_auth=" << dir->pot_auth << " pot_all=" << dir->pot_all << dendl;
}


void MDBalancer::hit_dir(utime_t now, CDir *dir, int type, int who, double amount, int newold)
{
  // hit me
  double v = dir->pop_me.get(type).hit(now, mds->mdcache->decayrate, amount);

  const bool hot = (v > g_conf->mds_bal_split_rd && type == META_POP_IRD) ||
                   (v > g_conf->mds_bal_split_wr && type == META_POP_IWR);

  dout(20) << "hit_dir " << dir->get_path() << " mds_bal_split_rd " << g_conf->mds_bal_split_rd << " mds_bal_split_wr " << g_conf->mds_bal_split_wr << " hit " << v << dendl; 
  dout(20) << "hit_dir " << dir->get_path() << " " << type << " pop is " << v << ", frag " << dir->get_frag()
           << " size " << dir->get_frag_size() << dendl;

  //maybe_fragment(dir, hot);

  // replicate?
  if (type == META_POP_IRD && who >= 0) {
    dir->pop_spread.hit(now, mds->mdcache->decayrate, who);
  }

  double rd_adj = 0.0;
  if (type == META_POP_IRD &&
      dir->last_popularity_sample < last_sample) {
    double dir_pop = dir->pop_auth_subtree.get(type).get(now, mds->mdcache->decayrate);    // hmm??
    dir->last_popularity_sample = last_sample;
    double pop_sp = dir->pop_spread.get(now, mds->mdcache->decayrate);
    dir_pop += pop_sp * 10;

    //if (dir->ino() == inodeno_t(0x10000000002))
    if (pop_sp > 0) {
      dout(20) << "hit_dir " << type << " pop " << dir_pop << " spread " << pop_sp
	      << " " << dir->pop_spread.last[0]
	      << " " << dir->pop_spread.last[1]
	      << " " << dir->pop_spread.last[2]
	      << " " << dir->pop_spread.last[3]
	      << " in " << *dir << dendl;
    }

    dout(20) << "TAG hit_dir " << dir->get_path() << " dir_pop " << dir_pop << " mds_bal_replicate_threshold " << g_conf->mds_bal_replicate_threshold << dendl; 
    if (dir->is_auth() && !dir->is_ambiguous_auth()) {
      if (!dir->is_rep() &&
	  dir_pop >= g_conf->mds_bal_replicate_threshold) {
	dout(20) << "hit_dir dir " << dir->get_path() << " dir_pop " << dir_pop << " > mds_bal_replicate_threshold " << g_conf->mds_bal_replicate_threshold << ", replicate dir " << dir->get_path() << "!" << dendl;
	// replicate
	double rdp = dir->pop_me.get(META_POP_IRD).get(now, mds->mdcache->decayrate);
	rd_adj = rdp / mds->get_mds_map()->get_num_in_mds() - rdp;
	rd_adj /= 2.0;  // temper somewhat

	dout(5) << "replicating dir " << *dir << " pop " << dir_pop << " .. rdp " << rdp << " adj " << rd_adj << dendl;

	#ifdef MDS_MONITOR
	dout(LUNULE_DEBUG_LEVEL) << "replicating dir " << *dir << " pop " << dir_pop << " .. rdp " << rdp << " adj " << rd_adj << dendl;
	#endif
	
	dir->dir_rep = CDir::REP_ALL;
	mds->mdcache->send_dir_updates(dir, true);

	// fixme this should adjust the whole pop hierarchy
	dir->pop_me.get(META_POP_IRD).adjust(rd_adj);
	dir->pop_auth_subtree.get(META_POP_IRD).adjust(rd_adj);
      }

      if (dir->ino() != 1 &&
	  dir->is_rep() &&
	  dir_pop < g_conf->mds_bal_unreplicate_threshold) {
	// unreplicate
	dout(5) << "unreplicating dir " << *dir << " pop " << dir_pop << dendl;

	dir->dir_rep = CDir::REP_NONE;
	mds->mdcache->send_dir_updates(dir);
      }
    }
  }

  // adjust ancestors
  bool hit_subtree = dir->is_auth();         // current auth subtree (if any)
  bool hit_subtree_nested = dir->is_auth();  // all nested auth subtrees

  CDir * origdir = dir;
  while (true) {
    dir->pop_nested.get(type).hit(now, mds->mdcache->decayrate, amount);
    if (rd_adj != 0.0)
      dir->pop_nested.get(META_POP_IRD).adjust(now, mds->mdcache->decayrate, rd_adj);

    if (hit_subtree) {
      dir->pop_auth_subtree.get(type).hit(now, mds->mdcache->decayrate, amount);
      if (rd_adj != 0.0)
	dir->pop_auth_subtree.get(META_POP_IRD).adjust(now, mds->mdcache->decayrate, rd_adj);
    }

    if (hit_subtree_nested) {
      dir->pop_auth_subtree_nested.get(type).hit(now, mds->mdcache->decayrate, amount);
      if (rd_adj != 0.0)
	dir->pop_auth_subtree_nested.get(META_POP_IRD).adjust(now, mds->mdcache->decayrate, rd_adj);
    }

    if (dir->is_subtree_root())
      hit_subtree = false;                // end of auth domain, stop hitting auth counters.

    if (dir->inode->get_parent_dn() == 0) break;
    dir = dir->inode->get_parent_dn()->get_dir();
  }

  if (newold < 1)	return;

  dir = origdir;
  dout(0) << __func__ << " DEBUG dir=" << dir->get_path() << dendl;
  //CDentry* dn = dir->get_inode()->get_parent_dn();
  //dout(0) << __func__ << " DEBUG2 dir=" << (dn ? dn->get_name() : "/") << dendl;
  // adjust potential load for brother dirfrags
  auto update_dir_pot = [this](CDir * dir, int level = 0) -> bool {
    CInode * in = dir->inode;
    int i;
    for (i = 0; i < level; i++) {
      if (!in->get_parent_dn())	break;
      in = in->get_parent_dn()->get_dir()->get_inode();
    }
    bool ret = (i == level);
    level = i;

    list<CDir *> petals;
    in->get_dirfrags(petals);
    int brothers_count = 0;
    int brothers_auth_count = 0;
    for (CDir * petal : petals) {
      brothers_count += petal->get_num_any();
      if (petal->is_auth())
	brothers_auth_count += petal->get_num_any();
    }

    dir->pot_cached.inc(beat_epoch);
    double cached_load = dir->pot_cached.pot_load(beat_epoch, true);
    if (cached_load < brothers_auth_count) {
      return false;
    }
    dir->pot_cached.clear(beat_epoch);
    
    double adj_auth_single = brothers_auth_count ? (cached_load / brothers_auth_count) : 0.0;
    double adj_all_single = brothers_count ? (cached_load / brothers_count) : 0.0;
    for (CDir * petal : petals) {
      update_dir_pot_recur(petal, level, petal->get_num_any() * adj_auth_single, petal->get_num_any() * adj_all_single);
    }
    return ret;
  };

  bool update_pot_auth = dir->is_auth();
  //if (!update_pot_auth || !dir->inode->get_parent_dn()) return;
  if (!dir->inode->get_parent_dn()) {
    update_dir_pot(dir);
    return;
  }

  if (update_dir_pot(dir, 1))
    dir = dir->inode->get_parent_dn()->get_dir();

  while (dir->inode->get_parent_dn()) {
    dir = dir->inode->get_parent_dn()->get_dir();
    // adjust ancestors' pot
    if (update_pot_auth)
      dir->pot_auth.inc(beat_epoch);
    //dir->pot_auth.inc(beat_epoch);
    dir->pot_all.inc(beat_epoch);
  }

  //set<CDir *> authsubs;
  //mds->mdcache->get_auth_subtrees(authsubs);
  //dout(0) << __func__ << " authsubtrees:" << dendl;
  //for (CDir * dir : authsubs) {
  //  string s;
  //  dir->get_inode()->make_path_string(s);
  //  dout(0) << __func__ << "  path: " << s << " pot_auth=" << dir->pot_auth << " pot_all=" << dir->pot_all << dendl;
  //}
}


/*
 * subtract off an exported chunk.
 *  this excludes *dir itself (encode_export_dir should have take care of that)
 *  we _just_ do the parents' nested counters.
 *
 * NOTE: call me _after_ forcing *dir into a subtree root,
 *       but _before_ doing the encode_export_dirs.
 */
void MDBalancer::subtract_export(CDir *dir, utime_t now)
{
  dirfrag_load_vec_t subload = dir->pop_auth_subtree;

  while (true) {
    dir = dir->inode->get_parent_dir();
    if (!dir) break;

    dir->pop_nested.sub(now, mds->mdcache->decayrate, subload);
    dir->pop_auth_subtree_nested.sub(now, mds->mdcache->decayrate, subload);
  }
}


void MDBalancer::add_import(CDir *dir, utime_t now)
{
  dirfrag_load_vec_t subload = dir->pop_auth_subtree;

  while (true) {
    dir = dir->inode->get_parent_dir();
    if (!dir) break;

    dir->pop_nested.add(now, mds->mdcache->decayrate, subload);
    dir->pop_auth_subtree_nested.add(now, mds->mdcache->decayrate, subload);
  }
}

void MDBalancer::handle_mds_failure(mds_rank_t who)
{
  if (0 == who) {
    last_epoch_under = 0;
  }
}

double MDBalancer::calc_mds_load(mds_load_t load, bool auth)
{
<<<<<<< HEAD
  if (!mds->mdcache->root){
    dout(0) << __func__ << " dont calculate mds load " << dendl;
    return 0.0;}
    
=======
  if (!mds->mdcache->root)
    return 0.0;
>>>>>>> remotes/origin/lunule1.2-alpha-beta

  //vector<string> betastrs;
  //pair<double, double> result = req_tracer.alpha_beta("/", total, betastrs);
  pair<double, double> result = mds->mdcache->root->alpha_beta(beat_epoch);
  double ret = load.mds_load(result.first, result.second, beat_epoch, auth, this);
<<<<<<< HEAD
  dout(0) << __func__ << " load=" << load << " alpha=" << result.first << " beta=" << result.second << " pop=" << load.mds_pop_load() << " pot=" << load.mds_pot_load(auth, beat_epoch) << " result=" << ret << dendl;
=======
  dout(7) << __func__ << " load=" << load << " alpha=" << result.first << " beta=" << result.second << " pop=" << load.mds_pop_load() << " pot=" << load.mds_pot_load(auth, beat_epoch) << " result=" << ret << dendl;
>>>>>>> remotes/origin/lunule1.2-alpha-beta
  //if (result.second < 0) {
  //  dout(7) << __func__ << " Illegal beta detected" << dendl;
  //  for (string s : betastrs) {
  //    dout(7) << __func__ << "   " << s << dendl;
  //  }
  //}
  return ret;
}
