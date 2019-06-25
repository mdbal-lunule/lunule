// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/Journal.h"
#include "include/rados/librados.hpp"
#include "common/errno.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "cls/journal/cls_journal_types.h"
#include "journal/Journaler.h"
#include "journal/Policy.h"
#include "journal/ReplayEntry.h"
#include "journal/Settings.h"
#include "journal/Utils.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/io/ImageRequestWQ.h"
#include "librbd/io/ObjectRequest.h"
#include "librbd/journal/CreateRequest.h"
#include "librbd/journal/DemoteRequest.h"
#include "librbd/journal/OpenRequest.h"
#include "librbd/journal/RemoveRequest.h"
#include "librbd/journal/Replay.h"
#include "librbd/journal/PromoteRequest.h"

#include <boost/scope_exit.hpp>
#include <utility>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::Journal: "

namespace librbd {

using util::create_async_context_callback;
using util::create_context_callback;
using journal::util::C_DecodeTag;
using journal::util::C_DecodeTags;

namespace {

// TODO: once journaler is 100% async, remove separate threads and
// reuse ImageCtx's thread pool
class ThreadPoolSingleton : public ThreadPool {
public:
  explicit ThreadPoolSingleton(CephContext *cct)
    : ThreadPool(cct, "librbd::Journal", "tp_librbd_journ", 1) {
    start();
  }
  ~ThreadPoolSingleton() override {
    stop();
  }
};

template <typename I>
struct C_IsTagOwner : public Context {
  librados::IoCtx &io_ctx;
  std::string image_id;
  bool *is_tag_owner;
  ContextWQ *op_work_queue;
  Context *on_finish;

  CephContext *cct = nullptr;
  Journaler *journaler;
  cls::journal::Client client;
  journal::ImageClientMeta client_meta;
  uint64_t tag_tid;
  journal::TagData tag_data;

  C_IsTagOwner(librados::IoCtx &io_ctx, const std::string &image_id,
               bool *is_tag_owner, ContextWQ *op_work_queue, Context *on_finish)
    : io_ctx(io_ctx), image_id(image_id), is_tag_owner(is_tag_owner),
      op_work_queue(op_work_queue), on_finish(on_finish),
      cct(reinterpret_cast<CephContext*>(io_ctx.cct())),
      journaler(new Journaler(io_ctx, image_id, Journal<>::IMAGE_CLIENT_ID,
                              {})) {
  }

  void finish(int r) override {
    ldout(cct, 20) << this << " C_IsTagOwner::" << __func__ << ": r=" << r
		   << dendl;
    if (r < 0) {
      lderr(cct) << this << " C_IsTagOwner::" << __func__ << ": "
                 << "failed to get tag owner: " << cpp_strerror(r) << dendl;
    } else {
      *is_tag_owner = (tag_data.mirror_uuid == Journal<>::LOCAL_MIRROR_UUID);
    }

    Journaler *journaler = this->journaler;
    Context *on_finish = this->on_finish;
    FunctionContext *ctx = new FunctionContext(
      [journaler, on_finish](int r) {
	on_finish->complete(r);
	delete journaler;
      });
    op_work_queue->queue(ctx, r);
  }
};

struct C_GetTagOwner : public Context {
  std::string *mirror_uuid;
  Context *on_finish;

  Journaler journaler;
  cls::journal::Client client;
  journal::ImageClientMeta client_meta;
  uint64_t tag_tid;
  journal::TagData tag_data;

  C_GetTagOwner(librados::IoCtx &io_ctx, const std::string &image_id,
                std::string *mirror_uuid, Context *on_finish)
    : mirror_uuid(mirror_uuid), on_finish(on_finish),
      journaler(io_ctx, image_id, Journal<>::IMAGE_CLIENT_ID, {}) {
  }

  virtual void finish(int r) {
    if (r >= 0) {
      *mirror_uuid = tag_data.mirror_uuid;
    }
    on_finish->complete(r);
  }
};

template <typename J>
struct GetTagsRequest {
  CephContext *cct;
  J *journaler;
  cls::journal::Client *client;
  journal::ImageClientMeta *client_meta;
  uint64_t *tag_tid;
  journal::TagData *tag_data;
  Context *on_finish;

  Mutex lock;

  GetTagsRequest(CephContext *cct, J *journaler, cls::journal::Client *client,
                 journal::ImageClientMeta *client_meta, uint64_t *tag_tid,
                 journal::TagData *tag_data, Context *on_finish)
    : cct(cct), journaler(journaler), client(client), client_meta(client_meta),
      tag_tid(tag_tid), tag_data(tag_data), on_finish(on_finish), lock("lock") {
  }

  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * GET_CLIENT * * * * * * * * * * * *
   *    |                             *
   *    v                             *
   * GET_TAGS * * * * * * * * * * * * * (error)
   *    |                             *
   *    v                             *
   * <finish> * * * * * * * * * * * * *
   *
   * @endverbatim
   */

  void send() {
    send_get_client();
  }

  void send_get_client() {
    ldout(cct, 20) << __func__ << dendl;

    FunctionContext *ctx = new FunctionContext(
      [this](int r) {
        handle_get_client(r);
      });
    journaler->get_client(Journal<ImageCtx>::IMAGE_CLIENT_ID, client, ctx);
  }

  void handle_get_client(int r) {
    ldout(cct, 20) << __func__ << ": r=" << r << dendl;

    if (r < 0) {
      complete(r);
      return;
    }

    librbd::journal::ClientData client_data;
    bufferlist::iterator bl_it = client->data.begin();
    try {
      ::decode(client_data, bl_it);
    } catch (const buffer::error &err) {
      lderr(cct) << this << " OpenJournalerRequest::" << __func__ << ": "
                 << "failed to decode client data" << dendl;
      complete(-EBADMSG);
      return;
    }

    journal::ImageClientMeta *image_client_meta =
      boost::get<journal::ImageClientMeta>(&client_data.client_meta);
    if (image_client_meta == nullptr) {
      lderr(cct) << this << " OpenJournalerRequest::" << __func__ << ": "
                 << "failed to get client meta" << dendl;
      complete(-EINVAL);
      return;
    }
    *client_meta = *image_client_meta;

    send_get_tags();
  }

  void send_get_tags() {
    ldout(cct, 20) << __func__ << dendl;

    FunctionContext *ctx = new FunctionContext(
      [this](int r) {
        handle_get_tags(r);
      });
    C_DecodeTags *tags_ctx = new C_DecodeTags(cct, &lock, tag_tid, tag_data,
                                              ctx);
    journaler->get_tags(client_meta->tag_class, &tags_ctx->tags, tags_ctx);
  }

