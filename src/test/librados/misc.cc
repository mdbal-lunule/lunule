// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "gtest/gtest.h"

#include "mds/mdstypes.h"
#include "include/err.h"
#include "include/buffer.h"
#include "include/rbd_types.h"
#include "include/rados/librados.h"
#include "include/rados/librados.hpp"
#include "include/stringify.h"
#include "common/Checksummer.h"
#include "global/global_context.h"
#include "test/librados/test.h"
#include "test/librados/TestCase.h"
#include "gtest/gtest.h"

#include <errno.h>
#include <map>
#include <sstream>
#include <string>
#include <boost/regex.hpp>

using namespace librados;
using std::map;
using std::ostringstream;
using std::string;

typedef RadosTest LibRadosMisc;
typedef RadosTestPP LibRadosMiscPP;
typedef RadosTestECPP LibRadosMiscECPP;

TEST(LibRadosMiscVersion, Version) {
  int major, minor, extra;
  rados_version(&major, &minor, &extra);
}

TEST(LibRadosMiscVersion, VersionPP) {
  int major, minor, extra;
  Rados::version(&major, &minor, &extra);
}

static void test_rados_log_cb(void *arg,
                              const char *line,
                              const char *who,
                              uint64_t sec, uint64_t nsec,
                              uint64_t seq, const char *level,
                              const char *msg)
{
    std::cerr << "monitor log callback invoked" << std::endl;
}

TEST(LibRadosMiscConnectFailure, ConnectFailure) {
  rados_t cluster;

  char *id = getenv("CEPH_CLIENT_ID");
  if (id)
    std::cerr << "Client id is: " << id << std::endl;

  ASSERT_EQ(0, rados_create(&cluster, NULL));
  ASSERT_EQ(0, rados_conf_read_file(cluster, NULL));
  ASSERT_EQ(0, rados_conf_parse_env(cluster, NULL));

  ASSERT_EQ(0, rados_conf_set(cluster, "client_mount_timeout", "0.000000001"));
  ASSERT_EQ(0, rados_conf_set(cluster, "debug_monc", "20"));
  ASSERT_EQ(0, rados_conf_set(cluster, "debug_ms", "1"));
  ASSERT_EQ(0, rados_conf_set(cluster, "log_to_stderr", "true"));

  ASSERT_EQ(-ENOTCONN, rados_monitor_log(cluster, "error",
                                         test_rados_log_cb, NULL));

  // try this a few times; sometimes we don't schedule fast enough for the
  // cond to time out
  int r;
  for (unsigned i=0; i<16; ++i) {
    cout << i << std::endl;
    r = rados_connect(cluster);
    if (r < 0)
      break;  // yay, we timed out
    // try again
    rados_shutdown(cluster);
    ASSERT_EQ(0, rados_create(&cluster, NULL));
  }
  ASSERT_NE(0, r);

  rados_shutdown(cluster);
}

TEST(LibRadosMiscPool, PoolCreationRace) {
  rados_t cluster_a, cluster_b;

  char *id = getenv("CEPH_CLIENT_ID");
  if (id)
    std::cerr << "Client id is: " << id << std::endl;

  ASSERT_EQ(0, rados_create(&cluster_a, NULL));
  ASSERT_EQ(0, rados_conf_read_file(cluster_a, NULL));
  // kludge: i want to --log-file foo and only get cluster b
  //ASSERT_EQ(0, rados_conf_parse_env(cluster_a, NULL));
  ASSERT_EQ(0, rados_connect(cluster_a));

  ASSERT_EQ(0, rados_create(&cluster_b, NULL));
  ASSERT_EQ(0, rados_conf_read_file(cluster_b, NULL));
  ASSERT_EQ(0, rados_conf_parse_env(cluster_b, NULL));
  ASSERT_EQ(0, rados_conf_set(cluster_b,
			      "objecter_debug_inject_relock_delay", "true"));
  ASSERT_EQ(0, rados_connect(cluster_b));

  char poolname[80];
  snprintf(poolname, sizeof(poolname), "poolrace.%d", rand());
  rados_pool_create(cluster_a, poolname);
  rados_ioctx_t a, b;
  rados_ioctx_create(cluster_a, poolname, &a);
  int64_t poolid = rados_ioctx_get_id(a);

  rados_ioctx_create2(cluster_b, poolid+1, &b);

  char pool2name[80];
  snprintf(pool2name, sizeof(pool2name), "poolrace2.%d", rand());
  rados_pool_create(cluster_a, pool2name);

  list<rados_completion_t> cls;
  // this should normally trigger pretty easily, but we need to bound
  // the requests because if we get too many we'll get stuck by always
  // sending enough messages that we hit the socket failure injection.
  int max = 512;
  while (max--) {
    char buf[100];
    rados_completion_t c;
    rados_aio_create_completion(0, 0, 0, &c);
    cls.push_back(c);
    rados_aio_read(b, "PoolCreationRaceObj", c, buf, 100, 0);
    cout << "started " << (void*)c << std::endl;
    if (rados_aio_is_complete(cls.front())) {
      break;
    }
  }
  while (!rados_aio_is_complete(cls.front())) {
    cout << "waiting 1 sec" << std::endl;
    sleep(1);
  }

  cout << " started " << cls.size() << " aios" << std::endl;
  for (auto c : cls) {
    cout << "waiting " << (void*)c << std::endl;
    rados_aio_wait_for_complete_and_cb(c);
    rados_aio_release(c);
  }
  cout << "done." << std::endl;

  rados_ioctx_destroy(a);
  rados_ioctx_destroy(b);
  rados_pool_delete(cluster_a, poolname);
  rados_pool_delete(cluster_a, pool2name);
  rados_shutdown(cluster_b);
  rados_shutdown(cluster_a);
}

TEST_F(LibRadosMisc, ClusterFSID) {
  char fsid[37];
  ASSERT_EQ(-ERANGE, rados_cluster_fsid(cluster, fsid, sizeof(fsid) - 1));
  ASSERT_EQ(sizeof(fsid) - 1,
            (size_t)rados_cluster_fsid(cluster, fsid, sizeof(fsid)));
}

TEST_F(LibRadosMiscPP, WaitOSDMapPP) {
  ASSERT_EQ(0, cluster.wait_for_latest_osdmap());
}

