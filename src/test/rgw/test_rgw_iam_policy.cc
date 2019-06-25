// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include <string>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>

#include <gtest/gtest.h>

#include "include/stringify.h"
#include "common/code_environment.h"
#include "common/ceph_context.h"
#include "global/global_init.h"
#include "rgw/rgw_auth.h"
#include "rgw/rgw_iam_policy.h"
#include "rgw/rgw_op.h"


using std::string;
using std::vector;

using boost::container::flat_set;
using boost::intrusive_ptr;
using boost::make_optional;
using boost::none;
using boost::optional;

using rgw::auth::Identity;
using rgw::auth::Principal;

using rgw::IAM::ARN;
using rgw::IAM::Effect;
using rgw::IAM::Environment;
using rgw::IAM::Partition;
using rgw::IAM::Policy;
using rgw::IAM::s3All;
using rgw::IAM::s3Count;
using rgw::IAM::s3GetAccelerateConfiguration;
using rgw::IAM::s3GetBucketAcl;
using rgw::IAM::s3GetBucketCORS;
using rgw::IAM::s3GetBucketLocation;
using rgw::IAM::s3GetBucketLogging;
using rgw::IAM::s3GetBucketNotification;
using rgw::IAM::s3GetBucketPolicy;
using rgw::IAM::s3GetBucketRequestPayment;
using rgw::IAM::s3GetBucketTagging;
using rgw::IAM::s3GetBucketVersioning;
using rgw::IAM::s3GetBucketWebsite;
using rgw::IAM::s3GetLifecycleConfiguration;
using rgw::IAM::s3GetObject;
using rgw::IAM::s3GetObjectAcl;
using rgw::IAM::s3GetObjectVersionAcl;
using rgw::IAM::s3GetObjectTorrent;
using rgw::IAM::s3GetObjectTagging;
using rgw::IAM::s3GetObjectVersion;
using rgw::IAM::s3GetObjectVersionTagging;
using rgw::IAM::s3GetObjectVersionTorrent;
using rgw::IAM::s3GetReplicationConfiguration;
using rgw::IAM::s3ListAllMyBuckets;
using rgw::IAM::s3ListBucket;
using rgw::IAM::s3ListBucket;
using rgw::IAM::s3ListBucketMultiPartUploads;
using rgw::IAM::s3ListBucketVersions;
using rgw::IAM::s3ListMultipartUploadParts;
using rgw::IAM::s3None;
using rgw::IAM::s3PutBucketAcl;
using rgw::IAM::s3PutBucketPolicy;
using rgw::IAM::Service;
using rgw::IAM::TokenID;
using rgw::IAM::Version;

class FakeIdentity : public Identity {
  const Principal id;
public:

  FakeIdentity(Principal&& id) : id(std::move(id)) {}
  uint32_t get_perms_from_aclspec(const aclspec_t& aclspec) const override {
    abort();
    return 0;
  };

  bool is_admin_of(const rgw_user& uid) const override {
    abort();
    return false;
  }

  bool is_owner_of(const rgw_user& uid) const override {
    abort();
    return false;
  }

  virtual uint32_t get_perm_mask() const override {
    abort();
    return 0;
  }

  void to_str(std::ostream& out) const override {
    out << id;
  }

  bool is_identity(const flat_set<Principal>& ids) const override {
    if (id.is_wildcard() && (!ids.empty())) {
      return true;
    }
    return ids.find(id) != ids.end() || ids.find(Principal::wildcard()) != ids.end();
  }
};

class PolicyTest : public ::testing::Test {
protected:
  intrusive_ptr<CephContext> cct;
  static const string arbitrary_tenant;
  static string example1;
  static string example2;
  static string example3;
public:
  PolicyTest() {
    cct = new CephContext(CEPH_ENTITY_TYPE_CLIENT);
  }
};

TEST_F(PolicyTest, Parse1) {
  optional<Policy> p;

  ASSERT_NO_THROW(p = Policy(cct.get(), arbitrary_tenant,
			     bufferlist::static_from_string(example1)));
  ASSERT_TRUE(p);

  EXPECT_EQ(p->text, example1);
  EXPECT_EQ(p->version, Version::v2012_10_17);
  EXPECT_FALSE(p->id);
  EXPECT_FALSE(p->statements[0].sid);
  EXPECT_FALSE(p->statements.empty());
  EXPECT_EQ(p->statements.size(), 1U);
  EXPECT_TRUE(p->statements[0].princ.empty());
  EXPECT_TRUE(p->statements[0].noprinc.empty());
  EXPECT_EQ(p->statements[0].effect, Effect::Allow);
  EXPECT_EQ(p->statements[0].action, s3ListBucket);
  EXPECT_EQ(p->statements[0].notaction, s3None);
  ASSERT_FALSE(p->statements[0].resource.empty());
  ASSERT_EQ(p->statements[0].resource.size(), 1U);
  EXPECT_EQ(p->statements[0].resource.begin()->partition, Partition::aws);
  EXPECT_EQ(p->statements[0].resource.begin()->service, Service::s3);
  EXPECT_TRUE(p->statements[0].resource.begin()->region.empty());
  EXPECT_EQ(p->statements[0].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[0].resource.begin()->resource, "example_bucket");
  EXPECT_TRUE(p->statements[0].notresource.empty());
  EXPECT_TRUE(p->statements[0].conditions.empty());
}

