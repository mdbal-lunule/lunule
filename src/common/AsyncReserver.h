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

#ifndef ASYNC_RESERVER_H
#define ASYNC_RESERVER_H

#include "common/Finisher.h"
#include "common/Formatter.h"

#define rdout(x) lgeneric_subdout(cct,reserver,x)

/**
 * Manages a configurable number of asyncronous reservations.
 *
 * Memory usage is linear with the number of items queued and
 * linear with respect to the total number of priorities used
 * over all time.
 */
template <typename T>
class AsyncReserver {
  CephContext *cct;
  Finisher *f;
  unsigned max_allowed;
  unsigned min_priority;
  Mutex lock;

  struct Reservation {
    T item;
    unsigned prio = 0;
    Context *grant = 0;
    Context *preempt = 0;
    Reservation() {}
    Reservation(T i, unsigned pr, Context *g, Context *p = 0)
      : item(i), prio(pr), grant(g), preempt(p) {}
    void dump(Formatter *f) const {
      f->dump_stream("item") << item;
      f->dump_unsigned("prio", prio);
      f->dump_bool("can_preempt", !!preempt);
    }
    friend ostream& operator<<(ostream& out, const Reservation& r) {
      return out << r.item << "(prio " << r.prio << " grant " << r.grant
		 << " preempt " << r.preempt << ")";
    }
  };

  map<unsigned, list<Reservation>> queues;
  map<T, pair<unsigned, typename list<Reservation>::iterator>> queue_pointers;
  map<T,Reservation> in_progress;
  set<pair<unsigned,T>> preempt_by_prio;  ///< in_progress that can be preempted

  void preempt_one() {
    assert(!preempt_by_prio.empty());
    auto q = in_progress.find(preempt_by_prio.begin()->second);
    assert(q != in_progress.end());
    Reservation victim = q->second;
    rdout(10) << __func__ << " preempt " << victim << dendl;
    f->queue(victim.preempt);
    victim.preempt = nullptr;
    in_progress.erase(q);
    preempt_by_prio.erase(preempt_by_prio.begin());
  }

  void do_queues() {
    rdout(20) << __func__ << ":\n";
    JSONFormatter jf(true);
    jf.open_object_section("queue");
    _dump(&jf);
    jf.close_section();
    jf.flush(*_dout);
    *_dout << dendl;

    // in case min_priority was adjusted up or max_allowed was adjusted down
    while (!preempt_by_prio.empty() &&
	   (in_progress.size() > max_allowed ||
	    preempt_by_prio.begin()->first < min_priority)) {
      preempt_one();
    }

    while (!queues.empty()) {
      // choose highest priority queue
      auto it = queues.end();
      --it;
      assert(!it->second.empty());
      if (it->first < min_priority) {
	break;
      }
      if (in_progress.size() >= max_allowed &&
	  !preempt_by_prio.empty() &&
	  it->first > preempt_by_prio.begin()->first) {
	preempt_one();
      }
      if (in_progress.size() >= max_allowed) {
	break; // no room
      }
      // grant
      Reservation p = it->second.front();
      rdout(10) << __func__ << " grant " << p << dendl;
      queue_pointers.erase(p.item);
      it->second.pop_front();
      if (it->second.empty()) {
	queues.erase(it);
      }
      f->queue(p.grant);
      p.grant = nullptr;
      in_progress[p.item] = p;
      if (p.preempt) {
	preempt_by_prio.insert(make_pair(p.prio, p.item));
      }
    }
  }
public:
  AsyncReserver(
    CephContext *cct,
    Finisher *f,
    unsigned max_allowed,
    unsigned min_priority = 0)
    : cct(cct),
      f(f),
      max_allowed(max_allowed),
      min_priority(min_priority),
      lock("AsyncReserver::lock") {}

  void set_max(unsigned max) {
    Mutex::Locker l(lock);
    max_allowed = max;
    do_queues();
  }

  void set_min_priority(unsigned min) {
    Mutex::Locker l(lock);
    min_priority = min;
    do_queues();
  }

  void dump(Formatter *f) {
    Mutex::Locker l(lock);
    _dump(f);
  }
  void _dump(Formatter *f) {
    f->dump_unsigned("max_allowed", max_allowed);
    f->dump_unsigned("min_priority", min_priority);
    f->open_array_section("queues");
    for (auto& p : queues) {
      f->open_object_section("queue");
      f->dump_unsigned("priority", p.first);
      f->open_array_section("items");
      for (auto& q : p.second) {
	f->dump_object("item", q);
      }
      f->close_section();
      f->close_section();
    }
    f->close_section();
    f->open_array_section("in_progress");
    for (auto& p : in_progress) {
      f->dump_object("item", p.second);
    }
    f->close_section();
  }

  /**
   * Requests a reservation
   *
   * Note, on_reserved may be called following cancel_reservation.  Thus,
   * the callback must be safe in that case.  Callback will be called
   * with no locks held.  cancel_reservation must be called to release the
   * reservation slot.
   */
  void request_reservation(
    T item,                   ///< [in] reservation key
    Context *on_reserved,     ///< [in] callback to be called on reservation
    unsigned prio,            ///< [in] priority
    Context *on_preempt = 0   ///< [in] callback to be called if we are preempted (optional)
    ) {
    Mutex::Locker l(lock);
    Reservation r(item, prio, on_reserved, on_preempt);
    rdout(10) << __func__ << " queue " << r << dendl;
    assert(!queue_pointers.count(item) &&
	   !in_progress.count(item));
    queues[prio].push_back(r);
    queue_pointers.insert(make_pair(item,
				    make_pair(prio,--(queues[prio]).end())));
    do_queues();
  }

  /**
   * Cancels reservation
   *
   * Frees the reservation under key for use.
   * Note, after cancel_reservation, the reservation_callback may or
   * may not still be called. 
   */
  void cancel_reservation(
    T item                   ///< [in] key for reservation to cancel
    ) {
    Mutex::Locker l(lock);
    auto i = queue_pointers.find(item);
    if (i != queue_pointers.end()) {
      unsigned prio = i->second.first;
      const Reservation& r = *i->second.second;
      rdout(10) << __func__ << " cancel " << r << " (was queued)" << dendl;
      delete r.grant;
      delete r.preempt;
      queues[prio].erase(i->second.second);
      if (queues[prio].empty()) {
	queues.erase(prio);
      }
      queue_pointers.erase(i);
    } else {
      auto p = in_progress.find(item);
      if (p != in_progress.end()) {
	rdout(10) << __func__ << " cancel " << p->second
		  << " (was in progress)" << dendl;
	if (p->second.preempt) {
	  preempt_by_prio.erase(make_pair(p->second.prio, p->second.item));
	  delete p->second.preempt;
	}
	in_progress.erase(p);
      } else {
	rdout(10) << __func__ << " cancel " << item << " (not found)" << dendl;
      }
    }
    do_queues();
  }

  /**
   * Has reservations
   *
   * Return true if there are reservations in progress
   */
  bool has_reservation() {
    Mutex::Locker l(lock);
    return !in_progress.empty();
  }
  static const unsigned MAX_PRIORITY = (unsigned)-1;
};

#undef rdout
#endif
