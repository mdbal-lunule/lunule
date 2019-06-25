// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2011 New Dream Network
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "include/int_types.h"
#include "include/rados/librados.h"
#include "include/rbd_types.h"
#include "include/rbd/librbd.h"
#include "include/rbd/librbd.hpp"
#include "include/event_type.h"
#include "include/err.h"

#include "gtest/gtest.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <sstream>
#include <list>
#include <set>
#include <thread>
#include <vector>

#include "test/librados/test.h"
#include "test/librbd/test_support.h"
#include "common/event_socket.h"
#include "include/interval_set.h"
#include "include/stringify.h"

#include <boost/assign/list_of.hpp>
#include <boost/scope_exit.hpp>

#ifdef HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

using namespace std;

using std::chrono::seconds;

#define ASSERT_PASSED(x, args...) \
  do {                            \
    bool passed = false;          \
    x(args, &passed);             \
    ASSERT_TRUE(passed);          \
  } while(0)

void register_test_librbd() {
}

static int get_features(bool *old_format, uint64_t *features)
{
  const char *c = getenv("RBD_FEATURES");
  if (c) {
    stringstream ss;
    ss << c;
    ss >> *features;
    if (ss.fail())
      return -EINVAL;
    *old_format = false;
    cout << "using new format!" << std::endl;
  } else {
    *old_format = true;
    *features = 0;
    cout << "using old format" << std::endl;
  }

  return 0;
}

static int create_image_full(rados_ioctx_t ioctx, const char *name,
                             uint64_t size, int *order, int old_format,
                             uint64_t features)
{
  if (old_format) {
    // ensure old-format tests actually use the old format
    int r = rados_conf_set(rados_ioctx_get_cluster(ioctx),
                           "rbd_default_format", "1");
    if (r < 0) {
      return r;
    }
    return rbd_create(ioctx, name, size, order);
  } else if ((features & RBD_FEATURE_STRIPINGV2) != 0) {
    uint64_t stripe_unit = IMAGE_STRIPE_UNIT;
    if (*order) {
      // use a conservative stripe_unit for non default order
      stripe_unit = (1ull << (*order-1));
    }

    printf("creating image with stripe unit: %" PRIu64 ", "
	   "stripe count: %" PRIu64 "\n",
           stripe_unit, IMAGE_STRIPE_COUNT);
    return rbd_create3(ioctx, name, size, features, order,
                       stripe_unit, IMAGE_STRIPE_COUNT);
  } else {
    return rbd_create2(ioctx, name, size, features, order);
  }
}

static int clone_image(rados_ioctx_t p_ioctx,
                       rbd_image_t p_image, const char *p_name,
                       const char *p_snap_name, rados_ioctx_t c_ioctx,
                       const char *c_name, uint64_t features, int *c_order)
{
  uint64_t stripe_unit, stripe_count;

  int r;
  r = rbd_get_stripe_unit(p_image, &stripe_unit);
  if (r != 0) {
    return r;
  }

  r = rbd_get_stripe_count(p_image, &stripe_count);
  if (r != 0) {
    return r;
  }

  return rbd_clone2(p_ioctx, p_name, p_snap_name, c_ioctx,
                    c_name, features, c_order, stripe_unit, stripe_count);
}


static int create_image(rados_ioctx_t ioctx, const char *name,
			uint64_t size, int *order)
{
  bool old_format;
  uint64_t features;

  int r = get_features(&old_format, &features);
  if (r < 0)
    return r;
  return create_image_full(ioctx, name, size, order, old_format, features);
}

static int create_image_pp(librbd::RBD &rbd,
			   librados::IoCtx &ioctx,
			   const char *name,
			   uint64_t size, int *order) {
  bool old_format;
  uint64_t features;
  int r = get_features(&old_format, &features);
  if (r < 0)
    return r;
  if (old_format) {
    librados::Rados rados(ioctx);
    int r = rados.conf_set("rbd_default_format", "1");
    if (r < 0) {
      return r;
    }
    return rbd.create(ioctx, name, size, order);
  } else {
    return rbd.create2(ioctx, name, size, features, order);
  }
}

class TestLibRBD : public ::testing::Test {
public:

  TestLibRBD() : m_pool_number() {
  }

  static void SetUpTestCase() {
    static bool seeded = false;
    if (!seeded) {
      seeded = true;
      int seed = getpid();
      cout << "seed " << seed << std::endl;
      srand(seed);
    }

    _pool_names.clear();
    _unique_pool_names.clear();
    _image_number = 0;
    ASSERT_EQ("", connect_cluster(&_cluster));
    ASSERT_EQ("", connect_cluster_pp(_rados));

    create_optional_data_pool();
  }

  static void TearDownTestCase() {
    rados_shutdown(_cluster);
    _rados.wait_for_latest_osdmap();
    _pool_names.insert(_pool_names.end(), _unique_pool_names.begin(),
		       _unique_pool_names.end());
    for (size_t i = 1; i < _pool_names.size(); ++i) {
      ASSERT_EQ(0, _rados.pool_delete(_pool_names[i].c_str()));
    }
    if (!_pool_names.empty()) {
      ASSERT_EQ(0, destroy_one_pool_pp(_pool_names[0], _rados));
    }
  }

  void SetUp() override {
    ASSERT_NE("", m_pool_name = create_pool());
  }

  bool is_skip_partial_discard_enabled() {
    std::string value;
    EXPECT_EQ(0, _rados.conf_get("rbd_skip_partial_discard", value));
    return value == "true";
  }

  void validate_object_map(rbd_image_t image, bool *passed) {
    uint64_t flags;
    ASSERT_EQ(0, rbd_get_flags(image, &flags));
    *passed = ((flags & RBD_FLAG_OBJECT_MAP_INVALID) == 0);
  }

  void validate_object_map(librbd::Image &image, bool *passed) {
    uint64_t flags;
    ASSERT_EQ(0, image.get_flags(&flags));
    *passed = ((flags & RBD_FLAG_OBJECT_MAP_INVALID) == 0);
  }

  std::string get_temp_image_name() {
    ++_image_number;
    return "image" + stringify(_image_number);
  }

  static void create_optional_data_pool() {
    bool created = false;
    std::string data_pool;
    ASSERT_EQ(0, create_image_data_pool(_rados, data_pool, &created));
    if (!data_pool.empty()) {
      printf("using image data pool: %s\n", data_pool.c_str());
      if (created) {
        _unique_pool_names.push_back(data_pool);
      }
    }
  }

  std::string create_pool(bool unique = false) {
    librados::Rados rados;
    std::string pool_name;
    if (unique) {
      pool_name = get_temp_pool_name("test-librbd-");
      EXPECT_EQ("", create_one_pool_pp(pool_name, rados));
      _unique_pool_names.push_back(pool_name);
    } else if (m_pool_number < _pool_names.size()) {
      pool_name = _pool_names[m_pool_number];
    } else {
      pool_name = get_temp_pool_name("test-librbd-");
      EXPECT_EQ("", create_one_pool_pp(pool_name, rados));
      _pool_names.push_back(pool_name);
    }
    ++m_pool_number;
    return pool_name;
  }

  static std::vector<std::string> _pool_names;
  static std::vector<std::string> _unique_pool_names;
  static rados_t _cluster;
  static librados::Rados _rados;
  static uint64_t _image_number;

  std::string m_pool_name;
  uint32_t m_pool_number;

};

std::vector<std::string> TestLibRBD::_pool_names;
std::vector<std::string> TestLibRBD::_unique_pool_names;
rados_t TestLibRBD::_cluster;
librados::Rados TestLibRBD::_rados;
uint64_t TestLibRBD::_image_number = 0;

TEST_F(TestLibRBD, CreateAndStat)
{
  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_info_t info;
  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  printf("image has size %llu and order %d\n", (unsigned long long) info.size, info.order);
  ASSERT_EQ(info.size, size);
  ASSERT_EQ(info.order, order);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, CreateWithSameDataPool)
{
  REQUIRE_FORMAT_V2();

  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_t image;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  bool old_format;
  uint64_t features;
  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  rbd_image_options_t image_options;
  rbd_image_options_create(&image_options);
  BOOST_SCOPE_EXIT( (&image_options) ) {
    rbd_image_options_destroy(image_options);
  } BOOST_SCOPE_EXIT_END;

  ASSERT_EQ(0, rbd_image_options_set_uint64(image_options,
                                            RBD_IMAGE_OPTION_FEATURES,
                                            features));
  ASSERT_EQ(0, rbd_image_options_set_string(image_options,
                                            RBD_IMAGE_OPTION_DATA_POOL,
                                            m_pool_name.c_str()));

  ASSERT_EQ(0, rbd_create4(ioctx, name.c_str(), size, image_options));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, CreateAndStatPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::image_info_t info;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    
    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
    ASSERT_EQ(0, image.stat(info, sizeof(info)));
    ASSERT_EQ(info.size, size);
    ASSERT_EQ(info.order, order);
  }

  ioctx.close();
}

TEST_F(TestLibRBD, GetId)
{
  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char id[4096];
  if (!is_feature_enabled(0)) {
    // V1 image
    ASSERT_EQ(-EINVAL, rbd_get_id(image, id, sizeof(id)));
  } else {
    ASSERT_EQ(-ERANGE, rbd_get_id(image, id, 0));
    ASSERT_EQ(0, rbd_get_id(image, id, sizeof(id)));
    ASSERT_LT(0U, strlen(id));
  }

  ASSERT_EQ(0, rbd_close(image));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, GetIdPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = get_temp_image_name();

  std::string id;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
  if (!is_feature_enabled(0)) {
    // V1 image
    ASSERT_EQ(-EINVAL, image.get_id(&id));
  } else {
    ASSERT_EQ(0, image.get_id(&id));
    ASSERT_LT(0U, id.size());
  }
}

TEST_F(TestLibRBD, GetBlockNamePrefix)
{
  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char prefix[4096];
  ASSERT_EQ(-ERANGE, rbd_get_block_name_prefix(image, prefix, 0));
  ASSERT_EQ(0, rbd_get_block_name_prefix(image, prefix, sizeof(prefix)));
  ASSERT_LT(0U, strlen(prefix));

  ASSERT_EQ(0, rbd_close(image));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, GetBlockNamePrefixPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = get_temp_image_name();

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
  ASSERT_LT(0U, image.get_block_name_prefix().size());
}

TEST_F(TestLibRBD, TestGetCreateTimestamp)
{
  REQUIRE_FORMAT_V2();

  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  struct timespec timestamp;
  ASSERT_EQ(0, rbd_get_create_timestamp(image, &timestamp));
  ASSERT_LT(0, timestamp.tv_sec);

  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, GetCreateTimestampPP)
{
  REQUIRE_FORMAT_V2();

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = get_temp_image_name();

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), 0, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  struct timespec timestamp;
  ASSERT_EQ(0, image.get_create_timestamp(&timestamp));
  ASSERT_LT(0, timestamp.tv_sec);
}

TEST_F(TestLibRBD, OpenAio)
{
  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  rbd_image_info_t info;
  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));

  rbd_completion_t open_comp;
  ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &open_comp));
  ASSERT_EQ(0, rbd_aio_open(ioctx, name.c_str(), &image, NULL, open_comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(open_comp));
  ASSERT_EQ(1, rbd_aio_is_complete(open_comp));
  ASSERT_EQ(0, rbd_aio_get_return_value(open_comp));
  rbd_aio_release(open_comp);

  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  printf("image has size %llu and order %d\n", (unsigned long long) info.size, info.order);
  ASSERT_EQ(info.size, size);
  ASSERT_EQ(info.order, order);

  rbd_completion_t close_comp;
  ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &close_comp));
  ASSERT_EQ(0, rbd_aio_close(image, close_comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(close_comp));
  ASSERT_EQ(1, rbd_aio_is_complete(close_comp));
  ASSERT_EQ(0, rbd_aio_get_return_value(close_comp));
  rbd_aio_release(close_comp);

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, OpenAioFail)
{
  rados_ioctx_t ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));

  std::string name = get_temp_image_name();
  rbd_image_t image;
  rbd_completion_t open_comp;
  ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &open_comp));
  ASSERT_EQ(0, rbd_aio_open(ioctx, name.c_str(), &image, NULL, open_comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(open_comp));
  ASSERT_EQ(1, rbd_aio_is_complete(open_comp));
  ASSERT_EQ(-ENOENT, rbd_aio_get_return_value(open_comp));
  rbd_aio_release(open_comp);

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, OpenAioPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::image_info_t info;
  librbd::Image image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::RBD::AioCompletion *open_comp =
      new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, rbd.aio_open(ioctx, image, name.c_str(), NULL, open_comp));
  ASSERT_EQ(0, open_comp->wait_for_complete());
  ASSERT_EQ(1, open_comp->is_complete());
  ASSERT_EQ(0, open_comp->get_return_value());
  open_comp->release();

  ASSERT_EQ(0, image.stat(info, sizeof(info)));
  ASSERT_EQ(info.size, size);
  ASSERT_EQ(info.order, order);

  // reopen
  open_comp = new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, rbd.aio_open(ioctx, image, name.c_str(), NULL, open_comp));
  ASSERT_EQ(0, open_comp->wait_for_complete());
  ASSERT_EQ(1, open_comp->is_complete());
  ASSERT_EQ(0, open_comp->get_return_value());
  open_comp->release();

  // close
  librbd::RBD::AioCompletion *close_comp =
      new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_close(close_comp));
  ASSERT_EQ(0, close_comp->wait_for_complete());
  ASSERT_EQ(1, close_comp->is_complete());
  ASSERT_EQ(0, close_comp->get_return_value());
  close_comp->release();

  // close closed image
  close_comp = new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(-EINVAL, image.aio_close(close_comp));
  close_comp->release();

  ioctx.close();
}

TEST_F(TestLibRBD, OpenAioFailPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    std::string name = get_temp_image_name();

    librbd::RBD::AioCompletion *open_comp =
      new librbd::RBD::AioCompletion(NULL, NULL);
    ASSERT_EQ(0, rbd.aio_open(ioctx, image, name.c_str(), NULL, open_comp));
    ASSERT_EQ(0, open_comp->wait_for_complete());
    ASSERT_EQ(1, open_comp->is_complete());
    ASSERT_EQ(-ENOENT, open_comp->get_return_value());
    open_comp->release();
  }

  ioctx.close();
}

TEST_F(TestLibRBD, ResizeAndStat)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_info_t info;
  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_resize(image, size * 4));
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  ASSERT_EQ(info.size, size * 4);

  ASSERT_EQ(0, rbd_resize(image, size / 2));
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  ASSERT_EQ(info.size, size / 2);

  // downsizing without allowing shrink should fail
  // and image size should not change
  ASSERT_EQ(-EINVAL, rbd_resize2(image, size / 4, false, NULL, NULL));
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  ASSERT_EQ(info.size, size / 2);

  ASSERT_EQ(0, rbd_resize2(image, size / 4, true, NULL, NULL));
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  ASSERT_EQ(info.size, size / 4);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, ResizeAndStatPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::image_info_t info;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    
    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
    
    ASSERT_EQ(0, image.resize(size * 4));
    ASSERT_EQ(0, image.stat(info, sizeof(info)));
    ASSERT_EQ(info.size, size * 4);
    
    ASSERT_EQ(0, image.resize(size / 2));
    ASSERT_EQ(0, image.stat(info, sizeof(info)));
    ASSERT_EQ(info.size, size / 2);
    ASSERT_PASSED(validate_object_map, image);
  }

  ioctx.close();
}

TEST_F(TestLibRBD, UpdateWatchAndResize)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  struct Watcher {
    rbd_image_t &m_image;
    mutex m_lock;
    condition_variable m_cond;
    size_t m_size = 0;
    static void cb(void *arg) {
      Watcher *watcher = static_cast<Watcher *>(arg);
      watcher->handle_notify();
    }
    Watcher(rbd_image_t &image) : m_image(image) {}
    void handle_notify() {
      rbd_image_info_t info;
      ASSERT_EQ(0, rbd_stat(m_image, &info, sizeof(info)));
      lock_guard<mutex> locker(m_lock);
      m_size = info.size;
      m_cond.notify_one();
    }
    void wait_for_size(size_t size) {
      unique_lock<mutex> locker(m_lock);
      ASSERT_TRUE(m_cond.wait_for(locker, seconds(5),
      				  [size, this] {
				    return this->m_size == size;}));
    }
  } watcher(image);
  uint64_t handle;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_update_watch(image, &handle, Watcher::cb, &watcher));

  ASSERT_EQ(0, rbd_resize(image, size * 4));
  watcher.wait_for_size(size * 4);

  ASSERT_EQ(0, rbd_resize(image, size / 2));
  watcher.wait_for_size(size / 2);

  ASSERT_EQ(0, rbd_update_unwatch(image, handle));

  ASSERT_EQ(0, rbd_close(image));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, UpdateWatchAndResizePP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    struct Watcher : public librbd::UpdateWatchCtx {
      Watcher(librbd::Image &image) : m_image(image) {
      }
      void handle_notify() override {
        librbd::image_info_t info;
	ASSERT_EQ(0, m_image.stat(info, sizeof(info)));
        lock_guard<mutex> locker(m_lock);
        m_size = info.size;
	m_cond.notify_one();
      }
      void wait_for_size(size_t size) {
	unique_lock<mutex> locker(m_lock);
	ASSERT_TRUE(m_cond.wait_for(locker, seconds(5),
				    [size, this] {
				      return this->m_size == size;}));
      }
      librbd::Image &m_image;
      mutex m_lock;
      condition_variable m_cond;
      size_t m_size = 0;
    } watcher(image);
    uint64_t handle;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    ASSERT_EQ(0, image.update_watch(&watcher, &handle));

    ASSERT_EQ(0, image.resize(size * 4));
    watcher.wait_for_size(size * 4);

    ASSERT_EQ(0, image.resize(size / 2));
    watcher.wait_for_size(size / 2);

    ASSERT_EQ(0, image.update_unwatch(handle));
  }

  ioctx.close();
}

int test_ls(rados_ioctx_t io_ctx, size_t num_expected, ...)
{
  int num_images, i;
  char *names, *cur_name;
  va_list ap;
  size_t max_size = 1024;

  names = (char *) malloc(sizeof(char) * 1024);
  int len = rbd_list(io_ctx, names, &max_size);

  std::set<std::string> image_names;
  for (i = 0, num_images = 0, cur_name = names; cur_name < names + len; i++) {
    printf("image: %s\n", cur_name);
    image_names.insert(cur_name);
    cur_name += strlen(cur_name) + 1;
    num_images++;
  }
  free(names);

  va_start(ap, num_expected);
  for (i = num_expected; i > 0; i--) {
    char *expected = va_arg(ap, char *);
    printf("expected = %s\n", expected);
    std::set<std::string>::iterator it = image_names.find(expected);
    if (it != image_names.end()) {
      printf("found %s\n", expected);
      image_names.erase(it);
      printf("erased %s\n", expected);
    } else {
      ADD_FAILURE() << "Unable to find image " << expected;
      va_end(ap);
      return -ENOENT;
    }
  }
  va_end(ap);

  if (!image_names.empty()) {
    ADD_FAILURE() << "Unexpected images discovered";
    return -EINVAL;
  }
  return num_images;
}

TEST_F(TestLibRBD, TestCreateLsDelete)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, create_pool(true).c_str(), &ioctx);

  int order = 0;
  std::string name = get_temp_image_name();
  std::string name2 = get_temp_image_name();
  uint64_t size = 2 << 20;
  
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(1, test_ls(ioctx, 1, name.c_str()));
  ASSERT_EQ(0, create_image(ioctx, name2.c_str(), size, &order));
  ASSERT_EQ(2, test_ls(ioctx, 2, name.c_str(), name2.c_str()));
  ASSERT_EQ(0, rbd_remove(ioctx, name.c_str()));
  ASSERT_EQ(1, test_ls(ioctx, 1, name2.c_str()));

  ASSERT_EQ(-ENOENT, rbd_remove(ioctx, name.c_str()));

  rados_ioctx_destroy(ioctx);
}

int test_ls_pp(librbd::RBD& rbd, librados::IoCtx& io_ctx, size_t num_expected, ...)
{
  int r;
  size_t i;
  va_list ap;
  vector<string> names;
  r = rbd.list(io_ctx, names);
  if (r == -ENOENT)
    r = 0;
  EXPECT_TRUE(r >= 0);
  cout << "num images is: " << names.size() << std::endl
	    << "expected: " << num_expected << std::endl;
  int num = names.size();

  for (i = 0; i < names.size(); i++) {
    cout << "image: " << names[i] << std::endl;
  }

  va_start(ap, num_expected);
  for (i = num_expected; i > 0; i--) {
    char *expected = va_arg(ap, char *);
    cout << "expected = " << expected << std::endl;
    vector<string>::iterator listed_name = find(names.begin(), names.end(), string(expected));
    if (listed_name == names.end()) {
      ADD_FAILURE() << "Unable to find image " << expected;
      va_end(ap);
      return -ENOENT;
    }
    names.erase(listed_name);
  }
  va_end(ap);

  if (!names.empty()) {
    ADD_FAILURE() << "Unexpected images discovered";
    return -EINVAL;
  }
  return num;
}

TEST_F(TestLibRBD, TestCreateLsDeletePP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(create_pool(true).c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    std::string name2 = get_temp_image_name();
    uint64_t size = 2 << 20;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(1, test_ls_pp(rbd, ioctx, 1, name.c_str()));
    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name2.c_str(), size, &order));
    ASSERT_EQ(2, test_ls_pp(rbd, ioctx, 2, name.c_str(), name2.c_str()));
    ASSERT_EQ(0, rbd.remove(ioctx, name.c_str()));
    ASSERT_EQ(1, test_ls_pp(rbd, ioctx, 1, name2.c_str()));
  }

  ioctx.close();
}


static int print_progress_percent(uint64_t offset, uint64_t src_size,
				     void *data)
{
  float percent = ((float)offset * 100) / src_size;
  printf("%3.2f%% done\n", percent);
  return 0; 
}

