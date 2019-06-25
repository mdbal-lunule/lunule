// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_HEARTBEATMAP_H
#define CEPH_HEARTBEATMAP_H

#include <list>
#include <atomic>
#include <string>

#include <pthread.h>

#include "RWLock.h"

class CephContext;

namespace ceph {

/*
 * HeartbeatMap -
 *
 * Maintain a set of handles for internal subsystems to periodically
 * check in with a health check and timeout.  Each user can register
 * and get a handle they can use to set or reset a timeout.  
 *
 * A simple is_healthy() method checks for any users who are not within
 * their grace period for a heartbeat.
 */

struct heartbeat_handle_d {
  const std::string name;
  pthread_t thread_id;
  std::atomic<unsigned> timeout = { 0 }, suicide_timeout = { 0 };
  time_t grace, suicide_grace;
  std::list<heartbeat_handle_d*>::iterator list_item;

  explicit heartbeat_handle_d(const std::string& n)
    : name(n), thread_id(0), grace(0), suicide_grace(0)
  { }
};

class HeartbeatMap {
 public:
  // register/unregister
  heartbeat_handle_d *add_worker(const std::string& name, pthread_t thread_id);
  void remove_worker(const heartbeat_handle_d *h);

  // reset the timeout so that it expects another touch within grace amount of time
  void reset_timeout(heartbeat_handle_d *h, time_t grace, time_t suicide_grace);
  // clear the timeout so that it's not checked on
  void clear_timeout(heartbeat_handle_d *h);

  // return false if any of the timeouts are currently expired.
  bool is_healthy();

  // touch cct->_conf->heartbeat_file if is_healthy()
  void check_touch_file();

  // get the number of unhealthy workers
  int get_unhealthy_workers() const;

  // get the number of total workers
  int get_total_workers() const;

  explicit HeartbeatMap(CephContext *cct);
  ~HeartbeatMap();

 private:
  CephContext *m_cct;
  RWLock m_rwlock;
  time_t m_inject_unhealthy_until;
  std::list<heartbeat_handle_d*> m_workers;
  std::atomic<unsigned> m_unhealthy_workers = { 0 };
  std::atomic<unsigned> m_total_workers = { 0 };

  bool _check(const heartbeat_handle_d *h, const char *who, time_t now);
};

}
#endif
