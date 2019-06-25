// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librbd/test_mock_fixture.h"
#include "test/librbd/test_support.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "test/librbd/mock/exclusive_lock/MockPolicy.h"
#include "librbd/io/ImageRequestWQ.h"
#include "librbd/io/ImageRequest.h"

namespace librbd {
namespace {

struct MockTestImageCtx : public MockImageCtx {
  MockTestImageCtx(ImageCtx &image_ctx) : MockImageCtx(image_ctx) {
  }
};

} // anonymous namespace

namespace io {

template <>
struct ImageRequest<librbd::MockTestImageCtx> {
  static ImageRequest* s_instance;
  AioCompletion *aio_comp;

  static ImageRequest* create_write_request(librbd::MockTestImageCtx &image_ctx,
                                            AioCompletion *aio_comp,
                                            Extents &&image_extents,
                                            bufferlist &&bl, int op_flags,
                                            const ZTracer::Trace &parent_trace) {
    assert(s_instance != nullptr);
    s_instance->aio_comp = aio_comp;
    return s_instance;
  }
  static void aio_write(librbd::MockTestImageCtx *ictx, AioCompletion *c,
                        Extents &&image_extents, bufferlist &&bl, int op_flags,
                        const ZTracer::Trace &parent_trace) {
  }


  MOCK_CONST_METHOD0(is_write_op, bool());
  MOCK_CONST_METHOD0(start_op, void());
  MOCK_CONST_METHOD0(send, void());
  MOCK_CONST_METHOD1(fail, void(int));

  ImageRequest() {
    s_instance = this;
  }
};

} // namespace io

namespace util {

inline ImageCtx *get_image_ctx(MockTestImageCtx *image_ctx) {
  return image_ctx->image_ctx;
}

} // namespace util

} // namespace librbd

template <>
struct ThreadPool::PointerWQ<librbd::io::ImageRequest<librbd::MockTestImageCtx>> {
  typedef librbd::io::ImageRequest<librbd::MockTestImageCtx> ImageRequest;
  static PointerWQ* s_instance;

  Mutex m_lock;

  PointerWQ(const std::string &name, time_t, int, ThreadPool *)
    : m_lock(name) {
    s_instance = this;
  }
  virtual ~PointerWQ() {
  }

  MOCK_METHOD0(drain, void());
  MOCK_METHOD0(empty, bool());
  MOCK_METHOD0(signal, void());
  MOCK_METHOD0(process_finish, void());

  MOCK_METHOD0(front, ImageRequest*());
  MOCK_METHOD1(requeue, void(ImageRequest*));

  MOCK_METHOD0(dequeue, void*());
  MOCK_METHOD1(queue, void(ImageRequest*));

  void register_work_queue() {
    // no-op
  }
  Mutex &get_pool_lock() {
    return m_lock;
  }

  void* invoke_dequeue() {
    Mutex::Locker locker(m_lock);
    return _void_dequeue();
  }
  void invoke_process(ImageRequest *image_request) {
    process(image_request);
  }

  virtual void *_void_dequeue() {
    return dequeue();
  }
  virtual void process(ImageRequest *req) = 0;

};

ThreadPool::PointerWQ<librbd::io::ImageRequest<librbd::MockTestImageCtx>>*
  ThreadPool::PointerWQ<librbd::io::ImageRequest<librbd::MockTestImageCtx>>::s_instance = nullptr;
librbd::io::ImageRequest<librbd::MockTestImageCtx>*
  librbd::io::ImageRequest<librbd::MockTestImageCtx>::s_instance = nullptr;

#include "librbd/io/ImageRequestWQ.cc"

namespace librbd {
namespace io {

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::WithArg;

struct TestMockIoImageRequestWQ : public TestMockFixture {
  typedef ImageRequestWQ<librbd::MockTestImageCtx> MockImageRequestWQ;
  typedef ImageRequest<librbd::MockTestImageCtx> MockImageRequest;

  void expect_is_write_op(MockImageRequest &image_request, bool write_op) {
    EXPECT_CALL(image_request, is_write_op()).WillOnce(Return(write_op));
  }

  void expect_signal(MockImageRequestWQ &image_request_wq) {
    EXPECT_CALL(image_request_wq, signal());
  }

  void expect_queue(MockImageRequestWQ &image_request_wq) {
    EXPECT_CALL(image_request_wq, queue(_));
  }

  void expect_front(MockImageRequestWQ &image_request_wq,
                    MockImageRequest *image_request) {
    EXPECT_CALL(image_request_wq, front()).WillOnce(Return(image_request));
  }

  void expect_is_refresh_request(MockTestImageCtx &mock_image_ctx,
                                 bool required) {
    EXPECT_CALL(*mock_image_ctx.state, is_refresh_required()).WillOnce(
      Return(required));
  }

  void expect_dequeue(MockImageRequestWQ &image_request_wq,
                      MockImageRequest *image_request) {
    EXPECT_CALL(image_request_wq, dequeue()).WillOnce(Return(image_request));
  }

