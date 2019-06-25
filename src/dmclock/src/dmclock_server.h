// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Copyright (C) 2017 Red Hat Inc.
 */


#pragma once

/* COMPILATION OPTIONS
 *
 * By default we include an optimization over the originally published
 * dmclock algorithm using not the values of rho and delta that were
 * sent in with a request but instead the most recent rho and delta
 * values from the requests's client. To restore the algorithm's
 * original behavior, define DO_NOT_DELAY_TAG_CALC (i.e., compiler
 * argument -DDO_NOT_DELAY_TAG_CALC).
 *
 * The prop_heap does not seem to be necessary. The only thing it
 * would help with is quickly finding the mininum proportion/prioity
 * when an idle client became active. To have the code maintain the
 * proportional heap, define USE_PROP_HEAP (i.e., compiler argument
 * -DUSE_PROP_HEAP).
 */

#include <assert.h>

#include <cmath>
#include <memory>
#include <map>
#include <deque>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <iostream>
#include <sstream>
#include <limits>

#include <boost/variant.hpp>

#include "indirect_intrusive_heap.h"
#include "run_every.h"
#include "dmclock_util.h"
#include "dmclock_recs.h"

#ifdef PROFILE
#include "profile.h"
#endif


namespace crimson {

  namespace dmclock {

    namespace c = crimson;

    constexpr double max_tag = std::numeric_limits<double>::is_iec559 ?
      std::numeric_limits<double>::infinity() :
      std::numeric_limits<double>::max();
    constexpr double min_tag = std::numeric_limits<double>::is_iec559 ?
      -std::numeric_limits<double>::infinity() :
      std::numeric_limits<double>::lowest();
    constexpr uint tag_modulo = 1000000;

    struct ClientInfo {
      const double reservation;  // minimum
      const double weight;       // proportional
      const double limit;        // maximum

      // multiplicative inverses of above, which we use in calculations
      // and don't want to recalculate repeatedly
      const double reservation_inv;
      const double weight_inv;
      const double limit_inv;

      // order parameters -- min, "normal", max
      ClientInfo(double _reservation, double _weight, double _limit) :
	reservation(_reservation),
	weight(_weight),
	limit(_limit),
	reservation_inv(0.0 == reservation ? 0.0 : 1.0 / reservation),
	weight_inv(     0.0 == weight      ? 0.0 : 1.0 / weight),
	limit_inv(      0.0 == limit       ? 0.0 : 1.0 / limit)
      {
	// empty
      }


      friend std::ostream& operator<<(std::ostream& out,
				      const ClientInfo& client) {
	out <<
	  "{ ClientInfo:: r:" << client.reservation <<
	  " w:" << std::fixed << client.weight <<
	  " l:" << std::fixed << client.limit <<
	  " 1/r:" << std::fixed << client.reservation_inv <<
	  " 1/w:" << std::fixed << client.weight_inv <<
	  " 1/l:" << std::fixed << client.limit_inv <<
	  " }";
	return out;
      }
    }; // class ClientInfo


    struct RequestTag {
      double reservation;
      double proportion;
      double limit;
      bool   ready; // true when within limit
#ifndef DO_NOT_DELAY_TAG_CALC
      Time   arrival;
#endif

      RequestTag(const RequestTag& prev_tag,
		 const ClientInfo& client,
		 const uint32_t delta,
		 const uint32_t rho,
		 const Time time,
		 const double cost = 0.0) :
	reservation(cost + tag_calc(time,
				    prev_tag.reservation,
				    client.reservation_inv,
				    rho,
				    true)),
	proportion(tag_calc(time,
			    prev_tag.proportion,
			    client.weight_inv,
			    delta,
			    true)),
	limit(tag_calc(time,
		       prev_tag.limit,
		       client.limit_inv,
		       delta,
		       false)),
	ready(false)
#ifndef DO_NOT_DELAY_TAG_CALC
	, arrival(time)
#endif
      {
	assert(reservation < max_tag || proportion < max_tag);
      }

      RequestTag(const RequestTag& prev_tag,
		 const ClientInfo& client,
		 const ReqParams req_params,
		 const Time time,
		 const double cost = 0.0) :
	RequestTag(prev_tag, client, req_params.delta, req_params.rho, time, cost)
      { /* empty */ }

      RequestTag(double _res, double _prop, double _lim, const Time _arrival) :
	reservation(_res),
	proportion(_prop),
	limit(_lim),
	ready(false)
#ifndef DO_NOT_DELAY_TAG_CALC
	, arrival(_arrival)
#endif
      {
	assert(reservation < max_tag || proportion < max_tag);
      }

      RequestTag(const RequestTag& other) :
	reservation(other.reservation),
	proportion(other.proportion),
	limit(other.limit),
	ready(other.ready)
#ifndef DO_NOT_DELAY_TAG_CALC
	, arrival(other.arrival)
#endif
      {
	// empty
      }

      static std::string format_tag_change(double before, double after) {
	if (before == after) {
	  return std::string("same");
	} else {
	  std::stringstream ss;
	  ss << format_tag(before) << "=>" << format_tag(after);
	  return ss.str();
	}
      }

      static std::string format_tag(double value) {
	if (max_tag == value) {
	  return std::string("max");
	} else if (min_tag == value) {
	  return std::string("min");
	} else {
	  return format_time(value, tag_modulo);
	}
      }

    private:

      static double tag_calc(const Time time,
			     double prev,
			     double increment,
			     uint32_t dist_req_val,
			     bool extreme_is_high) {
	if (0.0 == increment) {
	  return extreme_is_high ? max_tag : min_tag;
	} else {
	  if (0 != dist_req_val) {
	    increment *= dist_req_val;
	  }
	  return std::max(time, prev + increment);
	}
      }

      friend std::ostream& operator<<(std::ostream& out,
				      const RequestTag& tag) {
	out <<
	  "{ RequestTag:: ready:" << (tag.ready ? "true" : "false") <<
	  " r:" << format_tag(tag.reservation) <<
	  " p:" << format_tag(tag.proportion) <<
	  " l:" << format_tag(tag.limit) <<
#if 0 // try to resolve this to make sure Time is operator<<'able.
#ifndef DO_NOT_DELAY_TAG_CALC
	  " arrival:" << tag.arrival <<
#endif
#endif
	  " }";
	return out;
      }
    }; // class RequestTag


    // C is client identifier type, R is request type, B is heap
    // branching factor
    template<typename C, typename R, uint B>
    class PriorityQueueBase {
      // we don't want to include gtest.h just for FRIEND_TEST
      friend class dmclock_server_client_idle_erase_Test;

    public:

      using RequestRef = std::unique_ptr<R>;

    protected:

      using TimePoint = decltype(std::chrono::steady_clock::now());
      using Duration = std::chrono::milliseconds;
      using MarkPoint = std::pair<TimePoint,Counter>;

      enum class ReadyOption {ignore, lowers, raises};

      // forward decl for friend decls
      template<double RequestTag::*, ReadyOption, bool>
      struct ClientCompare;

      class ClientReq {
	friend PriorityQueueBase;

	RequestTag tag;
	C          client_id;
	RequestRef request;

      public:

	ClientReq(const RequestTag& _tag,
		  const C&          _client_id,
		  RequestRef&&      _request) :
	  tag(_tag),
	  client_id(_client_id),
	  request(std::move(_request))
	{
	  // empty
	}

	friend std::ostream& operator<<(std::ostream& out, const ClientReq& c) {
	  out << "{ ClientReq:: tag:" << c.tag << " client:" <<
	    c.client_id << " }";
	  return out;
	}
      }; // class ClientReq

    public:

      // NOTE: ClientRec is in the "public" section for compatibility
      // with g++ 4.8.4, which complains if it's not. By g++ 6.3.1
      // ClientRec could be "protected" with no issue. [See comments
      // associated with function submit_top_request.]
      class ClientRec {
	friend PriorityQueueBase<C,R,B>;

	C                     client;
	RequestTag            prev_tag;
	std::deque<ClientReq> requests;

	// amount added from the proportion tag as a result of
	// an idle client becoming unidle
	double                prop_delta = 0.0;

	c::IndIntruHeapData   reserv_heap_data;
	c::IndIntruHeapData   lim_heap_data;
	c::IndIntruHeapData   ready_heap_data;
#if USE_PROP_HEAP
	c::IndIntruHeapData   prop_heap_data;
#endif

      public:

	ClientInfo            info;
	bool                  idle;
	Counter               last_tick;
	uint32_t              cur_rho;
	uint32_t              cur_delta;

	ClientRec(C _client,
		  const ClientInfo& _info,
		  Counter current_tick) :
	  client(_client),
	  prev_tag(0.0, 0.0, 0.0, TimeZero),
	  info(_info),
	  idle(true),
	  last_tick(current_tick),
	  cur_rho(1),
	  cur_delta(1)
	{
	  // empty
	}

	inline const RequestTag& get_req_tag() const {
	  return prev_tag;
	}

	static inline void assign_unpinned_tag(double& lhs, const double rhs) {
	  if (rhs != max_tag && rhs != min_tag) {
	    lhs = rhs;
	  }
	}

	inline void update_req_tag(const RequestTag& _prev,
				   const Counter& _tick) {
	  assign_unpinned_tag(prev_tag.reservation, _prev.reservation);
	  assign_unpinned_tag(prev_tag.limit, _prev.limit);
	  assign_unpinned_tag(prev_tag.proportion, _prev.proportion);
	  last_tick = _tick;
	}

	inline void add_request(const RequestTag& tag,
				const C&          client_id,
				RequestRef&&      request) {
	  requests.emplace_back(ClientReq(tag, client_id, std::move(request)));
	}

	inline const ClientReq& next_request() const {
	  return requests.front();
	}

	inline ClientReq& next_request() {
	  return requests.front();
	}

	inline void pop_request() {
	  requests.pop_front();
	}

	inline bool has_request() const {
	  return !requests.empty();
	}

	inline size_t request_count() const {
	  return requests.size();
	}

	// NB: because a deque is the underlying structure, this
	// operation might be expensive
	bool remove_by_req_filter_fw(std::function<bool(R&&)> filter_accum) {
	  bool any_removed = false;
	  for (auto i = requests.begin();
	       i != requests.end();
	       /* no inc */) {
	    if (filter_accum(std::move(*i->request))) {
	      any_removed = true;
	      i = requests.erase(i);
	    } else {
	      ++i;
	    }
	  }
	  return any_removed;
	}

	// NB: because a deque is the underlying structure, this
	// operation might be expensive
	bool remove_by_req_filter_bw(std::function<bool(R&&)> filter_accum) {
	  bool any_removed = false;
	  for (auto i = requests.rbegin();
	       i != requests.rend();
	       /* no inc */) {
	    if (filter_accum(std::move(*i->request))) {
	      any_removed = true;
	      i = decltype(i){ requests.erase(std::next(i).base()) };
	    } else {
	      ++i;
	    }
	  }
	  return any_removed;
	}

