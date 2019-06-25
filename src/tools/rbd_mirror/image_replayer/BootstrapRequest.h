// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef RBD_MIRROR_IMAGE_REPLAYER_BOOTSTRAP_REQUEST_H
#define RBD_MIRROR_IMAGE_REPLAYER_BOOTSTRAP_REQUEST_H

#include "include/int_types.h"
#include "include/rados/librados.hpp"
#include "common/Mutex.h"
#include "cls/journal/cls_journal_types.h"
#include "librbd/journal/TypeTraits.h"
#include "tools/rbd_mirror/BaseRequest.h"
#include "tools/rbd_mirror/types.h"
#include <list>
#include <string>

class Context;
class ContextWQ;
class Mutex;
class SafeTimer;
namespace journal { class Journaler; }
namespace librbd { class ImageCtx; }
namespace librbd { namespace journal { struct MirrorPeerClientMeta; } }

namespace rbd {
namespace mirror {

class ProgressContext;

template <typename> class ImageSync;
template <typename> class InstanceWatcher;

namespace image_replayer {

template <typename ImageCtxT = librbd::ImageCtx>
class BootstrapRequest : public BaseRequest {
public:
  typedef librbd::journal::TypeTraits<ImageCtxT> TypeTraits;
  typedef typename TypeTraits::Journaler Journaler;
  typedef librbd::journal::MirrorPeerClientMeta MirrorPeerClientMeta;
  typedef rbd::mirror::ProgressContext ProgressContext;

  static BootstrapRequest* create(
        librados::IoCtx &local_io_ctx,
        librados::IoCtx &remote_io_ctx,
        InstanceWatcher<ImageCtxT> *instance_watcher,
        ImageCtxT **local_image_ctx,
        const std::string &local_image_id,
        const std::string &remote_image_id,
        const std::string &global_image_id,
        ContextWQ *work_queue, SafeTimer *timer,
        Mutex *timer_lock,
        const std::string &local_mirror_uuid,
        const std::string &remote_mirror_uuid,
        Journaler *journaler,
        cls::journal::ClientState *client_state,
        MirrorPeerClientMeta *client_meta,
        Context *on_finish,
        bool *do_resync,
        ProgressContext *progress_ctx = nullptr) {
    return new BootstrapRequest(local_io_ctx, remote_io_ctx,
                                instance_watcher, local_image_ctx,
                                local_image_id, remote_image_id,
                                global_image_id, work_queue, timer, timer_lock,
                                local_mirror_uuid, remote_mirror_uuid,
                                journaler, client_state, client_meta, on_finish,
                                do_resync, progress_ctx);
  }

  BootstrapRequest(librados::IoCtx &local_io_ctx,
                   librados::IoCtx &remote_io_ctx,
                   InstanceWatcher<ImageCtxT> *instance_watcher,
                   ImageCtxT **local_image_ctx,
                   const std::string &local_image_id,
                   const std::string &remote_image_id,
                   const std::string &global_image_id, ContextWQ *work_queue,
                   SafeTimer *timer, Mutex *timer_lock,
                   const std::string &local_mirror_uuid,
                   const std::string &remote_mirror_uuid, Journaler *journaler,
                   cls::journal::ClientState *client_state,
                   MirrorPeerClientMeta *client_meta, Context *on_finish,
                   bool *do_resync, ProgressContext *progress_ctx = nullptr);
  ~BootstrapRequest() override;

  bool is_syncing() const;

