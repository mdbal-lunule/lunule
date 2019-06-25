// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <atomic>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include "common/Mutex.h"
#include "common/Cond.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "msg/Dispatcher.h"
#include "msg/msg_types.h"
#include "msg/Message.h"
#include "msg/Messenger.h"
#include "msg/Connection.h"
#include "messages/MPing.h"
#include "messages/MCommand.h"

#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/binomial_distribution.hpp>
#include <gtest/gtest.h>

typedef boost::mt11213b gen_type;

#include "common/dout.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << " ceph_test_msgr "


#if GTEST_HAS_PARAM_TEST

#define CHECK_AND_WAIT_TRUE(expr) do {  \
  int n = 1000;                         \
  while (--n) {                         \
    if (expr)                           \
      break;                            \
    usleep(1000);                       \
  }                                     \
} while(0);

class MessengerTest : public ::testing::TestWithParam<const char*> {
 public:
  Messenger *server_msgr;
  Messenger *client_msgr;

  MessengerTest(): server_msgr(NULL), client_msgr(NULL) {}
  void SetUp() override {
    lderr(g_ceph_context) << __func__ << " start set up " << GetParam() << dendl;
    server_msgr = Messenger::create(g_ceph_context, string(GetParam()), entity_name_t::OSD(0), "server", getpid(), 0);
    client_msgr = Messenger::create(g_ceph_context, string(GetParam()), entity_name_t::CLIENT(-1), "client", getpid(), 0);
    server_msgr->set_default_policy(Messenger::Policy::stateless_server(0));
    client_msgr->set_default_policy(Messenger::Policy::lossy_client(0));
  }
  void TearDown() override {
    ASSERT_EQ(server_msgr->get_dispatch_queue_len(), 0);
    ASSERT_EQ(client_msgr->get_dispatch_queue_len(), 0);
    delete server_msgr;
    delete client_msgr;
  }

};


class FakeDispatcher : public Dispatcher {
 public:
  struct Session : public RefCountedObject {
    atomic<uint64_t> count;
    ConnectionRef con;

    explicit Session(ConnectionRef c): RefCountedObject(g_ceph_context), count(0), con(c) {
    }
    uint64_t get_count() { return count; }
  };

  Mutex lock;
  Cond cond;
  bool is_server;
  bool got_new;
  bool got_remote_reset;
  bool got_connect;
  bool loopback;

  explicit FakeDispatcher(bool s): Dispatcher(g_ceph_context), lock("FakeDispatcher::lock"),
                          is_server(s), got_new(false), got_remote_reset(false),
                          got_connect(false), loopback(false) {}
  bool ms_can_fast_dispatch_any() const override { return true; }
  bool ms_can_fast_dispatch(const Message *m) const override {
    switch (m->get_type()) {
    case CEPH_MSG_PING:
      return true;
    default:
      return false;
    }
  }

  void ms_handle_fast_connect(Connection *con) override {
    lock.Lock();
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
    Session *s = static_cast<Session*>(con->get_priv());
    if (!s) {
      s = new Session(con);
      con->set_priv(s->get());
      lderr(g_ceph_context) << __func__ << " con: " << con << " count: " << s->count << dendl;
    }
    s->put();
    got_connect = true;
    cond.Signal();
    lock.Unlock();
  }
  void ms_handle_fast_accept(Connection *con) override {
    Session *s = static_cast<Session*>(con->get_priv());
    if (!s) {
      s = new Session(con);
      con->set_priv(s->get());
    }
    s->put();
  }
  bool ms_dispatch(Message *m) override {
    Session *s = static_cast<Session*>(m->get_connection()->get_priv());
    if (!s) {
      s = new Session(m->get_connection());
      m->get_connection()->set_priv(s->get());
    }
    s->put();
    s->count++;
    lderr(g_ceph_context) << __func__ << " conn: " << m->get_connection() << " session " << s << " count: " << s->count << dendl;
    if (is_server) {
      reply_message(m);
    }
    Mutex::Locker l(lock);
    got_new = true;
    cond.Signal();
    m->put();
    return true;
  }
  bool ms_handle_reset(Connection *con) override {
    Mutex::Locker l(lock);
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
    Session *s = static_cast<Session*>(con->get_priv());
    if (s) {
      s->con.reset(NULL);  // break con <-> session ref cycle
      con->set_priv(NULL);   // break ref <-> session cycle, if any
      s->put();
    }
    return true;
  }
  void ms_handle_remote_reset(Connection *con) override {
    Mutex::Locker l(lock);
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
    Session *s = static_cast<Session*>(con->get_priv());
    if (s) {
      s->con.reset(NULL);  // break con <-> session ref cycle
      con->set_priv(NULL);   // break ref <-> session cycle, if any
      s->put();
    }
    got_remote_reset = true;
    cond.Signal();
  }
  bool ms_handle_refused(Connection *con) override {
    return false;
  }
  void ms_fast_dispatch(Message *m) override {
    Session *s = static_cast<Session*>(m->get_connection()->get_priv());
    if (!s) {
      s = new Session(m->get_connection());
      m->get_connection()->set_priv(s->get());
    }
    s->put();
    s->count++;
    lderr(g_ceph_context) << __func__ << " conn: " << m->get_connection() << " session " << s << " count: " << s->count << dendl;
    if (is_server) {
      if (loopback)
        assert(m->get_source().is_osd());
      else
        reply_message(m);
    } else if (loopback) {
      assert(m->get_source().is_client());
    }
    m->put();
    Mutex::Locker l(lock);
    got_new = true;
    cond.Signal();
  }