  void handle_get_tags(int r) {
    ldout(cct, 20) << __func__ << ": r=" << r << dendl;

    complete(r);
  }

  void complete(int r) {
    on_finish->complete(r);
    delete this;
  }
};

template <typename J>
void get_tags(CephContext *cct, J *journaler,
              cls::journal::Client *client,
              journal::ImageClientMeta *client_meta,
              uint64_t *tag_tid, journal::TagData *tag_data,
              Context *on_finish) {
  ldout(cct, 20) << __func__ << dendl;

  GetTagsRequest<J> *req =
    new GetTagsRequest<J>(cct, journaler, client, client_meta, tag_tid,
                          tag_data, on_finish);
  req->send();
}

template <typename J>
int allocate_journaler_tag(CephContext *cct, J *journaler,
                           uint64_t tag_class,
                           const journal::TagPredecessor &predecessor,
                           const std::string &mirror_uuid,
                           cls::journal::Tag *new_tag) {
  journal::TagData tag_data;
  tag_data.mirror_uuid = mirror_uuid;
  tag_data.predecessor = predecessor;

  bufferlist tag_bl;
  ::encode(tag_data, tag_bl);

  C_SaferCond allocate_tag_ctx;
  journaler->allocate_tag(tag_class, tag_bl, new_tag, &allocate_tag_ctx);

  int r = allocate_tag_ctx.wait();
  if (r < 0) {
    lderr(cct) << __func__ << ": "
               << "failed to allocate tag: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

} // anonymous namespace

// client id for local image
template <typename I>
const std::string Journal<I>::IMAGE_CLIENT_ID("");

// mirror uuid to use for local images
template <typename I>
const std::string Journal<I>::LOCAL_MIRROR_UUID("");

// mirror uuid to use for orphaned (demoted) images
template <typename I>
const std::string Journal<I>::ORPHAN_MIRROR_UUID("<orphan>");

template <typename I>
std::ostream &operator<<(std::ostream &os,
                         const typename Journal<I>::State &state) {
  switch (state) {
  case Journal<I>::STATE_UNINITIALIZED:
    os << "Uninitialized";
    break;
  case Journal<I>::STATE_INITIALIZING:
    os << "Initializing";
    break;
  case Journal<I>::STATE_REPLAYING:
    os << "Replaying";
    break;
  case Journal<I>::STATE_FLUSHING_RESTART:
    os << "FlushingRestart";
    break;
  case Journal<I>::STATE_RESTARTING_REPLAY:
    os << "RestartingReplay";
    break;
  case Journal<I>::STATE_FLUSHING_REPLAY:
    os << "FlushingReplay";
    break;
  case Journal<I>::STATE_READY:
    os << "Ready";
    break;
  case Journal<I>::STATE_STOPPING:
    os << "Stopping";
    break;
  case Journal<I>::STATE_CLOSING:
    os << "Closing";
    break;
  case Journal<I>::STATE_CLOSED:
    os << "Closed";
    break;
  default:
    os << "Unknown (" << static_cast<uint32_t>(state) << ")";
    break;
  }
  return os;
}

template <typename I>
Journal<I>::Journal(I &image_ctx)
  : m_image_ctx(image_ctx), m_journaler(NULL),
    m_lock("Journal<I>::m_lock"), m_state(STATE_UNINITIALIZED),
    m_error_result(0), m_replay_handler(this), m_close_pending(false),
    m_event_lock("Journal<I>::m_event_lock"), m_event_tid(0),
    m_blocking_writes(false), m_journal_replay(NULL),
    m_metadata_listener(this) {

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 5) << this << ": ictx=" << &m_image_ctx << dendl;

  ThreadPoolSingleton *thread_pool_singleton;
  cct->lookup_or_create_singleton_object<ThreadPoolSingleton>(
    thread_pool_singleton, "librbd::journal::thread_pool");
  m_work_queue = new ContextWQ("librbd::journal::work_queue",
                               cct->_conf->get_val<int64_t>("rbd_op_thread_timeout"),
                               thread_pool_singleton);
  ImageCtx::get_timer_instance(cct, &m_timer, &m_timer_lock);
}

template <typename I>
Journal<I>::~Journal() {
  if (m_work_queue != nullptr) {
    m_work_queue->drain();
    delete m_work_queue;
  }

  assert(m_state == STATE_UNINITIALIZED || m_state == STATE_CLOSED);
  assert(m_journaler == NULL);
  assert(m_journal_replay == NULL);
  assert(m_wait_for_state_contexts.empty());
}

template <typename I>
bool Journal<I>::is_journal_supported(I &image_ctx) {
  assert(image_ctx.snap_lock.is_locked());
  return ((image_ctx.features & RBD_FEATURE_JOURNALING) &&
          !image_ctx.read_only && image_ctx.snap_id == CEPH_NOSNAP);
}

template <typename I>
int Journal<I>::create(librados::IoCtx &io_ctx, const std::string &image_id,
                       uint8_t order, uint8_t splay_width,
                       const std::string &object_pool) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  ThreadPool *thread_pool;
  ContextWQ *op_work_queue;
  ImageCtx::get_thread_pool_instance(cct, &thread_pool, &op_work_queue);

  C_SaferCond cond;
  journal::TagData tag_data(LOCAL_MIRROR_UUID);
  journal::CreateRequest<I> *req = journal::CreateRequest<I>::create(
    io_ctx, image_id, order, splay_width, object_pool, cls::journal::Tag::TAG_CLASS_NEW,
    tag_data, IMAGE_CLIENT_ID, op_work_queue, &cond);
  req->send();

  return cond.wait();
}

template <typename I>
int Journal<I>::remove(librados::IoCtx &io_ctx, const std::string &image_id) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  ThreadPool *thread_pool;
  ContextWQ *op_work_queue;
  ImageCtx::get_thread_pool_instance(cct, &thread_pool, &op_work_queue);

  C_SaferCond cond;
  journal::RemoveRequest<I> *req = journal::RemoveRequest<I>::create(
    io_ctx, image_id, IMAGE_CLIENT_ID, op_work_queue, &cond);
  req->send();

  return cond.wait();
}

