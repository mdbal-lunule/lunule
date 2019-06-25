// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "cls/rbd/cls_rbd_types.h"
#include "librbd/operation/SnapshotCreateRequest.h"
#include "common/dout.h"
#include "common/errno.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include "librbd/io/ImageRequestWQ.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::SnapshotCreateRequest: "

namespace librbd {
namespace operation {

using util::create_async_context_callback;
using util::create_context_callback;
using util::create_rados_callback;

template <typename I>
SnapshotCreateRequest<I>::SnapshotCreateRequest(I &image_ctx,
                                                Context *on_finish,
						const cls::rbd::SnapshotNamespace &snap_namespace,
                                                const std::string &snap_name,
                                                uint64_t journal_op_tid,
                                                bool skip_object_map)
  : Request<I>(image_ctx, on_finish, journal_op_tid),
    m_snap_namespace(snap_namespace), m_snap_name(snap_name),
    m_skip_object_map(skip_object_map), m_ret_val(0), m_snap_id(CEPH_NOSNAP) {
}

template <typename I>
void SnapshotCreateRequest<I>::send_op() {
  send_suspend_requests();
}

template <typename I>
void SnapshotCreateRequest<I>::send_suspend_requests() {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  // TODO suspend (shrink) resize to ensure consistent RBD mirror
  send_suspend_aio();
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_suspend_requests(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  // TODO
  send_suspend_aio();
  return nullptr;
}

template <typename I>
void SnapshotCreateRequest<I>::send_suspend_aio() {
  I &image_ctx = this->m_image_ctx;
  assert(image_ctx.owner_lock.is_locked());

  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  image_ctx.io_work_queue->block_writes(create_context_callback<
    SnapshotCreateRequest<I>,
    &SnapshotCreateRequest<I>::handle_suspend_aio>(this));
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_suspend_aio(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  if (*result < 0) {
    lderr(cct) << "failed to block writes: " << cpp_strerror(*result) << dendl;
    image_ctx.io_work_queue->unblock_writes();
    return this->create_context_finisher(*result);
  }

  send_append_op_event();
  return nullptr;
}

template <typename I>
void SnapshotCreateRequest<I>::send_append_op_event() {
  I &image_ctx = this->m_image_ctx;
  if (!this->template append_op_event<
        SnapshotCreateRequest<I>,
        &SnapshotCreateRequest<I>::handle_append_op_event>(this)) {
    send_allocate_snap_id();
    return;
  }

  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_append_op_event(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  if (*result < 0) {
    image_ctx.io_work_queue->unblock_writes();
    lderr(cct) << "failed to commit journal entry: " << cpp_strerror(*result)
               << dendl;
    return this->create_context_finisher(*result);
  }

  send_allocate_snap_id();
  return nullptr;
}

template <typename I>
void SnapshotCreateRequest<I>::send_allocate_snap_id() {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  librados::AioCompletion *rados_completion = create_rados_callback<
    SnapshotCreateRequest<I>,
    &SnapshotCreateRequest<I>::handle_allocate_snap_id>(this);
  image_ctx.data_ctx.aio_selfmanaged_snap_create(&m_snap_id, rados_completion);
  rados_completion->release();
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_allocate_snap_id(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << ", "
                << "snap_id=" << m_snap_id << dendl;

  if (*result < 0) {
    save_result(result);
    image_ctx.io_work_queue->unblock_writes();
    lderr(cct) << "failed to allocate snapshot id: " << cpp_strerror(*result)
               << dendl;
    return this->create_context_finisher(*result);
  }

  send_create_snap();
  return nullptr;
}

template <typename I>
void SnapshotCreateRequest<I>::send_create_snap() {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  RWLock::RLocker owner_locker(image_ctx.owner_lock);
  RWLock::RLocker snap_locker(image_ctx.snap_lock);
  RWLock::RLocker parent_locker(image_ctx.parent_lock);

  // should have been canceled prior to releasing lock
  assert(image_ctx.exclusive_lock == nullptr ||
         image_ctx.exclusive_lock->is_lock_owner());

  // save current size / parent info for creating snapshot record in ImageCtx
  m_size = image_ctx.size;
  m_parent_info = image_ctx.parent_md;

  librados::ObjectWriteOperation op;
  if (image_ctx.old_format) {
    cls_client::old_snapshot_add(&op, m_snap_id, m_snap_name);
  } else {
    cls_client::snapshot_add(&op, m_snap_id, m_snap_name, m_snap_namespace);
  }

  librados::AioCompletion *rados_completion = create_rados_callback<
    SnapshotCreateRequest<I>,
    &SnapshotCreateRequest<I>::handle_create_snap>(this);
  int r = image_ctx.md_ctx.aio_operate(image_ctx.header_oid,
                                       rados_completion, &op);
  assert(r == 0);
  rados_completion->release();
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_create_snap(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  if (*result == -ESTALE) {
    send_allocate_snap_id();
    return nullptr;
  } else if (*result < 0) {
    save_result(result);
    send_release_snap_id();
    return nullptr;
  }

  return send_create_object_map();
}

template <typename I>
Context *SnapshotCreateRequest<I>::send_create_object_map() {
  I &image_ctx = this->m_image_ctx;

  update_snap_context();

  image_ctx.snap_lock.get_read();
  if (image_ctx.object_map == nullptr || m_skip_object_map) {
    image_ctx.snap_lock.put_read();

    image_ctx.io_work_queue->unblock_writes();
    return this->create_context_finisher(0);
  }

  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  {
    RWLock::RLocker object_map_lock(image_ctx.object_map_lock);
    image_ctx.object_map->snapshot_add(
      m_snap_id, create_context_callback<
        SnapshotCreateRequest<I>,
        &SnapshotCreateRequest<I>::handle_create_object_map>(this));
  }
  image_ctx.snap_lock.put_read();
  return nullptr;
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_create_object_map(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  assert(*result == 0);

  image_ctx.io_work_queue->unblock_writes();
  return this->create_context_finisher(0);
}

template <typename I>
void SnapshotCreateRequest<I>::send_release_snap_id() {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  assert(m_snap_id != CEPH_NOSNAP);

  librados::AioCompletion *rados_completion = create_rados_callback<
    SnapshotCreateRequest<I>,
    &SnapshotCreateRequest<I>::handle_release_snap_id>(this);
  image_ctx.data_ctx.aio_selfmanaged_snap_remove(m_snap_id, rados_completion);
  rados_completion->release();
}

template <typename I>
Context *SnapshotCreateRequest<I>::handle_release_snap_id(int *result) {
  I &image_ctx = this->m_image_ctx;
  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << ": r=" << *result << dendl;

  assert(m_ret_val < 0);
  *result = m_ret_val;

  image_ctx.io_work_queue->unblock_writes();
  return this->create_context_finisher(m_ret_val);
}

template <typename I>
void SnapshotCreateRequest<I>::update_snap_context() {
  I &image_ctx = this->m_image_ctx;

  RWLock::RLocker owner_locker(image_ctx.owner_lock);
  RWLock::WLocker snap_locker(image_ctx.snap_lock);
  if (image_ctx.old_format) {
    return;
  }

  if (image_ctx.get_snap_info(m_snap_id) != NULL) {
    return;
  }

  CephContext *cct = image_ctx.cct;
  ldout(cct, 5) << this << " " << __func__ << dendl;

  // should have been canceled prior to releasing lock
  assert(image_ctx.exclusive_lock == nullptr ||
         image_ctx.exclusive_lock->is_lock_owner());

  // immediately add a reference to the new snapshot
  utime_t snap_time = ceph_clock_now();
  image_ctx.add_snap(m_snap_namespace, m_snap_name, m_snap_id, m_size,
		     m_parent_info, RBD_PROTECTION_STATUS_UNPROTECTED,
		     0, snap_time);

  // immediately start using the new snap context if we
  // own the exclusive lock
  std::vector<snapid_t> snaps;
  snaps.push_back(m_snap_id);
  snaps.insert(snaps.end(), image_ctx.snapc.snaps.begin(),
               image_ctx.snapc.snaps.end());

  image_ctx.snapc.seq = m_snap_id;
  image_ctx.snapc.snaps.swap(snaps);
  image_ctx.data_ctx.selfmanaged_snap_set_write_ctx(
    image_ctx.snapc.seq, image_ctx.snaps);
}

} // namespace operation
} // namespace librbd

template class librbd::operation::SnapshotCreateRequest<librbd::ImageCtx>;