  bool ms_verify_authorizer(Connection *con, int peer_type, int protocol,
                            bufferlist& authorizer, bufferlist& authorizer_reply,
                            bool& isvalid, CryptoKey& session_key) override {
    isvalid = true;
    return true;
  }

  void reply_message(Message *m) {
    MPing *rm = new MPing();
    m->get_connection()->send_message(rm);
  }
};

typedef FakeDispatcher::Session Session;

TEST_P(MessengerTest, SimpleTest) {
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();

  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. simple round trip
  MPing *m = new MPing();
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(conn->is_connected());
  ASSERT_TRUE((static_cast<Session*>(conn->get_priv()))->get_count() == 1);
  ASSERT_TRUE(conn->peer_is_osd());

  // 2. test rebind port
  set<int> avoid_ports;
  for (int i = 0; i < 10 ; i++)
    avoid_ports.insert(server_msgr->get_myaddr().get_port() + i);
  server_msgr->rebind(avoid_ports);
  ASSERT_TRUE(avoid_ports.count(server_msgr->get_myaddr().get_port()) == 0);

  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);

  // 3. test markdown connection
  conn->mark_down();
  ASSERT_FALSE(conn->is_connected());

  // 4. test failed connection
  server_msgr->shutdown();
  server_msgr->wait();

  m = new MPing();
  conn->send_message(m);
  CHECK_AND_WAIT_TRUE(!conn->is_connected());
  ASSERT_FALSE(conn->is_connected());

  // 5. loopback connection
  srv_dispatcher.loopback = true;
  conn = client_msgr->get_loopback_connection();
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  srv_dispatcher.loopback = false;
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  client_msgr->shutdown();
  client_msgr->wait();
  server_msgr->shutdown();
  server_msgr->wait();
}

TEST_P(MessengerTest, NameAddrTest) {
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();

  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  MPing *m = new MPing();
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  ASSERT_TRUE(conn->get_peer_addr() == server_msgr->get_myaddr());
  ConnectionRef server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  // Make should server_conn is the one we already accepted from client,
  // so it means client_msgr has the same addr when server connection has
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, FeatureTest) {
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  uint64_t all_feature_supported, feature_required, feature_supported = 0;
  for (int i = 0; i < 10; i++)
    feature_supported |= 1ULL << i;
  feature_required = feature_supported | 1ULL << 13;
  all_feature_supported = feature_required | 1ULL << 14;

  Messenger::Policy p = server_msgr->get_policy(entity_name_t::TYPE_CLIENT);
  p.features_required = feature_required;
  server_msgr->set_policy(entity_name_t::TYPE_CLIENT, p);
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();

  // 1. Suppose if only support less than required
  p = client_msgr->get_policy(entity_name_t::TYPE_OSD);
  p.features_supported = feature_supported;
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);
  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  MPing *m = new MPing();
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  conn->send_message(m);
  CHECK_AND_WAIT_TRUE(!conn->is_connected());
  // should failed build a connection
  ASSERT_FALSE(conn->is_connected());

  client_msgr->shutdown();
  client_msgr->wait();

  // 2. supported met required
  p = client_msgr->get_policy(entity_name_t::TYPE_OSD);
  p.features_supported = all_feature_supported;
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);
  client_msgr->start();

  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);

  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, TimeoutTest) {
  g_ceph_context->_conf->set_val("ms_tcp_read_timeout", "1");
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();

  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. build the connection
  MPing *m = new MPing();
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(conn->is_connected());
  ASSERT_TRUE((static_cast<Session*>(conn->get_priv()))->get_count() == 1);
  ASSERT_TRUE(conn->peer_is_osd());

  // 2. wait for idle
  usleep(2500*1000);
  ASSERT_FALSE(conn->is_connected());

  server_msgr->shutdown();
  server_msgr->wait();

  client_msgr->shutdown();
  client_msgr->wait();
  g_ceph_context->_conf->set_val("ms_tcp_read_timeout", "900");
}

