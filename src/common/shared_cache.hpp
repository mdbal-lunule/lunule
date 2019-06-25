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

#ifndef CEPH_SHAREDCACHE_H
#define CEPH_SHAREDCACHE_H

#include <map>
#include <list>
#include "common/Mutex.h"
#include "common/Cond.h"
#include "include/unordered_map.h"

// re-include our assert to clobber the system one; fix dout:
#include "include/assert.h"

template <class K, class V, class C = std::less<K>, class H = std::hash<K> >
class SharedLRU {
  CephContext *cct;
  typedef ceph::shared_ptr<V> VPtr;
  typedef ceph::weak_ptr<V> WeakVPtr;
  Mutex lock;
  size_t max_size;
  Cond cond;
  unsigned size;
public:
  int waiting;
private:
  ceph::unordered_map<K, typename list<pair<K, VPtr> >::iterator, H> contents;
  list<pair<K, VPtr> > lru;

  map<K, pair<WeakVPtr, V*>, C> weak_refs;

  void trim_cache(list<VPtr> *to_release) {
    while (size > max_size) {
      to_release->push_back(lru.back().second);
      lru_remove(lru.back().first);
    }
  }

  void lru_remove(const K& key) {
    typename ceph::unordered_map<K, typename list<pair<K, VPtr> >::iterator, H>::iterator i = 
      contents.find(key);
    if (i == contents.end())
      return;
    lru.erase(i->second);
    --size;
    contents.erase(i);
  }

  void lru_add(const K& key, const VPtr& val, list<VPtr> *to_release) {
    typename ceph::unordered_map<K, typename list<pair<K, VPtr> >::iterator, H>::iterator i =
      contents.find(key);
    if (i != contents.end()) {
      lru.splice(lru.begin(), lru, i->second);
    } else {
      ++size;
      lru.push_front(make_pair(key, val));
      contents[key] = lru.begin();
      trim_cache(to_release);
    }
  }

  void remove(const K& key, V *valptr) {
    Mutex::Locker l(lock);
    typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.find(key);
    if (i != weak_refs.end() && i->second.second == valptr) {
      weak_refs.erase(i);
    }
    cond.Signal();
  }

  class Cleanup {
  public:
    SharedLRU<K, V, C> *cache;
    K key;
    Cleanup(SharedLRU<K, V, C> *cache, K key) : cache(cache), key(key) {}
    void operator()(V *ptr) {
      cache->remove(key, ptr);
      delete ptr;
    }
  };

public:
  SharedLRU(CephContext *cct = NULL, size_t max_size = 20)
    : cct(cct), lock("SharedLRU::lock"), max_size(max_size), 
      size(0), waiting(0) {
    contents.rehash(max_size); 
  }
  
  ~SharedLRU() {
    contents.clear();
    lru.clear();
    if (!weak_refs.empty()) {
      lderr(cct) << "leaked refs:\n";
      dump_weak_refs(*_dout);
      *_dout << dendl;
      if (cct->_conf->get_val<bool>("debug_asserts_on_shutdown")) {
	assert(weak_refs.empty());
      }
    }
  }

  /// adjust container comparator (for purposes of get_next sort order)
  void reset_comparator(C comp) {
    // get_next uses weak_refs; that's the only container we need to
    // reorder.
    map<K, pair<WeakVPtr, V*>, C> temp;

    Mutex::Locker l(lock);
    temp.swap(weak_refs);

    // reconstruct with new comparator
    weak_refs = map<K, pair<WeakVPtr, V*>, C>(comp);
    weak_refs.insert(temp.begin(), temp.end());
  }

  C get_comparator() {
    return weak_refs.key_comp();
  }

  void set_cct(CephContext *c) {
    cct = c;
  }

  void dump_weak_refs() {
    lderr(cct) << "leaked refs:\n";
    dump_weak_refs(*_dout);
    *_dout << dendl;
  }

  void dump_weak_refs(ostream& out) {
    for (typename map<K, pair<WeakVPtr, V*>, C>::iterator p = weak_refs.begin();
	 p != weak_refs.end();
	 ++p) {
      out << __func__ << " " << this << " weak_refs: "
	  << p->first << " = " << p->second.second
	  << " with " << p->second.first.use_count() << " refs"
	  << std::endl;
    }
  }

  //clear all strong reference from the lru.
  void clear() {
    while (true) {
      VPtr val; // release any ref we have after we drop the lock
      Mutex::Locker l(lock);
      if (size == 0)
        break;

      val = lru.back().second;
      lru_remove(lru.back().first);
    }
  }

  void clear(const K& key) {
    VPtr val; // release any ref we have after we drop the lock
    {
      Mutex::Locker l(lock);
      typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.find(key);
      if (i != weak_refs.end()) {
	val = i->second.first.lock();
      }
      lru_remove(key);
    }
  }