TEST_F(PolicyTest, Eval1) {
  auto p  = Policy(cct.get(), arbitrary_tenant,
		   bufferlist::static_from_string(example1));
  Environment e;

  EXPECT_EQ(p.eval(e, none, s3ListBucket,
		   ARN(Partition::aws, Service::s3,
		       "", arbitrary_tenant, "example_bucket")),
	    Effect::Allow);

  EXPECT_EQ(p.eval(e, none, s3PutBucketAcl,
		   ARN(Partition::aws, Service::s3,
		       "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);

  EXPECT_EQ(p.eval(e, none, s3ListBucket,
		   ARN(Partition::aws, Service::s3,
		       "", arbitrary_tenant, "erroneous_bucket")),
	    Effect::Pass);

}

TEST_F(PolicyTest, Parse2) {
  optional<Policy> p;

  ASSERT_NO_THROW(p = Policy(cct.get(), arbitrary_tenant,
			     bufferlist::static_from_string(example2)));
  ASSERT_TRUE(p);

  EXPECT_EQ(p->text, example2);
  EXPECT_EQ(p->version, Version::v2012_10_17);
  EXPECT_EQ(*p->id, "S3-Account-Permissions");
  ASSERT_FALSE(p->statements.empty());
  EXPECT_EQ(p->statements.size(), 1U);
  EXPECT_EQ(*p->statements[0].sid, "1");
  EXPECT_FALSE(p->statements[0].princ.empty());
  EXPECT_EQ(p->statements[0].princ.size(), 1U);
  EXPECT_EQ(*p->statements[0].princ.begin(),
	    Principal::tenant("ACCOUNT-ID-WITHOUT-HYPHENS"));
  EXPECT_TRUE(p->statements[0].noprinc.empty());
  EXPECT_EQ(p->statements[0].effect, Effect::Allow);
  EXPECT_EQ(p->statements[0].action, s3All);
  EXPECT_EQ(p->statements[0].notaction, s3None);
  ASSERT_FALSE(p->statements[0].resource.empty());
  ASSERT_EQ(p->statements[0].resource.size(), 2U);
  EXPECT_EQ(p->statements[0].resource.begin()->partition, Partition::aws);
  EXPECT_EQ(p->statements[0].resource.begin()->service, Service::s3);
  EXPECT_TRUE(p->statements[0].resource.begin()->region.empty());
  EXPECT_EQ(p->statements[0].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[0].resource.begin()->resource, "mybucket");
  EXPECT_EQ((p->statements[0].resource.begin() + 1)->partition,
	    Partition::aws);
  EXPECT_EQ((p->statements[0].resource.begin() + 1)->service,
	    Service::s3);
  EXPECT_TRUE((p->statements[0].resource.begin() + 1)->region.empty());
  EXPECT_EQ((p->statements[0].resource.begin() + 1)->account,
	    arbitrary_tenant);
  EXPECT_EQ((p->statements[0].resource.begin() + 1)->resource, "mybucket/*");
  EXPECT_TRUE(p->statements[0].notresource.empty());
  EXPECT_TRUE(p->statements[0].conditions.empty());
}

TEST_F(PolicyTest, Eval2) {
  auto p  = Policy(cct.get(), arbitrary_tenant,
		   bufferlist::static_from_string(example2));
  Environment e;

  auto trueacct = FakeIdentity(
    Principal::tenant("ACCOUNT-ID-WITHOUT-HYPHENS"));

  auto notacct = FakeIdentity(
    Principal::tenant("some-other-account"));
  for (auto i = 0ULL; i < s3Count; ++i) {
    EXPECT_EQ(p.eval(e, trueacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "mybucket")),
	      Effect::Allow);
    EXPECT_EQ(p.eval(e, trueacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "mybucket/myobject")),
	      Effect::Allow);

    EXPECT_EQ(p.eval(e, notacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "mybucket")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(e, notacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "mybucket/myobject")),
	      Effect::Pass);

    EXPECT_EQ(p.eval(e, trueacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "notyourbucket")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(e, trueacct, 1ULL << i,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "notyourbucket/notyourobject")),
	      Effect::Pass);

  }
}