TEST_P(MessengerTest, StatefulTest) {
  Message *m;
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  Messenger::Policy p = Messenger::Policy::stateful_server(0);
  server_msgr->set_policy(entity_name_t::TYPE_CLIENT, p);
  p = Messenger::Policy::lossless_client(0);
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);

  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();
  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. test for server standby
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  conn->mark_down();
  ASSERT_FALSE(conn->is_connected());
  ConnectionRef server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  // don't lose state
  ASSERT_TRUE(static_cast<Session*>(server_conn->get_priv())->get_count() == 1);

  srv_dispatcher.got_new = false;
  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  {
    Mutex::Locker l(srv_dispatcher.lock);
    while (!srv_dispatcher.got_remote_reset)
      srv_dispatcher.cond.Wait(srv_dispatcher.lock);
  }

  // 2. test for client reconnect
  ASSERT_FALSE(cli_dispatcher.got_remote_reset);
  cli_dispatcher.got_connect = false;
  cli_dispatcher.got_new = false;
  cli_dispatcher.got_remote_reset = false;
  server_conn->mark_down();
  ASSERT_FALSE(server_conn->is_connected());
  // ensure client detect server socket closed
  {
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_remote_reset)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_remote_reset = false;
  }
  {
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_connect)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_connect = false;
  }
  CHECK_AND_WAIT_TRUE(conn->is_connected());
  ASSERT_TRUE(conn->is_connected());

  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    ASSERT_TRUE(conn->is_connected());
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  // resetcheck happen
  ASSERT_EQ(1U, static_cast<Session*>(conn->get_priv())->get_count());
  server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  ASSERT_EQ(1U, static_cast<Session*>(server_conn->get_priv())->get_count());
  cli_dispatcher.got_remote_reset = false;

  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, StatelessTest) {
  Message *m;
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  Messenger::Policy p = Messenger::Policy::stateless_server(0);
  server_msgr->set_policy(entity_name_t::TYPE_CLIENT, p);
  p = Messenger::Policy::lossy_client(0);
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);

  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();
  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. test for server lose state
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  conn->mark_down();
  ASSERT_FALSE(conn->is_connected());

  srv_dispatcher.got_new = false;
  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  ConnectionRef server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  // server lose state
  {
    Mutex::Locker l(srv_dispatcher.lock);
    while (!srv_dispatcher.got_new)
      srv_dispatcher.cond.Wait(srv_dispatcher.lock);
  }
  ASSERT_EQ(1U, static_cast<Session*>(server_conn->get_priv())->get_count());

  // 2. test for client lossy
  server_conn->mark_down();
  ASSERT_FALSE(server_conn->is_connected());
  conn->send_keepalive();
  CHECK_AND_WAIT_TRUE(!conn->is_connected());
  ASSERT_FALSE(conn->is_connected());
  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);

  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, ClientStandbyTest) {
  Message *m;
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  Messenger::Policy p = Messenger::Policy::stateful_server(0);
  server_msgr->set_policy(entity_name_t::TYPE_CLIENT, p);
  p = Messenger::Policy::lossless_peer(0);
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);

  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();
  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. test for client standby, resetcheck
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  ConnectionRef server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  ASSERT_FALSE(cli_dispatcher.got_remote_reset);
  cli_dispatcher.got_connect = false;
  server_conn->mark_down();
  ASSERT_FALSE(server_conn->is_connected());
  // client should be standby
  usleep(300*1000);
  // client should be standby, so we use original connection
  {
    // Try send message to verify got remote reset callback
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    {
      Mutex::Locker l(cli_dispatcher.lock);
      while (!cli_dispatcher.got_remote_reset)
        cli_dispatcher.cond.Wait(cli_dispatcher.lock);
      cli_dispatcher.got_remote_reset = false;
      while (!cli_dispatcher.got_connect)
        cli_dispatcher.cond.Wait(cli_dispatcher.lock);
      cli_dispatcher.got_connect = false;
    }
    CHECK_AND_WAIT_TRUE(conn->is_connected());
    ASSERT_TRUE(conn->is_connected());
    m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(static_cast<Session*>(conn->get_priv())->get_count() == 1);
  server_conn = server_msgr->get_connection(client_msgr->get_myinst());
  ASSERT_TRUE(static_cast<Session*>(server_conn->get_priv())->get_count() == 1);

  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, AuthTest) {
  g_ceph_context->_conf->set_val("auth_cluster_required", "cephx");
  g_ceph_context->_conf->set_val("auth_service_required", "cephx");
  g_ceph_context->_conf->set_val("auth_client_required", "cephx");
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();

  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  // 1. simple auth round trip
  MPing *m = new MPing();
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(conn->is_connected());
  ASSERT_TRUE((static_cast<Session*>(conn->get_priv()))->get_count() == 1);

  // 2. mix auth
  g_ceph_context->_conf->set_val("auth_cluster_required", "none");
  g_ceph_context->_conf->set_val("auth_service_required", "none");
  g_ceph_context->_conf->set_val("auth_client_required", "none");
  conn->mark_down();
  ASSERT_FALSE(conn->is_connected());
  conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    MPing *m = new MPing();
    ASSERT_EQ(conn->send_message(m), 0);
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.Wait(cli_dispatcher.lock);
    cli_dispatcher.got_new = false;
  }
  ASSERT_TRUE(conn->is_connected());
  ASSERT_TRUE((static_cast<Session*>(conn->get_priv()))->get_count() == 1);

  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}

