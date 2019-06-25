// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H
#define CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H

#include "test/rbd_mirror/test_fixture.h"
#include "test/librados_test_stub/LibradosTestStub.h"
#include "common/WorkQueue.h"
#include <boost/shared_ptr.hpp>
#include <gmock/gmock.h>
#include "include/assert.h"

namespace librados {
class TestRadosClient;
class MockTestMemIoCtxImpl;
class MockTestMemRadosClient;
}

namespace librbd {
class MockImageCtx;
}

ACTION_P(CopyInBufferlist, str) {
  arg0->append(str);
}

ACTION_P(CompleteContext, r) {
  arg0->complete(r);
}

ACTION_P2(CompleteContext, wq, r) {
  ContextWQ *context_wq = reinterpret_cast<ContextWQ *>(wq);
  context_wq->queue(arg0, r);
}

MATCHER_P(ContentsEqual, bl, "") {
  // TODO fix const-correctness of bufferlist
  return const_cast<bufferlist &>(arg).contents_equal(
    const_cast<bufferlist &>(bl));
}

namespace rbd {
namespace mirror {

class TestMockFixture : public TestFixture {
public:
  typedef boost::shared_ptr<librados::TestCluster> TestClusterRef;

  static void SetUpTestCase();
  static void TearDownTestCase();

  void TearDown() override;

  void expect_test_features(librbd::MockImageCtx &mock_image_ctx);

private:
  static TestClusterRef s_test_cluster;
};

} // namespace mirror
} // namespace rbd

#endif // CEPH_TEST_RBD_MIRROR_TEST_MOCK_FIXTURE_H
