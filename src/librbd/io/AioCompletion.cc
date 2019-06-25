// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/io/AioCompletion.h"
#include <errno.h>

#include "common/ceph_context.h"
#include "common/dout.h"
#include "common/errno.h"
#include "common/perf_counters.h"
#include "common/WorkQueue.h"

#include "librbd/ImageCtx.h"
#include "librbd/internal.h"
#include "librbd/Journal.h"
#include "librbd/Types.h"

#ifdef WITH_LTTNG
#include "tracing/librbd.h"
#else
#define tracepoint(...)
#endif

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::io::AioCompletion: " << this \
                           << " " << __func__ << ": "

namespace librbd {
namespace io {

int AioCompletion::wait_for_complete() {
  tracepoint(librbd, aio_wait_for_complete_enter, this);
  lock.Lock();
  while (state != AIO_STATE_COMPLETE)
    cond.Wait(lock);
  lock.Unlock();
  tracepoint(librbd, aio_wait_for_complete_exit, 0);
  return 0;
}

void AioCompletion::finalize(ssize_t rval)
{
  assert(lock.is_locked());
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  ldout(cct, 20) << "r=" << rval << dendl;
  if (rval >= 0 && aio_type == AIO_TYPE_READ) {
    read_result.assemble_result(cct);
  }
}

void AioCompletion::complete() {
  assert(lock.is_locked());
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  tracepoint(librbd, aio_complete_enter, this, rval);
  utime_t elapsed;
  elapsed = ceph_clock_now() - start_time;
  switch (aio_type) {
  case AIO_TYPE_GENERIC:
  case AIO_TYPE_OPEN:
  case AIO_TYPE_CLOSE:
    break;
  case AIO_TYPE_READ:
    ictx->perfcounter->tinc(l_librbd_rd_latency, elapsed); break;
  case AIO_TYPE_WRITE:
    ictx->perfcounter->tinc(l_librbd_wr_latency, elapsed); break;
  case AIO_TYPE_DISCARD:
    ictx->perfcounter->tinc(l_librbd_discard_latency, elapsed); break;
  case AIO_TYPE_FLUSH:
    ictx->perfcounter->tinc(l_librbd_aio_flush_latency, elapsed); break;
  case AIO_TYPE_WRITESAME:
    ictx->perfcounter->tinc(l_librbd_ws_latency, elapsed); break;
  case AIO_TYPE_COMPARE_AND_WRITE:
    ictx->perfcounter->tinc(l_librbd_cmp_latency, elapsed); break;
  default:
    lderr(cct) << "completed invalid aio_type: " << aio_type << dendl;
    break;
  }

  // inform the journal that the op has successfully committed
  if (journal_tid != 0) {
    assert(ictx->journal != NULL);
    ictx->journal->commit_io_event(journal_tid, rval);
  }

  state = AIO_STATE_CALLBACK;
  if (complete_cb) {
    lock.Unlock();
    complete_cb(rbd_comp, complete_arg);
    lock.Lock();
  }

  if (ictx && event_notify && ictx->event_socket.is_valid()) {
    ictx->completed_reqs_lock.Lock();
    ictx->completed_reqs.push_back(&m_xlist_item);
    ictx->completed_reqs_lock.Unlock();
    ictx->event_socket.notify();
  }

  state = AIO_STATE_COMPLETE;
  cond.Signal();

  // note: possible for image to be closed after op marked finished
  if (async_op.started()) {
    async_op.finish_op();
  }
  tracepoint(librbd, aio_complete_exit);
}

void AioCompletion::init_time(ImageCtx *i, aio_type_t t) {
  Mutex::Locker locker(lock);
  if (ictx == nullptr) {
    ictx = i;
    aio_type = t;
    start_time = ceph_clock_now();
  }
}

void AioCompletion::start_op(bool ignore_type) {
  Mutex::Locker locker(lock);
  assert(ictx != nullptr);
  assert(!async_op.started());
  if (state == AIO_STATE_PENDING &&
      (ignore_type || aio_type != AIO_TYPE_FLUSH)) {
    async_op.start_op(*ictx);
  }
}

void AioCompletion::fail(int r)
{
  lock.Lock();
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  lderr(cct) << cpp_strerror(r) << dendl;
  assert(pending_count == 0);
  rval = r;
  complete();
  put_unlock();
}

void AioCompletion::set_request_count(uint32_t count) {
  lock.Lock();
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  ldout(cct, 20) << "pending=" << count << dendl;
  assert(pending_count == 0);
  pending_count = count;
  lock.Unlock();

  // if no pending requests, completion will fire now
  unblock();
}

void AioCompletion::complete_request(ssize_t r)
{
  lock.Lock();
  assert(ictx != nullptr);
  CephContext *cct = ictx->cct;

  if (rval >= 0) {
    if (r < 0 && r != -EEXIST)
      rval = r;
    else if (r > 0)
      rval += r;
  }
  assert(pending_count);
  int count = --pending_count;

  ldout(cct, 20) << "cb=" << complete_cb << ", "
                 << "pending=" << pending_count << dendl;
  if (!count && blockers == 0) {
    finalize(rval);
    complete();
  }
  put_unlock();
}

void AioCompletion::associate_journal_event(uint64_t tid) {
  Mutex::Locker l(lock);
  assert(state == AIO_STATE_PENDING);
  journal_tid = tid;
}

bool AioCompletion::is_complete() {
  tracepoint(librbd, aio_is_complete_enter, this);
  bool done;
  {
    Mutex::Locker l(lock);
    done = this->state == AIO_STATE_COMPLETE;
  }
  tracepoint(librbd, aio_is_complete_exit, done);
  return done;
}

ssize_t AioCompletion::get_return_value() {
  tracepoint(librbd, aio_get_return_value_enter, this);
  lock.Lock();
  ssize_t r = rval;
  lock.Unlock();
  tracepoint(librbd, aio_get_return_value_exit, r);
  return r;
}

} // namespace io
} // namespace librbd