TEST_F(LibRadosMiscPP, LongNamePP) {
  bufferlist bl;
  bl.append("content");
  int maxlen = g_conf->osd_max_object_name_len;
  ASSERT_EQ(0, ioctx.write(string(maxlen/2, 'a').c_str(), bl, bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(string(maxlen-1, 'a').c_str(), bl, bl.length(), 0));
  ASSERT_EQ(0, ioctx.write(string(maxlen, 'a').c_str(), bl, bl.length(), 0));
  ASSERT_EQ(-ENAMETOOLONG, ioctx.write(string(maxlen+1, 'a').c_str(), bl, bl.length(), 0));
  ASSERT_EQ(-ENAMETOOLONG, ioctx.write(string(maxlen*2, 'a').c_str(), bl, bl.length(), 0));
}

TEST_F(LibRadosMiscPP, LongLocatorPP) {
  bufferlist bl;
  bl.append("content");
  int maxlen = g_conf->osd_max_object_name_len;
  ioctx.locator_set_key(
    string((maxlen/2), 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.locator_set_key(
    string(maxlen - 1, 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.locator_set_key(
    string(maxlen, 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.locator_set_key(
    string(maxlen+1, 'a'));
  ASSERT_EQ(
    -ENAMETOOLONG,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.locator_set_key(
    string((maxlen*2), 'a'));
  ASSERT_EQ(
    -ENAMETOOLONG,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
}

TEST_F(LibRadosMiscPP, LongNSpacePP) {
  bufferlist bl;
  bl.append("content");
  int maxlen = g_conf->osd_max_object_namespace_len;
  ioctx.set_namespace(
    string((maxlen/2), 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.set_namespace(
    string(maxlen - 1, 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.set_namespace(
    string(maxlen, 'a'));
  ASSERT_EQ(
    0,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.set_namespace(
    string(maxlen+1, 'a'));
  ASSERT_EQ(
    -ENAMETOOLONG,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
  ioctx.set_namespace(
    string((maxlen*2), 'a'));
  ASSERT_EQ(
    -ENAMETOOLONG,
    ioctx.write(
      string("a").c_str(),
      bl, bl.length(), 0));
}

TEST_F(LibRadosMiscPP, LongAttrNamePP) {
  bufferlist bl;
  bl.append("content");
  int maxlen = g_conf->osd_max_attr_name_len;
  ASSERT_EQ(0, ioctx.setxattr("bigattrobj", string(maxlen/2, 'a').c_str(), bl));
  ASSERT_EQ(0, ioctx.setxattr("bigattrobj", string(maxlen-1, 'a').c_str(), bl));
  ASSERT_EQ(0, ioctx.setxattr("bigattrobj", string(maxlen, 'a').c_str(), bl));
  ASSERT_EQ(-ENAMETOOLONG, ioctx.setxattr("bigattrobj", string(maxlen+1, 'a').c_str(), bl));
  ASSERT_EQ(-ENAMETOOLONG, ioctx.setxattr("bigattrobj", string(maxlen*2, 'a').c_str(), bl));
}

static std::string read_key_from_tmap(IoCtx& ioctx, const std::string &obj,
				      const std::string &key)
{
  bufferlist bl;
  int r = ioctx.read(obj, bl, 0, 0);
  if (r <= 0) {
    ostringstream oss;
    oss << "ioctx.read(" << obj << ", bl, 0, 0) returned " << r;
    return oss.str();
  }
  bufferlist::iterator p = bl.begin();
  bufferlist header;
  map<string, bufferlist> m;
  ::decode(header, p);
  ::decode(m, p);
  map<string, bufferlist>::iterator i = m.find(key);
  if (i == m.end())
    return "";
  std::string retstring;
  ::decode(retstring, i->second);
  return retstring;
}

static std::string add_key_to_tmap(IoCtx &ioctx, const std::string &obj,
	  const std::string &key, const std::string &val)
{
  __u8 c = CEPH_OSD_TMAP_SET;

  bufferlist tmbl;
  ::encode(c, tmbl);
  ::encode(key, tmbl);
  bufferlist blbl;
  ::encode(val, blbl);
  ::encode(blbl, tmbl);
  int ret = ioctx.tmap_update(obj, tmbl);
  if (ret) {
    ostringstream oss;
    oss << "ioctx.tmap_update(obj=" << obj << ", key="
	<< key << ", val=" << val << ") failed with error " << ret;
    return oss.str();
  }
  return "";
}

static int remove_key_from_tmap(IoCtx &ioctx, const std::string &obj,
					const std::string &key)
{
  __u8 c = CEPH_OSD_TMAP_RM;

  bufferlist tmbl;
  ::encode(c, tmbl);
  ::encode(key, tmbl);
  int ret = ioctx.tmap_update(obj, tmbl);
  if (ret) {
    ostringstream oss;
    oss << "ioctx.tmap_update(obj=" << obj << ", key="
	<< key << ") failed with error " << ret;
  }
  return ret;
}

TEST_F(LibRadosMiscPP, TmapUpdatePP) {
  // create tmap
  {
    __u8 c = CEPH_OSD_TMAP_CREATE;
    std::string my_tmap("my_tmap");
    bufferlist emptybl;

    bufferlist tmbl;
    ::encode(c, tmbl);
    ::encode(my_tmap, tmbl);
    ::encode(emptybl, tmbl);
    ASSERT_EQ(0, ioctx.tmap_update("foo", tmbl));
  }

  ASSERT_EQ(string(""), add_key_to_tmap(ioctx, "foo", "key1", "val1"));

  ASSERT_EQ(string(""), add_key_to_tmap(ioctx, "foo", "key2", "val2"));

  // read key1 from the tmap
  ASSERT_EQ(string("val1"), read_key_from_tmap(ioctx, "foo", "key1"));

  // remove key1 from tmap
  ASSERT_EQ(0, remove_key_from_tmap(ioctx, "foo", "key1"));
  ASSERT_EQ(-ENOENT, remove_key_from_tmap(ioctx, "foo", "key1"));

  // key should be removed
  ASSERT_EQ(string(""), read_key_from_tmap(ioctx, "foo", "key1"));
}

TEST_F(LibRadosMiscPP, TmapUpdateMisorderedPP) {
  // create tmap
  {
    __u8 c = CEPH_OSD_TMAP_CREATE;
    std::string my_tmap("my_tmap");
    bufferlist emptybl;

    bufferlist tmbl;
    ::encode(c, tmbl);
    ::encode(my_tmap, tmbl);
    ::encode(emptybl, tmbl);
    ASSERT_EQ(0, ioctx.tmap_update("foo", tmbl));
  }

  // good update
  {
    __u8 c = CEPH_OSD_TMAP_SET;
    bufferlist tmbl;
    ::encode(c, tmbl);
    ::encode("a", tmbl);
    bufferlist blbl;
    ::encode("old", blbl);
    ::encode(blbl, tmbl);

    ::encode(c, tmbl);
    ::encode("b", tmbl);
    ::encode(blbl, tmbl);

    ::encode(c, tmbl);
    ::encode("c", tmbl);
    ::encode(blbl, tmbl);

    ASSERT_EQ(0, ioctx.tmap_update("foo", tmbl));
  }

  // bad update
  {
    __u8 c = CEPH_OSD_TMAP_SET;
    bufferlist tmbl;
    ::encode(c, tmbl);
    ::encode("b", tmbl);
    bufferlist blbl;
    ::encode("new", blbl);
    ::encode(blbl, tmbl);

    ::encode(c, tmbl);
    ::encode("a", tmbl);
    ::encode(blbl, tmbl);

    ::encode(c, tmbl);
    ::encode("c", tmbl);
    ::encode(blbl, tmbl);

    ASSERT_EQ(0, ioctx.tmap_update("foo", tmbl));
  }

  // check
  ASSERT_EQ(string("new"), read_key_from_tmap(ioctx, "foo", "a"));
  ASSERT_EQ(string("new"), read_key_from_tmap(ioctx, "foo", "b"));
  ASSERT_EQ(string("new"), read_key_from_tmap(ioctx, "foo", "c"));

  ASSERT_EQ(0, remove_key_from_tmap(ioctx, "foo", "a"));
  ASSERT_EQ(string(""), read_key_from_tmap(ioctx, "foo", "a"));

  ASSERT_EQ(0, remove_key_from_tmap(ioctx, "foo", "b"));
  ASSERT_EQ(string(""), read_key_from_tmap(ioctx, "foo", "a"));
}

TEST_F(LibRadosMiscPP, TmapUpdateMisorderedPutPP) {
  // create unsorted tmap
  string h("header");
  bufferlist bl;
  ::encode(h, bl);
  uint32_t n = 3;
  ::encode(n, bl);
  ::encode(string("b"), bl);
  ::encode(string("bval"), bl);
  ::encode(string("a"), bl);
  ::encode(string("aval"), bl);
  ::encode(string("c"), bl);
  ::encode(string("cval"), bl);
  bufferlist orig = bl;  // tmap_put steals bl content
  ASSERT_EQ(0, ioctx.tmap_put("foo", bl));

  // check
  bufferlist newbl;
  ioctx.read("foo", newbl, orig.length(), 0);
  ASSERT_EQ(orig.contents_equal(newbl), false);
}

TEST_F(LibRadosMiscPP, Tmap2OmapPP) {
  // create tmap
  bufferlist hdr;
  hdr.append("header");
  map<string, bufferlist> omap;
  omap["1"].append("a");
  omap["2"].append("b");
  omap["3"].append("c");
  {
    bufferlist bl;
    ::encode(hdr, bl);
    ::encode(omap, bl);
    ASSERT_EQ(0, ioctx.tmap_put("foo", bl));
  }

  // convert tmap to omap
  ASSERT_EQ(0, ioctx.tmap_to_omap("foo", false));

  // if tmap was truncated ?
  {
    uint64_t size;
    time_t mtime;
    ASSERT_EQ(0, ioctx.stat("foo", &size, &mtime));
    ASSERT_EQ(0U, size);
  }

  // if 'nullok' works
  ASSERT_EQ(0, ioctx.tmap_to_omap("foo", true));
  ASSERT_LE(ioctx.tmap_to_omap("foo", false), 0);

  {
    // read omap
    bufferlist got;
    map<string, bufferlist> m;
    ObjectReadOperation o;
    o.omap_get_header(&got, NULL);
    o.omap_get_vals2("", 1024, &m, nullptr, nullptr);
    ASSERT_EQ(0, ioctx.operate("foo", &o, NULL));

    // compare header
    ASSERT_TRUE(hdr.contents_equal(got));

    // compare values
    ASSERT_EQ(omap.size(), m.size());
    bool same = true;
    for (map<string, bufferlist>::iterator p = omap.begin(); p != omap.end(); ++p) {
      map<string, bufferlist>::iterator q = m.find(p->first);
      if (q == m.end() || !p->second.contents_equal(q->second)) {
	same = false;
	break;
      }
    }
    ASSERT_TRUE(same);
  }
}

TEST_F(LibRadosMisc, Exec) {
  char buf[128];
  memset(buf, 0xcc, sizeof(buf));
  ASSERT_EQ(0, rados_write(ioctx, "foo", buf, sizeof(buf), 0));
  char buf2[512];
  int res = rados_exec(ioctx, "foo", "rbd", "get_all_features",
			  NULL, 0, buf2, sizeof(buf2));
  ASSERT_GT(res, 0);
  bufferlist bl;
  bl.append(buf2, res);
  bufferlist::iterator iter = bl.begin();
  uint64_t all_features;
  ::decode(all_features, iter);
  // make sure *some* features are specified; don't care which ones
  ASSERT_NE(all_features, (unsigned)0);
}

TEST_F(LibRadosMiscPP, ExecPP) {
  bufferlist bl;
  ASSERT_EQ(0, ioctx.write("foo", bl, 0, 0));
  bufferlist bl2, out;
  int r = ioctx.exec("foo", "rbd", "get_all_features", bl2, out);
  ASSERT_EQ(0, r);
  bufferlist::iterator iter = out.begin();
  uint64_t all_features;
  ::decode(all_features, iter);
  // make sure *some* features are specified; don't care which ones
  ASSERT_NE(all_features, (unsigned)0);
}

void set_completion_complete(rados_completion_t cb, void *arg)
{
  bool *my_aio_complete = (bool*)arg;
  *my_aio_complete = true;
}

TEST_F(LibRadosMiscPP, BadFlagsPP) {
  unsigned badflags = CEPH_OSD_FLAG_PARALLELEXEC;
  {
    bufferlist bl;
    bl.append("data");
    ASSERT_EQ(0, ioctx.write("badfoo", bl, bl.length(), 0));
  }
  {
    ASSERT_EQ(-EINVAL, ioctx.remove("badfoo", badflags));
  }
}

TEST_F(LibRadosMiscPP, Operate1PP) {
  ObjectWriteOperation o;
  {
    bufferlist bl;
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
    o.omap_clear(); // shouldn't affect attrs!
  }
  ASSERT_EQ(0, ioctx.operate("foo", &o));

  ObjectWriteOperation empty;
  ASSERT_EQ(0, ioctx.operate("foo", &empty));

  {
    bufferlist bl;
    ASSERT_GT(ioctx.getxattr("foo", "key1", bl), 0);
    ASSERT_EQ(0, strcmp(bl.c_str(), val1.c_str()));
  }
  ObjectWriteOperation o2;
  {
    bufferlist bl;
    bl.append(val1);
    o2.cmpxattr("key1", CEPH_OSD_CMPXATTR_OP_EQ, bl);
    o2.rmxattr("key1");
  }
  ASSERT_EQ(-ECANCELED, ioctx.operate("foo", &o2));
  ObjectWriteOperation o3;
  {
    bufferlist bl;
    bl.append(val1);
    o3.cmpxattr("key1", CEPH_OSD_CMPXATTR_OP_EQ, bl);
  }
  ASSERT_EQ(-ECANCELED, ioctx.operate("foo", &o3));
}

TEST_F(LibRadosMiscPP, Operate2PP) {
  ObjectWriteOperation o;
  {
    bufferlist bl;
    bl.append("abcdefg");
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
    o.truncate(0);
  }
  ASSERT_EQ(0, ioctx.operate("foo", &o));
  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat("foo", &size, &mtime));
  ASSERT_EQ(0U, size);
}

TEST_F(LibRadosMiscPP, BigObjectPP) {
  bufferlist bl;
  bl.append("abcdefg");
  ASSERT_EQ(0, ioctx.write("foo", bl, bl.length(), 0));

  {
    ObjectWriteOperation o;
    o.truncate(500000000000ull);
    ASSERT_EQ(-EFBIG, ioctx.operate("foo", &o));
  }
  {
    ObjectWriteOperation o;
    o.zero(500000000000ull, 1);
    ASSERT_EQ(-EFBIG, ioctx.operate("foo", &o));
  }
  {
    ObjectWriteOperation o;
    o.zero(1, 500000000000ull);
    ASSERT_EQ(-EFBIG, ioctx.operate("foo", &o));
  }
  {
    ObjectWriteOperation o;
    o.zero(500000000000ull, 500000000000ull);
    ASSERT_EQ(-EFBIG, ioctx.operate("foo", &o));
  }

#ifdef __LP64__
  // this test only works on 64-bit platforms
  ASSERT_EQ(-EFBIG, ioctx.write("foo", bl, bl.length(), 500000000000ull));
#endif
}

TEST_F(LibRadosMiscPP, AioOperatePP) {
  bool my_aio_complete = false;
  AioCompletion *my_completion = cluster.aio_create_completion(
	  (void*)&my_aio_complete, set_completion_complete, NULL);
  AioCompletion *my_completion_null = NULL;
  ASSERT_NE(my_completion, my_completion_null);

  ObjectWriteOperation o;
  {
    bufferlist bl;
    o.write(0, bl);
  }
  std::string val1("val1");
  {
    bufferlist bl;
    bl.append(val1.c_str(), val1.size() + 1);
    o.setxattr("key1", bl);
    bufferlist bl2;
    char buf2[1024];
    memset(buf2, 0xdd, sizeof(buf2));
    bl2.append(buf2, sizeof(buf2));
    o.append(bl2);
  }
  ASSERT_EQ(0, ioctx.aio_operate("foo", my_completion, &o));
  ASSERT_EQ(0, my_completion->wait_for_complete_and_cb());
  ASSERT_EQ(my_aio_complete, true);
  my_completion->release();

  uint64_t size;
  time_t mtime;
  ASSERT_EQ(0, ioctx.stat("foo", &size, &mtime));
  ASSERT_EQ(1024U, size);
}

TEST_F(LibRadosMiscPP, AssertExistsPP) {
  char buf[64];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl;
  bl.append(buf, sizeof(buf));

  ObjectWriteOperation op;
  op.assert_exists();
  op.write(0, bl);
  ASSERT_EQ(-ENOENT, ioctx.operate("asdffoo", &op));
  ASSERT_EQ(0, ioctx.create("asdffoo", true));
  ASSERT_EQ(0, ioctx.operate("asdffoo", &op));
  ASSERT_EQ(-EEXIST, ioctx.create("asdffoo", true));
}

TEST_F(LibRadosMiscPP, AssertVersionPP) {
  char buf[64];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl;
  bl.append(buf, sizeof(buf));

  // Create test object...
  ASSERT_EQ(0, ioctx.create("asdfbar", true));
  // ...then write it again to guarantee that the
  // (unsigned) version must be at least 1 (not 0)
  // since we want to decrement it by 1 later.
  ASSERT_EQ(0, ioctx.write_full("asdfbar", bl));

  uint64_t v = ioctx.get_last_version();
  ObjectWriteOperation op1;
  op1.assert_version(v+1);
  op1.write(0, bl);
  ASSERT_EQ(-EOVERFLOW, ioctx.operate("asdfbar", &op1));
  ObjectWriteOperation op2;
  op2.assert_version(v-1);
  op2.write(0, bl);
  ASSERT_EQ(-ERANGE, ioctx.operate("asdfbar", &op2));
  ObjectWriteOperation op3;
  op3.assert_version(v);
  op3.write(0, bl);
  ASSERT_EQ(0, ioctx.operate("asdfbar", &op3));
}

TEST_F(LibRadosMiscPP, BigAttrPP) {
  char buf[64];
  memset(buf, 0xcc, sizeof(buf));
  bufferlist bl;
  bl.append(buf, sizeof(buf));

  ASSERT_EQ(0, ioctx.create("foo", true));

  bufferlist got;

  cout << "osd_max_attr_size = " << g_conf->osd_max_attr_size << std::endl;
  if (g_conf->osd_max_attr_size) {
    bl.clear();
    got.clear();
    bl.append(buffer::create(g_conf->osd_max_attr_size));
    ASSERT_EQ(0, ioctx.setxattr("foo", "one", bl));
    ASSERT_EQ((int)bl.length(), ioctx.getxattr("foo", "one", got));
    ASSERT_TRUE(bl.contents_equal(got));

    bl.clear();
    bl.append(buffer::create(g_conf->osd_max_attr_size+1));
    ASSERT_EQ(-EFBIG, ioctx.setxattr("foo", "one", bl));
  } else {
    cout << "osd_max_attr_size == 0; skipping test" << std::endl;
  }

  for (int i=0; i<1000; i++) {
    bl.clear();
    got.clear();
    bl.append(buffer::create(MIN(g_conf->osd_max_attr_size, 1024)));
    char n[10];
    snprintf(n, sizeof(n), "a%d", i);
    ASSERT_EQ(0, ioctx.setxattr("foo", n, bl));
    ASSERT_EQ((int)bl.length(), ioctx.getxattr("foo", n, got));
    ASSERT_TRUE(bl.contents_equal(got));
  }
}

TEST_F(LibRadosMiscPP, CopyPP) {
  bufferlist bl, x;
  bl.append("hi there");
  x.append("bar");

  // small object
  bufferlist blc = bl;
  bufferlist xc = x;
  ASSERT_EQ(0, ioctx.write_full("foo", blc));
  ASSERT_EQ(0, ioctx.setxattr("foo", "myattr", xc));

  version_t uv = ioctx.get_last_version();
  {
    // pass future version
    ObjectWriteOperation op;
    op.copy_from2("foo", ioctx, uv + 1, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    ASSERT_EQ(-EOVERFLOW, ioctx.operate("foo.copy", &op));
  }
  {
    // pass old version
    ObjectWriteOperation op;
    op.copy_from2("foo", ioctx, uv - 1, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    ASSERT_EQ(-ERANGE, ioctx.operate("foo.copy", &op));
  }
  {
    ObjectWriteOperation op;
    op.copy_from2("foo", ioctx, uv, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    ASSERT_EQ(0, ioctx.operate("foo.copy", &op));

    bufferlist bl2, x2;
    ASSERT_EQ((int)bl.length(), ioctx.read("foo.copy", bl2, 10000, 0));
    ASSERT_TRUE(bl.contents_equal(bl2));
    ASSERT_EQ((int)x.length(), ioctx.getxattr("foo.copy", "myattr", x2));
    ASSERT_TRUE(x.contents_equal(x2));
  }

  // small object without a version
  {
    ObjectWriteOperation op;
    op.copy_from2("foo", ioctx, 0, LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    ASSERT_EQ(0, ioctx.operate("foo.copy2", &op));

    bufferlist bl2, x2;
    ASSERT_EQ((int)bl.length(), ioctx.read("foo.copy2", bl2, 10000, 0));
    ASSERT_TRUE(bl.contents_equal(bl2));
    ASSERT_EQ((int)x.length(), ioctx.getxattr("foo.copy2", "myattr", x2));
    ASSERT_TRUE(x.contents_equal(x2));
  }

  // do a big object
  bl.append(buffer::create(g_conf->osd_copyfrom_max_chunk * 3));
  bl.zero();
  bl.append("tail");
  blc = bl;
  xc = x;
  ASSERT_EQ(0, ioctx.write_full("big", blc));
  ASSERT_EQ(0, ioctx.setxattr("big", "myattr", xc));

  {
    ObjectWriteOperation op;
    op.copy_from2("big", ioctx, ioctx.get_last_version(), LIBRADOS_OP_FLAG_FADVISE_DONTNEED);
    ASSERT_EQ(0, ioctx.operate("big.copy", &op));

    bufferlist bl2, x2;
    ASSERT_EQ((int)bl.length(), ioctx.read("big.copy", bl2, bl.length(), 0));
    ASSERT_TRUE(bl.contents_equal(bl2));
    ASSERT_EQ((int)x.length(), ioctx.getxattr("foo.copy", "myattr", x2));
    ASSERT_TRUE(x.contents_equal(x2));
  }

  {
    ObjectWriteOperation op;
    op.copy_from2("big", ioctx, 0, LIBRADOS_OP_FLAG_FADVISE_SEQUENTIAL);
    ASSERT_EQ(0, ioctx.operate("big.copy2", &op));

    bufferlist bl2, x2;
    ASSERT_EQ((int)bl.length(), ioctx.read("big.copy2", bl2, bl.length(), 0));
    ASSERT_TRUE(bl.contents_equal(bl2));
    ASSERT_EQ((int)x.length(), ioctx.getxattr("foo.copy2", "myattr", x2));
    ASSERT_TRUE(x.contents_equal(x2));
  }
}

class LibRadosTwoPoolsECPP : public RadosTestECPP
{
public:
  LibRadosTwoPoolsECPP() {};
  ~LibRadosTwoPoolsECPP() override {};
protected:
  static void SetUpTestCase() {
    pool_name = get_temp_pool_name();
    ASSERT_EQ("", create_one_ec_pool_pp(pool_name, s_cluster));
    src_pool_name = get_temp_pool_name();
    ASSERT_EQ(0, s_cluster.pool_create(src_pool_name.c_str()));

    librados::IoCtx ioctx;
    ASSERT_EQ(0, s_cluster.ioctx_create(pool_name.c_str(), ioctx));
    ioctx.application_enable("rados", true);

    librados::IoCtx src_ioctx;
    ASSERT_EQ(0, s_cluster.ioctx_create(src_pool_name.c_str(), src_ioctx));
    src_ioctx.application_enable("rados", true);
  }
  static void TearDownTestCase() {
    ASSERT_EQ(0, s_cluster.pool_delete(src_pool_name.c_str()));
    ASSERT_EQ(0, destroy_one_ec_pool_pp(pool_name, s_cluster));
  }
  static std::string src_pool_name;

  void SetUp() override {
    RadosTestECPP::SetUp();
    ASSERT_EQ(0, cluster.ioctx_create(src_pool_name.c_str(), src_ioctx));
    src_ioctx.set_namespace(nspace);
  }
  void TearDown() override {
    // wait for maps to settle before next test
    cluster.wait_for_latest_osdmap();

    RadosTestECPP::TearDown();

    cleanup_default_namespace(src_ioctx);
    cleanup_namespace(src_ioctx, nspace);

    src_ioctx.close();
  }

  librados::IoCtx src_ioctx;
};
std::string LibRadosTwoPoolsECPP::src_pool_name;

//copy_from between ecpool and no-ecpool.
TEST_F(LibRadosTwoPoolsECPP, CopyFrom) {
  bufferlist z;
  z.append_zero(4194304*2);
  bufferlist b;
  b.append("copyfrom");

  // create big object w/ omapheader
  {
    ASSERT_EQ(0, src_ioctx.write_full("foo", z));
    ASSERT_EQ(0, src_ioctx.omap_set_header("foo", b));
    version_t uv = src_ioctx.get_last_version();
    ObjectWriteOperation op;
    op.copy_from("foo", src_ioctx, uv);
    ASSERT_EQ(-EOPNOTSUPP, ioctx.operate("foo.copy", &op));
  }

  // same with small object
  {
    ASSERT_EQ(0, src_ioctx.omap_set_header("bar", b));
    version_t uv = src_ioctx.get_last_version();
    ObjectWriteOperation op;
    op.copy_from("bar", src_ioctx, uv);
    ASSERT_EQ(-EOPNOTSUPP, ioctx.operate("bar.copy", &op));
  }
}

TEST_F(LibRadosMiscPP, CopyScrubPP) {
  bufferlist inbl, bl, x;
  for (int i=0; i<100; ++i)
    x.append("barrrrrrrrrrrrrrrrrrrrrrrrrr");
  bl.append(buffer::create(g_conf->osd_copyfrom_max_chunk * 3));
  bl.zero();
  bl.append("tail");
  bufferlist cbl;

  map<string, bufferlist> to_set;
  for (int i=0; i<1000; ++i)
    to_set[string("foo") + stringify(i)] = x;

  // small
  cbl = x;
  ASSERT_EQ(0, ioctx.write_full("small", cbl));
  ASSERT_EQ(0, ioctx.setxattr("small", "myattr", x));

  // big
  cbl = bl;
  ASSERT_EQ(0, ioctx.write_full("big", cbl));

  // without header
  cbl = bl;
  ASSERT_EQ(0, ioctx.write_full("big2", cbl));
  ASSERT_EQ(0, ioctx.setxattr("big2", "myattr", x));
  ASSERT_EQ(0, ioctx.setxattr("big2", "myattr2", x));
  ASSERT_EQ(0, ioctx.omap_set("big2", to_set));

  // with header
  cbl = bl;
  ASSERT_EQ(0, ioctx.write_full("big3", cbl));
  ASSERT_EQ(0, ioctx.omap_set_header("big3", x));
  ASSERT_EQ(0, ioctx.omap_set("big3", to_set));

  // deep scrub to ensure digests are in place
  {
    for (int i=0; i<10; ++i) {
      ostringstream ss;
      ss << "{\"prefix\": \"pg deep-scrub\", \"pgid\": \""
	 << ioctx.get_id() << "." << i
	 << "\"}";
      cluster.mon_command(ss.str(), inbl, NULL, NULL);
    }

    // give it a few seconds to go.  this is sloppy but is usually enough time
    cout << "waiting for initial deep scrubs..." << std::endl;
    sleep(30);
    cout << "done waiting, doing copies" << std::endl;
  }

  {
    ObjectWriteOperation op;
    op.copy_from("small", ioctx, 0);
    ASSERT_EQ(0, ioctx.operate("small.copy", &op));
  }

  {
    ObjectWriteOperation op;
    op.copy_from("big", ioctx, 0);
    ASSERT_EQ(0, ioctx.operate("big.copy", &op));
  }

  {
    ObjectWriteOperation op;
    op.copy_from("big2", ioctx, 0);
    ASSERT_EQ(0, ioctx.operate("big2.copy", &op));
  }

  {
    ObjectWriteOperation op;
    op.copy_from("big3", ioctx, 0);
    ASSERT_EQ(0, ioctx.operate("big3.copy", &op));
  }

  // deep scrub to ensure digests are correct
  {
    for (int i=0; i<10; ++i) {
      ostringstream ss;
      ss << "{\"prefix\": \"pg deep-scrub\", \"pgid\": \""
	 << ioctx.get_id() << "." << i
	 << "\"}";
      cluster.mon_command(ss.str(), inbl, NULL, NULL);
    }

    // give it a few seconds to go.  this is sloppy but is usually enough time
    cout << "waiting for final deep scrubs..." << std::endl;
    sleep(30);
    cout << "done waiting" << std::endl;
  }
}

TEST_F(LibRadosMiscPP, WriteSamePP) {
  bufferlist bl;
  char buf[128];
  bufferlist fl;
  char full[128 * 4];
  char *cmp;

  /* zero the full range before using writesame */
  memset(full, 0, sizeof(full));
  fl.append(full, sizeof(full));
  ASSERT_EQ(0, ioctx.write("ws", fl, fl.length(), 0));

  memset(buf, 0xcc, sizeof(buf));
  bl.clear();
  bl.append(buf, sizeof(buf));
  /* write the same buf four times */
  ASSERT_EQ(0, ioctx.writesame("ws", bl, sizeof(full), 0));

  /* read back the full buffer and confirm that it matches */
  fl.clear();
  fl.append(full, sizeof(full));
  ASSERT_EQ((int)fl.length(), ioctx.read("ws", fl, fl.length(), 0));

  for (cmp = fl.c_str(); cmp < fl.c_str() + fl.length(); cmp += sizeof(buf)) {
    ASSERT_EQ(0, memcmp(cmp, buf, sizeof(buf)));
  }

  /* write_len not a multiple of data_len should throw error */
  bl.clear();
  bl.append(buf, sizeof(buf));
  ASSERT_EQ(-EINVAL, ioctx.writesame("ws", bl, (sizeof(buf) * 4) - 1, 0));
  ASSERT_EQ(-EINVAL,
	    ioctx.writesame("ws", bl, bl.length() / 2, 0));
  /* write_len = data_len, i.e. same as write() */
  ASSERT_EQ(0, ioctx.writesame("ws", bl, sizeof(buf), 0));
  bl.clear();
  ASSERT_EQ(-EINVAL,
	    ioctx.writesame("ws", bl, sizeof(buf), 0));
}

TEST_F(LibRadosMisc, WriteSame) {
  char buf[128];
  char full[128 * 4];
  char *cmp;

  /* zero the full range before using writesame */
  memset(full, 0, sizeof(full));
  ASSERT_EQ(0, rados_write(ioctx, "ws", full, sizeof(full), 0));

  memset(buf, 0xcc, sizeof(buf));
  /* write the same buf four times */
  ASSERT_EQ(0, rados_writesame(ioctx, "ws", buf, sizeof(buf), sizeof(full), 0));

  /* read back the full buffer and confirm that it matches */
  ASSERT_EQ((int)sizeof(full), rados_read(ioctx, "ws", full, sizeof(full), 0));

  for (cmp = full; cmp < full + sizeof(full); cmp += sizeof(buf)) {
    ASSERT_EQ(0, memcmp(cmp, buf, sizeof(buf)));
  }

  /* write_len not a multiple of data_len should throw error */
  ASSERT_EQ(-EINVAL, rados_writesame(ioctx, "ws", buf, sizeof(buf),
				     (sizeof(buf) * 4) - 1, 0));
  ASSERT_EQ(-EINVAL,
	    rados_writesame(ioctx, "ws", buf, sizeof(buf), sizeof(buf) / 2, 0));
  ASSERT_EQ(-EINVAL,
	    rados_writesame(ioctx, "ws", buf, 0, sizeof(buf), 0));
  /* write_len = data_len, i.e. same as rados_write() */
  ASSERT_EQ(0, rados_writesame(ioctx, "ws", buf, sizeof(buf), sizeof(buf), 0));
}

template <typename T>
class LibRadosChecksum : public LibRadosMiscPP {
public:
  typedef typename T::alg_t alg_t;
  typedef typename T::value_t value_t;
  typedef typename alg_t::init_value_t init_value_t;

  static const rados_checksum_type_t type = T::type;

  bufferlist content_bl;

  using LibRadosMiscPP::SetUpTestCase;
  using LibRadosMiscPP::TearDownTestCase;

  void SetUp() override {
    LibRadosMiscPP::SetUp();

    std::string content(4096, '\0');
    for (size_t i = 0; i < content.length(); ++i) {
      content[i] = static_cast<char>(rand() % (126 - 33) + 33);
    }
    content_bl.append(content);
    ASSERT_EQ(0, ioctx.write("foo", content_bl, content_bl.length(), 0));
  }
};

template <rados_checksum_type_t _type, typename AlgT, typename ValueT>
class LibRadosChecksumParams {
public:
  typedef AlgT alg_t;
  typedef ValueT value_t;
  static const rados_checksum_type_t type = _type;
};

typedef ::testing::Types<
    LibRadosChecksumParams<LIBRADOS_CHECKSUM_TYPE_XXHASH32,
			   Checksummer::xxhash32, uint32_t>,
    LibRadosChecksumParams<LIBRADOS_CHECKSUM_TYPE_XXHASH64,
			   Checksummer::xxhash64, uint64_t>,
    LibRadosChecksumParams<LIBRADOS_CHECKSUM_TYPE_CRC32C,
			   Checksummer::crc32c, uint32_t>
  > LibRadosChecksumTypes;

TYPED_TEST_CASE(LibRadosChecksum, LibRadosChecksumTypes);

TYPED_TEST(LibRadosChecksum, Subset) {
  uint32_t chunk_size = 1024;
  uint32_t csum_count = this->content_bl.length() / chunk_size;

  typename TestFixture::init_value_t init_value = -1;
  bufferlist init_value_bl;
  ::encode(init_value, init_value_bl);

  std::vector<bufferlist> checksum_bls(csum_count);
  std::vector<int> checksum_rvals(csum_count);

  // individual checksum ops for each chunk
  ObjectReadOperation op;
  for (uint32_t i = 0; i < csum_count; ++i) {
    op.checksum(TestFixture::type, init_value_bl, i * chunk_size, chunk_size,
		0, &checksum_bls[i], &checksum_rvals[i]);
  }
  ASSERT_EQ(0, this->ioctx.operate("foo", &op, NULL));

  for (uint32_t i = 0; i < csum_count; ++i) {
    ASSERT_EQ(0, checksum_rvals[i]);

    auto bl_it = checksum_bls[i].begin();
    uint32_t count;
    ::decode(count, bl_it);
    ASSERT_EQ(1U, count);

    typename TestFixture::value_t value;
    ::decode(value, bl_it);

    bufferlist content_sub_bl;
    content_sub_bl.substr_of(this->content_bl, i * chunk_size, chunk_size);

    typename TestFixture::value_t expected_value;
    bufferptr expected_value_bp = buffer::create_static(
      sizeof(expected_value), reinterpret_cast<char*>(&expected_value));
    Checksummer::template calculate<typename TestFixture::alg_t>(
      init_value, chunk_size, 0, chunk_size, content_sub_bl,
      &expected_value_bp);
    ASSERT_EQ(expected_value, value);
  }
}

TYPED_TEST(LibRadosChecksum, Chunked) {
  uint32_t chunk_size = 1024;
  uint32_t csum_count = this->content_bl.length() / chunk_size;

  typename TestFixture::init_value_t init_value = -1;
  bufferlist init_value_bl;
  ::encode(init_value, init_value_bl);

  bufferlist checksum_bl;
  int checksum_rval;

  // single op with chunked checksum results
  ObjectReadOperation op;
  op.checksum(TestFixture::type, init_value_bl, 0, this->content_bl.length(),
	      chunk_size, &checksum_bl, &checksum_rval);
  ASSERT_EQ(0, this->ioctx.operate("foo", &op, NULL));
  ASSERT_EQ(0, checksum_rval);

  auto bl_it = checksum_bl.begin();
  uint32_t count;
  ::decode(count, bl_it);
  ASSERT_EQ(csum_count, count);

  std::vector<typename TestFixture::value_t> expected_values(csum_count);
  bufferptr expected_values_bp = buffer::create_static(
    csum_count * sizeof(typename TestFixture::value_t),
    reinterpret_cast<char*>(&expected_values[0]));

  Checksummer::template calculate<typename TestFixture::alg_t>(
    init_value, chunk_size, 0, this->content_bl.length(), this->content_bl,
    &expected_values_bp);

  for (uint32_t i = 0; i < csum_count; ++i) {
    typename TestFixture::value_t value;
    ::decode(value, bl_it);
    ASSERT_EQ(expected_values[i], value);
  }
}

TEST_F(LibRadosMiscPP, CmpExtPP) {
  bufferlist cmp_bl, bad_cmp_bl, write_bl;
  char stored_str[] = "1234567891";
  char mismatch_str[] = "1234577777";

  write_bl.append(stored_str);
  ioctx.write("cmpextpp", write_bl, write_bl.length(), 0);
  cmp_bl.append(stored_str);
  ASSERT_EQ(0, ioctx.cmpext("cmpextpp", 0, cmp_bl));

  bad_cmp_bl.append(mismatch_str);
  ASSERT_EQ(-MAX_ERRNO - 5, ioctx.cmpext("cmpextpp", 0, bad_cmp_bl));
}

TEST_F(LibRadosMisc, CmpExt) {
  bufferlist cmp_bl, bad_cmp_bl, write_bl;
  char stored_str[] = "1234567891";
  char mismatch_str[] = "1234577777";

  ASSERT_EQ(0,
	    rados_write(ioctx, "cmpextpp", stored_str, sizeof(stored_str), 0));

  ASSERT_EQ(0,
	    rados_cmpext(ioctx, "cmpextpp", stored_str, sizeof(stored_str), 0));

  ASSERT_EQ(-MAX_ERRNO - 5,
	    rados_cmpext(ioctx, "cmpextpp", mismatch_str, sizeof(mismatch_str), 0));
}

TEST_F(LibRadosMisc, Applications) {
  const char *cmd[] = {"{\"prefix\":\"osd dump\"}", nullptr};
  char *buf, *st;
  size_t buflen, stlen;
  ASSERT_EQ(0, rados_mon_command(cluster, (const char **)cmd, 1, "", 0, &buf,
                                 &buflen, &st, &stlen));
  ASSERT_LT(0u, buflen);
  string result(buf);
  rados_buffer_free(buf);
  rados_buffer_free(st);
  if (!boost::regex_search(result, boost::regex("require_osd_release [l-z]"))) {
    std::cout << "SKIPPING";
    return;
  }

  char apps[128];
  size_t app_len;

  app_len = sizeof(apps);
  ASSERT_EQ(0, rados_application_list(ioctx, apps, &app_len));
  ASSERT_EQ(6U, app_len);
  ASSERT_EQ(0, memcmp("rados\0", apps, app_len));

  ASSERT_EQ(0, rados_application_enable(ioctx, "app1", 1));
  ASSERT_EQ(-EPERM, rados_application_enable(ioctx, "app2", 0));
  ASSERT_EQ(0, rados_application_enable(ioctx, "app2", 1));

  ASSERT_EQ(-ERANGE, rados_application_list(ioctx, apps, &app_len));
  ASSERT_EQ(16U, app_len);
  ASSERT_EQ(0, rados_application_list(ioctx, apps, &app_len));
  ASSERT_EQ(16U, app_len);
  ASSERT_EQ(0, memcmp("app1\0app2\0rados\0", apps, app_len));

  char keys[128];
  char vals[128];
  size_t key_len;
  size_t val_len;

  key_len = sizeof(keys);
  val_len = sizeof(vals);
  ASSERT_EQ(-ENOENT, rados_application_metadata_list(ioctx, "dne", keys,
                                                     &key_len, vals, &val_len));
  ASSERT_EQ(0, rados_application_metadata_list(ioctx, "app1", keys, &key_len,
                                               vals, &val_len));
  ASSERT_EQ(0U, key_len);
  ASSERT_EQ(0U, val_len);

  ASSERT_EQ(-ENOENT, rados_application_metadata_set(ioctx, "dne", "key",
                                                    "value"));
  ASSERT_EQ(0, rados_application_metadata_set(ioctx, "app1", "key1", "value1"));
  ASSERT_EQ(0, rados_application_metadata_set(ioctx, "app1", "key2", "value2"));

  ASSERT_EQ(-ERANGE, rados_application_metadata_list(ioctx, "app1", keys,
                                                     &key_len, vals, &val_len));
  ASSERT_EQ(10U, key_len);
  ASSERT_EQ(14U, val_len);
  ASSERT_EQ(0, rados_application_metadata_list(ioctx, "app1", keys, &key_len,
                                               vals, &val_len));
  ASSERT_EQ(10U, key_len);
  ASSERT_EQ(14U, val_len);
  ASSERT_EQ(0, memcmp("key1\0key2\0", keys, key_len));
  ASSERT_EQ(0, memcmp("value1\0value2\0", vals, val_len));

  ASSERT_EQ(0, rados_application_metadata_remove(ioctx, "app1", "key1"));
  ASSERT_EQ(0, rados_application_metadata_list(ioctx, "app1", keys, &key_len,
                                               vals, &val_len));
  ASSERT_EQ(5U, key_len);
  ASSERT_EQ(7U, val_len);
  ASSERT_EQ(0, memcmp("key2\0", keys, key_len));
  ASSERT_EQ(0, memcmp("value2\0", vals, val_len));
}

TEST_F(LibRadosMiscPP, Applications) {
  bufferlist inbl, outbl;
  string outs;
  ASSERT_EQ(0, cluster.mon_command("{\"prefix\": \"osd dump\"}",
				   inbl, &outbl, &outs));
  ASSERT_LT(0u, outbl.length());
  ASSERT_LE(0u, outs.length());
  if (!boost::regex_search(outbl.to_str(),
                           boost::regex("require_osd_release [l-z]"))) {
    std::cout << "SKIPPING";
    return;
  }

  std::set<std::string> expected_apps = {"rados"};
  std::set<std::string> apps;
  ASSERT_EQ(0, ioctx.application_list(&apps));
  ASSERT_EQ(expected_apps, apps);

  ASSERT_EQ(0, ioctx.application_enable("app1", true));
  ASSERT_EQ(-EPERM, ioctx.application_enable("app2", false));
  ASSERT_EQ(0, ioctx.application_enable("app2", true));

  expected_apps = {"app1", "app2", "rados"};
  ASSERT_EQ(0, ioctx.application_list(&apps));
  ASSERT_EQ(expected_apps, apps);

  std::map<std::string, std::string> expected_meta;
  std::map<std::string, std::string> meta;
  ASSERT_EQ(-ENOENT, ioctx.application_metadata_list("dne", &meta));
  ASSERT_EQ(0, ioctx.application_metadata_list("app1", &meta));
  ASSERT_EQ(expected_meta, meta);

  ASSERT_EQ(-ENOENT, ioctx.application_metadata_set("dne", "key1", "value1"));
  ASSERT_EQ(0, ioctx.application_metadata_set("app1", "key1", "value1"));
  ASSERT_EQ(0, ioctx.application_metadata_set("app1", "key2", "value2"));

  expected_meta = {{"key1", "value1"}, {"key2", "value2"}};
  ASSERT_EQ(0, ioctx.application_metadata_list("app1", &meta));
  ASSERT_EQ(expected_meta, meta);

  ASSERT_EQ(0, ioctx.application_metadata_remove("app1", "key1"));

  expected_meta = {{"key2", "value2"}};
  ASSERT_EQ(0, ioctx.application_metadata_list("app1", &meta));
  ASSERT_EQ(expected_meta, meta);
}

TEST_F(LibRadosMiscECPP, CompareExtentRange) {
  bufferlist bl1;
  bl1.append("ceph");
  ObjectWriteOperation write;
  write.write(0, bl1);
  ASSERT_EQ(0, ioctx.operate("foo", &write));

  bufferlist bl2;
  bl2.append("ph");
  bl2.append(std::string(2, '\0'));
  ObjectReadOperation read1;
  read1.cmpext(2, bl2, nullptr);
  ASSERT_EQ(0, ioctx.operate("foo", &read1, nullptr));

  bufferlist bl3;
  bl3.append(std::string(4, '\0'));
  ObjectReadOperation read2;
  read2.cmpext(2097152, bl3, nullptr);
  ASSERT_EQ(0, ioctx.operate("foo", &read2, nullptr));
}