TEST_P(MessengerTest, MessageTest) {
  FakeDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1");
  Messenger::Policy p = Messenger::Policy::stateful_server(0);
  server_msgr->set_policy(entity_name_t::TYPE_CLIENT, p);
  p = Messenger::Policy::lossless_peer(0);
  client_msgr->set_policy(entity_name_t::TYPE_OSD, p);

  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();
  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();


  // 1. A very large "front"(as well as "payload")
  // Because a external message need to invade Messenger::decode_message,
  // here we only use existing message class(MCommand)
  ConnectionRef conn = client_msgr->get_connection(server_msgr->get_myinst());
  {
    uuid_d uuid;
    uuid.generate_random();
    vector<string> cmds;
    string s("abcdefghijklmnopqrstuvwxyz");
    for (int i = 0; i < 1024*30; i++)
      cmds.push_back(s);
    MCommand *m = new MCommand(uuid);
    m->cmd = cmds;
    conn->send_message(m);
    utime_t t;
    t += 1000*1000*500;
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.WaitInterval(cli_dispatcher.lock, t);
    ASSERT_TRUE(cli_dispatcher.got_new);
    cli_dispatcher.got_new = false;
  }

  // 2. A very large "data"
  {
    bufferlist bl;
    string s("abcdefghijklmnopqrstuvwxyz");
    for (int i = 0; i < 1024*30; i++)
      bl.append(s);
    MPing *m = new MPing();
    m->set_data(bl);
    conn->send_message(m);
    utime_t t;
    t += 1000*1000*500;
    Mutex::Locker l(cli_dispatcher.lock);
    while (!cli_dispatcher.got_new)
      cli_dispatcher.cond.WaitInterval(cli_dispatcher.lock, t);
    ASSERT_TRUE(cli_dispatcher.got_new);
    cli_dispatcher.got_new = false;
  }
  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr->wait();
  client_msgr->wait();
}


class SyntheticWorkload;

struct Payload {
  enum Who : uint8_t {
    PING = 0,
    PONG = 1,
  };
  uint8_t who;
  uint64_t seq;
  bufferlist data;

  Payload(Who who, uint64_t seq, const bufferlist& data)
    : who(who), seq(seq), data(data)
  {}
  Payload() = default;
  DENC(Payload, v, p) {
    DENC_START(1, 1, p);
    denc(v.who, p);
    denc(v.seq, p);
    denc(v.data, p);
    DENC_FINISH(p);
  }
};
WRITE_CLASS_DENC(Payload)

ostream& operator<<(ostream& out, const Payload &pl)
{
  return out << "reply=" << pl.who << " i = " << pl.seq;
}

class SyntheticDispatcher : public Dispatcher {
 public:
  Mutex lock;
  Cond cond;
  bool is_server;
  bool got_new;
  bool got_remote_reset;
  bool got_connect;
  map<ConnectionRef, list<uint64_t> > conn_sent;
  map<uint64_t, bufferlist> sent;
  atomic<uint64_t> index;
  SyntheticWorkload *workload;

  SyntheticDispatcher(bool s, SyntheticWorkload *wl):
      Dispatcher(g_ceph_context), lock("SyntheticDispatcher::lock"), is_server(s), got_new(false),
      got_remote_reset(false), got_connect(false), index(0), workload(wl) {}
  bool ms_can_fast_dispatch_any() const override { return true; }
  bool ms_can_fast_dispatch(const Message *m) const override {
    switch (m->get_type()) {
    case CEPH_MSG_PING:
    case MSG_COMMAND:
      return true;
    default:
      return false;
    }
  }

  void ms_handle_fast_connect(Connection *con) override {
    Mutex::Locker l(lock);
    list<uint64_t> c = conn_sent[con];
    for (list<uint64_t>::iterator it = c.begin();
         it != c.end(); ++it)
      sent.erase(*it);
    conn_sent.erase(con);
    got_connect = true;
    cond.Signal();
  }
  void ms_handle_fast_accept(Connection *con) override {
    Mutex::Locker l(lock);
    list<uint64_t> c = conn_sent[con];
    for (list<uint64_t>::iterator it = c.begin();
         it != c.end(); ++it)
      sent.erase(*it);
    conn_sent.erase(con);
    cond.Signal();
  }
  bool ms_dispatch(Message *m) override {
    ceph_abort();
  }
  bool ms_handle_reset(Connection *con) override;
  void ms_handle_remote_reset(Connection *con) override {
    Mutex::Locker l(lock);
    list<uint64_t> c = conn_sent[con];
    for (list<uint64_t>::iterator it = c.begin();
         it != c.end(); ++it)
      sent.erase(*it);
    conn_sent.erase(con);
    got_remote_reset = true;
  }
  bool ms_handle_refused(Connection *con) override {
    return false;
  }
  void ms_fast_dispatch(Message *m) override {
    // MSG_COMMAND is used to disorganize regular message flow
    if (m->get_type() == MSG_COMMAND) {
      m->put();
      return ;
    }

    Payload pl;
    auto p = m->get_data().begin();
    ::decode(pl, p);
    if (pl.who == Payload::PING) {
      lderr(g_ceph_context) << __func__ << " conn=" << m->get_connection() << pl << dendl;
      reply_message(m, pl);
      m->put();
      Mutex::Locker l(lock);
      got_new = true;
      cond.Signal();
    } else {
      Mutex::Locker l(lock);
      if (sent.count(pl.seq)) {
	lderr(g_ceph_context) << __func__ << " conn=" << m->get_connection() << pl << dendl;
	ASSERT_EQ(conn_sent[m->get_connection()].front(), pl.seq);
	ASSERT_TRUE(pl.data.contents_equal(sent[pl.seq]));
	conn_sent[m->get_connection()].pop_front();
	sent.erase(pl.seq);
      }
      m->put();
      got_new = true;
      cond.Signal();
    }
  }

