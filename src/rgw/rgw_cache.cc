// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_cache.h"

#include <errno.h>

#define dout_subsys ceph_subsys_rgw

using namespace std;

int ObjectCache::get(const string& name, ObjectCacheInfo& info, uint32_t mask, rgw_cache_entry_info *cache_info)
{
  RWLock::RLocker l(lock);

  if (!enabled) {
    return -ENOENT;
  }

  auto iter = cache_map.find(name);
  if (iter == cache_map.end() ||
      (expiry.count() &&
       (ceph::coarse_mono_clock::now() - iter->second.info.time_added) > expiry)) {
    ldout(cct, 10) << "cache get: name=" << name << " : miss" << dendl;
    if (perfcounter)
      perfcounter->inc(l_rgw_cache_miss);
    return -ENOENT;
  }

  ObjectCacheEntry *entry = &iter->second;

  if (lru_counter - entry->lru_promotion_ts > lru_window) {
    ldout(cct, 20) << "cache get: touching lru, lru_counter=" << lru_counter
                   << " promotion_ts=" << entry->lru_promotion_ts << dendl;
    lock.unlock();
    lock.get_write(); /* promote lock to writer */

    /* need to redo this because entry might have dropped off the cache */
    iter = cache_map.find(name);
    if (iter == cache_map.end()) {
      ldout(cct, 10) << "lost race! cache get: name=" << name << " : miss" << dendl;
      if(perfcounter) perfcounter->inc(l_rgw_cache_miss);
      return -ENOENT;
    }

    entry = &iter->second;
    /* check again, we might have lost a race here */
    if (lru_counter - entry->lru_promotion_ts > lru_window) {
      touch_lru(name, *entry, iter->second.lru_iter);
    }
  }

  ObjectCacheInfo& src = iter->second.info;
  if ((src.flags & mask) != mask) {
    ldout(cct, 10) << "cache get: name=" << name << " : type miss (requested=0x"
                   << std::hex << mask << ", cached=0x" << src.flags
                   << std::dec << ")" << dendl;
    if(perfcounter) perfcounter->inc(l_rgw_cache_miss);
    return -ENOENT;
  }
  ldout(cct, 10) << "cache get: name=" << name << " : hit (requested=0x"
                 << std::hex << mask << ", cached=0x" << src.flags
                 << std::dec << ")" << dendl;

  info = src;
  if (cache_info) {
    cache_info->cache_locator = name;
    cache_info->gen = entry->gen;
  }
  if(perfcounter) perfcounter->inc(l_rgw_cache_hit);

  return 0;
}

bool ObjectCache::chain_cache_entry(list<rgw_cache_entry_info *>& cache_info_entries, RGWChainedCache::Entry *chained_entry)
{
  RWLock::WLocker l(lock);

  if (!enabled) {
    return false;
  }

  list<rgw_cache_entry_info *>::iterator citer;

  list<ObjectCacheEntry *> cache_entry_list;

  /* first verify that all entries are still valid */
  for (citer = cache_info_entries.begin(); citer != cache_info_entries.end(); ++citer) {
    rgw_cache_entry_info *cache_info = *citer;

    ldout(cct, 10) << "chain_cache_entry: cache_locator=" << cache_info->cache_locator << dendl;
    map<string, ObjectCacheEntry>::iterator iter = cache_map.find(cache_info->cache_locator);
    if (iter == cache_map.end()) {
      ldout(cct, 20) << "chain_cache_entry: couldn't find cache locator" << dendl;
      return false;
    }

    ObjectCacheEntry *entry = &iter->second;

    if (entry->gen != cache_info->gen) {
      ldout(cct, 20) << "chain_cache_entry: entry.gen (" << entry->gen << ") != cache_info.gen (" << cache_info->gen << ")" << dendl;
      return false;
    }

    cache_entry_list.push_back(entry);
  }


  chained_entry->cache->chain_cb(chained_entry->key, chained_entry->data);

  list<ObjectCacheEntry *>::iterator liter;

  for (liter = cache_entry_list.begin(); liter != cache_entry_list.end(); ++liter) {
    ObjectCacheEntry *entry = *liter;

    entry->chained_entries.push_back(make_pair(chained_entry->cache, chained_entry->key));
  }

  return true;
}

