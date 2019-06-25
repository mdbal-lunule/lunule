// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librados_test_stub/TestWatchNotify.h"
#include "include/Context.h"
#include "include/stringify.h"
#include "common/Finisher.h"
#include "test/librados_test_stub/TestRadosClient.h"
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include "include/assert.h"

#define dout_subsys ceph_subsys_rados
#undef dout_prefix
#define dout_prefix *_dout << "TestWatchNotify::" << __func__ << ": "

namespace librados {

std::ostream& operator<<(std::ostream& out,
			 const TestWatchNotify::WatcherID &watcher_id) {
  out << "(" << watcher_id.first << "," << watcher_id.second << ")";
  return out;
}

TestWatchNotify::TestWatchNotify()
  : m_lock("librados::TestWatchNotify::m_lock") {
}

void TestWatchNotify::flush(TestRadosClient *rados_client) {
  CephContext *cct = rados_client->cct();

  ldout(cct, 20) << "enter" << dendl;
  // block until we know no additional async notify callbacks will occur
  Mutex::Locker locker(m_lock);
  while (m_pending_notifies > 0) {
    m_file_watcher_cond.Wait(m_lock);
  }
}

int TestWatchNotify::list_watchers(const std::string& o,
                                   std::list<obj_watch_t> *out_watchers) {
  Mutex::Locker lock(m_lock);
  SharedWatcher watcher = get_watcher(o);

  out_watchers->clear();
  for (TestWatchNotify::WatchHandles::iterator it =
         watcher->watch_handles.begin();
       it != watcher->watch_handles.end(); ++it) {
    obj_watch_t obj;
    strcpy(obj.addr, it->second.addr.c_str());
    obj.watcher_id = static_cast<int64_t>(it->second.gid);
    obj.cookie = it->second.handle;
    obj.timeout_seconds = 30;
    out_watchers->push_back(obj);
  }
  return 0;
}

void TestWatchNotify::aio_flush(TestRadosClient *rados_client,
                                Context *on_finish) {
  rados_client->get_aio_finisher()->queue(on_finish);
}

void TestWatchNotify::aio_watch(TestRadosClient *rados_client,
                                const std::string& o, uint64_t gid,
                                uint64_t *handle,
                                librados::WatchCtx2 *watch_ctx,
                                Context *on_finish) {
  int r = watch(rados_client, o, gid, handle, nullptr, watch_ctx);
  rados_client->get_aio_finisher()->queue(on_finish, r);
}

void TestWatchNotify::aio_unwatch(TestRadosClient *rados_client,
                                  uint64_t handle, Context *on_finish) {
  unwatch(rados_client, handle);
  rados_client->get_aio_finisher()->queue(on_finish);
}

void TestWatchNotify::aio_notify(TestRadosClient *rados_client,
                                 const std::string& oid, bufferlist& bl,
                                 uint64_t timeout_ms, bufferlist *pbl,
                                 Context *on_notify) {
  CephContext *cct = rados_client->cct();

  Mutex::Locker lock(m_lock);
  ++m_pending_notifies;
  uint64_t notify_id = ++m_notify_id;

  ldout(cct, 20) << "oid=" << oid << ": notify_id=" << notify_id << dendl;

  SharedWatcher watcher = get_watcher(oid);

  SharedNotifyHandle notify_handle(new NotifyHandle());
  notify_handle->rados_client = rados_client;
  notify_handle->pbl = pbl;
  notify_handle->on_notify = on_notify;
  for (auto &watch_handle_pair : watcher->watch_handles) {
    WatchHandle &watch_handle = watch_handle_pair.second;
    notify_handle->pending_watcher_ids.insert(std::make_pair(
      watch_handle.gid, watch_handle.handle));
  }
  watcher->notify_handles[notify_id] = notify_handle;

  FunctionContext *ctx = new FunctionContext(
    boost::bind(&TestWatchNotify::execute_notify, this, rados_client, oid, bl,
                notify_id));
  rados_client->get_aio_finisher()->queue(ctx);
}

int TestWatchNotify::notify(TestRadosClient *rados_client,
                            const std::string& oid, bufferlist& bl,
                            uint64_t timeout_ms, bufferlist *pbl) {
  C_SaferCond cond;
  aio_notify(rados_client, oid, bl, timeout_ms, pbl, &cond);
  return cond.wait();
}

void TestWatchNotify::notify_ack(TestRadosClient *rados_client,
                                 const std::string& o, uint64_t notify_id,
                                 uint64_t handle, uint64_t gid,
                                 bufferlist& bl) {
  CephContext *cct = rados_client->cct();
  ldout(cct, 20) << "notify_id=" << notify_id << ", handle=" << handle
		 << ", gid=" << gid << dendl;
  Mutex::Locker lock(m_lock);
  WatcherID watcher_id = std::make_pair(gid, handle);
  ack_notify(rados_client, o, notify_id, watcher_id, bl);
  finish_notify(rados_client, o, notify_id);
}

int TestWatchNotify::watch(TestRadosClient *rados_client,
                           const std::string& o, uint64_t gid,
                           uint64_t *handle, librados::WatchCtx *ctx,
                           librados::WatchCtx2 *ctx2) {
  CephContext *cct = rados_client->cct();

  Mutex::Locker lock(m_lock);
  SharedWatcher watcher = get_watcher(o);

  WatchHandle watch_handle;
  watch_handle.rados_client = rados_client;
  watch_handle.addr = "127.0.0.1:0/" + stringify(rados_client->get_nonce());
  watch_handle.nonce = rados_client->get_nonce();
  watch_handle.gid = gid;
  watch_handle.handle = ++m_handle;
  watch_handle.watch_ctx = ctx;
  watch_handle.watch_ctx2 = ctx2;
  watcher->watch_handles[watch_handle.handle] = watch_handle;

  *handle = watch_handle.handle;

  ldout(cct, 20) << "oid=" << o << ", gid=" << gid << ": handle=" << *handle
	         << dendl;
  return 0;
}

int TestWatchNotify::unwatch(TestRadosClient *rados_client,
                             uint64_t handle) {
  CephContext *cct = rados_client->cct();

  ldout(cct, 20) << "handle=" << handle << dendl;
  Mutex::Locker locker(m_lock);
  for (FileWatchers::iterator it = m_file_watchers.begin();
       it != m_file_watchers.end(); ++it) {
    SharedWatcher watcher = it->second;

    WatchHandles::iterator w_it = watcher->watch_handles.find(handle);
    if (w_it != watcher->watch_handles.end()) {
      watcher->watch_handles.erase(w_it);
      if (watcher->watch_handles.empty() && watcher->notify_handles.empty()) {
        m_file_watchers.erase(it);
      }
      break;
    }
  }
  return 0;
}

TestWatchNotify::SharedWatcher TestWatchNotify::get_watcher(
    const std::string& oid) {
  assert(m_lock.is_locked());
  SharedWatcher &watcher = m_file_watchers[oid];
  if (!watcher) {
    watcher.reset(new Watcher());
  }
  return watcher;
}

void TestWatchNotify::execute_notify(TestRadosClient *rados_client,
                                     const std::string &oid,
                                     bufferlist &bl, uint64_t notify_id) {
  CephContext *cct = rados_client->cct();

  ldout(cct, 20) << "oid=" << oid << ", notify_id=" << notify_id << dendl;

  Mutex::Locker lock(m_lock);
  SharedWatcher watcher = get_watcher(oid);
  WatchHandles &watch_handles = watcher->watch_handles;

  NotifyHandles::iterator n_it = watcher->notify_handles.find(notify_id);
  if (n_it == watcher->notify_handles.end()) {
    ldout(cct, 1) << "oid=" << oid << ", notify_id=" << notify_id
		  << ": not found" << dendl;
    return;
  }

  SharedNotifyHandle notify_handle = n_it->second;
  WatcherIDs watcher_ids(notify_handle->pending_watcher_ids);
  for (WatcherIDs::iterator w_id_it = watcher_ids.begin();
       w_id_it != watcher_ids.end(); ++w_id_it) {
    WatcherID watcher_id = *w_id_it;
    WatchHandles::iterator w_it = watch_handles.find(watcher_id.second);
    if (w_it == watch_handles.end()) {
      // client disconnected before notification processed
      notify_handle->pending_watcher_ids.erase(watcher_id);
    } else {
      WatchHandle watch_handle = w_it->second;
      assert(watch_handle.gid == watcher_id.first);
      assert(watch_handle.handle == watcher_id.second);

      uint64_t notifier_id = rados_client->get_instance_id();
      watch_handle.rados_client->get_aio_finisher()->queue(new FunctionContext(
        [this, oid, bl, notify_id, watch_handle, notifier_id](int r) {
          bufferlist notify_bl;
          notify_bl.append(bl);

          if (watch_handle.watch_ctx2 != NULL) {
            watch_handle.watch_ctx2->handle_notify(notify_id,
                                                   watch_handle.handle,
                                                   notifier_id, notify_bl);
          } else if (watch_handle.watch_ctx != NULL) {
            watch_handle.watch_ctx->notify(0, 0, notify_bl);

            // auto ack old-style watch/notify clients
            ack_notify(watch_handle.rados_client, oid, notify_id,
                       {watch_handle.gid, watch_handle.handle}, bufferlist());
          }
        }));
    }
  }

  finish_notify(rados_client, oid, notify_id);

  if (--m_pending_notifies == 0) {
    m_file_watcher_cond.Signal();
  }
}

void TestWatchNotify::ack_notify(TestRadosClient *rados_client,
                                 const std::string &oid,
                                 uint64_t notify_id,
                                 const WatcherID &watcher_id,
                                 const bufferlist &bl) {
  CephContext *cct = rados_client->cct();

  ldout(cct, 20) << "oid=" << oid << ", notify_id=" << notify_id
		 << ", WatcherID=" << watcher_id << dendl;

  assert(m_lock.is_locked());
  SharedWatcher watcher = get_watcher(oid);

  NotifyHandles::iterator it = watcher->notify_handles.find(notify_id);
  if (it == watcher->notify_handles.end()) {
    ldout(cct, 1) << "oid=" << oid << ", notify_id=" << notify_id
		  << ", WatcherID=" << watcher_id << ": not found" << dendl;
    return;
  }

  bufferlist response;
  response.append(bl);

  SharedNotifyHandle notify_handle = it->second;
  notify_handle->notify_responses[watcher_id] = response;
  notify_handle->pending_watcher_ids.erase(watcher_id);
}

void TestWatchNotify::finish_notify(TestRadosClient *rados_client,
                                    const std::string &oid,
                                    uint64_t notify_id) {
  CephContext *cct = rados_client->cct();

  ldout(cct, 20) << "oid=" << oid << ", notify_id=" << notify_id << dendl;

  assert(m_lock.is_locked());
  SharedWatcher watcher = get_watcher(oid);

  NotifyHandles::iterator it = watcher->notify_handles.find(notify_id);
  if (it == watcher->notify_handles.end()) {
    ldout(cct, 1) << "oid=" << oid << ", notify_id=" << notify_id
	          << ": not found" << dendl;
    return;
  }

  SharedNotifyHandle notify_handle = it->second;
  if (!notify_handle->pending_watcher_ids.empty()) {
    ldout(cct, 10) << "oid=" << oid << ", notify_id=" << notify_id
	           << ": pending watchers, returning" << dendl;
    return;
  }

  ldout(cct, 20) << "oid=" << oid << ", notify_id=" << notify_id
		 << ": completing" << dendl;

  if (notify_handle->pbl != NULL) {
    ::encode(notify_handle->notify_responses, *notify_handle->pbl);
    ::encode(notify_handle->pending_watcher_ids, *notify_handle->pbl);
  }

  notify_handle->rados_client->get_aio_finisher()->queue(
    notify_handle->on_notify, 0);
  watcher->notify_handles.erase(notify_id);
  if (watcher->watch_handles.empty() && watcher->notify_handles.empty()) {
    m_file_watchers.erase(oid);
  }
}

void TestWatchNotify::blacklist(uint32_t nonce) {
  Mutex::Locker locker(m_lock);

  for (auto file_it = m_file_watchers.begin();
       file_it != m_file_watchers.end(); ) {
    auto &watcher = file_it->second;
    for (auto w_it = watcher->watch_handles.begin();
         w_it != watcher->watch_handles.end();) {
      if (w_it->second.nonce == nonce) {
        w_it = watcher->watch_handles.erase(w_it);
      } else {
        ++w_it;
      }
    }
    if (watcher->watch_handles.empty() && watcher->notify_handles.empty()) {
        file_it = m_file_watchers.erase(file_it);
    } else {
      ++file_it;
    }
  }
}

} // namespace librados
