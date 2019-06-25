// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 eNovance SAS <licensing@enovance.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#ifndef RGW_REST_METADATA_H
#define RGW_REST_METADATA_H

class RGWOp_Metadata_List : public RGWRESTOp {
public:
  RGWOp_Metadata_List() {}
  ~RGWOp_Metadata_List() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_READ);
  }
  void execute() override;
  const string name() override;
};

class RGWOp_Metadata_Get : public RGWRESTOp {
public:
  RGWOp_Metadata_Get() {}
  ~RGWOp_Metadata_Get() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_READ);
  }
  void execute() override;
  const string name() override;
};

class RGWOp_Metadata_Put : public RGWRESTOp {
  int get_data(bufferlist& bl);
  string update_status;
  obj_version ondisk_version;
public:
  RGWOp_Metadata_Put() {}
  ~RGWOp_Metadata_Put() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_WRITE);
  }
  void execute() override;
  void send_response() override;
  const string name() override { return "set_metadata"; }
  RGWOpType get_type() override { return RGW_OP_ADMIN_SET_METADATA; }
};

class RGWOp_Metadata_Delete : public RGWRESTOp {
public:
  RGWOp_Metadata_Delete() {}
  ~RGWOp_Metadata_Delete() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_WRITE);
  }
  void execute() override;
  const string name() override { return "remove_metadata"; }
};

class RGWOp_Metadata_Lock : public RGWRESTOp {
public:
  RGWOp_Metadata_Lock() {}
  ~RGWOp_Metadata_Lock() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_WRITE);
  }
  void execute() override;
  const string name() override {
    return "lock_metadata_object";
  }
};

class RGWOp_Metadata_Unlock : public RGWRESTOp {
public:
  RGWOp_Metadata_Unlock() {}
  ~RGWOp_Metadata_Unlock() override {}

  int check_caps(RGWUserCaps& caps) override {
    return caps.check_cap("metadata", RGW_CAP_WRITE);
  }
  void execute() override;
  const string name() override {
    return "unlock_metadata_object";
  }
};

class RGWHandler_Metadata : public RGWHandler_Auth_S3 {
protected:
  RGWOp *op_get() override;
  RGWOp *op_put() override;
  RGWOp *op_delete() override;
  RGWOp *op_post() override;

  int read_permissions(RGWOp*) override {
    return 0;
  }
public:
  using RGWHandler_Auth_S3::RGWHandler_Auth_S3;
  ~RGWHandler_Metadata() override = default;
};

class RGWRESTMgr_Metadata : public RGWRESTMgr {
public:
  RGWRESTMgr_Metadata() = default;
  ~RGWRESTMgr_Metadata() override = default;

  RGWHandler_REST* get_handler(struct req_state* const s,
                               const rgw::auth::StrategyRegistry& auth_registry,
                               const std::string& frontend_prefix) override {
    return new RGWHandler_Metadata(auth_registry);
  }
};

#endif /* RGW_REST_METADATA_H */