TEST_F(TestLibRBD, TestCopy)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, create_pool(true).c_str(), &ioctx);

  rbd_image_t image;
  rbd_image_t image2;
  rbd_image_t image3;
  int order = 0;
  std::string name = get_temp_image_name();
  std::string name2 = get_temp_image_name();
  std::string name3 = get_temp_image_name();

  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));
  ASSERT_EQ(1, test_ls(ioctx, 1, name.c_str()));

  size_t sum_key_len = 0;
  size_t sum_value_len = 0;
  std::string key;
  std::string val;
  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_set(image, key.c_str(), val.c_str()));

    sum_key_len += (key.size() + 1);
    sum_value_len += (val.size() + 1);
  }

  char keys[1024];
  char vals[1024];
  size_t keys_len = sizeof(keys);
  size_t vals_len = sizeof(vals);

  char value[1024];
  size_t value_len = sizeof(value);

  ASSERT_EQ(0, rbd_copy(image, ioctx, name2.c_str()));
  ASSERT_EQ(2, test_ls(ioctx, 2, name.c_str(), name2.c_str()));
  ASSERT_EQ(0, rbd_open(ioctx, name2.c_str(), &image2, NULL));
  ASSERT_EQ(0, rbd_metadata_list(image2, "", 70, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, sum_key_len);
  ASSERT_EQ(vals_len, sum_value_len);

  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_get(image2, key.c_str(), value, &value_len));
    ASSERT_STREQ(val.c_str(), value);

    value_len = sizeof(value);
  }

  ASSERT_EQ(0, rbd_copy_with_progress(image, ioctx, name3.c_str(),
				      print_progress_percent, NULL));
  ASSERT_EQ(3, test_ls(ioctx, 3, name.c_str(), name2.c_str(), name3.c_str()));

  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  ASSERT_EQ(0, rbd_open(ioctx, name3.c_str(), &image3, NULL));
  ASSERT_EQ(0, rbd_metadata_list(image3, "", 70, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, sum_key_len);
  ASSERT_EQ(vals_len, sum_value_len);

  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_get(image3, key.c_str(), value, &value_len));
    ASSERT_STREQ(val.c_str(), value);

    value_len = sizeof(value);
  }

  ASSERT_EQ(0, rbd_close(image));
  ASSERT_EQ(0, rbd_close(image2));
  ASSERT_EQ(0, rbd_close(image3));
  rados_ioctx_destroy(ioctx);
}

class PrintProgress : public librbd::ProgressContext
{
public:
  int update_progress(uint64_t offset, uint64_t src_size) override
  {
    float percent = ((float)offset * 100) / src_size;
    printf("%3.2f%% done\n", percent);
    return 0;
  }
};

TEST_F(TestLibRBD, TestCopyPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(create_pool(true).c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    librbd::Image image2;
    librbd::Image image3;
    int order = 0;
    std::string name = get_temp_image_name();
    std::string name2 = get_temp_image_name();
    std::string name3 = get_temp_image_name();
    uint64_t size = 2 << 20;
    PrintProgress pp;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    std::string key;
    std::string val;
    for (int i = 1; i <= 70; i++) {
      key = "key" + stringify(i);
      val = "value" + stringify(i);
      ASSERT_EQ(0, image.metadata_set(key, val));
    }

    ASSERT_EQ(1, test_ls_pp(rbd, ioctx, 1, name.c_str()));
    ASSERT_EQ(0, image.copy(ioctx, name2.c_str()));
    ASSERT_EQ(2, test_ls_pp(rbd, ioctx, 2, name.c_str(), name2.c_str()));
    ASSERT_EQ(0, rbd.open(ioctx, image2, name2.c_str(), NULL));

    map<string, bufferlist> pairs;
    std::string value;
    ASSERT_EQ(0, image2.metadata_list("", 70, &pairs));
    ASSERT_EQ(70U, pairs.size());

    for (int i = 1; i <= 70; i++) {
      key = "key" + stringify(i);
      val = "value" + stringify(i);
      ASSERT_EQ(0, image2.metadata_get(key.c_str(), &value));
      ASSERT_STREQ(val.c_str(), value.c_str());
    }

    ASSERT_EQ(0, image.copy_with_progress(ioctx, name3.c_str(), pp));
    ASSERT_EQ(3, test_ls_pp(rbd, ioctx, 3, name.c_str(), name2.c_str(),
                           name3.c_str()));
    ASSERT_EQ(0, rbd.open(ioctx, image3, name3.c_str(), NULL));

    pairs.clear();
    ASSERT_EQ(0, image3.metadata_list("", 70, &pairs));
    ASSERT_EQ(70U, pairs.size());

    for (int i = 1; i <= 70; i++) {
      key = "key" + stringify(i);
      val = "value" + stringify(i);
      ASSERT_EQ(0, image3.metadata_get(key.c_str(), &value));
      ASSERT_STREQ(val.c_str(), value.c_str());
    }
  }

  ioctx.close();
}

int test_ls_snaps(rbd_image_t image, int num_expected, ...)
{
  int num_snaps, i, j, max_size = 10;
  va_list ap;
  rbd_snap_info_t snaps[max_size];
  num_snaps = rbd_snap_list(image, snaps, &max_size);
  printf("num snaps is: %d\nexpected: %d\n", num_snaps, num_expected);

  for (i = 0; i < num_snaps; i++) {
    printf("snap: %s\n", snaps[i].name);
  }

  va_start(ap, num_expected);
  for (i = num_expected; i > 0; i--) {
    char *expected = va_arg(ap, char *);
    uint64_t expected_size = va_arg(ap, uint64_t);
    bool found = false;
    for (j = 0; j < num_snaps; j++) {
      if (snaps[j].name == NULL)
	continue;
      if (strcmp(snaps[j].name, expected) == 0) {
	printf("found %s with size %llu\n", snaps[j].name, (unsigned long long) snaps[j].size);
	EXPECT_EQ(expected_size, snaps[j].size);
	free((void *) snaps[j].name);
	snaps[j].name = NULL;
	found = true;
	break;
      }
    }
    EXPECT_TRUE(found);
  }
  va_end(ap);

  for (i = 0; i < num_snaps; i++) {
    EXPECT_EQ((const char *)0, snaps[i].name);
  }

  return num_snaps;
}

TEST_F(TestLibRBD, TestCreateLsDeleteSnap)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  uint64_t size2 = 4 << 20;
  
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_snap_create(image, "snap1"));
  ASSERT_EQ(1, test_ls_snaps(image, 1, "snap1", size));
  ASSERT_EQ(0, rbd_resize(image, size2));
  ASSERT_EQ(0, rbd_snap_create(image, "snap2"));
  ASSERT_EQ(2, test_ls_snaps(image, 2, "snap1", size, "snap2", size2));
  ASSERT_EQ(0, rbd_snap_remove(image, "snap1"));
  ASSERT_EQ(1, test_ls_snaps(image, 1, "snap2", size2));
  ASSERT_EQ(0, rbd_snap_remove(image, "snap2"));
  ASSERT_EQ(0, test_ls_snaps(image, 0));
  
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

int test_get_snapshot_timestamp(rbd_image_t image, uint64_t snap_id)
{
  struct timespec timestamp;
  EXPECT_EQ(0, rbd_snap_get_timestamp(image, snap_id, &timestamp));
  EXPECT_LT(0, timestamp.tv_sec);
  return 0;
}

TEST_F(TestLibRBD, TestGetSnapShotTimeStamp)
{
  REQUIRE_FORMAT_V2();

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int num_snaps, max_size = 10;
  rbd_snap_info_t snaps[max_size];

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_snap_create(image, "snap1"));
  num_snaps = rbd_snap_list(image, snaps, &max_size);
  ASSERT_EQ(1, num_snaps);
  ASSERT_EQ(0, test_get_snapshot_timestamp(image, snaps[0].id));
  free((void *)snaps[0].name);

  ASSERT_EQ(0, rbd_snap_create(image, "snap2"));
  num_snaps = rbd_snap_list(image, snaps, &max_size);
  ASSERT_EQ(2, num_snaps);
  ASSERT_EQ(0, test_get_snapshot_timestamp(image, snaps[0].id));
  ASSERT_EQ(0, test_get_snapshot_timestamp(image, snaps[1].id));
  free((void *)snaps[0].name);
  free((void *)snaps[1].name);

  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}


int test_ls_snaps(librbd::Image& image, size_t num_expected, ...)
{
  int r;
  size_t i, j;
  va_list ap;
  vector<librbd::snap_info_t> snaps;
  r = image.snap_list(snaps);
  EXPECT_TRUE(r >= 0);
  cout << "num snaps is: " << snaps.size() << std::endl
	    << "expected: " << num_expected << std::endl;

  for (i = 0; i < snaps.size(); i++) {
    cout << "snap: " << snaps[i].name << std::endl;
  }

  va_start(ap, num_expected);
  for (i = num_expected; i > 0; i--) {
    char *expected = va_arg(ap, char *);
    uint64_t expected_size = va_arg(ap, uint64_t);
    int found = 0;
    for (j = 0; j < snaps.size(); j++) {
      if (snaps[j].name == "")
	continue;
      if (strcmp(snaps[j].name.c_str(), expected) == 0) {
	cout << "found " << snaps[j].name << " with size " << snaps[j].size
	     << std::endl;
	EXPECT_EQ(expected_size, snaps[j].size);
	snaps[j].name = "";
	found = 1;
	break;
      }
    }
    EXPECT_TRUE(found);
  }
  va_end(ap);

  for (i = 0; i < snaps.size(); i++) {
    EXPECT_EQ("", snaps[i].name);
  }

  return snaps.size();
}

TEST_F(TestLibRBD, TestCreateLsDeleteSnapPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    uint64_t size2 = 4 << 20;
    
    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
   
    bool exists;
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_create("snap1"));
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(1, test_ls_snaps(image, 1, "snap1", size));
    ASSERT_EQ(0, image.resize(size2));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_create("snap2"));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(2, test_ls_snaps(image, 2, "snap1", size, "snap2", size2));
    ASSERT_EQ(0, image.snap_remove("snap1"));
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(1, test_ls_snaps(image, 1, "snap2", size2));
    ASSERT_EQ(0, image.snap_remove("snap2"));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, test_ls_snaps(image, 0));
  }

  ioctx.close();
}

TEST_F(TestLibRBD, TestCreateLsRenameSnapPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    uint64_t size2 = 4 << 20;
    
    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
    
    bool exists;
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_create("snap1"));
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(1, test_ls_snaps(image, 1, "snap1", size));
    ASSERT_EQ(0, image.resize(size2));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_create("snap2"));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(2, test_ls_snaps(image, 2, "snap1", size, "snap2", size2));
    ASSERT_EQ(0, image.snap_rename("snap1","snap1-rename"));
    ASSERT_EQ(2, test_ls_snaps(image, 2, "snap1-rename", size, "snap2", size2));
    ASSERT_EQ(0, image.snap_exists2("snap1", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_exists2("snap1-rename", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(0, image.snap_remove("snap1-rename"));
    ASSERT_EQ(0, image.snap_rename("snap2","snap2-rename"));
    ASSERT_EQ(1, test_ls_snaps(image, 1, "snap2-rename", size2));
    ASSERT_EQ(0, image.snap_exists2("snap2", &exists));
    ASSERT_FALSE(exists);
    ASSERT_EQ(0, image.snap_exists2("snap2-rename", &exists));
    ASSERT_TRUE(exists);
    ASSERT_EQ(0, image.snap_remove("snap2-rename"));
    ASSERT_EQ(0, test_ls_snaps(image, 0));
  }

  ioctx.close();
}

void simple_write_cb(rbd_completion_t cb, void *arg)
{
  printf("write completion cb called!\n");
}

void simple_read_cb(rbd_completion_t cb, void *arg)
{
  printf("read completion cb called!\n");
}

void aio_write_test_data_and_poll(rbd_image_t image, int fd, const char *test_data,
                                  uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  uint64_t data = 0x123;
  rbd_aio_create_completion((void*)&data, (rbd_callback_t) simple_write_cb, &comp);
  printf("created completion\n");
  printf("started write\n");
  if (iohint)
    rbd_aio_write2(image, off, len, test_data, comp, iohint);
  else
    rbd_aio_write(image, off, len, test_data, comp);

  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;

  ASSERT_EQ(1, poll(&pfd, 1, -1));
  ASSERT_TRUE(pfd.revents & POLLIN);

  rbd_completion_t comps[1];
  ASSERT_EQ(1, rbd_poll_io_events(image, comps, 1));
  uint64_t count;
  ASSERT_EQ(static_cast<ssize_t>(sizeof(count)),
            read(fd, &count, sizeof(count)));
  int r = rbd_aio_get_return_value(comps[0]);
  ASSERT_TRUE(rbd_aio_is_complete(comps[0]));
  ASSERT_TRUE(*(uint64_t*)rbd_aio_get_arg(comps[0]) == data);
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished write\n");
  rbd_aio_release(comps[0]);
  *passed = true;
}

void aio_write_test_data(rbd_image_t image, const char *test_data, uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_write_cb, &comp);
  printf("created completion\n");
  if (iohint)
    rbd_aio_write2(image, off, len, test_data, comp, iohint);
  else
    rbd_aio_write(image, off, len, test_data, comp);
  printf("started write\n");
  rbd_aio_wait_for_complete(comp);
  int r = rbd_aio_get_return_value(comp);
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished write\n");
  rbd_aio_release(comp);
  *passed = true;
}

void write_test_data(rbd_image_t image, const char *test_data, uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  ssize_t written;
  if (iohint)
    written = rbd_write2(image, off, len, test_data, iohint);
  else
    written = rbd_write(image, off, len, test_data);
  printf("wrote: %d\n", (int) written);
  ASSERT_EQ(len, static_cast<size_t>(written));
  *passed = true;
}

void aio_discard_test_data(rbd_image_t image, uint64_t off, uint64_t len, bool *passed)
{
  rbd_completion_t comp;
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_write_cb, &comp);
  rbd_aio_discard(image, off, len, comp);
  rbd_aio_wait_for_complete(comp);
  int r = rbd_aio_get_return_value(comp);
  ASSERT_EQ(0, r);
  printf("aio discard: %d~%d = %d\n", (int)off, (int)len, (int)r);
  rbd_aio_release(comp);
  *passed = true;
}

void discard_test_data(rbd_image_t image, uint64_t off, size_t len, bool *passed)
{
  ssize_t written;
  written = rbd_discard(image, off, len);
  printf("discard: %d~%d = %d\n", (int)off, (int)len, (int)written);
  ASSERT_EQ(len, static_cast<size_t>(written));
  *passed = true;
}

void aio_read_test_data_and_poll(rbd_image_t image, int fd, const char *expected,
                                 uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  char *result = (char *)malloc(len + 1);

  ASSERT_NE(static_cast<char *>(NULL), result);
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  printf("created completion\n");
  printf("started read\n");
  if (iohint)
    rbd_aio_read2(image, off, len, result, comp, iohint);
  else
    rbd_aio_read(image, off, len, result, comp);

  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN;

  ASSERT_EQ(1, poll(&pfd, 1, -1));
  ASSERT_TRUE(pfd.revents & POLLIN);

  rbd_completion_t comps[1];
  ASSERT_EQ(1, rbd_poll_io_events(image, comps, 1));
  uint64_t count;
  ASSERT_EQ(static_cast<ssize_t>(sizeof(count)),
            read(fd, &count, sizeof(count)));

  int r = rbd_aio_get_return_value(comps[0]);
  ASSERT_TRUE(rbd_aio_is_complete(comps[0]));
  printf("return value is: %d\n", r);
  ASSERT_EQ(len, static_cast<size_t>(r));
  rbd_aio_release(comps[0]);
  if (memcmp(result, expected, len)) {
    printf("read: %s\nexpected: %s\n", result, expected);
    ASSERT_EQ(0, memcmp(result, expected, len));
  }
  free(result);
  *passed = true;
}

void aio_read_test_data(rbd_image_t image, const char *expected, uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  char *result = (char *)malloc(len + 1);

  ASSERT_NE(static_cast<char *>(NULL), result);
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  printf("created completion\n");
  if (iohint)
    rbd_aio_read2(image, off, len, result, comp, iohint);
  else
    rbd_aio_read(image, off, len, result, comp);
  printf("started read\n");
  rbd_aio_wait_for_complete(comp);
  int r = rbd_aio_get_return_value(comp);
  printf("return value is: %d\n", r);
  ASSERT_EQ(len, static_cast<size_t>(r));
  rbd_aio_release(comp);
  if (memcmp(result, expected, len)) {
    printf("read: %s\nexpected: %s\n", result, expected);
    ASSERT_EQ(0, memcmp(result, expected, len));
  }
  free(result);
  *passed = true;
}

void read_test_data(rbd_image_t image, const char *expected, uint64_t off, size_t len, uint32_t iohint, bool *passed)
{
  ssize_t read;
  char *result = (char *)malloc(len + 1);

  ASSERT_NE(static_cast<char *>(NULL), result);
  if (iohint)
    read = rbd_read2(image, off, len, result, iohint);
  else
    read = rbd_read(image, off, len, result);
  printf("read: %d\n", (int) read);
  ASSERT_EQ(len, static_cast<size_t>(read));
  result[len] = '\0';
  if (memcmp(result, expected, len)) {
    printf("read: %s\nexpected: %s\n", result, expected);
    ASSERT_EQ(0, memcmp(result, expected, len));
  }
  free(result);
  *passed = true;
}

void aio_writesame_test_data(rbd_image_t image, const char *test_data, uint64_t off, uint64_t len,
                             uint64_t data_len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_write_cb, &comp);
  printf("created completion\n");
  int r;
  r = rbd_aio_writesame(image, off, len, test_data, data_len, comp, iohint);
  printf("started writesame\n");
  if (len % data_len) {
    ASSERT_EQ(-EINVAL, r);
    printf("expected fail, finished writesame\n");
    rbd_aio_release(comp);
    *passed = true;
    return;
  }

  rbd_aio_wait_for_complete(comp);
  r = rbd_aio_get_return_value(comp);
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished writesame\n");
  rbd_aio_release(comp);

  //verify data
  printf("to verify the data\n");
  ssize_t read;
  char *result = (char *)malloc(data_len+ 1);
  ASSERT_NE(static_cast<char *>(NULL), result);
  uint64_t left = len;
  while (left > 0) {
    read = rbd_read(image, off, data_len, result);
    ASSERT_EQ(data_len, static_cast<size_t>(read));
    result[data_len] = '\0';
    if (memcmp(result, test_data, data_len)) {
      printf("read: %d ~ %d\n", (int) off, (int) read);
      printf("read: %s\nexpected: %s\n", result, test_data);
      ASSERT_EQ(0, memcmp(result, test_data, data_len));
    }
    off += data_len;
    left -= data_len;
  }
  ASSERT_EQ(0U, left);
  free(result);
  printf("verified\n");

  *passed = true;
}

void writesame_test_data(rbd_image_t image, const char *test_data, uint64_t off, uint64_t len,
                         uint64_t data_len, uint32_t iohint, bool *passed)
{
  ssize_t written;
  written = rbd_writesame(image, off, len, test_data, data_len, iohint);
  if (len % data_len) {
    ASSERT_EQ(-EINVAL, written);
    printf("expected fail, finished writesame\n");
    *passed = true;
    return;
  }
  ASSERT_EQ(len, static_cast<size_t>(written));
  printf("wrote: %d\n", (int) written);

  //verify data
  printf("to verify the data\n");
  ssize_t read;
  char *result = (char *)malloc(data_len+ 1);
  ASSERT_NE(static_cast<char *>(NULL), result);
  uint64_t left = len;
  while (left > 0) {
    read = rbd_read(image, off, data_len, result);
    ASSERT_EQ(data_len, static_cast<size_t>(read));
    result[data_len] = '\0';
    if (memcmp(result, test_data, data_len)) {
      printf("read: %d ~ %d\n", (int) off, (int) read);
      printf("read: %s\nexpected: %s\n", result, test_data);
      ASSERT_EQ(0, memcmp(result, test_data, data_len));
    }
    off += data_len;
    left -= data_len;
  }
  ASSERT_EQ(0U, left);
  free(result);
  printf("verified\n");

  *passed = true;
}

void aio_compare_and_write_test_data(rbd_image_t image, const char *cmp_data,
                                     const char *test_data, uint64_t off,
                                     size_t len, uint32_t iohint, bool *passed)
{
  rbd_completion_t comp;
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_write_cb, &comp);
  printf("created completion\n");

  uint64_t mismatch_offset;
  rbd_aio_compare_and_write(image, off, len, cmp_data, test_data, comp, &mismatch_offset, iohint);
  printf("started aio compare and write\n");
  rbd_aio_wait_for_complete(comp);
  int r = rbd_aio_get_return_value(comp);
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished aio compare and write\n");
  rbd_aio_release(comp);
  *passed = true;
}

void compare_and_write_test_data(rbd_image_t image, const char *cmp_data,
                                 const char *test_data, uint64_t off, size_t len,
                                 uint64_t *mismatch_off, uint32_t iohint, bool *passed)
{
  printf("start compare and write\n");
  ssize_t written;
  written = rbd_compare_and_write(image, off, len, cmp_data, test_data, mismatch_off, iohint);
  printf("compare and  wrote: %d\n", (int) written);
  ASSERT_EQ(len, static_cast<size_t>(written));
  *passed = true;
}


