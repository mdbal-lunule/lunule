// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/rbd_mirror/test_mock_fixture.h"
#include "librbd/journal/TypeTraits.h"
#include "tools/rbd_mirror/InstanceWatcher.h"
#include "tools/rbd_mirror/Threads.h"
#include "tools/rbd_mirror/image_replayer/BootstrapRequest.h"
#include "tools/rbd_mirror/image_replayer/CloseImageRequest.h"
#include "tools/rbd_mirror/image_replayer/CreateImageRequest.h"
#include "tools/rbd_mirror/image_replayer/IsPrimaryRequest.h"
#include "tools/rbd_mirror/image_replayer/OpenImageRequest.h"
#include "tools/rbd_mirror/image_replayer/OpenLocalImageRequest.h"
#include "test/journal/mock/MockJournaler.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "test/librbd/mock/MockImageCtx.h"
#include "test/librbd/mock/MockJournal.h"

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

namespace util {

static std::string s_image_id;

template <>
std::string generate_image_id<MockTestImageCtx>(librados::IoCtx&) {
  assert(!s_image_id.empty());
  return s_image_id;
}

} // namespace util
} // namespace librbd

namespace rbd {
namespace mirror {

class ProgressContext;

template<>
struct ImageSync<librbd::MockTestImageCtx> {
  static ImageSync* s_instance;
  Context *on_finish = nullptr;

  static ImageSync* create(
      librbd::MockTestImageCtx *local_image_ctx,
      librbd::MockTestImageCtx *remote_image_ctx,
      SafeTimer *timer, Mutex *timer_lock, const std::string &mirror_uuid,
      ::journal::MockJournaler *journaler,
      librbd::journal::MirrorPeerClientMeta *client_meta, ContextWQ *work_queue,
      InstanceWatcher<librbd::MockTestImageCtx> *instance_watcher,
      Context *on_finish, ProgressContext *progress_ctx) {
    assert(s_instance != nullptr);
    s_instance->on_finish = on_finish;
    return s_instance;
  }

  ImageSync() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~ImageSync() {
    s_instance = nullptr;
  }

  MOCK_METHOD0(get, void());
  MOCK_METHOD0(put, void());
  MOCK_METHOD0(send, void());
  MOCK_METHOD0(cancel, void());
};

ImageSync<librbd::MockTestImageCtx>*
  ImageSync<librbd::MockTestImageCtx>::s_instance = nullptr;

template<>
struct InstanceWatcher<librbd::MockTestImageCtx> {
};

namespace image_replayer {

template<>
struct CloseImageRequest<librbd::MockTestImageCtx> {
  static CloseImageRequest* s_instance;
  librbd::MockTestImageCtx **image_ctx = nullptr;
  Context *on_finish = nullptr;

  static CloseImageRequest* create(librbd::MockTestImageCtx **image_ctx,
                                   Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->image_ctx = image_ctx;
    s_instance->on_finish = on_finish;
    s_instance->construct(*image_ctx);
    return s_instance;
  }

  CloseImageRequest() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~CloseImageRequest() {
    s_instance = nullptr;
  }

  MOCK_METHOD1(construct, void(librbd::MockTestImageCtx *image_ctx));
  MOCK_METHOD0(send, void());
};

template<>
struct CreateImageRequest<librbd::MockTestImageCtx> {
  static CreateImageRequest* s_instance;
  Context *on_finish = nullptr;

  static CreateImageRequest* create(librados::IoCtx &local_io_ctx,
                                    ContextWQ *work_queue,
                                    const std::string &global_image_id,
                                    const std::string &remote_mirror_uuid,
                                    const std::string &local_image_name,
				    const std::string &local_image_id,
                                    librbd::MockTestImageCtx *remote_image_ctx,
                                    Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->on_finish = on_finish;
    s_instance->construct(local_image_id);
    return s_instance;
  }

  CreateImageRequest() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~CreateImageRequest() {
    s_instance = nullptr;
  }

  MOCK_METHOD1(construct, void(const std::string&));
  MOCK_METHOD0(send, void());
};

template<>
struct IsPrimaryRequest<librbd::MockTestImageCtx> {
  static IsPrimaryRequest* s_instance;
  bool *primary = nullptr;
  Context *on_finish = nullptr;