void ObjectCache::put(const string& name, ObjectCacheInfo& info, rgw_cache_entry_info *cache_info)
{
  RWLock::WLocker l(lock);

  if (!enabled) {
    return;
  }

  ldout(cct, 10) << "cache put: name=" << name << " info.flags=0x"
                 << std::hex << info.flags << std::dec << dendl;
  map<string, ObjectCacheEntry>::iterator iter = cache_map.find(name);
  if (iter == cache_map.end()) {
    ObjectCacheEntry entry;
    entry.lru_iter = lru.end();
    cache_map.insert(pair<string, ObjectCacheEntry>(name, entry));
    iter = cache_map.find(name);
  }
  ObjectCacheEntry& entry = iter->second;
  ObjectCacheInfo& target = entry.info;

  invalidate_lru(entry);

  entry.chained_entries.clear();
  entry.gen++;

  touch_lru(name, entry, entry.lru_iter);

  target.status = info.status;

  if (info.status < 0) {
    target.flags = 0;
    target.xattrs.clear();
    target.data.clear();
    return;
  }

  if (cache_info) {
    cache_info->cache_locator = name;
    cache_info->gen = entry.gen;
  }

  target.flags |= info.flags;

  if (info.flags & CACHE_FLAG_META)
    target.meta = info.meta;
  else if (!(info.flags & CACHE_FLAG_MODIFY_XATTRS))
    target.flags &= ~CACHE_FLAG_META; // non-meta change should reset meta

  if (info.flags & CACHE_FLAG_XATTRS) {
    target.xattrs = info.xattrs;
    map<string, bufferlist>::iterator iter;
    for (iter = target.xattrs.begin(); iter != target.xattrs.end(); ++iter) {
      ldout(cct, 10) << "updating xattr: name=" << iter->first << " bl.length()=" << iter->second.length() << dendl;
    }
  } else if (info.flags & CACHE_FLAG_MODIFY_XATTRS) {
    map<string, bufferlist>::iterator iter;
    for (iter = info.rm_xattrs.begin(); iter != info.rm_xattrs.end(); ++iter) {
      ldout(cct, 10) << "removing xattr: name=" << iter->first << dendl;
      target.xattrs.erase(iter->first);
    }
    for (iter = info.xattrs.begin(); iter != info.xattrs.end(); ++iter) {
      ldout(cct, 10) << "appending xattr: name=" << iter->first << " bl.length()=" << iter->second.length() << dendl;
      target.xattrs[iter->first] = iter->second;
    }
  }

  if (info.flags & CACHE_FLAG_DATA)
    target.data = info.data;

  if (info.flags & CACHE_FLAG_OBJV)
    target.version = info.version;
}

bool ObjectCache::remove(const string& name)
{
  RWLock::WLocker l(lock);

  if (!enabled) {
    return false;
  }

  map<string, ObjectCacheEntry>::iterator iter = cache_map.find(name);
  if (iter == cache_map.end())
    return false;

  ldout(cct, 10) << "removing " << name << " from cache" << dendl;
  ObjectCacheEntry& entry = iter->second;

  for (list<pair<RGWChainedCache *, string> >::iterator iiter = entry.chained_entries.begin();
       iiter != entry.chained_entries.end(); ++iiter) {
    RGWChainedCache *chained_cache = iiter->first;
    chained_cache->invalidate(iiter->second);
  }

  remove_lru(name, iter->second.lru_iter);
  cache_map.erase(iter);
  return true;
}

void ObjectCache::touch_lru(const string& name, ObjectCacheEntry& entry,
			    std::list<string>::iterator& lru_iter)
{
  while (lru_size > (size_t)cct->_conf->rgw_cache_lru_size) {
    list<string>::iterator iter = lru.begin();
    if ((*iter).compare(name) == 0) {
      /*
       * if the entry we're touching happens to be at the lru end, don't remove it,
       * lru shrinking can wait for next time
       */
      break;
    }
    map<string, ObjectCacheEntry>::iterator map_iter = cache_map.find(*iter);
    ldout(cct, 10) << "removing entry: name=" << *iter << " from cache LRU" << dendl;
    if (map_iter != cache_map.end()) {
      ObjectCacheEntry& entry = map_iter->second;
      invalidate_lru(entry);
      cache_map.erase(map_iter);
    }
    lru.pop_front();
    lru_size--;
  }

  if (lru_iter == lru.end()) {
    lru.push_back(name);
    lru_size++;
    lru_iter--;
    ldout(cct, 10) << "adding " << name << " to cache LRU end" << dendl;
  } else {
    ldout(cct, 10) << "moving " << name << " to cache LRU end" << dendl;
    lru.erase(lru_iter);
    lru.push_back(name);
    lru_iter = lru.end();
    --lru_iter;
  }

  lru_counter++;
  entry.lru_promotion_ts = lru_counter;
}

void ObjectCache::remove_lru(const string& name,
			     std::list<string>::iterator& lru_iter)
{
  if (lru_iter == lru.end())
    return;

  lru.erase(lru_iter);
  lru_size--;
  lru_iter = lru.end();
}

void ObjectCache::invalidate_lru(ObjectCacheEntry& entry)
{
  for (list<pair<RGWChainedCache *, string> >::iterator iter = entry.chained_entries.begin();
       iter != entry.chained_entries.end(); ++iter) {
    RGWChainedCache *chained_cache = iter->first;
    chained_cache->invalidate(iter->second);
  }
}

void ObjectCache::set_enabled(bool status)
{
  RWLock::WLocker l(lock);

  enabled = status;

  if (!enabled) {
    do_invalidate_all();
  }
}

void ObjectCache::invalidate_all()
{
  RWLock::WLocker l(lock);

  do_invalidate_all();
}

void ObjectCache::do_invalidate_all()
{
  cache_map.clear();
  lru.clear();

  lru_size = 0;
  lru_counter = 0;
  lru_window = 0;

  for (list<RGWChainedCache *>::iterator iter = chained_cache.begin(); iter != chained_cache.end(); ++iter) {
    (*iter)->invalidate_all();
  }
}

void ObjectCache::chain_cache(RGWChainedCache *cache) {
  RWLock::WLocker l(lock);
  chained_cache.push_back(cache);
}