TEST_F(TestLibRBD, TestIO)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  bool skip_discard = is_skip_partial_discard_enabled();

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char test_data[TEST_IO_SIZE + 1];
  char zero_data[TEST_IO_SIZE + 1];
  char mismatch_data[TEST_IO_SIZE + 1];
  int i;
  uint64_t mismatch_offset;

  for (i = 0; i < TEST_IO_SIZE; ++i) {
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  }
  test_data[TEST_IO_SIZE] = '\0';
  memset(zero_data, 0, sizeof(zero_data));
  memset(mismatch_data, 9, sizeof(mismatch_data));

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(write_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_write_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(compare_and_write_test_data, image, test_data, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, &mismatch_offset, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_compare_and_write_test_data, image, test_data, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(read_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_read_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  // discard 2nd, 4th sections.
  ASSERT_PASSED(discard_test_data, image, TEST_IO_SIZE, TEST_IO_SIZE);
  ASSERT_PASSED(aio_discard_test_data, image, TEST_IO_SIZE*3, TEST_IO_SIZE);

  ASSERT_PASSED(read_test_data, image, test_data,  0, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*2, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE*3, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*4, TEST_IO_SIZE, 0);

  for (i = 0; i < 15; ++i) {
    if (i % 3 == 2) {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
    } else if (i % 3 == 1) {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
    } else {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
    }
  }
  for (i = 0; i < 15; ++i) {
    if (i % 3 == 2) {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
    } else if (i % 3 == 1) {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
    } else {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
    }
  }

  rbd_image_info_t info;
  rbd_completion_t comp;
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  // can't read or write starting past end
  ASSERT_EQ(-EINVAL, rbd_write(image, info.size, 1, test_data));
  ASSERT_EQ(-EINVAL, rbd_read(image, info.size, 1, test_data));
  // reading through end returns amount up to end
  ASSERT_EQ(10, rbd_read(image, info.size - 10, 100, test_data));
  // writing through end returns amount up to end
  ASSERT_EQ(10, rbd_write(image, info.size - 10, 100, test_data));

  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_write(image, info.size, 1, test_data, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(-EINVAL, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_read(image, info.size, 1, test_data, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(-EINVAL, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  ASSERT_PASSED(write_test_data, image, zero_data, 0, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
  ASSERT_EQ(-EILSEQ, rbd_compare_and_write(image, 0, TEST_IO_SIZE, mismatch_data, mismatch_data, &mismatch_offset, 0));
  ASSERT_EQ(0U, mismatch_offset);
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_compare_and_write(image, 0, TEST_IO_SIZE, mismatch_data, mismatch_data, comp, &mismatch_offset, 0));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(0U, mismatch_offset);
  rbd_aio_release(comp);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestIOWithIOHint)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  bool skip_discard = is_skip_partial_discard_enabled();

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char test_data[TEST_IO_SIZE + 1];
  char zero_data[TEST_IO_SIZE + 1];
  char mismatch_data[TEST_IO_SIZE + 1];
  int i;
  uint64_t mismatch_offset;

  for (i = 0; i < TEST_IO_SIZE; ++i) {
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  }
  test_data[TEST_IO_SIZE] = '\0';
  memset(zero_data, 0, sizeof(zero_data));
  memset(mismatch_data, 9, sizeof(mismatch_data));

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(write_test_data, image, test_data, TEST_IO_SIZE * i,
		  TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_write_test_data, image, test_data, TEST_IO_SIZE * i,
		  TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(compare_and_write_test_data, image, test_data, test_data,
      TEST_IO_SIZE * i, TEST_IO_SIZE, &mismatch_offset, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_compare_and_write_test_data, image, test_data, test_data,
      TEST_IO_SIZE * i, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(read_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE,
		  LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_read_test_data, image, test_data, TEST_IO_SIZE * i,
		  TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL|LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

  // discard 2nd, 4th sections.
  ASSERT_PASSED(discard_test_data, image, TEST_IO_SIZE, TEST_IO_SIZE);
  ASSERT_PASSED(aio_discard_test_data, image, TEST_IO_SIZE*3, TEST_IO_SIZE);

  ASSERT_PASSED(read_test_data, image, test_data,  0, TEST_IO_SIZE,
		LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE, TEST_IO_SIZE,
		LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*2, TEST_IO_SIZE,
		LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE*3, TEST_IO_SIZE,
		LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*4, TEST_IO_SIZE, 0);

  for (i = 0; i < 15; ++i) {
    if (i % 3 == 2) {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
    } else if (i % 3 == 1) {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
    } else {
      ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
    }
  }
  for (i = 0; i < 15; ++i) {
    if (i % 3 == 2) {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    } else if (i % 3 == 1) {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    } else {
      ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32,
                    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    }
  }

  rbd_image_info_t info;
  rbd_completion_t comp;
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  // can't read or write starting past end
  ASSERT_EQ(-EINVAL, rbd_write(image, info.size, 1, test_data));
  ASSERT_EQ(-EINVAL, rbd_read(image, info.size, 1, test_data));
  // reading through end returns amount up to end
  ASSERT_EQ(10, rbd_read2(image, info.size - 10, 100, test_data,
			  LIBRADOS_OP_FLAG_FADVISE_NOCACHE));
  // writing through end returns amount up to end
  ASSERT_EQ(10, rbd_write2(image, info.size - 10, 100, test_data,
			    LIBRADOS_OP_FLAG_FADVISE_DONTNEED));

  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_read2(image, info.size, 1, test_data, comp,
			     LIBRADOS_OP_FLAG_FADVISE_DONTNEED));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(-EINVAL, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  ASSERT_PASSED(write_test_data, image, zero_data, 0, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
  ASSERT_EQ(-EILSEQ, rbd_compare_and_write(image, 0, TEST_IO_SIZE, mismatch_data, mismatch_data,
                                           &mismatch_offset, LIBRADOS_OP_FLAG_FADVISE_DONTNEED));
  ASSERT_EQ(0U, mismatch_offset);
  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_compare_and_write(image, 0, TEST_IO_SIZE, mismatch_data, mismatch_data,
                                         comp, &mismatch_offset, LIBRADOS_OP_FLAG_FADVISE_DONTNEED));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(0U, mismatch_offset);
  rbd_aio_release(comp);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestDataPoolIO)
{
  REQUIRE_FORMAT_V2();

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  std::string data_pool_name = create_pool(true);

  bool skip_discard = is_skip_partial_discard_enabled();

  rbd_image_t image;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  bool old_format;
  uint64_t features;
  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  rbd_image_options_t image_options;
  rbd_image_options_create(&image_options);
  BOOST_SCOPE_EXIT( (&image_options) ) {
    rbd_image_options_destroy(image_options);
  } BOOST_SCOPE_EXIT_END;

  ASSERT_EQ(0, rbd_image_options_set_uint64(image_options,
                                            RBD_IMAGE_OPTION_FEATURES,
                                            features));
  ASSERT_EQ(0, rbd_image_options_set_string(image_options,
                                            RBD_IMAGE_OPTION_DATA_POOL,
                                            data_pool_name.c_str()));

  ASSERT_EQ(0, rbd_create4(ioctx, name.c_str(), size, image_options));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));
  ASSERT_NE(-1, rbd_get_data_pool_id(image));

  char test_data[TEST_IO_SIZE + 1];
  char zero_data[TEST_IO_SIZE + 1];
  int i;

  for (i = 0; i < TEST_IO_SIZE; ++i) {
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  }
  test_data[TEST_IO_SIZE] = '\0';
  memset(zero_data, 0, sizeof(zero_data));

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(write_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_write_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(read_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_read_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  // discard 2nd, 4th sections.
  ASSERT_PASSED(discard_test_data, image, TEST_IO_SIZE, TEST_IO_SIZE);
  ASSERT_PASSED(aio_discard_test_data, image, TEST_IO_SIZE*3, TEST_IO_SIZE);

  ASSERT_PASSED(read_test_data, image, test_data,  0, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*2, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		TEST_IO_SIZE*3, TEST_IO_SIZE, 0);
  ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*4, TEST_IO_SIZE, 0);

  rbd_image_info_t info;
  rbd_completion_t comp;
  ASSERT_EQ(0, rbd_stat(image, &info, sizeof(info)));
  // can't read or write starting past end
  ASSERT_EQ(-EINVAL, rbd_write(image, info.size, 1, test_data));
  ASSERT_EQ(-EINVAL, rbd_read(image, info.size, 1, test_data));
  // reading through end returns amount up to end
  ASSERT_EQ(10, rbd_read(image, info.size - 10, 100, test_data));
  // writing through end returns amount up to end
  ASSERT_EQ(10, rbd_write(image, info.size - 10, 100, test_data));

  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_write(image, info.size, 1, test_data, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(-EINVAL, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  rbd_aio_create_completion(NULL, (rbd_callback_t) simple_read_cb, &comp);
  ASSERT_EQ(0, rbd_aio_read(image, info.size, 1, test_data, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(-EINVAL, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestScatterGatherIO)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 20 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  std::string write_buffer("This is a test");
  struct iovec bad_iovs[] = {
    {.iov_base = NULL, .iov_len = static_cast<size_t>(-1)}
  };
  struct iovec write_iovs[] = {
    {.iov_base = &write_buffer[0],  .iov_len = 5},
    {.iov_base = &write_buffer[5],  .iov_len = 3},
    {.iov_base = &write_buffer[8],  .iov_len = 2},
    {.iov_base = &write_buffer[10], .iov_len = 4}
  };

  rbd_completion_t comp;
  rbd_aio_create_completion(NULL, NULL, &comp);
  ASSERT_EQ(-EINVAL, rbd_aio_writev(image, write_iovs, 0, 0, comp));
  ASSERT_EQ(-EINVAL, rbd_aio_writev(image, bad_iovs, 1, 0, comp));
  ASSERT_EQ(0, rbd_aio_writev(image, write_iovs,
                              sizeof(write_iovs) / sizeof(struct iovec),
                              1<<order, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(0, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);

  std::string read_buffer(write_buffer.size(), '1');
  struct iovec read_iovs[] = {
    {.iov_base = &read_buffer[0],  .iov_len = 4},
    {.iov_base = &read_buffer[8],  .iov_len = 4},
    {.iov_base = &read_buffer[12], .iov_len = 2}
  };

  rbd_aio_create_completion(NULL, NULL, &comp);
  ASSERT_EQ(-EINVAL, rbd_aio_readv(image, read_iovs, 0, 0, comp));
  ASSERT_EQ(-EINVAL, rbd_aio_readv(image, bad_iovs, 1, 0, comp));
  ASSERT_EQ(0, rbd_aio_readv(image, read_iovs,
                             sizeof(read_iovs) / sizeof(struct iovec),
                             1<<order, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(10, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);
  ASSERT_EQ("This1111 is a ", read_buffer);

  std::string linear_buffer(write_buffer.size(), '1');
  struct iovec linear_iovs[] = {
    {.iov_base = &linear_buffer[4], .iov_len = 4}
  };
  rbd_aio_create_completion(NULL, NULL, &comp);
  ASSERT_EQ(0, rbd_aio_readv(image, linear_iovs,
                             sizeof(linear_iovs) / sizeof(struct iovec),
                             1<<order, comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(comp));
  ASSERT_EQ(4, rbd_aio_get_return_value(comp));
  rbd_aio_release(comp);
  ASSERT_EQ("1111This111111", linear_buffer);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestEmptyDiscard)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 20 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_PASSED(aio_discard_test_data, image, 0, 1*1024*1024);
  ASSERT_PASSED(aio_discard_test_data, image, 0, 4*1024*1024);
  ASSERT_PASSED(aio_discard_test_data, image, 3*1024*1024, 1*1024*1024);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}


void simple_write_cb_pp(librbd::completion_t cb, void *arg)
{
  cout << "write completion cb called!" << std::endl;
}

void simple_read_cb_pp(librbd::completion_t cb, void *arg)
{
  cout << "read completion cb called!" << std::endl;
}

void aio_write_test_data(librbd::Image& image, const char *test_data,
			 off_t off, uint32_t iohint, bool *passed)
{
  ceph::bufferlist bl;
  bl.append(test_data, strlen(test_data));
  librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL, (librbd::callback_t) simple_write_cb_pp);
  printf("created completion\n");
  if (iohint)
    image.aio_write2(off, strlen(test_data), bl, comp, iohint);
  else
    image.aio_write(off, strlen(test_data), bl, comp);
  printf("started write\n");
  comp->wait_for_complete();
  int r = comp->get_return_value();
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished write\n");
  comp->release();
  *passed = true;
}

void aio_discard_test_data(librbd::Image& image, off_t off, size_t len, bool *passed)
{
  librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL, (librbd::callback_t) simple_write_cb_pp);
  image.aio_discard(off, len, comp);
  comp->wait_for_complete();
  int r = comp->get_return_value();
  ASSERT_EQ(0, r);
  comp->release();
  *passed = true;
}

void write_test_data(librbd::Image& image, const char *test_data, off_t off, uint32_t iohint, bool *passed)
{
  size_t written;
  size_t len = strlen(test_data);
  ceph::bufferlist bl;
  bl.append(test_data, len);
  if (iohint)
    written = image.write2(off, len, bl, iohint);
  else
    written = image.write(off, len, bl);
  printf("wrote: %u\n", (unsigned int) written);
  ASSERT_EQ(bl.length(), written);
  *passed = true;
}

void discard_test_data(librbd::Image& image, off_t off, size_t len, bool *passed)
{
  size_t written;
  written = image.discard(off, len);
  printf("discard: %u~%u\n", (unsigned)off, (unsigned)len);
  ASSERT_EQ(len, written);
  *passed = true;
}

void aio_read_test_data(librbd::Image& image, const char *expected, off_t off, size_t expected_len, uint32_t iohint, bool *passed)
{
  librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL, (librbd::callback_t) simple_read_cb_pp);
  ceph::bufferlist bl;
  printf("created completion\n");
  if (iohint)
    image.aio_read2(off, expected_len, bl, comp, iohint);
  else
    image.aio_read(off, expected_len, bl, comp);
  printf("started read\n");
  comp->wait_for_complete();
  int r = comp->get_return_value();
  printf("return value is: %d\n", r);
  ASSERT_EQ(TEST_IO_SIZE, r);
  ASSERT_EQ(0, memcmp(expected, bl.c_str(), TEST_IO_SIZE));
  printf("finished read\n");
  comp->release();
  *passed = true;
}

void read_test_data(librbd::Image& image, const char *expected, off_t off, size_t expected_len, uint32_t iohint, bool *passed)
{
  int read;
  size_t len = expected_len;
  ceph::bufferlist bl;
  if (iohint)
    read = image.read2(off, len, bl, iohint);
  else
    read = image.read(off, len, bl);
  ASSERT_TRUE(read >= 0);
  std::string bl_str(bl.c_str(), read);

  printf("read: %u\n", (unsigned int) read);
  int result = memcmp(bl_str.c_str(), expected, expected_len);
  if (result != 0) {
    printf("read: %s\nexpected: %s\n", bl_str.c_str(), expected);
    ASSERT_EQ(0, result);
  }
  *passed = true;
}

void aio_writesame_test_data(librbd::Image& image, const char *test_data, off_t off,
                             size_t len, size_t data_len, uint32_t iohint, bool *passed)
{
  ceph::bufferlist bl;
  bl.append(test_data, data_len);
  librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL, (librbd::callback_t) simple_write_cb_pp);
  printf("created completion\n");
  int r;
  r = image.aio_writesame(off, len, bl, comp, iohint);
  printf("started writesame\n");
  if (len % data_len) {
    ASSERT_EQ(-EINVAL, r);
    printf("expected fail, finished writesame\n");
    comp->release();
    *passed = true;
    return;
  }

  comp->wait_for_complete();
  r = comp->get_return_value();
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished writesame\n");
  comp->release();

  //verify data
  printf("to verify the data\n");
  int read;
  uint64_t left = len;
  while (left > 0) {
    ceph::bufferlist bl;
    read = image.read(off, data_len, bl);
    ASSERT_EQ(data_len, static_cast<size_t>(read));
    std::string bl_str(bl.c_str(), read);
    int result = memcmp(bl_str.c_str(), test_data, data_len);
    if (result !=0 ) {
      printf("read: %u ~ %u\n", (unsigned int) off, (unsigned int) read);
      printf("read: %s\nexpected: %s\n", bl_str.c_str(), test_data);
      ASSERT_EQ(0, result);
    }
    off += data_len;
    left -= data_len;
  }
  ASSERT_EQ(0U, left);
  printf("verified\n");

  *passed = true;
}

void writesame_test_data(librbd::Image& image, const char *test_data, off_t off,
                         ssize_t len, size_t data_len, uint32_t iohint,
                         bool *passed)
{
  ssize_t written;
  ceph::bufferlist bl;
  bl.append(test_data, data_len);
  written = image.writesame(off, len, bl, iohint);
  if (len % data_len) {
    ASSERT_EQ(-EINVAL, written);
    printf("expected fail, finished writesame\n");
    *passed = true;
    return;
  }
  ASSERT_EQ(len, written);
  printf("wrote: %u\n", (unsigned int) written);
  *passed = true;

  //verify data
  printf("to verify the data\n");
  int read;
  uint64_t left = len;
  while (left > 0) {
    ceph::bufferlist bl;
    read = image.read(off, data_len, bl);
    ASSERT_EQ(data_len, static_cast<size_t>(read));
    std::string bl_str(bl.c_str(), read);
    int result = memcmp(bl_str.c_str(), test_data, data_len);
    if (result !=0 ) {
      printf("read: %u ~ %u\n", (unsigned int) off, (unsigned int) read);
      printf("read: %s\nexpected: %s\n", bl_str.c_str(), test_data);
      ASSERT_EQ(0, result);
    }
    off += data_len;
    left -= data_len;
  }
  ASSERT_EQ(0U, left);
  printf("verified\n");

  *passed = true;
}

void aio_compare_and_write_test_data(librbd::Image& image, const char *cmp_data,
                                     const char *test_data, off_t off, ssize_t len,
                                     uint32_t iohint, bool *passed)
{
  ceph::bufferlist cmp_bl;
  cmp_bl.append(cmp_data, strlen(cmp_data));
  ceph::bufferlist test_bl;
  test_bl.append(test_data, strlen(test_data));
  librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL, (librbd::callback_t) simple_write_cb_pp);
  printf("created completion\n");

  uint64_t mismatch_offset;
  image.aio_compare_and_write(off, len, cmp_bl, test_bl, comp, &mismatch_offset, iohint);
  printf("started aio compare and write\n");
  comp->wait_for_complete();
  int r = comp->get_return_value();
  printf("return value is: %d\n", r);
  ASSERT_EQ(0, r);
  printf("finished aio compare and write\n");
  comp->release();
  *passed = true;
}

void compare_and_write_test_data(librbd::Image& image, const char *cmp_data, const char *test_data,
                                 off_t off, ssize_t len, uint64_t *mismatch_off, uint32_t iohint, bool *passed)
{
  size_t written;
  ceph::bufferlist cmp_bl;
  cmp_bl.append(cmp_data, strlen(cmp_data));
  ceph::bufferlist test_bl;
  test_bl.append(test_data, strlen(test_data));
  printf("start compare and write\n");
  written = image.compare_and_write(off, len, cmp_bl, test_bl, mismatch_off, iohint);
  printf("compare and  wrote: %d\n", (int) written);
  ASSERT_EQ(len, static_cast<ssize_t>(written));
  *passed = true;
}

TEST_F(TestLibRBD, TestIOPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  bool skip_discard = is_skip_partial_discard_enabled();

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    char test_data[TEST_IO_SIZE + 1];
    char zero_data[TEST_IO_SIZE + 1];
    int i;
    uint64_t mismatch_offset;

    for (i = 0; i < TEST_IO_SIZE; ++i) {
      test_data[i] = (char) (rand() % (126 - 33) + 33);
    }
    test_data[TEST_IO_SIZE] = '\0';
    memset(zero_data, 0, sizeof(zero_data));

    for (i = 0; i < 5; ++i)
      ASSERT_PASSED(write_test_data, image, test_data, strlen(test_data) * i, 0);

    for (i = 5; i < 10; ++i)
      ASSERT_PASSED(aio_write_test_data, image, test_data, strlen(test_data) * i, 0);

    for (i = 0; i < 5; ++i)
      ASSERT_PASSED(compare_and_write_test_data, image, test_data, test_data, TEST_IO_SIZE * i,
        TEST_IO_SIZE, &mismatch_offset, 0);

    for (i = 5; i < 10; ++i)
      ASSERT_PASSED(aio_compare_and_write_test_data, image, test_data, test_data, TEST_IO_SIZE * i,
        TEST_IO_SIZE, 0);

    for (i = 0; i < 5; ++i)
      ASSERT_PASSED(read_test_data, image, test_data, strlen(test_data) * i, TEST_IO_SIZE, 0);

    for (i = 5; i < 10; ++i)
      ASSERT_PASSED(aio_read_test_data, image, test_data, strlen(test_data) * i, TEST_IO_SIZE, 0);

    // discard 2nd, 4th sections.
    ASSERT_PASSED(discard_test_data, image, TEST_IO_SIZE, TEST_IO_SIZE);
    ASSERT_PASSED(aio_discard_test_data, image, TEST_IO_SIZE*3, TEST_IO_SIZE);

    ASSERT_PASSED(read_test_data, image, test_data,  0, TEST_IO_SIZE, 0);
    ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		  TEST_IO_SIZE, TEST_IO_SIZE, 0);
    ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*2, TEST_IO_SIZE, 0);
    ASSERT_PASSED(read_test_data, image, skip_discard ? test_data : zero_data,
		  TEST_IO_SIZE*3, TEST_IO_SIZE, 0);
    ASSERT_PASSED(read_test_data, image, test_data,  TEST_IO_SIZE*4, TEST_IO_SIZE, 0);

    for (i = 0; i < 15; ++i) {
      if (i % 3 == 2) {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
      } else if (i % 3 == 1) {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      } else {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      }
    }
    for (i = 0; i < 15; ++i) {
      if (i % 3 == 2) {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, 0);
      } else if (i % 3 == 1) {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE + i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      } else {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i, TEST_IO_SIZE * i * 32, TEST_IO_SIZE, 0);
      }
    }

    ASSERT_PASSED(validate_object_map, image);
  }

  ioctx.close();
}

TEST_F(TestLibRBD, TestIOPPWithIOHint)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    char test_data[TEST_IO_SIZE + 1];
    char zero_data[TEST_IO_SIZE + 1];
    test_data[TEST_IO_SIZE] = '\0';
    int i;

    for (i = 0; i < TEST_IO_SIZE; ++i) {
      test_data[i] = (char) (rand() % (126 - 33) + 33);
    }
    memset(zero_data, 0, sizeof(zero_data));

    for (i = 0; i < 5; ++i)
      ASSERT_PASSED(write_test_data, image, test_data, strlen(test_data) * i,
		    LIBRADOS_OP_FLAG_FADVISE_NOCACHE);

    for (i = 5; i < 10; ++i)
      ASSERT_PASSED(aio_write_test_data, image, test_data, strlen(test_data) * i,
		    LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

    ASSERT_PASSED(read_test_data, image, test_data, strlen(test_data),
		  TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_RANDOM);

    for (i = 5; i < 10; ++i)
      ASSERT_PASSED(aio_read_test_data, image, test_data, strlen(test_data) * i,
		    TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL|LIBRADOS_OP_FLAG_FADVISE_DONTNEED);

    for (i = 0; i < 15; ++i) {
      if (i % 3 == 2) {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      } else if (i % 3 == 1) {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE + i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE + i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      } else {
        ASSERT_PASSED(writesame_test_data, image, test_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
        ASSERT_PASSED(writesame_test_data, image, zero_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_NOCACHE);
      }
    }
    for (i = 0; i < 15; ++i) {
      if (i % 3 == 2) {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32 + i, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      } else if (i % 3 == 1) {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE + i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE + i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      } else {
        ASSERT_PASSED(aio_writesame_test_data, image, test_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
        ASSERT_PASSED(aio_writesame_test_data, image, zero_data, TEST_IO_SIZE * i,
                      TEST_IO_SIZE * i * 32, TEST_IO_SIZE, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
      }
    }

    ASSERT_PASSED(validate_object_map, image);
  }

  ioctx.close();
}



TEST_F(TestLibRBD, TestIOToSnapshot)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t isize = 2 << 20;
  
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), isize, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  int i, r;
  rbd_image_t image_at_snap;
  char orig_data[TEST_IO_TO_SNAP_SIZE + 1];
  char test_data[TEST_IO_TO_SNAP_SIZE + 1];

  for (i = 0; i < TEST_IO_TO_SNAP_SIZE; ++i)
    test_data[i] = (char) (i + 48);
  test_data[TEST_IO_TO_SNAP_SIZE] = '\0';
  orig_data[TEST_IO_TO_SNAP_SIZE] = '\0';

  r = rbd_read(image, 0, TEST_IO_TO_SNAP_SIZE, orig_data);
  ASSERT_EQ(r, TEST_IO_TO_SNAP_SIZE);

  ASSERT_EQ(0, test_ls_snaps(image, 0));
  ASSERT_EQ(0, rbd_snap_create(image, "orig"));
  ASSERT_EQ(1, test_ls_snaps(image, 1, "orig", isize));
  ASSERT_PASSED(read_test_data, image, orig_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  printf("write test data!\n");
  ASSERT_PASSED(write_test_data, image, test_data, 0, TEST_IO_TO_SNAP_SIZE, 0);
  ASSERT_EQ(0, rbd_snap_create(image, "written"));
  ASSERT_EQ(2, test_ls_snaps(image, 2, "orig", isize, "written", isize));

  ASSERT_PASSED(read_test_data, image, test_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  rbd_snap_set(image, "orig");
  ASSERT_PASSED(read_test_data, image, orig_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  rbd_snap_set(image, "written");
  ASSERT_PASSED(read_test_data, image, test_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  rbd_snap_set(image, "orig");

  r = rbd_write(image, 0, TEST_IO_TO_SNAP_SIZE, test_data);
  printf("write to snapshot returned %d\n", r);
  ASSERT_LT(r, 0);
  cout << strerror(-r) << std::endl;

  ASSERT_PASSED(read_test_data, image, orig_data, 0, TEST_IO_TO_SNAP_SIZE, 0);
  rbd_snap_set(image, "written");
  ASSERT_PASSED(read_test_data, image, test_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  r = rbd_snap_rollback(image, "orig");
  ASSERT_EQ(r, -EROFS);

  r = rbd_snap_set(image, NULL);
  ASSERT_EQ(r, 0);
  r = rbd_snap_rollback(image, "orig");
  ASSERT_EQ(r, 0);

  ASSERT_PASSED(write_test_data, image, test_data, 0, TEST_IO_TO_SNAP_SIZE, 0);

  rbd_flush(image);

  printf("opening testimg@orig\n");
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image_at_snap, "orig"));
  ASSERT_PASSED(read_test_data, image_at_snap, orig_data, 0, TEST_IO_TO_SNAP_SIZE, 0);
  r = rbd_write(image_at_snap, 0, TEST_IO_TO_SNAP_SIZE, test_data);
  printf("write to snapshot returned %d\n", r);
  ASSERT_LT(r, 0);
  cout << strerror(-r) << std::endl;
  ASSERT_EQ(0, rbd_close(image_at_snap));

  ASSERT_EQ(2, test_ls_snaps(image, 2, "orig", isize, "written", isize));
  ASSERT_EQ(0, rbd_snap_remove(image, "written"));
  ASSERT_EQ(1, test_ls_snaps(image, 1, "orig", isize));
  ASSERT_EQ(0, rbd_snap_remove(image, "orig"));
  ASSERT_EQ(0, test_ls_snaps(image, 0));

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestClone)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  rados_ioctx_t ioctx;
  rbd_image_info_t pinfo, cinfo;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  bool old_format;
  uint64_t features;
  rbd_image_t parent, child;
  int order = 0;

  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  std::string parent_name = get_temp_image_name();
  std::string child_name = get_temp_image_name();

  // make a parent to clone from
  ASSERT_EQ(0, create_image_full(ioctx, parent_name.c_str(), 4<<20, &order,
				 false, features));
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, NULL));
  printf("made parent image \"parent\"\n");

  char *data = (char *)"testdata";
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(parent, 0, strlen(data), data));

  // can't clone a non-snapshot, expect failure
  EXPECT_NE(0, clone_image(ioctx, parent, parent_name.c_str(), NULL, ioctx,
                           child_name.c_str(), features, &order));

  // verify that there is no parent info on "parent"
  ASSERT_EQ(-ENOENT, rbd_get_parent_info(parent, NULL, 0, NULL, 0, NULL, 0));
  printf("parent has no parent info\n");

  // create 70 metadatas to verify we can clone all key/value pairs
  std::string key;
  std::string val;
  size_t sum_key_len = 0;
  size_t sum_value_len = 0;
  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_set(parent, key.c_str(), val.c_str()));

    sum_key_len += (key.size() + 1);
    sum_value_len += (val.size() + 1);
  }

  char keys[1024];
  char vals[1024];
  size_t keys_len = sizeof(keys);
  size_t vals_len = sizeof(vals);

  char value[1024];
  size_t value_len = sizeof(value);

  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, rbd_snap_create(parent, "parent_snap"));
  printf("made snapshot \"parent@parent_snap\"\n");
  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, "parent_snap"));

  ASSERT_EQ(-EINVAL, clone_image(ioctx, parent, parent_name.c_str(), "parent_snap",
                                 ioctx, child_name.c_str(), features, &order));

  // unprotected image should fail unprotect
  ASSERT_EQ(-EINVAL, rbd_snap_unprotect(parent, "parent_snap"));
  printf("can't unprotect an unprotected snap\n");

  ASSERT_EQ(0, rbd_snap_protect(parent, "parent_snap"));
  // protecting again should fail
  ASSERT_EQ(-EBUSY, rbd_snap_protect(parent, "parent_snap"));
  printf("can't protect a protected snap\n");

  // This clone and open should work
  ASSERT_EQ(0, clone_image(ioctx, parent, parent_name.c_str(), "parent_snap",
                           ioctx, child_name.c_str(), features, &order));
  ASSERT_EQ(0, rbd_open(ioctx, child_name.c_str(), &child, NULL));
  printf("made and opened clone \"child\"\n");

  // check read
  ASSERT_PASSED(read_test_data, child, data, 0, strlen(data), 0);

  // check write
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(child, 20, strlen(data), data));
  ASSERT_PASSED(read_test_data, child, data, 20, strlen(data), 0);
  ASSERT_PASSED(read_test_data, child, data, 0, strlen(data), 0);

  // check attributes
  ASSERT_EQ(0, rbd_stat(parent, &pinfo, sizeof(pinfo)));
  ASSERT_EQ(0, rbd_stat(child, &cinfo, sizeof(cinfo)));
  EXPECT_EQ(cinfo.size, pinfo.size);
  uint64_t overlap;
  rbd_get_overlap(child, &overlap);
  EXPECT_EQ(overlap, pinfo.size);
  EXPECT_EQ(cinfo.obj_size, pinfo.obj_size);
  EXPECT_EQ(cinfo.order, pinfo.order);
  printf("sizes and overlaps are good between parent and child\n");

  // check key/value pairs in child image
  ASSERT_EQ(0, rbd_metadata_list(child, "", 70, keys, &keys_len, vals,
                                &vals_len));
  ASSERT_EQ(sum_key_len, keys_len);
  ASSERT_EQ(sum_value_len, vals_len);

  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_get(child, key.c_str(), value, &value_len));
    ASSERT_STREQ(val.c_str(), value);

    value_len = sizeof(value);
  }
  printf("child image successfully cloned all image-meta pairs\n");

  // sizing down child results in changing overlap and size, not parent size
  ASSERT_EQ(0, rbd_resize(child, 2UL<<20));
  ASSERT_EQ(0, rbd_stat(child, &cinfo, sizeof(cinfo)));
  rbd_get_overlap(child, &overlap);
  ASSERT_EQ(overlap, 2UL<<20);
  ASSERT_EQ(cinfo.size, 2UL<<20);
  ASSERT_EQ(0, rbd_resize(child, 4UL<<20));
  ASSERT_EQ(0, rbd_stat(child, &cinfo, sizeof(cinfo)));
  rbd_get_overlap(child, &overlap);
  ASSERT_EQ(overlap, 2UL<<20);
  ASSERT_EQ(cinfo.size, 4UL<<20);
  printf("sized down clone, changed overlap\n");

  // sizing back up doesn't change that
  ASSERT_EQ(0, rbd_resize(child, 5UL<<20));
  ASSERT_EQ(0, rbd_stat(child, &cinfo, sizeof(cinfo)));
  rbd_get_overlap(child, &overlap);
  ASSERT_EQ(overlap, 2UL<<20);
  ASSERT_EQ(cinfo.size, 5UL<<20);
  ASSERT_EQ(0, rbd_stat(parent, &pinfo, sizeof(pinfo)));
  printf("parent info: size %lld obj_size %lld parent_pool %lld\n",
	 (unsigned long long)pinfo.size, (unsigned long long)pinfo.obj_size,
	 (unsigned long long)pinfo.parent_pool);
  ASSERT_EQ(pinfo.size, 4UL<<20);
  printf("sized up clone, changed size but not overlap or parent's size\n");
  
  ASSERT_PASSED(validate_object_map, child);
  ASSERT_EQ(0, rbd_close(child));

  ASSERT_PASSED(validate_object_map, parent);
  ASSERT_EQ(-EBUSY, rbd_snap_remove(parent, "parent_snap"));
  printf("can't remove parent while child still exists\n");
  ASSERT_EQ(0, rbd_remove(ioctx, child_name.c_str()));
  ASSERT_EQ(-EBUSY, rbd_snap_remove(parent, "parent_snap"));
  printf("can't remove parent while still protected\n");
  ASSERT_EQ(0, rbd_snap_unprotect(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_remove(parent, "parent_snap"));
  printf("removed parent snap after unprotecting\n");

  ASSERT_EQ(0, rbd_close(parent));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestClone2)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  bool old_format;
  uint64_t features;
  rbd_image_t parent, child;
  int order = 0;

  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  std::string parent_name = get_temp_image_name();
  std::string child_name = get_temp_image_name();

  // make a parent to clone from
  ASSERT_EQ(0, create_image_full(ioctx, parent_name.c_str(), 4<<20, &order,
				 false, features));
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, NULL));
  printf("made parent image \"parent\"\n");

  char *data = (char *)"testdata";
  char *childata = (char *)"childata";
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(parent, 0, strlen(data), data));
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(parent, 12, strlen(data), data));

  // can't clone a non-snapshot, expect failure
  EXPECT_NE(0, clone_image(ioctx, parent, parent_name.c_str(), NULL, ioctx,
                           child_name.c_str(), features, &order));

  // verify that there is no parent info on "parent"
  ASSERT_EQ(-ENOENT, rbd_get_parent_info(parent, NULL, 0, NULL, 0, NULL, 0));
  printf("parent has no parent info\n");

  // create 70 metadatas to verify we can clone all key/value pairs
  std::string key;
  std::string val;
  size_t sum_key_len = 0;
  size_t sum_value_len = 0;
  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_set(parent, key.c_str(), val.c_str()));

    sum_key_len += (key.size() + 1);
    sum_value_len += (val.size() + 1);
  }

  char keys[1024];
  char vals[1024];
  size_t keys_len = sizeof(keys);
  size_t vals_len = sizeof(vals);

  char value[1024];
  size_t value_len = sizeof(value);

  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, rbd_snap_create(parent, "parent_snap"));
  printf("made snapshot \"parent@parent_snap\"\n");
  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, "parent_snap"));

  ASSERT_EQ(-EINVAL, clone_image(ioctx, parent, parent_name.c_str(), "parent_snap",
                                 ioctx, child_name.c_str(), features, &order));

  // unprotected image should fail unprotect
  ASSERT_EQ(-EINVAL, rbd_snap_unprotect(parent, "parent_snap"));
  printf("can't unprotect an unprotected snap\n");

  ASSERT_EQ(0, rbd_snap_protect(parent, "parent_snap"));
  // protecting again should fail
  ASSERT_EQ(-EBUSY, rbd_snap_protect(parent, "parent_snap"));
  printf("can't protect a protected snap\n");

  // This clone and open should work
  ASSERT_EQ(0, clone_image(ioctx, parent, parent_name.c_str(), "parent_snap",
                           ioctx, child_name.c_str(), features, &order));
  ASSERT_EQ(0, rbd_open(ioctx, child_name.c_str(), &child, NULL));
  printf("made and opened clone \"child\"\n");

  // check key/value pairs in child image
  ASSERT_EQ(0, rbd_metadata_list(child, "", 70, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(sum_key_len, keys_len);
  ASSERT_EQ(sum_value_len, vals_len);

  for (int i = 1; i <= 70; i++) {
    key = "key" + stringify(i);
    val = "value" + stringify(i);
    ASSERT_EQ(0, rbd_metadata_get(child, key.c_str(), value, &value_len));
    ASSERT_STREQ(val.c_str(), value);

    value_len = sizeof(value);
  }
  printf("child image successfully cloned all image-meta pairs\n");

  // write something in
  ASSERT_EQ((ssize_t)strlen(childata), rbd_write(child, 20, strlen(childata), childata));

  char test[strlen(data) * 2];
  ASSERT_EQ((ssize_t)strlen(data), rbd_read(child, 20, strlen(data), test));
  ASSERT_EQ(0, memcmp(test, childata, strlen(childata)));

  // overlap
  ASSERT_EQ((ssize_t)sizeof(test), rbd_read(child, 20 - strlen(data), sizeof(test), test));
  ASSERT_EQ(0, memcmp(test, data, strlen(data)));
  ASSERT_EQ(0, memcmp(test + strlen(data), childata, strlen(childata)));

  // all parent
  ASSERT_EQ((ssize_t)sizeof(test), rbd_read(child, 0, sizeof(test), test));
  ASSERT_EQ(0, memcmp(test, data, strlen(data)));

  ASSERT_PASSED(validate_object_map, child);
  ASSERT_PASSED(validate_object_map, parent);

  ASSERT_EQ(0, rbd_close(child));
  ASSERT_EQ(0, rbd_close(parent));
  rados_ioctx_destroy(ioctx);
}

static void test_list_children(rbd_image_t image, ssize_t num_expected, ...)
{
  va_list ap;
  va_start(ap, num_expected);
  size_t pools_len = 100;
  size_t children_len = 100;
  char *pools = NULL;
  char *children = NULL;
  ssize_t num_children;

  do {
    free(pools);
    free(children);
    pools = (char *) malloc(pools_len);
    children = (char *) malloc(children_len);
    num_children = rbd_list_children(image, pools, &pools_len,
				     children, &children_len);
  } while (num_children == -ERANGE);

  ASSERT_EQ(num_expected, num_children);
  for (ssize_t i = num_expected; i > 0; --i) {
    char *expected_pool = va_arg(ap, char *);
    char *expected_image = va_arg(ap, char *);
    char *pool = pools;
    char *image = children;
    bool found = 0;
    printf("\ntrying to find %s/%s\n", expected_pool, expected_image);
    for (ssize_t j = 0; j < num_children; ++j) {
      printf("checking %s/%s\n", pool, image);
      if (strcmp(expected_pool, pool) == 0 &&
	  strcmp(expected_image, image) == 0) {
	printf("found child %s/%s\n\n", pool, image);
	found = 1;
	break;
      }
      pool += strlen(pool) + 1;
      image += strlen(image) + 1;
      if (j == num_children - 1) {
	ASSERT_EQ(pool - pools - 1, (ssize_t) pools_len);
	ASSERT_EQ(image - children - 1, (ssize_t) children_len);
      }
    }
    ASSERT_TRUE(found);
  }
  va_end(ap);

  if (pools)
    free(pools);
  if (children)
    free(children);
}

TEST_F(TestLibRBD, ListChildren)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  rados_ioctx_t ioctx1, ioctx2;
  string pool_name1 = create_pool(true);
  string pool_name2 = create_pool(true);
  ASSERT_NE("", pool_name2);

  rados_ioctx_create(_cluster, pool_name1.c_str(), &ioctx1);
  rados_ioctx_create(_cluster, pool_name2.c_str(), &ioctx2);

  bool old_format;
  uint64_t features;
  rbd_image_t parent;
  int order = 0;

  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  std::string parent_name = get_temp_image_name();
  std::string child_name1 = get_temp_image_name();
  std::string child_name2 = get_temp_image_name();
  std::string child_name3 = get_temp_image_name();
  std::string child_name4 = get_temp_image_name();

  // make a parent to clone from
  ASSERT_EQ(0, create_image_full(ioctx1, parent_name.c_str(), 4<<20, &order,
				 false, features));
  ASSERT_EQ(0, rbd_open(ioctx1, parent_name.c_str(), &parent, NULL));
  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, rbd_snap_create(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_set(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_protect(parent, "parent_snap"));

  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_open(ioctx1, parent_name.c_str(), &parent, "parent_snap"));

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name1.c_str(), features, &order));
  test_list_children(parent, 1, pool_name2.c_str(), child_name1.c_str());

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx1, child_name2.c_str(), features, &order));
  test_list_children(parent, 2, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str());

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name3.c_str(), features, &order));
  test_list_children(parent, 3, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str());

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name4.c_str(), features, &order));
  test_list_children(parent, 4, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name1.c_str()));
  test_list_children(parent, 3,
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name3.c_str()));
  test_list_children(parent, 2,
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name4.c_str()));
  test_list_children(parent, 1,
		     pool_name1.c_str(), child_name2.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx1, child_name2.c_str()));
  test_list_children(parent, 0);

  ASSERT_EQ(0, rbd_snap_unprotect(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_remove(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_remove(ioctx1, parent_name.c_str()));
  rados_ioctx_destroy(ioctx1);
  rados_ioctx_destroy(ioctx2);
}

TEST_F(TestLibRBD, ListChildrenTiered)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  string pool_name1 = create_pool(true);
  string pool_name2 = create_pool(true);
  string pool_name3 = create_pool(true);
  ASSERT_NE("", pool_name1);
  ASSERT_NE("", pool_name2);
  ASSERT_NE("", pool_name3);

  std::string cmdstr = "{\"prefix\": \"osd tier add\", \"pool\": \"" +
     pool_name1 + "\", \"tierpool\":\"" + pool_name3 + "\", \"force_nonempty\":\"\"}";
  char *cmd[1];
  cmd[0] = (char *)cmdstr.c_str();
  ASSERT_EQ(0, rados_mon_command(_cluster, (const char **)cmd, 1, "", 0, NULL, 0, NULL, 0));

  cmdstr = "{\"prefix\": \"osd tier cache-mode\", \"pool\": \"" +
     pool_name3 + "\", \"mode\":\"writeback\"}";
  cmd[0] = (char *)cmdstr.c_str();
  ASSERT_EQ(0, rados_mon_command(_cluster, (const char **)cmd, 1, "", 0, NULL, 0, NULL, 0));

  cmdstr = "{\"prefix\": \"osd tier set-overlay\", \"pool\": \"" +
     pool_name1 + "\", \"overlaypool\":\"" + pool_name3 + "\"}";
  cmd[0] = (char *)cmdstr.c_str();
  ASSERT_EQ(0, rados_mon_command(_cluster, (const char **)cmd, 1, "", 0, NULL, 0, NULL, 0));

  EXPECT_EQ(0, rados_wait_for_latest_osdmap(_cluster));

  string parent_name = get_temp_image_name();
  string child_name1 = get_temp_image_name();
  string child_name2 = get_temp_image_name();
  string child_name3 = get_temp_image_name();
  string child_name4 = get_temp_image_name();

  rados_ioctx_t ioctx1, ioctx2;
  rados_ioctx_create(_cluster, pool_name1.c_str(), &ioctx1);
  rados_ioctx_create(_cluster, pool_name2.c_str(), &ioctx2);

  bool old_format;
  uint64_t features;
  rbd_image_t parent;
  int order = 0;

  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  // make a parent to clone from
  ASSERT_EQ(0, create_image_full(ioctx1, parent_name.c_str(), 4<<20, &order,
				 false, features));
  ASSERT_EQ(0, rbd_open(ioctx1, parent_name.c_str(), &parent, NULL));
  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, rbd_snap_create(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_set(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_protect(parent, "parent_snap"));

  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_open(ioctx1, parent_name.c_str(), &parent, "parent_snap"));

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name1.c_str(), features, &order));
  test_list_children(parent, 1, pool_name2.c_str(), child_name1.c_str());

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx1, child_name2.c_str(), features, &order));
  test_list_children(parent, 2, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str());

  // read from the cache to populate it
  rbd_image_t tier_image;
  ASSERT_EQ(0, rbd_open(ioctx1, child_name2.c_str(), &tier_image, NULL));
  size_t len = 4 * 1024 * 1024;
  char* buf = (char*)malloc(len);
  ssize_t size = rbd_read(tier_image, 0, len, buf);
  ASSERT_GT(size, 0);
  free(buf);
  ASSERT_EQ(0, rbd_close(tier_image));

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name3.c_str(), features, &order));
  test_list_children(parent, 3, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str());

  ASSERT_EQ(0, clone_image(ioctx1, parent, parent_name.c_str(), "parent_snap",
                           ioctx2, child_name4.c_str(), features, &order));
  test_list_children(parent, 4, pool_name2.c_str(), child_name1.c_str(),
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name1.c_str()));
  test_list_children(parent, 3,
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name3.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name3.c_str()));
  test_list_children(parent, 2,
		     pool_name1.c_str(), child_name2.c_str(),
		     pool_name2.c_str(), child_name4.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx2, child_name4.c_str()));
  test_list_children(parent, 1,
		     pool_name1.c_str(), child_name2.c_str());

  ASSERT_EQ(0, rbd_remove(ioctx1, child_name2.c_str()));
  test_list_children(parent, 0);

  ASSERT_EQ(0, rbd_snap_unprotect(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_snap_remove(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_remove(ioctx1, parent_name.c_str()));
  rados_ioctx_destroy(ioctx1);
  rados_ioctx_destroy(ioctx2);
  cmdstr = "{\"prefix\": \"osd tier remove-overlay\", \"pool\": \"" +
     pool_name1 + "\"}";
  cmd[0] = (char *)cmdstr.c_str();
  ASSERT_EQ(0, rados_mon_command(_cluster, (const char **)cmd, 1, "", 0, NULL, 0, NULL, 0));
  cmdstr = "{\"prefix\": \"osd tier remove\", \"pool\": \"" +
     pool_name1 + "\", \"tierpool\":\"" + pool_name3 + "\"}";
  cmd[0] = (char *)cmdstr.c_str();
  ASSERT_EQ(0, rados_mon_command(_cluster, (const char **)cmd, 1, "", 0, NULL, 0, NULL, 0));
}

TEST_F(TestLibRBD, LockingPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    std::string cookie1 = "foo";
    std::string cookie2 = "bar";

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    // no lockers initially
    std::list<librbd::locker_t> lockers;
    std::string tag;
    bool exclusive;
    ASSERT_EQ(0, image.list_lockers(&lockers, &exclusive, &tag));
    ASSERT_EQ(0u, lockers.size());
    ASSERT_EQ("", tag);

    // exclusive lock is exclusive
    ASSERT_EQ(0, image.lock_exclusive(cookie1));
    ASSERT_EQ(-EEXIST, image.lock_exclusive(cookie1));
    ASSERT_EQ(-EBUSY, image.lock_exclusive(""));
    ASSERT_EQ(-EEXIST, image.lock_shared(cookie1, ""));
    ASSERT_EQ(-EBUSY, image.lock_shared(cookie1, "test"));
    ASSERT_EQ(-EBUSY, image.lock_shared("", "test"));
    ASSERT_EQ(-EBUSY, image.lock_shared("", ""));

    // list exclusive
    ASSERT_EQ(0, image.list_lockers(&lockers, &exclusive, &tag));
    ASSERT_TRUE(exclusive);
    ASSERT_EQ("", tag);
    ASSERT_EQ(1u, lockers.size());
    ASSERT_EQ(cookie1, lockers.front().cookie);

    // unlock
    ASSERT_EQ(-ENOENT, image.unlock(""));
    ASSERT_EQ(-ENOENT, image.unlock(cookie2));
    ASSERT_EQ(0, image.unlock(cookie1));
    ASSERT_EQ(-ENOENT, image.unlock(cookie1));
    ASSERT_EQ(0, image.list_lockers(&lockers, &exclusive, &tag));
    ASSERT_EQ(0u, lockers.size());

    ASSERT_EQ(0, image.lock_shared(cookie1, ""));
    ASSERT_EQ(-EEXIST, image.lock_shared(cookie1, ""));
    ASSERT_EQ(0, image.lock_shared(cookie2, ""));
    ASSERT_EQ(-EEXIST, image.lock_shared(cookie2, ""));
    ASSERT_EQ(-EEXIST, image.lock_exclusive(cookie1));
    ASSERT_EQ(-EEXIST, image.lock_exclusive(cookie2));
    ASSERT_EQ(-EBUSY, image.lock_exclusive(""));
    ASSERT_EQ(-EBUSY, image.lock_exclusive("test"));

    // list shared
    ASSERT_EQ(0, image.list_lockers(&lockers, &exclusive, &tag));
    ASSERT_EQ(2u, lockers.size());
  }

  ioctx.close();
}