  void purge(const K &key) {
    VPtr val; // release any ref we have after we drop the lock
    {
      Mutex::Locker l(lock);
      typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.find(key);
      if (i != weak_refs.end()) {
	val = i->second.first.lock();
        weak_refs.erase(i);
      }
      lru_remove(key);
    }
  }

  void set_size(size_t new_size) {
    list<VPtr> to_release;
    {
      Mutex::Locker l(lock);
      max_size = new_size;
      trim_cache(&to_release);
    }
  }

  // Returns K key s.t. key <= k for all currently cached k,v
  K cached_key_lower_bound() {
    Mutex::Locker l(lock);
    return weak_refs.begin()->first;
  }

  VPtr lower_bound(const K& key) {
    VPtr val;
    list<VPtr> to_release;
    {
      Mutex::Locker l(lock);
      ++waiting;
      bool retry = false;
      do {
	retry = false;
	if (weak_refs.empty())
	  break;
	typename map<K, pair<WeakVPtr, V*>, C>::iterator i =
	  weak_refs.lower_bound(key);
	if (i == weak_refs.end())
	  --i;
	val = i->second.first.lock();
	if (val) {
	  lru_add(i->first, val, &to_release);
	} else {
	  retry = true;
	}
	if (retry)
	  cond.Wait(lock);
      } while (retry);
      --waiting;
    }
    return val;
  }
  bool get_next(const K &key, pair<K, VPtr> *next) {
    pair<K, VPtr> r;
    {
      Mutex::Locker l(lock);
      VPtr next_val;
      typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.upper_bound(key);

      while (i != weak_refs.end() &&
	     !(next_val = i->second.first.lock()))
	++i;

      if (i == weak_refs.end())
	return false;

      if (next)
	r = make_pair(i->first, next_val);
    }
    if (next)
      *next = r;
    return true;
  }
  bool get_next(const K &key, pair<K, V> *next) {
    pair<K, VPtr> r;
    bool found = get_next(key, &r);
    if (!found || !next)
      return found;
    next->first = r.first;
    assert(r.second);
    next->second = *(r.second);
    return found;
  }

  VPtr lookup(const K& key) {
    VPtr val;
    list<VPtr> to_release;
    {
      Mutex::Locker l(lock);
      ++waiting;
      bool retry = false;
      do {
	retry = false;
	typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.find(key);
	if (i != weak_refs.end()) {
	  val = i->second.first.lock();
	  if (val) {
	    lru_add(key, val, &to_release);
	  } else {
	    retry = true;
	  }
	}
	if (retry)
	  cond.Wait(lock);
      } while (retry);
      --waiting;
    }
    return val;
  }
  VPtr lookup_or_create(const K &key) {
    VPtr val;
    list<VPtr> to_release;
    {
      Mutex::Locker l(lock);
      bool retry = false;
      do {
	retry = false;
	typename map<K, pair<WeakVPtr, V*>, C>::iterator i = weak_refs.find(key);
	if (i != weak_refs.end()) {
	  val = i->second.first.lock();
	  if (val) {
	    lru_add(key, val, &to_release);
	    return val;
	  } else {
	    retry = true;
	  }
	}
	if (retry)
	  cond.Wait(lock);
      } while (retry);

      V *new_value = new V();
      VPtr new_val(new_value, Cleanup(this, key));
      weak_refs.insert(make_pair(key, make_pair(new_val, new_value)));
      lru_add(key, new_val, &to_release);
      return new_val;
    }
  }

  /**
   * empty()
   *
   * Returns true iff there are no live references left to anything that has been
   * in the cache.
   */
  bool empty() {
    Mutex::Locker l(lock);
    return weak_refs.empty();
  }

  /***
   * Inserts a key if not present, or bumps it to the front of the LRU if
   * it is, and then gives you a reference to the value. If the key already
   * existed, you are responsible for deleting the new value you tried to
   * insert.
   *
   * @param key The key to insert
   * @param value The value that goes with the key
   * @param existed Set to true if the value was already in the
   * map, false otherwise
   * @return A reference to the map's value for the given key
   */
  VPtr add(const K& key, V *value, bool *existed = NULL) {
    VPtr val;
    list<VPtr> to_release;
    {
      Mutex::Locker l(lock);
      typename map<K, pair<WeakVPtr, V*>, C>::iterator actual =
	weak_refs.lower_bound(key);
      if (actual != weak_refs.end() && actual->first == key) {
        if (existed) 
          *existed = true;

        return actual->second.first.lock();
      }

      if (existed)      
        *existed = false;

      val = VPtr(value, Cleanup(this, key));
      weak_refs.insert(actual, make_pair(key, make_pair(val, value)));
      lru_add(key, val, &to_release);
    }
    return val;
  }

  friend class SharedLRUTest;
};

#endif
