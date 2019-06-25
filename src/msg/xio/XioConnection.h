// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Portions Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef XIO_CONNECTION_H
#define XIO_CONNECTION_H

#include <atomic>

#include <boost/intrusive/avl_set.hpp>
#include <boost/intrusive/list.hpp>

extern "C" {
#include "libxio.h"
}

#include "XioInSeq.h"
#include "XioSubmit.h"
#include "msg/Connection.h"
#include "msg/Messenger.h"
#include "auth/AuthSessionHandler.h"

#define XIO_ALL_FEATURES (CEPH_FEATURES_ALL)


#define XIO_NOP_TAG_MARKDOWN 0x0001

namespace bi = boost::intrusive;

class XioPortal;
class XioMessenger;
class XioSend;

class XioConnection : public Connection
{
public:
  enum type { ACTIVE, PASSIVE };

  enum class session_states : unsigned {
    INIT = 0,
    START,
    UP,
    FLOW_CONTROLLED,
    DISCONNECTED,
    DELETED,
    BARRIER
  };

  enum class session_startup_states : unsigned {
    IDLE = 0,
    CONNECTING,
    ACCEPTING,
    READY,
    FAIL
  };

private:
  XioConnection::type xio_conn_type;
  XioPortal *portal;
  std::atomic<bool> connected = { false };
  entity_inst_t peer;
  struct xio_session *session;
  struct xio_connection	*conn;
  pthread_spinlock_t sp;
  std::atomic<int64_t> send = { 0 };
  std::atomic<int64_t> recv = { 0 };
  uint32_t n_reqs; // Accelio-initiated reqs in progress (!counting partials)
  uint32_t magic;
  uint32_t special_handling;
  uint64_t scount;
  uint32_t send_ctr;
  int q_high_mark;
  int q_low_mark;

  struct lifecycle {
    // different from Pipe states?
    enum lf_state {
      INIT,
      LOCAL_DISCON,
      REMOTE_DISCON,
      RECONNECTING,
      UP,
      DEAD } state;

    /* XXX */
    uint32_t reconnects;
    uint32_t connect_seq, peer_global_seq;
    uint64_t in_seq, out_seq_acked; // atomic<uint64_t>, got receipt
    std::atomic<int64_t> out_seq = { 0 }; 

    lifecycle() : state(lifecycle::INIT), reconnects(0), connect_seq(0),
		  peer_global_seq(0), in_seq(0), out_seq_acked(0)
		  {}

    void set_in_seq(uint64_t seq) {
      in_seq = seq;
    }

    uint64_t next_out_seq() {
      return ++out_seq;
    }

  } state;

  /* batching */
  XioInSeq in_seq;

  class CState
  {
  public:
    static const int FLAG_NONE = 0x0000;
    static const int FLAG_BAD_AUTH = 0x0001;
    static const int FLAG_MAPPED = 0x0002;
    static const int FLAG_RESET = 0x0004;

    static const int OP_FLAG_NONE = 0x0000;
    static const int OP_FLAG_LOCKED = 0x0001;
    static const int OP_FLAG_LRU = 0x0002;

    uint64_t features;
    Messenger::Policy policy;

    CryptoKey session_key;
    ceph::shared_ptr<AuthSessionHandler> session_security;
    AuthAuthorizer *authorizer;
    XioConnection *xcon;
    uint32_t protocol_version;

    std::atomic<session_states> session_state = { 0 };
    std::atomic<session_startup_state> startup_state = { 0 };

    uint32_t reconnects;
    uint32_t connect_seq, global_seq, peer_global_seq;
    uint64_t in_seq, out_seq_acked; // atomic<uint64_t>, got receipt
    std::atomic<uint64_t> out_seq = { 0 }; 

    uint32_t flags;

    explicit CState(XioConnection* _xcon)
      : features(0),
	authorizer(NULL),
	xcon(_xcon),
	protocol_version(0),
	session_state(INIT),
	startup_state(IDLE),
	reconnects(0),
	connect_seq(0),
	global_seq(0),
	peer_global_seq(0),
	in_seq(0),
	out_seq_acked(0),
	flags(FLAG_NONE) {}

    uint64_t get_session_state() {
      return session_state;
    }

    uint64_t get_startup_state() {
      return startup_state;
    }

    void set_in_seq(uint64_t seq) {
      in_seq = seq;
    }

    uint64_t next_out_seq() {
      return ++out_seq;
    };

    // state machine
    int init_state();
    int next_state(Message* m);
#if 0 // future (session startup)
    int msg_connect(MConnect *m);
    int msg_connect_reply(MConnectReply *m);
    int msg_connect_reply(MConnectAuthReply *m);
    int msg_connect_auth(MConnectAuth *m);
    int msg_connect_auth_reply(MConnectAuthReply *m);
#endif
    int state_up_ready(uint32_t flags);
    int state_flow_controlled(uint32_t flags);
    int state_discon();
    int state_fail(Message* m, uint32_t flags);

  } cstate; /* CState */

  // message submission queue
  struct SendQ {
    bool keepalive;
    bool ack;
    utime_t ack_time;
    Message::Queue mqueue; // deferred
    XioSubmit::Queue requeue;

    SendQ():keepalive(false), ack(false){}
  } outgoing;

