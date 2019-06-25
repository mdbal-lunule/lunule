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

#ifndef CEPH_MMGRBEACON_H
#define CEPH_MMGRBEACON_H

#include "messages/PaxosServiceMessage.h"
#include "mon/MonCommand.h"

#include "include/types.h"


class MMgrBeacon : public PaxosServiceMessage {

  static const int HEAD_VERSION = 6;
  static const int COMPAT_VERSION = 1;

protected:
  uint64_t gid;
  entity_addr_t server_addr;
  bool available;
  std::string name;
  uuid_d fsid;
  std::set<std::string> available_modules;
  map<string,string> metadata; ///< misc metadata about this osd

  // From active daemon to populate MgrMap::services
  std::map<std::string, std::string> services;

  // Only populated during activation
  std::vector<MonCommand> command_descs;

public:
  MMgrBeacon()
    : PaxosServiceMessage(MSG_MGR_BEACON, 0, HEAD_VERSION, COMPAT_VERSION),
      gid(0), available(false)
  {
  }

  MMgrBeacon(const uuid_d& fsid_, uint64_t gid_, const std::string &name_,
             entity_addr_t server_addr_, bool available_,
	     const std::set<std::string>& module_list,
	     map<string,string>&& metadata)
    : PaxosServiceMessage(MSG_MGR_BEACON, 0, HEAD_VERSION, COMPAT_VERSION),
      gid(gid_), server_addr(server_addr_), available(available_), name(name_),
      fsid(fsid_), available_modules(module_list), metadata(std::move(metadata))
  {
  }

  uint64_t get_gid() const { return gid; }
  entity_addr_t get_server_addr() const { return server_addr; }
  bool get_available() const { return available; }
  const std::string& get_name() const { return name; }
  const uuid_d& get_fsid() const { return fsid; }
  std::set<std::string>& get_available_modules() { return available_modules; }
  const std::map<std::string,std::string>& get_metadata() const {
    return metadata;
  }

  const std::map<std::string,std::string>& get_services() const {
    return services;
  }

  void set_services(const std::map<std::string, std::string> &svcs)
  {
    services = svcs;
  }

  void set_command_descs(const std::vector<MonCommand> &cmds)
  {
    command_descs = cmds;
  }

  const std::vector<MonCommand> &get_command_descs()
  {
    return command_descs;
  }

private:
  ~MMgrBeacon() override {}

public:

  const char *get_type_name() const override { return "mgrbeacon"; }

  void print(ostream& out) const override {
    out << get_type_name() << " mgr." << name << "(" << fsid << ","
	<< gid << ", " << server_addr << ", " << available
	<< ")";
  }

  void encode_payload(uint64_t features) override {
    paxos_encode();
    ::encode(server_addr, payload, features);
    ::encode(gid, payload);
    ::encode(available, payload);
    ::encode(name, payload);
    ::encode(fsid, payload);
    ::encode(available_modules, payload);
    ::encode(command_descs, payload);
    ::encode(metadata, payload);
    ::encode(services, payload);
  }
  void decode_payload() override {
    bufferlist::iterator p = payload.begin();
    paxos_decode(p);
    ::decode(server_addr, p);
    ::decode(gid, p);
    ::decode(available, p);
    ::decode(name, p);
    if (header.version >= 2) {
      ::decode(fsid, p);
    }
    if (header.version >= 3) {
      ::decode(available_modules, p);
    }
    if (header.version >= 4) {
      ::decode(command_descs, p);
    }
    if (header.version >= 5) {
      ::decode(metadata, p);
    }
    if (header.version >= 6) {
      ::decode(services, p);
    }
  }
};

#endif