  bool ms_verify_authorizer(Connection *con, int peer_type, int protocol,
                            bufferlist& authorizer, bufferlist& authorizer_reply,
                            bool& isvalid, CryptoKey& session_key) override {
    isvalid = true;
    return true;
  }

  void reply_message(const Message *m, Payload& pl) {
    pl.who = Payload::PONG;
    bufferlist bl;
    ::encode(pl, bl);
    MPing *rm = new MPing();
    rm->set_data(bl);
    m->get_connection()->send_message(rm);
    lderr(g_ceph_context) << __func__ << " conn=" << m->get_connection() << " reply m=" << m << " i=" << pl.seq << dendl;
  }

  void send_message_wrap(ConnectionRef con, const bufferlist& data) {
    Message *m = new MPing();
    Payload pl{Payload::PING, index++, data};
    bufferlist bl;
    ::encode(pl, bl);
    m->set_data(bl);
    if (!con->get_messenger()->get_default_policy().lossy) {
      Mutex::Locker l(lock);
      sent[pl.seq] = pl.data;
      conn_sent[con].push_back(pl.seq);
    }
    lderr(g_ceph_context) << __func__ << " conn=" << con.get() << " send m=" << m << " i=" << pl.seq << dendl;
    ASSERT_EQ(0, con->send_message(m));
  }

  uint64_t get_pending() {
    Mutex::Locker l(lock);
    return sent.size();
  }

  void clear_pending(ConnectionRef con) {
    Mutex::Locker l(lock);

    for (list<uint64_t>::iterator it = conn_sent[con].begin();
         it != conn_sent[con].end(); ++it)
      sent.erase(*it);
    conn_sent.erase(con);
  }

  void print() {
    for (auto && p : conn_sent) {
      if (!p.second.empty()) {
        lderr(g_ceph_context) << __func__ << " " << p.first << " wait " << p.second.size() << dendl;
      }
    }
  }
};


class SyntheticWorkload {
  Mutex lock;
  Cond cond;
  set<Messenger*> available_servers;
  set<Messenger*> available_clients;
  map<ConnectionRef, pair<Messenger*, Messenger*> > available_connections;
  SyntheticDispatcher dispatcher;
  gen_type rng;
  vector<bufferlist> rand_data;

 public:
  static const unsigned max_in_flight = 64;
  static const unsigned max_connections = 128;
  static const unsigned max_message_len = 1024 * 1024 * 4;

  SyntheticWorkload(int servers, int clients, string type, int random_num,
                    Messenger::Policy srv_policy, Messenger::Policy cli_policy):
      lock("SyntheticWorkload::lock"), dispatcher(false, this), rng(time(NULL)) {
    Messenger *msgr;
    int base_port = 16800;
    entity_addr_t bind_addr;
    char addr[64];
    for (int i = 0; i < servers; ++i) {
      msgr = Messenger::create(g_ceph_context, type, entity_name_t::OSD(0),
                               "server", getpid()+i, 0);
      snprintf(addr, sizeof(addr), "127.0.0.1:%d", base_port+i);
      bind_addr.parse(addr);
      msgr->bind(bind_addr);
      msgr->add_dispatcher_head(&dispatcher);

      assert(msgr);
      msgr->set_default_policy(srv_policy);
      available_servers.insert(msgr);
      msgr->start();
    }

    for (int i = 0; i < clients; ++i) {
      msgr = Messenger::create(g_ceph_context, type, entity_name_t::CLIENT(-1),
                               "client", getpid()+i+servers, 0);
      if (cli_policy.standby) {
        snprintf(addr, sizeof(addr), "127.0.0.1:%d", base_port+i+servers);
        bind_addr.parse(addr);
        msgr->bind(bind_addr);
      }
      msgr->add_dispatcher_head(&dispatcher);

      assert(msgr);
      msgr->set_default_policy(cli_policy);
      available_clients.insert(msgr);
      msgr->start();
    }

    for (int i = 0; i < random_num; i++) {
      bufferlist bl;
      boost::uniform_int<> u(32, max_message_len);
      uint64_t value_len = u(rng);
      bufferptr bp(value_len);
      bp.zero();
      for (uint64_t j = 0; j < value_len-sizeof(i); ) {
        memcpy(bp.c_str()+j, &i, sizeof(i));
        j += 4096;
      }

      bl.append(bp);
      rand_data.push_back(bl);
    }
  }

