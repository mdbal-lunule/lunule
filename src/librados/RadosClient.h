// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2012 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#ifndef CEPH_LIBRADOS_RADOSCLIENT_H
#define CEPH_LIBRADOS_RADOSCLIENT_H

#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "common/Timer.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "mon/MonClient.h"
#include "mgr/MgrClient.h"
#include "msg/Dispatcher.h"

#include "IoCtxImpl.h"

struct AuthAuthorizer;
struct Context;
class CephContext;
struct Connection;
struct md_config_t;
class Message;
class MLog;
class Messenger;
class AioCompletionImpl;

class librados::RadosClient : public Dispatcher
{
  std::unique_ptr<CephContext,
		  std::function<void(CephContext*)> > cct_deleter;

public:
  using Dispatcher::cct;
  md_config_t *conf;
private:
  enum {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
  } state;

  MonClient monclient;
  MgrClient mgrclient;
  Messenger *messenger;

  uint64_t instance_id;

  bool _dispatch(Message *m);
  bool ms_dispatch(Message *m) override;

  bool ms_get_authorizer(int dest_type, AuthAuthorizer **authorizer, bool force_new) override;
  void ms_handle_connect(Connection *con) override;
  bool ms_handle_reset(Connection *con) override;
  void ms_handle_remote_reset(Connection *con) override;
  bool ms_handle_refused(Connection *con) override;

  Objecter *objecter;

  Mutex lock;
  Cond cond;
  SafeTimer timer;
  int refcnt;

  version_t log_last_version;
  rados_log_callback_t log_cb;
  rados_log_callback2_t log_cb2;
  void *log_cb_arg;
  string log_watch;

  bool service_daemon = false;
  string daemon_name, service_name;
  map<string,string> daemon_metadata;

  int wait_for_osdmap();

public:
  Finisher finisher;

  explicit RadosClient(CephContext *cct_);
  ~RadosClient() override;
  int ping_monitor(string mon_id, string *result);
  int connect();
  void shutdown();

  int watch_flush();
  int async_watch_flush(AioCompletionImpl *c);

  uint64_t get_instance_id();

  int wait_for_latest_osdmap();

  int create_ioctx(const char *name, IoCtxImpl **io);
  int create_ioctx(int64_t, IoCtxImpl **io);

  int get_fsid(std::string *s);
  int64_t lookup_pool(const char *name);
  bool pool_requires_alignment(int64_t pool_id);
  int pool_requires_alignment2(int64_t pool_id, bool *requires);
  uint64_t pool_required_alignment(int64_t pool_id);
  int pool_required_alignment2(int64_t pool_id, uint64_t *alignment);
  int pool_get_auid(uint64_t pool_id, unsigned long long *auid);
  int pool_get_name(uint64_t pool_id, std::string *auid);

  int pool_list(std::list<std::pair<int64_t, string> >& ls);
  int get_pool_stats(std::list<string>& ls, map<string,::pool_stat_t>& result);
  int get_fs_stats(ceph_statfs& result);
  bool get_pool_is_selfmanaged_snaps_mode(const std::string& pool);

  /*
  -1 was set as the default value and monitor will pickup the right crush rule with below order:
    a) osd pool default crush replicated ruleset
    b) the first ruleset in crush ruleset
    c) error out if no value find
  */
  int pool_create(string& name, unsigned long long auid=0, int16_t crush_rule=-1);
  int pool_create_async(string& name, PoolAsyncCompletionImpl *c, unsigned long long auid=0,
			int16_t crush_rule=-1);
  int pool_get_base_tier(int64_t pool_id, int64_t* base_tier);
  int pool_delete(const char *name);

  int pool_delete_async(const char *name, PoolAsyncCompletionImpl *c);

  int blacklist_add(const string& client_address, uint32_t expire_seconds);

  int mon_command(const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  void mon_command_async(const vector<string>& cmd, const bufferlist &inbl,
                         bufferlist *outbl, string *outs, Context *on_finish);
  int mon_command(int rank,
		  const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int mon_command(string name,
		  const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int mgr_command(const vector<string>& cmd, const bufferlist &inbl,
	          bufferlist *outbl, string *outs);
  int osd_command(int osd, vector<string>& cmd, const bufferlist& inbl,
                  bufferlist *poutbl, string *prs);
  int pg_command(pg_t pgid, vector<string>& cmd, const bufferlist& inbl,
	         bufferlist *poutbl, string *prs);

  void handle_log(MLog *m);
  int monitor_log(const string& level, rados_log_callback_t cb,
		  rados_log_callback2_t cb2, void *arg);

  void get();
  bool put();
  void blacklist_self(bool set);

  int service_daemon_register(
    const std::string& service,  ///< service name (e.g., 'rgw')
    const std::string& name,     ///< daemon name (e.g., 'gwfoo')
    const std::map<std::string,std::string>& metadata); ///< static metadata about daemon
  int service_daemon_update_status(
    const std::map<std::string,std::string>& status);

  mon_feature_t get_required_monitor_features() const;
};

#endif
