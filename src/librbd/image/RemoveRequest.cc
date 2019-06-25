// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/dout.h"
#include "common/errno.h"
#include "librbd/internal.h"
#include "librbd/ImageState.h"
#include "librbd/Journal.h"
#include "librbd/ObjectMap.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/MirroringWatcher.h"
#include "librbd/journal/RemoveRequest.h"
#include "librbd/image/RemoveRequest.h"
#include "librbd/operation/TrimRequest.h"
#include "librbd/mirror/DisableRequest.h"

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::image::RemoveRequest: " << this << " " \
                           << __func__ << ": "

namespace librbd {
namespace image {

using librados::IoCtx;
using util::create_context_callback;
using util::create_async_context_callback;
using util::create_rados_callback;

template<typename I>
RemoveRequest<I>::RemoveRequest(IoCtx &ioctx, const std::string &image_name,
                                const std::string &image_id, bool force,
                                bool from_trash_remove,
                                ProgressContext &prog_ctx,
                                ContextWQ *op_work_queue, Context *on_finish)
  : m_ioctx(ioctx), m_image_name(image_name), m_image_id(image_id),
    m_force(force), m_from_trash_remove(from_trash_remove),
    m_prog_ctx(prog_ctx), m_op_work_queue(op_work_queue),
    m_on_finish(on_finish) {
  m_cct = reinterpret_cast<CephContext *>(m_ioctx.cct());

  m_image_ctx = I::create((m_image_id.empty() ? m_image_name : std::string()),
                          m_image_id, nullptr, m_ioctx, false);
}

template<typename I>
void RemoveRequest<I>::send() {
  ldout(m_cct, 20) << dendl;

  open_image();
}

template<typename I>
void RemoveRequest<I>::open_image() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  Context *ctx = create_context_callback<klass, &klass::handle_open_image>(
    this);

  m_image_ctx->state->open(true, ctx);
}

template<typename I>
void RemoveRequest<I>::handle_open_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    m_image_ctx->destroy();
    m_image_ctx = nullptr;

    if (r != -ENOENT) {
      lderr(m_cct) << "error opening image: " << cpp_strerror(r) << dendl;
      finish(r);
      return;
    }

    remove_image();
    return;
  }

  m_image_id = m_image_ctx->id;
  m_image_name = m_image_ctx->name;
  m_header_oid = m_image_ctx->header_oid;
  m_old_format = m_image_ctx->old_format;
  m_unknown_format = false;

  check_exclusive_lock();
}

template<typename I>
void RemoveRequest<I>::check_exclusive_lock() {
  ldout(m_cct, 20) << dendl;

  if (m_image_ctx->exclusive_lock == nullptr) {
    validate_image_removal();
  } else {
    acquire_exclusive_lock();
  }
}

template<typename I>
void RemoveRequest<I>::acquire_exclusive_lock() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  if (m_force) {
    Context *ctx = create_context_callback<
      klass, &klass::handle_exclusive_lock_force>(this);
    m_exclusive_lock = m_image_ctx->exclusive_lock;
    m_exclusive_lock->shut_down(ctx);
  } else {
    Context *ctx = create_context_callback<
      klass, &klass::handle_exclusive_lock>(this);
    RWLock::WLocker owner_lock(m_image_ctx->owner_lock);
    m_image_ctx->exclusive_lock->try_acquire_lock(ctx);
  }
}

template<typename I>
void RemoveRequest<I>::handle_exclusive_lock_force(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  delete m_exclusive_lock;
  m_exclusive_lock = nullptr;

  if (r < 0) {
    lderr(m_cct) << "error shutting down exclusive lock: "
                 << cpp_strerror(r) << dendl;
    send_close_image(r);
    return;
  }

  assert(m_image_ctx->exclusive_lock == nullptr);
  validate_image_removal();
}

template<typename I>
void RemoveRequest<I>::handle_exclusive_lock(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 || !m_image_ctx->exclusive_lock->is_lock_owner()) {
    lderr(m_cct) << "cannot obtain exclusive lock - not removing" << dendl;
    send_close_image(-EBUSY);
    return;
  }

  validate_image_removal();
}

template<typename I>
void RemoveRequest<I>::validate_image_removal() {
  ldout(m_cct, 20) << dendl;

  check_image_snaps();
}

template<typename I>
void RemoveRequest<I>::check_image_snaps() {
  ldout(m_cct, 20) << dendl;

  if (m_image_ctx->snaps.size()) {
    lderr(m_cct) << "image has snapshots - not removing" << dendl;
    send_close_image(-ENOTEMPTY);
    return;
  }

  list_image_watchers();
}

