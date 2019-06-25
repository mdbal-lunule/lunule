// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/ObjectRequest.h"
#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "common/WorkQueue.h"
#include "include/Context.h"
#include "include/err.h"

#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ObjectMap.h"
#include "librbd/Utils.h"
#include "librbd/io/AioCompletion.h"
#include "librbd/io/CopyupRequest.h"
#include "librbd/io/ImageRequest.h"
#include "librbd/io/ReadResult.h"

#include <boost/bind.hpp>
#include <boost/optional.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::ObjectRequest: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace io {

namespace {

template <typename I>
inline bool is_copy_on_read(I *ictx, librados::snap_t snap_id) {
  RWLock::RLocker snap_locker(ictx->snap_lock);
  return (ictx->clone_copy_on_read &&
          !ictx->read_only && snap_id == CEPH_NOSNAP &&
          (ictx->exclusive_lock == nullptr ||
           ictx->exclusive_lock->is_lock_owner()));
}

} // anonymous namespace

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_write(I *ictx, const std::string &oid,
                               uint64_t object_no, uint64_t object_off,
                               const ceph::bufferlist &data,
                               const ::SnapContext &snapc, int op_flags,
			       const ZTracer::Trace &parent_trace,
                               Context *completion) {
  return new ObjectWriteRequest<I>(ictx, oid, object_no, object_off, data,
                                   snapc, op_flags, parent_trace, completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_discard(I *ictx, const std::string &oid,
                                 uint64_t object_no, uint64_t object_off,
                                 uint64_t object_len,
                                 const ::SnapContext &snapc,
                                 bool disable_clone_remove,
                                 bool update_object_map,
                                 const ZTracer::Trace &parent_trace,
                                 Context *completion) {
  return new ObjectDiscardRequest<I>(ictx, oid, object_no, object_off,
                                     object_len, snapc, disable_clone_remove,
                                     update_object_map, parent_trace,
                                     completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_writesame(I *ictx, const std::string &oid,
                                   uint64_t object_no, uint64_t object_off,
                                   uint64_t object_len,
                                   const ceph::bufferlist &data,
                                   const ::SnapContext &snapc, int op_flags,
				   const ZTracer::Trace &parent_trace,
                                   Context *completion) {
  return new ObjectWriteSameRequest<I>(ictx, oid, object_no, object_off,
                                       object_len, data, snapc, op_flags,
                                       parent_trace, completion);
}

template <typename I>
ObjectRequest<I>*
ObjectRequest<I>::create_compare_and_write(I *ictx, const std::string &oid,
                                           uint64_t object_no,
                                           uint64_t object_off,
                                           const ceph::bufferlist &cmp_data,
                                           const ceph::bufferlist &write_data,
                                           const ::SnapContext &snapc,
                                           uint64_t *mismatch_offset,
                                           int op_flags,
                                           const ZTracer::Trace &parent_trace,
                                           Context *completion) {
  return new ObjectCompareAndWriteRequest<I>(ictx, oid, object_no, object_off,
                                             cmp_data, write_data, snapc,
                                             mismatch_offset, op_flags,
                                             parent_trace, completion);
}

template <typename I>
ObjectRequest<I>::ObjectRequest(I *ictx, const std::string &oid,
                                uint64_t objectno, uint64_t off,
                                uint64_t len, librados::snap_t snap_id,
                                const char *trace_name,
                                const ZTracer::Trace &trace,
				Context *completion)
  : m_ictx(ictx), m_oid(oid), m_object_no(objectno), m_object_off(off),
    m_object_len(len), m_snap_id(snap_id), m_completion(completion),
    m_trace(util::create_trace(*ictx, "", trace)) {
  if (m_trace.valid()) {
    m_trace.copy_name(trace_name + std::string(" ") + oid);
    m_trace.event("start");
  }
}

template <typename I>
void ObjectRequest<I>::add_write_hint(I& image_ctx,
                                      librados::ObjectWriteOperation *wr) {
  if (image_ctx.enable_alloc_hint) {
    wr->set_alloc_hint(image_ctx.get_object_size(),
                       image_ctx.get_object_size());
  }
}

template <typename I>
bool ObjectRequest<I>::compute_parent_extents(Extents *parent_extents) {
  assert(m_ictx->snap_lock.is_locked());
  assert(m_ictx->parent_lock.is_locked());

  m_has_parent = false;
  parent_extents->clear();

  uint64_t parent_overlap;
  int r = m_ictx->get_parent_overlap(m_snap_id, &parent_overlap);
  if (r < 0) {
    // NOTE: it's possible for a snapshot to be deleted while we are
    // still reading from it
    lderr(m_ictx->cct) << "failed to retrieve parent overlap: "
                       << cpp_strerror(r) << dendl;
    return false;
  } else if (parent_overlap == 0) {
    return false;
  }

  Striper::extent_to_file(m_ictx->cct, &m_ictx->layout, m_object_no, 0,
                          m_ictx->layout.object_size, *parent_extents);
  uint64_t object_overlap = m_ictx->prune_parent_extents(*parent_extents,
                                                         parent_overlap);
  if (object_overlap > 0) {
    ldout(m_ictx->cct, 20) << "overlap " << parent_overlap << " "
                           << "extents " << *parent_extents << dendl;
    m_has_parent = !parent_extents->empty();
    return true;
  }
  return false;
}

template <typename I>
void ObjectRequest<I>::async_finish(int r) {
  ldout(m_ictx->cct, 20) << "r=" << r << dendl;
  m_ictx->op_work_queue->queue(util::create_context_callback<
    ObjectRequest<I>, &ObjectRequest<I>::finish>(this), r);
}

template <typename I>
void ObjectRequest<I>::finish(int r) {
  ldout(m_ictx->cct, 20) << "r=" << r << dendl;
  m_completion->complete(r);
  delete this;
}

/** read **/

template <typename I>
ObjectReadRequest<I>::ObjectReadRequest(I *ictx, const std::string &oid,
                                        uint64_t objectno, uint64_t offset,
                                        uint64_t len, librados::snap_t snap_id,
                                        int op_flags, bool cache_initiated,
                                        const ZTracer::Trace &parent_trace,
                                        Context *completion)
  : ObjectRequest<I>(ictx, oid, objectno, offset, len, snap_id, "read",
                     parent_trace, completion),
    m_op_flags(op_flags), m_cache_initiated(cache_initiated) {
}

template <typename I>
void ObjectReadRequest<I>::send() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  if (!m_cache_initiated && image_ctx->object_cacher != nullptr) {
    read_cache();
  } else {
    read_object();
  }
}

template <typename I>
void ObjectReadRequest<I>::read_cache() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  // must use async callback to avoid cache_lock cycle
  auto cache_ctx = util::create_async_context_callback(
    *image_ctx, util::create_context_callback<
      ObjectReadRequest<I>, &ObjectReadRequest<I>::handle_read_cache>(this));
  image_ctx->aio_read_from_cache(
    this->m_oid, this->m_object_no, &m_read_data, this->m_object_len,
    this->m_object_off, cache_ctx, m_op_flags,
    (this->m_trace.valid() ? &this->m_trace : nullptr));
}

template <typename I>
void ObjectReadRequest<I>::handle_read_cache(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    read_parent();
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to read from cache: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  this->finish(0);
}

template <typename I>
void ObjectReadRequest<I>::read_object() {
  I *image_ctx = this->m_ictx;
  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);
    if (image_ctx->object_map != nullptr &&
        !image_ctx->object_map->object_may_exist(this->m_object_no)) {
      image_ctx->op_work_queue->queue(new FunctionContext([this](int r) {
          read_parent();
        }), 0);
      return;
    }
  }

  ldout(image_ctx->cct, 20) << dendl;

  librados::ObjectReadOperation op;
  if (this->m_object_len >= image_ctx->sparse_read_threshold_bytes) {
    op.sparse_read(this->m_object_off, this->m_object_len, &m_ext_map,
                   &m_read_data, nullptr);
  } else {
    op.read(this->m_object_off, this->m_object_len, &m_read_data, nullptr);
  }
  op.set_op_flags2(m_op_flags);

  librados::AioCompletion *rados_completion = util::create_rados_callback<
    ObjectReadRequest<I>, &ObjectReadRequest<I>::handle_read_object>(this);
  int flags = image_ctx->get_read_flags(this->m_snap_id);
  int r = image_ctx->data_ctx.aio_operate(
    this->m_oid, rados_completion, &op, flags, nullptr,
    (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
  assert(r == 0);

  rados_completion->release();
}

template <typename I>
void ObjectReadRequest<I>::handle_read_object(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    read_parent();
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to read from object: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  this->finish(0);
}

template <typename I>
void ObjectReadRequest<I>::read_parent() {
  I *image_ctx = this->m_ictx;
  if (m_cache_initiated) {
    this->finish(-ENOENT);
    return;
  }

  uint64_t object_overlap = 0;
  Extents parent_extents;
  {
    RWLock::RLocker snap_locker(image_ctx->snap_lock);
    RWLock::RLocker parent_locker(image_ctx->parent_lock);

    // calculate reverse mapping onto the image
    Striper::extent_to_file(image_ctx->cct, &image_ctx->layout,
                            this->m_object_no, this->m_object_off,
                            this->m_object_len, parent_extents);

    uint64_t parent_overlap = 0;
    int r = image_ctx->get_parent_overlap(this->m_snap_id, &parent_overlap);
    if (r == 0) {
      object_overlap = image_ctx->prune_parent_extents(parent_extents,
                                                       parent_overlap);
    }
  }

  if (object_overlap == 0) {
    this->finish(-ENOENT);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  AioCompletion *parent_completion = AioCompletion::create_and_start<
    ObjectReadRequest<I>, &ObjectReadRequest<I>::handle_read_parent>(
      this, util::get_image_ctx(image_ctx->parent), AIO_TYPE_READ);
  ImageRequest<I>::aio_read(image_ctx->parent, parent_completion,
                            std::move(parent_extents), ReadResult{&m_read_data},
                            0, this->m_trace);
}

template <typename I>
void ObjectReadRequest<I>::handle_read_parent(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    this->finish(r);
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to read parent extents: "
                          << cpp_strerror(r) << dendl;
    this->finish(r);
    return;
  }

  copyup();
}

template <typename I>
void ObjectReadRequest<I>::copyup() {
  I *image_ctx = this->m_ictx;
  if (!is_copy_on_read(image_ctx, this->m_snap_id)) {
    this->finish(0);
    return;
  }

  image_ctx->owner_lock.get_read();
  image_ctx->snap_lock.get_read();
  image_ctx->parent_lock.get_read();
  Extents parent_extents;
  if (!this->compute_parent_extents(&parent_extents) ||
      (image_ctx->exclusive_lock != nullptr &&
       !image_ctx->exclusive_lock->is_lock_owner())) {
    image_ctx->parent_lock.put_read();
    image_ctx->snap_lock.put_read();
    image_ctx->owner_lock.put_read();
    this->finish(0);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  Mutex::Locker copyup_locker(image_ctx->copyup_list_lock);
  auto it = image_ctx->copyup_list.find(this->m_object_no);
  if (it == image_ctx->copyup_list.end()) {
    // create and kick off a CopyupRequest
    auto new_req = CopyupRequest<I>::create(
      image_ctx, this->m_oid, this->m_object_no, std::move(parent_extents),
      this->m_trace);

    image_ctx->copyup_list[this->m_object_no] = new_req;
    new_req->send();
  }

  image_ctx->parent_lock.put_read();
  image_ctx->snap_lock.put_read();
  image_ctx->owner_lock.put_read();
  this->finish(0);
}

/** write **/

template <typename I>
AbstractObjectWriteRequest<I>::AbstractObjectWriteRequest(
    I *ictx, const std::string &oid, uint64_t object_no, uint64_t object_off,
    uint64_t len, const ::SnapContext &snapc, const char *trace_name,
    const ZTracer::Trace &parent_trace, Context *completion)
  : ObjectRequest<I>(ictx, oid, object_no, object_off, len, CEPH_NOSNAP,
                     trace_name, parent_trace, completion),
    m_snap_seq(snapc.seq.val)
{
  m_snaps.insert(m_snaps.end(), snapc.snaps.begin(), snapc.snaps.end());

  {
    RWLock::RLocker snap_locker(ictx->snap_lock);
    RWLock::RLocker parent_locker(ictx->parent_lock);
    this->compute_parent_extents(&m_parent_extents);
  }

  if (this->m_object_off == 0 &&
      this->m_object_len == ictx->get_object_size()) {
    m_full_object = true;
  }

  if (!this->has_parent() ||
      (m_full_object && m_snaps.empty() && !is_post_copyup_write_required())) {
    this->m_copyup_enabled = false;
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::add_write_hint(
    librados::ObjectWriteOperation *wr) {
  I *image_ctx = this->m_ictx;
  RWLock::RLocker snap_locker(image_ctx->snap_lock);
  if (image_ctx->object_map == nullptr || !this->m_object_may_exist) {
    ObjectRequest<I>::add_write_hint(*image_ctx, wr);
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::send() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << this->get_op_type() << " " << this->m_oid << " "
                            << this->m_object_off << "~" << this->m_object_len
                            << dendl;
  {
    RWLock::RLocker snap_lock(image_ctx->snap_lock);
    if (image_ctx->object_map == nullptr) {
      m_object_may_exist = true;
    } else {
      // should have been flushed prior to releasing lock
      assert(image_ctx->exclusive_lock->is_lock_owner());
      m_object_may_exist = image_ctx->object_map->object_may_exist(
        this->m_object_no);
    }
  }

  if (!m_object_may_exist && is_no_op_for_nonexistent_object()) {
    ldout(image_ctx->cct, 20) << "skipping no-op on nonexistent object"
                              << dendl;
    this->async_finish(0);
    return;
  }

  pre_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::pre_write_object_map_update() {
  I *image_ctx = this->m_ictx;

  image_ctx->snap_lock.get_read();
  if (image_ctx->object_map == nullptr || !is_object_map_update_enabled()) {
    image_ctx->snap_lock.put_read();
    write_object();
    return;
  }

  if (!m_object_may_exist && m_copyup_enabled) {
    // optimization: copyup required
    image_ctx->snap_lock.put_read();
    copyup();
    return;
  }

  uint8_t new_state = this->get_pre_write_object_map_state();
  ldout(image_ctx->cct, 20) << this->m_oid << " " << this->m_object_off
                            << "~" << this->m_object_len << dendl;

  image_ctx->object_map_lock.get_write();
  if (image_ctx->object_map->template aio_update<
        AbstractObjectWriteRequest<I>,
        &AbstractObjectWriteRequest<I>::handle_pre_write_object_map_update>(
          CEPH_NOSNAP, this->m_object_no, new_state, {}, this->m_trace, this)) {
    image_ctx->object_map_lock.put_write();
    image_ctx->snap_lock.put_read();
    return;
  }

  image_ctx->object_map_lock.put_write();
  image_ctx->snap_lock.put_read();
  write_object();
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_pre_write_object_map_update(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  assert(r == 0);
  write_object();
}

template <typename I>
void AbstractObjectWriteRequest<I>::write_object() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  librados::ObjectWriteOperation write;
  if (m_copyup_enabled) {
    ldout(image_ctx->cct, 20) << "guarding write" << dendl;
    write.assert_exists();
  }

  add_write_hint(&write);
  add_write_ops(&write);
  assert(write.size() != 0);

  librados::AioCompletion *rados_completion = util::create_rados_callback<
    AbstractObjectWriteRequest<I>,
    &AbstractObjectWriteRequest<I>::handle_write_object>(this);
  int r = image_ctx->data_ctx.aio_operate(
    this->m_oid, rados_completion, &write, m_snap_seq, m_snaps,
    (this->m_trace.valid() ? this->m_trace.get_info() : nullptr));
  assert(r == 0);
  rados_completion->release();
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_write_object(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  r = filter_write_result(r);
  if (r == -ENOENT) {
    if (m_copyup_enabled) {
      copyup();
      return;
    }
  } else if (r == -EILSEQ) {
    ldout(image_ctx->cct, 10) << "failed to write object" << dendl;
    this->finish(r);
    return;
  } else if (r < 0) {
    lderr(image_ctx->cct) << "failed to write object: " << cpp_strerror(r)
                          << dendl;
    this->finish(r);
    return;
  }

  post_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::copyup() {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << dendl;

  assert(!m_copyup_in_progress);
  m_copyup_in_progress = true;

  image_ctx->copyup_list_lock.Lock();
  auto it = image_ctx->copyup_list.find(this->m_object_no);
  if (it == image_ctx->copyup_list.end()) {
    auto new_req = CopyupRequest<I>::create(
      image_ctx, this->m_oid, this->m_object_no,
      std::move(this->m_parent_extents), this->m_trace);
    this->m_parent_extents.clear();

    // make sure to wait on this CopyupRequest
    new_req->append_request(this);
    image_ctx->copyup_list[this->m_object_no] = new_req;

    image_ctx->copyup_list_lock.Unlock();
    new_req->send();
  } else {
    it->second->append_request(this);
    image_ctx->copyup_list_lock.Unlock();
  }
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_copyup(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  assert(m_copyup_in_progress);
  m_copyup_in_progress = false;

  if (r < 0) {
    lderr(image_ctx->cct) << "failed to copyup object: " << cpp_strerror(r)
                          << dendl;
    this->finish(r);
    return;
  }

  if (is_post_copyup_write_required()) {
    write_object();
    return;
  }

  post_write_object_map_update();
}

template <typename I>
void AbstractObjectWriteRequest<I>::post_write_object_map_update() {
  I *image_ctx = this->m_ictx;

  image_ctx->snap_lock.get_read();
  if (image_ctx->object_map == nullptr || !is_object_map_update_enabled() ||
      !is_non_existent_post_write_object_map_state()) {
    image_ctx->snap_lock.put_read();
    this->finish(0);
    return;
  }

  ldout(image_ctx->cct, 20) << dendl;

  // should have been flushed prior to releasing lock
  assert(image_ctx->exclusive_lock->is_lock_owner());
  image_ctx->object_map_lock.get_write();
  if (image_ctx->object_map->template aio_update<
        AbstractObjectWriteRequest<I>,
        &AbstractObjectWriteRequest<I>::handle_post_write_object_map_update>(
          CEPH_NOSNAP, this->m_object_no, OBJECT_NONEXISTENT, OBJECT_PENDING,
          this->m_trace, this)) {
    image_ctx->object_map_lock.put_write();
    image_ctx->snap_lock.put_read();
    return;
  }

  image_ctx->object_map_lock.put_write();
  image_ctx->snap_lock.put_read();
  this->finish(0);
}

template <typename I>
void AbstractObjectWriteRequest<I>::handle_post_write_object_map_update(int r) {
  I *image_ctx = this->m_ictx;
  ldout(image_ctx->cct, 20) << "r=" << r << dendl;

  assert(r == 0);
  this->finish(0);
}

template <typename I>
void ObjectWriteRequest<I>::add_write_ops(librados::ObjectWriteOperation *wr) {
  if (this->m_full_object) {
    wr->write_full(m_write_data);
  } else {
    wr->write(this->m_object_off, m_write_data);
  }
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
void ObjectWriteSameRequest<I>::add_write_ops(
    librados::ObjectWriteOperation *wr) {
  wr->writesame(this->m_object_off, this->m_object_len, m_write_data);
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
void ObjectCompareAndWriteRequest<I>::add_write_ops(
    librados::ObjectWriteOperation *wr) {
  wr->cmpext(this->m_object_off, m_cmp_bl, nullptr);

  if (this->m_full_object) {
    wr->write_full(m_write_bl);
  } else {
    wr->write(this->m_object_off, m_write_bl);
  }
  wr->set_op_flags2(m_op_flags);
}

template <typename I>
int ObjectCompareAndWriteRequest<I>::filter_write_result(int r) const {
  if (r <= -MAX_ERRNO) {
    I *image_ctx = this->m_ictx;
    Extents image_extents;

    // object extent compare mismatch
    uint64_t offset = -MAX_ERRNO - r;
    Striper::extent_to_file(image_ctx->cct, &image_ctx->layout,
                            this->m_object_no, offset, this->m_object_len,
                            image_extents);
    assert(image_extents.size() == 1);

    if (m_mismatch_offset) {
      *m_mismatch_offset = image_extents[0].first;
    }
    r = -EILSEQ;
  }
  return r;
}

} // namespace io
} // namespace librbd

template class librbd::io::ObjectRequest<librbd::ImageCtx>;
template class librbd::io::ObjectReadRequest<librbd::ImageCtx>;
template class librbd::io::AbstractObjectWriteRequest<librbd::ImageCtx>;
template class librbd::io::ObjectWriteRequest<librbd::ImageCtx>;
template class librbd::io::ObjectDiscardRequest<librbd::ImageCtx>;
template class librbd::io::ObjectWriteSameRequest<librbd::ImageCtx>;
template class librbd::io::ObjectCompareAndWriteRequest<librbd::ImageCtx>;