  static IsPrimaryRequest* create(librbd::MockTestImageCtx *image_ctx,
                                  bool *primary, Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->primary = primary;
    s_instance->on_finish = on_finish;
    return s_instance;
  }

  IsPrimaryRequest() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~IsPrimaryRequest() {
    s_instance = nullptr;
  }

  MOCK_METHOD0(send, void());
};

template<>
struct OpenImageRequest<librbd::MockTestImageCtx> {
  static OpenImageRequest* s_instance;
  librbd::MockTestImageCtx **image_ctx = nullptr;
  Context *on_finish = nullptr;

  static OpenImageRequest* create(librados::IoCtx &io_ctx,
                                  librbd::MockTestImageCtx **image_ctx,
                                  const std::string &image_id,
                                  bool read_only, Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->image_ctx = image_ctx;
    s_instance->on_finish = on_finish;
    s_instance->construct(io_ctx, image_id);
    return s_instance;
  }

  OpenImageRequest() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~OpenImageRequest() {
    s_instance = nullptr;
  }

  MOCK_METHOD2(construct, void(librados::IoCtx &io_ctx,
                               const std::string &image_id));
  MOCK_METHOD0(send, void());
};

template<>
struct OpenLocalImageRequest<librbd::MockTestImageCtx> {
  static OpenLocalImageRequest* s_instance;
  librbd::MockTestImageCtx **image_ctx = nullptr;
  Context *on_finish = nullptr;

  static OpenLocalImageRequest* create(librados::IoCtx &local_io_ctx,
                                       librbd::MockTestImageCtx **local_image_ctx,
                                       const std::string &local_image_id,
                                       ContextWQ *work_queue,
                                       Context *on_finish) {
    assert(s_instance != nullptr);
    s_instance->image_ctx = local_image_ctx;
    s_instance->on_finish = on_finish;
    s_instance->construct(local_io_ctx, local_image_id);
    return s_instance;
  }

  OpenLocalImageRequest() {
    assert(s_instance == nullptr);
    s_instance = this;
  }
  ~OpenLocalImageRequest() {
    s_instance = nullptr;
  }

  MOCK_METHOD2(construct, void(librados::IoCtx &io_ctx,
                               const std::string &image_id));
  MOCK_METHOD0(send, void());
};

CloseImageRequest<librbd::MockTestImageCtx>*
  CloseImageRequest<librbd::MockTestImageCtx>::s_instance = nullptr;
CreateImageRequest<librbd::MockTestImageCtx>*
  CreateImageRequest<librbd::MockTestImageCtx>::s_instance = nullptr;
IsPrimaryRequest<librbd::MockTestImageCtx>*
  IsPrimaryRequest<librbd::MockTestImageCtx>::s_instance = nullptr;
OpenImageRequest<librbd::MockTestImageCtx>*
  OpenImageRequest<librbd::MockTestImageCtx>::s_instance = nullptr;
OpenLocalImageRequest<librbd::MockTestImageCtx>*
  OpenLocalImageRequest<librbd::MockTestImageCtx>::s_instance = nullptr;

} // namespace image_replayer
} // namespace mirror
} // namespace rbd

// template definitions
#include "tools/rbd_mirror/image_replayer/BootstrapRequest.cc"

namespace rbd {
namespace mirror {
namespace image_replayer {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::WithArg;

MATCHER_P(IsSameIoCtx, io_ctx, "") {
  return &get_mock_io_ctx(arg) == &get_mock_io_ctx(*io_ctx);
}

class TestMockImageReplayerBootstrapRequest : public TestMockFixture {
public:
  typedef BootstrapRequest<librbd::MockTestImageCtx> MockBootstrapRequest;
  typedef CloseImageRequest<librbd::MockTestImageCtx> MockCloseImageRequest;
  typedef CreateImageRequest<librbd::MockTestImageCtx> MockCreateImageRequest;
  typedef ImageSync<librbd::MockTestImageCtx> MockImageSync;
  typedef InstanceWatcher<librbd::MockTestImageCtx> MockInstanceWatcher;
  typedef IsPrimaryRequest<librbd::MockTestImageCtx> MockIsPrimaryRequest;
  typedef OpenImageRequest<librbd::MockTestImageCtx> MockOpenImageRequest;
  typedef OpenLocalImageRequest<librbd::MockTestImageCtx> MockOpenLocalImageRequest;
  typedef std::list<cls::journal::Tag> Tags;