	inline bool
	remove_by_req_filter(std::function<bool(R&&)> filter_accum,
			     bool visit_backwards) {
	  if (visit_backwards) {
	    return remove_by_req_filter_bw(filter_accum);
	  } else {
	    return remove_by_req_filter_fw(filter_accum);
	  }
	}

	friend std::ostream&
	operator<<(std::ostream& out,
		   const typename PriorityQueueBase<C,R,B>::ClientRec& e) {
	  out << "{ ClientRec::" <<
	    " client:" << e.client <<
	    " prev_tag:" << e.prev_tag <<
	    " req_count:" << e.requests.size() <<
	    " top_req:";
	  if (e.has_request()) {
	    out << e.next_request();
	  } else {
	    out << "none";
	  }
	  out << " }";

	  return out;
	}
      }; // class ClientRec

      using ClientRecRef = std::shared_ptr<ClientRec>;

      // when we try to get the next request, we'll be in one of three
      // situations -- we'll have one to return, have one that can
      // fire in the future, or not have any
      enum class NextReqType { returning, future, none };

      // specifies which queue next request will get popped from
      enum class HeapId { reservation, ready };

      // this is returned from next_req to tell the caller the situation
      struct NextReq {
	NextReqType type;
	union {
	  HeapId    heap_id;
	  Time      when_ready;
	};
      };


      // a function that can be called to look up client information
      using ClientInfoFunc = std::function<ClientInfo(const C&)>;


      bool empty() const {
	DataGuard g(data_mtx);
	return (resv_heap.empty() || ! resv_heap.top().has_request());
      }


      size_t client_count() const {
	DataGuard g(data_mtx);
	return resv_heap.size();
      }


      size_t request_count() const {
	DataGuard g(data_mtx);
	size_t total = 0;
	for (auto i = resv_heap.cbegin(); i != resv_heap.cend(); ++i) {
	  total += i->request_count();
	}
	return total;
      }


      bool remove_by_req_filter(std::function<bool(R&&)> filter_accum,
				bool visit_backwards = false) {
	bool any_removed = false;
	DataGuard g(data_mtx);
	for (auto i : client_map) {
	  bool modified =
	    i.second->remove_by_req_filter(filter_accum, visit_backwards);
	  if (modified) {
	    resv_heap.adjust(*i.second);
	    limit_heap.adjust(*i.second);
	    ready_heap.adjust(*i.second);
#if USE_PROP_HEAP
	    prop_heap.adjust(*i.second);
#endif
	    any_removed = true;
	  }
	}
	return any_removed;
      }


      // use as a default value when no accumulator is provide
      static void request_sink(R&& req) {
	// do nothing
      }


      void remove_by_client(const C& client,
			    bool reverse = false,
			    std::function<void (R&&)> accum = request_sink) {
	DataGuard g(data_mtx);

	auto i = client_map.find(client);

	if (i == client_map.end()) return;

	if (reverse) {
	  for (auto j = i->second->requests.rbegin();
	       j != i->second->requests.rend();
	       ++j) {
	    accum(std::move(*j->request));
	  }
	} else {
	  for (auto j = i->second->requests.begin();
	       j != i->second->requests.end();
	       ++j) {
	    accum(std::move(*j->request));
	  }
	}

	i->second->requests.clear();

	resv_heap.adjust(*i->second);
	limit_heap.adjust(*i->second);
	ready_heap.adjust(*i->second);
#if USE_PROP_HEAP
	prop_heap.adjust(*i->second);
#endif
      }


      uint get_heap_branching_factor() const {
	return B;
      }


      friend std::ostream& operator<<(std::ostream& out,
				      const PriorityQueueBase& q) {
	std::lock_guard<decltype(q.data_mtx)> guard(q.data_mtx);

	out << "{ PriorityQueue::";
	for (const auto& c : q.client_map) {
	  out << "  { client:" << c.first << ", record:" << *c.second <<
	    " }";
	}
	if (!q.resv_heap.empty()) {
	  const auto& resv = q.resv_heap.top();
	  out << " { reservation_top:" << resv << " }";
	  const auto& ready = q.ready_heap.top();
	  out << " { ready_top:" << ready << " }";
	  const auto& limit = q.limit_heap.top();
	  out << " { limit_top:" << limit << " }";
	} else {
	  out << " HEAPS-EMPTY";
	}
	out << " }";

	return out;
      }

      // for debugging
      void display_queues(std::ostream& out,
			  bool show_res = true,
			  bool show_lim = true,
			  bool show_ready = true,
			  bool show_prop = true) const {
	auto filter = [](const ClientRec& e)->bool { return true; };
	DataGuard g(data_mtx);
	if (show_res) {
	  resv_heap.display_sorted(out << "RESER:", filter);
	}
	if (show_lim) {
	  limit_heap.display_sorted(out << "LIMIT:", filter);
	}
	if (show_ready) {
	  ready_heap.display_sorted(out << "READY:", filter);
	}
#if USE_PROP_HEAP
	if (show_prop) {
	  prop_heap.display_sorted(out << "PROPO:", filter);
	}
#endif
      } // display_queues


    protected:

      // The ClientCompare functor is essentially doing a precedes?
      // operator, returning true if and only if the first parameter
      // must precede the second parameter. If the second must precede
      // the first, or if they are equivalent, false should be
      // returned. The reason for this behavior is that it will be
      // called to test if two items are out of order and if true is
      // returned it will reverse the items. Therefore false is the
      // default return when it doesn't matter to prevent unnecessary
      // re-ordering.
      //
      // The template is supporting variations in sorting based on the
      // heap in question and allowing these variations to be handled
      // at compile-time.
      //
      // tag_field determines which tag is being used for comparison
      //
      // ready_opt determines how the ready flag influences the sort
      //
      // use_prop_delta determines whether the proportional delta is
      // added in for comparison
      template<double RequestTag::*tag_field,
	       ReadyOption ready_opt,
	       bool use_prop_delta>
      struct ClientCompare {
	bool operator()(const ClientRec& n1, const ClientRec& n2) const {
	  if (n1.has_request()) {
	    if (n2.has_request()) {
	      const auto& t1 = n1.next_request().tag;
	      const auto& t2 = n2.next_request().tag;
	      if (ReadyOption::ignore == ready_opt || t1.ready == t2.ready) {
		// if we don't care about ready or the ready values are the same
		if (use_prop_delta) {
		  return (t1.*tag_field + n1.prop_delta) <
		    (t2.*tag_field + n2.prop_delta);
		} else {
		  return t1.*tag_field < t2.*tag_field;
		}
	      } else if (ReadyOption::raises == ready_opt) {
		// use_ready == true && the ready fields are different
		return t1.ready;
	      } else {
		return t2.ready;
	      }
	    } else {
	      // n1 has request but n2 does not
	      return true;
	    }
	  } else if (n2.has_request()) {
	    // n2 has request but n1 does not
	    return false;
	  } else {
	    // both have none; keep stable w false
	    return false;
	  }
	}
      };

      ClientInfoFunc       client_info_f;

      mutable std::mutex data_mtx;
      using DataGuard = std::lock_guard<decltype(data_mtx)>;

      // stable mapping between client ids and client queues
      std::map<C,ClientRecRef> client_map;

      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::reserv_heap_data,
		      ClientCompare<&RequestTag::reservation,
				    ReadyOption::ignore,
				    false>,
		      B> resv_heap;
#if USE_PROP_HEAP
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::prop_heap_data,
		      ClientCompare<&RequestTag::proportion,
				    ReadyOption::ignore,
				    true>,
		      B> prop_heap;
#endif
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::lim_heap_data,
		      ClientCompare<&RequestTag::limit,
				    ReadyOption::lowers,
				    false>,
		      B> limit_heap;
      c::IndIntruHeap<ClientRecRef,
		      ClientRec,
		      &ClientRec::ready_heap_data,
		      ClientCompare<&RequestTag::proportion,
				    ReadyOption::raises,
				    true>,
		      B> ready_heap;

      // if all reservations are met and all other requestes are under
      // limit, this will allow the request next in terms of
      // proportion to still get issued
      bool             allow_limit_break;

      std::atomic_bool finishing;

      // every request creates a tick
      Counter tick = 0;

      // performance data collection
      size_t reserv_sched_count = 0;
      size_t prop_sched_count = 0;
      size_t limit_break_sched_count = 0;

      Duration                  idle_age;
      Duration                  erase_age;
      Duration                  check_time;
      std::deque<MarkPoint>     clean_mark_points;

      // NB: All threads declared at end, so they're destructed first!

      std::unique_ptr<RunEvery> cleaning_job;


      // COMMON constructor that others feed into; we can accept three
      // different variations of durations
      template<typename Rep, typename Per>
      PriorityQueueBase(ClientInfoFunc _client_info_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break) :
	client_info_f(_client_info_f),
	allow_limit_break(_allow_limit_break),
	finishing(false),
	idle_age(std::chrono::duration_cast<Duration>(_idle_age)),
	erase_age(std::chrono::duration_cast<Duration>(_erase_age)),
	check_time(std::chrono::duration_cast<Duration>(_check_time))
      {
	assert(_erase_age >= _idle_age);
	assert(_check_time < _idle_age);
	cleaning_job =
	  std::unique_ptr<RunEvery>(
	    new RunEvery(check_time,
			 std::bind(&PriorityQueueBase::do_clean, this)));
      }


      ~PriorityQueueBase() {
	finishing = true;
      }


