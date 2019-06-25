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


#include <functional>
#include <map>
#include <list>
#include <cmath>

#include "common/Formatter.h"
#include "common/OpQueue.h"

#include "dmclock/src/dmclock_server.h"

// the following is done to unclobber _ASSERT_H so it returns to the
// way ceph likes it
#include "include/assert.h"


namespace ceph {

  namespace dmc = crimson::dmclock;

  template <typename T, typename K>
  class mClockQueue : public OpQueue <T, K> {

    using priority_t = unsigned;
    using cost_t = unsigned;

    typedef std::list<std::pair<cost_t, T> > ListPairs;

    static unsigned filter_list_pairs(ListPairs *l,
				      std::function<bool (const T&)> f,
				      std::list<T>* out = nullptr) {
      unsigned ret = 0;
      for (typename ListPairs::iterator i = l->end();
	   i != l->begin();
	   /* no inc */
	) {
	auto next = i;
	--next;
	if (f(next->second)) {
	  ++ret;
	  if (out) out->push_back(next->second);
	  l->erase(next);
	} else {
	  i = next;
	}
      }
      return ret;
    }

    struct SubQueue {
    private:
      typedef std::map<K, ListPairs> Classes;
      // client-class to ordered queue
      Classes q;

      unsigned tokens, max_tokens;
      int64_t size;

      typename Classes::iterator cur;

    public:

      SubQueue(const SubQueue &other)
	: q(other.q),
	  tokens(other.tokens),
	  max_tokens(other.max_tokens),
	  size(other.size),
	  cur(q.begin()) {}

      SubQueue()
	: tokens(0),
	  max_tokens(0),
	  size(0), cur(q.begin()) {}

      void set_max_tokens(unsigned mt) {
	max_tokens = mt;
      }

      unsigned get_max_tokens() const {
	return max_tokens;
      }

      unsigned num_tokens() const {
	return tokens;
      }

      void put_tokens(unsigned t) {
	tokens += t;
	if (tokens > max_tokens) {
	  tokens = max_tokens;
	}
      }

      void take_tokens(unsigned t) {
	if (tokens > t) {
	  tokens -= t;
	} else {
	  tokens = 0;
	}
      }

      void enqueue(K cl, cost_t cost, T item) {
	q[cl].push_back(std::make_pair(cost, item));
	if (cur == q.end())
	  cur = q.begin();
	size++;
      }

      void enqueue_front(K cl, cost_t cost, T item) {
	q[cl].push_front(std::make_pair(cost, item));
	if (cur == q.end())
	  cur = q.begin();
	size++;
      }

      std::pair<cost_t, T> front() const {
	assert(!(q.empty()));
	assert(cur != q.end());
	return cur->second.front();
      }

      void pop_front() {
	assert(!(q.empty()));
	assert(cur != q.end());
	cur->second.pop_front();
	if (cur->second.empty()) {
	  auto i = cur;
	  ++cur;
	  q.erase(i);
	} else {
	  ++cur;
	}
	if (cur == q.end()) {
	  cur = q.begin();
	}
	size--;
      }

      unsigned length() const {
	assert(size >= 0);
	return (unsigned)size;
      }

      bool empty() const {
	return q.empty();
      }

      void remove_by_filter(std::function<bool (const T&)> f) {
	for (typename Classes::iterator i = q.begin();
	     i != q.end();
	     /* no-inc */) {
	  size -= filter_list_pairs(&(i->second), f);
	  if (i->second.empty()) {
	    if (cur == i) {
	      ++cur;
	    }
	    i = q.erase(i);
	  } else {
	    ++i;
	  }
	}
	if (cur == q.end()) cur = q.begin();
      }

      void remove_by_class(K k, std::list<T> *out) {
	typename Classes::iterator i = q.find(k);
	if (i == q.end()) {
	  return;
	}
	size -= i->second.size();
	if (i == cur) {
	  ++cur;
	}
	if (out) {
	  for (auto j = i->second.rbegin(); j != i->second.rend(); ++j) {
	    out->push_front(j->second);
	  }
	}
	q.erase(i);
	if (cur == q.end()) cur = q.begin();
      }

      void dump(ceph::Formatter *f) const {
	f->dump_int("size", size);
	f->dump_int("num_keys", q.size());
      }
    };

    using SubQueues = std::map<priority_t, SubQueue>;

    SubQueues high_queue;