template <typename I>
int Journal<I>::reset(librados::IoCtx &io_ctx, const std::string &image_id) {
  CephContext *cct = reinterpret_cast<CephContext *>(io_ctx.cct());
  ldout(cct, 5) << __func__ << ": image=" << image_id << dendl;

  Journaler journaler(io_ctx, image_id, IMAGE_CLIENT_ID, {});

  C_SaferCond cond;
  journaler.init(&cond);
  BOOST_SCOPE_EXIT_ALL(&journaler) {
    journaler.shut_down();
  };

  int r = cond.wait();
  if (r == -ENOENT) {
    return 0;
  } else if (r < 0) {
    lderr(cct) << __func__ << ": "
               << "failed to initialize journal: " << cpp_strerror(r) << dendl;
    return r;
  }

  uint8_t order, splay_width;
  int64_t pool_id;
  journaler.get_metadata(&order, &splay_width, &pool_id);

  std::string pool_name;
  if (pool_id != -1) {
    librados::Rados rados(io_ctx);
    r = rados.pool_reverse_lookup(pool_id, &pool_name);
    if (r < 0) {
      lderr(cct) << __func__ << ": "
                 << "failed to lookup data pool: " << cpp_strerror(r) << dendl;
      return r;
    }
  }

  C_SaferCond ctx1;
  journaler.remove(true, &ctx1);
  r = ctx1.wait();
  if (r < 0) {
    lderr(cct) << __func__ << ": "
               << "failed to reset journal: " << cpp_strerror(r) << dendl;
    return r;
  }

  r = create(io_ctx, image_id, order, splay_width, pool_name);
  if (r < 0) {
    lderr(cct) << __func__ << ": "
               << "failed to create journal: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

template <typename I>
void Journal<I>::is_tag_owner(I *image_ctx, bool *owner,
                              Context *on_finish) {
  Journal<I>::is_tag_owner(image_ctx->md_ctx, image_ctx->id, owner,
                           image_ctx->op_work_queue, on_finish);
}

template <typename I>
void Journal<I>::is_tag_owner(librados::IoCtx& io_ctx, std::string& image_id,
                              bool *is_tag_owner, ContextWQ *op_work_queue,
                              Context *on_finish) {
  CephContext *cct = reinterpret_cast<CephContext*>(io_ctx.cct());
  ldout(cct, 20) << __func__ << dendl;

  C_IsTagOwner<I> *is_tag_owner_ctx =  new C_IsTagOwner<I>(
    io_ctx, image_id, is_tag_owner, op_work_queue, on_finish);
  get_tags(cct, is_tag_owner_ctx->journaler, &is_tag_owner_ctx->client,
	   &is_tag_owner_ctx->client_meta, &is_tag_owner_ctx->tag_tid,
	   &is_tag_owner_ctx->tag_data, is_tag_owner_ctx);
}

template <typename I>
void Journal<I>::get_tag_owner(IoCtx& io_ctx, std::string& image_id,
                               std::string *mirror_uuid,
                               ContextWQ *op_work_queue, Context *on_finish) {
  CephContext *cct = (CephContext *)io_ctx.cct();
  ldout(cct, 20) << __func__ << dendl;

  auto ctx = new C_GetTagOwner(io_ctx, image_id, mirror_uuid, on_finish);
  get_tags(cct, &ctx->journaler, &ctx->client, &ctx->client_meta, &ctx->tag_tid,
           &ctx->tag_data, create_async_context_callback(op_work_queue, ctx));
}

template <typename I>
int Journal<I>::request_resync(I *image_ctx) {
  CephContext *cct = image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  Journaler journaler(image_ctx->md_ctx, image_ctx->id, IMAGE_CLIENT_ID, {});

  Mutex lock("lock");
  journal::ImageClientMeta client_meta;
  uint64_t tag_tid;
  journal::TagData tag_data;

  C_SaferCond open_ctx;
  auto open_req = journal::OpenRequest<I>::create(image_ctx, &journaler, &lock,
                                                  &client_meta, &tag_tid,
                                                  &tag_data, &open_ctx);
  open_req->send();

  BOOST_SCOPE_EXIT_ALL(&journaler) {
    journaler.shut_down();
  };

  int r = open_ctx.wait();
  if (r < 0) {
    return r;
  }

  client_meta.resync_requested = true;

  journal::ClientData client_data(client_meta);
  bufferlist client_data_bl;
  ::encode(client_data, client_data_bl);

  C_SaferCond update_client_ctx;
  journaler.update_client(client_data_bl, &update_client_ctx);

  r = update_client_ctx.wait();
  if (r < 0) {
    lderr(cct) << __func__ << ": "
               << "failed to update client: " << cpp_strerror(r) << dendl;
    return r;
  }
  return 0;
}

template <typename I>
void Journal<I>::promote(I *image_ctx, Context *on_finish) {
  CephContext *cct = image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  auto promote_req = journal::PromoteRequest<I>::create(image_ctx, false,
                                                        on_finish);
  promote_req->send();
}

template <typename I>
void Journal<I>::demote(I *image_ctx, Context *on_finish) {
  CephContext *cct = image_ctx->cct;
  ldout(cct, 20) << __func__ << dendl;

  auto req = journal::DemoteRequest<I>::create(*image_ctx, on_finish);
  req->send();
}

template <typename I>
bool Journal<I>::is_journal_ready() const {
  Mutex::Locker locker(m_lock);
  return (m_state == STATE_READY);
}

template <typename I>
bool Journal<I>::is_journal_replaying() const {
  Mutex::Locker locker(m_lock);
  return is_journal_replaying(m_lock);
}

template <typename I>
bool Journal<I>::is_journal_replaying(const Mutex &) const {
  assert(m_lock.is_locked());
  return (m_state == STATE_REPLAYING ||
          m_state == STATE_FLUSHING_REPLAY ||
          m_state == STATE_FLUSHING_RESTART ||
          m_state == STATE_RESTARTING_REPLAY);
}

template <typename I>
bool Journal<I>::is_journal_appending() const {
  assert(m_image_ctx.snap_lock.is_locked());
  Mutex::Locker locker(m_lock);
  return (m_state == STATE_READY &&
          !m_image_ctx.get_journal_policy()->append_disabled());
}

template <typename I>
void Journal<I>::wait_for_journal_ready(Context *on_ready) {
  on_ready = create_async_context_callback(m_image_ctx, on_ready);

  Mutex::Locker locker(m_lock);
  if (m_state == STATE_READY) {
    on_ready->complete(m_error_result);
  } else {
    wait_for_steady_state(on_ready);
  }
}

template <typename I>
void Journal<I>::open(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  on_finish = create_async_context_callback(m_image_ctx, on_finish);

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_UNINITIALIZED);
  wait_for_steady_state(on_finish);
  create_journaler();
}

template <typename I>
void Journal<I>::close(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  on_finish = create_async_context_callback(m_image_ctx, on_finish);

  Mutex::Locker locker(m_lock);
  while (m_listener_notify) {
    m_listener_cond.Wait(m_lock);
  }

  Listeners listeners(m_listeners);
  m_listener_notify = true;
  m_lock.Unlock();
  for (auto listener : listeners) {
    listener->handle_close();
  }

  m_lock.Lock();
  m_listener_notify = false;
  m_listener_cond.Signal();

  assert(m_state != STATE_UNINITIALIZED);
  if (m_state == STATE_CLOSED) {
    on_finish->complete(m_error_result);
    return;
  }

  if (m_state == STATE_READY) {
    stop_recording();
  }

  m_close_pending = true;
  wait_for_steady_state(on_finish);
}

template <typename I>
bool Journal<I>::is_tag_owner() const {
  Mutex::Locker locker(m_lock);
  return is_tag_owner(m_lock);
}

template <typename I>
bool Journal<I>::is_tag_owner(const Mutex &) const {
  assert(m_lock.is_locked());
  return (m_tag_data.mirror_uuid == LOCAL_MIRROR_UUID);
}

template <typename I>
uint64_t Journal<I>::get_tag_tid() const {
  Mutex::Locker locker(m_lock);
  return m_tag_tid;
}

template <typename I>
journal::TagData Journal<I>::get_tag_data() const {
  Mutex::Locker locker(m_lock);
  return m_tag_data;
}

template <typename I>
void Journal<I>::allocate_local_tag(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  journal::TagPredecessor predecessor;
  predecessor.mirror_uuid = LOCAL_MIRROR_UUID;
  {
    Mutex::Locker locker(m_lock);
    assert(m_journaler != nullptr && is_tag_owner(m_lock));

    cls::journal::Client client;
    int r = m_journaler->get_cached_client(IMAGE_CLIENT_ID, &client);
    if (r < 0) {
      lderr(cct) << this << " " << __func__ << ": "
                 << "failed to retrieve client: " << cpp_strerror(r) << dendl;
      m_image_ctx.op_work_queue->queue(on_finish, r);
      return;
    }

    // since we are primary, populate the predecessor with our known commit
    // position
    assert(m_tag_data.mirror_uuid == LOCAL_MIRROR_UUID);
    if (!client.commit_position.object_positions.empty()) {
      auto position = client.commit_position.object_positions.front();
      predecessor.commit_valid = true;
      predecessor.tag_tid = position.tag_tid;
      predecessor.entry_tid = position.entry_tid;
    }
  }

  allocate_tag(LOCAL_MIRROR_UUID, predecessor, on_finish);
}

template <typename I>
void Journal<I>::allocate_tag(const std::string &mirror_uuid,
                              const journal::TagPredecessor &predecessor,
                              Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ":  mirror_uuid=" << mirror_uuid
                 << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_journaler != nullptr);

  journal::TagData tag_data;
  tag_data.mirror_uuid = mirror_uuid;
  tag_data.predecessor = predecessor;

  bufferlist tag_bl;
  ::encode(tag_data, tag_bl);

  C_DecodeTag *decode_tag_ctx = new C_DecodeTag(cct, &m_lock, &m_tag_tid,
                                                &m_tag_data, on_finish);
  m_journaler->allocate_tag(m_tag_class, tag_bl, &decode_tag_ctx->tag,
                            decode_tag_ctx);
}