  void SetUp() override {
    TestMockFixture::SetUp();

    librbd::RBD rbd;
    ASSERT_EQ(0, create_image(rbd, m_remote_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_remote_io_ctx, m_image_name, &m_remote_image_ctx));
  }

  void create_local_image() {
    librbd::RBD rbd;
    ASSERT_EQ(0, create_image(rbd, m_local_io_ctx, m_image_name, m_image_size));
    ASSERT_EQ(0, open_image(m_local_io_ctx, m_image_name, &m_local_image_ctx));
  }

  void expect_journaler_get_client(::journal::MockJournaler &mock_journaler,
                                   const std::string &client_id,
                                   cls::journal::Client &client, int r) {
    EXPECT_CALL(mock_journaler, get_client(StrEq(client_id), _, _))
      .WillOnce(DoAll(WithArg<1>(Invoke([client](cls::journal::Client *out_client) {
                                          *out_client = client;
                                        })),
                      WithArg<2>(Invoke([this, r](Context *on_finish) {
                                          m_threads->work_queue->queue(on_finish, r);
                                        }))));
  }

  void expect_journaler_get_tags(::journal::MockJournaler &mock_journaler,
                                 uint64_t tag_class, const Tags& tags,
                                 int r) {
    EXPECT_CALL(mock_journaler, get_tags(tag_class, _, _))
      .WillOnce(DoAll(WithArg<1>(Invoke([tags](Tags *out_tags) {
                                          *out_tags = tags;
                                        })),
                      WithArg<2>(Invoke([this, r](Context *on_finish) {
                                          m_threads->work_queue->queue(on_finish, r);
                                        }))));
  }

  void expect_journaler_register_client(::journal::MockJournaler &mock_journaler,
                                        const librbd::journal::ClientData &client_data,
                                        int r) {
    bufferlist bl;
    ::encode(client_data, bl);

    EXPECT_CALL(mock_journaler, register_client(ContentsEqual(bl), _))
      .WillOnce(WithArg<1>(Invoke([this, r](Context *on_finish) {
                                    m_threads->work_queue->queue(on_finish, r);
                                  })));
  }

  void expect_journaler_unregister_client(::journal::MockJournaler &mock_journaler,
                                          int r) {
    EXPECT_CALL(mock_journaler, unregister_client(_))
      .WillOnce(Invoke([this, r](Context *on_finish) {
                  m_threads->work_queue->queue(on_finish, r);
                }));
  }

  void expect_journaler_update_client(::journal::MockJournaler &mock_journaler,
                                      const librbd::journal::ClientData &client_data,
                                      int r) {
    bufferlist bl;
    ::encode(client_data, bl);

    EXPECT_CALL(mock_journaler, update_client(ContentsEqual(bl), _))
      .WillOnce(WithArg<1>(Invoke([this, r](Context *on_finish) {
                                    m_threads->work_queue->queue(on_finish, r);
                                  })));
  }

  void expect_open_image(MockOpenImageRequest &mock_open_image_request,
                         librados::IoCtx &io_ctx, const std::string &image_id,
                         librbd::MockTestImageCtx &mock_image_ctx, int r) {
    EXPECT_CALL(mock_open_image_request, construct(IsSameIoCtx(&io_ctx), image_id));
    EXPECT_CALL(mock_open_image_request, send())
      .WillOnce(Invoke([this, &mock_open_image_request, &mock_image_ctx, r]() {
          *mock_open_image_request.image_ctx = &mock_image_ctx;
          m_threads->work_queue->queue(mock_open_image_request.on_finish, r);
        }));
  }

  void expect_open_local_image(MockOpenLocalImageRequest &mock_open_local_image_request,
                               librados::IoCtx &io_ctx, const std::string &image_id,
                               librbd::MockTestImageCtx *mock_image_ctx, int r) {
    EXPECT_CALL(mock_open_local_image_request,
                construct(IsSameIoCtx(&io_ctx), image_id));
    EXPECT_CALL(mock_open_local_image_request, send())
      .WillOnce(Invoke([this, &mock_open_local_image_request, mock_image_ctx, r]() {
          *mock_open_local_image_request.image_ctx = mock_image_ctx;
          m_threads->work_queue->queue(mock_open_local_image_request.on_finish,
                                       r);
        }));
  }

