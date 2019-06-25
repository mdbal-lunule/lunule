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


#ifndef CEPH_MOSDPGCREATE_H
#define CEPH_MOSDPGCREATE_H

#include "msg/Message.h"
#include "osd/osd_types.h"

/*
 * PGCreate - instruct an OSD to create a pg, if it doesn't already exist
 */

struct MOSDPGCreate : public Message {

  const static int HEAD_VERSION = 3;
  const static int COMPAT_VERSION = 3;

  version_t          epoch = 0;
  map<pg_t,pg_create_t> mkpg;
  map<pg_t,utime_t> ctimes;

  MOSDPGCreate()
    : Message(MSG_OSD_PG_CREATE, HEAD_VERSION, COMPAT_VERSION) {}
  MOSDPGCreate(epoch_t e)
    : Message(MSG_OSD_PG_CREATE, HEAD_VERSION, COMPAT_VERSION),
      epoch(e) { }
private:
  ~MOSDPGCreate() override {}

public:  
  const char *get_type_name() const override { return "pg_create"; }

  void encode_payload(uint64_t features) override {
    ::encode(epoch, payload);
    ::encode(mkpg, payload);
    ::encode(ctimes, payload);
  }
  void decode_payload() override {
    bufferlist::iterator p = payload.begin();
    ::decode(epoch, p);
    ::decode(mkpg, p);
    ::decode(ctimes, p);
  }

  void print(ostream& out) const override {
    out << "osd_pg_create(e" << epoch;
    for (map<pg_t,pg_create_t>::const_iterator i = mkpg.begin();
         i != mkpg.end();
         ++i) {
      out << " " << i->first << ":" << i->second.created;
    }
    out << ")";
  }
};

#endif