TEST_F(TestLibRBD, FlushAio)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  size_t num_aios = 256;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char test_data[TEST_IO_SIZE + 1];
  size_t i;
  for (i = 0; i < TEST_IO_SIZE; ++i) {
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  }

  rbd_completion_t write_comps[num_aios];
  for (i = 0; i < num_aios; ++i) {
    ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &write_comps[i]));
    uint64_t offset = rand() % (size - TEST_IO_SIZE);
    ASSERT_EQ(0, rbd_aio_write(image, offset, TEST_IO_SIZE, test_data,
			       write_comps[i]));
  }

  rbd_completion_t flush_comp;
  ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &flush_comp));
  ASSERT_EQ(0, rbd_aio_flush(image, flush_comp));
  ASSERT_EQ(0, rbd_aio_wait_for_complete(flush_comp));
  ASSERT_EQ(1, rbd_aio_is_complete(flush_comp));
  rbd_aio_release(flush_comp);

  for (i = 0; i < num_aios; ++i) {
    ASSERT_EQ(1, rbd_aio_is_complete(write_comps[i]));
    rbd_aio_release(write_comps[i]);
  }

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));
  ASSERT_EQ(0, rbd_remove(ioctx, name.c_str()));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, FlushAioPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    const size_t num_aios = 256;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    char test_data[TEST_IO_SIZE + 1];
    size_t i;
    for (i = 0; i < TEST_IO_SIZE; ++i) {
      test_data[i] = (char) (rand() % (126 - 33) + 33);
    }
    test_data[TEST_IO_SIZE] = '\0';

    librbd::RBD::AioCompletion *write_comps[num_aios];
    ceph::bufferlist bls[num_aios];
    for (i = 0; i < num_aios; ++i) {
      bls[i].append(test_data, strlen(test_data));
      write_comps[i] = new librbd::RBD::AioCompletion(NULL, NULL);
      uint64_t offset = rand() % (size - TEST_IO_SIZE);
      ASSERT_EQ(0, image.aio_write(offset, TEST_IO_SIZE, bls[i],
				   write_comps[i]));
    }

    librbd::RBD::AioCompletion *flush_comp =
      new librbd::RBD::AioCompletion(NULL, NULL);
    ASSERT_EQ(0, image.aio_flush(flush_comp));
    ASSERT_EQ(0, flush_comp->wait_for_complete());
    ASSERT_EQ(1, flush_comp->is_complete());
    flush_comp->release();

    for (i = 0; i < num_aios; ++i) {
      librbd::RBD::AioCompletion *comp = write_comps[i];
      ASSERT_EQ(1, comp->is_complete());
      comp->release();
    }
    ASSERT_PASSED(validate_object_map, image);
  }

  ioctx.close();
}


