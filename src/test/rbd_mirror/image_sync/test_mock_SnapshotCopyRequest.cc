// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/rbd_mirror/test_mock_fixture.h"
#include "include/rbd/librbd.hpp"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Operations.h"
#include "librbd/journal/TypeTraits.h"
#include "test/journal/mock/MockJournaler.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "tools/rbd_mirror/image_sync/SnapshotCopyRequest.h"
#include "tools/rbd_mirror/image_sync/SnapshotCreateRequest.h"
#include "tools/rbd_mirror/Threads.h"

namespace librbd {

namespace {

struct MockTestImageCtx : public librbd::MockImageCtx {
  MockTestImageCtx(librbd::ImageCtx &image_ctx)
    : librbd::MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace

namespace journal {

template <>
struct TypeTraits<librbd::MockTestImageCtx> {
  typedef ::journal::MockJournaler Journaler;
};

} // namespace journal
} // namespace librbd

namespace rbd {
namespace mirror {
namespace image_sync {

template <>
struct SnapshotCreateRequest<librbd::MockTestImageCtx> {
  static SnapshotCreateRequest* s_instance;
  static SnapshotCreateRequest* create(librbd::MockTestImageCtx* image_ctx,
                                       const std::string &snap_name,
                                       const cls::rbd::SnapshotNamespace &snap_namespace,
                                       uint64_t size,
                                       const librbd::ParentSpec &parent_spec,
                                       uint64_t parent_overlap,
                                       Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->on_finish = on_finish;
    return s_instance;
  }

  Context *on_finish = nullptr;

  SnapshotCreateRequest() {
    s_instance = this;
  }

  MOCK_METHOD0(send, void());
};

SnapshotCreateRequest<librbd::MockTestImageCtx>* SnapshotCreateRequest<librbd::MockTestImageCtx>::s_instance = nullptr;

} // namespace image_sync
} // namespace mirror
} // namespace rbd

// template definitions
#include "tools/rbd_mirror/image_sync/SnapshotCopyRequest.cc"
template class rbd::mirror::image_sync::SnapshotCopyRequest<librbd::MockTestImageCtx>;

namespace rbd {
namespace mirror {
namespace image_sync {

using ::testing::_;
using ::testing::DoAll;
using ::testing::DoDefault;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::Return;
using ::testing::ReturnNew;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::WithArg;

class TestMockImageSyncSnapshotCopyRequest : public TestMockFixture {
public:
  typedef SnapshotCopyRequest<librbd::MockTestImageCtx> MockSnapshotCopyRequest;
  typedef SnapshotCreateRequest<librbd::MockTestImageCtx> MockSnapshotCreateRequest;

  void SetUp() override {
    TestMockFixture::SetUp();

    librbd::RBD rbd;
    ASSERT_EQ(0, create_image(rbd, m_remote_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_remote_io_ctx, m_image_name, &m_remote_image_ctx));

    ASSERT_EQ(0, create_image(rbd, m_local_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_local_io_ctx, m_image_name, &m_local_image_ctx));
  }

  void expect_start_op(librbd::MockExclusiveLock &mock_exclusive_lock) {
    EXPECT_CALL(mock_exclusive_lock, start_op()).WillOnce(
      ReturnNew<FunctionContext>([](int) {}));
  }

  void expect_get_snap_namespace(librbd::MockTestImageCtx &mock_image_ctx,
                                 uint64_t snap_id) {
    EXPECT_CALL(mock_image_ctx, get_snap_namespace(snap_id, _))
      .WillOnce(DoAll(SetArgPointee<1>(cls::rbd::UserSnapshotNamespace()),
                      Return(0)));
  }

  void expect_snap_create(librbd::MockTestImageCtx &mock_image_ctx,
                          MockSnapshotCreateRequest &mock_snapshot_create_request,
                          const std::string &snap_name, uint64_t snap_id, int r) {
    EXPECT_CALL(mock_snapshot_create_request, send())
      .WillOnce(DoAll(Invoke([&mock_image_ctx, snap_id, snap_name]() {
                        inject_snap(mock_image_ctx, snap_id, snap_name);
                      }),
                      Invoke([this, &mock_snapshot_create_request, r]() {
                        m_threads->work_queue->queue(mock_snapshot_create_request.on_finish, r);
                      })));
  }

  void expect_snap_remove(librbd::MockTestImageCtx &mock_image_ctx,
                          const std::string &snap_name, int r) {
    EXPECT_CALL(*mock_image_ctx.operations, execute_snap_remove(_, StrEq(snap_name), _))
                  .WillOnce(WithArg<2>(Invoke([this, r](Context *ctx) {
                              m_threads->work_queue->queue(ctx, r);
                            })));
  }

  void expect_snap_protect(librbd::MockTestImageCtx &mock_image_ctx,
                           const std::string &snap_name, int r) {
    EXPECT_CALL(*mock_image_ctx.operations, execute_snap_protect(_, StrEq(snap_name), _))
                  .WillOnce(WithArg<2>(Invoke([this, r](Context *ctx) {
                              m_threads->work_queue->queue(ctx, r);
                            })));
  }