template <typename I>
void Journal<I>::flush_commit_position(Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_journaler != nullptr);
  m_journaler->flush_commit_position(on_finish);
}

template <typename I>
uint64_t Journal<I>::append_write_event(uint64_t offset, size_t length,
                                        const bufferlist &bl,
                                        const IOObjectRequests &requests,
                                        bool flush_entry) {
  assert(m_max_append_size > journal::AioWriteEvent::get_fixed_size());
  uint64_t max_write_data_size =
    m_max_append_size - journal::AioWriteEvent::get_fixed_size();

  // ensure that the write event fits within the journal entry
  Bufferlists bufferlists;
  uint64_t bytes_remaining = length;
  uint64_t event_offset = 0;
  do {
    uint64_t event_length = MIN(bytes_remaining, max_write_data_size);

    bufferlist event_bl;
    event_bl.substr_of(bl, event_offset, event_length);
    journal::EventEntry event_entry(journal::AioWriteEvent(offset + event_offset,
                                                           event_length,
                                                           event_bl),
                                    ceph_clock_now());

    bufferlists.emplace_back();
    ::encode(event_entry, bufferlists.back());

    event_offset += event_length;
    bytes_remaining -= event_length;
  } while (bytes_remaining > 0);

  return append_io_events(journal::EVENT_TYPE_AIO_WRITE, bufferlists, requests,
                          offset, length, flush_entry, 0);
}

template <typename I>
uint64_t Journal<I>::append_io_event(journal::EventEntry &&event_entry,
                                     const IOObjectRequests &requests,
                                     uint64_t offset, size_t length,
                                     bool flush_entry, int filter_ret_val) {
  bufferlist bl;
  event_entry.timestamp = ceph_clock_now();
  ::encode(event_entry, bl);
  return append_io_events(event_entry.get_event_type(), {bl}, requests, offset,
                          length, flush_entry, filter_ret_val);
}

template <typename I>
uint64_t Journal<I>::append_io_events(journal::EventType event_type,
                                      const Bufferlists &bufferlists,
                                      const IOObjectRequests &requests,
                                      uint64_t offset, size_t length,
                                      bool flush_entry, int filter_ret_val) {
  assert(!bufferlists.empty());

  uint64_t tid;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);

    tid = ++m_event_tid;
    assert(tid != 0);
  }

  Futures futures;
  for (auto &bl : bufferlists) {
    assert(bl.length() <= m_max_append_size);
    futures.push_back(m_journaler->append(m_tag_tid, bl));
  }

  {
    Mutex::Locker event_locker(m_event_lock);
    m_events[tid] = Event(futures, requests, offset, length, filter_ret_val);
  }

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "event=" << event_type << ", "
                 << "new_reqs=" << requests.size() << ", "
                 << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "flush=" << flush_entry << ", tid=" << tid << dendl;

  Context *on_safe = create_async_context_callback(
    m_image_ctx, new C_IOEventSafe(this, tid));
  if (flush_entry) {
    futures.back().flush(on_safe);
  } else {
    futures.back().wait(on_safe);
  }

  return tid;
}

template <typename I>
void Journal<I>::commit_io_event(uint64_t tid, int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 "r=" << r << dendl;

  Mutex::Locker event_locker(m_event_lock);
  typename Events::iterator it = m_events.find(tid);
  if (it == m_events.end()) {
    return;
  }
  complete_event(it, r);
}