      // data_mtx must be held by caller
      void do_add_request(RequestRef&& request,
			  const C& client_id,
			  const ReqParams& req_params,
			  const Time time,
			  const double cost = 0.0) {
	++tick;

	// this pointer will help us create a reference to a shared
	// pointer, no matter which of two codepaths we take
	ClientRec* temp_client;

	auto client_it = client_map.find(client_id);
	if (client_map.end() != client_it) {
	  temp_client = &(*client_it->second); // address of obj of shared_ptr
	} else {
	  ClientInfo info = client_info_f(client_id);
	  ClientRecRef client_rec =
	    std::make_shared<ClientRec>(client_id, info, tick);
	  resv_heap.push(client_rec);
#if USE_PROP_HEAP
	  prop_heap.push(client_rec);
#endif
	  limit_heap.push(client_rec);
	  ready_heap.push(client_rec);
	  client_map[client_id] = client_rec;
	  temp_client = &(*client_rec); // address of obj of shared_ptr
	}

	// for convenience, we'll create a reference to the shared pointer
	ClientRec& client = *temp_client;

	if (client.idle) {
	  // We need to do an adjustment so that idle clients compete
	  // fairly on proportional tags since those tags may have
	  // drifted from real-time. Either use the lowest existing
	  // proportion tag -- O(1) -- or the client with the lowest
	  // previous proportion tag -- O(n) where n = # clients.
	  //
	  // So we don't have to maintain a propotional queue that
	  // keeps the minimum on proportional tag alone (we're
	  // instead using a ready queue), we'll have to check each
	  // client.
	  //
	  // The alternative would be to maintain a proportional queue
	  // (define USE_PROP_TAG) and do an O(1) operation here.

	  // Was unable to confirm whether equality testing on
	  // std::numeric_limits<double>::max() is guaranteed, so
	  // we'll use a compile-time calculated trigger that is one
	  // third the max, which should be much larger than any
	  // expected organic value.
	  constexpr double lowest_prop_tag_trigger =
	    std::numeric_limits<double>::max() / 3.0;

	  double lowest_prop_tag = std::numeric_limits<double>::max();
	  for (auto const &c : client_map) {
	    // don't use ourselves (or anything else that might be
	    // listed as idle) since we're now in the map
	    if (!c.second->idle) {
	      double p;
	      // use either lowest proportion tag or previous proportion tag
	      if (c.second->has_request()) {
		p = c.second->next_request().tag.proportion +
		  c.second->prop_delta;
	      } else {
	        p = c.second->get_req_tag().proportion + c.second->prop_delta;
	      }

	      if (p < lowest_prop_tag) {
		lowest_prop_tag = p;
	      }
	    }
	  }

	  // if this conditional does not fire, it
	  if (lowest_prop_tag < lowest_prop_tag_trigger) {
	    client.prop_delta = lowest_prop_tag - time;
	  }
	  client.idle = false;
	} // if this client was idle

#ifndef DO_NOT_DELAY_TAG_CALC
	RequestTag tag(0, 0, 0, time);

	if (!client.has_request()) {
	  tag = RequestTag(client.get_req_tag(),
			   client.info,
			   req_params,
			   time,
			   cost);

	  // copy tag to previous tag for client
	  client.update_req_tag(tag, tick);
	}
#else
	RequestTag tag(client.get_req_tag(), client.info, req_params, time, cost);
	// copy tag to previous tag for client
	client.update_req_tag(tag, tick);
#endif

	client.add_request(tag, client.client, std::move(request));
	if (1 == client.requests.size()) {
	  // NB: can the following 4 calls to adjust be changed
	  // promote? Can adding a request ever demote a client in the
	  // heaps?
	  resv_heap.adjust(client);
	  limit_heap.adjust(client);
	  ready_heap.adjust(client);
#if USE_PROP_HEAP
	  prop_heap.adjust(client);
#endif
	}

	client.cur_rho = req_params.rho;
	client.cur_delta = req_params.delta;

	resv_heap.adjust(client);
	limit_heap.adjust(client);
	ready_heap.adjust(client);
#if USE_PROP_HEAP
	prop_heap.adjust(client);
#endif
      } // add_request


      // data_mtx should be held when called; top of heap should have
      // a ready request
      template<typename C1, IndIntruHeapData ClientRec::*C2, typename C3>
      void pop_process_request(IndIntruHeap<C1, ClientRec, C2, C3, B>& heap,
			       std::function<void(const C& client,
						  RequestRef& request)> process) {
	// gain access to data
	ClientRec& top = heap.top();

	RequestRef request = std::move(top.next_request().request);
#ifndef DO_NOT_DELAY_TAG_CALC
	RequestTag tag = top.next_request().tag;
#endif

	// pop request and adjust heaps
	top.pop_request();

#ifndef DO_NOT_DELAY_TAG_CALC
	if (top.has_request()) {
	  ClientReq& next_first = top.next_request();
	  next_first.tag = RequestTag(tag, top.info,
				      top.cur_delta, top.cur_rho,
				      next_first.tag.arrival);

  	  // copy tag to previous tag for client
	  top.update_req_tag(next_first.tag, tick);
	}
#endif

	resv_heap.demote(top);
	limit_heap.adjust(top);
#if USE_PROP_HEAP
	prop_heap.demote(top);
#endif
	ready_heap.demote(top);

	// process
	process(top.client, request);
      } // pop_process_request


      // data_mtx should be held when called
      void reduce_reservation_tags(ClientRec& client) {
	for (auto& r : client.requests) {
	  r.tag.reservation -= client.info.reservation_inv;

#ifndef DO_NOT_DELAY_TAG_CALC
	  // reduce only for front tag. because next tags' value are invalid
	  break;
#endif
	}
	// don't forget to update previous tag
	client.prev_tag.reservation -= client.info.reservation_inv;
	resv_heap.promote(client);
      }


      // data_mtx should be held when called
      void reduce_reservation_tags(const C& client_id) {
	auto client_it = client_map.find(client_id);

	// means the client was cleaned from map; should never happen
	// as long as cleaning times are long enough
	assert(client_map.end() != client_it);
	reduce_reservation_tags(*client_it->second);
      }


