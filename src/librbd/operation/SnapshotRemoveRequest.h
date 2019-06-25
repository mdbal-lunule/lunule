// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_OPERATION_SNAPSHOT_REMOVE_REQUEST_H
#define CEPH_LIBRBD_OPERATION_SNAPSHOT_REMOVE_REQUEST_H

#include "librbd/operation/Request.h"
#include "librbd/Types.h"
#include <string>

class Context;

namespace librbd {

class ImageCtx;

namespace operation {

template <typename ImageCtxT = ImageCtx>
class SnapshotRemoveRequest : public Request<ImageCtxT> {
public:
  /**
   * Snap Remove goes through the following state machine:
   *
   * @verbatim
   *
   * <start> ------\
   *  .            |
   *  .            v
   *  .     STATE_REMOVE_OBJECT_MAP
   *  .            |            .
   *  .            v            .
   *  . . > STATE_REMOVE_CHILD  .
   *  .            |            .
   *  .            |      . . . .
   *  .            |      .
   *  .            v      v
   *  . . > STATE_REMOVE_SNAP
   *               |
   *               v
   *        STATE_RELEASE_SNAP_ID
   *               |
   *               v
   *           <finish>
   *
   * @endverbatim
   *
   * The _REMOVE_OBJECT_MAP state is skipped if the object map is not enabled.
   * The _REMOVE_CHILD state is skipped if the parent is still in-use.
   */
  enum State {
    STATE_REMOVE_OBJECT_MAP,
    STATE_REMOVE_CHILD,
    STATE_REMOVE_SNAP,
    STATE_RELEASE_SNAP_ID,
    STATE_ERROR
  };

  SnapshotRemoveRequest(ImageCtxT &image_ctx, Context *on_finish,
			const cls::rbd::SnapshotNamespace &snap_namespace,
		        const std::string &snap_name,
			uint64_t snap_id);

protected:
  void send_op() override;
  bool should_complete(int r) override;

  journal::Event create_event(uint64_t op_tid) const override {
    return journal::SnapRemoveEvent(op_tid, m_snap_namespace, m_snap_name);
  }

private:
  cls::rbd::SnapshotNamespace m_snap_namespace;
  std::string m_snap_name;
  uint64_t m_snap_id;
  State m_state;

  int filter_state_return_code(int r) const {
    if (m_state == STATE_REMOVE_CHILD && r == -ENOENT) {
      return 0;
    }
    return r;
  }

  void send_remove_object_map();
  void send_remove_child();
  void send_remove_snap();
  void send_release_snap_id();

  void remove_snap_context();
  int scan_for_parents(ParentSpec &pspec);

};

} // namespace operation
} // namespace librbd

extern template class librbd::operation::SnapshotRemoveRequest<librbd::ImageCtx>;

#endif // CEPH_LIBRBD_OPERATION_SNAPSHOT_REMOVE_REQUEST_H