  void expect_close_image(MockCloseImageRequest &mock_close_image_request,
                          librbd::MockTestImageCtx &mock_image_ctx, int r) {
    EXPECT_CALL(mock_close_image_request, construct(&mock_image_ctx));
    EXPECT_CALL(mock_close_image_request, send())
      .WillOnce(Invoke([this, &mock_close_image_request, r]() {
          *mock_close_image_request.image_ctx = nullptr;
          m_threads->work_queue->queue(mock_close_image_request.on_finish, r);
        }));
  }

  void expect_is_primary(MockIsPrimaryRequest &mock_is_primary_request,
			 bool primary, int r) {
    EXPECT_CALL(mock_is_primary_request, send())
      .WillOnce(Invoke([this, &mock_is_primary_request, primary, r]() {
          *mock_is_primary_request.primary = primary;
          m_threads->work_queue->queue(mock_is_primary_request.on_finish, r);
        }));
  }

  void expect_journal_get_tag_tid(librbd::MockJournal &mock_journal,
                                  uint64_t tag_tid) {
    EXPECT_CALL(mock_journal, get_tag_tid()).WillOnce(Return(tag_tid));
  }

  void expect_journal_get_tag_data(librbd::MockJournal &mock_journal,
                                   const librbd::journal::TagData &tag_data) {
    EXPECT_CALL(mock_journal, get_tag_data()).WillOnce(Return(tag_data));
  }

  void expect_is_resync_requested(librbd::MockJournal &mock_journal,
                                  bool do_resync, int r) {
    EXPECT_CALL(mock_journal, is_resync_requested(_))
      .WillOnce(DoAll(SetArgPointee<0>(do_resync),
                      Return(r)));
  }

  void expect_create_image(MockCreateImageRequest &mock_create_image_request,
                           const std::string &image_id, int r) {
    EXPECT_CALL(mock_create_image_request, construct(image_id));
    EXPECT_CALL(mock_create_image_request, send())
      .WillOnce(Invoke([this, &mock_create_image_request, r]() {
          m_threads->work_queue->queue(mock_create_image_request.on_finish, r);
        }));
  }

  void expect_image_sync(MockImageSync &mock_image_sync, int r) {
    EXPECT_CALL(mock_image_sync, get());
    EXPECT_CALL(mock_image_sync, send())
      .WillOnce(Invoke([this, &mock_image_sync, r]() {
            m_threads->work_queue->queue(mock_image_sync.on_finish, r);
          }));
    EXPECT_CALL(mock_image_sync, put());
  }

  bufferlist encode_tag_data(const librbd::journal::TagData &tag_data) {
    bufferlist bl;
    ::encode(tag_data, bl);
    return bl;
  }

  MockBootstrapRequest *create_request(MockInstanceWatcher *mock_instance_watcher,
                                       ::journal::MockJournaler &mock_journaler,
                                       const std::string &local_image_id,
                                       const std::string &remote_image_id,
                                       const std::string &global_image_id,
                                       const std::string &local_mirror_uuid,
                                       const std::string &remote_mirror_uuid,
                                       cls::journal::ClientState *client_state,
                                       librbd::journal::MirrorPeerClientMeta *mirror_peer_client_meta,
                                       Context *on_finish) {
    return new MockBootstrapRequest(m_local_io_ctx,
                                    m_remote_io_ctx,
                                    mock_instance_watcher,
                                    &m_local_test_image_ctx,
                                    local_image_id,
                                    remote_image_id,
                                    global_image_id,
                                    m_threads->work_queue,
                                    m_threads->timer,
                                    &m_threads->timer_lock,
                                    local_mirror_uuid,
                                    remote_mirror_uuid,
                                    &mock_journaler,
                                    client_state, mirror_peer_client_meta,
                                    on_finish, &m_do_resync);
  }

