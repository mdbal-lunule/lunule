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

#include "XioMessenger.h"
#include "XioConnection.h"
#include "XioMsg.h"


int XioDispatchHook::release_msgs()
{
  XioCompletion *xcmp;
  int r = msg_seq.size();
  cl_flag = true;

  /* queue for release */
  xcmp = static_cast<XioCompletion *>(rsp_pool.alloc(sizeof(XioCompletion)));
  new (xcmp) XioCompletion(xcon, this);
  xcmp->trace = m->trace;

  /* merge with portal traffic */
  xcon->portal->enqueue(xcon, xcmp);

  assert(r);
  return r;
}

/*static*/ size_t XioMsgHdr::get_max_encoded_length() {
  ceph_msg_header _ceph_msg_header;
  ceph_msg_footer _ceph_msg_footer;
  XioMsgHdr hdr (_ceph_msg_header, _ceph_msg_footer, 0 /* features */);
  const std::list<buffer::ptr>& hdr_buffers = hdr.get_bl().buffers();
  assert(hdr_buffers.size() == 1); /* accelio header is small without scatter gather */
  return hdr_buffers.begin()->length();
}

void XioMsg::print_debug(CephContext *cct, const char *tag) const {
  print_xio_msg_hdr(cct, tag, hdr, get_xio_msg());
  print_ceph_msg(cct, tag, m);
}
