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

#ifndef CEPH_MOSDPGPEERNOTIFY_H
#define CEPH_MOSDPGPEERNOTIFY_H

#include "msg/Message.h"

#include "osd/osd_types.h"

/*
 * PGNotify - notify primary of my PGs and versions.
 */

class MOSDPGNotify : public Message {

  static const int HEAD_VERSION = 6;
  static const int COMPAT_VERSION = 2;

  epoch_t epoch = 0;
  /// query_epoch is the epoch of the query being responded to, or
  /// the current epoch if this is not being sent in response to a
  /// query. This allows the recipient to disregard responses to old
  /// queries.
  vector<pair<pg_notify_t,PastIntervals> > pg_list;   // pgid -> version

 public:
  version_t get_epoch() const { return epoch; }
  const vector<pair<pg_notify_t,PastIntervals> >& get_pg_list() const {
    return pg_list;
  }

  MOSDPGNotify()
    : Message(MSG_OSD_PG_NOTIFY, HEAD_VERSION, COMPAT_VERSION) { 
    set_priority(CEPH_MSG_PRIO_HIGH);
  }
  MOSDPGNotify(epoch_t e, vector<pair<pg_notify_t,PastIntervals> >& l)
    : Message(MSG_OSD_PG_NOTIFY, HEAD_VERSION, COMPAT_VERSION),
      epoch(e) {
    pg_list.swap(l);
    set_priority(CEPH_MSG_PRIO_HIGH);
  }
private:
  ~MOSDPGNotify() override {}

public:  
  const char *get_type_name() const override { return "PGnot"; }

  void encode_payload(uint64_t features) override {
    if (HAVE_FEATURE(features, SERVER_LUMINOUS)) {
      header.version = HEAD_VERSION;
    } else {
      // for jewel+kraken compat only
      header.version = 5;

      // Use query_epoch for first entry for backwards compatibility
      epoch_t query_epoch = epoch;
      if (pg_list.size())
	query_epoch = pg_list.begin()->first.query_epoch;
    
      ::encode(epoch, payload);

      // v2 was vector<pg_info_t>
      __u32 n = pg_list.size();
      ::encode(n, payload);
      for (auto p = pg_list.begin();
	   p != pg_list.end();
	   p++)
	::encode(p->first.info, payload);

      ::encode(query_epoch, payload);

      // v3 needs the PastIntervals for each record
      for (auto p = pg_list.begin();
	   p != pg_list.end();
	   p++) {
	p->second.encode_classic(payload);
      }

      // v4 needs epoch_sent, query_epoch
      for (vector<pair<pg_notify_t,PastIntervals> >::iterator p = pg_list.begin();
	   p != pg_list.end();
	   p++)
	::encode(pair<epoch_t, epoch_t>(
		   p->first.epoch_sent, p->first.query_epoch),
		 payload);

      // v5 needs from, to
      for (vector<pair<pg_notify_t, PastIntervals> >::iterator p = pg_list.begin();
	   p != pg_list.end();
	   ++p) {
	::encode(p->first.from, payload);
	::encode(p->first.to, payload);
      }
      return;
    }

    ::encode(epoch, payload);
    ::encode(pg_list, payload);
  }

  void decode_payload() override {
    bufferlist::iterator p = payload.begin();
    if (header.version < 6) {
      // for kraken+jewel compat only
      epoch_t query_epoch;
      ::decode(epoch, p);

      // decode pg_info_t portion of the vector
      __u32 n;
      ::decode(n, p);
      pg_list.resize(n);
      for (unsigned i=0; i<n; i++) {
	::decode(pg_list[i].first.info, p);
      }

      ::decode(query_epoch, p);

      if (header.version >= 3) {
	// get the PastIntervals portion
	for (unsigned i=0; i<n; i++) {
	  pg_list[i].second.decode_classic(p);
	}
      }

      // v3 needs epoch_sent, query_epoch
      for (auto i = pg_list.begin();
	   i != pg_list.end();
	   i++) {
	if (header.version >= 4) {
	  pair<epoch_t, epoch_t> dec;
	  ::decode(dec, p);
	  i->first.epoch_sent = dec.first;
	  i->first.query_epoch = dec.second;
	} else {
	  i->first.epoch_sent = epoch;
	  i->first.query_epoch = query_epoch;
	}
      }

      // v5 needs from and to
      if (header.version >= 5) {
	for (auto i = pg_list.begin();
	     i != pg_list.end();
	     i++) {
	  ::decode(i->first.from, p);
	  ::decode(i->first.to, p);
	}
      }
      return;
    }

    ::decode(epoch, p);
    ::decode(pg_list, p);
  }
  void print(ostream& out) const override {
    out << "pg_notify(";
    for (auto i = pg_list.begin();
         i != pg_list.end();
         ++i) {
      if (i != pg_list.begin())
	out << " ";
      out << i->first << "=" << i->second;
    }
    out << " epoch " << epoch
	<< ")";
  }
};

#endif