  librbd::ImageCtx *m_remote_image_ctx;
  librbd::ImageCtx *m_local_image_ctx = nullptr;
  librbd::MockTestImageCtx *m_local_test_image_ctx = nullptr;
  bool m_do_resync;
};

TEST_F(TestMockImageReplayerBootstrapRequest, NonPrimaryRemoteSyncingState) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, false, 0);

  // switch the state to replaying
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  client_data.client_meta = mirror_peer_client_meta;
  expect_journaler_update_client(mock_journaler, client_data, 0);

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_SYNCING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(-EREMOTEIO, ctx.wait());
}

TEST_F(TestMockImageReplayerBootstrapRequest, RemoteDemotePromote) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the local image
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // remote demotion / promotion event
  Tags tags = {
    {2, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 1, 99})},
    {3, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 2, 1})},
    {4, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 2, 1})},
    {5, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 4, 369})}
  };
  expect_journaler_get_tags(mock_journaler, 123, tags, 0);
  expect_journal_get_tag_tid(mock_journal, 345);
  expect_journal_get_tag_data(mock_journal, {"remote mirror uuid"});

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockImageReplayerBootstrapRequest, MultipleRemoteDemotePromotes) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the local image
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // remote demotion / promotion event
  Tags tags = {
    {2, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 1, 99})},
    {3, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 2, 1})},
    {4, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 3, 1})},
    {5, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 4, 1})},
    {6, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 5, 1})},
    {7, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 6, 1})},
    {8, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 7, 1})}
  };
  expect_journaler_get_tags(mock_journaler, 123, tags, 0);
  expect_journal_get_tag_tid(mock_journal, 345);
  expect_journal_get_tag_data(mock_journal, {librbd::Journal<>::ORPHAN_MIRROR_UUID,
                                             "remote mirror uuid", true, 4, 1});

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockImageReplayerBootstrapRequest, LocalDemoteRemotePromote) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the local image
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // remote demotion / promotion event
  Tags tags = {
    {2, 123, encode_tag_data({"local mirror uuid", "local mirror uuid",
                              true, 344, 99})},
    {3, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              "local mirror uuid", true, 345, 1})},
    {4, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              true, 3, 1})}
  };
  expect_journaler_get_tags(mock_journaler, 123, tags, 0);
  expect_journal_get_tag_tid(mock_journal, 346);
  expect_journal_get_tag_data(mock_journal,
                              {librbd::Journal<>::ORPHAN_MIRROR_UUID,
                               librbd::Journal<>::LOCAL_MIRROR_UUID,
                               true, 345, 1});

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockImageReplayerBootstrapRequest, SplitBrainForcePromote) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the local image
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // remote demotion / promotion event
  Tags tags = {
    {2, 123, encode_tag_data({librbd::Journal<>::LOCAL_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 1, 99})},
    {3, 123, encode_tag_data({librbd::Journal<>::ORPHAN_MIRROR_UUID,
                              librbd::Journal<>::LOCAL_MIRROR_UUID,
                              true, 2, 1})}
  };
  expect_journaler_get_tags(mock_journaler, 123, tags, 0);
  expect_journal_get_tag_tid(mock_journal, 345);
  expect_journal_get_tag_data(mock_journal, {librbd::Journal<>::LOCAL_MIRROR_UUID,
                                             librbd::Journal<>::ORPHAN_MIRROR_UUID,
                                             true, 344, 0});

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_local_image_ctx, 0);
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(-EEXIST, ctx.wait());
  ASSERT_EQ(NULL, m_local_test_image_ctx);
}

TEST_F(TestMockImageReplayerBootstrapRequest, ResyncRequested) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the local image
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);

  // resync is requested
  expect_is_resync_requested(mock_journal, true, 0);


  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta{
    mock_local_image_ctx.id};
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, mock_local_image_ctx.id,
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  m_do_resync = false;
  request->send();
  ASSERT_EQ(0, ctx.wait());
  ASSERT_TRUE(m_do_resync);
}

