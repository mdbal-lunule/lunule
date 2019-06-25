// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IMAGE_CREATE_REQUEST_H
#define CEPH_LIBRBD_IMAGE_CREATE_REQUEST_H

#include "include/int_types.h"
#include "include/buffer.h"
#include "common/WorkQueue.h"
#include "librbd/ObjectMap.h"
#include "include/rados/librados.hpp"
#include "include/rbd_types.h"
#include "cls/rbd/cls_rbd_types.h"
#include "include/rbd/librbd.hpp"
#include "librbd/ImageCtx.h"
#include "common/Timer.h"
#include "librbd/journal/TypeTraits.h"

class Context;

using librados::IoCtx;

namespace journal {
  class Journaler;
}

namespace librbd {
namespace image {

template <typename ImageCtxT = ImageCtx>
class CreateRequest {
public:
  static CreateRequest *create(IoCtx &ioctx, const std::string &image_name,
                               const std::string &image_id, uint64_t size,
                               const ImageOptions &image_options,
                               const std::string &non_primary_global_image_id,
                               const std::string &primary_mirror_uuid,
                               bool skip_mirror_enable,
                               ContextWQ *op_work_queue, Context *on_finish) {
    return new CreateRequest(ioctx, image_name, image_id, size, image_options,
                             non_primary_global_image_id, primary_mirror_uuid,
                             skip_mirror_enable, op_work_queue,
                             on_finish);
  }

  static int validate_order(CephContext *cct, uint8_t order);

  void send();

private:
  /**
   * @verbatim
   *
   *                                  <start> . . . . > . . . . .
   *                                     |                      .
   *                                     v                      .
   *                               VALIDATE POOL                v (pool validation
   *                                     |                      .  disabled)
   *                                     v                      .
   *                             VALIDATE OVERWRITE             .
   *                                     |                      .
   *                                     v                      .
   * (error: bottom up)           CREATE ID OBJECT. . < . . . . .
   *  _______<_______                    |
   * |               |                   v
   * |               |          ADD IMAGE TO DIRECTORY
   * |               |               /   |
   * |      REMOVE ID OBJECT<-------/    v
   * |               |           NEGOTIATE FEATURES (when using default features)
   * |               |                   |
   * |               |                   v         (stripingv2 disabled)
   * |               |              CREATE IMAGE. . . . > . . . .
   * v               |               /   |                      .
   * |      REMOVE FROM DIR<--------/    v                      .
   * |               |          SET STRIPE UNIT COUNT           .
   * |               |               /   |  \ . . . . . > . . . .
   * |      REMOVE HEADER OBJ<------/    v                     /. (object-map
   * |               |\           OBJECT MAP RESIZE . . < . . * v  disabled)
   * |               | \              /  |  \ . . . . . > . . . .
   * |               |  *<-----------/   v                     /. (journaling
   * |               |             FETCH MIRROR MODE. . < . . * v  disabled)
   * |               |                /   |                     .
   * |     REMOVE OBJECT MAP<--------/    v                     .
   * |               |\             JOURNAL CREATE              .
   * |               | \               /  |                     .
   * v               |  *<------------/   v                     .
   * |               |           MIRROR IMAGE ENABLE            .
   * |               |                /   |                     .
   * |        JOURNAL REMOVE*<-------/    |                     .
   * |                                    v                     .
   * |_____________>___________________<finish> . . . . < . . . .
   *
   * @endverbatim
   */

  CreateRequest(IoCtx &ioctx, const std::string &image_name,
                const std::string &image_id, uint64_t size,
                const ImageOptions &image_options,
                const std::string &non_primary_global_image_id,
                const std::string &primary_mirror_uuid,
                bool skip_mirror_enable,
                ContextWQ *op_work_queue, Context *on_finish);

  IoCtx &m_ioctx;
  IoCtx m_data_io_ctx;
  std::string m_image_name;
  std::string m_image_id;
  uint64_t m_size;
  uint8_t m_order = 0;
  uint64_t m_features = 0;
  uint64_t m_stripe_unit = 0;
  uint64_t m_stripe_count = 0;
  uint8_t m_journal_order = 0;
  uint8_t m_journal_splay_width = 0;
  std::string m_journal_pool;
  std::string m_data_pool;
  int64_t m_data_pool_id = -1;
  const std::string m_non_primary_global_image_id;
  const std::string m_primary_mirror_uuid;
  bool m_skip_mirror_enable;
  bool m_negotiate_features = false;

  ContextWQ *m_op_work_queue;
  Context *m_on_finish;

  CephContext *m_cct;
  int m_r_saved;  // used to return actual error after cleanup
  bool m_force_non_primary;
  file_layout_t m_layout;
  std::string m_id_obj, m_header_obj, m_objmap_name;

  bufferlist m_outbl;
  rbd_mirror_mode_t m_mirror_mode;
  cls::rbd::MirrorImage m_mirror_image_internal;

  void validate_pool();
  void handle_validate_pool(int r);

  void validate_overwrite();
  void handle_validate_overwrite(int r);

  void create_id_object();
  void handle_create_id_object(int r);

  void add_image_to_directory();
  void handle_add_image_to_directory(int r);

  void negotiate_features();
  void handle_negotiate_features(int r);

  void create_image();
  void handle_create_image(int r);

  void set_stripe_unit_count();
  void handle_set_stripe_unit_count(int r);

  void object_map_resize();
  void handle_object_map_resize(int r);

  void fetch_mirror_mode();
  void handle_fetch_mirror_mode(int r);

  void journal_create();
  void handle_journal_create(int r);

  void mirror_image_enable();
  void handle_mirror_image_enable(int r);

  void complete(int r);

  // cleanup
  void journal_remove();
  void handle_journal_remove(int r);

  void remove_object_map();
  void handle_remove_object_map(int r);

  void remove_header_object();
  void handle_remove_header_object(int r);

  void remove_from_dir();
  void handle_remove_from_dir(int r);

  void remove_id_object();
  void handle_remove_id_object(int r);
};

} //namespace image
} //namespace librbd

extern template class librbd::image::CreateRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_IMAGE_CREATE_REQUEST_H