TEST_F(PolicyTest, Parse3) {
  optional<Policy> p;

  ASSERT_NO_THROW(p = Policy(cct.get(), arbitrary_tenant,
			     bufferlist::static_from_string(example3)));
  ASSERT_TRUE(p);

  EXPECT_EQ(p->text, example3);
  EXPECT_EQ(p->version, Version::v2012_10_17);
  EXPECT_FALSE(p->id);
  ASSERT_FALSE(p->statements.empty());
  EXPECT_EQ(p->statements.size(), 3U);

  EXPECT_EQ(*p->statements[0].sid, "FirstStatement");
  EXPECT_TRUE(p->statements[0].princ.empty());
  EXPECT_TRUE(p->statements[0].noprinc.empty());
  EXPECT_EQ(p->statements[0].effect, Effect::Allow);
  EXPECT_EQ(p->statements[0].action, s3PutBucketPolicy);
  EXPECT_EQ(p->statements[0].notaction, s3None);
  ASSERT_FALSE(p->statements[0].resource.empty());
  ASSERT_EQ(p->statements[0].resource.size(), 1U);
  EXPECT_EQ(p->statements[0].resource.begin()->partition, Partition::wildcard);
  EXPECT_EQ(p->statements[0].resource.begin()->service, Service::wildcard);
  EXPECT_EQ(p->statements[0].resource.begin()->region, "*");
  EXPECT_EQ(p->statements[0].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[0].resource.begin()->resource, "*");
  EXPECT_TRUE(p->statements[0].notresource.empty());
  EXPECT_TRUE(p->statements[0].conditions.empty());

  EXPECT_EQ(*p->statements[1].sid, "SecondStatement");
  EXPECT_TRUE(p->statements[1].princ.empty());
  EXPECT_TRUE(p->statements[1].noprinc.empty());
  EXPECT_EQ(p->statements[1].effect, Effect::Allow);
  EXPECT_EQ(p->statements[1].action, s3ListAllMyBuckets);
  EXPECT_EQ(p->statements[1].notaction, s3None);
  ASSERT_FALSE(p->statements[1].resource.empty());
  ASSERT_EQ(p->statements[1].resource.size(), 1U);
  EXPECT_EQ(p->statements[1].resource.begin()->partition, Partition::wildcard);
  EXPECT_EQ(p->statements[1].resource.begin()->service, Service::wildcard);
  EXPECT_EQ(p->statements[1].resource.begin()->region, "*");
  EXPECT_EQ(p->statements[1].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[1].resource.begin()->resource, "*");
  EXPECT_TRUE(p->statements[1].notresource.empty());
  EXPECT_TRUE(p->statements[1].conditions.empty());

  EXPECT_EQ(*p->statements[2].sid, "ThirdStatement");
  EXPECT_TRUE(p->statements[2].princ.empty());
  EXPECT_TRUE(p->statements[2].noprinc.empty());
  EXPECT_EQ(p->statements[2].effect, Effect::Allow);
  EXPECT_EQ(p->statements[2].action, (s3ListMultipartUploadParts |
				      s3ListBucket | s3ListBucketVersions |
				      s3ListAllMyBuckets |
				      s3ListBucketMultiPartUploads |
				      s3GetObject | s3GetObjectVersion |
				      s3GetObjectAcl | s3GetObjectVersionAcl |
				      s3GetObjectTorrent |
				      s3GetObjectVersionTorrent |
				      s3GetAccelerateConfiguration |
				      s3GetBucketAcl | s3GetBucketCORS |
				      s3GetBucketVersioning |
				      s3GetBucketRequestPayment |
				      s3GetBucketLocation |
				      s3GetBucketPolicy |
				      s3GetBucketNotification |
				      s3GetBucketLogging |
				      s3GetBucketTagging |
				      s3GetBucketWebsite |
				      s3GetLifecycleConfiguration |
				      s3GetReplicationConfiguration |
				      s3GetObjectTagging |
				      s3GetObjectVersionTagging));
  EXPECT_EQ(p->statements[2].notaction, s3None);
  ASSERT_FALSE(p->statements[2].resource.empty());
  ASSERT_EQ(p->statements[2].resource.size(), 2U);
  EXPECT_EQ(p->statements[2].resource.begin()->partition, Partition::aws);
  EXPECT_EQ(p->statements[2].resource.begin()->service, Service::s3);
  EXPECT_TRUE(p->statements[2].resource.begin()->region.empty());
  EXPECT_EQ(p->statements[2].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[2].resource.begin()->resource, "confidential-data");
  EXPECT_EQ((p->statements[2].resource.begin() + 1)->partition,
	    Partition::aws);
  EXPECT_EQ((p->statements[2].resource.begin() + 1)->service, Service::s3);
  EXPECT_TRUE((p->statements[2].resource.begin() + 1)->region.empty());
  EXPECT_EQ((p->statements[2].resource.begin() + 1)->account,
	    arbitrary_tenant);
  EXPECT_EQ((p->statements[2].resource.begin() + 1)->resource,
	    "confidential-data/*");
  EXPECT_TRUE(p->statements[2].notresource.empty());
  ASSERT_FALSE(p->statements[2].conditions.empty());
  ASSERT_EQ(p->statements[2].conditions.size(), 1U);
  EXPECT_EQ(p->statements[2].conditions[0].op, TokenID::Bool);
  EXPECT_EQ(p->statements[2].conditions[0].key, "aws:MultiFactorAuthPresent");
  EXPECT_FALSE(p->statements[2].conditions[0].ifexists);
  ASSERT_FALSE(p->statements[2].conditions[0].vals.empty());
  EXPECT_EQ(p->statements[2].conditions[0].vals.size(), 1U);
  EXPECT_EQ(p->statements[2].conditions[0].vals[0], "true");
}

