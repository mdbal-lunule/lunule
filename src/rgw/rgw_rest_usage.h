// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_REST_USAGE_H
#define CEPH_RGW_REST_USAGE_H

#include "rgw_rest.h"
#include "rgw_rest_s3.h"


class RGWHandler_Usage : public RGWHandler_Auth_S3 {
protected:
  RGWOp *op_get() override;
  RGWOp *op_delete() override;
public:
  using RGWHandler_Auth_S3::RGWHandler_Auth_S3;
  ~RGWHandler_Usage() override = default;

  int read_permissions(RGWOp*) override {
    return 0;
  }
};

class RGWRESTMgr_Usage : public RGWRESTMgr {
public:
  RGWRESTMgr_Usage() = default;
  ~RGWRESTMgr_Usage() override = default;

  RGWHandler_REST* get_handler(struct req_state*,
                               const rgw::auth::StrategyRegistry& auth_registry,
                               const std::string&) override {
    return new RGWHandler_Usage(auth_registry);
  }
};

#endif
