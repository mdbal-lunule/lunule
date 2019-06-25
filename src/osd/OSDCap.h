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
 * OSDCaps: Hold the capabilities associated with a single authenticated
 * user key. These are specified by text strings of the form
 * "allow r" (which allows reading anything on the OSD)
 * "allow rwx auid foo" (which allows full access to listed auids)
 *  "allow rwx pool foo" (which allows full access to listed pools)
 * "allow *" (which allows full access to EVERYTHING)
 *
 * The full grammar is documented in the parser in OSDCap.cc.
 *
 * The OSD assumes that anyone with * caps is an admin and has full
 * message permissions. This means that only the monitor and the OSDs
 * should get *
 */

#ifndef CEPH_OSDCAP_H
#define CEPH_OSDCAP_H

#include <ostream>
using std::ostream;

#include "include/types.h"
#include "OpRequest.h"

#include <list>
#include <vector>
#include <boost/optional.hpp>

static const __u8 OSD_CAP_R     = (1 << 1);      // read
static const __u8 OSD_CAP_W     = (1 << 2);      // write
static const __u8 OSD_CAP_CLS_R = (1 << 3);      // class read
static const __u8 OSD_CAP_CLS_W = (1 << 4);      // class write
static const __u8 OSD_CAP_X     = (OSD_CAP_CLS_R | OSD_CAP_CLS_W); // execute
static const __u8 OSD_CAP_ANY   = 0xff;          // *

struct osd_rwxa_t {
  __u8 val;

  // cppcheck-suppress noExplicitConstructor
  osd_rwxa_t(__u8 v = 0) : val(v) {}
  osd_rwxa_t& operator=(__u8 v) {
    val = v;
    return *this;
  }
  operator __u8() const {
    return val;
  }
};

ostream& operator<<(ostream& out, const osd_rwxa_t& p);

struct OSDCapSpec {
  osd_rwxa_t allow;
  std::string class_name;
  std::string class_allow;

  OSDCapSpec() : allow(0) {}
  explicit OSDCapSpec(osd_rwxa_t v) : allow(v) {}
  explicit OSDCapSpec(std::string n) : allow(0), class_name(std::move(n)) {}
  OSDCapSpec(std::string n, std::string a) :
    allow(0), class_name(std::move(n)), class_allow(std::move(a)) {}

  bool allow_all() const {
    return allow == OSD_CAP_ANY;
  }
};

ostream& operator<<(ostream& out, const OSDCapSpec& s);

struct OSDCapPoolNamespace {
  std::string pool_name;
  boost::optional<std::string> nspace = boost::none;

  OSDCapPoolNamespace() {
  }
  OSDCapPoolNamespace(const std::string& pool_name,
                      const boost::optional<std::string>& nspace = boost::none)
    : pool_name(pool_name), nspace(nspace) {
  }

  bool is_match(const std::string& pn, const std::string& ns) const;
  bool is_match_all() const;
};

ostream& operator<<(ostream& out, const OSDCapPoolNamespace& pns);


struct OSDCapMatch {
  // auid and pool_name/nspace are mutually exclusive
  int64_t auid = CEPH_AUTH_UID_DEFAULT;
  OSDCapPoolNamespace pool_namespace;
  std::string object_prefix;

  OSDCapMatch() {}
  OSDCapMatch(const OSDCapPoolNamespace& pns) : pool_namespace(pns) {}
  OSDCapMatch(const std::string& pl, const std::string& pre)
    : pool_namespace(pl), object_prefix(pre) {}
  OSDCapMatch(const std::string& pl, const std::string& ns,
              const std::string& pre)
    : pool_namespace(pl, ns), object_prefix(pre) {}
  OSDCapMatch(uint64_t auid, const std::string& pre)
    : auid(auid), object_prefix(pre) {}

  /**
   * check if given request parameters match our constraints
   *
   * @param pool_name pool name
   * @param nspace_name namespace name
   * @param pool_auid pool's auid
   * @param object object name
   * @return true if we match, false otherwise
   */
  bool is_match(const std::string& pool_name, const std::string& nspace_name,
                int64_t pool_auid, const std::string& object) const;
  bool is_match_all() const;
};

ostream& operator<<(ostream& out, const OSDCapMatch& m);


struct OSDCapProfile {
  std::string name;
  OSDCapPoolNamespace pool_namespace;

  OSDCapProfile() {
  }
  OSDCapProfile(const std::string& name,
                const std::string& pool_name,
                const boost::optional<std::string>& nspace = boost::none)
    : name(name), pool_namespace(pool_name, nspace) {
  }

  inline bool is_valid() const {
    return !name.empty();
  }
};

ostream& operator<<(ostream& out, const OSDCapProfile& m);

struct OSDCapGrant {
  OSDCapMatch match;
  OSDCapSpec spec;
  OSDCapProfile profile;

  // explicit grants that a profile grant expands to; populated as
  // needed by expand_profile() and cached here.
  std::list<OSDCapGrant> profile_grants;

  OSDCapGrant() {}
  OSDCapGrant(const OSDCapMatch& m, const OSDCapSpec& s) : match(m), spec(s) {}
  OSDCapGrant(const OSDCapProfile& profile) : profile(profile) {
    expand_profile();
  }

  bool allow_all() const;
  bool is_capable(const string& pool_name, const string& ns, int64_t pool_auid,
                  const string& object, bool op_may_read, bool op_may_write,
                  const std::vector<OpRequest::ClassInfo>& classes,
                  std::vector<bool>* class_allowed) const;

  void expand_profile();
};

ostream& operator<<(ostream& out, const OSDCapGrant& g);


struct OSDCap {
  std::vector<OSDCapGrant> grants;

  OSDCap() {}
  explicit OSDCap(std::vector<OSDCapGrant> g) : grants(std::move(g)) {}

  bool allow_all() const;
  void set_allow_all();
  bool parse(const std::string& str, ostream *err=NULL);

  /**
   * check if we are capable of something
   *
   * This method actually checks a description of a particular operation against
   * what the capability has specified.  Currently that is just rwx with matches
   * against pool, pool auid, and object name prefix.
   *
   * @param pool_name name of the pool we are accessing
   * @param ns name of the namespace we are accessing
   * @param pool_auid owner of the pool we are accessing
   * @param object name of the object we are accessing
   * @param op_may_read whether the operation may need to read
   * @param op_may_write whether the operation may need to write
   * @param classes (class-name, rd, wr, whitelisted-flag) tuples
   * @return true if the operation is allowed, false otherwise
   */
  bool is_capable(const string& pool_name, const string& ns, int64_t pool_auid,
		  const string& object, bool op_may_read, bool op_may_write,
		  const std::vector<OpRequest::ClassInfo>& classes) const;
};

static inline ostream& operator<<(ostream& out, const OSDCap& cap) 
{
  return out << "osdcap" << cap.grants;
}

#endif