int iterate_cb(uint64_t off, size_t len, int exists, void *arg)
{
  //cout << "iterate_cb " << off << "~" << len << std::endl;
  interval_set<uint64_t> *diff = static_cast<interval_set<uint64_t> *>(arg);
  diff->insert(off, len);
  return 0;
}

static int iterate_error_cb(uint64_t off, size_t len, int exists, void *arg)
{
  return -EINVAL;
}

void scribble(librbd::Image& image, int n, int max, bool skip_discard,
              interval_set<uint64_t> *exists,
              interval_set<uint64_t> *what)
{
  uint64_t size;
  image.size(&size);
  interval_set<uint64_t> exists_at_start = *exists;

  for (int i=0; i<n; i++) {
    uint64_t off = rand() % (size - max + 1);
    uint64_t len = 1 + rand() % max;
    if (!skip_discard && rand() % 4 == 0) {
      ASSERT_EQ((int)len, image.discard(off, len));
      interval_set<uint64_t> w;
      w.insert(off, len);

      // the zeroed bit no longer exists...
      w.intersection_of(*exists);
      exists->subtract(w);

      // the bits we discarded are no long written...
      interval_set<uint64_t> w2 = w;
      w2.intersection_of(*what);
      what->subtract(w2);

      // except for the extents that existed at the start that we overwrote.
      interval_set<uint64_t> w3;
      w3.insert(off, len);
      w3.intersection_of(exists_at_start);
      what->union_of(w3);

    } else {
      bufferlist bl;
      bl.append(buffer::create(len));
      bl.zero();
      ASSERT_EQ((int)len, image.write(off, len, bl));
      interval_set<uint64_t> w;
      w.insert(off, len);
      what->union_of(w);
      exists->union_of(w);
    }
  }
}

interval_set<uint64_t> round_diff_interval(const interval_set<uint64_t>& diff,
                                           uint64_t object_size)
{
  if (object_size == 0) {
    return diff;
  }

  interval_set<uint64_t> rounded_diff;
  for (interval_set<uint64_t>::const_iterator it = diff.begin();
       it != diff.end(); ++it) {
    uint64_t off = it.get_start();
    uint64_t len = it.get_len();
    off -= off % object_size;
    len += (object_size - (len % object_size));
    interval_set<uint64_t> interval;
    interval.insert(off, len);
    rounded_diff.union_of(interval);
  }
  return rounded_diff;
}

template <typename T>
class DiffIterateTest : public TestLibRBD {
public:
  static const uint8_t whole_object = T::whole_object;
};

template <bool _whole_object>
class DiffIterateParams {
public:
  static const uint8_t whole_object = _whole_object;
};

typedef ::testing::Types<DiffIterateParams<false>,
                         DiffIterateParams<true> > DiffIterateTypes;
TYPED_TEST_CASE(DiffIterateTest, DiffIterateTypes);

TYPED_TEST(DiffIterateTest, DiffIterate)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  bool skip_discard = this->is_skip_partial_discard_enabled();

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = this->get_temp_image_name();
    uint64_t size = 20 << 20;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    uint64_t object_size = 0;
    if (this->whole_object) {
      object_size = 1 << order;
    }

    interval_set<uint64_t> exists;
    interval_set<uint64_t> one, two;
    scribble(image, 10, 102400, skip_discard, &exists, &one);
    cout << " wrote " << one << std::endl;
    ASSERT_EQ(0, image.snap_create("one"));
    scribble(image, 10, 102400, skip_discard, &exists, &two);

    two = round_diff_interval(two, object_size);
    cout << " wrote " << two << std::endl;

    interval_set<uint64_t> diff;
    ASSERT_EQ(0, image.diff_iterate2("one", 0, size, true, this->whole_object,
                                     iterate_cb, (void *)&diff));
    cout << " diff was " << diff << std::endl;
    if (!two.subset_of(diff)) {
      interval_set<uint64_t> i;
      i.intersection_of(two, diff);
      interval_set<uint64_t> l = two;
      l.subtract(i);
      cout << " ... two - (two*diff) = " << l << std::endl;
    }
    ASSERT_TRUE(two.subset_of(diff));
  }
  ioctx.close();
}

struct diff_extent {
  diff_extent(uint64_t _offset, uint64_t _length, bool _exists,
              uint64_t object_size) :
    offset(_offset), length(_length), exists(_exists)
  {
    if (object_size != 0) {
      offset -= offset % object_size;
      length = object_size;
    }
  }
  uint64_t offset;
  uint64_t length;
  bool exists;
  bool operator==(const diff_extent& o) const {
    return offset == o.offset && length == o.length && exists == o.exists;
  }
};

ostream& operator<<(ostream & o, const diff_extent& e) {
  return o << '(' << e.offset << '~' << e.length << ' ' << (e.exists ? "true" : "false") << ')';
}

int vector_iterate_cb(uint64_t off, size_t len, int exists, void *arg)
{
  cout << "iterate_cb " << off << "~" << len << std::endl;
  vector<diff_extent> *diff = static_cast<vector<diff_extent> *>(arg);
  diff->push_back(diff_extent(off, len, exists, 0));
  return 0;
}

TYPED_TEST(DiffIterateTest, DiffIterateDiscard)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = this->get_temp_image_name();
  uint64_t size = 20 << 20;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint64_t object_size = 0;
  if (this->whole_object) {
    object_size = 1 << order;
  }
  vector<diff_extent> extents;
  ceph::bufferlist bl;

  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(0u, extents.size());

  char data[256];
  memset(data, 1, sizeof(data));
  bl.append(data, 256);
  ASSERT_EQ(256, image.write(0, 256, bl));
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(1u, extents.size());
  ASSERT_EQ(diff_extent(0, 256, true, object_size), extents[0]);

  int obj_ofs = 256;
  ASSERT_EQ(1 << order, image.discard(0, 1 << order));

  extents.clear();
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(0u, extents.size());

  ASSERT_EQ(0, image.snap_create("snap1"));
  ASSERT_EQ(256, image.write(0, 256, bl));
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(1u, extents.size());
  ASSERT_EQ(diff_extent(0, 256, true, object_size), extents[0]);
  ASSERT_EQ(0, image.snap_create("snap2"));

  ASSERT_EQ(obj_ofs, image.discard(0, obj_ofs));

  extents.clear();
  ASSERT_EQ(0, image.snap_set("snap2"));
  ASSERT_EQ(0, image.diff_iterate2("snap1", 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(1u, extents.size());
  ASSERT_EQ(diff_extent(0, 256, true, object_size), extents[0]);

  ASSERT_EQ(0, image.snap_set(NULL));
  ASSERT_EQ(1 << order, image.discard(0, 1 << order));
  ASSERT_EQ(0, image.snap_create("snap3"));
  ASSERT_EQ(0, image.snap_set("snap3"));

  extents.clear();
  ASSERT_EQ(0, image.diff_iterate2("snap1", 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(1u, extents.size());
  ASSERT_EQ(diff_extent(0, 256, false, object_size), extents[0]);
  ASSERT_PASSED(this->validate_object_map, image);
}

TYPED_TEST(DiffIterateTest, DiffIterateStress)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  bool skip_discard = this->is_skip_partial_discard_enabled();

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = this->get_temp_image_name();
  uint64_t size = 400 << 20;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint64_t object_size = 0;
  if (this->whole_object) {
    object_size = 1 << order;
  }

  interval_set<uint64_t> curexists;
  vector<interval_set<uint64_t> > wrote;
  vector<interval_set<uint64_t> > exists;
  vector<string> snap;
  int n = 20;
  for (int i=0; i<n; i++) {
    interval_set<uint64_t> w;
    scribble(image, 10, 8192000, skip_discard, &curexists, &w);
    cout << " i=" << i << " exists " << curexists << " wrote " << w << std::endl;
    string s = "snap" + stringify(i);
    ASSERT_EQ(0, image.snap_create(s.c_str()));
    wrote.push_back(w);
    exists.push_back(curexists);
    snap.push_back(s);
  }

  for (int h=0; h<n-1; h++) {
    for (int i=0; i<n-h-1; i++) {
      for (int j=(h==0 ? i+1 : n-1); j<n; j++) {
        interval_set<uint64_t> diff, actual, uex;
        for (int k=i+1; k<=j; k++)
          diff.union_of(wrote[k]);
        cout << "from " << i << " to "
             << (h != 0 ? string("HEAD") : stringify(j)) << " diff "
             << round_diff_interval(diff, object_size) << std::endl;

        // limit to extents that exists both at the beginning and at the end
        uex.union_of(exists[i], exists[j]);
        diff.intersection_of(uex);
        diff = round_diff_interval(diff, object_size);
        cout << " limited diff " << diff << std::endl;

        ASSERT_EQ(0, image.snap_set(h==0 ? snap[j].c_str() : NULL));
        ASSERT_EQ(0, image.diff_iterate2(snap[i].c_str(), 0, size, true,
                                         this->whole_object, iterate_cb,
                                         (void *)&actual));
        cout << " actual was " << actual << std::endl;
        if (!diff.subset_of(actual)) {
          interval_set<uint64_t> i;
          i.intersection_of(diff, actual);
          interval_set<uint64_t> l = diff;
          l.subtract(i);
          cout << " ... diff - (actual*diff) = " << l << std::endl;
        }
        ASSERT_TRUE(diff.subset_of(actual));
      }
    }
    ASSERT_EQ(0, image.snap_set(NULL));
    ASSERT_EQ(0, image.snap_remove(snap[n-h-1].c_str()));
  }

  ASSERT_PASSED(this->validate_object_map, image);
}

TYPED_TEST(DiffIterateTest, DiffIterateRegression6926)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  int order = 0;
  std::string name = this->get_temp_image_name();
  uint64_t size = 20 << 20;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint64_t object_size = 0;
  if (this->whole_object) {
    object_size = 1 << order;
  }
  vector<diff_extent> extents;
  ceph::bufferlist bl;

  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(0u, extents.size());

  ASSERT_EQ(0, image.snap_create("snap1"));
  char data[256];
  memset(data, 1, sizeof(data));
  bl.append(data, 256);
  ASSERT_EQ(256, image.write(0, 256, bl));

  extents.clear();
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(1u, extents.size());
  ASSERT_EQ(diff_extent(0, 256, true, object_size), extents[0]);

  ASSERT_EQ(0, image.snap_set("snap1"));
  extents.clear();
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
      			           vector_iterate_cb, (void *) &extents));
  ASSERT_EQ(static_cast<size_t>(0), extents.size());
}

TYPED_TEST(DiffIterateTest, DiffIterateIgnoreParent)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  bool skip_discard = this->is_skip_partial_discard_enabled();

  librbd::RBD rbd;
  librbd::Image image;
  std::string name = this->get_temp_image_name();
  uint64_t size = 20 << 20;
  int order = 0;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint64_t object_size = 0;
  if (this->whole_object) {
    object_size = 1 << order;
  }

  bufferlist bl;
  bl.append(buffer::create(size));
  bl.zero();
  interval_set<uint64_t> one;
  one.insert(0, size);
  ASSERT_EQ((int)size, image.write(0, size, bl));
  ASSERT_EQ(0, image.snap_create("one"));
  ASSERT_EQ(0, image.snap_protect("one"));

  std::string clone_name = this->get_temp_image_name();
  ASSERT_EQ(0, rbd.clone(ioctx, name.c_str(), "one", ioctx, clone_name.c_str(),
                         RBD_FEATURE_LAYERING, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, clone_name.c_str(), NULL));

  interval_set<uint64_t> exists;
  interval_set<uint64_t> two;
  scribble(image, 10, 102400, skip_discard, &exists, &two);
  two = round_diff_interval(two, object_size);
  cout << " wrote " << two << " to clone" << std::endl;

  interval_set<uint64_t> diff;
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, false, this->whole_object,
                                   iterate_cb, (void *)&diff));
  cout << " diff was " << diff << std::endl;
  if (!this->whole_object) {
    ASSERT_FALSE(one.subset_of(diff));
  }
  ASSERT_TRUE(two.subset_of(diff));
}

TYPED_TEST(DiffIterateTest, DiffIterateCallbackError)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  bool skip_discard = this->is_skip_partial_discard_enabled();

  {
    librbd::RBD rbd;
    librbd::Image image;
    int order = 0;
    std::string name = this->get_temp_image_name();
    uint64_t size = 20 << 20;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    interval_set<uint64_t> exists;
    interval_set<uint64_t> one;
    scribble(image, 10, 102400, skip_discard, &exists, &one);
    cout << " wrote " << one << std::endl;

    interval_set<uint64_t> diff;
    ASSERT_EQ(-EINVAL, image.diff_iterate2(NULL, 0, size, true,
                                           this->whole_object,
                                           iterate_error_cb, NULL));
  }
  ioctx.close();
}

TYPED_TEST(DiffIterateTest, DiffIterateParentDiscard)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, this->_rados.ioctx_create(this->m_pool_name.c_str(), ioctx));

  bool skip_discard = this->is_skip_partial_discard_enabled();

  librbd::RBD rbd;
  librbd::Image image;
  std::string name = this->get_temp_image_name();
  uint64_t size = 20 << 20;
  int order = 0;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint64_t object_size = 0;
  if (this->whole_object) {
    object_size = 1 << order;
  }

  interval_set<uint64_t> exists;
  interval_set<uint64_t> one;
  scribble(image, 10, 102400, skip_discard, &exists, &one);
  ASSERT_EQ(0, image.snap_create("one"));

  ASSERT_EQ(1 << order, image.discard(0, 1 << order));
  ASSERT_EQ(0, image.snap_create("two"));
  ASSERT_EQ(0, image.snap_protect("two"));
  exists.clear();
  one.clear();

  std::string clone_name = this->get_temp_image_name();
  ASSERT_EQ(0, rbd.clone(ioctx, name.c_str(), "two", ioctx,
                         clone_name.c_str(), RBD_FEATURE_LAYERING, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, clone_name.c_str(), NULL));

  interval_set<uint64_t> two;
  scribble(image, 10, 102400, skip_discard, &exists, &two);
  two = round_diff_interval(two, object_size);

  interval_set<uint64_t> diff;
  ASSERT_EQ(0, image.diff_iterate2(NULL, 0, size, true, this->whole_object,
                                   iterate_cb, (void *)&diff));
  ASSERT_TRUE(two.subset_of(diff));
}

TEST_F(TestLibRBD, ZeroLengthWrite)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char read_data[1];
  ASSERT_EQ(0, rbd_write(image, 0, 0, NULL));
  ASSERT_EQ(1, rbd_read(image, 0, 1, read_data));
  ASSERT_EQ('\0', read_data[0]);

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}


TEST_F(TestLibRBD, ZeroLengthDiscard)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  const char data[] = "blah";
  char read_data[sizeof(data)];
  ASSERT_EQ((int)strlen(data), rbd_write(image, 0, strlen(data), data));
  ASSERT_EQ(0, rbd_discard(image, 0, 0));
  ASSERT_EQ((int)strlen(data), rbd_read(image, 0, strlen(data), read_data));
  ASSERT_EQ(0, memcmp(data, read_data, strlen(data)));

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, ZeroLengthRead)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char read_data[1];
  ASSERT_EQ(0, rbd_read(image, 0, 0, read_data));

  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, LargeCacheRead)
{
  std::string config_value;
  ASSERT_EQ(0, _rados.conf_get("rbd_cache", config_value));
  if (config_value == "false") {
    std::cout << "SKIPPING due to disabled cache" << std::endl;
    return;
  }

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  uint32_t new_cache_size = 1 << 20;
  std::string orig_cache_size;
  ASSERT_EQ(0, _rados.conf_get("rbd_cache_size", orig_cache_size));
  ASSERT_EQ(0, _rados.conf_set("rbd_cache_size",
                               stringify(new_cache_size).c_str()));
  ASSERT_EQ(0, _rados.conf_get("rbd_cache_size", config_value));
  ASSERT_EQ(stringify(new_cache_size), config_value);
  BOOST_SCOPE_EXIT( (orig_cache_size) ) {
    ASSERT_EQ(0, _rados.conf_set("rbd_cache_size", orig_cache_size.c_str()));
  } BOOST_SCOPE_EXIT_END;

  rbd_image_t image;
  int order = 21;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << order;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  std::string buffer(1 << order, '1');
 
  ASSERT_EQ(static_cast<ssize_t>(buffer.size()),
	    rbd_write(image, 0, buffer.size(), buffer.c_str()));

  ASSERT_EQ(0, rbd_invalidate_cache(image));

  ASSERT_EQ(static_cast<ssize_t>(buffer.size()), 
  	    rbd_read(image, 0, buffer.size(), &buffer[0]));

  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestPendingAio)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  bool old_format;
  uint64_t features;
  rbd_image_t image;
  int order = 0;

  ASSERT_EQ(0, get_features(&old_format, &features));
  ASSERT_FALSE(old_format);

  std::string name = get_temp_image_name();

  uint64_t size = 4 << 20;
  ASSERT_EQ(0, create_image_full(ioctx, name.c_str(), size, &order,
				 false, features));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  char test_data[TEST_IO_SIZE];
  for (size_t i = 0; i < TEST_IO_SIZE; ++i) {
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  }

  size_t num_aios = 256;
  rbd_completion_t comps[num_aios];
  for (size_t i = 0; i < num_aios; ++i) {
    ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &comps[i]));
    uint64_t offset = rand() % (size - TEST_IO_SIZE);
    ASSERT_EQ(0, rbd_aio_write(image, offset, TEST_IO_SIZE, test_data,
                               comps[i]));
  }
  for (size_t i = 0; i < num_aios; ++i) {
    ASSERT_EQ(0, rbd_aio_wait_for_complete(comps[i]));
    rbd_aio_release(comps[i]);
  }
  ASSERT_EQ(0, rbd_invalidate_cache(image));

  for (size_t i = 0; i < num_aios; ++i) {
    ASSERT_EQ(0, rbd_aio_create_completion(NULL, NULL, &comps[i]));
    uint64_t offset = rand() % (size - TEST_IO_SIZE);
    ASSERT_LE(0, rbd_aio_read(image, offset, TEST_IO_SIZE, test_data,
                              comps[i]));
  }

  ASSERT_PASSED(validate_object_map, image);
  ASSERT_EQ(0, rbd_close(image));
  for (size_t i = 0; i < num_aios; ++i) {
    ASSERT_EQ(1, rbd_aio_is_complete(comps[i]));
    rbd_aio_release(comps[i]);
  }

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, Flatten)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string parent_name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, parent_name.c_str(), size, &order));

  librbd::Image parent_image;
  ASSERT_EQ(0, rbd.open(ioctx, parent_image, parent_name.c_str(), NULL));

  bufferlist bl;
  bl.append(std::string(4096, '1'));
  ASSERT_EQ((ssize_t)bl.length(), parent_image.write(0, bl.length(), bl));

  ASSERT_EQ(0, parent_image.snap_create("snap1"));
  ASSERT_EQ(0, parent_image.snap_protect("snap1"));

  uint64_t features;
  ASSERT_EQ(0, parent_image.features(&features));

  std::string clone_name = get_temp_image_name();
  EXPECT_EQ(0, rbd.clone(ioctx, parent_name.c_str(), "snap1", ioctx,
       clone_name.c_str(), features, &order));

  librbd::Image clone_image;
  ASSERT_EQ(0, rbd.open(ioctx, clone_image, clone_name.c_str(), NULL));
  ASSERT_EQ(0, clone_image.flatten());

  librbd::RBD::AioCompletion *read_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  bufferlist read_bl;
  clone_image.aio_read(0, bl.length(), read_bl, read_comp);
  ASSERT_EQ(0, read_comp->wait_for_complete());
  ASSERT_EQ((ssize_t)bl.length(), read_comp->get_return_value());
  read_comp->release();
  ASSERT_TRUE(bl.contents_equal(read_bl));

  ASSERT_PASSED(validate_object_map, clone_image);
}

