// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2009-2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <boost/config/warning_disable.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix.hpp>

#include "OSDCap.h"
#include "common/config.h"
#include "common/debug.h"

using std::ostream;
using std::vector;

ostream& operator<<(ostream& out, const osd_rwxa_t& p)
{
  if (p == OSD_CAP_ANY)
    return out << "*";

  if (p & OSD_CAP_R)
    out << "r";
  if (p & OSD_CAP_W)
    out << "w";
  if ((p & OSD_CAP_X) == OSD_CAP_X) {
    out << "x";
  } else {
    if (p & OSD_CAP_CLS_R)
      out << " class-read";
    if (p & OSD_CAP_CLS_W)
      out << " class-write";
  }
  return out;
}

ostream& operator<<(ostream& out, const OSDCapSpec& s)
{
  if (s.allow)
    return out << s.allow;
  if (s.class_name.length())
    return out << "class '" << s.class_name << "' '" << s.class_allow << "'";
  return out;
}

ostream& operator<<(ostream& out, const OSDCapPoolNamespace& pns)
{
  if (!pns.pool_name.empty()) {
    out << "pool " << pns.pool_name << " ";
  }
  if (pns.nspace) {
    out << "namespace ";
    if (pns.nspace->empty()) {
      out << "\"\"";
    } else {
      out << *pns.nspace;
    }
    out << " ";
  }
  return out;
}

ostream& operator<<(ostream& out, const OSDCapMatch& m)
{
  if (m.auid != -1LL) {
    out << "auid " << m.auid << " ";
  } else {
    out << m.pool_namespace;
  }

  if (m.object_prefix.length()) {
    out << "object_prefix " << m.object_prefix << " ";
  }
  return out;
}

ostream& operator<<(ostream& out, const OSDCapProfile& m)
{
  out << "profile " << m.name;
  out << m.pool_namespace;
  return out;
}

bool OSDCapPoolNamespace::is_match(const std::string& pn,
                                   const std::string& ns) const
{
  if (!pool_name.empty()) {
    if (pool_name != pn) {
      return false;
    }
  }
  if (nspace) {
    if (*nspace != ns) {
      return false;
    }
  }
  return true;
}

bool OSDCapPoolNamespace::is_match_all() const
{
  if (!pool_name.empty())
    return false;
  if (nspace)
    return false;
  return true;
}

bool OSDCapMatch::is_match(const string& pn, const string& ns,
                           int64_t pool_auid, const string& object) const
{
  if (auid >= 0) {
    if (auid != pool_auid)
      return false;
  } else if (!pool_namespace.is_match(pn, ns)) {
    return false;
  }

  if (object_prefix.length()) {
    if (object.find(object_prefix) != 0)
      return false;
  }
  return true;
}

bool OSDCapMatch::is_match_all() const
{
  if (auid >= 0) {
    return false;
  } else if (!pool_namespace.is_match_all()) {
    return false;
  }

  if (object_prefix.length()) {
    return false;
  }
  return true;
}

ostream& operator<<(ostream& out, const OSDCapGrant& g)
{
  out << "grant(";
  if (g.profile.is_valid()) {
    out << g.profile << " [";
    for (auto it = g.profile_grants.cbegin();
         it != g.profile_grants.cend(); ++it) {
      if (it != g.profile_grants.cbegin()) {
        out << ",";
      }
      out << *it;
    }
    out << "]";
  } else {
    out << g.match << g.spec;
  }
  out << ")";
  return out;
}

bool OSDCapGrant::allow_all() const
{
  if (profile.is_valid()) {
    return std::any_of(profile_grants.cbegin(), profile_grants.cend(),
                       [](const OSDCapGrant& grant) {
        return grant.allow_all();
      });
  }

  return (match.is_match_all() && spec.allow_all());
}

