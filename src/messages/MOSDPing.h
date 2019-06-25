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


/**
 * This is used to send pings between daemons (so far, the OSDs) for
 * heartbeat purposes. We include a timestamp and distinguish between
 * outgoing pings and responses to those. If you set the
 * min_message in the constructor, the message will inflate itself
 * to the specified size -- this is good for dealing with network
 * issues with jumbo frames. See http://tracker.ceph.com/issues/20087
 *
 */

#ifndef CEPH_MOSDPING_H
#define CEPH_MOSDPING_H

#include "common/Clock.h"

#include "msg/Message.h"
#include "osd/osd_types.h"


class MOSDPing : public Message {

  static const int HEAD_VERSION = 4;
  static const int COMPAT_VERSION = 4;

 public:
  enum {
    HEARTBEAT = 0,
    START_HEARTBEAT = 1,
    YOU_DIED = 2,
    STOP_HEARTBEAT = 3,
    PING = 4,
    PING_REPLY = 5,
  };
  const char *get_op_name(int op) const {
    switch (op) {
    case HEARTBEAT: return "heartbeat";
    case START_HEARTBEAT: return "start_heartbeat";
    case STOP_HEARTBEAT: return "stop_heartbeat";
    case YOU_DIED: return "you_died";
    case PING: return "ping";
    case PING_REPLY: return "ping_reply";
    default: return "???";
    }
  }

  uuid_d fsid;
  epoch_t map_epoch = 0;
  __u8 op = 0;
  utime_t stamp;
  uint32_t min_message_size;

  MOSDPing(const uuid_d& f, epoch_t e, __u8 o, utime_t s, uint32_t min_message)
    : Message(MSG_OSD_PING, HEAD_VERSION, COMPAT_VERSION),
      fsid(f), map_epoch(e), op(o), stamp(s), min_message_size(min_message)
  { }
  MOSDPing()
    : Message(MSG_OSD_PING, HEAD_VERSION, COMPAT_VERSION), min_message_size(0)
  {}
private:
  ~MOSDPing() override {}

public:
  void decode_payload() override {
    bufferlist::iterator p = payload.begin();
    ::decode(fsid, p);
    ::decode(map_epoch, p);
    if (header.version < 4) {
      osd_peer_stat_t peer_stat;
      epoch_t peer_as_of_epoch;
      ::decode(peer_as_of_epoch, p);
      ::decode(op, p);
      ::decode(peer_stat, p);
    } else {
      ::decode(op, p);
    }
    ::decode(stamp, p);
    if (header.version >= 3) {
      int payload_mid_length = p.get_off();
      uint32_t size;
      ::decode(size, p);
      p.advance(size);
      min_message_size = size + payload_mid_length;
    }
  }
  void encode_payload(uint64_t features) override {
    ::encode(fsid, payload);
    ::encode(map_epoch, payload);
    
    // with luminous, we drop peer_as_of_epoch and peer_stat
    if (HAVE_FEATURE(features, SERVER_LUMINOUS)) {
      header.version = HEAD_VERSION;
      ::encode(op, payload);
    } else {
      epoch_t dummy_epoch = {};
      osd_peer_stat_t dummy_stat = {};
      header.version = 3;
      header.compat_version = 2;
      ::encode(dummy_epoch, payload);
      ::encode(op, payload);   
      ::encode(dummy_stat, payload);
    }
    ::encode(stamp, payload);
    size_t s = 0;
    if (min_message_size > payload.length())
      s = min_message_size - payload.length();
    ::encode((uint32_t)s, payload);
    if (s) {
      // this should be big enough for normal min_message padding sizes. since
      // we are targetting jumbo ethernet frames around 9000 bytes, 16k should
      // be more than sufficient!  the compiler will statically zero this so
      // that at runtime we are only adding a bufferptr reference to it.
      static char zeros[16384] = {};
      while (s > sizeof(zeros)) {
        payload.append(buffer::create_static(sizeof(zeros), zeros));
        s -= sizeof(zeros);
      }
      if (s) {
        payload.append(buffer::create_static(s, zeros));
      }
    }
  }

  const char *get_type_name() const override { return "osd_ping"; }
  void print(ostream& out) const override {
    out << "osd_ping(" << get_op_name(op)
	<< " e" << map_epoch
	<< " stamp " << stamp
	<< ")";
  }
};

#endif