template<typename I>
void RemoveRequest<I>::list_image_watchers() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectReadOperation op;
  op.list_watchers(&m_watchers, &m_ret_val);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_list_image_watchers>(this);

  int r = m_image_ctx->md_ctx.aio_operate(m_header_oid, rados_completion,
                                          &op, &m_out_bl);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_list_image_watchers(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r == 0 && m_ret_val < 0) {
    r = m_ret_val;
  }
  if (r < 0) {
    lderr(m_cct) << "error listing image watchers: " << cpp_strerror(r)
                 << dendl;
    send_close_image(r);
    return;
  }

  get_mirror_image();
}

template<typename I>
void RemoveRequest<I>::get_mirror_image() {
  ldout(m_cct, 20) << dendl;
  if ((m_watchers.empty()) ||
      ((m_image_ctx->features & RBD_FEATURE_JOURNALING) == 0)) {
    check_image_watchers();
    return;
  }

  librados::ObjectReadOperation op;
  cls_client::mirror_image_get_start(&op, m_image_id);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *comp =
    create_rados_callback<klass, &klass::handle_get_mirror_image>(this);
  m_out_bl.clear();
  int r = m_image_ctx->md_ctx.aio_operate(RBD_MIRRORING, comp, &op, &m_out_bl);
  assert(r == 0);
  comp->release();
}

template<typename I>
void RemoveRequest<I>::handle_get_mirror_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT || r == -EOPNOTSUPP) {
    check_image_watchers();
    return;
  } else if (r < 0) {
    ldout(m_cct, 5) << "error retrieving mirror image: " << cpp_strerror(r)
                    << dendl;
  }

  list_mirror_watchers();
}

template<typename I>
void RemoveRequest<I>::list_mirror_watchers() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectReadOperation op;
  op.list_watchers(&m_mirror_watchers, &m_ret_val);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_list_mirror_watchers>(this);
  m_out_bl.clear();
  int r = m_image_ctx->md_ctx.aio_operate(RBD_MIRRORING, rados_completion,
                                          &op, &m_out_bl);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_list_mirror_watchers(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r == 0 && m_ret_val < 0) {
    r = m_ret_val;
  }
  if (r < 0 && r != -ENOENT) {
    ldout(m_cct, 5) << "error listing mirror watchers: " << cpp_strerror(r)
                    << dendl;
  }

  for (auto &watcher : m_mirror_watchers) {
    m_watchers.remove_if([watcher] (obj_watch_t &w) {
        return (strncmp(w.addr, watcher.addr, sizeof(w.addr)) == 0);
      });
  }

  check_image_watchers();
}

template<typename I>
void RemoveRequest<I>::check_image_watchers() {
  if (m_watchers.size() > 1) {
    lderr(m_cct) << "image has watchers - not removing" << dendl;
    send_close_image(-EBUSY);
    return;
  }

  check_group();
}

template<typename I>
void RemoveRequest<I>::check_group() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectReadOperation op;
  librbd::cls_client::image_get_group_start(&op);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion = create_rados_callback<
    klass, &klass::handle_check_group>(this);
  m_out_bl.clear();
  int r = m_image_ctx->md_ctx.aio_operate(m_header_oid, rados_completion, &op,
                                          &m_out_bl);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_check_group(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  cls::rbd::GroupSpec s;
  if (r == 0) {
    bufferlist::iterator it = m_out_bl.begin();
    r = librbd::cls_client::image_get_group_finish(&it, &s);
  }
  if (r < 0 && r != -EOPNOTSUPP) {
    lderr(m_cct) << "error fetching group for image: "
                 << cpp_strerror(r) << dendl;
    send_close_image(r);
    return;
  }

  if (s.is_valid()) {
    lderr(m_cct) << "image is in a group - not removing" << dendl;
    send_close_image(-EMLINK);
    return;
  }

  trim_image();
}

template<typename I>
void RemoveRequest<I>::trim_image() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  Context *ctx = create_async_context_callback(
    *m_image_ctx, create_context_callback<
      klass, &klass::handle_trim_image>(this));

  RWLock::RLocker owner_lock(m_image_ctx->owner_lock);
  auto req = librbd::operation::TrimRequest<I>::create(
    *m_image_ctx, ctx, m_image_ctx->size, 0, m_prog_ctx);
  req->send();
}

template<typename I>
void RemoveRequest<I>::handle_trim_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    lderr(m_cct) << "warning: failed to remove some object(s): "
                 << cpp_strerror(r) << dendl;
  }

  if (m_old_format) {
    send_close_image(r);
    return;
  }

  remove_child();
}

