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

#include <sstream>
#include <stdlib.h>
#include <limits.h>

// #include <boost/intrusive_ptr.hpp>
// Because intusive_ptr clobbers our assert...
#include "include/assert.h"

#include "mon/Monitor.h"
#include "mon/HealthService.h"
#include "mon/OldHealthMonitor.h"
#include "mon/DataHealthService.h"

#include "messages/MMonHealth.h"
#include "common/Formatter.h"
// #include "common/config.h"

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, this)
static ostream& _prefix(std::ostream *_dout, const Monitor *mon,
                        const OldHealthMonitor *hmon) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name() << ")." << hmon->get_name()
                << "(" << hmon->get_epoch() << ") ";
}

void OldHealthMonitor::init()
{
  dout(10) << __func__ << dendl;
  assert(services.empty());
  services[HealthService::SERVICE_HEALTH_DATA] = new DataHealthService(mon);

  for (map<int,HealthService*>::iterator it = services.begin();
       it != services.end();
       ++it) {
    it->second->init();
  }
}

bool OldHealthMonitor::service_dispatch(MonOpRequestRef op)
{
  assert(op->get_req()->get_type() == MSG_MON_HEALTH);
  MMonHealth *hm = static_cast<MMonHealth*>(op->get_req());
  int service_type = hm->get_service_type();
  if (services.count(service_type) == 0) {
    dout(1) << __func__ << " service type " << service_type
            << " not registered -- drop message!" << dendl;
    return false;
  }
  return services[service_type]->service_dispatch(op);
}

void OldHealthMonitor::start_epoch() {
  epoch_t epoch = get_epoch();
  for (map<int,HealthService*>::iterator it = services.begin();
       it != services.end(); ++it) {
    it->second->start(epoch);
  }
}

void OldHealthMonitor::finish_epoch() {
  generic_dout(20) << "OldHealthMonitor::finish_epoch()" << dendl;
  for (map<int,HealthService*>::iterator it = services.begin();
       it != services.end(); ++it) {
    assert(it->second != NULL);
    it->second->finish();
  }
}

void OldHealthMonitor::service_shutdown()
{
  dout(0) << "OldHealthMonitor::service_shutdown "
          << services.size() << " services" << dendl;
  for (map<int,HealthService*>::iterator it = services.begin();
      it != services.end();
       ++it) {
    it->second->shutdown();
    delete it->second;
  }
  services.clear();
}

void OldHealthMonitor::get_health(
  list<pair<health_status_t,string> >& summary,
  list<pair<health_status_t,string> > *detail)
{
  for (map<int,HealthService*>::iterator it = services.begin();
       it != services.end();
       ++it) {
    it->second->get_health(summary, detail);
  }
}