TEST_F(TestLibRBD, SnapshotLimit)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  uint64_t limit;

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_snap_get_limit(image, &limit));
  ASSERT_EQ(UINT64_MAX, limit);
  ASSERT_EQ(0, rbd_snap_set_limit(image, 2));
  ASSERT_EQ(0, rbd_snap_get_limit(image, &limit));
  ASSERT_EQ(2U, limit);

  ASSERT_EQ(0, rbd_snap_create(image, "snap1"));
  ASSERT_EQ(0, rbd_snap_create(image, "snap2"));
  ASSERT_EQ(-EDQUOT, rbd_snap_create(image, "snap3"));
  ASSERT_EQ(0, rbd_snap_set_limit(image, UINT64_MAX));
  ASSERT_EQ(0, rbd_snap_create(image, "snap3"));
  ASSERT_EQ(0, rbd_close(image));

  rados_ioctx_destroy(ioctx);
}
  

TEST_F(TestLibRBD, SnapshotLimitPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  {
    librbd::RBD rbd;
    librbd::Image image;
    std::string name = get_temp_image_name();
    uint64_t size = 2 << 20;
    int order = 0;
    uint64_t limit;

    ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    ASSERT_EQ(0, image.snap_get_limit(&limit));
    ASSERT_EQ(UINT64_MAX, limit);
    ASSERT_EQ(0, image.snap_set_limit(2));
    ASSERT_EQ(0, image.snap_get_limit(&limit));
    ASSERT_EQ(2U, limit);

    ASSERT_EQ(0, image.snap_create("snap1"));
    ASSERT_EQ(0, image.snap_create("snap2"));
    ASSERT_EQ(-EDQUOT, image.snap_create("snap3"));
    ASSERT_EQ(0, image.snap_set_limit(UINT64_MAX));
    ASSERT_EQ(0, image.snap_create("snap3"));
  }

  ioctx.close();
}

TEST_F(TestLibRBD, RebuildObjectMapViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK | RBD_FEATURE_OBJECT_MAP);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  std::string object_map_oid;
  {
    librbd::Image image;
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    std::string image_id;
    ASSERT_EQ(0, get_image_id(image, &image_id));
    object_map_oid = RBD_OBJECT_MAP_PREFIX + image_id;
  }

  // corrupt the object map
  bufferlist bl;
  bl.append("foo");
  ASSERT_EQ(0, ioctx.write(object_map_oid, bl, bl.length(), 0));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bool lock_owner;
  bl.clear();
  ASSERT_EQ(0, image1.write(0, 0, bl));
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  uint64_t flags;
  ASSERT_EQ(0, image1.get_flags(&flags));
  ASSERT_TRUE((flags & RBD_FLAG_OBJECT_MAP_INVALID) != 0);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));
  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  PrintProgress prog_ctx;
  ASSERT_EQ(0, image2.rebuild_object_map(prog_ctx));
  ASSERT_PASSED(validate_object_map, image1);
  ASSERT_PASSED(validate_object_map, image2);
}

TEST_F(TestLibRBD, RenameViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  std::string new_name = get_temp_image_name();
  ASSERT_EQ(0, rbd.rename(ioctx, name.c_str(), new_name.c_str()));
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, new_name.c_str(), NULL));
}

TEST_F(TestLibRBD, SnapCreateViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  // switch to writeback cache
  ASSERT_EQ(0, image1.flush());

  bufferlist bl;
  bl.append(std::string(4096, '1'));
  ASSERT_EQ((ssize_t)bl.length(), image1.write(0, bl.length(), bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.snap_create("snap1"));
  bool exists;
  ASSERT_EQ(0, image1.snap_exists2("snap1", &exists));
  ASSERT_TRUE(exists);
  ASSERT_EQ(0, image2.snap_exists2("snap1", &exists));
  ASSERT_TRUE(exists);

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
}

TEST_F(TestLibRBD, SnapRemoveViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_FAST_DIFF);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));
  ASSERT_EQ(0, image1.snap_create("snap1"));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.snap_remove("snap1"));
  bool exists;
  ASSERT_EQ(0, image1.snap_exists2("snap1", &exists));
  ASSERT_FALSE(exists);
  ASSERT_EQ(0, image2.snap_exists2("snap1", &exists));
  ASSERT_FALSE(exists);

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
}

TEST_F(TestLibRBD, SnapRemove2)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));
  ASSERT_EQ(0, image1.snap_create("snap1"));
  bool exists;
  ASSERT_EQ(0, image1.snap_exists2("snap1", &exists));
  ASSERT_TRUE(exists);
  ASSERT_EQ(0, image1.snap_protect("snap1"));
  bool is_protected;
  ASSERT_EQ(0, image1.snap_is_protected("snap1", &is_protected));
  ASSERT_TRUE(is_protected);

  uint64_t features;
  ASSERT_EQ(0, image1.features(&features));

  std::string child_name = get_temp_image_name();
  EXPECT_EQ(0, rbd.clone(ioctx, name.c_str(), "snap1", ioctx,
			 child_name.c_str(), features, &order));

  ASSERT_EQ(0, image1.snap_exists2("snap1", &exists));
  ASSERT_TRUE(exists);
  ASSERT_EQ(0, image1.snap_is_protected("snap1", &is_protected));
  ASSERT_TRUE(is_protected);

  ASSERT_EQ(-EBUSY, image1.snap_remove("snap1"));
  PrintProgress pp;
  ASSERT_EQ(0, image1.snap_remove2("snap1", RBD_SNAP_REMOVE_FORCE, pp));
  ASSERT_EQ(0, image1.snap_exists2("snap1", &exists));
  ASSERT_FALSE(exists);
}

TEST_F(TestLibRBD, SnapRenameViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING | RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));
  ASSERT_EQ(0, image1.snap_create("snap1"));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.snap_rename("snap1", "snap1-rename"));
  bool exists;
  ASSERT_EQ(0, image1.snap_exists2("snap1-rename", &exists));
  ASSERT_TRUE(exists);
  ASSERT_EQ(0, image2.snap_exists2("snap1-rename", &exists));
  ASSERT_TRUE(exists);

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
}

TEST_F(TestLibRBD, SnapProtectViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING | RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
  ASSERT_EQ(0, image1.snap_create("snap1"));

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.snap_protect("snap1"));
  bool is_protected;
  ASSERT_EQ(0, image2.snap_is_protected("snap1", &is_protected));
  ASSERT_TRUE(is_protected);
  ASSERT_EQ(0, image1.snap_is_protected("snap1", &is_protected));
  ASSERT_TRUE(is_protected);

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
}

TEST_F(TestLibRBD, SnapUnprotectViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING | RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
  ASSERT_EQ(0, image1.snap_create("snap1"));
  ASSERT_EQ(0, image1.snap_protect("snap1"));
  bool is_protected;
  ASSERT_EQ(0, image1.snap_is_protected("snap1", &is_protected));
  ASSERT_TRUE(is_protected);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.snap_unprotect("snap1"));
  ASSERT_EQ(0, image2.snap_is_protected("snap1", &is_protected));
  ASSERT_FALSE(is_protected);
  ASSERT_EQ(0, image1.snap_is_protected("snap1", &is_protected));
  ASSERT_FALSE(is_protected);

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
}

TEST_F(TestLibRBD, FlattenViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string parent_name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, parent_name.c_str(), size, &order));

  librbd::Image parent_image;
  ASSERT_EQ(0, rbd.open(ioctx, parent_image, parent_name.c_str(), NULL));
  ASSERT_EQ(0, parent_image.snap_create("snap1"));
  ASSERT_EQ(0, parent_image.snap_protect("snap1"));

  uint64_t features;
  ASSERT_EQ(0, parent_image.features(&features));

  std::string name = get_temp_image_name();
  EXPECT_EQ(0, rbd.clone(ioctx, parent_name.c_str(), "snap1", ioctx,
			 name.c_str(), features, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.flatten());

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
  ASSERT_PASSED(validate_object_map, image1);
}

TEST_F(TestLibRBD, ResizeViaLockOwner)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image1.write(0, 0, bl));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  ASSERT_EQ(0, image2.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, image2.resize(0));

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);
  ASSERT_PASSED(validate_object_map, image1);
}

TEST_F(TestLibRBD, ObjectMapConsistentSnap)
{
  REQUIRE_FEATURE(RBD_FEATURE_OBJECT_MAP);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  int num_snaps = 10;
  for (int i = 0; i < num_snaps; ++i) {
    std::string snap_name = "snap" + stringify(i);
    ASSERT_EQ(0, image1.snap_create(snap_name.c_str()));
  }


  thread writer([&image1](){
      librbd::image_info_t info;
      int r = image1.stat(info, sizeof(info));
      assert(r == 0);
      bufferlist bl;
      bl.append("foo");
      for (unsigned i = 0; i < info.num_objs; ++i) {
	r = image1.write((1 << info.order) * i, bl.length(), bl);
	assert(r == (int) bl.length());
      }
    });
  writer.join();

  for (int i = 0; i < num_snaps; ++i) {
    std::string snap_name = "snap" + stringify(i);
    ASSERT_EQ(0, image1.snap_set(snap_name.c_str()));
    ASSERT_PASSED(validate_object_map, image1);
  }

  ASSERT_EQ(0, image1.snap_set(NULL));
  ASSERT_PASSED(validate_object_map, image1);
}

void memset_rand(char *buf, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    buf[i] = (char) (rand() % (126 - 33) + 33);
  }
}

TEST_F(TestLibRBD, Metadata)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));

  rbd_image_t image;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  rbd_image_t image1;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image1, NULL));

  char keys[1024];
  char vals[1024];
  size_t keys_len = sizeof(keys);
  size_t vals_len = sizeof(vals);

  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);

  ASSERT_EQ(0, rbd_metadata_list(image, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(0U, keys_len);
  ASSERT_EQ(0U, vals_len);

  char value[1024];
  size_t value_len = sizeof(value);
  memset_rand(value, value_len);

  ASSERT_EQ(0, rbd_metadata_set(image1, "key1", "value1"));
  ASSERT_EQ(0, rbd_metadata_set(image1, "key2", "value2"));
  ASSERT_EQ(0, rbd_metadata_get(image1, "key1", value, &value_len));
  ASSERT_STREQ(value, "value1");
  value_len = 1;
  ASSERT_EQ(-ERANGE, rbd_metadata_get(image1, "key1", value, &value_len));
  ASSERT_EQ(value_len, strlen("value1") + 1);

  ASSERT_EQ(-ERANGE, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                       &vals_len));
  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, strlen("key1") + 1 + strlen("key2") + 1);
  ASSERT_EQ(vals_len, strlen("value1") + 1 + strlen("value2") + 1);
  ASSERT_STREQ(keys, "key1");
  ASSERT_STREQ(keys + strlen(keys) + 1, "key2");
  ASSERT_STREQ(vals, "value1");
  ASSERT_STREQ(vals + strlen(vals) + 1, "value2");

  ASSERT_EQ(0, rbd_metadata_remove(image1, "key1"));
  ASSERT_EQ(-ENOENT, rbd_metadata_remove(image1, "key3"));
  value_len = sizeof(value);
  ASSERT_EQ(-ENOENT, rbd_metadata_get(image1, "key3", value, &value_len));
  ASSERT_EQ(0, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, strlen("key2") + 1);
  ASSERT_EQ(vals_len, strlen("value2") + 1);
  ASSERT_STREQ(keys, "key2");
  ASSERT_STREQ(vals, "value2");

  // test config setting
  ASSERT_EQ(0, rbd_metadata_set(image1, "conf_rbd_cache", "false"));
  ASSERT_EQ(-EINVAL, rbd_metadata_set(image1, "conf_rbd_cache", "INVALID_VAL"));
  ASSERT_EQ(0, rbd_metadata_remove(image1, "conf_rbd_cache"));

  // test metadata with snapshot adding
  ASSERT_EQ(0, rbd_snap_create(image1, "snap1"));
  ASSERT_EQ(0, rbd_snap_protect(image1, "snap1"));
  ASSERT_EQ(0, rbd_snap_set(image1, "snap1"));

  ASSERT_EQ(0, rbd_metadata_set(image1, "key1", "value1"));
  ASSERT_EQ(0, rbd_metadata_set(image1, "key3", "value3"));

  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len,
            strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") + 1);
  ASSERT_EQ(vals_len,
            strlen("value1") + 1 + strlen("value2") + 1 + strlen("value3") + 1);
  ASSERT_STREQ(keys, "key1");
  ASSERT_STREQ(keys + strlen("key1") + 1, "key2");
  ASSERT_STREQ(keys + strlen("key1") + 1 + strlen("key2") + 1, "key3");
  ASSERT_STREQ(vals, "value1");
  ASSERT_STREQ(vals + strlen("value1") + 1, "value2");
  ASSERT_STREQ(vals + strlen("value1") + 1 + strlen("value2") + 1, "value3");

  ASSERT_EQ(0, rbd_snap_set(image1, NULL));
  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len,
            strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") + 1);
  ASSERT_EQ(vals_len,
            strlen("value1") + 1 + strlen("value2") + 1 + strlen("value3") + 1);
  ASSERT_STREQ(keys, "key1");
  ASSERT_STREQ(keys + strlen("key1") + 1, "key2");
  ASSERT_STREQ(keys + strlen("key1") + 1 + strlen("key2") + 1, "key3");
  ASSERT_STREQ(vals, "value1");
  ASSERT_STREQ(vals + strlen("value1") + 1, "value2");
  ASSERT_STREQ(vals + strlen("value1") + 1 + strlen("value2") + 1, "value3");

  // test metadata with cloning
  uint64_t features;
  ASSERT_EQ(0, rbd_get_features(image1, &features));

  string cname = get_temp_image_name();
  EXPECT_EQ(0, rbd_clone(ioctx, name.c_str(), "snap1", ioctx,
                         cname.c_str(), features, &order));
  rbd_image_t image2;
  ASSERT_EQ(0, rbd_open(ioctx, cname.c_str(), &image2, NULL));
  ASSERT_EQ(0, rbd_metadata_set(image2, "key4", "value4"));

  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image2, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") +
            1 + strlen("key4") + 1);
  ASSERT_EQ(vals_len, strlen("value1") + 1 + strlen("value2") + 1 +
            strlen("value3") + 1 + strlen("value4") + 1);
  ASSERT_STREQ(keys + strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") +
               1, "key4");
  ASSERT_STREQ(vals + strlen("value1") + 1 + strlen("value2") + 1 +
               strlen("value3") + 1, "value4");

  ASSERT_EQ(0, rbd_metadata_list(image1, "", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len,
            strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") + 1);
  ASSERT_EQ(vals_len,
            strlen("value1") + 1 + strlen("value2") + 1 + strlen("value3") + 1);
  ASSERT_EQ(-ENOENT, rbd_metadata_get(image1, "key4", value, &value_len));

  // test short buffer cases
  keys_len = strlen("key1") + 1;
  vals_len = strlen("value1") + 1;
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image2, "", 1, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, strlen("key1") + 1);
  ASSERT_EQ(vals_len, strlen("value1") + 1);
  ASSERT_STREQ(keys, "key1");
  ASSERT_STREQ(vals, "value1");

  ASSERT_EQ(-ERANGE, rbd_metadata_list(image2, "", 2, keys, &keys_len, vals,
                                       &vals_len));
  ASSERT_EQ(keys_len, strlen("key1") + 1 + strlen("key2") + 1);
  ASSERT_EQ(vals_len, strlen("value1") + 1 + strlen("value2") + 1);

  ASSERT_EQ(-ERANGE, rbd_metadata_list(image2, "", 0, keys, &keys_len, vals,
                                       &vals_len));
  ASSERT_EQ(keys_len, strlen("key1") + 1 + strlen("key2") + 1 + strlen("key3") +
            1 + strlen("key4") + 1);
  ASSERT_EQ(vals_len, strlen("value1") + 1 + strlen("value2") + 1 +
            strlen("value3") + 1 + strlen("value4") + 1);

  // test `start` param
  keys_len = sizeof(keys);
  vals_len = sizeof(vals);
  memset_rand(keys, keys_len);
  memset_rand(vals, vals_len);
  ASSERT_EQ(0, rbd_metadata_list(image2, "key2", 0, keys, &keys_len, vals,
                                 &vals_len));
  ASSERT_EQ(keys_len, strlen("key3") + 1 + strlen("key4") + 1);
  ASSERT_EQ(vals_len, strlen("value3") + 1 + strlen("value4") + 1);
  ASSERT_STREQ(keys, "key3");
  ASSERT_STREQ(vals, "value3");

  ASSERT_EQ(0, rbd_close(image));
  ASSERT_EQ(0, rbd_close(image1));
  ASSERT_EQ(0, rbd_close(image2));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, MetadataPP)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  uint64_t features;
  string value;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));
  map<string, bufferlist> pairs;
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_TRUE(pairs.empty());

  ASSERT_EQ(0, image1.metadata_set("key1", "value1"));
  ASSERT_EQ(0, image1.metadata_set("key2", "value2"));
  ASSERT_EQ(0, image1.metadata_get("key1", &value));
  ASSERT_EQ(0, strcmp("value1", value.c_str()));
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_EQ(2U, pairs.size());
  ASSERT_EQ(0, strncmp("value1", pairs["key1"].c_str(), 6));
  ASSERT_EQ(0, strncmp("value2", pairs["key2"].c_str(), 6));

  pairs.clear();
  ASSERT_EQ(0, image1.metadata_remove("key1"));
  ASSERT_EQ(-ENOENT, image1.metadata_remove("key3"));
  ASSERT_TRUE(image1.metadata_get("key3", &value) < 0);
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_EQ(1U, pairs.size());
  ASSERT_EQ(0, strncmp("value2", pairs["key2"].c_str(), 6));

  // test config setting
  ASSERT_EQ(0, image1.metadata_set("conf_rbd_cache", "false"));
  ASSERT_EQ(-EINVAL, image1.metadata_set("conf_rbd_cache", "INVALID_VALUE"));
  ASSERT_EQ(0, image1.metadata_remove("conf_rbd_cache"));

  // test metadata with snapshot adding
  ASSERT_EQ(0, image1.snap_create("snap1"));
  ASSERT_EQ(0, image1.snap_protect("snap1"));
  ASSERT_EQ(0, image1.snap_set("snap1"));

  pairs.clear();
  ASSERT_EQ(0, image1.metadata_set("key1", "value1"));
  ASSERT_EQ(0, image1.metadata_set("key3", "value3"));
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_EQ(3U, pairs.size());
  ASSERT_EQ(0, strncmp("value1", pairs["key1"].c_str(), 6));
  ASSERT_EQ(0, strncmp("value2", pairs["key2"].c_str(), 6));
  ASSERT_EQ(0, strncmp("value3", pairs["key3"].c_str(), 6));

  ASSERT_EQ(0, image1.snap_set(NULL));
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_EQ(3U, pairs.size());
  ASSERT_EQ(0, strncmp("value1", pairs["key1"].c_str(), 6));
  ASSERT_EQ(0, strncmp("value2", pairs["key2"].c_str(), 6));
  ASSERT_EQ(0, strncmp("value3", pairs["key3"].c_str(), 6));

  // test metadata with cloning
  string cname = get_temp_image_name();
  librbd::Image image2;
  ASSERT_EQ(0, image1.features(&features));
  EXPECT_EQ(0, rbd.clone(ioctx, name.c_str(), "snap1", ioctx,
                         cname.c_str(), features, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image2, cname.c_str(), NULL));
  ASSERT_EQ(0, image2.metadata_set("key4", "value4"));
  pairs.clear();
  ASSERT_EQ(0, image2.metadata_list("", 0, &pairs));
  ASSERT_EQ(4U, pairs.size());
  pairs.clear();
  ASSERT_EQ(0, image1.metadata_list("", 0, &pairs));
  ASSERT_EQ(3U, pairs.size());
  ASSERT_EQ(-ENOENT, image1.metadata_get("key4", &value));
}