  ConnectionRef _get_random_connection() {
    while (dispatcher.get_pending() > max_in_flight) {
      lock.Unlock();
      usleep(500);
      lock.Lock();
    }
    assert(lock.is_locked());
    boost::uniform_int<> choose(0, available_connections.size() - 1);
    int index = choose(rng);
    map<ConnectionRef, pair<Messenger*, Messenger*> >::iterator i = available_connections.begin();
    for (; index > 0; --index, ++i) ;
    return i->first;
  }

  bool can_create_connection() {
    return available_connections.size() < max_connections;
  }

  void generate_connection() {
    Mutex::Locker l(lock);
    if (!can_create_connection())
      return ;

    Messenger *server, *client;
    {
      boost::uniform_int<> choose(0, available_servers.size() - 1);
      int index = choose(rng);
      set<Messenger*>::iterator i = available_servers.begin();
      for (; index > 0; --index, ++i) ;
      server = *i;
    }
    {
      boost::uniform_int<> choose(0, available_clients.size() - 1);
      int index = choose(rng);
      set<Messenger*>::iterator i = available_clients.begin();
      for (; index > 0; --index, ++i) ;
      client = *i;
    }

    pair<Messenger*, Messenger*> p;
    {
      boost::uniform_int<> choose(0, available_servers.size() - 1);
      if (server->get_default_policy().server) {
        p = make_pair(client, server);
      } else {
        ConnectionRef conn = client->get_connection(server->get_myinst());
        if (available_connections.count(conn) || choose(rng) % 2)
          p = make_pair(client, server);
        else
          p = make_pair(server, client);
      }
    }
    ConnectionRef conn = p.first->get_connection(p.second->get_myinst());
    available_connections[conn] = p;
  }

  void send_message() {
    Mutex::Locker l(lock);
    ConnectionRef conn = _get_random_connection();
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val >= 95) {
      uuid_d uuid;
      uuid.generate_random();
      MCommand *m = new MCommand(uuid);
      vector<string> cmds;
      cmds.push_back("command");
      m->cmd = cmds;
      m->set_priority(200);
      conn->send_message(m);
    } else {
      boost::uniform_int<> u(0, rand_data.size()-1);
      dispatcher.send_message_wrap(conn, rand_data[u(rng)]);
    }
  }

  void drop_connection() {
    Mutex::Locker l(lock);
    if (available_connections.size() < 10)
      return;
    ConnectionRef conn = _get_random_connection();
    dispatcher.clear_pending(conn);
    conn->mark_down();
    pair<Messenger*, Messenger*> &p = available_connections[conn];
    // it's a lossless policy, so we need to mark down each side
    if (!p.first->get_default_policy().server && !p.second->get_default_policy().server) {
      ASSERT_EQ(conn->get_messenger(), p.first);
      ConnectionRef peer = p.second->get_connection(p.first->get_myinst());
      peer->mark_down();
      dispatcher.clear_pending(peer);
      available_connections.erase(peer);
    }
    ASSERT_EQ(available_connections.erase(conn), 1U);
  }

  void print_internal_state(bool detail=false) {
    Mutex::Locker l(lock);
    lderr(g_ceph_context) << "available_connections: " << available_connections.size()
         << " inflight messages: " << dispatcher.get_pending() << dendl;
    if (detail && !available_connections.empty()) {
      dispatcher.print();
    }
  }

  void wait_for_done() {
    int64_t tick_us = 1000 * 100; // 100ms
    int64_t timeout_us = 5 * 60 * 1000 * 1000; // 5 mins
    int i = 0;
    while (dispatcher.get_pending()) {
      usleep(tick_us);
      timeout_us -= tick_us;
      if (i++ % 50 == 0)
        print_internal_state(true);
      if (timeout_us < 0)
        assert(0 == " loop time exceed 5 mins, it looks we stuck into some problems!");
    }
    for (set<Messenger*>::iterator it = available_servers.begin();
         it != available_servers.end(); ++it) {
      (*it)->shutdown();
      (*it)->wait();
      ASSERT_EQ((*it)->get_dispatch_queue_len(), 0);
      delete (*it);
    }
    available_servers.clear();

    for (set<Messenger*>::iterator it = available_clients.begin();
         it != available_clients.end(); ++it) {
      (*it)->shutdown();
      (*it)->wait();
      ASSERT_EQ((*it)->get_dispatch_queue_len(), 0);
      delete (*it);
    }
    available_clients.clear();
  }

  void handle_reset(Connection *con) {
    Mutex::Locker l(lock);
    available_connections.erase(con);
    dispatcher.clear_pending(con);
  }
};

bool SyntheticDispatcher::ms_handle_reset(Connection *con) {
  workload->handle_reset(con);
  return true;
}

TEST_P(MessengerTest, SyntheticStressTest) {
  SyntheticWorkload test_msg(8, 32, GetParam(), 100,
                             Messenger::Policy::stateful_server(0),
                             Messenger::Policy::lossless_client(0));
  for (int i = 0; i < 100; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 5000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 90) {
      test_msg.generate_connection();
    } else if (val > 80) {
      test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 1000 + 500);
    }
  }
  test_msg.wait_for_done();
}

