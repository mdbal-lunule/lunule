// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "ObjectCopyRequest.h"
#include "librados/snap_set_diff.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include "common/errno.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd::mirror::image_sync::ObjectCopyRequest: " \
                           << this << " " << __func__

namespace librados {

bool operator==(const clone_info_t& rhs, const clone_info_t& lhs) {
  return (rhs.cloneid == lhs.cloneid &&
          rhs.snaps == lhs.snaps &&
          rhs.overlap == lhs.overlap &&
          rhs.size == lhs.size);
}

bool operator==(const snap_set_t& rhs, const snap_set_t& lhs) {
  return (rhs.clones == lhs.clones &&
          rhs.seq == lhs.seq);
}

} // namespace librados

namespace rbd {
namespace mirror {
namespace image_sync {

using librbd::util::create_context_callback;
using librbd::util::create_rados_callback;

template <typename I>
ObjectCopyRequest<I>::ObjectCopyRequest(I *local_image_ctx, I *remote_image_ctx,
                                        const SnapMap *snap_map,
                                        uint64_t object_number,
                                        Context *on_finish)
  : m_local_image_ctx(local_image_ctx), m_remote_image_ctx(remote_image_ctx),
    m_snap_map(snap_map), m_object_number(object_number),
    m_on_finish(on_finish) {
  assert(!snap_map->empty());

  m_local_io_ctx.dup(m_local_image_ctx->data_ctx);
  m_local_oid = m_local_image_ctx->get_object_name(object_number);

  m_remote_io_ctx.dup(m_remote_image_ctx->data_ctx);
  m_remote_oid = m_remote_image_ctx->get_object_name(object_number);

  dout(20) << ": "
           << "remote_oid=" << m_remote_oid << ", "
           << "local_oid=" << m_local_oid << dendl;
}

template <typename I>
void ObjectCopyRequest<I>::send() {
  send_list_snaps();
}

template <typename I>
void ObjectCopyRequest<I>::send_list_snaps() {
  dout(20) << dendl;

  librados::AioCompletion *rados_completion = create_rados_callback<
    ObjectCopyRequest<I>, &ObjectCopyRequest<I>::handle_list_snaps>(this);

  librados::ObjectReadOperation op;
  m_snap_set = {};
  m_snap_ret = 0;
  op.list_snaps(&m_snap_set, &m_snap_ret);

  m_remote_io_ctx.snap_set_read(CEPH_SNAPDIR);
  int r = m_remote_io_ctx.aio_operate(m_remote_oid, rados_completion, &op,
                                      nullptr);
  assert(r == 0);
  rados_completion->release();
}

template <typename I>
void ObjectCopyRequest<I>::handle_list_snaps(int r) {
  if (r == 0 && m_snap_ret < 0) {
    r = m_snap_ret;
  }

  dout(20) << ": r=" << r << dendl;

  if (r == -ENOENT) {
    finish(0);
    return;
  }

  if (r < 0) {
    derr << ": failed to list snaps: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  if (m_retry_missing_read) {
    if (m_snap_set == m_retry_snap_set) {
      derr << ": read encountered missing object using up-to-date snap set"
           << dendl;
      finish(-ENOENT);
      return;
    }

    dout(20) << ": retrying using updated snap set" << dendl;
    m_retry_missing_read = false;
    m_retry_snap_set = {};
  }

  compute_diffs();
  send_read_object();
}

template <typename I>
void ObjectCopyRequest<I>::send_read_object() {
  if (m_snap_sync_ops.empty()) {
    // no more snapshot diffs to read from remote
    finish(0);
    return;
  }

  // build the read request
  auto &sync_ops = m_snap_sync_ops.begin()->second;
  assert(!sync_ops.empty());

  bool read_required = false;
  librados::ObjectReadOperation op;
  for (auto &sync_op : sync_ops) {
    switch (sync_op.type) {
    case SYNC_OP_TYPE_WRITE:
      if (!read_required) {
        // map the sync op start snap id back to the necessary read snap id
        librados::snap_t remote_snap_seq =
          m_snap_sync_ops.begin()->first.second;
        m_remote_io_ctx.snap_set_read(remote_snap_seq);

        dout(20) << ": remote_snap_seq=" << remote_snap_seq << dendl;
        read_required = true;
      }
      dout(20) << ": read op: " << sync_op.offset << "~" << sync_op.length
               << dendl;
      op.sparse_read(sync_op.offset, sync_op.length, &sync_op.extent_map,
                     &sync_op.out_bl, nullptr);
      op.set_op_flags2(LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL |
                       LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      break;
    default:
      break;
    }
  }

  if (!read_required) {
    // nothing written to this object for this snapshot (must be trunc/remove)
    send_write_object();
    return;
  }

  librados::AioCompletion *comp = create_rados_callback<
    ObjectCopyRequest<I>, &ObjectCopyRequest<I>::handle_read_object>(this);
  int r = m_remote_io_ctx.aio_operate(m_remote_oid, comp, &op, nullptr);
  assert(r == 0);
  comp->release();
}

template <typename I>
void ObjectCopyRequest<I>::handle_read_object(int r) {
  dout(20) << ": r=" << r << dendl;

  auto snap_seq = m_snap_sync_ops.begin()->first.second;
  if (r == -ENOENT && m_read_whole_object[snap_seq]) {
    dout(5) << ": object missing when forced to read whole object"
            << dendl;
    r = 0;
  }

  if (r == -ENOENT) {
    m_retry_snap_set = m_snap_set;
    m_retry_missing_read = true;

    dout(5) << ": object missing potentially due to removed snapshot" << dendl;
    send_list_snaps();
    return;
  }

  if (r < 0) {
    derr << ": failed to read from remote object: " << cpp_strerror(r)
         << dendl;
    finish(r);
    return;
  }

  send_write_object();
}

template <typename I>
void ObjectCopyRequest<I>::send_write_object() {
  // retrieve the local snap context for the op
  SnapIds local_snap_ids;
  librados::snap_t local_snap_seq = 0;
  librados::snap_t remote_snap_seq = m_snap_sync_ops.begin()->first.first;
  if (remote_snap_seq != 0) {
    auto snap_map_it = m_snap_map->find(remote_snap_seq);
    assert(snap_map_it != m_snap_map->end());

    // write snapshot context should be before actual snapshot
    if (snap_map_it != m_snap_map->begin()) {
      --snap_map_it;
      assert(!snap_map_it->second.empty());
      local_snap_seq = snap_map_it->second.front();
      local_snap_ids = snap_map_it->second;
    }
  }

  Context *finish_op_ctx;
  {
    RWLock::RLocker owner_locker(m_local_image_ctx->owner_lock);
    finish_op_ctx = start_local_op(m_local_image_ctx->owner_lock);
  }
  if (finish_op_ctx == nullptr) {
    derr << ": lost exclusive lock" << dendl;
    finish(-EROFS);
    return;
  }

  dout(20) << ": "
           << "local_snap_seq=" << local_snap_seq << ", "
           << "local_snaps=" << local_snap_ids << dendl;

  auto &sync_ops = m_snap_sync_ops.begin()->second;
  assert(!sync_ops.empty());
  uint64_t object_offset;
  uint64_t buffer_offset;
  librados::ObjectWriteOperation op;
  for (auto &sync_op : sync_ops) {
    switch (sync_op.type) {
    case SYNC_OP_TYPE_WRITE:
      object_offset = sync_op.offset;
      buffer_offset = 0;
      for (auto it : sync_op.extent_map) {
        if (object_offset < it.first) {
          dout(20) << ": zero op: " << object_offset << "~"
                   << it.first - object_offset << dendl;
          op.zero(object_offset, it.first - object_offset);
        }
        dout(20) << ": write op: " << it.first << "~" << it.second << dendl;
        bufferlist tmpbl;
        tmpbl.substr_of(sync_op.out_bl, buffer_offset, it.second);
        op.write(it.first, tmpbl);
        op.set_op_flags2(LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL |
                         LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
        buffer_offset += it.second;
        object_offset = it.first + it.second;
      }
      if (object_offset < sync_op.offset + sync_op.length) {
        uint64_t sync_op_end = sync_op.offset + sync_op.length;
        assert(sync_op_end <= m_snap_object_sizes[remote_snap_seq]);
        if (sync_op_end == m_snap_object_sizes[remote_snap_seq]) {
          dout(20) << ": trunc op: " << object_offset << dendl;
          op.truncate(object_offset);
          m_snap_object_sizes[remote_snap_seq] = object_offset;
        } else {
          dout(20) << ": zero op: " << object_offset << "~"
                   << sync_op_end - object_offset << dendl;
          op.zero(object_offset, sync_op_end - object_offset);
        }
      }
      break;
    case SYNC_OP_TYPE_TRUNC:
      if (sync_op.offset > m_snap_object_sizes[remote_snap_seq]) {
        // skip (must have been updated in WRITE op case issuing trunc op)
        break;
      }
      dout(20) << ": trunc op: " << sync_op.offset << dendl;
      op.truncate(sync_op.offset);
      break;
    case SYNC_OP_TYPE_REMOVE:
      dout(20) << ": remove op" << dendl;
      op.remove();
      break;
    default:
      assert(false);
    }
  }

  auto ctx = new FunctionContext([this, finish_op_ctx](int r) {
      handle_write_object(r);
      finish_op_ctx->complete(0);
    });
  librados::AioCompletion *comp = create_rados_callback(ctx);
  int r = m_local_io_ctx.aio_operate(m_local_oid, comp, &op, local_snap_seq,
                                     local_snap_ids);
  assert(r == 0);
  comp->release();
}

template <typename I>
void ObjectCopyRequest<I>::handle_write_object(int r) {
  dout(20) << ": r=" << r << dendl;

  if (r == -ENOENT) {
    r = 0;
  }
  if (r < 0) {
    derr << ": failed to write to local object: " << cpp_strerror(r)
         << dendl;
    finish(r);
    return;
  }

  m_snap_sync_ops.erase(m_snap_sync_ops.begin());
  if (!m_snap_sync_ops.empty()) {
    send_read_object();
    return;
  }

  send_update_object_map();
}

template <typename I>
void ObjectCopyRequest<I>::send_update_object_map() {
  m_local_image_ctx->owner_lock.get_read();
  m_local_image_ctx->snap_lock.get_read();
  if (!m_local_image_ctx->test_features(RBD_FEATURE_OBJECT_MAP,
                                        m_local_image_ctx->snap_lock) ||
      m_snap_object_states.empty()) {
    m_local_image_ctx->snap_lock.put_read();
    m_local_image_ctx->owner_lock.put_read();
    finish(0);
    return;
  } else if (m_local_image_ctx->object_map == nullptr) {
    // possible that exclusive lock was lost in background
    derr << ": object map is not initialized" << dendl;

    m_local_image_ctx->snap_lock.put_read();
    m_local_image_ctx->owner_lock.put_read();
    finish(-EINVAL);
    return;
  }

  assert(m_local_image_ctx->object_map != nullptr);

  auto snap_object_state = *m_snap_object_states.begin();
  m_snap_object_states.erase(m_snap_object_states.begin());

  dout(20) << ": "
           << "local_snap_id=" << snap_object_state.first << ", "
           << "object_state=" << static_cast<uint32_t>(snap_object_state.second)
           << dendl;

  auto finish_op_ctx = start_local_op(m_local_image_ctx->owner_lock);
  if (finish_op_ctx == nullptr) {
    derr << ": lost exclusive lock" << dendl;
    m_local_image_ctx->snap_lock.put_read();
    m_local_image_ctx->owner_lock.put_read();
    finish(-EROFS);
    return;
  }

  auto ctx = new FunctionContext([this, finish_op_ctx](int r) {
      handle_update_object_map(r);
      finish_op_ctx->complete(0);
    });

  RWLock::WLocker object_map_locker(m_local_image_ctx->object_map_lock);
  bool sent = m_local_image_ctx->object_map->template aio_update<
    Context, &Context::complete>(
      snap_object_state.first, m_object_number, snap_object_state.second, {},
      {}, ctx);
  assert(sent);
  m_local_image_ctx->snap_lock.put_read();
  m_local_image_ctx->owner_lock.put_read();
}

template <typename I>
void ObjectCopyRequest<I>::handle_update_object_map(int r) {
  dout(20) << ": r=" << r << dendl;

  assert(r == 0);
  if (!m_snap_object_states.empty()) {
    send_update_object_map();
    return;
  }
  finish(0);
}

template <typename I>
Context *ObjectCopyRequest<I>::start_local_op(RWLock &owner_lock) {
  assert(m_local_image_ctx->owner_lock.is_locked());
  if (m_local_image_ctx->exclusive_lock == nullptr) {
    return nullptr;
  }
  return m_local_image_ctx->exclusive_lock->start_op();
}

template <typename I>
void ObjectCopyRequest<I>::compute_diffs() {
  CephContext *cct = m_local_image_ctx->cct;

  m_snap_sync_ops = {};
  m_snap_object_states = {};
  m_snap_object_sizes = {};

  librados::snap_t remote_sync_pont_snap_id = m_snap_map->rbegin()->first;
  uint64_t prev_end_size = 0;
  bool prev_exists = false;
  librados::snap_t start_remote_snap_id = 0;
  for (auto &pair : *m_snap_map) {
    assert(!pair.second.empty());
    librados::snap_t end_remote_snap_id = pair.first;
    librados::snap_t end_local_snap_id = pair.second.front();

    interval_set<uint64_t> diff;
    uint64_t end_size;
    bool exists;
    librados::snap_t clone_end_snap_id;
    bool read_whole_object;
    calc_snap_set_diff(cct, m_snap_set, start_remote_snap_id,
                       end_remote_snap_id, &diff, &end_size, &exists,
                       &clone_end_snap_id, &read_whole_object);

    if (read_whole_object) {
      dout(1) << ": need to read full object" << dendl;
      diff.insert(0, m_remote_image_ctx->layout.object_size);
      exists = true;
      end_size = m_remote_image_ctx->layout.object_size;
      clone_end_snap_id = end_remote_snap_id;
    }

    dout(20) << ": "
             << "start_remote_snap=" << start_remote_snap_id << ", "
             << "end_remote_snap_id=" << end_remote_snap_id << ", "
             << "clone_end_snap_id=" << clone_end_snap_id << ", "
             << "end_local_snap_id=" << end_local_snap_id << ", "
             << "diff=" << diff << ", "
             << "end_size=" << end_size << ", "
             << "exists=" << exists << dendl;
    if (exists) {
      // clip diff to size of object (in case it was truncated)
      if (end_size < prev_end_size) {
        interval_set<uint64_t> trunc;
        trunc.insert(end_size, prev_end_size);
        trunc.intersection_of(diff);
        diff.subtract(trunc);
        dout(20) << ": clearing truncate diff: " << trunc << dendl;
      }

      // prepare the object map state
      {
        RWLock::RLocker snap_locker(m_local_image_ctx->snap_lock);
        uint8_t object_state = OBJECT_EXISTS;
        if (m_local_image_ctx->test_features(RBD_FEATURE_FAST_DIFF,
                                             m_local_image_ctx->snap_lock) &&
            prev_exists && diff.empty() && end_size == prev_end_size) {
          object_state = OBJECT_EXISTS_CLEAN;
        }
        m_snap_object_states[end_local_snap_id] = object_state;
      }

      // reads should be issued against the newest (existing) snapshot within
      // the associated snapshot object clone. writes should be issued
      // against the oldest snapshot in the snap_map.
      assert(clone_end_snap_id >= end_remote_snap_id);
      if (clone_end_snap_id > remote_sync_pont_snap_id) {
        // do not read past the sync point snapshot
        clone_end_snap_id = remote_sync_pont_snap_id;
      }
      m_read_whole_object[clone_end_snap_id] = read_whole_object;

      // object write/zero, or truncate
      // NOTE: a single snapshot clone might represent multiple snapshots, but
      // the write/zero and truncate ops will only be associated with the first
      // snapshot encountered within the clone since the diff will be empty for
      // subsequent snapshots and the size will remain constant for a clone.
      for (auto it = diff.begin(); it != diff.end(); ++it) {
        dout(20) << ": read/write op: " << it.get_start() << "~"
                 << it.get_len() << dendl;
        m_snap_sync_ops[{end_remote_snap_id, clone_end_snap_id}].emplace_back(
          SYNC_OP_TYPE_WRITE, it.get_start(), it.get_len());
      }
      if (end_size < prev_end_size) {
        dout(20) << ": trunc op: " << end_size << dendl;
        m_snap_sync_ops[{end_remote_snap_id, clone_end_snap_id}].emplace_back(
          SYNC_OP_TYPE_TRUNC, end_size, 0U);
      }
      m_snap_object_sizes[end_remote_snap_id] = end_size;
    } else {
      if (prev_exists) {
        // object remove
        dout(20) << ": remove op" << dendl;
        m_snap_sync_ops[{end_remote_snap_id, end_remote_snap_id}].emplace_back(
          SYNC_OP_TYPE_REMOVE, 0U, 0U);
      }
    }

    prev_end_size = end_size;
    prev_exists = exists;
    start_remote_snap_id = end_remote_snap_id;
  }
}

template <typename I>
void ObjectCopyRequest<I>::finish(int r) {
  dout(20) << ": r=" << r << dendl;

  // ensure IoCtxs are closed prior to proceeding
  auto on_finish = m_on_finish;
  delete this;

  on_finish->complete(r);
}

} // namespace image_sync
} // namespace mirror
} // namespace rbd

template class rbd::mirror::image_sync::ObjectCopyRequest<librbd::ImageCtx>;
