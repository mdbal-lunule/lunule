// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 John Spray <john.spray@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#pragma once

#include <string>
#include "include/encoding.h"

struct MonCommand {
  std::string cmdstring;
  std::string helpstring;
  std::string module;
  std::string req_perms;
  std::string availability;
  uint64_t flags;

  // MonCommand flags
  static const uint64_t FLAG_NONE       = 0;
  static const uint64_t FLAG_NOFORWARD  = 1 << 0;
  static const uint64_t FLAG_OBSOLETE   = 1 << 1;
  static const uint64_t FLAG_DEPRECATED = 1 << 2;
  static const uint64_t FLAG_MGR        = 1 << 3;

  bool has_flag(uint64_t flag) const { return (flags & flag) != 0; }
  void set_flag(uint64_t flag) { flags |= flag; }
  void unset_flag(uint64_t flag) { flags &= ~flag; }

  void encode(bufferlist &bl) const {
    ENCODE_START(1, 1, bl);
    encode_bare(bl);
    ::encode(flags, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator &bl) {
    DECODE_START(1, bl);
    decode_bare(bl);
    ::decode(flags, bl);
    DECODE_FINISH(bl);
  }

  /**
   * Unversioned encoding for use within encode_array.
   */
  void encode_bare(bufferlist &bl) const {
    ::encode(cmdstring, bl);
    ::encode(helpstring, bl);
    ::encode(module, bl);
    ::encode(req_perms, bl);
    ::encode(availability, bl);
  }
  void decode_bare(bufferlist::iterator &bl) {
    ::decode(cmdstring, bl);
    ::decode(helpstring, bl);
    ::decode(module, bl);
    ::decode(req_perms, bl);
    ::decode(availability, bl);
  }
  bool is_compat(const MonCommand* o) const {
    return cmdstring == o->cmdstring &&
	module == o->module && req_perms == o->req_perms &&
	availability == o->availability;
  }

  bool is_noforward() const {
    return has_flag(MonCommand::FLAG_NOFORWARD);
  }

  bool is_obsolete() const {
    return has_flag(MonCommand::FLAG_OBSOLETE);
  }

  bool is_deprecated() const {
    return has_flag(MonCommand::FLAG_DEPRECATED);
  }

  bool is_mgr() const {
    return has_flag(MonCommand::FLAG_MGR);
  }

  static void encode_array(const MonCommand *cmds, int size, bufferlist &bl) {
    ENCODE_START(2, 1, bl);
    uint16_t s = size;
    ::encode(s, bl);
    for (int i = 0; i < size; ++i) {
      cmds[i].encode_bare(bl);
    }
    for (int i = 0; i < size; i++) {
      ::encode(cmds[i].flags, bl);
    }
    ENCODE_FINISH(bl);
  }
  static void decode_array(MonCommand **cmds, int *size,
                           bufferlist::iterator &bl) {
    DECODE_START(2, bl);
    uint16_t s = 0;
    ::decode(s, bl);
    *size = s;
    *cmds = new MonCommand[*size];
    for (int i = 0; i < *size; ++i) {
      (*cmds)[i].decode_bare(bl);
    }
    if (struct_v >= 2) {
      for (int i = 0; i < *size; i++)
        ::decode((*cmds)[i].flags, bl);
    } else {
      for (int i = 0; i < *size; i++)
        (*cmds)[i].flags = 0;
    }
    DECODE_FINISH(bl);
  }

  // this uses a u16 for the count, so we need a special encoder/decoder.
  static void encode_vector(const std::vector<MonCommand>& cmds,
			    bufferlist &bl) {
    ENCODE_START(2, 1, bl);
    uint16_t s = cmds.size();
    ::encode(s, bl);
    for (unsigned i = 0; i < s; ++i) {
      cmds[i].encode_bare(bl);
    }
    for (unsigned i = 0; i < s; i++) {
      ::encode(cmds[i].flags, bl);
    }
    ENCODE_FINISH(bl);
  }
  static void decode_vector(std::vector<MonCommand> &cmds,
			    bufferlist::iterator &bl) {
    DECODE_START(2, bl);
    uint16_t s = 0;
    ::decode(s, bl);
    cmds.resize(s);
    for (unsigned i = 0; i < s; ++i) {
      cmds[i].decode_bare(bl);
    }
    if (struct_v >= 2) {
      for (unsigned i = 0; i < s; i++)
        ::decode(cmds[i].flags, bl);
    } else {
      for (unsigned i = 0; i < s; i++)
        cmds[i].flags = 0;
    }
    DECODE_FINISH(bl);
  }

  bool requires_perm(char p) const {
    return (req_perms.find(p) != std::string::npos);
  }
};
WRITE_CLASS_ENCODER(MonCommand)