  void expect_snap_unprotect(librbd::MockTestImageCtx &mock_image_ctx,
                             const std::string &snap_name, int r) {
    EXPECT_CALL(*mock_image_ctx.operations, execute_snap_unprotect(_, StrEq(snap_name), _))
                  .WillOnce(WithArg<2>(Invoke([this, r](Context *ctx) {
                              m_threads->work_queue->queue(ctx, r);
                            })));
  }

  void expect_snap_is_protected(librbd::MockTestImageCtx &mock_image_ctx,
                                uint64_t snap_id, bool is_protected, int r) {
    EXPECT_CALL(mock_image_ctx, is_snap_protected(snap_id, _))
                  .WillOnce(DoAll(SetArgPointee<1>(is_protected),
                                  Return(r)));
  }

  void expect_snap_is_unprotected(librbd::MockTestImageCtx &mock_image_ctx,
                                  uint64_t snap_id, bool is_unprotected, int r) {
    EXPECT_CALL(mock_image_ctx, is_snap_unprotected(snap_id, _))
                  .WillOnce(DoAll(SetArgPointee<1>(is_unprotected),
                                  Return(r)));
  }

  void expect_update_client(journal::MockJournaler &mock_journaler, int r) {
    EXPECT_CALL(mock_journaler, update_client(_, _))
                  .WillOnce(WithArg<1>(CompleteContext(r)));
  }

  static void inject_snap(librbd::MockTestImageCtx &mock_image_ctx,
                          uint64_t snap_id, const std::string &snap_name) {
    mock_image_ctx.snap_ids[{cls::rbd::UserSnapshotNamespace(),
			     snap_name}] = snap_id;
  }

  MockSnapshotCopyRequest *create_request(librbd::MockTestImageCtx &mock_remote_image_ctx,
                                          librbd::MockTestImageCtx &mock_local_image_ctx,
                                          journal::MockJournaler &mock_journaler,
                                          Context *on_finish) {
    return new MockSnapshotCopyRequest(&mock_local_image_ctx,
                                       &mock_remote_image_ctx, &m_snap_map,
                                       &mock_journaler, &m_client_meta,
                                       m_threads->work_queue, on_finish);
  }

  int create_snap(librbd::ImageCtx *image_ctx, const std::string &snap_name,
                  bool protect = false) {
    int r = image_ctx->operations->snap_create(cls::rbd::UserSnapshotNamespace(),
					       snap_name.c_str());
    if (r < 0) {
      return r;
    }

    if (protect) {
      r = image_ctx->operations->snap_protect(cls::rbd::UserSnapshotNamespace(),
					      snap_name.c_str());
      if (r < 0) {
        return r;
      }
    }

    r = image_ctx->state->refresh();
    if (r < 0) {
      return r;
    }
    return 0;
  }

  void validate_snap_seqs(const librbd::journal::MirrorPeerClientMeta::SnapSeqs &snap_seqs) {
    ASSERT_EQ(snap_seqs, m_client_meta.snap_seqs);
  }

  void validate_snap_map(const MockSnapshotCopyRequest::SnapMap &snap_map) {
    ASSERT_EQ(snap_map, m_snap_map);
  }

  librbd::ImageCtx *m_remote_image_ctx;
  librbd::ImageCtx *m_local_image_ctx;