  void send() override;
  void cancel() override;

private:
  /**
   * @verbatim
   *
   * <start>
   *    |
   *    v
   * GET_REMOTE_TAG_CLASS * * * * * * * * * * * * * * * * * *
   *    |                                                   * (error)
   *    v                                                   *
   * OPEN_REMOTE_IMAGE  * * * * * * * * * * * * * * * * * * *
   *    |                                                   *
   *    |/--------------------------------------------------*---\
   *    v                                                   *   |
   * IS_PRIMARY * * * * * * * * * * * * * * * * * * * * *   *   |
   *    |                                               *   *   |
   *    | (remote image primary, no local image id)     *   *   |
   *    \----> UPDATE_CLIENT_IMAGE  * * * * * * * * * * *   *   |
   *    |         |                                     *   *   |
   *    |         v                                     *   *   |
   *    \----> CREATE_LOCAL_IMAGE * * * * * * * * * * * *   *   |
   *    |         |                                     *   *   |
   *    |         v                                     *   *   |
   *    | (remote image primary)                        *   *   |
   *    \----> OPEN_LOCAL_IMAGE * * * * * * * * * * * * *   *   |
   *    |         |   .                                 *   *   |
   *    |         |   . (image doesn't exist)           *   *   |
   *    |         |   . . > UNREGISTER_CLIENT * * * * * *   *   |
   *    |         |             |                       *   *   |
   *    |         |             v                       *   *   |
   *    |         |         REGISTER_CLIENT * * * * * * *   *   |
   *    |         |             |                       *   *   |
   *    |         |             \-----------------------*---*---/
   *    |         |                                     *   *
   *    |         v (skip if not needed)                *   *
   *    |      GET_REMOTE_TAGS  * * * * * * *           *   *
   *    |         |                         *           *   *
   *    |         v (skip if not needed)    v           *   *
   *    |      IMAGE_SYNC * * * > CLOSE_LOCAL_IMAGE     *   *
   *    |         |                         |           *   *
   *    |         \-----------------\ /-----/           *   *
   *    |                            |                  *   *
   *    |                            |                  *   *
   *    | (skip if not needed)       |                  *   *
   *    \----> UPDATE_CLIENT_STATE  *|* * * * * * * * * *   *
   *                |                |                  *   *
   *    /-----------/----------------/                  *   *
   *    |                                               *   *
   *    v                                               *   *
   * CLOSE_REMOTE_IMAGE < * * * * * * * * * * * * * * * *   *
   *    |                                                   *
   *    v                                                   *
   * <finish> < * * * * * * * * * * * * * * * * * * * * * * *
   *
   * @endverbatim
   */
  typedef std::list<cls::journal::Tag> Tags;

  librados::IoCtx &m_local_io_ctx;
  librados::IoCtx &m_remote_io_ctx;
  InstanceWatcher<ImageCtxT> *m_instance_watcher;
  ImageCtxT **m_local_image_ctx;
  std::string m_local_image_id;
  std::string m_remote_image_id;
  std::string m_global_image_id;
  ContextWQ *m_work_queue;
  SafeTimer *m_timer;
  Mutex *m_timer_lock;
  std::string m_local_mirror_uuid;
  std::string m_remote_mirror_uuid;
  Journaler *m_journaler;
  cls::journal::ClientState *m_client_state;
  MirrorPeerClientMeta *m_client_meta;
  ProgressContext *m_progress_ctx;
  bool *m_do_resync;

  mutable Mutex m_lock;
  bool m_canceled = false;

  Tags m_remote_tags;
  cls::journal::Client m_client;
  uint64_t m_remote_tag_class = 0;
  ImageCtxT *m_remote_image_ctx = nullptr;
  bool m_primary = false;
  int m_ret_val = 0;
  ImageSync<ImageCtxT> *m_image_sync = nullptr;

  bufferlist m_out_bl;

  void get_remote_tag_class();
  void handle_get_remote_tag_class(int r);

  void open_remote_image();
  void handle_open_remote_image(int r);

  void is_primary();
  void handle_is_primary(int r);

  void update_client_state();
  void handle_update_client_state(int r);

  void open_local_image();
  void handle_open_local_image(int r);

  void unregister_client();
  void handle_unregister_client(int r);

  void register_client();
  void handle_register_client(int r);

  void create_local_image();
  void handle_create_local_image(int r);

  void update_client_image();
  void handle_update_client_image(int r);

  void get_remote_tags();
  void handle_get_remote_tags(int r);

  void image_sync();
  void handle_image_sync(int r);

  void close_local_image();
  void handle_close_local_image(int r);

  void close_remote_image();
  void handle_close_remote_image(int r);

  void update_progress(const std::string &description);
};

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

extern template class rbd::mirror::image_replayer::BootstrapRequest<librbd::ImageCtx>;

#endif // RBD_MIRROR_IMAGE_REPLAYER_BOOTSTRAP_REQUEST_H
