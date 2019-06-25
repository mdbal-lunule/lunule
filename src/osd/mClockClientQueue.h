// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#pragma once

#include <ostream>

#include "boost/variant.hpp"

#include "common/config.h"
#include "common/ceph_context.h"
#include "osd/PGQueueable.h"

#include "common/mClockPriorityQueue.h"


namespace ceph {

  using Request = std::pair<spg_t, PGQueueable>;
  using Client = entity_inst_t;


  // This class exists to bridge the ceph code, which treats the class
  // as the client, and the queue, where the class is
  // osd_op_type_t. So this adpater class will transform calls
  // appropriately.
  class mClockClientQueue : public OpQueue<Request, Client> {

    enum class osd_op_type_t {
      client_op, osd_subop, bg_snaptrim, bg_recovery, bg_scrub };

    using InnerClient = std::pair<entity_inst_t,osd_op_type_t>;

    using queue_t = mClockQueue<Request, InnerClient>;

    queue_t queue;

    struct mclock_op_tags_t {
      crimson::dmclock::ClientInfo client_op;
      crimson::dmclock::ClientInfo osd_subop;
      crimson::dmclock::ClientInfo snaptrim;
      crimson::dmclock::ClientInfo recov;
      crimson::dmclock::ClientInfo scrub;

      mclock_op_tags_t(CephContext *cct);
    };

    static std::unique_ptr<mclock_op_tags_t> mclock_op_tags;

  public:

    mClockClientQueue(CephContext *cct);

    static crimson::dmclock::ClientInfo
    op_class_client_info_f(const InnerClient& client);

    inline unsigned length() const override final {
      return queue.length();
    }

    // Ops of this priority should be deleted immediately
    inline void remove_by_class(Client cl,
				std::list<Request> *out) override final {
      queue.remove_by_filter(
	[&cl, out] (const Request& r) -> bool {
	  if (cl == r.second.get_owner()) {
	    out->push_front(r);
	    return true;
	  } else {
	    return false;
	  }
	});
    }

    void enqueue_strict(Client cl,
			unsigned priority,
			Request item) override final;

    // Enqueue op in the front of the strict queue
    void enqueue_strict_front(Client cl,
			      unsigned priority,
			      Request item) override final;

    // Enqueue op in the back of the regular queue
    void enqueue(Client cl,
		 unsigned priority,
		 unsigned cost,
		 Request item) override final;

    // Enqueue the op in the front of the regular queue
    void enqueue_front(Client cl,
		       unsigned priority,
		       unsigned cost,
		       Request item) override final;

    // Return an op to be dispatch
    Request dequeue() override final;

    // Returns if the queue is empty
    inline bool empty() const override final {
      return queue.empty();
    }

    // Formatted output of the queue
    void dump(ceph::Formatter *f) const override final;

  protected:

    struct pg_queueable_visitor_t : public boost::static_visitor<osd_op_type_t> {
      osd_op_type_t operator()(const OpRequestRef& o) const {
	// don't know if it's a client_op or a
        return osd_op_type_t::client_op;
      }

      osd_op_type_t operator()(const PGSnapTrim& o) const {
        return osd_op_type_t::bg_snaptrim;
      }

      osd_op_type_t operator()(const PGScrub& o) const {
        return osd_op_type_t::bg_scrub;
      }

      osd_op_type_t operator()(const PGRecovery& o) const {
        return osd_op_type_t::bg_recovery;
      }
    }; // class pg_queueable_visitor_t

    static pg_queueable_visitor_t pg_queueable_visitor;

    osd_op_type_t get_osd_op_type(const Request& request);
    InnerClient get_inner_client(const Client& cl, const Request& request);
  }; // class mClockClientAdapter

} // namespace ceph