      // data_mtx should be held when called
      NextReq do_next_request(Time now) {
	NextReq result;

	// if reservation queue is empty, all are empty (i.e., no active clients)
	if(resv_heap.empty()) {
	  result.type = NextReqType::none;
	  return result;
	}

	// try constraint (reservation) based scheduling

	auto& reserv = resv_heap.top();
	if (reserv.has_request() &&
	    reserv.next_request().tag.reservation <= now) {
	  result.type = NextReqType::returning;
	  result.heap_id = HeapId::reservation;
	  return result;
	}

	// no existing reservations before now, so try weight-based
	// scheduling

	// all items that are within limit are eligible based on
	// priority
	auto limits = &limit_heap.top();
	while (limits->has_request() &&
	       !limits->next_request().tag.ready &&
	       limits->next_request().tag.limit <= now) {
	  limits->next_request().tag.ready = true;
	  ready_heap.promote(*limits);
	  limit_heap.demote(*limits);

	  limits = &limit_heap.top();
	}

	auto& readys = ready_heap.top();
	if (readys.has_request() &&
	    readys.next_request().tag.ready &&
	    readys.next_request().tag.proportion < max_tag) {
	  result.type = NextReqType::returning;
	  result.heap_id = HeapId::ready;
	  return result;
	}

	// if nothing is schedulable by reservation or
	// proportion/weight, and if we allow limit break, try to
	// schedule something with the lowest proportion tag or
	// alternatively lowest reservation tag.
	if (allow_limit_break) {
	  if (readys.has_request() &&
	      readys.next_request().tag.proportion < max_tag) {
	    result.type = NextReqType::returning;
	    result.heap_id = HeapId::ready;
	    return result;
	  } else if (reserv.has_request() &&
		     reserv.next_request().tag.reservation < max_tag) {
	    result.type = NextReqType::returning;
	    result.heap_id = HeapId::reservation;
	    return result;
	  }
	}

	// nothing scheduled; make sure we re-run when next
	// reservation item or next limited item comes up

	Time next_call = TimeMax;
	if (resv_heap.top().has_request()) {
	  next_call =
	    min_not_0_time(next_call,
			   resv_heap.top().next_request().tag.reservation);
	}
	if (limit_heap.top().has_request()) {
	  const auto& next = limit_heap.top().next_request();
	  assert(!next.tag.ready || max_tag == next.tag.proportion);
	  next_call = min_not_0_time(next_call, next.tag.limit);
	}
	if (next_call < TimeMax) {
	  result.type = NextReqType::future;
	  result.when_ready = next_call;
	  return result;
	} else {
	  result.type = NextReqType::none;
	  return result;
	}
      } // do_next_request


      // if possible is not zero and less than current then return it;
      // otherwise return current; the idea is we're trying to find
      // the minimal time but ignoring zero
      static inline const Time& min_not_0_time(const Time& current,
					       const Time& possible) {
	return TimeZero == possible ? current : std::min(current, possible);
      }


      /*
       * This is being called regularly by RunEvery. Every time it's
       * called it notes the time and delta counter (mark point) in a
       * deque. It also looks at the deque to find the most recent
       * mark point that is older than clean_age. It then walks the
       * map and delete all server entries that were last used before
       * that mark point.
       */
      void do_clean() {
	TimePoint now = std::chrono::steady_clock::now();
	DataGuard g(data_mtx);
	clean_mark_points.emplace_back(MarkPoint(now, tick));

	// first erase the super-old client records

	Counter erase_point = 0;
	auto point = clean_mark_points.front();
	while (point.first <= now - erase_age) {
	  erase_point = point.second;
	  clean_mark_points.pop_front();
	  point = clean_mark_points.front();
	}

	Counter idle_point = 0;
	for (auto i : clean_mark_points) {
	  if (i.first <= now - idle_age) {
	    idle_point = i.second;
	  } else {
	    break;
	  }
	}

	if (erase_point > 0 || idle_point > 0) {
	  for (auto i = client_map.begin(); i != client_map.end(); /* empty */) {
	    auto i2 = i++;
	    if (erase_point && i2->second->last_tick <= erase_point) {
	      delete_from_heaps(i2->second);
	      client_map.erase(i2);
	    } else if (idle_point && i2->second->last_tick <= idle_point) {
	      i2->second->idle = true;
	    }
	  } // for
	} // if
      } // do_clean


      // data_mtx must be held by caller
      template<IndIntruHeapData ClientRec::*C1,typename C2>
      void delete_from_heap(ClientRecRef& client,
			    c::IndIntruHeap<ClientRecRef,ClientRec,C1,C2,B>& heap) {
	auto i = heap.rfind(client);
	heap.remove(i);
      }


      // data_mtx must be held by caller
      void delete_from_heaps(ClientRecRef& client) {
	delete_from_heap(client, resv_heap);
#if USE_PROP_HEAP
	delete_from_heap(client, prop_heap);
#endif
	delete_from_heap(client, limit_heap);
	delete_from_heap(client, ready_heap);
      }
    }; // class PriorityQueueBase


    template<typename C, typename R, uint B=2>
    class PullPriorityQueue : public PriorityQueueBase<C,R,B> {
      using super = PriorityQueueBase<C,R,B>;

    public:

      // When a request is pulled, this is the return type.
      struct PullReq {
	struct Retn {
	  C                           client;
	  typename super::RequestRef  request;
	  PhaseType                   phase;
	};

	typename super::NextReqType   type;
	boost::variant<Retn,Time>     data;

	bool is_none() const { return type == super::NextReqType::none; }

	bool is_retn() const { return type == super::NextReqType::returning; }
	Retn& get_retn() {
	  return boost::get<Retn>(data);
	}

	bool is_future() const { return type == super::NextReqType::future; }
	Time getTime() const { return boost::get<Time>(data); }
      };


#ifdef PROFILE
      ProfileTimer<std::chrono::nanoseconds> pull_request_timer;
      ProfileTimer<std::chrono::nanoseconds> add_request_timer;
#endif

      template<typename Rep, typename Per>
      PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break = false) :
	super(_client_info_f,
	      _idle_age, _erase_age, _check_time,
	      _allow_limit_break)
      {
	// empty
      }