bool OSDCapGrant::is_capable(const string& pool_name, const string& ns,
                             int64_t pool_auid, const string& object,
                             bool op_may_read, bool op_may_write,
                             const std::vector<OpRequest::ClassInfo>& classes,
                             std::vector<bool>* class_allowed) const
{
  osd_rwxa_t allow = 0;
  if (profile.is_valid()) {
    return std::any_of(profile_grants.cbegin(), profile_grants.cend(),
                       [&](const OSDCapGrant& grant) {
        return grant.is_capable(pool_name, ns, pool_auid, object, op_may_read,
                                op_may_write, classes, class_allowed);
      });
  } else {
    if (match.is_match(pool_name, ns, pool_auid, object)) {
      allow = allow | spec.allow;
      if ((op_may_read && !(allow & OSD_CAP_R)) ||
          (op_may_write && !(allow & OSD_CAP_W))) {
        return false;
      }
      if (!classes.empty()) {
        // check 'allow *'
        if (spec.allow_all()) {
          return true;
        }

        // compare this grant to each class in the operation
        for (size_t i = 0; i < classes.size(); ++i) {
          // check 'allow class foo'
          if (!spec.class_name.empty() && classes[i].name == spec.class_name) {
            (*class_allowed)[i] = true;
            continue;
          }
          // check 'allow x | class-{rw}': must be on whitelist
          if (!classes[i].whitelisted) {
            continue;
          }
          if ((classes[i].read && !(allow & OSD_CAP_CLS_R)) ||
              (classes[i].write && !(allow & OSD_CAP_CLS_W))) {
            continue;
          }
          (*class_allowed)[i] = true;
        }
        if (!std::all_of(class_allowed->cbegin(), class_allowed->cend(),
              [](bool v) { return v; })) {
          return false;
        }
      }
      return true;
    }
  }
  return false;
}

void OSDCapGrant::expand_profile()
{
  if (profile.name == "read-only") {
    // grants READ-ONLY caps to the OSD
    profile_grants.emplace_back(OSDCapMatch(profile.pool_namespace),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_R)));
    return;
  }
  if (profile.name == "read-write") {
    // grants READ-WRITE caps to the OSD
    profile_grants.emplace_back(OSDCapMatch(profile.pool_namespace),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_R | OSD_CAP_W)));
  }

  if (profile.name == "rbd") {
    // RBD read-write grant
    profile_grants.emplace_back(OSDCapMatch("", "", "rbd_children"),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_CLS_R)));
    profile_grants.emplace_back(OSDCapMatch("", "", "rbd_mirroring"),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_CLS_R)));
    profile_grants.emplace_back(OSDCapMatch(profile.pool_namespace),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_R |
                                                      OSD_CAP_W |
                                                      OSD_CAP_X)));
  }
  if (profile.name == "rbd-read-only") {
    // RBD read-only grant
    profile_grants.emplace_back(OSDCapMatch(profile.pool_namespace),
                                OSDCapSpec(osd_rwxa_t(OSD_CAP_R |
                                                      OSD_CAP_CLS_R)));
  }
}

bool OSDCap::allow_all() const
{
  for (auto &grant : grants) {
    if (grant.allow_all()) {
      return true;
    }
  }
  return false;
}

void OSDCap::set_allow_all()
{
  grants.clear();
  grants.push_back(OSDCapGrant(OSDCapMatch(), OSDCapSpec(OSD_CAP_ANY)));
}

bool OSDCap::is_capable(const string& pool_name, const string& ns,
                        int64_t pool_auid, const string& object,
                        bool op_may_read, bool op_may_write,
			const std::vector<OpRequest::ClassInfo>& classes) const
{
  std::vector<bool> class_allowed(classes.size(), false);
  for (auto &grant : grants) {
    if (grant.is_capable(pool_name, ns, pool_auid, object, op_may_read,
                         op_may_write, classes, &class_allowed)) {
      return true;
    }
  }
  return false;
}


// grammar
namespace qi = boost::spirit::qi;
namespace ascii = boost::spirit::ascii;
namespace phoenix = boost::phoenix;