TEST_F(TestMockImageReplayerBootstrapRequest, PrimaryRemote) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // update client state back to syncing
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;

  librbd::util::s_image_id = mock_local_image_ctx.id;
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta;
  mirror_peer_client_meta.image_id = mock_local_image_ctx.id;
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_SYNCING;
  client_data.client_meta = mirror_peer_client_meta;
  client.data.clear();
  ::encode(client_data, client.data);
  expect_journaler_update_client(mock_journaler, client_data, 0);

  // create the local image
  MockCreateImageRequest mock_create_image_request;
  expect_create_image(mock_create_image_request, mock_local_image_ctx.id, 0);

  // open the local image
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // sync the remote image to the local image
  MockImageSync mock_image_sync;
  expect_image_sync(mock_image_sync, 0);

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  mirror_peer_client_meta.image_id = "";
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, "",
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
}

TEST_F(TestMockImageReplayerBootstrapRequest, PrimaryRemoteLocalDeleted) {
  create_local_image();

  InSequence seq;

  // lookup remote image tag class
  cls::journal::Client client;
  librbd::journal::ClientData client_data{
    librbd::journal::ImageClientMeta{123}};
  ::encode(client_data, client.data);
  ::journal::MockJournaler mock_journaler;
  expect_journaler_get_client(mock_journaler,
                              librbd::Journal<>::IMAGE_CLIENT_ID,
                              client, 0);

  // open the remote image
  librbd::MockJournal mock_journal;
  librbd::MockTestImageCtx mock_remote_image_ctx(*m_remote_image_ctx);
  MockOpenImageRequest mock_open_image_request;
  expect_open_image(mock_open_image_request, m_remote_io_ctx,
                    mock_remote_image_ctx.id, mock_remote_image_ctx, 0);

  // test if remote image is primary
  MockIsPrimaryRequest mock_is_primary_request;
  expect_is_primary(mock_is_primary_request, true, 0);

  // open the missing local image
  MockOpenLocalImageRequest mock_open_local_image_request;
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          "missing image id", nullptr, -ENOENT);

  // re-register the client
  expect_journaler_unregister_client(mock_journaler, 0);
  librbd::journal::MirrorPeerClientMeta mirror_peer_client_meta;
  mirror_peer_client_meta.image_id = "";
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  client_data.client_meta = mirror_peer_client_meta;
  expect_journaler_register_client(mock_journaler, client_data, 0);

  // test if remote image is primary
  expect_is_primary(mock_is_primary_request, true, 0);

  // update client state back to syncing
  librbd::MockTestImageCtx mock_local_image_ctx(*m_local_image_ctx);
  mock_local_image_ctx.journal = &mock_journal;

  librbd::util::s_image_id = mock_local_image_ctx.id;
  mirror_peer_client_meta.image_id = mock_local_image_ctx.id;
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_SYNCING;
  client_data.client_meta = mirror_peer_client_meta;
  client.data.clear();
  ::encode(client_data, client.data);
  expect_journaler_update_client(mock_journaler, client_data, 0);

  // create the missing local image
  MockCreateImageRequest mock_create_image_request;
  expect_create_image(mock_create_image_request, mock_local_image_ctx.id, 0);

  // open the local image
  expect_open_local_image(mock_open_local_image_request, m_local_io_ctx,
                          mock_local_image_ctx.id, &mock_local_image_ctx, 0);
  expect_is_resync_requested(mock_journal, false, 0);

  // sync the remote image to the local image
  MockImageSync mock_image_sync;
  expect_image_sync(mock_image_sync, 0);

  MockCloseImageRequest mock_close_image_request;
  expect_close_image(mock_close_image_request, mock_remote_image_ctx, 0);

  C_SaferCond ctx;
  MockInstanceWatcher mock_instance_watcher;
  cls::journal::ClientState client_state = cls::journal::CLIENT_STATE_CONNECTED;
  mirror_peer_client_meta.image_id = "missing image id";
  mirror_peer_client_meta.state = librbd::journal::MIRROR_PEER_STATE_REPLAYING;
  MockBootstrapRequest *request = create_request(
    &mock_instance_watcher, mock_journaler, "missing image id",
    mock_remote_image_ctx.id, "global image id", "local mirror uuid",
    "remote mirror uuid", &client_state, &mirror_peer_client_meta, &ctx);
  request->send();
  ASSERT_EQ(0, ctx.wait());
}

} // namespace image_replayer
} // namespace mirror
} // namespace rbd