  void expect_get_exclusive_lock_policy(MockTestImageCtx &mock_image_ctx,
                                        librbd::exclusive_lock::MockPolicy &policy) {
    EXPECT_CALL(mock_image_ctx,
                get_exclusive_lock_policy()).WillOnce(Return(&policy));
  }

  void expect_may_auto_request_lock(librbd::exclusive_lock::MockPolicy &policy,
                                    bool value) {
    EXPECT_CALL(policy, may_auto_request_lock()).WillOnce(Return(value));
  }

  void expect_acquire_lock(MockExclusiveLock &mock_exclusive_lock,
                           Context **on_finish) {
    EXPECT_CALL(mock_exclusive_lock, acquire_lock(_))
      .WillOnce(Invoke([on_finish](Context *ctx) {
                    *on_finish = ctx;
                  }));
  }

  void expect_process_finish(MockImageRequestWQ &mock_image_request_wq) {
    EXPECT_CALL(mock_image_request_wq, process_finish()).Times(1);
  }

  void expect_fail(MockImageRequest &mock_image_request, int r) {
    EXPECT_CALL(mock_image_request, fail(r))
      .WillOnce(Invoke([&mock_image_request](int r) {
                    mock_image_request.aio_comp->get();
                    mock_image_request.aio_comp->fail(r);
                  }));
  }

  void expect_refresh(MockTestImageCtx &mock_image_ctx, Context **on_finish) {
    EXPECT_CALL(*mock_image_ctx.state, refresh(_))
      .WillOnce(Invoke([on_finish](Context *ctx) {
                    *on_finish = ctx;
                  }));
  }
};

TEST_F(TestMockIoImageRequestWQ, AcquireLockError) {
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  MockTestImageCtx mock_image_ctx(*ictx);
  MockExclusiveLock mock_exclusive_lock;
  mock_image_ctx.exclusive_lock = &mock_exclusive_lock;

  InSequence seq;
  MockImageRequestWQ mock_image_request_wq(&mock_image_ctx, "io", 60, nullptr);
  expect_signal(mock_image_request_wq);
  mock_image_request_wq.set_require_lock(DIRECTION_WRITE, true);

  auto mock_image_request = new MockImageRequest();
  expect_is_write_op(*mock_image_request, true);
  expect_queue(mock_image_request_wq);
  auto *aio_comp = new librbd::io::AioCompletion();
  mock_image_request_wq.aio_write(aio_comp, 0, 0, {}, 0);

  librbd::exclusive_lock::MockPolicy mock_exclusive_lock_policy;
  expect_front(mock_image_request_wq, mock_image_request);
  expect_is_refresh_request(mock_image_ctx, false);
  expect_is_write_op(*mock_image_request, true);
  expect_dequeue(mock_image_request_wq, mock_image_request);
  expect_get_exclusive_lock_policy(mock_image_ctx, mock_exclusive_lock_policy);
  expect_may_auto_request_lock(mock_exclusive_lock_policy, true);
  Context *on_acquire = nullptr;
  expect_acquire_lock(mock_exclusive_lock, &on_acquire);
  ASSERT_TRUE(mock_image_request_wq.invoke_dequeue() == nullptr);
  ASSERT_TRUE(on_acquire != nullptr);

  expect_process_finish(mock_image_request_wq);
  expect_fail(*mock_image_request, -EPERM);
  expect_is_write_op(*mock_image_request, true);
  expect_signal(mock_image_request_wq);
  on_acquire->complete(-EPERM);

  ASSERT_EQ(0, aio_comp->wait_for_complete());
  ASSERT_EQ(-EPERM, aio_comp->get_return_value());
  aio_comp->release();
}

TEST_F(TestMockIoImageRequestWQ, RefreshError) {
  librbd::ImageCtx *ictx;
  ASSERT_EQ(0, open_image(m_image_name, &ictx));

  MockTestImageCtx mock_image_ctx(*ictx);

  InSequence seq;
  MockImageRequestWQ mock_image_request_wq(&mock_image_ctx, "io", 60, nullptr);

  auto mock_image_request = new MockImageRequest();
  expect_is_write_op(*mock_image_request, true);
  expect_queue(mock_image_request_wq);
  auto *aio_comp = new librbd::io::AioCompletion();
  mock_image_request_wq.aio_write(aio_comp, 0, 0, {}, 0);

  expect_front(mock_image_request_wq, mock_image_request);
  expect_is_refresh_request(mock_image_ctx, true);
  expect_is_write_op(*mock_image_request, true);
  expect_dequeue(mock_image_request_wq, mock_image_request);
  Context *on_refresh = nullptr;
  expect_refresh(mock_image_ctx, &on_refresh);
  ASSERT_TRUE(mock_image_request_wq.invoke_dequeue() == nullptr);
  ASSERT_TRUE(on_refresh != nullptr);

  expect_process_finish(mock_image_request_wq);
  expect_fail(*mock_image_request, -EPERM);
  expect_is_write_op(*mock_image_request, true);
  expect_signal(mock_image_request_wq);
  on_refresh->complete(-EPERM);

  ASSERT_EQ(0, aio_comp->wait_for_complete());
  ASSERT_EQ(-EPERM, aio_comp->get_return_value());
  aio_comp->release();
}

} // namespace io
} // namespace librbd