template<typename I>
void RemoveRequest<I>::remove_child() {
  ldout(m_cct, 20) << dendl;

  m_image_ctx->parent_lock.get_read();
  ParentInfo parent_info = m_image_ctx->parent_md;
  m_image_ctx->parent_lock.put_read();

  librados::ObjectWriteOperation op;
  librbd::cls_client::remove_child(&op, parent_info.spec, m_image_id);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_remove_child>(this);
  int r = m_image_ctx->md_ctx.aio_operate(RBD_CHILDREN, rados_completion, &op);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_remove_child(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r == -ENOENT) {
    r = 0;
  } else if (r < 0) {
    lderr(m_cct) << "error removing child from children list: "
                 << cpp_strerror(r) << dendl;
    send_close_image(r);
    return;
  }


  send_disable_mirror();
}

template<typename I>
void RemoveRequest<I>::send_disable_mirror() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  Context *ctx = create_context_callback<
    klass, &klass::handle_disable_mirror>(this);

  mirror::DisableRequest<I> *req =
    mirror::DisableRequest<I>::create(m_image_ctx, m_force, !m_force, ctx);
  req->send();
}

template<typename I>
void RemoveRequest<I>::handle_disable_mirror(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r == -EOPNOTSUPP) {
    r = 0;
  } else if (r < 0) {
    lderr(m_cct) << "error disabling image mirroring: "
                 << cpp_strerror(r) << dendl;
  }

  send_close_image(r);
}

template<typename I>
void RemoveRequest<I>::send_close_image(int r) {
  ldout(m_cct, 20) << dendl;

  m_ret_val = r;
  using klass = RemoveRequest<I>;
  Context *ctx = create_context_callback<
    klass, &klass::handle_send_close_image>(this);

  m_image_ctx->state->close(ctx);
}

template<typename I>
void RemoveRequest<I>::handle_send_close_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0) {
    lderr(m_cct) << "error encountered while closing image: "
                 << cpp_strerror(r) << dendl;
  }

  m_image_ctx->destroy();
  m_image_ctx = nullptr;
  if (m_ret_val < 0) {
    r = m_ret_val;
    finish(r);
    return;
  }

  remove_header();
}

template<typename I>
void RemoveRequest<I>::remove_header() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_remove_header>(this);
  int r = m_ioctx.aio_remove(m_header_oid, rados_completion);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_remove_header(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error removing header: " << cpp_strerror(r) << dendl;
    m_ret_val = r;
  }

  remove_image();
}

template<typename I>
void RemoveRequest<I>::remove_header_v2() {
  ldout(m_cct, 20) << dendl;

  if (m_header_oid.empty()) {
    m_header_oid = util::header_name(m_image_id);
  }

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_remove_header_v2>(this);
  int r = m_ioctx.aio_remove(m_header_oid, rados_completion);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_remove_header_v2(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error removing header: " << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  send_journal_remove();
}

template<typename I>
void RemoveRequest<I>::send_journal_remove() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  Context *ctx = create_context_callback<
    klass, &klass::handle_journal_remove>(this);

  journal::RemoveRequest<I> *req = journal::RemoveRequest<I>::create(
    m_ioctx, m_image_id, Journal<>::IMAGE_CLIENT_ID, m_op_work_queue, ctx);
  req->send();
}

template<typename I>
void RemoveRequest<I>::handle_journal_remove(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "failed to remove image journal: " << cpp_strerror(r)
                 << dendl;
    finish(r);
    return;
  } else {
    r = 0;
  }

  send_object_map_remove();
}

template<typename I>
void RemoveRequest<I>::send_object_map_remove() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_object_map_remove>(this);

  int r = ObjectMap<>::aio_remove(m_ioctx,
				  m_image_id,
                                  rados_completion);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_object_map_remove(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "failed to remove image journal: " << cpp_strerror(r)
                 << dendl;
    finish(r);
    return;
  } else {
    r = 0;
  }

  mirror_image_remove();
}

template<typename I>
void RemoveRequest<I>::mirror_image_remove() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectWriteOperation op;
  cls_client::mirror_image_remove(&op, m_image_id);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_mirror_image_remove>(this);
  int r = m_ioctx.aio_operate(RBD_MIRRORING, rados_completion, &op);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_mirror_image_remove(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT && r != -EOPNOTSUPP) {
    lderr(m_cct) << "failed to remove mirror image state: "
                 << cpp_strerror(r) << dendl;
    finish(r);
    return;
  }

  if (m_from_trash_remove) {
    // both the id object and the directory entry have been removed in
    // a previous call to trash_move.
    finish(0);
    return;
  }

  remove_id_object();
}