template <typename I>
void Journal<I>::commit_io_event_extent(uint64_t tid, uint64_t offset,
                                        uint64_t length, int r) {
  assert(length > 0);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "offset=" << offset << ", "
                 << "length=" << length << ", "
                 << "r=" << r << dendl;

  Mutex::Locker event_locker(m_event_lock);
  typename Events::iterator it = m_events.find(tid);
  if (it == m_events.end()) {
    return;
  }

  Event &event = it->second;
  if (event.ret_val == 0 && r < 0) {
    event.ret_val = r;
  }

  ExtentInterval extent;
  extent.insert(offset, length);

  ExtentInterval intersect;
  intersect.intersection_of(extent, event.pending_extents);

  event.pending_extents.subtract(intersect);
  if (!event.pending_extents.empty()) {
    ldout(cct, 20) << this << " " << __func__ << ": "
                   << "pending extents: " << event.pending_extents << dendl;
    return;
  }
  complete_event(it, event.ret_val);
}

template <typename I>
void Journal<I>::append_op_event(uint64_t op_tid,
                                 journal::EventEntry &&event_entry,
                                 Context *on_safe) {
  assert(m_image_ctx.owner_lock.is_locked());

  bufferlist bl;
  event_entry.timestamp = ceph_clock_now();
  ::encode(event_entry, bl);

  Future future;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);

    future = m_journaler->append(m_tag_tid, bl);

    // delay committing op event to ensure consistent replay
    assert(m_op_futures.count(op_tid) == 0);
    m_op_futures[op_tid] = future;
  }

  on_safe = create_async_context_callback(m_image_ctx, on_safe);
  on_safe = new FunctionContext([this, on_safe](int r) {
      // ensure all committed IO before this op is committed
      m_journaler->flush_commit_position(on_safe);
    });
  future.flush(on_safe);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": "
                 << "op_tid=" << op_tid << ", "
                 << "event=" << event_entry.get_event_type() << dendl;
}

template <typename I>
void Journal<I>::commit_op_event(uint64_t op_tid, int r, Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": op_tid=" << op_tid << ", "
                 << "r=" << r << dendl;

  journal::EventEntry event_entry((journal::OpFinishEvent(op_tid, r)),
                                  ceph_clock_now());

  bufferlist bl;
  ::encode(event_entry, bl);

  Future op_start_future;
  Future op_finish_future;
  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_READY);

    // ready to commit op event
    auto it = m_op_futures.find(op_tid);
    assert(it != m_op_futures.end());
    op_start_future = it->second;
    m_op_futures.erase(it);

    op_finish_future = m_journaler->append(m_tag_tid, bl);
  }

  op_finish_future.flush(create_async_context_callback(
    m_image_ctx, new C_OpEventSafe(this, op_tid, op_start_future,
                                   op_finish_future, on_safe)));
}

template <typename I>
void Journal<I>::replay_op_ready(uint64_t op_tid, Context *on_resume) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 10) << this << " " << __func__ << ": op_tid=" << op_tid << dendl;

  {
    Mutex::Locker locker(m_lock);
    assert(m_journal_replay != nullptr);
    m_journal_replay->replay_op_ready(op_tid, on_resume);
  }
}

template <typename I>
void Journal<I>::flush_event(uint64_t tid, Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "on_safe=" << on_safe << dendl;

  Future future;
  {
    Mutex::Locker event_locker(m_event_lock);
    future = wait_event(m_lock, tid, on_safe);
  }

  if (future.is_valid()) {
    future.flush(nullptr);
  }
}

template <typename I>
void Journal<I>::wait_event(uint64_t tid, Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << tid << ", "
                 << "on_safe=" << on_safe << dendl;

  Mutex::Locker event_locker(m_event_lock);
  wait_event(m_lock, tid, on_safe);
}

template <typename I>
typename Journal<I>::Future Journal<I>::wait_event(Mutex &lock, uint64_t tid,
                                                   Context *on_safe) {
  assert(m_event_lock.is_locked());
  CephContext *cct = m_image_ctx.cct;

  typename Events::iterator it = m_events.find(tid);
  assert(it != m_events.end());

  Event &event = it->second;
  if (event.safe) {
    // journal entry already safe
    ldout(cct, 20) << this << " " << __func__ << ": "
                   << "journal entry already safe" << dendl;
    m_image_ctx.op_work_queue->queue(on_safe, event.ret_val);
    return Future();
  }

  event.on_safe_contexts.push_back(create_async_context_callback(m_image_ctx,
                                                                 on_safe));
  return event.futures.back();
}

template <typename I>
void Journal<I>::start_external_replay(journal::Replay<I> **journal_replay,
                                       Context *on_start) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_READY);
  assert(m_journal_replay == nullptr);

  on_start = util::create_async_context_callback(m_image_ctx, on_start);
  on_start = new FunctionContext(
    [this, journal_replay, on_start](int r) {
      handle_start_external_replay(r, journal_replay, on_start);
    });

  // safely flush all in-flight events before starting external replay
  m_journaler->stop_append(util::create_async_context_callback(m_image_ctx,
                                                               on_start));
}

template <typename I>
void Journal<I>::handle_start_external_replay(int r,
                                              journal::Replay<I> **journal_replay,
                                              Context *on_finish) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_READY);
  assert(m_journal_replay == nullptr);

  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to stop recording: " << cpp_strerror(r) << dendl;
    *journal_replay = nullptr;

    // get back to a sane-state
    start_append();
    on_finish->complete(r);
    return;
  }

  transition_state(STATE_REPLAYING, 0);
  m_journal_replay = journal::Replay<I>::create(m_image_ctx);
  *journal_replay = m_journal_replay;
  on_finish->complete(0);
}

template <typename I>
void Journal<I>::stop_external_replay() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_journal_replay != nullptr);
  assert(m_state == STATE_REPLAYING);

  delete m_journal_replay;
  m_journal_replay = nullptr;

  if (m_close_pending) {
    destroy_journaler(0);
    return;
  }

  start_append();
}