TEST_P(MessengerTest, SyntheticStressTest1) {
  SyntheticWorkload test_msg(16, 32, GetParam(), 100,
                             Messenger::Policy::lossless_peer_reuse(0),
                             Messenger::Policy::lossless_peer_reuse(0));
  for (int i = 0; i < 10; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 10000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 80) {
      test_msg.generate_connection();
    } else if (val > 60) {
      test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 1000 + 500);
    }
  }
  test_msg.wait_for_done();
}


TEST_P(MessengerTest, SyntheticInjectTest) {
  uint64_t dispatch_throttle_bytes = g_ceph_context->_conf->ms_dispatch_throttle_bytes;
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "30");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0.1");
  g_ceph_context->_conf->set_val("ms_dispatch_throttle_bytes", "16777216");
  SyntheticWorkload test_msg(8, 32, GetParam(), 100,
                             Messenger::Policy::stateful_server(0),
                             Messenger::Policy::lossless_client(0));
  for (int i = 0; i < 100; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 90) {
      test_msg.generate_connection();
    } else if (val > 80) {
      test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 500 + 100);
    }
  }
  test_msg.wait_for_done();
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "0");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0");
  g_ceph_context->_conf->set_val(
      "ms_dispatch_throttle_bytes", std::to_string(dispatch_throttle_bytes));
}

TEST_P(MessengerTest, SyntheticInjectTest2) {
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "30");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0.1");
  SyntheticWorkload test_msg(8, 16, GetParam(), 100,
                             Messenger::Policy::lossless_peer_reuse(0),
                             Messenger::Policy::lossless_peer_reuse(0));
  for (int i = 0; i < 100; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 90) {
      test_msg.generate_connection();
    } else if (val > 80) {
      test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 500 + 100);
    }
  }
  test_msg.wait_for_done();
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "0");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0");
}

TEST_P(MessengerTest, SyntheticInjectTest3) {
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "600");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0.1");
  SyntheticWorkload test_msg(8, 16, GetParam(), 100,
                             Messenger::Policy::stateless_server(0),
                             Messenger::Policy::lossy_client(0));
  for (int i = 0; i < 100; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 90) {
      test_msg.generate_connection();
    } else if (val > 80) {
      test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 500 + 100);
    }
  }
  test_msg.wait_for_done();
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "0");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0");
}


TEST_P(MessengerTest, SyntheticInjectTest4) {
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "30");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0.1");
  g_ceph_context->_conf->set_val("ms_inject_delay_probability", "1");
  g_ceph_context->_conf->set_val("ms_inject_delay_type", "client osd", false);
  g_ceph_context->_conf->set_val("ms_inject_delay_max", "5");
  SyntheticWorkload test_msg(16, 32, GetParam(), 100,
                             Messenger::Policy::lossless_peer(0),
                             Messenger::Policy::lossless_peer(0));
  for (int i = 0; i < 100; ++i) {
    if (!(i % 10)) lderr(g_ceph_context) << "seeding connection " << i << dendl;
    test_msg.generate_connection();
  }
  gen_type rng(time(NULL));
  for (int i = 0; i < 1000; ++i) {
    if (!(i % 10)) {
      lderr(g_ceph_context) << "Op " << i << ": " << dendl;
      test_msg.print_internal_state();
    }
    boost::uniform_int<> true_false(0, 99);
    int val = true_false(rng);
    if (val > 95) {
      test_msg.generate_connection();
    } else if (val > 80) {
      // test_msg.drop_connection();
    } else if (val > 10) {
      test_msg.send_message();
    } else {
      usleep(rand() % 500 + 100);
    }
  }
  test_msg.wait_for_done();
  g_ceph_context->_conf->set_val("ms_inject_socket_failures", "0");
  g_ceph_context->_conf->set_val("ms_inject_internal_delays", "0");
  g_ceph_context->_conf->set_val("ms_inject_delay_probability", "0");
  g_ceph_context->_conf->set_val("ms_inject_delay_type", "", false);
  g_ceph_context->_conf->set_val("ms_inject_delay_max", "0");
}


class MarkdownDispatcher : public Dispatcher {
  Mutex lock;
  set<ConnectionRef> conns;
  bool last_mark;
 public:
  std::atomic<uint64_t> count = { 0 };
  explicit MarkdownDispatcher(bool s): Dispatcher(g_ceph_context), lock("MarkdownDispatcher::lock"),
                              last_mark(false) {}
  bool ms_can_fast_dispatch_any() const override { return false; }
  bool ms_can_fast_dispatch(const Message *m) const override {
    switch (m->get_type()) {
    case CEPH_MSG_PING:
      return true;
    default:
      return false;
    }
  }