template<typename I>
void RemoveRequest<I>::remove_image() {
  ldout(m_cct, 20) << dendl;

  if (m_old_format || m_unknown_format) {
    remove_v1_image();
  } else {
    remove_v2_image();
  }
}

template<typename I>
void RemoveRequest<I>::remove_v1_image() {
  ldout(m_cct, 20) << dendl;

  Context *ctx = new FunctionContext([this] (int r) {
      r = tmap_rm(m_ioctx, m_image_name);
      handle_remove_v1_image(r);
    });

  m_op_work_queue->queue(ctx, 0);
}

template<typename I>
void RemoveRequest<I>::handle_remove_v1_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  m_old_format = (r == 0);
  if (r == 0 || (r < 0 && !m_unknown_format)) {
    if (r < 0 && r != -ENOENT) {
      lderr(m_cct) << "error removing image from v1 directory: "
                   << cpp_strerror(r) << dendl;
    }

    m_on_finish->complete(r);
    delete this;
    return;
  }

  if (!m_old_format) {
    remove_v2_image();
  }
}

template<typename I>
void RemoveRequest<I>::remove_v2_image() {
  ldout(m_cct, 20) << dendl;

  if (m_image_id.empty()) {
    dir_get_image_id();
    return;
  } else if (m_image_name.empty()) {
    dir_get_image_name();
    return;
  }

  remove_header_v2();
  return;
}

template<typename I>
void RemoveRequest<I>::dir_get_image_id() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectReadOperation op;
  librbd::cls_client::dir_get_id_start(&op, m_image_name);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_dir_get_image_id>(this);
  m_out_bl.clear();
  int r = m_ioctx.aio_operate(RBD_DIRECTORY, rados_completion, &op, &m_out_bl);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_dir_get_image_id(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error fetching image id: " << cpp_strerror(r)
                 << dendl;
    finish(r);
    return;
  }

  if (r == 0) {
    bufferlist::iterator iter = m_out_bl.begin();
    r = librbd::cls_client::dir_get_id_finish(&iter, &m_image_id);
    if (r < 0) {
      finish(r);
      return;
    }
  }

  remove_header_v2();
}

template<typename I>
void RemoveRequest<I>::dir_get_image_name() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectReadOperation op;
  librbd::cls_client::dir_get_name_start(&op, m_image_id);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_dir_get_image_name>(this);
  m_out_bl.clear();
  int r = m_ioctx.aio_operate(RBD_DIRECTORY, rados_completion, &op, &m_out_bl);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_dir_get_image_name(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error fetching image name: " << cpp_strerror(r)
                 << dendl;
    finish(r);
    return;
  }

  if (r == 0) {
    bufferlist::iterator iter = m_out_bl.begin();
    r = librbd::cls_client::dir_get_name_finish(&iter, &m_image_name);
    if (r < 0) {
      finish(r);
      return;
    }
  }

  remove_header_v2();
}

template<typename I>
void RemoveRequest<I>::remove_id_object() {
  ldout(m_cct, 20) << dendl;

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_remove_id_object>(this);
  int r = m_ioctx.aio_remove(util::id_obj_name(m_image_name), rados_completion);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_remove_id_object(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error removing id object: " << cpp_strerror(r)
                 << dendl;
    finish(r);
    return;
  }

  dir_remove_image();
}

template<typename I>
void RemoveRequest<I>::dir_remove_image() {
  ldout(m_cct, 20) << dendl;

  librados::ObjectWriteOperation op;
  librbd::cls_client::dir_remove_image(&op, m_image_name, m_image_id);

  using klass = RemoveRequest<I>;
  librados::AioCompletion *rados_completion =
    create_rados_callback<klass, &klass::handle_dir_remove_image>(this);
  int r = m_ioctx.aio_operate(RBD_DIRECTORY, rados_completion, &op);
  assert(r == 0);
  rados_completion->release();
}

template<typename I>
void RemoveRequest<I>::handle_dir_remove_image(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  if (r < 0 && r != -ENOENT) {
    lderr(m_cct) << "error removing image from v2 directory: "
                 << cpp_strerror(r) << dendl;
  }

  finish(r);
}

template<typename I>
void RemoveRequest<I>::finish(int r) {
  ldout(m_cct, 20) << "r=" << r << dendl;

  m_on_finish->complete(r);
  delete this;
}

} // namespace image
} // namespace librbd

template class librbd::image::RemoveRequest<librbd::ImageCtx>;
