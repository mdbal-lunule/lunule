// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 John Spray <john.spray@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#ifndef CLUSTER_STATE_H_
#define CLUSTER_STATE_H_

#include "mds/FSMap.h"
#include "mon/MgrMap.h"
#include "common/Mutex.h"

#include "osdc/Objecter.h"
#include "mon/MonClient.h"
#include "mon/PGMap.h"
#include "mgr/ServiceMap.h"

class MMgrDigest;
class MMonMgrReport;
class MPGStats;


/**
 * Cluster-scope state (things like cluster maps) as opposed
 * to daemon-level state (things like perf counters and smart)
 */
class ClusterState
{
protected:
  MonClient *monc;
  Objecter *objecter;
  FSMap fsmap;
  ServiceMap servicemap;
  mutable Mutex lock;

  MgrMap mgr_map;

  set<int64_t> existing_pools; ///< pools that exist, as of PGMap epoch
  PGMap pg_map;
  PGMap::Incremental pending_inc;

  PGMapStatService pgservice;

  bufferlist health_json;
  bufferlist mon_status_json;

public:

  void load_digest(MMgrDigest *m);
  void ingest_pgstats(MPGStats *stats);

  void update_delta_stats();

  const bufferlist &get_health() const {return health_json;}
  const bufferlist &get_mon_status() const {return mon_status_json;}

  ClusterState(MonClient *monc_, Objecter *objecter_, const MgrMap& mgrmap);

  void set_objecter(Objecter *objecter_);
  void set_fsmap(FSMap const &new_fsmap);
  void set_mgr_map(MgrMap const &new_mgrmap);
  void set_service_map(ServiceMap const &new_service_map);

  void notify_osdmap(const OSDMap &osd_map);

  bool have_fsmap() const {
    Mutex::Locker l(lock);
    return fsmap.get_epoch() > 0;
  }

  template<typename Callback, typename...Args>
  void with_servicemap(Callback&& cb, Args&&...args) const
  {
    Mutex::Locker l(lock);
    std::forward<Callback>(cb)(servicemap, std::forward<Args>(args)...);
  }

  template<typename Callback, typename...Args>
  void with_fsmap(Callback&& cb, Args&&...args) const
  {
    Mutex::Locker l(lock);
    std::forward<Callback>(cb)(fsmap, std::forward<Args>(args)...);
  }

  template<typename Callback, typename...Args>
  void with_mgrmap(Callback&& cb, Args&&...args) const
  {
    Mutex::Locker l(lock);
    std::forward<Callback>(cb)(mgr_map, std::forward<Args>(args)...);
  }

  template<typename Callback, typename...Args>
  auto with_pgmap(Callback&& cb, Args&&...args) const ->
    decltype(cb(pg_map, std::forward<Args>(args)...))
  {
    Mutex::Locker l(lock);
    return std::forward<Callback>(cb)(pg_map, std::forward<Args>(args)...);
  }

  template<typename Callback, typename...Args>
  auto with_pgservice(Callback&& cb, Args&&...args) const ->
    decltype(cb(pgservice, std::forward<Args>(args)...))
  {
    Mutex::Locker l(lock);
    return std::forward<Callback>(cb)(pg_map, std::forward<Args>(args)...);
  }

  template<typename... Args>
  void with_monmap(Args &&... args) const
  {
    Mutex::Locker l(lock);
    assert(monc != nullptr);
    monc->with_monmap(std::forward<Args>(args)...);
  }

  template<typename... Args>
  auto with_osdmap(Args &&... args) const ->
    decltype(objecter->with_osdmap(std::forward<Args>(args)...))
  {
    assert(objecter != nullptr);
    return objecter->with_osdmap(std::forward<Args>(args)...);
  }

};

#endif