TEST_F(TestLibRBD, UpdateFeatures)
{
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  uint8_t old_format;
  ASSERT_EQ(0, image.old_format(&old_format));
  if (old_format) {
    ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_EXCLUSIVE_LOCK, true));
    return;
  }

  uint64_t features;
  ASSERT_EQ(0, image.features(&features));

  // must provide a single feature
  ASSERT_EQ(-EINVAL, image.update_features(0, true));

  uint64_t disable_features;
  disable_features = features & (RBD_FEATURE_EXCLUSIVE_LOCK |
                                 RBD_FEATURE_OBJECT_MAP |
                                 RBD_FEATURE_FAST_DIFF |
                                 RBD_FEATURE_JOURNALING);
  if (disable_features != 0) {
    ASSERT_EQ(0, image.update_features(disable_features, false));
  }

  ASSERT_EQ(0, image.features(&features));
  ASSERT_EQ(0U, features & disable_features);

  // cannot enable object map nor journaling w/o exclusive lock
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_OBJECT_MAP, true));
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_JOURNALING, true));
  ASSERT_EQ(0, image.update_features(RBD_FEATURE_EXCLUSIVE_LOCK, true));

  ASSERT_EQ(0, image.features(&features));
  ASSERT_NE(0U, features & RBD_FEATURE_EXCLUSIVE_LOCK);

  // cannot enable fast diff w/o object map
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_FAST_DIFF, true));
  ASSERT_EQ(0, image.update_features(RBD_FEATURE_OBJECT_MAP, true));
  ASSERT_EQ(0, image.features(&features));
  ASSERT_NE(0U, features & RBD_FEATURE_OBJECT_MAP);

  uint64_t expected_flags = RBD_FLAG_OBJECT_MAP_INVALID;
  uint64_t flags;
  ASSERT_EQ(0, image.get_flags(&flags));
  ASSERT_EQ(expected_flags, flags);

  ASSERT_EQ(0, image.update_features(RBD_FEATURE_OBJECT_MAP, false));
  ASSERT_EQ(0, image.features(&features));
  ASSERT_EQ(0U, features & RBD_FEATURE_OBJECT_MAP);

  ASSERT_EQ(0, image.update_features(RBD_FEATURE_OBJECT_MAP |
                                     RBD_FEATURE_FAST_DIFF |
                                     RBD_FEATURE_JOURNALING, true));

  expected_flags = RBD_FLAG_OBJECT_MAP_INVALID | RBD_FLAG_FAST_DIFF_INVALID;
  ASSERT_EQ(0, image.get_flags(&flags));
  ASSERT_EQ(expected_flags, flags);

  // cannot disable object map w/ fast diff
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_OBJECT_MAP, false));
  ASSERT_EQ(0, image.update_features(RBD_FEATURE_FAST_DIFF, false));
  ASSERT_EQ(0, image.features(&features));
  ASSERT_EQ(0U, features & RBD_FEATURE_FAST_DIFF);

  expected_flags = RBD_FLAG_OBJECT_MAP_INVALID;
  ASSERT_EQ(0, image.get_flags(&flags));
  ASSERT_EQ(expected_flags, flags);

  // cannot disable exclusive lock w/ object map
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_EXCLUSIVE_LOCK, false));
  ASSERT_EQ(0, image.update_features(RBD_FEATURE_OBJECT_MAP, false));

  // cannot disable exclusive lock w/ journaling
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_EXCLUSIVE_LOCK, false));
  ASSERT_EQ(0, image.update_features(RBD_FEATURE_JOURNALING, false));

  ASSERT_EQ(0, image.get_flags(&flags));
  ASSERT_EQ(0U, flags);

  ASSERT_EQ(0, image.update_features(RBD_FEATURE_EXCLUSIVE_LOCK, false));

  ASSERT_EQ(0, image.features(&features));
  if ((features & RBD_FEATURE_DEEP_FLATTEN) != 0) {
    ASSERT_EQ(0, image.update_features(RBD_FEATURE_DEEP_FLATTEN, false));
  }
  ASSERT_EQ(-EINVAL, image.update_features(RBD_FEATURE_DEEP_FLATTEN, true));
}

TEST_F(TestLibRBD, RebuildObjectMap)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 18;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  PrintProgress prog_ctx;
  std::string object_map_oid;
  bufferlist bl;
  bl.append("foo");
  {
    librbd::Image image;
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    uint64_t features;
    ASSERT_EQ(0, image.features(&features));
    if ((features & RBD_FEATURE_OBJECT_MAP) == 0) {
      ASSERT_EQ(-EINVAL, image.rebuild_object_map(prog_ctx));
      return;
    }

    ASSERT_EQ((ssize_t)bl.length(), image.write(0, bl.length(), bl));

    ASSERT_EQ(0, image.snap_create("snap1"));
    ASSERT_EQ((ssize_t)bl.length(), image.write(1<<order, bl.length(), bl));

    std::string image_id;
    ASSERT_EQ(0, get_image_id(image, &image_id));
    object_map_oid = RBD_OBJECT_MAP_PREFIX + image_id;
  }

  // corrupt the object map
  ASSERT_EQ(0, ioctx.write(object_map_oid, bl, bl.length(), 0));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bool lock_owner;
  bl.clear();
  ASSERT_EQ(0, image1.write(0, 0, bl));
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  uint64_t flags;
  ASSERT_EQ(0, image1.get_flags(&flags));
  ASSERT_TRUE((flags & RBD_FLAG_OBJECT_MAP_INVALID) != 0);

  ASSERT_EQ(0, image1.rebuild_object_map(prog_ctx));

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  bufferlist read_bl;
  ASSERT_EQ((ssize_t)bl.length(), image2.read(0, bl.length(), read_bl));
  ASSERT_TRUE(bl.contents_equal(read_bl));

  read_bl.clear();
  ASSERT_EQ((ssize_t)bl.length(), image2.read(1<<order, bl.length(), read_bl));
  ASSERT_TRUE(bl.contents_equal(read_bl));

  ASSERT_PASSED(validate_object_map, image1);
  ASSERT_PASSED(validate_object_map, image2);
}

TEST_F(TestLibRBD, RebuildNewObjectMap)
{
  REQUIRE_FEATURE(RBD_FEATURE_OBJECT_MAP);

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 18;
  uint64_t features = RBD_FEATURE_EXCLUSIVE_LOCK;
  ASSERT_EQ(0, create_image_full(ioctx, name.c_str(), size, &order,
				 false, features));

  rbd_image_t image;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));
  ASSERT_EQ(0, rbd_update_features(image, RBD_FEATURE_OBJECT_MAP, true));
  ASSERT_EQ(0, rbd_rebuild_object_map(image, print_progress_percent, NULL));

  ASSERT_PASSED(validate_object_map, image);

  ASSERT_EQ(0, rbd_close(image));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, CheckObjectMap)
{
  REQUIRE_FEATURE(RBD_FEATURE_OBJECT_MAP);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 18;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  PrintProgress prog_ctx;
  bufferlist bl1;
  bufferlist bl2;
  bl1.append("foo");
  {
    librbd::Image image;
    ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

    uint64_t features;
    ASSERT_EQ(0, image.features(&features));

    ASSERT_EQ((ssize_t)bl1.length(), image.write(0, bl1.length(), bl1));

    ASSERT_EQ(0, image.snap_create("snap1"));
    ASSERT_EQ((ssize_t)bl1.length(), image.write(1<<order, bl1.length(), bl1));
  }

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  std::string image_id;
  ASSERT_EQ(0, get_image_id(image1, &image_id));

  std::string object_map_oid = RBD_OBJECT_MAP_PREFIX + image_id;

  ASSERT_LT(0, ioctx.read(object_map_oid, bl2, 1024, 0));

  bool lock_owner;
  ASSERT_EQ((ssize_t)bl1.length(), image1.write(3 * (1 << 18), bl1.length(), bl1));
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  //reopen image to reread now corrupt object map from disk
  image1.close();

  bl1.clear();
  ASSERT_LT(0, ioctx.read(object_map_oid, bl1, 1024, 0));
  ASSERT_FALSE(bl1.contents_equal(bl2));

  ASSERT_EQ(0, ioctx.write_full(object_map_oid, bl2));
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  uint64_t flags;
  ASSERT_EQ(0, image1.get_flags(&flags));
  ASSERT_TRUE((flags & RBD_FLAG_OBJECT_MAP_INVALID) == 0);

  ASSERT_EQ(0, image1.check_object_map(prog_ctx));

  ASSERT_EQ(0, image1.get_flags(&flags));
  ASSERT_TRUE((flags & RBD_FLAG_OBJECT_MAP_INVALID) != 0);
}

TEST_F(TestLibRBD, BlockingAIO)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  bool skip_discard = is_skip_partial_discard_enabled();

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 18;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  std::string non_blocking_aio;
  ASSERT_EQ(0, _rados.conf_get("rbd_non_blocking_aio", non_blocking_aio));
  ASSERT_EQ(0, _rados.conf_set("rbd_non_blocking_aio", "0"));
  BOOST_SCOPE_EXIT( (non_blocking_aio) ) {
    ASSERT_EQ(0, _rados.conf_set("rbd_non_blocking_aio",
                                 non_blocking_aio.c_str()));
  } BOOST_SCOPE_EXIT_END;

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  bufferlist bl;
  ASSERT_EQ(0, image.write(0, bl.length(), bl));

  bl.append(std::string(256, '1'));
  librbd::RBD::AioCompletion *write_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_write(0, bl.length(), bl, write_comp));

  librbd::RBD::AioCompletion *flush_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_flush(flush_comp));
  ASSERT_EQ(0, flush_comp->wait_for_complete());
  ASSERT_EQ(0, flush_comp->get_return_value());
  flush_comp->release();

  ASSERT_EQ(1, write_comp->is_complete());
  ASSERT_EQ(0, write_comp->get_return_value());
  write_comp->release();

  librbd::RBD::AioCompletion *discard_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_discard(128, 128, discard_comp));
  ASSERT_EQ(0, discard_comp->wait_for_complete());
  discard_comp->release();

  librbd::RBD::AioCompletion *read_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  bufferlist read_bl;
  image.aio_read(0, bl.length(), read_bl, read_comp);
  ASSERT_EQ(0, read_comp->wait_for_complete());
  ASSERT_EQ((ssize_t)bl.length(), read_comp->get_return_value());
  read_comp->release();

  bufferlist expected_bl;
  expected_bl.append(std::string(128, '1'));
  expected_bl.append(std::string(128, skip_discard ? '1' : '\0'));
  ASSERT_TRUE(expected_bl.contents_equal(read_bl));
}

TEST_F(TestLibRBD, ExclusiveLockTransition)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  std::list<librbd::RBD::AioCompletion *> comps;
  ceph::bufferlist bl;
  bl.append(std::string(1 << order, '1'));
  for (size_t object_no = 0; object_no < (size >> 12); ++object_no) {
    librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL,
                                                                      NULL);
    comps.push_back(comp);
    if (object_no % 2 == 0) {
      ASSERT_EQ(0, image1.aio_write(object_no << order, bl.length(), bl, comp));
    } else {
      ASSERT_EQ(0, image2.aio_write(object_no << order, bl.length(), bl, comp));
    }
  }

  while (!comps.empty()) {
    librbd::RBD::AioCompletion *comp = comps.front();
    comps.pop_front();
    ASSERT_EQ(0, comp->wait_for_complete());
    ASSERT_EQ(1, comp->is_complete());
    comp->release();
  }

  librbd::Image image3;
  ASSERT_EQ(0, rbd.open(ioctx, image3, name.c_str(), NULL));
  for (size_t object_no = 0; object_no < (size >> 12); ++object_no) {
    bufferlist read_bl;
    ASSERT_EQ((ssize_t)bl.length(), image3.read(object_no << order, bl.length(),
						read_bl));
    ASSERT_TRUE(bl.contents_equal(read_bl));
  }

  ASSERT_PASSED(validate_object_map, image1);
  ASSERT_PASSED(validate_object_map, image2);
  ASSERT_PASSED(validate_object_map, image3);
}

TEST_F(TestLibRBD, ExclusiveLockReadTransition)
{
  REQUIRE_FEATURE(RBD_FEATURE_JOURNALING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));

  bool lock_owner;
  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_FALSE(lock_owner);

  // journaling should force read ops to acquire the lock
  bufferlist read_bl;
  ASSERT_EQ(0, image1.read(0, 0, read_bl));

  ASSERT_EQ(0, image1.is_exclusive_lock_owner(&lock_owner));
  ASSERT_TRUE(lock_owner);

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));

  std::list<librbd::RBD::AioCompletion *> comps;
  std::list<bufferlist> read_bls;
  for (size_t object_no = 0; object_no < (size >> 12); ++object_no) {
    librbd::RBD::AioCompletion *comp = new librbd::RBD::AioCompletion(NULL,
                                                                      NULL);
    comps.push_back(comp);
    read_bls.emplace_back();
    if (object_no % 2 == 0) {
      ASSERT_EQ(0, image1.aio_read(object_no << order, 1 << order, read_bls.back(), comp));
    } else {
      ASSERT_EQ(0, image2.aio_read(object_no << order, 1 << order, read_bls.back(), comp));
    }
  }

  while (!comps.empty()) {
    librbd::RBD::AioCompletion *comp = comps.front();
    comps.pop_front();
    ASSERT_EQ(0, comp->wait_for_complete());
    ASSERT_EQ(1, comp->is_complete());
    comp->release();
  }
}

TEST_F(TestLibRBD, CacheMayCopyOnWrite) {
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));
  ASSERT_EQ(0, image.snap_create("one"));
  ASSERT_EQ(0, image.snap_protect("one"));

  std::string clone_name = this->get_temp_image_name();
  ASSERT_EQ(0, rbd.clone(ioctx, name.c_str(), "one", ioctx, clone_name.c_str(),
                         RBD_FEATURE_LAYERING, &order));

  librbd::Image clone;
  ASSERT_EQ(0, rbd.open(ioctx, clone, clone_name.c_str(), NULL));
  ASSERT_EQ(0, clone.flush());

  bufferlist expect_bl;
  expect_bl.append(std::string(1024, '\0'));

  // test double read path
  bufferlist read_bl;
  uint64_t offset = 0;
  ASSERT_EQ(1024, clone.read(offset + 2048, 1024, read_bl));
  ASSERT_TRUE(expect_bl.contents_equal(read_bl));

  bufferlist write_bl;
  write_bl.append(std::string(1024, '1'));
  ASSERT_EQ(1024, clone.write(offset, write_bl.length(), write_bl));

  read_bl.clear();
  ASSERT_EQ(1024, clone.read(offset + 2048, 1024, read_bl));
  ASSERT_TRUE(expect_bl.contents_equal(read_bl));

  // test read retry path
  offset = 1 << order;
  ASSERT_EQ(1024, clone.write(offset, write_bl.length(), write_bl));

  read_bl.clear();
  ASSERT_EQ(1024, clone.read(offset + 2048, 1024, read_bl));
  ASSERT_TRUE(expect_bl.contents_equal(read_bl));
}

TEST_F(TestLibRBD, FlushEmptyOpsOnExternalSnapshot) {
  std::string cache_enabled;
  ASSERT_EQ(0, _rados.conf_get("rbd_cache", cache_enabled));
  ASSERT_EQ(0, _rados.conf_set("rbd_cache", "false"));
  BOOST_SCOPE_EXIT( (cache_enabled) ) {
    ASSERT_EQ(0, _rados.conf_set("rbd_cache", cache_enabled.c_str()));
  } BOOST_SCOPE_EXIT_END;

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 18;
  int order = 0;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image1;
  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image1, name.c_str(), NULL));
  ASSERT_EQ(0, rbd.open(ioctx, image2, name.c_str(), NULL));
  ASSERT_EQ(0, image1.snap_create("snap1"));

  librbd::RBD::AioCompletion *read_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  bufferlist read_bl;
  image2.aio_read(0, 1024, read_bl, read_comp);
  ASSERT_EQ(0, read_comp->wait_for_complete());
  read_comp->release();
}

TEST_F(TestLibRBD, TestImageOptions)
{
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  //make create image options
  uint64_t features = RBD_FEATURE_LAYERING | RBD_FEATURE_STRIPINGV2 ;
  uint64_t order = 0;
  uint64_t stripe_unit = IMAGE_STRIPE_UNIT;
  uint64_t stripe_count = IMAGE_STRIPE_COUNT;
  rbd_image_options_t opts;
  rbd_image_options_create(&opts);

  bool is_set;
  ASSERT_EQ(-EINVAL, rbd_image_options_is_set(opts, 12345, &is_set));
  ASSERT_EQ(0, rbd_image_options_is_set(opts, RBD_IMAGE_OPTION_FORMAT,
                                        &is_set));
  ASSERT_FALSE(is_set);

  ASSERT_EQ(0, rbd_image_options_set_uint64(opts, RBD_IMAGE_OPTION_FORMAT,
	  2));
  ASSERT_EQ(0, rbd_image_options_set_uint64(opts, RBD_IMAGE_OPTION_FEATURES,
	  features));
  ASSERT_EQ(0, rbd_image_options_set_uint64(opts, RBD_IMAGE_OPTION_ORDER,
	  order));
  ASSERT_EQ(0, rbd_image_options_set_uint64(opts, RBD_IMAGE_OPTION_STRIPE_UNIT,
	  stripe_unit));
  ASSERT_EQ(0, rbd_image_options_set_uint64(opts, RBD_IMAGE_OPTION_STRIPE_COUNT,
	  stripe_count));

  ASSERT_EQ(0, rbd_image_options_is_set(opts, RBD_IMAGE_OPTION_FORMAT,
                                        &is_set));
  ASSERT_TRUE(is_set);

  std::string parent_name = get_temp_image_name();

  // make parent
  ASSERT_EQ(0, rbd_create4(ioctx, parent_name.c_str(), 4<<20, opts));

  // check order is returned in opts
  ASSERT_EQ(0, rbd_image_options_get_uint64(opts, RBD_IMAGE_OPTION_ORDER,
	  &order));
  ASSERT_NE((uint64_t)0, order);

  // write some data to parent
  rbd_image_t parent;
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, NULL));
  char *data = (char *)"testdata";
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(parent, 0, strlen(data), data));
  ASSERT_EQ((ssize_t)strlen(data), rbd_write(parent, 12, strlen(data), data));

  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, rbd_snap_create(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_close(parent));
  ASSERT_EQ(0, rbd_open(ioctx, parent_name.c_str(), &parent, "parent_snap"));

  // clone
  std::string child_name = get_temp_image_name();
  ASSERT_EQ(0, rbd_snap_protect(parent, "parent_snap"));
  ASSERT_EQ(0, rbd_clone3(ioctx, parent_name.c_str(), "parent_snap", ioctx,
	  child_name.c_str(), opts));

  // copy
  std::string copy1_name = get_temp_image_name();
  ASSERT_EQ(0, rbd_copy3(parent, ioctx, copy1_name.c_str(), opts));
  std::string copy2_name = get_temp_image_name();
  ASSERT_EQ(0, rbd_copy_with_progress3(parent, ioctx, copy2_name.c_str(), opts,
	  print_progress_percent, NULL));

  ASSERT_EQ(0, rbd_close(parent));

  rbd_image_options_destroy(opts);

  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, TestImageOptionsPP)
{
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  //make create image options
  uint64_t features = RBD_FEATURE_LAYERING | RBD_FEATURE_STRIPINGV2 ;
  uint64_t order = 0;
  uint64_t stripe_unit = IMAGE_STRIPE_UNIT;
  uint64_t stripe_count = IMAGE_STRIPE_COUNT;
  librbd::ImageOptions opts;
  ASSERT_EQ(0, opts.set(RBD_IMAGE_OPTION_FORMAT, static_cast<uint64_t>(2)));
  ASSERT_EQ(0, opts.set(RBD_IMAGE_OPTION_FEATURES, features));
  ASSERT_EQ(0, opts.set(RBD_IMAGE_OPTION_ORDER, order));
  ASSERT_EQ(0, opts.set(RBD_IMAGE_OPTION_STRIPE_UNIT, stripe_unit));
  ASSERT_EQ(0, opts.set(RBD_IMAGE_OPTION_STRIPE_COUNT, stripe_count));

  librbd::RBD rbd;
  std::string parent_name = get_temp_image_name();

  // make parent
  ASSERT_EQ(0, rbd.create4(ioctx, parent_name.c_str(), 4<<20, opts));

  // check order is returned in opts
  ASSERT_EQ(0, opts.get(RBD_IMAGE_OPTION_ORDER, &order));
  ASSERT_NE((uint64_t)0, order);

  // write some data to parent
  librbd::Image parent;
  ASSERT_EQ(0, rbd.open(ioctx, parent, parent_name.c_str(), NULL));

  ssize_t len = 1024;
  bufferlist bl;
  bl.append(buffer::create(len));
  bl.zero();
  ASSERT_EQ(len, parent.write(0, len, bl));
  ASSERT_EQ(len, parent.write(len, len, bl));

  // create a snapshot, reopen as the parent we're interested in
  ASSERT_EQ(0, parent.snap_create("parent_snap"));
  ASSERT_EQ(0, parent.close());
  ASSERT_EQ(0, rbd.open(ioctx, parent, parent_name.c_str(), "parent_snap"));

  // clone
  std::string child_name = get_temp_image_name();
  ASSERT_EQ(0, parent.snap_protect("parent_snap"));
  ASSERT_EQ(0, rbd.clone3(ioctx, parent_name.c_str(), "parent_snap", ioctx,
	  child_name.c_str(), opts));

  // copy
  std::string copy1_name = get_temp_image_name();
  ASSERT_EQ(0, parent.copy3(ioctx, copy1_name.c_str(), opts));
  std::string copy2_name = get_temp_image_name();
  PrintProgress pp;
  ASSERT_EQ(0, parent.copy_with_progress3(ioctx, copy2_name.c_str(), opts, pp));

  ASSERT_EQ(0, parent.close());
}

TEST_F(TestLibRBD, EventSocketPipe)
{
  EventSocket event_sock;
  int pipe_fd[2]; // read and write fd
  char buf[32];

  ASSERT_EQ(0, pipe(pipe_fd));

  ASSERT_FALSE(event_sock.is_valid());

  ASSERT_EQ(-EINVAL, event_sock.init(pipe_fd[1], EVENT_SOCKET_TYPE_NONE));
  ASSERT_FALSE(event_sock.is_valid());

  ASSERT_EQ(-EINVAL, event_sock.init(pipe_fd[1], 44));
  ASSERT_FALSE(event_sock.is_valid());

#ifndef HAVE_EVENTFD
  ASSERT_EQ(-EINVAL, event_sock.init(pipe_fd[1], EVENT_SOCKET_TYPE_EVENTFD));
  ASSERT_FALSE(event_sock.is_valid());
#endif

  ASSERT_EQ(0, event_sock.init(pipe_fd[1], EVENT_SOCKET_TYPE_PIPE));
  ASSERT_TRUE(event_sock.is_valid());
  ASSERT_EQ(0, event_sock.notify());
  ASSERT_EQ(1, read(pipe_fd[0], buf, 32));
  ASSERT_EQ('i', buf[0]);

  close(pipe_fd[0]);
  close(pipe_fd[1]);
}

TEST_F(TestLibRBD, EventSocketEventfd)
{
#ifdef HAVE_EVENTFD
  EventSocket event_sock;
  int event_fd;
  struct pollfd poll_fd;
  char buf[32];

  event_fd = eventfd(0, EFD_NONBLOCK);
  ASSERT_NE(-1, event_fd);

  ASSERT_FALSE(event_sock.is_valid());

  ASSERT_EQ(-EINVAL, event_sock.init(event_fd, EVENT_SOCKET_TYPE_NONE));
  ASSERT_FALSE(event_sock.is_valid());

  ASSERT_EQ(-EINVAL, event_sock.init(event_fd, 44));
  ASSERT_FALSE(event_sock.is_valid());

  ASSERT_EQ(0, event_sock.init(event_fd, EVENT_SOCKET_TYPE_EVENTFD));
  ASSERT_TRUE(event_sock.is_valid());
  ASSERT_EQ(0, event_sock.notify());

  poll_fd.fd = event_fd;
  poll_fd.events = POLLIN;
  ASSERT_EQ(1, poll(&poll_fd, 1, -1));
  ASSERT_TRUE(poll_fd.revents & POLLIN);

  ASSERT_EQ(static_cast<ssize_t>(sizeof(uint64_t)), read(event_fd, buf, 32));
  ASSERT_EQ(1U, *reinterpret_cast<uint64_t *>(buf));

  close(event_fd);
#endif
}