template <typename Iterator>
struct OSDCapParser : qi::grammar<Iterator, OSDCap()>
{
  OSDCapParser() : OSDCapParser::base_type(osdcap)
  {
    using qi::char_;
    using qi::int_;
    using qi::lexeme;
    using qi::alnum;
    using qi::_val;
    using qi::_1;
    using qi::_2;
    using qi::_3;
    using qi::eps;
    using qi::lit;

    quoted_string %=
      lexeme['"' >> +(char_ - '"') >> '"'] | 
      lexeme['\'' >> +(char_ - '\'') >> '\''];
    equoted_string %=
      lexeme['"' >> *(char_ - '"') >> '"'] |
      lexeme['\'' >> *(char_ - '\'') >> '\''];
    unquoted_word %= +char_("a-zA-Z0-9_.-");
    str %= quoted_string | unquoted_word;
    estr %= equoted_string | unquoted_word;

    spaces = +ascii::space;

    pool_name %= -(spaces >> lit("pool") >> (lit('=') | spaces) >> str);
    nspace %= (spaces >> lit("namespace") >> (lit('=') | spaces) >> estr);

    // match := [pool[=]<poolname> [namespace[=]<namespace>] | auid <123>] [object_prefix <prefix>]
    auid %= (spaces >> lit("auid") >> spaces >> int_);
    object_prefix %= -(spaces >> lit("object_prefix") >> spaces >> str);

    match = (
      (auid >> object_prefix)                 [_val = phoenix::construct<OSDCapMatch>(_1, _2)] |
      (pool_name >> nspace >> object_prefix)  [_val = phoenix::construct<OSDCapMatch>(_1, _2, _3)] |
      (pool_name >> object_prefix)            [_val = phoenix::construct<OSDCapMatch>(_1, _2)]);

    // rwxa := * | [r][w][x] [class-read] [class-write]
    rwxa =
      (spaces >> lit("*")[_val = OSD_CAP_ANY]) |
      ( eps[_val = 0] >>
	(
	 spaces >>
	 ( lit('r')[_val |= OSD_CAP_R] ||
	   lit('w')[_val |= OSD_CAP_W] ||
	   lit('x')[_val |= OSD_CAP_X] )) ||
	( (spaces >> lit("class-read")[_val |= OSD_CAP_CLS_R]) ||
	  (spaces >> lit("class-write")[_val |= OSD_CAP_CLS_W]) ));

    // capspec := * | rwx | class <name> [classcap]
    class_name %= (spaces >> lit("class") >> spaces >> str);
    class_cap %= -(spaces >> str);
    capspec = (
      (rwxa)                    [_val = phoenix::construct<OSDCapSpec>(_1)] |
      (class_name >> class_cap) [_val = phoenix::construct<OSDCapSpec>(_1, _2)]);

    // profile := profile <name> [pool[=]<pool> [namespace[=]<namespace>]]
    profile_name %= (lit("profile") >> (lit('=') | spaces) >> str);
    profile = (
      (profile_name >> pool_name >> nspace) [_val = phoenix::construct<OSDCapProfile>(_1, _2, _3)] |
      (profile_name >> pool_name)           [_val = phoenix::construct<OSDCapProfile>(_1, _2)]);

    // grant := allow match capspec
    grant = (*ascii::blank >>
	     ((lit("allow") >> capspec >> match)  [_val = phoenix::construct<OSDCapGrant>(_2, _1)] |
	      (lit("allow") >> match >> capspec)  [_val = phoenix::construct<OSDCapGrant>(_1, _2)] |
              (profile)                           [_val = phoenix::construct<OSDCapGrant>(_1)]
             ) >> *ascii::blank);
    // osdcap := grant [grant ...]
    grants %= (grant % (lit(';') | lit(',')));
    osdcap = grants  [_val = phoenix::construct<OSDCap>(_1)];
  }
  qi::rule<Iterator> spaces;
  qi::rule<Iterator, unsigned()> rwxa;
  qi::rule<Iterator, string()> quoted_string, equoted_string;
  qi::rule<Iterator, string()> unquoted_word;
  qi::rule<Iterator, string()> str, estr;
  qi::rule<Iterator, int()> auid;
  qi::rule<Iterator, string()> class_name;
  qi::rule<Iterator, string()> class_cap;
  qi::rule<Iterator, OSDCapSpec()> capspec;
  qi::rule<Iterator, string()> pool_name;
  qi::rule<Iterator, string()> nspace;
  qi::rule<Iterator, string()> object_prefix;
  qi::rule<Iterator, OSDCapMatch()> match;
  qi::rule<Iterator, string()> profile_name;
  qi::rule<Iterator, OSDCapProfile()> profile;
  qi::rule<Iterator, OSDCapGrant()> grant;
  qi::rule<Iterator, std::vector<OSDCapGrant>()> grants;
  qi::rule<Iterator, OSDCap()> osdcap;
};

bool OSDCap::parse(const string& str, ostream *err)
{
  OSDCapParser<string::const_iterator> g;
  string::const_iterator iter = str.begin();
  string::const_iterator end = str.end();

  bool r = qi::phrase_parse(iter, end, g, ascii::space, *this);
  if (r && iter == end)
    return true;

  // Make sure no grants are kept after parsing failed!
  grants.clear();

  if (err)
    *err << "osdcap parse failed, stopped at '" << std::string(iter, end)
	 << "' of '" << str << "'\n";

  return false; 
}