  MockSnapshotCopyRequest::SnapMap m_snap_map;
  librbd::journal::MirrorPeerClientMeta m_client_meta;
};

TEST_F(TestMockImageSyncSnapshotCopyRequest, Empty) {
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({});
  validate_snap_seqs({});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, UpdateClientError) {
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_update_client(mock_journaler, -EINVAL);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, UpdateClientCancel) {
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  InSequence seq;
  EXPECT_CALL(mock_journaler, update_client(_, _))
    .WillOnce(DoAll(InvokeWithoutArgs([request]() {
	    request->cancel();
	  }),
	WithArg<1>(CompleteContext(0))));

  request->send();
  ASSERT_EQ(-ECANCELED, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapCreate) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1"));
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap2"));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t remote_snap_id2 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap2"}];

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap1", 12, 0);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id2);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap2", 14, 0);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, false, 0);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id2, false, 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {12}}, {remote_snap_id2, {14, 12}}});
  validate_snap_seqs({{remote_snap_id1, 12}, {remote_snap_id2, 14}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapCreateError) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1"));

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  uint64_t remote_snap_id1 = mock_remote_image_ctx.snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  InSequence seq;
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap1", 12, -EINVAL);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapCreateCancel) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1"));

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  InSequence seq;
  expect_start_op(mock_exclusive_lock);
  EXPECT_CALL(mock_snapshot_create_request, send())
    .WillOnce(DoAll(InvokeWithoutArgs([request]() {
	    request->cancel();
	  }),
	Invoke([this, &mock_snapshot_create_request]() {
	    m_threads->work_queue->queue(mock_snapshot_create_request.on_finish, 0);
	  })));

  request->send();
  ASSERT_EQ(-ECANCELED, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapRemoveAndCreate) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1"));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1"));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx,
                             m_local_image_ctx->snap_ids[
                               {cls::rbd::UserSnapshotNamespace(), "snap1"}],
                             true, 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_remove(mock_local_image_ctx, "snap1", 0);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap1", 12, 0);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, false, 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {12}}});
  validate_snap_seqs({{remote_snap_id1, 12}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapRemoveError) {
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1"));

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx,
                             m_local_image_ctx->snap_ids[
                               {cls::rbd::UserSnapshotNamespace(), "snap1"}],
                             true, 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_remove(mock_local_image_ctx, "snap1", -EINVAL);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapUnprotect) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_snap_is_unprotected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_unprotect(mock_local_image_ctx, "snap1", 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, false, 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {local_snap_id1}}});
  validate_snap_seqs({{remote_snap_id1, local_snap_id1}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapUnprotectError) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_snap_is_unprotected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_unprotect(mock_local_image_ctx, "snap1", -EBUSY);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(-EBUSY, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapUnprotectCancel) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_snap_is_unprotected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_start_op(mock_exclusive_lock);
  EXPECT_CALL(*mock_local_image_ctx.operations,
	      execute_snap_unprotect(_, StrEq("snap1"), _))
    .WillOnce(DoAll(InvokeWithoutArgs([request]() {
	    request->cancel();
	  }),
	WithArg<2>(Invoke([this](Context *ctx) {
	    m_threads->work_queue->queue(ctx, 0);
	    }))));

  request->send();
  ASSERT_EQ(-ECANCELED, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapUnprotectRemove) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx,
                             m_local_image_ctx->snap_ids[
                               {cls::rbd::UserSnapshotNamespace(), "snap1"}],
                             false, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_unprotect(mock_local_image_ctx, "snap1", 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_remove(mock_local_image_ctx, "snap1", 0);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap1", 12, 0);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, false, 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {12}}});
  validate_snap_seqs({{remote_snap_id1, 12}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapCreateProtect) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  MockSnapshotCreateRequest mock_snapshot_create_request;
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_start_op(mock_exclusive_lock);
  expect_snap_create(mock_local_image_ctx, mock_snapshot_create_request, "snap1", 12, 0);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_snap_is_protected(mock_local_image_ctx, 12, false, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_protect(mock_local_image_ctx, "snap1", 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {12}}});
  validate_snap_seqs({{remote_snap_id1, 12}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapProtect) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, true, 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_snap_is_protected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_protect(mock_local_image_ctx, "snap1", 0);
  expect_update_client(mock_journaler, 0);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());

  validate_snap_map({{remote_snap_id1, {local_snap_id1}}});
  validate_snap_seqs({{remote_snap_id1, local_snap_id1}});
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapProtectError) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, true, 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_snap_is_protected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_start_op(mock_exclusive_lock);
  expect_snap_protect(mock_local_image_ctx, "snap1", -EINVAL);

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  request->send();
  ASSERT_EQ(-EINVAL, ctx.wait());
}

TEST_F(TestMockImageSyncSnapshotCopyRequest, SnapProtectCancel) {
  ASSERT_EQ(0, create_snap(m_remote_image_ctx, "snap1", true));
  ASSERT_EQ(0, create_snap(m_local_image_ctx, "snap1", true));

  uint64_t remote_snap_id1 = m_remote_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  uint64_t local_snap_id1 = m_local_image_ctx->snap_ids[
    {cls::rbd::UserSnapshotNamespace(), "snap1"}];
  m_client_meta.snap_seqs[remote_snap_id1] = local_snap_id1;

  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  journal::MockJournaler mock_journaler;

  librbd::MockExclusiveLock mock_exclusive_lock;
  mock_local_image_ctx.exclusive_lock = &mock_exclusive_lock;

  C_SaferCond ctx;
  MockSnapshotCopyRequest *request = create_request(mock_remote_image_ctx,
                                                    mock_local_image_ctx,
                                                    mock_journaler, &ctx);
  InSequence seq;
  expect_snap_is_unprotected(mock_local_image_ctx, local_snap_id1, true, 0);
  expect_get_snap_namespace(mock_local_image_ctx, local_snap_id1);
  expect_get_snap_namespace(mock_remote_image_ctx, remote_snap_id1);
  expect_snap_is_protected(mock_remote_image_ctx, remote_snap_id1, true, 0);
  expect_snap_is_protected(mock_local_image_ctx, local_snap_id1, false, 0);
  expect_start_op(mock_exclusive_lock);
  EXPECT_CALL(*mock_local_image_ctx.operations,
	      execute_snap_protect(_, StrEq("snap1"), _))
    .WillOnce(DoAll(InvokeWithoutArgs([request]() {
	    request->cancel();
	  }),
	WithArg<2>(Invoke([this](Context *ctx) {
	      m_threads->work_queue->queue(ctx, 0);
	    }))));

  request->send();
  ASSERT_EQ(-ECANCELED, ctx.wait());
}

} // namespace image_sync
} // namespace mirror
} // namespace rbd