TEST_F(TestLibRBD, ImagePollIO)
{
#ifdef HAVE_EVENTFD
  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  rbd_image_t image;
  int order = 0;
  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int fd = eventfd(0, EFD_NONBLOCK);

  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));

  ASSERT_EQ(0, rbd_set_image_notification(image, fd, EVENT_SOCKET_TYPE_EVENTFD));

  char test_data[TEST_IO_SIZE + 1];
  char zero_data[TEST_IO_SIZE + 1];
  int i;

  for (i = 0; i < TEST_IO_SIZE; ++i)
    test_data[i] = (char) (rand() % (126 - 33) + 33);
  test_data[TEST_IO_SIZE] = '\0';
  memset(zero_data, 0, sizeof(zero_data));

  for (i = 0; i < 5; ++i)
    ASSERT_PASSED(write_test_data, image, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_write_test_data_and_poll, image, fd, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  for (i = 5; i < 10; ++i)
    ASSERT_PASSED(aio_read_test_data_and_poll, image, fd, test_data, TEST_IO_SIZE * i, TEST_IO_SIZE, 0);

  ASSERT_EQ(0, rbd_close(image));
  rados_ioctx_destroy(ioctx);
#endif
}

namespace librbd {

static bool operator==(const mirror_peer_t &lhs, const mirror_peer_t &rhs) {
  return (lhs.uuid == rhs.uuid &&
          lhs.cluster_name == rhs.cluster_name &&
          lhs.client_name == rhs.client_name);
}

static std::ostream& operator<<(std::ostream &os, const mirror_peer_t &peer) {
  os << "uuid=" << peer.uuid << ", "
     << "cluster=" << peer.cluster_name << ", "
     << "client=" << peer.client_name;
  return os;
}

} // namespace librbd

TEST_F(TestLibRBD, Mirror) {
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;

  std::vector<librbd::mirror_peer_t> expected_peers;
  std::vector<librbd::mirror_peer_t> peers;
  ASSERT_EQ(0, rbd.mirror_peer_list(ioctx, &peers));
  ASSERT_EQ(expected_peers, peers);

  std::string uuid1;
  ASSERT_EQ(-EINVAL, rbd.mirror_peer_add(ioctx, &uuid1, "cluster1", "client"));

  rbd_mirror_mode_t mirror_mode;
  ASSERT_EQ(0, rbd.mirror_mode_get(ioctx, &mirror_mode));
  ASSERT_EQ(RBD_MIRROR_MODE_DISABLED, mirror_mode);

  ASSERT_EQ(0, rbd.mirror_mode_set(ioctx, RBD_MIRROR_MODE_IMAGE));
  ASSERT_EQ(0, rbd.mirror_mode_get(ioctx, &mirror_mode));

  // Add some images to the pool
  int order = 0;
  std::string parent_name = get_temp_image_name();
  std::string child_name = get_temp_image_name();
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, parent_name.c_str(), 2 << 20,
                               &order));
  bool old_format;
  uint64_t features;
  ASSERT_EQ(0, get_features(&old_format, &features));
  if ((features & RBD_FEATURE_LAYERING) != 0) {
    librbd::Image parent;
    ASSERT_EQ(0, rbd.open(ioctx, parent, parent_name.c_str(), NULL));
    ASSERT_EQ(0, parent.snap_create("parent_snap"));
    ASSERT_EQ(0, parent.close());
    ASSERT_EQ(0, rbd.open(ioctx, parent, parent_name.c_str(), "parent_snap"));
    ASSERT_EQ(0, parent.snap_protect("parent_snap"));
    ASSERT_EQ(0, parent.close());
    ASSERT_EQ(0, rbd.clone(ioctx, parent_name.c_str(), "parent_snap", ioctx,
                           child_name.c_str(), features, &order));
  }

  ASSERT_EQ(RBD_MIRROR_MODE_IMAGE, mirror_mode);

  ASSERT_EQ(0, rbd.mirror_mode_set(ioctx, RBD_MIRROR_MODE_POOL));
  ASSERT_EQ(0, rbd.mirror_mode_get(ioctx, &mirror_mode));
  ASSERT_EQ(RBD_MIRROR_MODE_POOL, mirror_mode);

  std::string uuid2;
  std::string uuid3;
  ASSERT_EQ(0, rbd.mirror_peer_add(ioctx, &uuid1, "cluster1", "client"));
  ASSERT_EQ(0, rbd.mirror_peer_add(ioctx, &uuid2, "cluster2", "admin"));
  ASSERT_EQ(-EEXIST, rbd.mirror_peer_add(ioctx, &uuid3, "cluster1", "foo"));
  ASSERT_EQ(0, rbd.mirror_peer_add(ioctx, &uuid3, "cluster3", "admin"));

  ASSERT_EQ(0, rbd.mirror_peer_list(ioctx, &peers));
  auto sort_peers = [](const librbd::mirror_peer_t &lhs,
                         const librbd::mirror_peer_t &rhs) {
      return lhs.uuid < rhs.uuid;
    };
  expected_peers = {
    {uuid1, "cluster1", "client"},
    {uuid2, "cluster2", "admin"},
    {uuid3, "cluster3", "admin"}};
  std::sort(expected_peers.begin(), expected_peers.end(), sort_peers);
  ASSERT_EQ(expected_peers, peers);

  ASSERT_EQ(0, rbd.mirror_peer_remove(ioctx, "uuid4"));
  ASSERT_EQ(0, rbd.mirror_peer_remove(ioctx, uuid2));

  ASSERT_EQ(-ENOENT, rbd.mirror_peer_set_client(ioctx, "uuid4", "new client"));
  ASSERT_EQ(0, rbd.mirror_peer_set_client(ioctx, uuid1, "new client"));

  ASSERT_EQ(-ENOENT, rbd.mirror_peer_set_cluster(ioctx, "uuid4",
                                                 "new cluster"));
  ASSERT_EQ(0, rbd.mirror_peer_set_cluster(ioctx, uuid3, "new cluster"));

  ASSERT_EQ(0, rbd.mirror_peer_list(ioctx, &peers));
  expected_peers = {
    {uuid1, "cluster1", "new client"},
    {uuid3, "new cluster", "admin"}};
  std::sort(expected_peers.begin(), expected_peers.end(), sort_peers);
  ASSERT_EQ(expected_peers, peers);

  ASSERT_EQ(-EBUSY, rbd.mirror_mode_set(ioctx, RBD_MIRROR_MODE_DISABLED));
}

TEST_F(TestLibRBD, FlushCacheWithCopyupOnExternalSnapshot) {
  REQUIRE_FEATURE(RBD_FEATURE_LAYERING);

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  librbd::Image image;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 0;

  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  bufferlist bl;
  bl.append(std::string(size, '1'));
  ASSERT_EQ((int)size, image.write(0, size, bl));
  ASSERT_EQ(0, image.snap_create("one"));
  ASSERT_EQ(0, image.snap_protect("one"));

  std::string clone_name = this->get_temp_image_name();
  ASSERT_EQ(0, rbd.clone(ioctx, name.c_str(), "one", ioctx, clone_name.c_str(),
                         RBD_FEATURE_LAYERING, &order));
  ASSERT_EQ(0, rbd.open(ioctx, image, clone_name.c_str(), NULL));

  librbd::Image image2;
  ASSERT_EQ(0, rbd.open(ioctx, image2, clone_name.c_str(), NULL));

  // prepare CoW writeback that will be flushed on next op
  bl.clear();
  bl.append(std::string(1, '1'));
  ASSERT_EQ(0, image.flush());
  ASSERT_EQ(1, image.write(0, 1, bl));
  ASSERT_EQ(0, image2.snap_create("snap1"));

  librbd::RBD::AioCompletion *read_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  bufferlist read_bl;
  image.aio_read(0, 1024, read_bl, read_comp);
  ASSERT_EQ(0, read_comp->wait_for_complete());
  read_comp->release();
}

TEST_F(TestLibRBD, ExclusiveLock)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  static char buf[10];

  rados_ioctx_t ioctx;
  rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx);

  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));

  rbd_image_t image1;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image1, NULL));

  int lock_owner;
  ASSERT_EQ(0, rbd_lock_acquire(image1, RBD_LOCK_MODE_EXCLUSIVE));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image1, &lock_owner));
  ASSERT_TRUE(lock_owner);

  rbd_lock_mode_t lock_mode;
  char *lock_owners[1];
  size_t max_lock_owners = 0;
  ASSERT_EQ(-ERANGE, rbd_lock_get_owners(image1, &lock_mode, lock_owners,
                                         &max_lock_owners));
  ASSERT_EQ(1U, max_lock_owners);

  max_lock_owners = 2;
  ASSERT_EQ(0, rbd_lock_get_owners(image1, &lock_mode, lock_owners,
                                   &max_lock_owners));
  ASSERT_EQ(RBD_LOCK_MODE_EXCLUSIVE, lock_mode);
  ASSERT_STRNE("", lock_owners[0]);
  ASSERT_EQ(1U, max_lock_owners);

  rbd_image_t image2;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image2, NULL));

  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image2, &lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(-EOPNOTSUPP, rbd_lock_break(image1, RBD_LOCK_MODE_SHARED, ""));
  ASSERT_EQ(-EBUSY, rbd_lock_break(image1, RBD_LOCK_MODE_EXCLUSIVE,
                                   "not the owner"));

  ASSERT_EQ(0, rbd_lock_release(image1));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image1, &lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(-ENOENT, rbd_lock_break(image1, RBD_LOCK_MODE_EXCLUSIVE,
                                    lock_owners[0]));
  rbd_lock_get_owners_cleanup(lock_owners, max_lock_owners);

  ASSERT_EQ(-EROFS, rbd_write(image1, 0, sizeof(buf), buf));
  ASSERT_EQ((ssize_t)sizeof(buf), rbd_write(image2, 0, sizeof(buf), buf));

  ASSERT_EQ(0, rbd_lock_acquire(image2, RBD_LOCK_MODE_EXCLUSIVE));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image2, &lock_owner));
  ASSERT_TRUE(lock_owner);

  ASSERT_EQ(0, rbd_lock_release(image2));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image2, &lock_owner));
  ASSERT_FALSE(lock_owner);

  ASSERT_EQ(0, rbd_lock_acquire(image1, RBD_LOCK_MODE_EXCLUSIVE));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image1, &lock_owner));
  ASSERT_TRUE(lock_owner);

  ASSERT_EQ((ssize_t)sizeof(buf), rbd_write(image1, 0, sizeof(buf), buf));
  ASSERT_EQ(-EROFS, rbd_write(image2, 0, sizeof(buf), buf));

  ASSERT_EQ(0, rbd_lock_release(image1));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image1, &lock_owner));
  ASSERT_FALSE(lock_owner);

  int owner_id = -1;
  mutex lock;
  const auto pingpong = [&,this](int m_id, rbd_image_t &m_image) {
      for (int i = 0; i < 10; i++) {
	{
	  lock_guard<mutex> locker(lock);
	  if (owner_id == m_id) {
	    std::cout << m_id << ": releasing exclusive lock" << std::endl;
	    EXPECT_EQ(0, rbd_lock_release(m_image));
	    int lock_owner;
	    EXPECT_EQ(0, rbd_is_exclusive_lock_owner(m_image, &lock_owner));
	    EXPECT_FALSE(lock_owner);
	    owner_id = -1;
	    std::cout << m_id << ": exclusive lock released" << std::endl;
	    continue;
	  }
	}

	std::cout << m_id << ": acquiring exclusive lock" << std::endl;
        int r;
        do {
          r = rbd_lock_acquire(m_image, RBD_LOCK_MODE_EXCLUSIVE);
          if (r == -EROFS) {
            usleep(1000);
          }
        } while (r == -EROFS);
	EXPECT_EQ(0, r);

	int lock_owner;
	EXPECT_EQ(0, rbd_is_exclusive_lock_owner(m_image, &lock_owner));
	EXPECT_TRUE(lock_owner);
	std::cout << m_id << ": exclusive lock acquired" << std::endl;
	{
	  lock_guard<mutex> locker(lock);
	  owner_id = m_id;
	}
	usleep(rand() % 50000);
      }

      lock_guard<mutex> locker(lock);
      if (owner_id == m_id) {
	EXPECT_EQ(0, rbd_lock_release(m_image));
	int lock_owner;
	EXPECT_EQ(0, rbd_is_exclusive_lock_owner(m_image, &lock_owner));
	EXPECT_FALSE(lock_owner);
	owner_id = -1;
      }
  };
  thread ping(bind(pingpong, 1, ref(image1)));
  thread pong(bind(pingpong, 2, ref(image2)));

  ping.join();
  pong.join();

  ASSERT_EQ(0, rbd_lock_acquire(image2, RBD_LOCK_MODE_EXCLUSIVE));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image2, &lock_owner));
  ASSERT_TRUE(lock_owner);

  ASSERT_EQ(0, rbd_close(image2));

  ASSERT_EQ(0, rbd_lock_acquire(image1, RBD_LOCK_MODE_EXCLUSIVE));
  ASSERT_EQ(0, rbd_is_exclusive_lock_owner(image1, &lock_owner));
  ASSERT_TRUE(lock_owner);

  ASSERT_EQ(0, rbd_close(image1));
  rados_ioctx_destroy(ioctx);
}

TEST_F(TestLibRBD, BreakLock)
{
  REQUIRE_FEATURE(RBD_FEATURE_EXCLUSIVE_LOCK);

  static char buf[10];

  rados_t blacklist_cluster;
  ASSERT_EQ("", connect_cluster(&blacklist_cluster));

  rados_ioctx_t ioctx, blacklist_ioctx;
  ASSERT_EQ(0, rados_ioctx_create(_cluster, m_pool_name.c_str(), &ioctx));
  ASSERT_EQ(0, rados_ioctx_create(blacklist_cluster, m_pool_name.c_str(),
                                  &blacklist_ioctx));

  std::string name = get_temp_image_name();
  uint64_t size = 2 << 20;
  int order = 0;
  ASSERT_EQ(0, create_image(ioctx, name.c_str(), size, &order));

  rbd_image_t image, blacklist_image;
  ASSERT_EQ(0, rbd_open(ioctx, name.c_str(), &image, NULL));
  ASSERT_EQ(0, rbd_open(blacklist_ioctx, name.c_str(), &blacklist_image, NULL));

  ASSERT_EQ(0, rbd_metadata_set(image, "rbd_blacklist_on_break_lock", "true"));
  ASSERT_EQ(0, rbd_lock_acquire(blacklist_image, RBD_LOCK_MODE_EXCLUSIVE));

  rbd_lock_mode_t lock_mode;
  char *lock_owners[1];
  size_t max_lock_owners = 1;
  ASSERT_EQ(0, rbd_lock_get_owners(image, &lock_mode, lock_owners,
                                   &max_lock_owners));
  ASSERT_EQ(RBD_LOCK_MODE_EXCLUSIVE, lock_mode);
  ASSERT_STRNE("", lock_owners[0]);
  ASSERT_EQ(1U, max_lock_owners);

  ASSERT_EQ(0, rbd_lock_break(image, RBD_LOCK_MODE_EXCLUSIVE, lock_owners[0]));
  ASSERT_EQ(0, rbd_lock_acquire(image, RBD_LOCK_MODE_EXCLUSIVE));
  EXPECT_EQ(0, rados_wait_for_latest_osdmap(blacklist_cluster));

  ASSERT_EQ((ssize_t)sizeof(buf), rbd_write(image, 0, sizeof(buf), buf));
  ASSERT_EQ(-EBLACKLISTED, rbd_write(blacklist_image, 0, sizeof(buf), buf));

  ASSERT_EQ(0, rbd_close(image));
  ASSERT_EQ(0, rbd_close(blacklist_image));

  rbd_lock_get_owners_cleanup(lock_owners, max_lock_owners);

  rados_ioctx_destroy(ioctx);
  rados_ioctx_destroy(blacklist_ioctx);
  rados_shutdown(blacklist_cluster);
}

TEST_F(TestLibRBD, DiscardAfterWrite)
{
  REQUIRE(!is_skip_partial_discard_enabled());

  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();
  uint64_t size = 1 << 20;
  int order = 18;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), NULL));

  // enable writeback cache
  ASSERT_EQ(0, image.flush());

  bufferlist bl;
  bl.append(std::string(256, '1'));

  librbd::RBD::AioCompletion *write_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_write(0, bl.length(), bl, write_comp));
  ASSERT_EQ(0, write_comp->wait_for_complete());
  write_comp->release();

  librbd::RBD::AioCompletion *discard_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  ASSERT_EQ(0, image.aio_discard(0, 256, discard_comp));
  ASSERT_EQ(0, discard_comp->wait_for_complete());
  discard_comp->release();

  librbd::RBD::AioCompletion *read_comp =
    new librbd::RBD::AioCompletion(NULL, NULL);
  bufferlist read_bl;
  image.aio_read(0, bl.length(), read_bl, read_comp);
  ASSERT_EQ(0, read_comp->wait_for_complete());
  ASSERT_EQ(bl.length(), read_comp->get_return_value());
  ASSERT_TRUE(read_bl.is_zero());
  read_comp->release();
}

TEST_F(TestLibRBD, DefaultFeatures) {
  std::string orig_default_features;
  ASSERT_EQ(0, _rados.conf_get("rbd_default_features", orig_default_features));
  BOOST_SCOPE_EXIT_ALL(orig_default_features) {
    ASSERT_EQ(0, _rados.conf_set("rbd_default_features",
                                 orig_default_features.c_str()));
  };

  std::list<std::pair<std::string, std::string> > feature_names_to_bitmask = {
    {"", orig_default_features},
    {"layering", "1"},
    {"layering, exclusive-lock", "5"},
    {"exclusive-lock,journaling", "68"},
    {"125", "125"}
  };

  for (auto &pair : feature_names_to_bitmask) {
    ASSERT_EQ(0, _rados.conf_set("rbd_default_features", pair.first.c_str()));
    std::string features;
    ASSERT_EQ(0, _rados.conf_get("rbd_default_features", features));
    ASSERT_EQ(pair.second, features);
  }
}

TEST_F(TestLibRBD, TestTrashMoveAndPurge) {
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), nullptr));
  uint8_t old_format;
  ASSERT_EQ(0, image.old_format(&old_format));

  if (old_format) {
    ASSERT_EQ(-EOPNOTSUPP, rbd.trash_move(ioctx, name.c_str(), 0));
    image.close();
    return;
  }
  std::string image_id;
  ASSERT_EQ(0, image.get_id(&image_id));
  image.close();

  ASSERT_EQ(0, rbd.trash_move(ioctx, name.c_str(), 0));

  std::vector<std::string> images;
  ASSERT_EQ(0, rbd.list(ioctx, images));
  for (const auto& image : images) {
    ASSERT_TRUE(image != name);
  }

  librbd::trash_image_info_t info;
  ASSERT_EQ(-ENOENT, rbd.trash_get(ioctx, "dummy image id", &info));
  ASSERT_EQ(0, rbd.trash_get(ioctx, image_id.c_str(), &info));
  ASSERT_EQ(image_id, info.id);

  std::vector<librbd::trash_image_info_t> entries;
  ASSERT_EQ(0, rbd.trash_list(ioctx, entries));
  ASSERT_FALSE(entries.empty());
  ASSERT_EQ(entries.begin()->id, image_id);

  entries.clear();
  PrintProgress pp;
  ASSERT_EQ(0, rbd.trash_remove_with_progress(ioctx, image_id.c_str(),
                                              false, pp));
  ASSERT_EQ(0, rbd.trash_list(ioctx, entries));
  ASSERT_TRUE(entries.empty());
}

TEST_F(TestLibRBD, TestTrashMoveAndPurgeNonExpiredDelay) {
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), nullptr));
  uint8_t old_format;
  ASSERT_EQ(0, image.old_format(&old_format));

  if (old_format) {
    ASSERT_EQ(-EOPNOTSUPP, rbd.trash_move(ioctx, name.c_str(), 0));
    image.close();
    return;
  }
  std::string image_id;
  ASSERT_EQ(0, image.get_id(&image_id));
  image.close();

  ASSERT_EQ(0, rbd.trash_move(ioctx, name.c_str(), 100));

  PrintProgress pp;
  ASSERT_EQ(-EPERM, rbd.trash_remove_with_progress(ioctx, image_id.c_str(),
                                                   false, pp));

  PrintProgress pp2;
  ASSERT_EQ(0, rbd.trash_remove_with_progress(ioctx, image_id.c_str(),
                                              true, pp2));
}

TEST_F(TestLibRBD, TestTrashMoveAndRestore) {
  librados::IoCtx ioctx;
  ASSERT_EQ(0, _rados.ioctx_create(m_pool_name.c_str(), ioctx));

  librbd::RBD rbd;
  std::string name = get_temp_image_name();

  uint64_t size = 1 << 18;
  int order = 12;
  ASSERT_EQ(0, create_image_pp(rbd, ioctx, name.c_str(), size, &order));

  librbd::Image image;
  ASSERT_EQ(0, rbd.open(ioctx, image, name.c_str(), nullptr));
  uint8_t old_format;
  ASSERT_EQ(0, image.old_format(&old_format));

  if (old_format) {
    ASSERT_EQ(-EOPNOTSUPP, rbd.trash_move(ioctx, name.c_str(), 0));
    image.close();
    return;
  }
  std::string image_id;
  ASSERT_EQ(0, image.get_id(&image_id));
  image.close();

  ASSERT_EQ(0, rbd.trash_move(ioctx, name.c_str(), 10));

  std::vector<std::string> images;
  ASSERT_EQ(0, rbd.list(ioctx, images));
  for (const auto& image : images) {
    ASSERT_TRUE(image != name);
  }

  std::vector<librbd::trash_image_info_t> entries;
  ASSERT_EQ(0, rbd.trash_list(ioctx, entries));
  ASSERT_FALSE(entries.empty());
  ASSERT_EQ(entries.begin()->id, image_id);

  images.clear();
  ASSERT_EQ(0, rbd.trash_restore(ioctx, image_id.c_str(), ""));
  ASSERT_EQ(0, rbd.list(ioctx, images));
  ASSERT_FALSE(images.empty());
  bool found = false;
  for (const auto& image : images) {
    if (image == name) {
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
}

// poorman's assert()
namespace ceph {
  void __ceph_assert_fail(const char *assertion, const char *file, int line,
			  const char *func) {
    assert(false);
  }
}