TEST_F(PolicyTest, Eval3) {
  auto p  = Policy(cct.get(), arbitrary_tenant,
		   bufferlist::static_from_string(example3));
  Environment em;
  Environment tr = { { "aws:MultiFactorAuthPresent", "true" } };
  Environment fa = { { "aws:MultiFactorAuthPresent", "false" } };

  auto s3allow = (s3ListMultipartUploadParts | s3ListBucket |
		  s3ListBucketVersions | s3ListAllMyBuckets |
		  s3ListBucketMultiPartUploads | s3GetObject |
		  s3GetObjectVersion | s3GetObjectAcl | s3GetObjectVersionAcl |
		  s3GetObjectTorrent | s3GetObjectVersionTorrent |
		  s3GetAccelerateConfiguration | s3GetBucketAcl |
		  s3GetBucketCORS | s3GetBucketVersioning |
		  s3GetBucketRequestPayment | s3GetBucketLocation |
		  s3GetBucketPolicy | s3GetBucketNotification |
		  s3GetBucketLogging | s3GetBucketTagging |
		  s3GetBucketWebsite | s3GetLifecycleConfiguration |
		  s3GetReplicationConfiguration |
		  s3GetObjectTagging | s3GetObjectVersionTagging);

  EXPECT_EQ(p.eval(em, none, s3PutBucketPolicy,
		   ARN(Partition::aws, Service::s3,
		       "", arbitrary_tenant, "mybucket")),
	    Effect::Allow);

  EXPECT_EQ(p.eval(em, none, s3PutBucketPolicy,
		   ARN(Partition::aws, Service::s3,
		       "", arbitrary_tenant, "mybucket")),
	    Effect::Allow);


  for (auto i = 0ULL; i < s3Count; ++i) {
    auto op = 1ULL << i;
    if ((op == s3ListAllMyBuckets) || (op == s3PutBucketPolicy)) {
      continue;
    }

    EXPECT_EQ(p.eval(em, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(tr, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data")),
	      op & s3allow ? Effect::Allow : Effect::Pass);
    EXPECT_EQ(p.eval(fa, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data")),
	      Effect::Pass);

    EXPECT_EQ(p.eval(em, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data/moo")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(tr, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data/moo")),
	      op & s3allow ? Effect::Allow : Effect::Pass);
    EXPECT_EQ(p.eval(fa, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "confidential-data/moo")),
	      Effect::Pass);

    EXPECT_EQ(p.eval(em, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "really-confidential-data")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(tr, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "really-confidential-data")),
	      Effect::Pass);
    EXPECT_EQ(p.eval(fa, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant, "really-confidential-data")),
	      Effect::Pass);

    EXPECT_EQ(p.eval(em, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant,
			 "really-confidential-data/moo")), Effect::Pass);
    EXPECT_EQ(p.eval(tr, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant,
			 "really-confidential-data/moo")), Effect::Pass);
    EXPECT_EQ(p.eval(fa, none, op,
		     ARN(Partition::aws, Service::s3,
			 "", arbitrary_tenant,
			 "really-confidential-data/moo")), Effect::Pass);

  }
}

const string PolicyTest::arbitrary_tenant = "arbitrary_tenant";
string PolicyTest::example1 = R"(
{
  "Version": "2012-10-17",
  "Statement": {
    "Effect": "Allow",
    "Action": "s3:ListBucket",
    "Resource": "arn:aws:s3:::example_bucket"
  }
}
)";

string PolicyTest::example2 = R"(
{
  "Version": "2012-10-17",
  "Id": "S3-Account-Permissions",
  "Statement": [{
    "Sid": "1",
    "Effect": "Allow",
    "Principal": {"AWS": ["arn:aws:iam::ACCOUNT-ID-WITHOUT-HYPHENS:root"]},
    "Action": "s3:*",
    "Resource": [
      "arn:aws:s3:::mybucket",
      "arn:aws:s3:::mybucket/*"
    ]
  }]
}
)";

string PolicyTest::example3 = R"(
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Sid": "FirstStatement",
      "Effect": "Allow",
      "Action": ["s3:PutBucketPolicy"],
      "Resource": "*"
    },
    {
      "Sid": "SecondStatement",
      "Effect": "Allow",
      "Action": "s3:ListAllMyBuckets",
      "Resource": "*"
    },
    {
      "Sid": "ThirdStatement",
      "Effect": "Allow",
      "Action": [
	"s3:List*",
	"s3:Get*"
      ],
      "Resource": [
	"arn:aws:s3:::confidential-data",
	"arn:aws:s3:::confidential-data/*"
      ],
      "Condition": {"Bool": {"aws:MultiFactorAuthPresent": "true"}}
    }
  ]
}
)";

class IPPolicyTest : public ::testing::Test {
protected:
  intrusive_ptr<CephContext> cct;
  static const string arbitrary_tenant;
  static string ip_address_allow_example;
  static string ip_address_deny_example;
  static string ip_address_full_example;
  // 192.168.1.0/24
  const rgw::IAM::MaskedIP allowedIPv4Range = { false, rgw::IAM::Address("11000000101010000000000100000000"), 24 };
  // 192.168.1.1/32
  const rgw::IAM::MaskedIP blacklistedIPv4 = { false, rgw::IAM::Address("11000000101010000000000100000001"), 32 };
  // 2001:db8:85a3:0:0:8a2e:370:7334/128
  const rgw::IAM::MaskedIP allowedIPv6 = { true, rgw::IAM::Address("00100000000000010000110110111000100001011010001100000000000000000000000000000000100010100010111000000011011100000111001100110100"), 128 };
  // ::1
  const rgw::IAM::MaskedIP blacklistedIPv6 = { true, rgw::IAM::Address(1), 128 };
  // 2001:db8:85a3:0:0:8a2e:370:7330/124
  const rgw::IAM::MaskedIP allowedIPv6Range = { true, rgw::IAM::Address("00100000000000010000110110111000100001011010001100000000000000000000000000000000100010100010111000000011011100000111001100110000"), 124 };
public:
  IPPolicyTest() {
    cct = new CephContext(CEPH_ENTITY_TYPE_CLIENT);
  }
};
const string IPPolicyTest::arbitrary_tenant = "arbitrary_tenant";

