// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/librados/test.h"
#include "common/Mutex.h"
#include "common/Timer.h"
#include "journal/JournalMetadata.h"
#include "cls/journal/cls_journal_types.h"
#include "gtest/gtest.h"

class ThreadPool;

class RadosTestFixture : public ::testing::Test {
public:
  static void SetUpTestCase();
  static void TearDownTestCase();

  static std::string get_temp_oid();

  RadosTestFixture();
  void SetUp() override;
  void TearDown() override;

  int create(const std::string &oid, uint8_t order = 14,
             uint8_t splay_width = 2);
  journal::JournalMetadataPtr create_metadata(const std::string &oid,
                                              const std::string &client_id = "client",
                                              double commit_internal = 0.1,
                                              uint64_t max_fetch_bytes = 0,
                                              int max_concurrent_object_sets = 0);
  int append(const std::string &oid, const bufferlist &bl);

  int client_register(const std::string &oid, const std::string &id = "client",
                      const std::string &description = "");
  int client_commit(const std::string &oid, const std::string &id,
                    const cls::journal::ObjectSetPosition &commit_position);

  bufferlist create_payload(const std::string &payload);

  struct Listener : public journal::JournalMetadataListener {
    RadosTestFixture *test_fixture;
    Mutex mutex;
    Cond cond;
    std::map<journal::JournalMetadata*, uint32_t> updates;

    Listener(RadosTestFixture *_test_fixture)
      : test_fixture(_test_fixture), mutex("mutex") {}

    void handle_update(journal::JournalMetadata *metadata) override {
      Mutex::Locker locker(mutex);
      ++updates[metadata];
      cond.Signal();
    }
  };

  int init_metadata(journal::JournalMetadataPtr metadata);

  bool wait_for_update(journal::JournalMetadataPtr metadata);

  static std::string _pool_name;
  static librados::Rados _rados;
  static uint64_t _oid_number;
  static ThreadPool *_thread_pool;

  librados::IoCtx m_ioctx;

  ContextWQ *m_work_queue;

  Mutex m_timer_lock;
  SafeTimer *m_timer;

  Listener m_listener;

  std::list<journal::JournalMetadataPtr> m_metadatas;
};