      // pull convenience constructor
      PullPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			bool _allow_limit_break = false) :
	PullPriorityQueue(_client_info_f,
			  std::chrono::minutes(10),
			  std::chrono::minutes(15),
			  std::chrono::minutes(6),
			  _allow_limit_break)
      {
	// empty
      }


      inline void add_request(R&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(std::move(request))),
		    client_id,
		    req_params,
		    get_time(),
		    addl_cost);
      }


      inline void add_request(R&& request,
			      const C& client_id,
			      double addl_cost = 0.0) {
	static const ReqParams null_req_params;
	add_request(typename super::RequestRef(new R(std::move(request))),
		    client_id,
		    null_req_params,
		    get_time(),
		    addl_cost);
      }



      inline void add_request_time(R&& request,
				   const C& client_id,
				   const ReqParams& req_params,
				   const Time time,
				   double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(std::move(request))),
		    client_id,
		    req_params,
		    time,
		    addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(request, req_params, client_id, get_time(), addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      double addl_cost = 0.0) {
	static const ReqParams null_req_params;
	add_request(request, null_req_params, client_id, get_time(), addl_cost);
      }


      // this does the work; the versions above provide alternate interfaces
      void add_request(typename super::RequestRef&& request,
		       const C&                     client_id,
		       const ReqParams&             req_params,
		       const Time                   time,
		       double                       addl_cost = 0.0) {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	add_request_timer.start();
#endif
	super::do_add_request(std::move(request),
			      client_id,
			      req_params,
			      time,
			      addl_cost);
	// no call to schedule_request for pull version
#ifdef PROFILE
	add_request_timer.stop();
#endif
      }


      inline PullReq pull_request() {
	return pull_request(get_time());
      }


      PullReq pull_request(Time now) {
	PullReq result;
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	pull_request_timer.start();
#endif

	typename super::NextReq next = super::do_next_request(now);
	result.type = next.type;
	switch(next.type) {
	case super::NextReqType::none:
	  return result;
	case super::NextReqType::future:
	  result.data = next.when_ready;
	  return result;
	case super::NextReqType::returning:
	  // to avoid nesting, break out and let code below handle this case
	  break;
	default:
	  assert(false);
	}

	// we'll only get here if we're returning an entry

	auto process_f =
	  [&] (PullReq& pull_result, PhaseType phase) ->
	  std::function<void(const C&,
			     typename super::RequestRef&)> {
	  return [&pull_result, phase](const C& client,
				       typename super::RequestRef& request) {
	    pull_result.data =
	    typename PullReq::Retn{client, std::move(request), phase};
	  };
	};

	switch(next.heap_id) {
	case super::HeapId::reservation:
	  super::pop_process_request(this->resv_heap,
				     process_f(result, PhaseType::reservation));
	  ++this->reserv_sched_count;
	  break;
	case super::HeapId::ready:
	  super::pop_process_request(this->ready_heap,
				     process_f(result, PhaseType::priority));
	  { // need to use retn temporarily
	    auto& retn = boost::get<typename PullReq::Retn>(result.data);
	    super::reduce_reservation_tags(retn.client);
	  }
	  ++this->prop_sched_count;
	  break;
	default:
	  assert(false);
	}

#ifdef PROFILE
	pull_request_timer.stop();
#endif
	return result;
      } // pull_request


    protected:


      // data_mtx should be held when called; unfortunately this
      // function has to be repeated in both push & pull
      // specializations
      typename super::NextReq next_request() {
	return next_request(get_time());
      }
    }; // class PullPriorityQueue


    // PUSH version
    template<typename C, typename R, uint B=2>
    class PushPriorityQueue : public PriorityQueueBase<C,R,B> {

    protected:

      using super = PriorityQueueBase<C,R,B>;

    public:

      // a function to see whether the server can handle another request
      using CanHandleRequestFunc = std::function<bool(void)>;

      // a function to submit a request to the server; the second
      // parameter is a callback when it's completed
      using HandleRequestFunc =
	std::function<void(const C&,typename super::RequestRef,PhaseType)>;

    protected:

      CanHandleRequestFunc can_handle_f;
      HandleRequestFunc    handle_f;
      // for handling timed scheduling
      std::mutex  sched_ahead_mtx;
      std::condition_variable sched_ahead_cv;
      Time sched_ahead_when = TimeZero;

#ifdef PROFILE
    public:
      ProfileTimer<std::chrono::nanoseconds> add_request_timer;
      ProfileTimer<std::chrono::nanoseconds> request_complete_timer;
    protected:
#endif

      // NB: threads declared last, so constructed last and destructed first

      std::thread sched_ahead_thd;

    public:

      // push full constructor
      template<typename Rep, typename Per>
      PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			CanHandleRequestFunc _can_handle_f,
			HandleRequestFunc _handle_f,
			std::chrono::duration<Rep,Per> _idle_age,
			std::chrono::duration<Rep,Per> _erase_age,
			std::chrono::duration<Rep,Per> _check_time,
			bool _allow_limit_break = false) :
	super(_client_info_f,
	      _idle_age, _erase_age, _check_time,
	      _allow_limit_break)
      {
	can_handle_f = _can_handle_f;
	handle_f = _handle_f;
	sched_ahead_thd = std::thread(&PushPriorityQueue::run_sched_ahead, this);
      }


      // push convenience constructor
      PushPriorityQueue(typename super::ClientInfoFunc _client_info_f,
			CanHandleRequestFunc _can_handle_f,
			HandleRequestFunc _handle_f,
			bool _allow_limit_break = false) :
	PushPriorityQueue(_client_info_f,
			  _can_handle_f,
			  _handle_f,
			  std::chrono::minutes(10),
			  std::chrono::minutes(15),
			  std::chrono::minutes(6),
			  _allow_limit_break)
      {
	// empty
      }


      ~PushPriorityQueue() {
	this->finishing = true;
	sched_ahead_cv.notify_one();
	sched_ahead_thd.join();
      }

    public:

      inline void add_request(R&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(std::move(request))),
		    client_id,
		    req_params,
		    get_time(),
		    addl_cost);
      }


      inline void add_request(typename super::RequestRef&& request,
			      const C& client_id,
			      const ReqParams& req_params,
			      double addl_cost = 0.0) {
	add_request(request, req_params, client_id, get_time(), addl_cost);
      }


      inline void add_request_time(const R& request,
				   const C& client_id,
				   const ReqParams& req_params,
				   const Time time,
				   double addl_cost = 0.0) {
	add_request(typename super::RequestRef(new R(request)),
		    client_id,
		    req_params,
		    time,
		    addl_cost);
      }


      void add_request(typename super::RequestRef&& request,
		       const C& client_id,
		       const ReqParams& req_params,
		       const Time time,
		       double addl_cost = 0.0) {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	add_request_timer.start();
#endif
	super::do_add_request(std::move(request),
			      client_id,
			      req_params,
			      time,
			      addl_cost);
	schedule_request();
#ifdef PROFILE
	add_request_timer.stop();
#endif
      }


      void request_completed() {
	typename super::DataGuard g(this->data_mtx);
#ifdef PROFILE
	request_complete_timer.start();
#endif
	schedule_request();
#ifdef PROFILE
	request_complete_timer.stop();
#endif
      }

    protected:

      // data_mtx should be held when called; furthermore, the heap
      // should not be empty and the top element of the heap should
      // not be already handled
      //
      // NOTE: the use of "super::ClientRec" in either the template
      // construct or as a parameter to submit_top_request generated
      // a compiler error in g++ 4.8.4, when ClientRec was
      // "protected" rather than "public". By g++ 6.3.1 this was not
      // an issue. But for backwards compatibility
      // PriorityQueueBase::ClientRec is public.
      template<typename C1,
	       IndIntruHeapData super::ClientRec::*C2,
	       typename C3,
	       uint B4>
      C submit_top_request(IndIntruHeap<C1,typename super::ClientRec,C2,C3,B4>& heap,
			   PhaseType phase) {
	C client_result;
	super::pop_process_request(heap,
				   [this, phase, &client_result]
				   (const C& client,
				    typename super::RequestRef& request) {
				     client_result = client;
				     handle_f(client, std::move(request), phase);
				   });
	return client_result;
      }


      // data_mtx should be held when called
      void submit_request(typename super::HeapId heap_id) {
	C client;
	switch(heap_id) {
	case super::HeapId::reservation:
	  // don't need to note client
	  (void) submit_top_request(this->resv_heap, PhaseType::reservation);
	  // unlike the other two cases, we do not reduce reservation
	  // tags here
	  ++this->reserv_sched_count;
	  break;
	case super::HeapId::ready:
	  client = submit_top_request(this->ready_heap, PhaseType::priority);
	  super::reduce_reservation_tags(client);
	  ++this->prop_sched_count;
	  break;
	default:
	  assert(false);
	}
      } // submit_request


      // data_mtx should be held when called; unfortunately this
      // function has to be repeated in both push & pull
      // specializations
      typename super::NextReq next_request() {
	return next_request(get_time());
      }


      // data_mtx should be held when called; overrides member
      // function in base class to add check for whether a request can
      // be pushed to the server
      typename super::NextReq next_request(Time now) {
	if (!can_handle_f()) {
	  typename super::NextReq result;
	  result.type = super::NextReqType::none;
	  return result;
	} else {
	  return super::do_next_request(now);
	}
      } // next_request


      // data_mtx should be held when called
      void schedule_request() {
	typename super::NextReq next_req = next_request();
	switch (next_req.type) {
	case super::NextReqType::none:
	  return;
	case super::NextReqType::future:
	  sched_at(next_req.when_ready);
	  break;
	case super::NextReqType::returning:
	  submit_request(next_req.heap_id);
	  break;
	default:
	  assert(false);
	}
      }


      // this is the thread that handles running schedule_request at
      // future times when nothing can be scheduled immediately
      void run_sched_ahead() {
	std::unique_lock<std::mutex> l(sched_ahead_mtx);

	while (!this->finishing) {
	  if (TimeZero == sched_ahead_when) {
	    sched_ahead_cv.wait(l);
	  } else {
	    Time now;
	    while (!this->finishing && (now = get_time()) < sched_ahead_when) {
	      long microseconds_l = long(1 + 1000000 * (sched_ahead_when - now));
	      auto microseconds = std::chrono::microseconds(microseconds_l);
	      sched_ahead_cv.wait_for(l, microseconds);
	    }
	    sched_ahead_when = TimeZero;
	    if (this->finishing) return;

	    l.unlock();
	    if (!this->finishing) {
	      typename super::DataGuard g(this->data_mtx);
	      schedule_request();
	    }
	    l.lock();
	  }
	}
      }


      void sched_at(Time when) {
	std::lock_guard<std::mutex> l(sched_ahead_mtx);
	if (this->finishing) return;
	if (TimeZero == sched_ahead_when || when < sched_ahead_when) {
	  sched_ahead_when = when;
	  sched_ahead_cv.notify_one();
	}
      }
    }; // class PushPriorityQueue

  } // namespace dmclock
} // namespace crimson