TEST_F(IPPolicyTest, MaskedIPOperations) {
  EXPECT_EQ(stringify(allowedIPv4Range), "192.168.1.0/24");
  EXPECT_EQ(stringify(blacklistedIPv4), "192.168.1.1/32");
  EXPECT_EQ(stringify(allowedIPv6), "2001:db8:85a3:0:0:8a2e:370:7334/128");
  EXPECT_EQ(stringify(allowedIPv6Range), "2001:db8:85a3:0:0:8a2e:370:7330/124");
  EXPECT_EQ(stringify(blacklistedIPv6), "0:0:0:0:0:0:0:1/128");
  EXPECT_EQ(allowedIPv4Range, blacklistedIPv4);
  EXPECT_EQ(allowedIPv6Range, allowedIPv6);
}

TEST_F(IPPolicyTest, asNetworkIPv4Range) {
  auto actualIPv4Range = rgw::IAM::Condition::as_network("192.168.1.0/24");
  ASSERT_TRUE(actualIPv4Range.is_initialized());
  EXPECT_EQ(*actualIPv4Range, allowedIPv4Range);
}

TEST_F(IPPolicyTest, asNetworkIPv4) {
  auto actualIPv4 = rgw::IAM::Condition::as_network("192.168.1.1");
  ASSERT_TRUE(actualIPv4.is_initialized());
  EXPECT_EQ(*actualIPv4, blacklistedIPv4);
}

TEST_F(IPPolicyTest, asNetworkIPv6Range) {
  auto actualIPv6Range = rgw::IAM::Condition::as_network("2001:db8:85a3:0:0:8a2e:370:7330/124");
  ASSERT_TRUE(actualIPv6Range.is_initialized());
  EXPECT_EQ(*actualIPv6Range, allowedIPv6Range);
}

TEST_F(IPPolicyTest, asNetworkIPv6) {
  auto actualIPv6 = rgw::IAM::Condition::as_network("2001:db8:85a3:0:0:8a2e:370:7334");
  ASSERT_TRUE(actualIPv6.is_initialized());
  EXPECT_EQ(*actualIPv6, allowedIPv6);
}

TEST_F(IPPolicyTest, asNetworkInvalid) {
  EXPECT_FALSE(rgw::IAM::Condition::as_network(""));
  EXPECT_FALSE(rgw::IAM::Condition::as_network("192.168.1.1/33"));
  EXPECT_FALSE(rgw::IAM::Condition::as_network("2001:db8:85a3:0:0:8a2e:370:7334/129"));
  EXPECT_FALSE(rgw::IAM::Condition::as_network("192.168.1.1:"));
  EXPECT_FALSE(rgw::IAM::Condition::as_network("1.2.3.10000"));
}

TEST_F(IPPolicyTest, IPEnvironment) {
  // Unfortunately RGWCivetWeb is too tightly tied to civetweb to test RGWCivetWeb::init_env.
  RGWEnv rgw_env;
  RGWUserInfo user;
  RGWRados rgw_rados;
  rgw_env.set("REMOTE_ADDR", "192.168.1.1");
  rgw_env.set("HTTP_HOST", "1.2.3.4");
  req_state rgw_req_state(cct.get(), &rgw_env, &user);
  Environment iam_env = rgw_build_iam_environment(&rgw_rados, &rgw_req_state);
  auto ip = iam_env.find("aws:SourceIp");
  ASSERT_NE(ip, iam_env.end());
  EXPECT_EQ(ip->second, "192.168.1.1");

  ASSERT_EQ(cct.get()->_conf->set_val("rgw_remote_addr_param", "SOME_VAR"), 0);
  EXPECT_EQ(cct.get()->_conf->rgw_remote_addr_param, "SOME_VAR");
  iam_env = rgw_build_iam_environment(&rgw_rados, &rgw_req_state);
  ip = iam_env.find("aws:SourceIp");
  EXPECT_EQ(ip, iam_env.end());

  rgw_env.set("SOME_VAR", "192.168.1.2");
  iam_env = rgw_build_iam_environment(&rgw_rados, &rgw_req_state);
  ip = iam_env.find("aws:SourceIp");
  ASSERT_NE(ip, iam_env.end());
  EXPECT_EQ(ip->second, "192.168.1.2");

  ASSERT_EQ(cct.get()->_conf->set_val("rgw_remote_addr_param", "HTTP_X_FORWARDED_FOR"), 0);
  rgw_env.set("HTTP_X_FORWARDED_FOR", "192.168.1.3");
  iam_env = rgw_build_iam_environment(&rgw_rados, &rgw_req_state);
  ip = iam_env.find("aws:SourceIp");
  ASSERT_NE(ip, iam_env.end());
  EXPECT_EQ(ip->second, "192.168.1.3");

  rgw_env.set("HTTP_X_FORWARDED_FOR", "192.168.1.4, 4.3.2.1, 2001:db8:85a3:8d3:1319:8a2e:370:7348");
  iam_env = rgw_build_iam_environment(&rgw_rados, &rgw_req_state);
  ip = iam_env.find("aws:SourceIp");
  ASSERT_NE(ip, iam_env.end());
  EXPECT_EQ(ip->second, "192.168.1.4");
}

