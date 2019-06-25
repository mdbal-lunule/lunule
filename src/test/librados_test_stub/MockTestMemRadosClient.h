// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef LIBRADOS_TEST_STUB_MOCK_TEST_MEM_RADOS_CLIENT_H
#define LIBRADOS_TEST_STUB_MOCK_TEST_MEM_RADOS_CLIENT_H

#include "test/librados_test_stub/TestMemRadosClient.h"
#include "test/librados_test_stub/MockTestMemIoCtxImpl.h"
#include "gmock/gmock.h"

namespace librados {

class TestMemCluster;

class MockTestMemRadosClient : public TestMemRadosClient {
public:
  MockTestMemRadosClient(CephContext *cct, TestMemCluster *test_mem_cluster)
    : TestMemRadosClient(cct, test_mem_cluster) {
    default_to_dispatch();
  }

  MOCK_METHOD2(create_ioctx, TestIoCtxImpl *(int64_t pool_id,
                                             const std::string &pool_name));
  TestIoCtxImpl *do_create_ioctx(int64_t pool_id,
                                 const std::string &pool_name) {
    return new ::testing::NiceMock<MockTestMemIoCtxImpl>(
      this, this, pool_id, pool_name,
      get_mem_cluster()->get_pool(pool_name));
  }

  MOCK_METHOD2(blacklist_add, int(const std::string& client_address,
                                  uint32_t expire_seconds));
  int do_blacklist_add(const std::string& client_address,
                       uint32_t expire_seconds) {
    return TestMemRadosClient::blacklist_add(client_address, expire_seconds);
  }

  MOCK_METHOD3(service_daemon_register,
               int(const std::string&,
                   const std::string&,
                   const std::map<std::string,std::string>&));
  int do_service_daemon_register(const std::string& service,
                                 const std::string& name,
                                 const std::map<std::string,std::string>& metadata) {
    return TestMemRadosClient::service_daemon_register(service, name, metadata);
  }

  MOCK_METHOD1(service_daemon_update_status,
               int(const std::map<std::string,std::string>&));
  int do_service_daemon_update_status(const std::map<std::string,std::string>& status) {
    return TestMemRadosClient::service_daemon_update_status(status);
  }

  void default_to_dispatch() {
    using namespace ::testing;

    ON_CALL(*this, create_ioctx(_, _)).WillByDefault(Invoke(this, &MockTestMemRadosClient::do_create_ioctx));
    ON_CALL(*this, blacklist_add(_, _)).WillByDefault(Invoke(this, &MockTestMemRadosClient::do_blacklist_add));
    ON_CALL(*this, service_daemon_register(_, _, _)).WillByDefault(Invoke(this, &MockTestMemRadosClient::do_service_daemon_register));
    ON_CALL(*this, service_daemon_update_status(_)).WillByDefault(Invoke(this, &MockTestMemRadosClient::do_service_daemon_update_status));
  }
};

} // namespace librados

#endif // LIBRADOS_TEST_STUB_MOCK_TEST_MEM_RADOS_CLIENT_H