  // conns_entity_map comparison functor
  struct EntityComp
  {
    // for internal ordering
    bool operator()(const XioConnection &lhs,  const XioConnection &rhs) const
      {  return lhs.get_peer() < rhs.get_peer(); }

    // for external search by entity_inst_t(peer)
    bool operator()(const entity_inst_t &peer, const XioConnection &c) const
      {  return peer < c.get_peer(); }

    bool operator()(const XioConnection &c, const entity_inst_t &peer) const
      {  return c.get_peer() < peer;  }
  };

  bi::list_member_hook<> conns_hook;
  bi::avl_set_member_hook<> conns_entity_map_hook;

  typedef bi::list< XioConnection,
		    bi::member_hook<XioConnection, bi::list_member_hook<>,
				    &XioConnection::conns_hook > > ConnList;

  typedef bi::member_hook<XioConnection, bi::avl_set_member_hook<>,
			  &XioConnection::conns_entity_map_hook> EntityHook;

  typedef bi::avl_set< XioConnection, EntityHook,
		       bi::compare<EntityComp> > EntitySet;

  friend class XioPortal;
  friend class XioMessenger;
  friend class XioDispatchHook;
  friend class XioMarkDownHook;
  friend class XioSend;

  int on_disconnect_event() {
    connected = false;
    pthread_spin_lock(&sp);
    discard_out_queues(CState::OP_FLAG_LOCKED);
    pthread_spin_unlock(&sp);
    return 0;
  }

  int on_teardown_event() {
    pthread_spin_lock(&sp);
    if (conn)
      xio_connection_destroy(conn);
    conn = NULL;
    pthread_spin_unlock(&sp);
    this->put();
    return 0;
  }

  int xio_qdepth_high_mark() {
    return q_high_mark;
  }

  int xio_qdepth_low_mark() {
    return q_low_mark;
  }

public:
  XioConnection(XioMessenger *m, XioConnection::type _type,
		const entity_inst_t& peer);

  ~XioConnection() {
    if (conn)
      xio_connection_destroy(conn);
  }
  ostream& conn_prefix(std::ostream *_dout);

  bool is_connected() override { return connected; }

  int send_message(Message *m) override;
  void send_keepalive() override {send_keepalive_or_ack();}
  void send_keepalive_or_ack(bool ack = false, const utime_t *tp = nullptr);
  void mark_down() override;
  int _mark_down(uint32_t flags);
  void mark_disposable() override;
  int _mark_disposable(uint32_t flags);

  const entity_inst_t& get_peer() const { return peer; }

  XioConnection* get() {
#if 0
    cout << "XioConnection::get " << this << " " << nref.load() << std::endl;
#endif
    RefCountedObject::get();
    return this;
  }

  void put() {
    RefCountedObject::put();
#if 0
    cout << "XioConnection::put " << this << " " << nref.load() << std::endl;
#endif
  }

  void disconnect() {
    if (is_connected()) {
      connected = false;
      xio_disconnect(conn); // normal teardown will clean up conn
    }
  }

  uint32_t get_magic() { return magic; }
  void set_magic(int _magic) { magic = _magic; }
  uint32_t get_special_handling() { return special_handling; }
  void set_special_handling(int n) { special_handling = n; }
  uint64_t get_scount() { return scount; }

  int passive_setup(); /* XXX */

  int handle_data_msg(struct xio_session *session, struct xio_msg *msg,
		 int more_in_batch, void *cb_user_context);
  int on_msg(struct xio_session *session, struct xio_msg *msg,
		 int more_in_batch, void *cb_user_context);
  int on_ow_msg_send_complete(struct xio_session *session, struct xio_msg *msg,
			      void *conn_user_context);
  int on_msg_error(struct xio_session *session, enum xio_status error,
		   struct xio_msg  *msg, void *conn_user_context);
  void msg_send_fail(XioSend *xsend, int code);
  void msg_release_fail(struct xio_msg *msg, int code);
private:
  void send_keepalive_or_ack_internal(bool ack = false, const utime_t *tp = nullptr);
  int flush_out_queues(uint32_t flags);
  int discard_out_queues(uint32_t flags);
  int adjust_clru(uint32_t flags);
};

typedef boost::intrusive_ptr<XioConnection> XioConnectionRef;

class XioLoopbackConnection : public Connection
{
private:
  std::atomic<uint64_t> seq = { 0 };
public:
  explicit XioLoopbackConnection(Messenger *m) : Connection(m->cct, m)
    {
      const entity_inst_t& m_inst = m->get_myinst();
      peer_addr = m_inst.addr;
      peer_type = m_inst.name.type();
      set_features(XIO_ALL_FEATURES); /* XXXX set to ours */
    }

  XioLoopbackConnection* get() {
    return static_cast<XioLoopbackConnection*>(RefCountedObject::get());
  }

  bool is_connected() override { return true; }

  int send_message(Message *m) override;
  void send_keepalive() override;
  void mark_down() override {}
  void mark_disposable() override {}

  uint64_t get_seq() {
    return seq;
  }

  uint64_t next_seq() {
    return ++seq;
  }
};

typedef boost::intrusive_ptr<XioLoopbackConnection> XioLoopbackConnectionRef;

#endif /* XIO_CONNECTION_H */