  void ms_handle_fast_connect(Connection *con) override {
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
    Mutex::Locker l(lock);
    conns.insert(con);
  }
  void ms_handle_fast_accept(Connection *con) override {
    Mutex::Locker l(lock);
    conns.insert(con);
  }
  bool ms_dispatch(Message *m) override {
    lderr(g_ceph_context) << __func__ << " conn: " << m->get_connection() << dendl;
    Mutex::Locker l(lock);
    count++;
    conns.insert(m->get_connection());
    if (conns.size() < 2 && !last_mark) {
      m->put();
      return true;
    }

    last_mark = true;
    usleep(rand() % 500);
    for (set<ConnectionRef>::iterator it = conns.begin(); it != conns.end(); ++it) {
      if ((*it) != m->get_connection().get()) {
        (*it)->mark_down();
        conns.erase(it);
        break;
      }
    }
    if (conns.empty())
      last_mark = false;
    m->put();
    return true;
  }
  bool ms_handle_reset(Connection *con) override {
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
    Mutex::Locker l(lock);
    conns.erase(con);
    usleep(rand() % 500);
    return true;
  }
  void ms_handle_remote_reset(Connection *con) override {
    Mutex::Locker l(lock);
    conns.erase(con);
    lderr(g_ceph_context) << __func__ << " " << con << dendl;
  }
  bool ms_handle_refused(Connection *con) override {
    return false;
  }
  void ms_fast_dispatch(Message *m) override {
    ceph_abort();
  }
  bool ms_verify_authorizer(Connection *con, int peer_type, int protocol,
                            bufferlist& authorizer, bufferlist& authorizer_reply,
                            bool& isvalid, CryptoKey& session_key) override {
    isvalid = true;
    return true;
  }
};


// Markdown with external lock
TEST_P(MessengerTest, MarkdownTest) {
  Messenger *server_msgr2 = Messenger::create(g_ceph_context, string(GetParam()), entity_name_t::OSD(0), "server", getpid(), 0);
  MarkdownDispatcher cli_dispatcher(false), srv_dispatcher(true);
  entity_addr_t bind_addr;
  bind_addr.parse("127.0.0.1:16800");
  server_msgr->bind(bind_addr);
  server_msgr->add_dispatcher_head(&srv_dispatcher);
  server_msgr->start();
  bind_addr.parse("127.0.0.1:16801");
  server_msgr2->bind(bind_addr);
  server_msgr2->add_dispatcher_head(&srv_dispatcher);
  server_msgr2->start();

  client_msgr->add_dispatcher_head(&cli_dispatcher);
  client_msgr->start();

  int i = 1000;
  uint64_t last = 0;
  bool equal = false;
  uint64_t equal_count = 0;
  while (i--) {
    ConnectionRef conn1 = client_msgr->get_connection(server_msgr->get_myinst());
    ConnectionRef conn2 = client_msgr->get_connection(server_msgr2->get_myinst());
    MPing *m = new MPing();
    ASSERT_EQ(conn1->send_message(m), 0);
    m = new MPing();
    ASSERT_EQ(conn2->send_message(m), 0);
    CHECK_AND_WAIT_TRUE(srv_dispatcher.count > last + 1);
    if (srv_dispatcher.count == last) {
      lderr(g_ceph_context) << __func__ << " last is " << last << dendl;
      equal = true;
      equal_count++;
    } else {
      equal = false;
      equal_count = 0;
    }
    last = srv_dispatcher.count;
    if (equal_count)
      usleep(1000*500);
    ASSERT_FALSE(equal && equal_count > 3);
  }
  server_msgr->shutdown();
  client_msgr->shutdown();
  server_msgr2->shutdown();
  server_msgr->wait();
  client_msgr->wait();
  server_msgr2->wait();
  delete server_msgr2;
}

INSTANTIATE_TEST_CASE_P(
  Messenger,
  MessengerTest,
  ::testing::Values(
    "async+posix",
    "simple"
  )
);

#else

// Google Test may not support value-parameterized tests with some
// compilers. If we use conditional compilation to compile out all
// code referring to the gtest_main library, MSVC linker will not link
// that library at all and consequently complain about missing entry
// point defined in that library (fatal error LNK1561: entry point
// must be defined). This dummy test keeps gtest_main linked in.
TEST(DummyTest, ValueParameterizedTestsAreNotSupportedOnThisPlatform) {}

#endif


int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  g_ceph_context->_conf->set_val("auth_cluster_required", "none");
  g_ceph_context->_conf->set_val("auth_service_required", "none");
  g_ceph_context->_conf->set_val("auth_client_required", "none");
  g_ceph_context->_conf->set_val("enable_experimental_unrecoverable_data_corrupting_features", "ms-type-async");
  g_ceph_context->_conf->set_val("ms_die_on_bad_msg", "true");
  g_ceph_context->_conf->set_val("ms_die_on_old_message", "true");
  g_ceph_context->_conf->set_val("ms_max_backoff", "1");
  common_init_finish(g_ceph_context);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/*
 * Local Variables:
 * compile-command: "cd ../.. ; make -j4 ceph_test_msgr && valgrind --tool=memcheck ./ceph_test_msgr"
 * End:
 */