template <typename I>
void Journal<I>::create_journaler() {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(m_lock.is_locked());
  assert(m_state == STATE_UNINITIALIZED || m_state == STATE_RESTARTING_REPLAY);
  assert(m_journaler == NULL);

  transition_state(STATE_INITIALIZING, 0);
  ::journal::Settings settings;
  settings.commit_interval = m_image_ctx.journal_commit_age;
  settings.max_payload_bytes = m_image_ctx.journal_max_payload_bytes;
  settings.max_concurrent_object_sets =
    m_image_ctx.journal_max_concurrent_object_sets;
  // TODO: a configurable filter to exclude certain peers from being
  // disconnected.
  settings.whitelisted_laggy_clients = {IMAGE_CLIENT_ID};

  m_journaler = new Journaler(m_work_queue, m_timer, m_timer_lock,
			      m_image_ctx.md_ctx, m_image_ctx.id,
			      IMAGE_CLIENT_ID, settings);
  m_journaler->add_listener(&m_metadata_listener);

  Context *ctx = create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_open>(this));
  auto open_req = journal::OpenRequest<I>::create(&m_image_ctx, m_journaler,
                                                  &m_lock, &m_client_meta,
                                                  &m_tag_tid, &m_tag_data, ctx);
  open_req->send();
}

template <typename I>
void Journal<I>::destroy_journaler(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  assert(m_lock.is_locked());

  delete m_journal_replay;
  m_journal_replay = NULL;

  m_journaler->remove_listener(&m_metadata_listener);

  transition_state(STATE_CLOSING, r);

  Context *ctx = create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_journal_destroyed>(this));
  ctx = new FunctionContext(
    [this, ctx](int r) {
      Mutex::Locker locker(m_lock);
      m_journaler->shut_down(ctx);
    });
  m_async_journal_op_tracker.wait(m_image_ctx, ctx);
}

template <typename I>
void Journal<I>::recreate_journaler(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  assert(m_lock.is_locked());
  assert(m_state == STATE_FLUSHING_RESTART ||
         m_state == STATE_FLUSHING_REPLAY);

  delete m_journal_replay;
  m_journal_replay = NULL;

  m_journaler->remove_listener(&m_metadata_listener);

  transition_state(STATE_RESTARTING_REPLAY, r);
  m_journaler->shut_down(create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_journal_destroyed>(this)));
}

template <typename I>
void Journal<I>::complete_event(typename Events::iterator it, int r) {
  assert(m_event_lock.is_locked());
  assert(m_state == STATE_READY);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": tid=" << it->first << " "
                 << "r=" << r << dendl;

  Event &event = it->second;
  if (r < 0 && r == event.filter_ret_val) {
    // ignore allowed error codes
    r = 0;
  }
  if (r < 0) {
    // event recorded to journal but failed to update disk, we cannot
    // commit this IO event. this event must be replayed.
    assert(event.safe);
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to commit IO to disk, replay required: "
               << cpp_strerror(r) << dendl;
  }

  event.committed_io = true;
  if (event.safe) {
    if (r >= 0) {
      for (auto &future : event.futures) {
        m_journaler->committed(future);
      }
    }
    m_events.erase(it);
  }
}

template <typename I>
void Journal<I>::start_append() {
  assert(m_lock.is_locked());
  m_journaler->start_append(m_image_ctx.journal_object_flush_interval,
			    m_image_ctx.journal_object_flush_bytes,
			    m_image_ctx.journal_object_flush_age);
  transition_state(STATE_READY, 0);
}

template <typename I>
void Journal<I>::handle_open(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_INITIALIZING);

  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to initialize journal: " << cpp_strerror(r)
               << dendl;
    destroy_journaler(r);
    return;
  }

  m_tag_class = m_client_meta.tag_class;
  m_max_append_size = m_journaler->get_max_append_size();
  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "tag_class=" << m_tag_class << ", "
                 << "max_append_size=" << m_max_append_size << dendl;

  transition_state(STATE_REPLAYING, 0);
  m_journal_replay = journal::Replay<I>::create(m_image_ctx);
  m_journaler->start_replay(&m_replay_handler);
}

template <typename I>
void Journal<I>::handle_replay_ready() {
  CephContext *cct = m_image_ctx.cct;
  ReplayEntry replay_entry;
  {
    Mutex::Locker locker(m_lock);
    if (m_state != STATE_REPLAYING) {
      return;
    }

    ldout(cct, 20) << this << " " << __func__ << dendl;
    if (!m_journaler->try_pop_front(&replay_entry)) {
      return;
    }

    // only one entry should be in-flight at a time
    assert(!m_processing_entry);
    m_processing_entry = true;
  }

  bufferlist data = replay_entry.get_data();
  bufferlist::iterator it = data.begin();

  journal::EventEntry event_entry;
  int r = m_journal_replay->decode(&it, &event_entry);
  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to decode journal event entry" << dendl;
    handle_replay_process_safe(replay_entry, r);
    return;
  }

  Context *on_ready = create_context_callback<
    Journal<I>, &Journal<I>::handle_replay_process_ready>(this);
  Context *on_commit = new C_ReplayProcessSafe(this, std::move(replay_entry));
  m_journal_replay->process(event_entry, on_ready, on_commit);
}

template <typename I>
void Journal<I>::handle_replay_complete(int r) {
  CephContext *cct = m_image_ctx.cct;

  bool cancel_ops = false;
  {
    Mutex::Locker locker(m_lock);
    if (m_state != STATE_REPLAYING) {
      return;
    }

    ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;
    if (r < 0) {
      cancel_ops = true;
      transition_state(STATE_FLUSHING_RESTART, r);
    } else {
      // state might change back to FLUSHING_RESTART on flush error
      transition_state(STATE_FLUSHING_REPLAY, 0);
    }
  }

  Context *ctx = new FunctionContext([this, cct](int r) {
      ldout(cct, 20) << this << " handle_replay_complete: "
                     << "handle shut down replay" << dendl;

      State state;
      {
        Mutex::Locker locker(m_lock);
        assert(m_state == STATE_FLUSHING_RESTART ||
               m_state == STATE_FLUSHING_REPLAY);
        state = m_state;
      }

      if (state == STATE_FLUSHING_RESTART) {
        handle_flushing_restart(0);
      } else {
        handle_flushing_replay();
      }
    });
  ctx = new FunctionContext([this, ctx](int r) {
      // ensure the commit position is flushed to disk
      m_journaler->flush_commit_position(ctx);
    });
  ctx = new FunctionContext([this, cct, cancel_ops, ctx](int r) {
      ldout(cct, 20) << this << " handle_replay_complete: "
                     << "shut down replay" << dendl;
      m_journal_replay->shut_down(cancel_ops, ctx);
    });
  m_journaler->stop_replay(ctx);
}

template <typename I>
void Journal<I>::handle_replay_process_ready(int r) {
  // journal::Replay is ready for more events -- attempt to pop another
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(r == 0);
  {
    Mutex::Locker locker(m_lock);
    assert(m_processing_entry);
    m_processing_entry = false;
  }
  handle_replay_ready();
}