    dmc::PullPriorityQueue<K,T> queue;

    // when enqueue_front is called, rather than try to re-calc tags
    // to put in mClock priority queue, we'll just keep a separate
    // list from which we dequeue items first, and only when it's
    // empty do we use queue.
    std::list<std::pair<K,T>> queue_front;

  public:

    mClockQueue(
      const typename dmc::PullPriorityQueue<K,T>::ClientInfoFunc& info_func) :
      queue(info_func, true)
    {
      // empty
    }

    unsigned length() const override final {
      unsigned total = 0;
      total += queue_front.size();
      total += queue.request_count();
      for (auto i = high_queue.cbegin(); i != high_queue.cend(); ++i) {
	assert(i->second.length());
	total += i->second.length();
      }
      return total;
    }

    // be sure to do things in reverse priority order and push_front
    // to the list so items end up on list in front-to-back priority
    // order
    void remove_by_filter(std::function<bool (const T&)> filter_accum) {
      queue.remove_by_req_filter(filter_accum, true);

      for (auto i = queue_front.rbegin(); i != queue_front.rend(); /* no-inc */) {
	if (filter_accum(i->second)) {
	  i = decltype(i){ queue_front.erase(std::next(i).base()) };
	} else {
	  ++i;
	}
      }

      for (typename SubQueues::iterator i = high_queue.begin();
	   i != high_queue.end();
	   /* no-inc */ ) {
	i->second.remove_by_filter(filter_accum);
	if (i->second.empty()) {
	  i = high_queue.erase(i);
	} else {
	  ++i;
	}
      }
    }

    void remove_by_class(K k, std::list<T> *out = nullptr) override final {
      if (out) {
	queue.remove_by_client(k,
			       true,
			       [&out] (const T& t) { out->push_front(t); });
      } else {
	queue.remove_by_client(k, true);
      }

      for (auto i = queue_front.rbegin(); i != queue_front.rend(); /* no-inc */) {
	if (k == i->first) {
	  if (nullptr != out) out->push_front(i->second);
	  i = decltype(i){ queue_front.erase(std::next(i).base()) };
	} else {
	  ++i;
	}
      }

      for (auto i = high_queue.begin(); i != high_queue.end(); /* no-inc */) {
	i->second.remove_by_class(k, out);
	if (i->second.empty()) {
	  i = high_queue.erase(i);
	} else {
	  ++i;
	}
      }
    }

    void enqueue_strict(K cl, unsigned priority, T item) override final {
      high_queue[priority].enqueue(cl, 0, item);
    }

    void enqueue_strict_front(K cl, unsigned priority, T item) override final {
      high_queue[priority].enqueue_front(cl, 0, item);
    }

    void enqueue(K cl, unsigned priority, unsigned cost, T item) override final {
      // priority is ignored
      queue.add_request(std::move(item), cl, cost);
    }

    void enqueue_front(K cl,
		       unsigned priority,
		       unsigned cost,
		       T item) override final {
      queue_front.emplace_front(std::pair<K,T>(cl, item));
    }

    bool empty() const override final {
      return queue.empty() && high_queue.empty() && queue_front.empty();
    }

    T dequeue() override final {
      assert(!empty());

      if (!(high_queue.empty())) {
	T ret = high_queue.rbegin()->second.front().second;
	high_queue.rbegin()->second.pop_front();
	if (high_queue.rbegin()->second.empty()) {
	  high_queue.erase(high_queue.rbegin()->first);
	}
	return ret;
      }

      if (!queue_front.empty()) {
	T ret = queue_front.front().second;
	queue_front.pop_front();
	return ret;
      }

      auto pr = queue.pull_request();
      assert(pr.is_retn());
      auto& retn = pr.get_retn();
      return *(retn.request);
    }

    void dump(ceph::Formatter *f) const override final {
      f->open_array_section("high_queues");
      for (typename SubQueues::const_iterator p = high_queue.begin();
	   p != high_queue.end();
	   ++p) {
	f->open_object_section("subqueue");
	f->dump_int("priority", p->first);
	p->second.dump(f);
	f->close_section();
      }
      f->close_section();

      f->open_object_section("queue_front");
      f->dump_int("size", queue_front.size());
      f->close_section();

      f->open_object_section("queue");
      f->dump_int("size", queue.request_count());
      f->close_section();
    } // dump
  };

} // namespace ceph
