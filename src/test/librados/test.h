// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_TEST_RADOS_API_TEST_H
#define CEPH_TEST_RADOS_API_TEST_H

#include "include/rados/librados.h"
#include "include/rados/librados.hpp"

#include <map>
#include <string>
#include <unistd.h>

std::string get_temp_pool_name(const std::string &prefix = "test-rados-api-");

std::string create_one_pool(const std::string &pool_name, rados_t *cluster,
    uint32_t pg_num=0);
std::string create_one_ec_pool(const std::string &pool_name, rados_t *cluster);
std::string create_one_pool_pp(const std::string &pool_name,
			    librados::Rados &cluster);
std::string create_one_pool_pp(const std::string &pool_name,
			       librados::Rados &cluster,
			       const std::map<std::string, std::string> &config);
std::string create_one_ec_pool_pp(const std::string &pool_name,
			    librados::Rados &cluster);
std::string connect_cluster(rados_t *cluster);
std::string connect_cluster_pp(librados::Rados &cluster);
std::string connect_cluster_pp(librados::Rados &cluster,
			       const std::map<std::string, std::string> &config);
int destroy_one_pool(const std::string &pool_name, rados_t *cluster);
int destroy_one_ec_pool(const std::string &pool_name, rados_t *cluster);
int destroy_one_pool_pp(const std::string &pool_name, librados::Rados &cluster);
int destroy_one_ec_pool_pp(const std::string &pool_name, librados::Rados &cluster);
void assert_eq_sparse(bufferlist& expected,
                      const std::map<uint64_t, uint64_t>& extents,
                      bufferlist& actual);

class TestAlarm
{
public:
  TestAlarm() {
    alarm(1200);
  }
  ~TestAlarm() {
    alarm(0);
  }
};

#endif
