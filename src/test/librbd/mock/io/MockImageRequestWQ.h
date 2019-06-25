// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_LIBRBD_MOCK_IO_IMAGE_REQUEST_WQ_H
#define CEPH_TEST_LIBRBD_MOCK_IO_IMAGE_REQUEST_WQ_H

#include "gmock/gmock.h"
#include "librbd/io/Types.h"

class Context;

namespace librbd {
namespace io {

struct MockImageRequestWQ {
  MOCK_METHOD1(block_writes, void(Context *));
  MOCK_METHOD0(unblock_writes, void());

  MOCK_METHOD2(set_require_lock, void(Direction, bool));
};

} // namespace io
} // namespace librbd

#endif // CEPH_TEST_LIBRBD_MOCK_IO_IMAGE_REQUEST_WQ_H