template <typename I>
void Journal<I>::handle_replay_process_safe(ReplayEntry replay_entry, int r) {
  CephContext *cct = m_image_ctx.cct;

  m_lock.Lock();
  assert(m_state == STATE_REPLAYING ||
         m_state == STATE_FLUSHING_RESTART ||
         m_state == STATE_FLUSHING_REPLAY);

  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;
  if (r < 0) {
    if (r != -ECANCELED) {
      lderr(cct) << this << " " << __func__ << ": "
                 << "failed to commit journal event to disk: "
                 << cpp_strerror(r) << dendl;
    }

    if (m_state == STATE_REPLAYING) {
      // abort the replay if we have an error
      transition_state(STATE_FLUSHING_RESTART, r);
      m_lock.Unlock();

      // stop replay, shut down, and restart
      Context* ctx = create_context_callback<
        Journal<I>, &Journal<I>::handle_flushing_restart>(this);
      ctx = new FunctionContext([this, ctx](int r) {
          // ensure the commit position is flushed to disk
          m_journaler->flush_commit_position(ctx);
        });
      ctx = new FunctionContext([this, cct, ctx](int r) {
          ldout(cct, 20) << this << " handle_replay_process_safe: "
                         << "shut down replay" << dendl;
          {
            Mutex::Locker locker(m_lock);
            assert(m_state == STATE_FLUSHING_RESTART);
          }

          m_journal_replay->shut_down(true, ctx);
        });
      m_journaler->stop_replay(ctx);
      return;
    } else if (m_state == STATE_FLUSHING_REPLAY) {
      // end-of-replay flush in-progress -- we need to restart replay
      transition_state(STATE_FLUSHING_RESTART, r);
      m_lock.Unlock();
      return;
    }
  } else {
    // only commit the entry if written successfully
    m_journaler->committed(replay_entry);
  }
  m_lock.Unlock();
}

template <typename I>
void Journal<I>::handle_flushing_restart(int r) {
  Mutex::Locker locker(m_lock);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(r == 0);
  assert(m_state == STATE_FLUSHING_RESTART);
  if (m_close_pending) {
    destroy_journaler(r);
    return;
  }

  recreate_journaler(r);
}

template <typename I>
void Journal<I>::handle_flushing_replay() {
  Mutex::Locker locker(m_lock);

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(m_state == STATE_FLUSHING_REPLAY || m_state == STATE_FLUSHING_RESTART);
  if (m_close_pending) {
    destroy_journaler(0);
    return;
  } else if (m_state == STATE_FLUSHING_RESTART) {
    // failed to replay one-or-more events -- restart
    recreate_journaler(0);
    return;
  }

  delete m_journal_replay;
  m_journal_replay = NULL;

  m_error_result = 0;
  start_append();
}

template <typename I>
void Journal<I>::handle_recording_stopped(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_STOPPING);

  destroy_journaler(r);
}

template <typename I>
void Journal<I>::handle_journal_destroyed(int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << dendl;

  if (r < 0) {
    lderr(cct) << this << " " << __func__
               << "error detected while closing journal: " << cpp_strerror(r)
               << dendl;
  }

  Mutex::Locker locker(m_lock);
  delete m_journaler;
  m_journaler = nullptr;

  assert(m_state == STATE_CLOSING || m_state == STATE_RESTARTING_REPLAY);
  if (m_state == STATE_RESTARTING_REPLAY) {
    create_journaler();
    return;
  }

  transition_state(STATE_CLOSED, r);
}

template <typename I>
void Journal<I>::handle_io_event_safe(int r, uint64_t tid) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << ", "
                 << "tid=" << tid << dendl;

  // journal will be flushed before closing
  assert(m_state == STATE_READY || m_state == STATE_STOPPING);
  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to commit IO event: "  << cpp_strerror(r) << dendl;
  }

  IOObjectRequests aio_object_requests;
  Contexts on_safe_contexts;
  {
    Mutex::Locker event_locker(m_event_lock);
    typename Events::iterator it = m_events.find(tid);
    assert(it != m_events.end());

    Event &event = it->second;
    aio_object_requests.swap(event.aio_object_requests);
    on_safe_contexts.swap(event.on_safe_contexts);

    if (r < 0 || event.committed_io) {
      // failed journal write so IO won't be sent -- or IO extent was
      // overwritten by future IO operations so this was a no-op IO event
      event.ret_val = r;
      for (auto &future : event.futures) {
        m_journaler->committed(future);
      }
    }

    if (event.committed_io) {
      m_events.erase(it);
    } else {
      event.safe = true;
    }
  }

  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "completing tid=" << tid << dendl;
  for (IOObjectRequests::iterator it = aio_object_requests.begin();
       it != aio_object_requests.end(); ++it) {
    if (r < 0) {
      // don't send aio requests if the journal fails -- bubble error up
      (*it)->fail(r);
    } else {
      // send any waiting aio requests now that journal entry is safe
      (*it)->send();
    }
  }

  // alert the cache about the journal event status
  for (Contexts::iterator it = on_safe_contexts.begin();
       it != on_safe_contexts.end(); ++it) {
    (*it)->complete(r);
  }
}

template <typename I>
void Journal<I>::handle_op_event_safe(int r, uint64_t tid,
                                      const Future &op_start_future,
                                      const Future &op_finish_future,
                                      Context *on_safe) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": r=" << r << ", "
                 << "tid=" << tid << dendl;

  // journal will be flushed before closing
  assert(m_state == STATE_READY || m_state == STATE_STOPPING);
  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to commit op event: "  << cpp_strerror(r) << dendl;
  }

  m_journaler->committed(op_start_future);
  m_journaler->committed(op_finish_future);

  // reduce the replay window after committing an op event
  m_journaler->flush_commit_position(on_safe);
}

template <typename I>
void Journal<I>::stop_recording() {
  assert(m_lock.is_locked());
  assert(m_journaler != NULL);

  assert(m_state == STATE_READY);
  transition_state(STATE_STOPPING, 0);

  m_journaler->stop_append(util::create_async_context_callback(
    m_image_ctx, create_context_callback<
      Journal<I>, &Journal<I>::handle_recording_stopped>(this)));
}

template <typename I>
void Journal<I>::transition_state(State state, int r) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": new state=" << state << dendl;
  assert(m_lock.is_locked());
  m_state = state;

  if (m_error_result == 0 && r < 0) {
    m_error_result = r;
  }

  if (is_steady_state()) {
    Contexts wait_for_state_contexts(std::move(m_wait_for_state_contexts));
    for (auto ctx : wait_for_state_contexts) {
      ctx->complete(m_error_result);
    }
  }
}