TEST_F(IPPolicyTest, ParseIPAddress) {
  optional<Policy> p;

  ASSERT_NO_THROW(p = Policy(cct.get(), arbitrary_tenant,
			     bufferlist::static_from_string(ip_address_full_example)));
  ASSERT_TRUE(p);

  EXPECT_EQ(p->text, ip_address_full_example);
  EXPECT_EQ(p->version, Version::v2012_10_17);
  EXPECT_EQ(*p->id, "S3IPPolicyTest");
  EXPECT_FALSE(p->statements.empty());
  EXPECT_EQ(p->statements.size(), 1U);
  EXPECT_EQ(*p->statements[0].sid, "IPAllow");
  EXPECT_FALSE(p->statements[0].princ.empty());
  EXPECT_EQ(p->statements[0].princ.size(), 1U);
  EXPECT_EQ(*p->statements[0].princ.begin(),
	    Principal::wildcard());
  EXPECT_TRUE(p->statements[0].noprinc.empty());
  EXPECT_EQ(p->statements[0].effect, Effect::Allow);
  EXPECT_EQ(p->statements[0].action, s3ListBucket);
  EXPECT_EQ(p->statements[0].notaction, s3None);
  ASSERT_FALSE(p->statements[0].resource.empty());
  ASSERT_EQ(p->statements[0].resource.size(), 2U);
  EXPECT_EQ(p->statements[0].resource.begin()->partition, Partition::aws);
  EXPECT_EQ(p->statements[0].resource.begin()->service, Service::s3);
  EXPECT_TRUE(p->statements[0].resource.begin()->region.empty());
  EXPECT_EQ(p->statements[0].resource.begin()->account, arbitrary_tenant);
  EXPECT_EQ(p->statements[0].resource.begin()->resource, "example_bucket");
  EXPECT_EQ((p->statements[0].resource.begin() + 1)->resource, "example_bucket/*");
  EXPECT_TRUE(p->statements[0].notresource.empty());
  ASSERT_FALSE(p->statements[0].conditions.empty());
  ASSERT_EQ(p->statements[0].conditions.size(), 2U);
  EXPECT_EQ(p->statements[0].conditions[0].op, TokenID::IpAddress);
  EXPECT_EQ(p->statements[0].conditions[0].key, "aws:SourceIp");
  ASSERT_FALSE(p->statements[0].conditions[0].vals.empty());
  EXPECT_EQ(p->statements[0].conditions[0].vals.size(), 2U);
  EXPECT_EQ(p->statements[0].conditions[0].vals[0], "192.168.1.0/24");
  EXPECT_EQ(p->statements[0].conditions[0].vals[1], "::1");
  optional<rgw::IAM::MaskedIP> convertedIPv4 = rgw::IAM::Condition::as_network(p->statements[0].conditions[0].vals[0]);
  EXPECT_TRUE(convertedIPv4.is_initialized());
  if (convertedIPv4.is_initialized()) {
    EXPECT_EQ(*convertedIPv4, allowedIPv4Range);
  }

  EXPECT_EQ(p->statements[0].conditions[1].op, TokenID::NotIpAddress);
  EXPECT_EQ(p->statements[0].conditions[1].key, "aws:SourceIp");
  ASSERT_FALSE(p->statements[0].conditions[1].vals.empty());
  EXPECT_EQ(p->statements[0].conditions[1].vals.size(), 2U);
  EXPECT_EQ(p->statements[0].conditions[1].vals[0], "192.168.1.1/32");
  EXPECT_EQ(p->statements[0].conditions[1].vals[1], "2001:0db8:85a3:0000:0000:8a2e:0370:7334");
  optional<rgw::IAM::MaskedIP> convertedIPv6 = rgw::IAM::Condition::as_network(p->statements[0].conditions[1].vals[1]);
  EXPECT_TRUE(convertedIPv6.is_initialized());
  if (convertedIPv6.is_initialized()) {
    EXPECT_EQ(*convertedIPv6, allowedIPv6);
  }
}

