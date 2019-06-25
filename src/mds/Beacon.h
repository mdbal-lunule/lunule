// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2012 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef BEACON_STATE_H
#define BEACON_STATE_H

#include <boost/utility/string_view.hpp>

#include "include/types.h"
#include "include/Context.h"
#include "common/Mutex.h"
#include "msg/Dispatcher.h"
#include "messages/MMDSBeacon.h"

class MonClient;
class MMDSBeacon;
class Message;
class MDSRank;


/**
 * One of these per MDS.  Handle beacon logic in this separate class so
 * that a busy MDS holding its own lock does not hold up sending beacon
 * messages to the mon and cause false lagginess.
 *
 * So that we can continue to operate while the MDS is holding its own lock,
 * we keep copies of the data needed to generate beacon messages.  The MDS is
 * responsible for calling Beacon::notify_* when things change.
 */
class Beacon : public Dispatcher
{
public:
  Beacon(CephContext *cct_, MonClient *monc_, boost::string_view name);
  ~Beacon() override;

  void init(MDSMap const *mdsmap);
  void shutdown();

  bool ms_dispatch(Message *m) override;
  void ms_handle_connect(Connection *c) override {}
  bool ms_handle_reset(Connection *c) override {return false;}
  void ms_handle_remote_reset(Connection *c) override {}
  bool ms_handle_refused(Connection *c) override {return false;}

  void notify_mdsmap(MDSMap const *mdsmap);
  void notify_health(MDSRank const *mds);

  void handle_mds_beacon(MMDSBeacon *m);
  void send();

  void set_want_state(MDSMap const *mdsmap, MDSMap::DaemonState const newstate);
  MDSMap::DaemonState get_want_state() const;

  /**
   * Send a beacon, and block until the ack is received from the mon
   * or `duration` seconds pass, whichever happens sooner.  Useful
   * for emitting a last message on shutdown.
   */
  void send_and_wait(const double duration);

  bool is_laggy();
  utime_t get_laggy_until() const;

private:
  void _notify_mdsmap(MDSMap const *mdsmap);
  void _send();

  //CephContext *cct;
  mutable Mutex lock;
  MonClient*    monc;
  SafeTimer     timer;

  // Items we duplicate from the MDS to have access under our own lock
  std::string name;
  version_t epoch;
  CompatSet compat;
  mds_rank_t standby_for_rank;
  std::string standby_for_name;
  fs_cluster_id_t standby_for_fscid;
  bool standby_replay;
  MDSMap::DaemonState want_state;

  // Internal beacon state
  version_t last_seq;          // last seq sent to monitor
  std::map<version_t,utime_t>  seq_stamp;    // seq # -> time sent
  utime_t last_acked_stamp;  // last time we sent a beacon that got acked
  utime_t last_mon_reconnect;
  bool was_laggy;
  utime_t laggy_until;

  // Health status to be copied into each beacon message
  MDSHealth health;

  // Ticker
  Context *sender = nullptr;

  version_t awaiting_seq;
  Cond waiting_cond;
};

#endif // BEACON_STATE_H