template <typename I>
bool Journal<I>::is_steady_state() const {
  assert(m_lock.is_locked());
  switch (m_state) {
  case STATE_READY:
  case STATE_CLOSED:
    return true;
  case STATE_UNINITIALIZED:
  case STATE_INITIALIZING:
  case STATE_REPLAYING:
  case STATE_FLUSHING_RESTART:
  case STATE_RESTARTING_REPLAY:
  case STATE_FLUSHING_REPLAY:
  case STATE_STOPPING:
  case STATE_CLOSING:
    break;
  }
  return false;
}

template <typename I>
void Journal<I>::wait_for_steady_state(Context *on_state) {
  assert(m_lock.is_locked());
  assert(!is_steady_state());

  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << ": on_state=" << on_state
                 << dendl;
  m_wait_for_state_contexts.push_back(on_state);
}

template <typename I>
int Journal<I>::is_resync_requested(bool *do_resync) {
  Mutex::Locker l(m_lock);
  return check_resync_requested(do_resync);
}

template <typename I>
int Journal<I>::check_resync_requested(bool *do_resync) {
  CephContext *cct = m_image_ctx.cct;
  ldout(cct, 20) << this << " " << __func__ << dendl;

  assert(m_lock.is_locked());
  assert(do_resync != nullptr);

  cls::journal::Client client;
  int r = m_journaler->get_cached_client(IMAGE_CLIENT_ID, &client);
  if (r < 0) {
     lderr(cct) << this << " " << __func__ << ": "
                << "failed to retrieve client: " << cpp_strerror(r) << dendl;
     return r;
  }

  librbd::journal::ClientData client_data;
  bufferlist::iterator bl_it = client.data.begin();
  try {
    ::decode(client_data, bl_it);
  } catch (const buffer::error &err) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to decode client data: " << err << dendl;
    return -EINVAL;
  }

  journal::ImageClientMeta *image_client_meta =
    boost::get<journal::ImageClientMeta>(&client_data.client_meta);
  if (image_client_meta == nullptr) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to access image client meta struct" << dendl;
    return -EINVAL;
  }

  *do_resync = image_client_meta->resync_requested;

  return 0;
}

struct C_RefreshTags : public Context {
  util::AsyncOpTracker &async_op_tracker;
  Context *on_finish = nullptr;

  Mutex lock;
  uint64_t tag_tid;
  journal::TagData tag_data;

  C_RefreshTags(util::AsyncOpTracker &async_op_tracker)
    : async_op_tracker(async_op_tracker),
      lock("librbd::Journal::C_RefreshTags::lock") {
    async_op_tracker.start_op();
  }
  ~C_RefreshTags() override {
     async_op_tracker.finish_op();
  }

  void finish(int r) override {
    on_finish->complete(r);
  }
};

template <typename I>
void Journal<I>::handle_metadata_updated() {
  CephContext *cct = m_image_ctx.cct;
  Mutex::Locker locker(m_lock);

  if (m_state != STATE_READY && !is_journal_replaying(m_lock)) {
    return;
  } else if (is_tag_owner(m_lock)) {
    ldout(cct, 20) << this << " " << __func__ << ": primary image" << dendl;
    return;
  } else if (m_listeners.empty()) {
    ldout(cct, 20) << this << " " << __func__ << ": no listeners" << dendl;
    return;
  }

  uint64_t refresh_sequence = ++m_refresh_sequence;
  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "refresh_sequence=" << refresh_sequence << dendl;

  // pull the most recent tags from the journal, decode, and
  // update the internal tag state
  C_RefreshTags *refresh_ctx = new C_RefreshTags(m_async_journal_op_tracker);
  refresh_ctx->on_finish = new FunctionContext(
    [this, refresh_sequence, refresh_ctx](int r) {
      handle_refresh_metadata(refresh_sequence, refresh_ctx->tag_tid,
                              refresh_ctx->tag_data, r);
    });
  C_DecodeTags *decode_tags_ctx = new C_DecodeTags(
      cct, &refresh_ctx->lock, &refresh_ctx->tag_tid,
      &refresh_ctx->tag_data, refresh_ctx);
  m_journaler->get_tags(m_tag_tid == 0 ? 0 : m_tag_tid - 1, m_tag_class,
                        &decode_tags_ctx->tags, decode_tags_ctx);
}

template <typename I>
void Journal<I>::handle_refresh_metadata(uint64_t refresh_sequence,
                                         uint64_t tag_tid,
                                         journal::TagData tag_data, int r) {
  CephContext *cct = m_image_ctx.cct;
  Mutex::Locker locker(m_lock);

  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": failed to refresh metadata: "
               << cpp_strerror(r) << dendl;
    return;
  } else if (m_state != STATE_READY && !is_journal_replaying(m_lock)) {
    return;
  } else if (refresh_sequence != m_refresh_sequence) {
    // another, more up-to-date refresh is in-flight
    return;
  }

  ldout(cct, 20) << this << " " << __func__ << ": "
                 << "refresh_sequence=" << refresh_sequence << ", "
                 << "tag_tid=" << tag_tid << ", "
                 << "tag_data=" << tag_data << dendl;
  while (m_listener_notify) {
    m_listener_cond.Wait(m_lock);
  }

  bool was_tag_owner = is_tag_owner(m_lock);
  if (m_tag_tid < tag_tid) {
    m_tag_tid = tag_tid;
    m_tag_data = tag_data;
  }
  bool promoted_to_primary = (!was_tag_owner && is_tag_owner(m_lock));

  bool resync_requested = false;
  r = check_resync_requested(&resync_requested);
  if (r < 0) {
    lderr(cct) << this << " " << __func__ << ": "
               << "failed to check if a resync was requested" << dendl;
    return;
  }

  Listeners listeners(m_listeners);
  m_listener_notify = true;
  m_lock.Unlock();

  if (promoted_to_primary) {
    for (auto listener : listeners) {
      listener->handle_promoted();
    }
  } else if (resync_requested) {
    for (auto listener : listeners) {
      listener->handle_resync();
    }
  }

  m_lock.Lock();
  m_listener_notify = false;
  m_listener_cond.Signal();
}

template <typename I>
void Journal<I>::add_listener(journal::Listener *listener) {
  Mutex::Locker locker(m_lock);
  m_listeners.insert(listener);
}

template <typename I>
void Journal<I>::remove_listener(journal::Listener *listener) {
  Mutex::Locker locker(m_lock);
  while (m_listener_notify) {
    m_listener_cond.Wait(m_lock);
  }
  m_listeners.erase(listener);
}

} // namespace librbd

#ifndef TEST_F
template class librbd::Journal<librbd::ImageCtx>;
#endif