TEST_F(IPPolicyTest, EvalIPAddress) {
  auto allowp  = Policy(cct.get(), arbitrary_tenant,
			bufferlist::static_from_string(ip_address_allow_example));
  auto denyp  = Policy(cct.get(), arbitrary_tenant,
		       bufferlist::static_from_string(ip_address_deny_example));
  auto fullp  = Policy(cct.get(), arbitrary_tenant,
		   bufferlist::static_from_string(ip_address_full_example));
  Environment e;
  Environment allowedIP, blacklistedIP, allowedIPv6, blacklistedIPv6;
  allowedIP["aws:SourceIp"] = "192.168.1.2";
  allowedIPv6["aws:SourceIp"] = "::1";
  blacklistedIP["aws:SourceIp"] = "192.168.1.1";
  blacklistedIPv6["aws:SourceIp"] = "2001:0db8:85a3:0000:0000:8a2e:0370:7334";

  auto trueacct = FakeIdentity(
    Principal::tenant("ACCOUNT-ID-WITHOUT-HYPHENS"));
  // Without an IP address in the environment then evaluation will always pass
  EXPECT_EQ(allowp.eval(e, trueacct, s3ListBucket,
			ARN(Partition::aws, Service::s3,
			    "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);
  EXPECT_EQ(fullp.eval(e, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Pass);

  EXPECT_EQ(allowp.eval(allowedIP, trueacct, s3ListBucket,
			ARN(Partition::aws, Service::s3,
			    "", arbitrary_tenant, "example_bucket")),
	    Effect::Allow);
  EXPECT_EQ(allowp.eval(blacklistedIPv6, trueacct, s3ListBucket,
			ARN(Partition::aws, Service::s3,
			    "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);


  EXPECT_EQ(denyp.eval(allowedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Deny);
  EXPECT_EQ(denyp.eval(allowedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Deny);

  EXPECT_EQ(denyp.eval(blacklistedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);
  EXPECT_EQ(denyp.eval(blacklistedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Pass);

  EXPECT_EQ(denyp.eval(blacklistedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);
  EXPECT_EQ(denyp.eval(blacklistedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Pass);
  EXPECT_EQ(denyp.eval(allowedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Deny);
  EXPECT_EQ(denyp.eval(allowedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Deny);

  EXPECT_EQ(fullp.eval(allowedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Allow);
  EXPECT_EQ(fullp.eval(allowedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Allow);

  EXPECT_EQ(fullp.eval(blacklistedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);
  EXPECT_EQ(fullp.eval(blacklistedIP, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Pass);

  EXPECT_EQ(fullp.eval(allowedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Allow);
  EXPECT_EQ(fullp.eval(allowedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Allow);

  EXPECT_EQ(fullp.eval(blacklistedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket")),
	    Effect::Pass);
  EXPECT_EQ(fullp.eval(blacklistedIPv6, trueacct, s3ListBucket,
		       ARN(Partition::aws, Service::s3,
			   "", arbitrary_tenant, "example_bucket/myobject")),
	    Effect::Pass);
}

string IPPolicyTest::ip_address_allow_example = R"(
{
  "Version": "2012-10-17",
  "Id": "S3SimpleIPPolicyTest",
  "Statement": [{
    "Sid": "1",
    "Effect": "Allow",
    "Principal": {"AWS": ["arn:aws:iam::ACCOUNT-ID-WITHOUT-HYPHENS:root"]},
    "Action": "s3:ListBucket",
    "Resource": [
      "arn:aws:s3:::example_bucket"
    ],
    "Condition": {
      "IpAddress": {"aws:SourceIp": "192.168.1.0/24"}
    }
  }]
}
)";

string IPPolicyTest::ip_address_deny_example = R"(
{
  "Version": "2012-10-17",
  "Id": "S3IPPolicyTest",
  "Statement": {
    "Effect": "Deny",
    "Sid": "IPDeny",
    "Action": "s3:ListBucket",
    "Principal": {"AWS": ["arn:aws:iam::ACCOUNT-ID-WITHOUT-HYPHENS:root"]},
    "Resource": [
      "arn:aws:s3:::example_bucket",
      "arn:aws:s3:::example_bucket/*"
    ],
    "Condition": {
      "NotIpAddress": {"aws:SourceIp": ["192.168.1.1/32", "2001:0db8:85a3:0000:0000:8a2e:0370:7334"]}
    }
  }
}
)";

string IPPolicyTest::ip_address_full_example = R"(
{
  "Version": "2012-10-17",
  "Id": "S3IPPolicyTest",
  "Statement": {
    "Effect": "Allow",
    "Sid": "IPAllow",
    "Action": "s3:ListBucket",
    "Principal": "*",
    "Resource": [
      "arn:aws:s3:::example_bucket",
      "arn:aws:s3:::example_bucket/*"
    ],
    "Condition": {
      "IpAddress": {"aws:SourceIp": ["192.168.1.0/24", "::1"]},
      "NotIpAddress": {"aws:SourceIp": ["192.168.1.1/32", "2001:0db8:85a3:0000:0000:8a2e:0370:7334"]}
    }
  }
}
)";

TEST(MatchWildcards, Simple)
{
  EXPECT_TRUE(match_wildcards("", ""));
  EXPECT_TRUE(match_wildcards("", "", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("", "abc"));
  EXPECT_FALSE(match_wildcards("", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abc", ""));
  EXPECT_FALSE(match_wildcards("abc", "", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("abc", "abc"));
  EXPECT_TRUE(match_wildcards("abc", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abc", "abC"));
  EXPECT_TRUE(match_wildcards("abc", "abC", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abC", "abc"));
  EXPECT_TRUE(match_wildcards("abC", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abc", "abcd"));
  EXPECT_FALSE(match_wildcards("abc", "abcd", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abcd", "abc"));
  EXPECT_FALSE(match_wildcards("abcd", "abc", MATCH_CASE_INSENSITIVE));
}

TEST(MatchWildcards, QuestionMark)
{
  EXPECT_FALSE(match_wildcards("?", ""));
  EXPECT_FALSE(match_wildcards("?", "", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("?", "a"));
  EXPECT_TRUE(match_wildcards("?", "a", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("?bc", "abc"));
  EXPECT_TRUE(match_wildcards("?bc", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a?c", "abc"));
  EXPECT_TRUE(match_wildcards("a?c", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("abc", "a?c"));
  EXPECT_FALSE(match_wildcards("abc", "a?c", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("a?c", "abC"));
  EXPECT_TRUE(match_wildcards("a?c", "abC", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("ab?", "abc"));
  EXPECT_TRUE(match_wildcards("ab?", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a?c?e", "abcde"));
  EXPECT_TRUE(match_wildcards("a?c?e", "abcde", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("???", "abc"));
  EXPECT_TRUE(match_wildcards("???", "abc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("???", "abcd"));
  EXPECT_FALSE(match_wildcards("???", "abcd", MATCH_CASE_INSENSITIVE));
}

TEST(MatchWildcards, Asterisk)
{
  EXPECT_TRUE(match_wildcards("*", ""));
  EXPECT_TRUE(match_wildcards("*", "", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("", "*"));
  EXPECT_FALSE(match_wildcards("", "*", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("*a", ""));
  EXPECT_FALSE(match_wildcards("*a", "", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("*a", "a"));
  EXPECT_TRUE(match_wildcards("*a", "a", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a*", "a"));
  EXPECT_TRUE(match_wildcards("a*", "a", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a*c", "ac"));
  EXPECT_TRUE(match_wildcards("a*c", "ac", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a*c", "abbc"));
  EXPECT_TRUE(match_wildcards("a*c", "abbc", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("a*c", "abbC"));
  EXPECT_TRUE(match_wildcards("a*c", "abbC", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("a*c*e", "abBce"));
  EXPECT_TRUE(match_wildcards("a*c*e", "abBce", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("http://*.example.com",
                              "http://www.example.com"));
  EXPECT_TRUE(match_wildcards("http://*.example.com",
                              "http://www.example.com", MATCH_CASE_INSENSITIVE));
  EXPECT_FALSE(match_wildcards("http://*.example.com",
                               "http://www.Example.com"));
  EXPECT_TRUE(match_wildcards("http://*.example.com",
                              "http://www.Example.com", MATCH_CASE_INSENSITIVE));
  EXPECT_TRUE(match_wildcards("http://example.com/*",
                              "http://example.com/index.html"));
  EXPECT_TRUE(match_wildcards("http://example.com/*/*.jpg",
                              "http://example.com/fun/smiley.jpg"));
  // note: parsing of * is not greedy, so * does not match 'bc' here
  EXPECT_FALSE(match_wildcards("a*c", "abcc"));
  EXPECT_FALSE(match_wildcards("a*c", "abcc", MATCH_CASE_INSENSITIVE));
}

TEST(MatchPolicy, Action)
{
  constexpr auto flag = MATCH_POLICY_ACTION;
  EXPECT_TRUE(match_policy("a:b:c", "a:b:c", flag));
  EXPECT_TRUE(match_policy("a:b:c", "A:B:C", flag)); // case insensitive
  EXPECT_TRUE(match_policy("a:*:e", "a:bcd:e", flag));
  EXPECT_FALSE(match_policy("a:*", "a:b:c", flag)); // cannot span segments
}

TEST(MatchPolicy, Resource)
{
  constexpr auto flag = MATCH_POLICY_RESOURCE;
  EXPECT_TRUE(match_policy("a:b:c", "a:b:c", flag));
  EXPECT_FALSE(match_policy("a:b:c", "A:B:C", flag)); // case sensitive
  EXPECT_TRUE(match_policy("a:*:e", "a:bcd:e", flag));
  EXPECT_FALSE(match_policy("a:*", "a:b:c", flag)); // cannot span segments
}

TEST(MatchPolicy, ARN)
{
  constexpr auto flag = MATCH_POLICY_ARN;
  EXPECT_TRUE(match_policy("a:b:c", "a:b:c", flag));
  EXPECT_TRUE(match_policy("a:b:c", "A:B:C", flag)); // case insensitive
  EXPECT_TRUE(match_policy("a:*:e", "a:bcd:e", flag));
  EXPECT_FALSE(match_policy("a:*", "a:b:c", flag)); // cannot span segments
}

TEST(MatchPolicy, String)
{
  constexpr auto flag = MATCH_POLICY_STRING;
  EXPECT_TRUE(match_policy("a:b:c", "a:b:c", flag));
  EXPECT_FALSE(match_policy("a:b:c", "A:B:C", flag)); // case sensitive
  EXPECT_TRUE(match_policy("a:*:e", "a:bcd:e", flag));
  EXPECT_FALSE(match_policy("a:*", "a:b:c", flag)); // cannot span segments
}
