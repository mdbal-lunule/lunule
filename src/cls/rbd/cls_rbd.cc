// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/** \file
 *
 * This is an OSD class that implements methods for
 * use with rbd.
 *
 * Most of these deal with the rbd header object. Methods prefixed
 * with old_ deal with the original rbd design, in which clients read
 * and interpreted the header object directly.
 *
 * The new format is meant to be opaque to clients - all their
 * interactions with non-data objects should go through this
 * class. The OSD class interface leaves the class to implement its
 * own argument and payload serialization/deserialization, so for ease
 * of implementation we use the existing ceph encoding/decoding
 * methods. Something like json might be preferable, but the rbd
 * kernel module has to be able to understand format as well. The
 * datatypes exposed to the clients are strings, unsigned integers,
 * and vectors of those types. The on-wire format can be found in
 * src/include/encoding.h.
 *
 * The methods for interacting with the new format document their
 * parameters as the client sees them - it would be silly to mention
 * in each one that they take an input and an output bufferlist.
 */
#include "include/types.h"

#include <algorithm>
#include <errno.h>
#include <sstream>

#include "common/bit_vector.hpp"
#include "common/errno.h"
#include "objclass/objclass.h"
#include "osd/osd_types.h"
#include "include/rbd_types.h"
#include "include/rbd/object_map_types.h"

#include "cls/rbd/cls_rbd.h"
#include "cls/rbd/cls_rbd_types.h"


/*
 * Object keys:
 *
 * <partial list>
 *
 * stripe_unit: size in bytes of the stripe unit.  if not present,
 *   the stripe unit is assumed to match the object size (1 << order).
 *
 * stripe_count: number of objects to stripe over before looping back.
 *   if not present or 1, striping is disabled.  this is the default.
 *
 */

CLS_VER(2,0)
CLS_NAME(rbd)

#define RBD_MAX_KEYS_READ 64
#define RBD_SNAP_KEY_PREFIX "snapshot_"
#define RBD_DIR_ID_KEY_PREFIX "id_"
#define RBD_DIR_NAME_KEY_PREFIX "name_"
#define RBD_METADATA_KEY_PREFIX "metadata_"

#define GROUP_SNAP_SEQ "snap_seq"

static int snap_read_header(cls_method_context_t hctx, bufferlist& bl)
{
  unsigned snap_count = 0;
  uint64_t snap_names_len = 0;
  struct rbd_obj_header_ondisk *header;

  CLS_LOG(20, "snapshots_list");

  while (1) {
    int len = sizeof(*header) +
      snap_count * sizeof(struct rbd_obj_snap_ondisk) +
      snap_names_len;

    int rc = cls_cxx_read(hctx, 0, len, &bl);
    if (rc < 0)
      return rc;

    if (bl.length() < sizeof(*header))
      return -EINVAL;

    header = (struct rbd_obj_header_ondisk *)bl.c_str();
    assert(header);

    if ((snap_count != header->snap_count) ||
        (snap_names_len != header->snap_names_len)) {
      snap_count = header->snap_count;
      snap_names_len = header->snap_names_len;
      bl.clear();
      continue;
    }
    break;
  }

  return 0;
}

static void key_from_snap_id(snapid_t snap_id, string *out)
{
  ostringstream oss;
  oss << RBD_SNAP_KEY_PREFIX
      << std::setw(16) << std::setfill('0') << std::hex << snap_id;
  *out = oss.str();
}

static snapid_t snap_id_from_key(const string &key)
{
  istringstream iss(key);
  uint64_t id;
  iss.ignore(strlen(RBD_SNAP_KEY_PREFIX)) >> std::hex >> id;
  return id;
}

template<typename T>
static int read_key(cls_method_context_t hctx, const string &key, T *out)
{
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, key, &bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading omap key %s: %s", key.c_str(), cpp_strerror(r).c_str());
    }
    return r;
  }

  try {
    bufferlist::iterator it = bl.begin();
    ::decode(*out, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding %s", key.c_str());
    return -EIO;
  }

  return 0;
}

static int remove_key(cls_method_context_t hctx, const string &key) {
  int r = cls_cxx_map_remove_key(hctx, key);
  if (r < 0 && r != -ENOENT) {
      CLS_ERR("failed to remove key: %s", key.c_str());
      return r;
  }
  return 0;
}

static bool is_valid_id(const string &id) {
  if (!id.size())
    return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (!isalnum(id[i])) {
      return false;
    }
  }
  return true;
}

/**
 * Initialize the header with basic metadata.
 * Extra features may initialize more fields in the future.
 * Everything is stored as key/value pairs as omaps in the header object.
 *
 * If features the OSD does not understand are requested, -ENOSYS is
 * returned.
 *
 * Input:
 * @param size number of bytes in the image (uint64_t)
 * @param order bits to shift to determine the size of data objects (uint8_t)
 * @param features what optional things this image will use (uint64_t)
 * @param object_prefix a prefix for all the data objects
 * @param data_pool_id pool id where data objects is stored (int64_t)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int create(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string object_prefix;
  uint64_t features, size;
  uint8_t order;
  int64_t data_pool_id = -1;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(size, iter);
    ::decode(order, iter);
    ::decode(features, iter);
    ::decode(object_prefix, iter);
    if (!iter.end()) {
      ::decode(data_pool_id, iter);
    }
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "create object_prefix=%s size=%llu order=%u features=%llu",
	  object_prefix.c_str(), (unsigned long long)size, order,
	  (unsigned long long)features);

  if (features & ~RBD_FEATURES_ALL) {
    return -ENOSYS;
  }

  if (!object_prefix.size()) {
    return -EINVAL;
  }

  bufferlist stored_prefixbl;
  int r = cls_cxx_map_get_val(hctx, "object_prefix", &stored_prefixbl);
  if (r != -ENOENT) {
    CLS_ERR("reading object_prefix returned %d", r);
    return -EEXIST;
  }

  bufferlist sizebl;
  bufferlist orderbl;
  bufferlist featuresbl;
  bufferlist object_prefixbl;
  bufferlist snap_seqbl;
  bufferlist create_timestampbl;
  uint64_t snap_seq = 0;
  utime_t create_timestamp = ceph_clock_now();
  ::encode(size, sizebl);
  ::encode(order, orderbl);
  ::encode(features, featuresbl);
  ::encode(object_prefix, object_prefixbl);
  ::encode(snap_seq, snap_seqbl);
  ::encode(create_timestamp, create_timestampbl);

  map<string, bufferlist> omap_vals;
  omap_vals["size"] = sizebl;
  omap_vals["order"] = orderbl;
  omap_vals["features"] = featuresbl;
  omap_vals["object_prefix"] = object_prefixbl;
  omap_vals["snap_seq"] = snap_seqbl;
  omap_vals["create_timestamp"] = create_timestampbl;

  if (features & RBD_FEATURE_DATA_POOL) {
    if (data_pool_id == -1) {
      CLS_ERR("data pool not provided with feature enabled");
      return -EINVAL;
    }

    bufferlist data_pool_id_bl;
    ::encode(data_pool_id, data_pool_id_bl);
    omap_vals["data_pool_id"] = data_pool_id_bl;
  } else if (data_pool_id != -1) {
    CLS_ERR("data pool provided with feature disabled");
    return -EINVAL;
  }

  r = cls_cxx_map_set_vals(hctx, &omap_vals);
  if (r < 0)
    return r;

  return 0;
}

/**
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t) (deprecated)
 * @param read_only true if the image will be used read-only (bool)
 *
 * Output:
 * @param features list of enabled features for the given snapshot (uint64_t)
 * @param incompatible incompatible feature bits
 * @returns 0 on success, negative error code on failure
 */
int get_features(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;
  bool read_only = false;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
    if (!iter.end()) {
      ::decode(read_only, iter);
    }
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_features snap_id=%" PRIu64 ", read_only=%d",
          snap_id, read_only);

  // NOTE: keep this deprecated snapshot logic to support negative
  // test cases in older (pre-Infernalis) releases. Remove once older
  // releases are no longer supported.
  if (snap_id != CEPH_NOSNAP) {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0) {
      return r;
    }
  }

  uint64_t features;
  int r = read_key(hctx, "features", &features);
  if (r < 0) {
    CLS_ERR("failed to read features off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  uint64_t incompatible = (read_only ? features & RBD_FEATURES_INCOMPATIBLE :
				       features & RBD_FEATURES_RW_INCOMPATIBLE);
  ::encode(features, *out);
  ::encode(incompatible, *out);
  return 0;
}

/**
 * set the image features
 *
 * Input:
 * @param features image features
 * @param mask image feature mask
 *
 * Output:
 * none
 *
 * @returns 0 on success, negative error code upon failure
 */
int set_features(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t features;
  uint64_t mask;
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(features, iter);
    ::decode(mask, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // check that features exists to make sure this is a header object
  // that was created correctly
  uint64_t orig_features = 0;
  int r = read_key(hctx, "features", &orig_features);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("Could not read image's features off disk: %s",
            cpp_strerror(r).c_str());
    return r;
  }

  // newer clients might attempt to mask off features we don't support
  mask &= RBD_FEATURES_ALL;

  uint64_t enabled_features = features & mask;
  if ((enabled_features & RBD_FEATURES_MUTABLE) != enabled_features) {
    CLS_ERR("Attempting to enable immutable feature: %" PRIu64,
            static_cast<uint64_t>(enabled_features & ~RBD_FEATURES_MUTABLE));
    return -EINVAL;
  }

  uint64_t disabled_features = ~features & mask;
  uint64_t disable_mask = (RBD_FEATURES_MUTABLE | RBD_FEATURES_DISABLE_ONLY);
  if ((disabled_features & disable_mask) != disabled_features) {
       CLS_ERR("Attempting to disable immutable feature: %" PRIu64,
               enabled_features & ~disable_mask);
       return -EINVAL;
  }

  features = (orig_features & ~mask) | (features & mask);
  CLS_LOG(10, "set_features features=%" PRIu64 " orig_features=%" PRIu64,
          features, orig_features);

  bufferlist bl;
  ::encode(features, bl);
  r = cls_cxx_map_set_val(hctx, "features", &bl);
  if (r < 0) {
    CLS_ERR("error updating features: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

/**
 * check that given feature(s) are set
 *
 * @param hctx context
 * @param need features needed
 * @return 0 if features are set, negative error (like ENOEXEC) otherwise
 */
int require_feature(cls_method_context_t hctx, uint64_t need)
{
  uint64_t features;
  int r = read_key(hctx, "features", &features);
  if (r == -ENOENT)   // this implies it's an old-style image with no features
    return -ENOEXEC;
  if (r < 0)
    return r;
  if ((features & need) != need) {
    CLS_LOG(10, "require_feature missing feature %llx, have %llx",
            (unsigned long long)need, (unsigned long long)features);
    return -ENOEXEC;
  }
  return 0;
}

/**
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param order bits to shift to get the size of data objects (uint8_t)
 * @param size size of the image in bytes for the given snapshot (uint64_t)
 * @returns 0 on success, negative error code on failure
 */
int get_size(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id, size;
  uint8_t order;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_size snap_id=%llu", (unsigned long long)snap_id);

  int r = read_key(hctx, "order", &order);
  if (r < 0) {
    CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (snap_id == CEPH_NOSNAP) {
    r = read_key(hctx, "size", &size);
    if (r < 0) {
      CLS_ERR("failed to read the image's size off of disk: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0)
      return r;

    size = snap.image_size;
  }

  ::encode(order, *out);
  ::encode(size, *out);

  return 0;
}

/**
 * Input:
 * @param size new capacity of the image in bytes (uint64_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int set_size(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t size;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(size, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // check that size exists to make sure this is a header object
  // that was created correctly
  uint64_t orig_size;
  int r = read_key(hctx, "size", &orig_size);
  if (r < 0) {
    CLS_ERR("Could not read image's size off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  CLS_LOG(20, "set_size size=%llu orig_size=%llu", (unsigned long long)size,
          (unsigned long long)orig_size);

  bufferlist sizebl;
  ::encode(size, sizebl);
  r = cls_cxx_map_set_val(hctx, "size", &sizebl);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  // if we are shrinking, and have a parent, shrink our overlap with
  // the parent, too.
  if (size < orig_size) {
    cls_rbd_parent parent;
    r = read_key(hctx, "parent", &parent);
    if (r == -ENOENT)
      r = 0;
    if (r < 0)
      return r;
    if (parent.exists() && parent.overlap > size) {
      bufferlist parentbl;
      parent.overlap = size;
      ::encode(parent, parentbl);
      r = cls_cxx_map_set_val(hctx, "parent", &parentbl);
      if (r < 0) {
	CLS_ERR("error writing parent: %s", cpp_strerror(r).c_str());
	return r;
      }
    }
  }

  return 0;
}

/**
 * verify that the header object exists
 *
 * @return 0 if the object exists, -ENOENT if it does not, or other error
 */
int check_exists(cls_method_context_t hctx)
{
  uint64_t size;
  time_t mtime;
  return cls_cxx_stat(hctx, &size, &mtime);
}

/**
 * get the current protection status of the specified snapshot
 *
 * Input:
 * @param snap_id (uint64_t) which snapshot to get the status of
 *
 * Output:
 * @param status (uint8_t) one of:
 * RBD_PROTECTION_STATUS_{PROTECTED, UNPROTECTED, UNPROTECTING}
 *
 * @returns 0 on success, negative error code on failure
 * @returns -EINVAL if snapid is CEPH_NOSNAP
 */
int get_protection_status(cls_method_context_t hctx, bufferlist *in,
			  bufferlist *out)
{
  snapid_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "get_protection_status: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_protection_status snap_id=%llu",
         (unsigned long long)snap_id.val);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id.val, &snapshot_key);
  r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    CLS_ERR("could not read key for snapshot id %" PRIu64, snap_id.val);
    return r;
  }

  if (snap.protection_status >= RBD_PROTECTION_STATUS_LAST) {
    CLS_ERR("invalid protection status for snap id %llu: %u",
	    (unsigned long long)snap_id.val, snap.protection_status);
    return -EIO;
  }

  ::encode(snap.protection_status, *out);
  return 0;
}

/**
 * set the proctection status of a snapshot
 *
 * Input:
 * @param snapid (uint64_t) which snapshot to set the status of
 * @param status (uint8_t) one of:
 * RBD_PROTECTION_STATUS_{PROTECTED, UNPROTECTED, UNPROTECTING}
 *
 * @returns 0 on success, negative error code on failure
 * @returns -EINVAL if snapid is CEPH_NOSNAP
 */
int set_protection_status(cls_method_context_t hctx, bufferlist *in,
			  bufferlist *out)
{
  snapid_t snap_id;
  uint8_t status;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
    ::decode(status, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "set_protection_status: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0) {
    CLS_LOG(20, "image does not support layering");
    return r;
  }

  CLS_LOG(20, "set_protection_status snapid=%llu status=%u",
	  (unsigned long long)snap_id.val, status);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  if (status >= RBD_PROTECTION_STATUS_LAST) {
    CLS_LOG(10, "invalid protection status for snap id %llu: %u",
	    (unsigned long long)snap_id.val, status);
    return -EINVAL;
  }

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id.val, &snapshot_key);
  r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    CLS_ERR("could not read key for snapshot id %" PRIu64, snap_id.val);
    return r;
  }

  snap.protection_status = status;
  bufferlist snapshot_bl;
  ::encode(snap, snapshot_bl);
  r = cls_cxx_map_set_val(hctx, snapshot_key, &snapshot_bl);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * get striping parameters
 *
 * Input:
 * none
 *
 * Output:
 * @param stripe unit (bytes)
 * @param stripe count (num objects)
 *
 * @returns 0 on success
 */
int get_stripe_unit_count(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_stripe_unit_count");

  r = require_feature(hctx, RBD_FEATURE_STRIPINGV2);
  if (r < 0)
    return r;

  uint64_t stripe_unit = 0, stripe_count = 0;
  r = read_key(hctx, "stripe_unit", &stripe_unit);
  if (r == -ENOENT) {
    // default to object size
    uint8_t order;
    r = read_key(hctx, "order", &order);
    if (r < 0) {
      CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
      return -EIO;
    }
    stripe_unit = 1ull << order;
  }
  if (r < 0)
    return r;
  r = read_key(hctx, "stripe_count", &stripe_count);
  if (r == -ENOENT) {
    // default to 1
    stripe_count = 1;
    r = 0;
  }
  if (r < 0)
    return r;

  ::encode(stripe_unit, *out);
  ::encode(stripe_count, *out);
  return 0;
}

/**
 * set striping parameters
 *
 * Input:
 * @param stripe unit (bytes)
 * @param stripe count (num objects)
 *
 * @returns 0 on success
 */
int set_stripe_unit_count(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t stripe_unit, stripe_count;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(stripe_unit, iter);
    ::decode(stripe_count, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "set_stripe_unit_count: invalid decode");
    return -EINVAL;
  }

  if (!stripe_count || !stripe_unit)
    return -EINVAL;

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "set_stripe_unit_count");

  r = require_feature(hctx, RBD_FEATURE_STRIPINGV2);
  if (r < 0)
    return r;

  uint8_t order;
  r = read_key(hctx, "order", &order);
  if (r < 0) {
    CLS_ERR("failed to read the order off of disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  if ((1ull << order) % stripe_unit || stripe_unit > (1ull << order)) {
    CLS_ERR("stripe unit %llu is not a factor of the object size %llu",
            (unsigned long long)stripe_unit, 1ull << order);
    return -EINVAL;
  }

  bufferlist bl, bl2;
  ::encode(stripe_unit, bl);
  r = cls_cxx_map_set_val(hctx, "stripe_unit", &bl);
  if (r < 0) {
    CLS_ERR("error writing stripe_unit metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  ::encode(stripe_count, bl2);
  r = cls_cxx_map_set_val(hctx, "stripe_count", &bl2);
  if (r < 0) {
    CLS_ERR("error writing stripe_count metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

int get_create_timestamp(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_create_timestamp");

  utime_t timestamp;
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, "create_timestamp", &bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading create_timestamp: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    try {
      bufferlist::iterator it = bl.begin();
      ::decode(timestamp, it);
    } catch (const buffer::error &err) {
      CLS_ERR("could not decode create_timestamp");
      return -EIO;
    }
  }

  ::encode(timestamp, *out);
  return 0;
}

/**
 * get the image flags
 *
 * Input:
 * @param snap_id which snapshot to query, to CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param flags image flags
 *
 * @returns 0 on success, negative error code upon failure
 */
int get_flags(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_flags snap_id=%llu", (unsigned long long)snap_id);

  uint64_t flags = 0;
  if (snap_id == CEPH_NOSNAP) {
    int r = read_key(hctx, "flags", &flags);
    if (r < 0 && r != -ENOENT) {
      CLS_ERR("failed to read flags off disk: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    cls_rbd_snap snap;
    string snapshot_key;
    key_from_snap_id(snap_id, &snapshot_key);
    int r = read_key(hctx, snapshot_key, &snap);
    if (r < 0) {
      return r;
    }
    flags = snap.flags;
  }

  ::encode(flags, *out);
  return 0;
}

/**
 * set the image flags
 *
 * Input:
 * @param flags image flags
 * @param mask image flag mask
 * @param snap_id which snapshot to update, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * none
 *
 * @returns 0 on success, negative error code upon failure
 */
int set_flags(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t flags;
  uint64_t mask;
  uint64_t snap_id = CEPH_NOSNAP;
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(flags, iter);
    ::decode(mask, iter);
    if (!iter.end()) {
      ::decode(snap_id, iter);
    }
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // check that size exists to make sure this is a header object
  // that was created correctly
  int r;
  uint64_t orig_flags = 0;
  cls_rbd_snap snap_meta;
  string snap_meta_key;
  if (snap_id == CEPH_NOSNAP) {
    r = read_key(hctx, "flags", &orig_flags);
    if (r < 0 && r != -ENOENT) {
      CLS_ERR("Could not read image's flags off disk: %s",
              cpp_strerror(r).c_str());
      return r;
    }
  } else {
    key_from_snap_id(snap_id, &snap_meta_key);
    r = read_key(hctx, snap_meta_key, &snap_meta);
    if (r < 0) {
      CLS_ERR("Could not read snapshot: snap_id=%" PRIu64 ": %s",
              snap_id, cpp_strerror(r).c_str());
      return r;
    }
    orig_flags = snap_meta.flags;
  }

  flags = (orig_flags & ~mask) | (flags & mask);
  CLS_LOG(20, "set_flags snap_id=%" PRIu64 ", orig_flags=%" PRIu64 ", "
              "new_flags=%" PRIu64 ", mask=%" PRIu64, snap_id, orig_flags,
              flags, mask);

  if (snap_id == CEPH_NOSNAP) {
    bufferlist bl;
    ::encode(flags, bl);
    r = cls_cxx_map_set_val(hctx, "flags", &bl);
  } else {
    snap_meta.flags = flags;

    bufferlist bl;
    ::encode(snap_meta, bl);
    r = cls_cxx_map_set_val(hctx, snap_meta_key, &bl);
  }

  if (r < 0) {
    CLS_ERR("error updating flags: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

/**
 * get the current parent, if any
 *
 * Input:
 * @param snap_id which snapshot to query, or CEPH_NOSNAP (uint64_t)
 *
 * Output:
 * @param pool parent pool id (-1 if parent does not exist)
 * @param image parent image id
 * @param snapid parent snapid
 * @param size portion of parent mapped under the child
 *
 * @returns 0 on success or parent does not exist, negative error code on failure
 */
int get_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_parent snap_id=%llu", (unsigned long long)snap_id);

  cls_rbd_parent parent;
  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r == 0) {
    if (snap_id == CEPH_NOSNAP) {
      r = read_key(hctx, "parent", &parent);
      if (r < 0 && r != -ENOENT)
	return r;
    } else {
      cls_rbd_snap snap;
      string snapshot_key;
      key_from_snap_id(snap_id, &snapshot_key);
      r = read_key(hctx, snapshot_key, &snap);
      if (r < 0 && r != -ENOENT)
	return r;
      parent = snap.parent;
    }
  }

  ::encode(parent.pool, *out);
  ::encode(parent.id, *out);
  ::encode(parent.snapid, *out);
  ::encode(parent.overlap, *out);
  return 0;
}

/**
 * set the image parent
 *
 * Input:
 * @param pool parent pool
 * @param id parent image id
 * @param snapid parent snapid
 * @param size parent size
 *
 * @returns 0 on success, or negative error code
 */
int set_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int64_t pool;
  string id;
  snapid_t snapid;
  uint64_t size;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(pool, iter);
    ::decode(id, iter);
    ::decode(snapid, iter);
    ::decode(size, iter);
  } catch (const buffer::error &err) {
    CLS_LOG(20, "cls_rbd::set_parent: invalid decode");
    return -EINVAL;
  }

  int r = check_exists(hctx);
  if (r < 0) {
    CLS_LOG(20, "cls_rbd::set_parent: child already exists");
    return r;
  }

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0) {
    CLS_LOG(20, "cls_rbd::set_parent: child does not support layering");
    return r;
  }

  CLS_LOG(20, "set_parent pool=%llu id=%s snapid=%llu size=%llu",
	  (unsigned long long)pool, id.c_str(), (unsigned long long)snapid.val,
	  (unsigned long long)size);

  if (pool < 0 || id.length() == 0 || snapid == CEPH_NOSNAP || size == 0) {
    return -EINVAL;
  }

  // make sure there isn't already a parent
  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r == 0) {
    CLS_LOG(20, "set_parent existing parent pool=%llu id=%s snapid=%llu"
            "overlap=%llu", (unsigned long long)parent.pool, parent.id.c_str(),
	    (unsigned long long)parent.snapid.val,
	    (unsigned long long)parent.overlap);
    return -EEXIST;
  }

  // our overlap is the min of our size and the parent's size.
  uint64_t our_size;
  r = read_key(hctx, "size", &our_size);
  if (r < 0)
    return r;

  bufferlist parentbl;
  parent.pool = pool;
  parent.id = id;
  parent.snapid = snapid;
  parent.overlap = MIN(our_size, size);
  ::encode(parent, parentbl);
  r = cls_cxx_map_set_val(hctx, "parent", &parentbl);
  if (r < 0) {
    CLS_ERR("error writing parent: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}


/**
 * remove the parent pointer
 *
 * This can only happen on the head, not on a snapshot.  No arguments.
 *
 * @returns 0 on success, negative error code on failure.
 */
int remove_parent(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  r = require_feature(hctx, RBD_FEATURE_LAYERING);
  if (r < 0)
    return r;

  uint64_t features;
  r = read_key(hctx, "features", &features);
  if (r < 0) {
    return r;
  }

  // remove the parent from all snapshots
  if ((features & RBD_FEATURE_DEEP_FLATTEN) != 0) {
    int max_read = RBD_MAX_KEYS_READ;
    vector<snapid_t> snap_ids;
    string last_read = RBD_SNAP_KEY_PREFIX;
    bool more;

    do {
      set<string> keys;
      r = cls_cxx_map_get_keys(hctx, last_read, max_read, &keys, &more);
      if (r < 0) {
        return r;
      }

      for (std::set<string>::const_iterator it = keys.begin();
           it != keys.end(); ++it) {
        if ((*it).find(RBD_SNAP_KEY_PREFIX) != 0) {
	  break;
        }

        uint64_t snap_id = snap_id_from_key(*it);
        cls_rbd_snap snap_meta;
        r = read_key(hctx, *it, &snap_meta);
        if (r < 0) {
          CLS_ERR("Could not read snapshot: snap_id=%" PRIu64 ": %s",
                  snap_id, cpp_strerror(r).c_str());
          return r;
        }

        snap_meta.parent = cls_rbd_parent();

        bufferlist bl;
        ::encode(snap_meta, bl);
        r = cls_cxx_map_set_val(hctx, *it, &bl);
        if (r < 0) {
          CLS_ERR("Could not update snapshot: snap_id=%" PRIu64 ": %s",
                  snap_id, cpp_strerror(r).c_str());
          return r;
        }
      }

      if (!keys.empty()) {
        last_read = *(keys.rbegin());
      }
    } while (more);
  }

  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r < 0)
    return r;

  r = cls_cxx_map_remove_key(hctx, "parent");
  if (r < 0) {
    CLS_ERR("error removing parent: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

/**
 * methods for dealing with rbd_children object
 */

static int decode_parent_common(bufferlist::iterator& it, uint64_t *pool_id,
				string *image_id, snapid_t *snap_id)
{
  try {
    ::decode(*pool_id, it);
    ::decode(*image_id, it);
    ::decode(*snap_id, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding parent spec");
    return -EINVAL;
  }
  return 0;
}

static int decode_parent(bufferlist *in, uint64_t *pool_id,
			 string *image_id, snapid_t *snap_id)
{
  bufferlist::iterator it = in->begin();
  return decode_parent_common(it, pool_id, image_id, snap_id);
}

static int decode_parent_and_child(bufferlist *in, uint64_t *pool_id,
			           string *image_id, snapid_t *snap_id,
				   string *c_image_id)
{
  bufferlist::iterator it = in->begin();
  int r = decode_parent_common(it, pool_id, image_id, snap_id);
  if (r < 0)
    return r;
  try {
    ::decode(*c_image_id, it);
  } catch (const buffer::error &err) {
    CLS_ERR("error decoding child image id");
    return -EINVAL;
  }
  return 0;
}

static string parent_key(uint64_t pool_id, string image_id, snapid_t snap_id)
{
  bufferlist key_bl;
  ::encode(pool_id, key_bl);
  ::encode(image_id, key_bl);
  ::encode(snap_id, key_bl);
  return string(key_bl.c_str(), key_bl.length());
}

/**
 * add child to rbd_children directory object
 *
 * rbd_children is a map of (p_pool_id, p_image_id, p_snap_id) to
 * [c_image_id, [c_image_id ... ]]
 *
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * @returns 0 on success, negative error on failure
 */

int add_child(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;

  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id, c_image_id;
  // Use set for ease of erase() for remove_child()
  std::set<string> children;

  r = decode_parent_and_child(in, &p_pool_id, &p_image_id, &p_snap_id,
			      &c_image_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "add_child %s to (%" PRIu64 ", %s, %" PRIu64 ")", c_image_id.c_str(),
	  p_pool_id, p_image_id.c_str(), p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  // get current child list for parent, if any
  r = read_key(hctx, key, &children);
  if ((r < 0) && (r != -ENOENT)) {
    CLS_LOG(20, "add_child: omap read failed: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (children.find(c_image_id) != children.end()) {
    CLS_LOG(20, "add_child: child already exists: %s", c_image_id.c_str());
    return -EEXIST;
  }
  // add new child
  children.insert(c_image_id);

  // write back
  bufferlist childbl;
  ::encode(children, childbl);
  r = cls_cxx_map_set_val(hctx, key, &childbl);
  if (r < 0)
    CLS_LOG(20, "add_child: omap write failed: %s", cpp_strerror(r).c_str());
  return r;
}

/**
 * remove child from rbd_children directory object
 *
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * @returns 0 on success, negative error on failure
 */

int remove_child(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;

  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id, c_image_id;
  std::set<string> children;

  r = decode_parent_and_child(in, &p_pool_id, &p_image_id, &p_snap_id,
			      &c_image_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "remove_child %s from (%" PRIu64 ", %s, %" PRIu64 ")",
	       c_image_id.c_str(), p_pool_id, p_image_id.c_str(),
	       p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  // get current child list for parent.  Unlike add_child(), an empty list
  // is an error (how can we remove something that doesn't exist?)
  r = read_key(hctx, key, &children);
  if (r < 0) {
    CLS_LOG(20, "remove_child: read omap failed: %s", cpp_strerror(r).c_str());
    return r;
  }

  if (children.find(c_image_id) == children.end()) {
    CLS_LOG(20, "remove_child: child not found: %s", c_image_id.c_str());
    return -ENOENT;
  }
  // find and remove child
  children.erase(c_image_id);

  // now empty?  remove key altogether
  if (children.empty()) {
    r = cls_cxx_map_remove_key(hctx, key);
    if (r < 0)
      CLS_LOG(20, "remove_child: remove key failed: %s", cpp_strerror(r).c_str());
  } else {
    // write back shortened children list
    bufferlist childbl;
    ::encode(children, childbl);
    r = cls_cxx_map_set_val(hctx, key, &childbl);
    if (r < 0)
      CLS_LOG(20, "remove_child: write omap failed: %s", cpp_strerror(r).c_str());
  }
  return r;
}

/**
 * Input:
 * @param p_pool_id parent pool id
 * @param p_image_id parent image oid
 * @param p_snap_id parent snapshot id
 * @param c_image_id new child image oid to add
 *
 * Output:
 * @param children set<string> of children
 *
 * @returns 0 on success, negative error on failure
 */
int get_children(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r;
  uint64_t p_pool_id;
  snapid_t p_snap_id;
  string p_image_id;
  std::set<string> children;

  r = decode_parent(in, &p_pool_id, &p_image_id, &p_snap_id);
  if (r < 0)
    return r;

  CLS_LOG(20, "get_children of (%" PRIu64 ", %s, %" PRIu64 ")",
	  p_pool_id, p_image_id.c_str(), p_snap_id.val);

  string key = parent_key(p_pool_id, p_image_id, p_snap_id);

  r = read_key(hctx, key, &children);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_LOG(20, "get_children: read omap failed: %s", cpp_strerror(r).c_str());
    return r;
  }
  ::encode(children, *out);
  return 0;
}


/**
 * Get the information needed to create a rados snap context for doing
 * I/O to the data objects. This must include all snapshots.
 *
 * Output:
 * @param snap_seq the highest snapshot id ever associated with the image (uint64_t)
 * @param snap_ids existing snapshot ids in descending order (vector<uint64_t>)
 * @returns 0 on success, negative error code on failure
 */
int get_snapcontext(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_snapcontext");

  int r;
  int max_read = RBD_MAX_KEYS_READ;
  vector<snapid_t> snap_ids;
  string last_read = RBD_SNAP_KEY_PREFIX;
  bool more;

  do {
    set<string> keys;
    r = cls_cxx_map_get_keys(hctx, last_read, max_read, &keys, &more);
    if (r < 0)
      return r;

    for (set<string>::const_iterator it = keys.begin();
	 it != keys.end(); ++it) {
      if ((*it).find(RBD_SNAP_KEY_PREFIX) != 0)
	break;
      snapid_t snap_id = snap_id_from_key(*it);
      snap_ids.push_back(snap_id);
    }
    if (!keys.empty())
      last_read = *(keys.rbegin());
  } while (more);

  uint64_t snap_seq;
  r = read_key(hctx, "snap_seq", &snap_seq);
  if (r < 0) {
    CLS_ERR("could not read the image's snap_seq off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  // snap_ids must be descending in a snap context
  std::reverse(snap_ids.begin(), snap_ids.end());

  ::encode(snap_seq, *out);
  ::encode(snap_ids, *out);

  return 0;
}

/**
 * Output:
 * @param object_prefix prefix for data object names (string)
 * @returns 0 on success, negative error code on failure
 */
int get_object_prefix(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_object_prefix");

  string object_prefix;
  int r = read_key(hctx, "object_prefix", &object_prefix);
  if (r < 0) {
    CLS_ERR("failed to read the image's object prefix off of disk: %s",
            cpp_strerror(r).c_str());
    return r;
  }

  ::encode(object_prefix, *out);

  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @param pool_id (int64_t) of data pool or -1 if none
 * @returns 0 on success, negative error code on failure
 */
int get_data_pool(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "get_data_pool");

  int64_t data_pool_id = -1;
  int r = read_key(hctx, "data_pool_id", &data_pool_id);
  if (r == -ENOENT) {
    data_pool_id = -1;
  } else if (r < 0) {
    CLS_ERR("error reading image data pool id: %s", cpp_strerror(r).c_str());
    return r;
  }

  ::encode(data_pool_id, *out);
  return 0;
}

int get_snapshot_name(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_snapshot_name snap_id=%llu", (unsigned long long)snap_id);

  if (snap_id == CEPH_NOSNAP)
    return -EINVAL;

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r < 0)
    return r;

  ::encode(snap.name, *out);

  return 0;
}

int get_snapshot_timestamp(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;
  
  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_snapshot_timestamp snap_id=%llu", (unsigned long long)snap_id);

  if (snap_id == CEPH_NOSNAP) {
    return -EINVAL;
  }
  
  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    return r;
  }

  ::encode(snap.timestamp, *out);
  return 0;
}

/**
 * Retrieve namespace of a snapshot.
 *
 * Input:
 * @param snap_id id of the snapshot (uint64_t)
 *
 * Output:
 * @param SnapshotNamespace
 * @returns 0 on success, negative error code on failure.
 */
int get_snapshot_namespace(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t snap_id;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "get_snapshot_namespace snap_id=%" PRIu64, snap_id);

  if (snap_id == CEPH_NOSNAP) {
    return -EINVAL;
  }

  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r < 0) {
    return r;
  }

  ::encode(snap.snapshot_namespace, *out);

  return 0;
}

/**
 * Adds a snapshot to an rbd header. Ensures the id and name are unique.
 *
 * Input:
 * @param snap_name name of the snapshot (string)
 * @param snap_id id of the snapshot (uint64_t)
 * @param snap_namespace namespace of the snapshot (cls::rbd::SnapshotNamespaceOnDisk)
 *
 * Output:
 * @returns 0 on success, negative error code on failure.
 * @returns -ESTALE if the input snap_id is less than the image's snap_seq
 * @returns -EEXIST if the id or name are already used by another snapshot
 */
int snapshot_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist snap_namebl, snap_idbl;
  cls_rbd_snap snap_meta;
  uint64_t snap_limit;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(snap_meta.name, iter);
    ::decode(snap_meta.id, iter);
    if (!iter.end()) {
      ::decode(snap_meta.snapshot_namespace, iter);
    }
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (boost::get<cls::rbd::UnknownSnapshotNamespace>(
        &snap_meta.snapshot_namespace.snapshot_namespace) != nullptr) {
    CLS_ERR("Unknown snapshot namespace provided");
    return -EINVAL;
  }

  CLS_LOG(20, "snapshot_add name=%s id=%llu", snap_meta.name.c_str(),
	 (unsigned long long)snap_meta.id.val);

  if (snap_meta.id > CEPH_MAXSNAP)
    return -EINVAL;

  uint64_t cur_snap_seq;
  int r = read_key(hctx, "snap_seq", &cur_snap_seq);
  if (r < 0) {
    CLS_ERR("Could not read image's snap_seq off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  // client lost a race with another snapshot creation.
  // snap_seq must be monotonically increasing.
  if (snap_meta.id < cur_snap_seq)
    return -ESTALE;

  r = read_key(hctx, "size", &snap_meta.image_size);
  if (r < 0) {
    CLS_ERR("Could not read image's size off disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, "features", &snap_meta.features);
  if (r < 0) {
    CLS_ERR("Could not read image's features off disk: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, "flags", &snap_meta.flags);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("Could not read image's flags off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  r = read_key(hctx, "snap_limit", &snap_limit);
  if (r == -ENOENT) {
    snap_limit = UINT64_MAX;
  } else if (r < 0) {
    CLS_ERR("Could not read snapshot limit off disk: %s", cpp_strerror(r).c_str());
    return r;
  }

  snap_meta.timestamp = ceph_clock_now();

  int max_read = RBD_MAX_KEYS_READ;
  uint64_t total_read = 0;
  string last_read = RBD_SNAP_KEY_PREFIX;
  bool more;
  do {
    map<string, bufferlist> vals;
    r = cls_cxx_map_get_vals(hctx, last_read, RBD_SNAP_KEY_PREFIX,
			     max_read, &vals, &more);
    if (r < 0)
      return r;

    total_read += vals.size();
    if (total_read >= snap_limit) {
      CLS_ERR("Attempt to create snapshot over limit of %" PRIu64, snap_limit);
      return -EDQUOT;
    }

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end(); ++it) {
      cls_rbd_snap old_meta;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(old_meta, iter);
      } catch (const buffer::error &err) {
	snapid_t snap_id = snap_id_from_key(it->first);
	CLS_ERR("error decoding snapshot metadata for snap_id: %llu",
	        (unsigned long long)snap_id.val);
	return -EIO;
      }
      if ((snap_meta.name == old_meta.name &&
	    snap_meta.snapshot_namespace == old_meta.snapshot_namespace) ||
	  snap_meta.id == old_meta.id) {
	CLS_LOG(20, "snap_name %s or snap_id %llu matches existing snap %s %llu",
		snap_meta.name.c_str(), (unsigned long long)snap_meta.id.val,
		old_meta.name.c_str(), (unsigned long long)old_meta.id.val);
	return -EEXIST;
      }
    }

    if (!vals.empty())
      last_read = vals.rbegin()->first;
  } while (more);

  // snapshot inherits parent, if any
  cls_rbd_parent parent;
  r = read_key(hctx, "parent", &parent);
  if (r < 0 && r != -ENOENT)
    return r;
  if (r == 0) {
    snap_meta.parent = parent;
  }

  bufferlist snap_metabl, snap_seqbl;
  ::encode(snap_meta, snap_metabl);
  ::encode(snap_meta.id, snap_seqbl);

  string snapshot_key;
  key_from_snap_id(snap_meta.id, &snapshot_key);
  map<string, bufferlist> vals;
  vals["snap_seq"] = snap_seqbl;
  vals[snapshot_key] = snap_metabl;
  r = cls_cxx_map_set_vals(hctx, &vals);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}


/**
 * rename snapshot .
 *
 * Input:
 * @param src_snap_id old snap id of the snapshot (snapid_t)
 * @param dst_snap_name new name of the snapshot (string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure.
 */
int snapshot_rename(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist snap_namebl, snap_idbl;
  snapid_t src_snap_id;
  string src_snap_key,dst_snap_name;
  cls_rbd_snap snap_meta;
  int r;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(src_snap_id, iter);
    ::decode(dst_snap_name, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  
  CLS_LOG(20, "snapshot_rename id=%llu dst_name=%s", (unsigned long long)src_snap_id.val,
	 dst_snap_name.c_str());

  int max_read = RBD_MAX_KEYS_READ;
  string last_read = RBD_SNAP_KEY_PREFIX;
  bool more;
  do {
    map<string, bufferlist> vals;
    r = cls_cxx_map_get_vals(hctx, last_read, RBD_SNAP_KEY_PREFIX,
			     max_read, &vals, &more);
    if (r < 0)
      return r;

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end(); ++it) {
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(snap_meta, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("error decoding snapshot metadata for snap : %s",
	        dst_snap_name.c_str());
	return -EIO;
      }
      if (dst_snap_name == snap_meta.name) {
	CLS_LOG(20, "snap_name %s  matches existing snap with snap id = %llu ",
		dst_snap_name.c_str(), (unsigned long long)snap_meta.id.val);
        return -EEXIST;
      }
    }
    if (!vals.empty())
      last_read = vals.rbegin()->first;
  } while (more);

  key_from_snap_id(src_snap_id, &src_snap_key);
  r = read_key(hctx, src_snap_key, &snap_meta); 
  if (r == -ENOENT) {
    CLS_LOG(20, "cannot find existing snap with snap id = %llu ", (unsigned long long)src_snap_id);
    return r;
  }
  snap_meta.name = dst_snap_name;
  bufferlist snap_metabl;
  ::encode(snap_meta, snap_metabl);

  r = cls_cxx_map_set_val(hctx, src_snap_key, &snap_metabl);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}
/**
 * Removes a snapshot from an rbd header.
 *
 * Input:
 * @param snap_id the id of the snapshot to remove (uint64_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int snapshot_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  snapid_t snap_id;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "snapshot_remove id=%llu", (unsigned long long)snap_id.val);

  // check if the key exists. we can't rely on remove_key doing this for
  // us, since OMAPRMKEYS returns success if the key is not there.
  // bug or feature? sounds like a bug, since tmap did not have this
  // behavior, but cls_rgw may rely on it...
  cls_rbd_snap snap;
  string snapshot_key;
  key_from_snap_id(snap_id, &snapshot_key);
  int r = read_key(hctx, snapshot_key, &snap);
  if (r == -ENOENT)
    return -ENOENT;

  if (snap.protection_status != RBD_PROTECTION_STATUS_UNPROTECTED)
    return -EBUSY;

  r = cls_cxx_map_remove_key(hctx, snapshot_key);
  if (r < 0) {
    CLS_ERR("error writing snapshot metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Returns a uint64_t of all the features supported by this class.
 */
int get_all_features(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t all_features = RBD_FEATURES_ALL;
  ::encode(all_features, *out);
  return 0;
}

/**
 * "Copy up" data from the parent of a clone to the clone's object(s).
 * Used for implementing copy-on-write for a clone image.  Client
 * will pass down a chunk of data that fits completely within one
 * clone block (one object), and is aligned (starts at beginning of block),
 * but may be shorter (for non-full parent blocks).  The class method
 * can't know the object size to validate the requested length,
 * so it just writes the data as given if the child object doesn't
 * already exist, and returns success if it does.
 *
 * Input:
 * @param in bufferlist of data to write
 *
 * Output:
 * @returns 0 on success, or if block already exists in child
 *  negative error code on other error
 */

int copyup(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  // check for existence; if child object exists, just return success
  if (cls_cxx_stat(hctx, NULL, NULL) == 0)
    return 0;
  CLS_LOG(20, "copyup: writing length %d\n", in->length());
  return cls_cxx_write(hctx, 0, in->length(), in);
}


/************************ rbd_id object methods **************************/

/**
 * Input:
 * @param in ignored
 *
 * Output:
 * @param id the id stored in the object
 * @returns 0 on success, negative error code on failure
 */
int get_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t size;
  int r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0)
    return r;

  if (size == 0)
    return -ENOENT;

  bufferlist read_bl;
  r = cls_cxx_read(hctx, 0, size, &read_bl);
  if (r < 0) {
    CLS_ERR("get_id: could not read id: %s", cpp_strerror(r).c_str());
    return r;
  }

  string id;
  try {
    bufferlist::iterator iter = read_bl.begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EIO;
  }

  ::encode(id, *out);
  return 0;
}

/**
 * Set the id of an image. The object must already exist.
 *
 * Input:
 * @param id the id of the image, as an alpha-numeric string
 *
 * Output:
 * @returns 0 on success, -EEXIST if the atomic create fails,
 *          negative error code on other error
 */
int set_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = check_exists(hctx);
  if (r < 0)
    return r;

  string id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (!is_valid_id(id)) {
    CLS_ERR("set_id: invalid id '%s'", id.c_str());
    return -EINVAL;
  }

  uint64_t size;
  r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0)
    return r;
  if (size != 0)
    return -EEXIST;

  CLS_LOG(20, "set_id: id=%s", id.c_str());

  bufferlist write_bl;
  ::encode(id, write_bl);
  return cls_cxx_write(hctx, 0, write_bl.length(), &write_bl);
}

/*********************** methods for rbd_directory ***********************/

static const string dir_key_for_id(const string &id)
{
  return RBD_DIR_ID_KEY_PREFIX + id;
}

static const string dir_key_for_name(const string &name)
{
  return RBD_DIR_NAME_KEY_PREFIX + name;
}

static const string dir_name_from_key(const string &key)
{
  return key.substr(strlen(RBD_DIR_NAME_KEY_PREFIX));
}

static int dir_add_image_helper(cls_method_context_t hctx,
				const string &name, const string &id,
				bool check_for_unique_id)
{
  if (!name.size() || !is_valid_id(id)) {
    CLS_ERR("dir_add_image_helper: invalid name '%s' or id '%s'",
	    name.c_str(), id.c_str());
    return -EINVAL;
  }

  CLS_LOG(20, "dir_add_image_helper name=%s id=%s", name.c_str(), id.c_str());

  string tmp;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);
  int r = read_key(hctx, name_key, &tmp);
  if (r != -ENOENT) {
    CLS_LOG(10, "name already exists");
    return -EEXIST;
  }
  r = read_key(hctx, id_key, &tmp);
  if (r != -ENOENT && check_for_unique_id) {
    CLS_LOG(10, "id already exists");
    return -EBADF;
  }
  bufferlist id_bl, name_bl;
  ::encode(id, id_bl);
  ::encode(name, name_bl);
  map<string, bufferlist> omap_vals;
  omap_vals[name_key] = id_bl;
  omap_vals[id_key] = name_bl;
  return cls_cxx_map_set_vals(hctx, &omap_vals);
}

static int dir_remove_image_helper(cls_method_context_t hctx,
				   const string &name, const string &id)
{
  CLS_LOG(20, "dir_remove_image_helper name=%s id=%s",
	  name.c_str(), id.c_str());

  string stored_name, stored_id;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);
  int r = read_key(hctx, name_key, &stored_id);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading name to id mapping: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, id_key, &stored_name);
  if (r < 0) {
    CLS_ERR("error reading id to name mapping: %s", cpp_strerror(r).c_str());
    return r;
  }

  // check if this op raced with a rename
  if (stored_name != name || stored_id != id) {
    CLS_ERR("stored name '%s' and id '%s' do not match args '%s' and '%s'",
	    stored_name.c_str(), stored_id.c_str(), name.c_str(), id.c_str());
    return -ESTALE;
  }

  r = cls_cxx_map_remove_key(hctx, name_key);
  if (r < 0) {
    CLS_ERR("error removing name: %s", cpp_strerror(r).c_str());
    return r;
  }

  r = cls_cxx_map_remove_key(hctx, id_key);
  if (r < 0) {
    CLS_ERR("error removing id: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Rename an image in the directory, updating both indexes
 * atomically. This can't be done from the client calling
 * dir_add_image and dir_remove_image in one transaction because the
 * results of the first method are not visibale to later steps.
 *
 * Input:
 * @param src original name of the image
 * @param dest new name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -ESTALE if src and id do not map to each other
 * @returns -ENOENT if src or id are not in the directory
 * @returns -EEXIST if dest already exists
 * @returns 0 on success, negative error code on failure
 */
int dir_rename_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string src, dest, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(src, iter);
    ::decode(dest, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = dir_remove_image_helper(hctx, src, id);
  if (r < 0)
    return r;
  // ignore duplicate id because the result of
  // remove_image_helper is not visible yet
  return dir_add_image_helper(hctx, dest, id, false);
}

/**
 * Get the id of an image given its name.
 *
 * Input:
 * @param name the name of the image
 *
 * Output:
 * @param id the id of the image
 * @returns 0 on success, negative error code on failure
 */
int dir_get_id(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string name;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "dir_get_id: name=%s", name.c_str());

  string id;
  int r = read_key(hctx, dir_key_for_name(name), &id);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading id for name '%s': %s", name.c_str(), cpp_strerror(r).c_str());
    return r;
  }
  ::encode(id, *out);
  return 0;
}

/**
 * Get the name of an image given its id.
 *
 * Input:
 * @param id the id of the image
 *
 * Output:
 * @param name the name of the image
 * @returns 0 on success, negative error code on failure
 */
int dir_get_name(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string id;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "dir_get_name: id=%s", id.c_str());

  string name;
  int r = read_key(hctx, dir_key_for_id(id), &name);
  if (r < 0) {
    CLS_ERR("error reading name for id '%s': %s", id.c_str(), cpp_strerror(r).c_str());
    return r;
  }
  ::encode(name, *out);
  return 0;
}

/**
 * List the names and ids of the images in the directory, sorted by
 * name.
 *
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param images map from name to id of up to max_return images
 * @returns 0 on success, negative error code on failure
 */
int dir_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string start_after;
  uint64_t max_return;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int max_read = RBD_MAX_KEYS_READ;
  map<string, string> images;
  string last_read = dir_key_for_name(start_after);
  bool more = true;

  while (more && images.size() < max_return) {
    map<string, bufferlist> vals;
    CLS_LOG(20, "last_read = '%s'", last_read.c_str());
    int r = cls_cxx_map_get_vals(hctx, last_read, RBD_DIR_NAME_KEY_PREFIX,
                                 max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading directory by name: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end(); ++it) {
      string id;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(id, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode id of image '%s'", it->first.c_str());
	return -EIO;
      }
      CLS_LOG(20, "adding '%s' -> '%s'", dir_name_from_key(it->first).c_str(), id.c_str());
      images[dir_name_from_key(it->first)] = id;
      if (images.size() >= max_return)
	break;
    }
    if (!vals.empty()) {
      last_read = dir_key_for_name(images.rbegin()->first);
    }
  }

  ::encode(images, *out);

  return 0;
}

/**
 * Add an image to the rbd directory. Creates the directory object if
 * needed, and updates the index from id to name and name to id.
 *
 * Input:
 * @param name the name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -EEXIST if the image name is already in the directory
 * @returns -EBADF if the image id is already in the directory
 * @returns 0 on success, negative error code on failure
 */
int dir_add_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = cls_cxx_create(hctx, false);
  if (r < 0) {
    CLS_ERR("could not create directory: %s", cpp_strerror(r).c_str());
    return r;
  }

  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  return dir_add_image_helper(hctx, name, id, true);
}

/**
 * Remove an image from the rbd directory.
 *
 * Input:
 * @param name the name of the image
 * @param id the id of the image
 *
 * Output:
 * @returns -ESTALE if the name and id do not map to each other
 * @returns 0 on success, negative error code on failure
 */
int dir_remove_image(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  return dir_remove_image_helper(hctx, name, id);
}

int object_map_read(cls_method_context_t hctx, BitVector<2> &object_map)
{
  uint64_t size;
  int r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0) {
    return r;
  }
  if (size == 0) {
    return -ENOENT;
  }

  bufferlist bl;
  r = cls_cxx_read(hctx, 0, size, &bl);
  if (r < 0) {
   return r;
  }

  try {
    bufferlist::iterator iter = bl.begin();
    ::decode(object_map, iter);
  } catch (const buffer::error &err) {
    CLS_ERR("failed to decode object map: %s", err.what());
    return -EINVAL;
  }
  return 0;
}

/**
 * Load an rbd image's object map
 *
 * Input:
 * none
 *
 * Output:
 * @param object map bit vector
 * @returns 0 on success, negative error code on failure
 */
int object_map_load(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  BitVector<2> object_map;
  int r = object_map_read(hctx, object_map);
  if (r < 0) {
    return r;
  }

  object_map.set_crc_enabled(false);
  ::encode(object_map, *out);
  return 0;
}

/**
 * Save an rbd image's object map
 *
 * Input:
 * @param object map bit vector
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_save(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  BitVector<2> object_map;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(object_map, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  object_map.set_crc_enabled(true);

  bufferlist bl;
  ::encode(object_map, bl);
  CLS_LOG(20, "object_map_save: object size=%" PRIu64 ", byte size=%u",
	  object_map.size(), bl.length());
  return cls_cxx_write_full(hctx, &bl);
}

/**
 * Resize an rbd image's object map
 *
 * Input:
 * @param object_count the max number of objects in the image
 * @param default_state the default state of newly created objects
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_resize(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t object_count;
  uint8_t default_state;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(object_count, iter);
    ::decode(default_state, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // protect against excessive memory requirements
  if (object_count > cls::rbd::MAX_OBJECT_MAP_OBJECT_COUNT) {
    CLS_ERR("object map too large: %" PRIu64, object_count);
    return -EINVAL;
  }

  BitVector<2> object_map;
  int r = object_map_read(hctx, object_map);
  if ((r < 0) && (r != -ENOENT)) {
    return r;
  }

  size_t orig_object_map_size = object_map.size();
  if (object_count < orig_object_map_size) {
    for (uint64_t i = object_count + 1; i < orig_object_map_size; ++i) {
      if (object_map[i] != default_state) {
	CLS_ERR("object map indicates object still exists: %" PRIu64, i);
	return -ESTALE;
      }
    }
    object_map.resize(object_count);
  } else if (object_count > orig_object_map_size) {
    object_map.resize(object_count);
    for (uint64_t i = orig_object_map_size; i < object_count; ++i) {
      object_map[i] = default_state;
    }
  }

  bufferlist map;
  ::encode(object_map, map);
  CLS_LOG(20, "object_map_resize: object size=%" PRIu64 ", byte size=%u",
	  object_count, map.length());
  return cls_cxx_write_full(hctx, &map);
}

/**
 * Update an rbd image's object map
 *
 * Input:
 * @param start_object_no the start object iterator
 * @param end_object_no the end object iterator
 * @param new_object_state the new object state
 * @param current_object_state optional current object state filter
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_update(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  uint64_t start_object_no;
  uint64_t end_object_no;
  uint8_t new_object_state;
  boost::optional<uint8_t> current_object_state;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_object_no, iter);
    ::decode(end_object_no, iter);
    ::decode(new_object_state, iter);
    ::decode(current_object_state, iter);
  } catch (const buffer::error &err) {
    CLS_ERR("failed to decode message");
    return -EINVAL;
  }

  uint64_t size;
  int r = cls_cxx_stat(hctx, &size, NULL);
  if (r < 0) {
    return r;
  }

  BitVector<2> object_map;
  bufferlist header_bl;
  r = cls_cxx_read2(hctx, 0, object_map.get_header_length(), &header_bl,
                    CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
  if (r < 0) {
    CLS_ERR("object map header read failed");
    return r;
  }

  try {
    bufferlist::iterator it = header_bl.begin();
    object_map.decode_header(it);
  } catch (const buffer::error &err) {
    CLS_ERR("failed to decode object map header: %s", err.what());
    return -EINVAL;
  }

  bufferlist footer_bl;
  r = cls_cxx_read2(hctx, object_map.get_footer_offset(),
		    size - object_map.get_footer_offset(), &footer_bl,
                    CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
  if (r < 0) {
    CLS_ERR("object map footer read failed");
    return r;
  }

  try {
    bufferlist::iterator it = footer_bl.begin();
    object_map.decode_footer(it);
  } catch (const buffer::error &err) {
    CLS_ERR("failed to decode object map footer: %s", err.what());
  }

  if (start_object_no >= end_object_no || end_object_no > object_map.size()) {
    return -ERANGE;
  }

  uint64_t byte_offset;
  uint64_t byte_length;
  object_map.get_data_extents(start_object_no,
			      end_object_no - start_object_no,
			      &byte_offset, &byte_length);

  bufferlist data_bl;
  r = cls_cxx_read2(hctx, object_map.get_header_length() + byte_offset,
		    byte_length, &data_bl, CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
  if (r < 0) {
    CLS_ERR("object map data read failed");
    return r;
  }

  try {
    bufferlist::iterator it = data_bl.begin();
    object_map.decode_data(it, byte_offset);
  } catch (const buffer::error &err) {
    CLS_ERR("failed to decode data chunk [%" PRIu64 "]: %s",
	    byte_offset, err.what());
    return -EINVAL;
  }

  bool updated = false;
  auto it = object_map.begin() + start_object_no;
  auto end_it = object_map.begin() + end_object_no;
  for (; it != end_it; ++it) {
    uint8_t state = *it;
    if ((!current_object_state || state == *current_object_state ||
        (*current_object_state == OBJECT_EXISTS &&
         state == OBJECT_EXISTS_CLEAN)) && state != new_object_state) {
      *it = new_object_state;
      updated = true;
    }
  }

  if (updated) {
    CLS_LOG(20, "object_map_update: %" PRIu64 "~%" PRIu64 " -> %" PRIu64,
	    byte_offset, byte_length,
	    object_map.get_header_length() + byte_offset);

    bufferlist data_bl;
    object_map.encode_data(data_bl, byte_offset, byte_length);
    r = cls_cxx_write2(hctx, object_map.get_header_length() + byte_offset,
		       data_bl.length(), &data_bl,
                       CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
    if (r < 0) {
      CLS_ERR("failed to write object map header: %s", cpp_strerror(r).c_str());
      return r;
    }

    footer_bl.clear();
    object_map.encode_footer(footer_bl);
    r = cls_cxx_write2(hctx, object_map.get_footer_offset(), footer_bl.length(),
		       &footer_bl, CEPH_OSD_OP_FLAG_FADVISE_WILLNEED);
    if (r < 0) {
      CLS_ERR("failed to write object map footer: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    CLS_LOG(20, "object_map_update: no update necessary");
  }

  return 0;
}

/**
 * Mark all _EXISTS objects as _EXISTS_CLEAN so future writes to the
 * image HEAD can be tracked.
 *
 * Input:
 * none
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_snap_add(cls_method_context_t hctx, bufferlist *in,
                        bufferlist *out)
{
  BitVector<2> object_map;
  int r = object_map_read(hctx, object_map);
  if (r < 0) {
    return r;
  }

  bool updated = false;
  auto it = object_map.begin();
  auto end_it = object_map.end();
  for (; it != end_it; ++it) {
    if (*it == OBJECT_EXISTS) {
      *it = OBJECT_EXISTS_CLEAN;
      updated = true;
    }
  }

  if (updated) {
    bufferlist bl;
    ::encode(object_map, bl);
    r = cls_cxx_write_full(hctx, &bl);
  }
  return r;
}

/**
 * Mark all _EXISTS_CLEAN objects as _EXISTS in the current object map
 * if the provided snapshot object map object is marked as _EXISTS.
 *
 * Input:
 * @param snapshot object map bit vector
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int object_map_snap_remove(cls_method_context_t hctx, bufferlist *in,
                           bufferlist *out)
{
  BitVector<2> src_object_map;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(src_object_map, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  BitVector<2> dst_object_map;
  int r = object_map_read(hctx, dst_object_map);
  if (r < 0) {
    return r;
  }

  bool updated = false;
  auto src_it = src_object_map.begin();
  auto dst_it = dst_object_map.begin();
  auto dst_it_end = dst_object_map.end();
  uint64_t i = 0;
  for (; dst_it != dst_it_end; ++dst_it) {
    if (*dst_it == OBJECT_EXISTS_CLEAN &&
        (i >= src_object_map.size() || *src_it == OBJECT_EXISTS)) {
      *dst_it = OBJECT_EXISTS;
      updated = true;
    }
    if (i < src_object_map.size())
      ++src_it;
    ++i;
  }

  if (updated) {
    bufferlist bl;
    ::encode(dst_object_map, bl);
    r = cls_cxx_write_full(hctx, &bl);
  }
  return r;
}

static const string metadata_key_for_name(const string &name)
{
  return RBD_METADATA_KEY_PREFIX + name;
}

static const string metadata_name_from_key(const string &key)
{
  return key.substr(strlen(RBD_METADATA_KEY_PREFIX));
}

/**
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list

 * Output:
 * @param value
 * @returns 0 on success, negative error code on failure
 */
int metadata_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string start_after;
  uint64_t max_return;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  // TODO remove implicit support for zero during the N-release
  if (max_return == 0) {
    max_return = RBD_MAX_KEYS_READ;
  }

  map<string, bufferlist> data;
  string last_read = metadata_key_for_name(start_after);
  bool more = true;

  while (more && data.size() < max_return) {
    map<string, bufferlist> raw_data;
    int max_read = MIN(RBD_MAX_KEYS_READ, max_return - data.size());
    int r = cls_cxx_map_get_vals(hctx, last_read, RBD_METADATA_KEY_PREFIX,
                                 max_read, &raw_data, &more);
    if (r < 0) {
      CLS_ERR("failed to read the vals off of disk: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (auto& kv : raw_data) {
      data[metadata_name_from_key(kv.first)].swap(kv.second);
    }

    if (!raw_data.empty()) {
      last_read = raw_data.rbegin()->first;
    }
  }

  ::encode(data, *out);
  return 0;
}

/**
 * Input:
 * @param data <map(key, value)>
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int metadata_set(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  map<string, bufferlist> data, raw_data;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(data, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  for (map<string, bufferlist>::iterator it = data.begin();
       it != data.end(); ++it) {
    CLS_LOG(20, "metdata_set key=%s value=%.*s", it->first.c_str(),
	    it->second.length(), it->second.c_str());
    raw_data[metadata_key_for_name(it->first)].swap(it->second);
  }
  int r = cls_cxx_map_set_vals(hctx, &raw_data);
  if (r < 0) {
    CLS_ERR("error writing metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Input:
 * @param key
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int metadata_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string key;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(key, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "metdata_set key=%s", key.c_str());

  int r = cls_cxx_map_remove_key(hctx, metadata_key_for_name(key));
  if (r < 0) {
    CLS_ERR("error remove metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Input:
 * @param key
 *
 * Output:
 * @param metadata value associated with the key
 * @returns 0 on success, negative error code on failure
 */
int metadata_get(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string key;
  bufferlist value;

  bufferlist::iterator iter = in->begin();
  try {
    ::decode(key, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "metdata_get key=%s", key.c_str());

  int r = cls_cxx_map_get_val(hctx, metadata_key_for_name(key), &value);
  if (r < 0) {
    CLS_ERR("error get metadata: %s", cpp_strerror(r).c_str());
    return r;
  }

  ::encode(value, *out);
  return 0;
}

int snapshot_get_limit(cls_method_context_t hctx, bufferlist *in,
		       bufferlist *out)
{
  uint64_t snap_limit;
  int r = read_key(hctx, "snap_limit", &snap_limit);
  if (r == -ENOENT) {
    snap_limit = UINT64_MAX;
  } else if (r < 0) {
    CLS_ERR("error retrieving snapshot limit: %s", cpp_strerror(r).c_str());
    return r;
  }

  CLS_LOG(20, "read snapshot limit %" PRIu64, snap_limit);
  ::encode(snap_limit, *out);

  return 0;
}

int snapshot_set_limit(cls_method_context_t hctx, bufferlist *in,
		       bufferlist *out)
{
  int rc;
  uint64_t new_limit;
  bufferlist bl;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(new_limit, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (new_limit == UINT64_MAX) {
    CLS_LOG(20, "remove snapshot limit\n");
    rc = cls_cxx_map_remove_key(hctx, "snap_limit");
  } else {
    CLS_LOG(20, "set snapshot limit to %" PRIu64 "\n", new_limit);
    ::encode(new_limit, bl);
    rc = cls_cxx_map_set_val(hctx, "snap_limit", &bl);
  }

  return rc;
}


/****************************** Old format *******************************/

int old_snapshots_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();
  bufferptr p(header->snap_names_len);
  char *buf = (char *)header;
  char *name = buf + sizeof(*header) + header->snap_count * sizeof(struct rbd_obj_snap_ondisk);
  char *end = name + header->snap_names_len;
  memcpy(p.c_str(),
         buf + sizeof(*header) + header->snap_count * sizeof(struct rbd_obj_snap_ondisk),
         header->snap_names_len);

  ::encode(header->snap_seq, *out);
  ::encode(header->snap_count, *out);

  for (unsigned i = 0; i < header->snap_count; i++) {
    string s = name;
    ::encode(header->snaps[i].id, *out);
    ::encode(header->snaps[i].image_size, *out);
    ::encode(s, *out);

    name += strlen(name) + 1;
    if (name > end)
      return -EIO;
  }

  return 0;
}

int old_snapshot_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  bufferlist newbl;
  bufferptr header_bp(sizeof(*header));
  struct rbd_obj_snap_ondisk *new_snaps;

  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();

  int snaps_id_ofs = sizeof(*header);
  int names_ofs = snaps_id_ofs + sizeof(*new_snaps) * header->snap_count;
  const char *snap_name;
  const char *snap_names = ((char *)header) + names_ofs;
  const char *end = snap_names + header->snap_names_len;
  bufferlist::iterator iter = in->begin();
  string s;
  uint64_t snap_id;

  try {
    ::decode(s, iter);
    ::decode(snap_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  snap_name = s.c_str();

  if (header->snap_seq > snap_id)
    return -ESTALE;

  uint64_t snap_limit;
  rc = read_key(hctx, "snap_limit", &snap_limit);
  if (rc == -ENOENT) {
    snap_limit = UINT64_MAX;
  } else if (rc < 0) {
    return rc;
  }

  if (header->snap_count >= snap_limit)
    return -EDQUOT;

  const char *cur_snap_name;
  for (cur_snap_name = snap_names; cur_snap_name < end; cur_snap_name += strlen(cur_snap_name) + 1) {
    if (strncmp(cur_snap_name, snap_name, end - cur_snap_name) == 0)
      return -EEXIST;
  }
  if (cur_snap_name > end)
    return -EIO;

  int snap_name_len = strlen(snap_name);

  bufferptr new_names_bp(header->snap_names_len + snap_name_len + 1);
  bufferptr new_snaps_bp(sizeof(*new_snaps) * (header->snap_count + 1));

  /* copy snap names and append to new snap name */
  char *new_snap_names = new_names_bp.c_str();
  strcpy(new_snap_names, snap_name);
  memcpy(new_snap_names + snap_name_len + 1, snap_names, header->snap_names_len);

  /* append new snap id */
  new_snaps = (struct rbd_obj_snap_ondisk *)new_snaps_bp.c_str();
  memcpy(new_snaps + 1, header->snaps, sizeof(*new_snaps) * header->snap_count);

  header->snap_count = header->snap_count + 1;
  header->snap_names_len = header->snap_names_len + snap_name_len + 1;
  header->snap_seq = snap_id;

  new_snaps[0].id = snap_id;
  new_snaps[0].image_size = header->image_size;

  memcpy(header_bp.c_str(), header, sizeof(*header));

  newbl.push_back(header_bp);
  newbl.push_back(new_snaps_bp);
  newbl.push_back(new_names_bp);

  rc = cls_cxx_write_full(hctx, &newbl);
  if (rc < 0)
    return rc;

  return 0;
}

int old_snapshot_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  bufferlist newbl;
  bufferptr header_bp(sizeof(*header));

  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();

  int snaps_id_ofs = sizeof(*header);
  int names_ofs = snaps_id_ofs + sizeof(struct rbd_obj_snap_ondisk) * header->snap_count;
  const char *snap_name;
  const char *snap_names = ((char *)header) + names_ofs;
  const char *orig_names = snap_names;
  const char *end = snap_names + header->snap_names_len;
  bufferlist::iterator iter = in->begin();
  string s;
  unsigned i;
  bool found = false;
  struct rbd_obj_snap_ondisk snap;

  try {
    ::decode(s, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  snap_name = s.c_str();

  for (i = 0; snap_names < end; i++) {
    if (strcmp(snap_names, snap_name) == 0) {
      snap = header->snaps[i];
      found = true;
      break;
    }
    snap_names += strlen(snap_names) + 1;
  }
  if (!found) {
    CLS_ERR("couldn't find snap %s\n", snap_name);
    return -ENOENT;
  }

  header->snap_names_len  = header->snap_names_len - (s.length() + 1);
  header->snap_count = header->snap_count - 1;

  bufferptr new_names_bp(header->snap_names_len);
  bufferptr new_snaps_bp(sizeof(header->snaps[0]) * header->snap_count);

  memcpy(header_bp.c_str(), header, sizeof(*header));
  newbl.push_back(header_bp);

  if (header->snap_count) {
    int snaps_len = 0;
    int names_len = 0;
    CLS_LOG(20, "i=%u\n", i);
    if (i > 0) {
      snaps_len = sizeof(header->snaps[0]) * i;
      names_len =  snap_names - orig_names;
      memcpy(new_snaps_bp.c_str(), header->snaps, snaps_len);
      memcpy(new_names_bp.c_str(), orig_names, names_len);
    }
    snap_names += s.length() + 1;

    if (i < header->snap_count) {
      memcpy(new_snaps_bp.c_str() + snaps_len,
             header->snaps + i + 1,
             sizeof(header->snaps[0]) * (header->snap_count - i));
      memcpy(new_names_bp.c_str() + names_len, snap_names , end - snap_names);
    }
    newbl.push_back(new_snaps_bp);
    newbl.push_back(new_names_bp);
  }

  rc = cls_cxx_write_full(hctx, &newbl);
  if (rc < 0)
    return rc;

  return 0;
}

/**
 * rename snapshot of old format.
 *
 * Input:
 * @param src_snap_id old snap id of the snapshot (snapid_t)
 * @param dst_snap_name new name of the snapshot (string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure.
*/
int old_snapshot_rename(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist bl;
  struct rbd_obj_header_ondisk *header;
  bufferlist newbl;
  bufferptr header_bp(sizeof(*header));
  snapid_t src_snap_id;
  const char *dst_snap_name;
  string dst;

  int rc = snap_read_header(hctx, bl);
  if (rc < 0)
    return rc;

  header = (struct rbd_obj_header_ondisk *)bl.c_str();

  int snaps_id_ofs = sizeof(*header);
  int names_ofs = snaps_id_ofs + sizeof(rbd_obj_snap_ondisk) * header->snap_count;
  const char *snap_names = ((char *)header) + names_ofs;
  const char *orig_names = snap_names;
  const char *end = snap_names + header->snap_names_len;
  bufferlist::iterator iter = in->begin();
  unsigned i;
  bool found = false;

  try {
    ::decode(src_snap_id, iter);
    ::decode(dst, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }
  dst_snap_name = dst.c_str();

  const char *cur_snap_name;
  for (cur_snap_name = snap_names; cur_snap_name < end; 
    cur_snap_name += strlen(cur_snap_name) + 1) {
    if (strcmp(cur_snap_name, dst_snap_name) == 0)
      return -EEXIST;
  }
  if (cur_snap_name > end)
    return -EIO;
  for (i = 0; i < header->snap_count; i++) {
    if (src_snap_id == header->snaps[i].id) {
      found = true;
      break;
    }
    snap_names += strlen(snap_names) + 1;
  }
  if (!found) {
    CLS_ERR("couldn't find snap %llu\n", (unsigned long long)src_snap_id.val);
    return -ENOENT;
  }
  
  CLS_LOG(20, "rename snap with snap id %llu to dest name %s", (unsigned long long)src_snap_id.val, dst_snap_name);
  header->snap_names_len  = header->snap_names_len - strlen(snap_names) + dst.length();

  bufferptr new_names_bp(header->snap_names_len);
  bufferptr new_snaps_bp(sizeof(header->snaps[0]) * header->snap_count);

  if (header->snap_count) {
    int names_len = 0;
    CLS_LOG(20, "i=%u\n", i);
    if (i > 0) {
      names_len =  snap_names - orig_names;
      memcpy(new_names_bp.c_str(), orig_names, names_len);
    }
    strcpy(new_names_bp.c_str() + names_len, dst_snap_name);
    names_len += strlen(dst_snap_name) + 1;
    snap_names += strlen(snap_names) + 1;
    if (i < header->snap_count) {
      memcpy(new_names_bp.c_str() + names_len, snap_names , end - snap_names);
    }
    memcpy(new_snaps_bp.c_str(), header->snaps, sizeof(header->snaps[0]) * header->snap_count);
  }

  memcpy(header_bp.c_str(), header, sizeof(*header));
  newbl.push_back(header_bp);
  newbl.push_back(new_snaps_bp);
  newbl.push_back(new_names_bp);

  rc = cls_cxx_write_full(hctx, &newbl);
  if (rc < 0)
    return rc;
  return 0;
}


namespace mirror {

static const std::string UUID("mirror_uuid");
static const std::string MODE("mirror_mode");
static const std::string PEER_KEY_PREFIX("mirror_peer_");
static const std::string IMAGE_KEY_PREFIX("image_");
static const std::string GLOBAL_KEY_PREFIX("global_");
static const std::string STATUS_GLOBAL_KEY_PREFIX("status_global_");
static const std::string INSTANCE_KEY_PREFIX("instance_");

std::string peer_key(const std::string &uuid) {
  return PEER_KEY_PREFIX + uuid;
}

std::string image_key(const string &image_id) {
  return IMAGE_KEY_PREFIX + image_id;
}

std::string global_key(const string &global_id) {
  return GLOBAL_KEY_PREFIX + global_id;
}

std::string status_global_key(const string &global_id) {
  return STATUS_GLOBAL_KEY_PREFIX + global_id;
}

std::string instance_key(const string &instance_id) {
  return INSTANCE_KEY_PREFIX + instance_id;
}

int uuid_get(cls_method_context_t hctx, std::string *mirror_uuid) {
  bufferlist mirror_uuid_bl;
  int r = cls_cxx_map_get_val(hctx, mirror::UUID, &mirror_uuid_bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading mirror uuid: %s", cpp_strerror(r).c_str());
    }
    return r;
  }

  *mirror_uuid = std::string(mirror_uuid_bl.c_str(), mirror_uuid_bl.length());
  return 0;
}

int list_watchers(cls_method_context_t hctx,
                  std::set<entity_inst_t> *entities) {
  obj_list_watch_response_t watchers;
  int r = cls_cxx_list_watchers(hctx, &watchers);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("error listing watchers: '%s'", cpp_strerror(r).c_str());
    return r;
  }

  entities->clear();
  for (auto &w : watchers.entries) {
    entities->emplace(w.name, w.addr);
  }
  return 0;
}

int read_peers(cls_method_context_t hctx,
               std::vector<cls::rbd::MirrorPeer> *peers) {
  std::string last_read = PEER_KEY_PREFIX;
  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  while (more) {
    std::map<std::string, bufferlist> vals;
    int r = cls_cxx_map_get_vals(hctx, last_read, PEER_KEY_PREFIX.c_str(),
                                 max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading peers: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (auto &it : vals) {
      try {
        bufferlist::iterator bl_it = it.second.begin();
        cls::rbd::MirrorPeer peer;
	::decode(peer, bl_it);
        peers->push_back(peer);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode peer '%s'", it.first.c_str());
	return -EIO;
      }
    }

    if (!vals.empty()) {
      last_read = vals.rbegin()->first;
    }
  }
  return 0;
}

int read_peer(cls_method_context_t hctx, const std::string &id,
              cls::rbd::MirrorPeer *peer) {
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, peer_key(id), &bl);
  if (r < 0) {
    CLS_ERR("error reading peer '%s': %s", id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }

  try {
    bufferlist::iterator bl_it = bl.begin();
    ::decode(*peer, bl_it);
  } catch (const buffer::error &err) {
    CLS_ERR("could not decode peer '%s'", id.c_str());
    return -EIO;
  }
  return 0;
}

int write_peer(cls_method_context_t hctx, const std::string &id,
               const cls::rbd::MirrorPeer &peer) {
  bufferlist bl;
  ::encode(peer, bl);

  int r = cls_cxx_map_set_val(hctx, peer_key(id), &bl);
  if (r < 0) {
    CLS_ERR("error writing peer '%s': %s", id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

int image_get(cls_method_context_t hctx, const string &image_id,
	      cls::rbd::MirrorImage *mirror_image) {
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, image_key(image_id), &bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading mirrored image '%s': '%s'", image_id.c_str(),
	      cpp_strerror(r).c_str());
    }
    return r;
  }

  try {
    bufferlist::iterator it = bl.begin();
    ::decode(*mirror_image, it);
  } catch (const buffer::error &err) {
    CLS_ERR("could not decode mirrored image '%s'", image_id.c_str());
    return -EIO;
  }

  return 0;
}

int image_set(cls_method_context_t hctx, const string &image_id,
	      const cls::rbd::MirrorImage &mirror_image) {
  bufferlist bl;
  ::encode(mirror_image, bl);

  cls::rbd::MirrorImage existing_mirror_image;
  int r = image_get(hctx, image_id, &existing_mirror_image);
  if (r == -ENOENT) {
    // make sure global id doesn't already exist
    std::string global_id_key = global_key(mirror_image.global_image_id);
    std::string image_id;
    r = read_key(hctx, global_id_key, &image_id);
    if (r >= 0) {
      return -EEXIST;
    } else if (r != -ENOENT) {
      CLS_ERR("error reading global image id: '%s': '%s'", image_id.c_str(),
              cpp_strerror(r).c_str());
      return r;
    }

    // make sure this was not a race for disabling
    if (mirror_image.state == cls::rbd::MIRROR_IMAGE_STATE_DISABLING) {
      CLS_ERR("image '%s' is already disabled", image_id.c_str());
      return r;
    }
  } else if (r < 0) {
    CLS_ERR("error reading mirrored image '%s': '%s'", image_id.c_str(),
	    cpp_strerror(r).c_str());
    return r;
  } else if (existing_mirror_image.global_image_id !=
                mirror_image.global_image_id) {
    // cannot change the global id
    return -EINVAL;
  }

  r = cls_cxx_map_set_val(hctx, image_key(image_id), &bl);
  if (r < 0) {
    CLS_ERR("error adding mirrored image '%s': %s", image_id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }

  bufferlist image_id_bl;
  ::encode(image_id, image_id_bl);
  r = cls_cxx_map_set_val(hctx, global_key(mirror_image.global_image_id),
                          &image_id_bl);
  if (r < 0) {
    CLS_ERR("error adding global id for image '%s': %s", image_id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

int image_remove(cls_method_context_t hctx, const string &image_id) {
  bufferlist bl;
  cls::rbd::MirrorImage mirror_image;
  int r = image_get(hctx, image_id, &mirror_image);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading mirrored image '%s': '%s'", image_id.c_str(),
	      cpp_strerror(r).c_str());
    }
    return r;
  }

  if (mirror_image.state != cls::rbd::MIRROR_IMAGE_STATE_DISABLING) {
    return -EBUSY;
  }

  r = cls_cxx_map_remove_key(hctx, image_key(image_id));
  if (r < 0) {
    CLS_ERR("error removing mirrored image '%s': %s", image_id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }

  r = cls_cxx_map_remove_key(hctx, global_key(mirror_image.global_image_id));
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("error removing global id for image '%s': %s", image_id.c_str(),
           cpp_strerror(r).c_str());
    return r;
  }

  r = cls_cxx_map_remove_key(hctx,
                             status_global_key(mirror_image.global_image_id));
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("error removing global status for image '%s': %s", image_id.c_str(),
           cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

struct MirrorImageStatusOnDisk : cls::rbd::MirrorImageStatus {
  entity_inst_t origin;

  MirrorImageStatusOnDisk() {
  }
  MirrorImageStatusOnDisk(const cls::rbd::MirrorImageStatus &status) :
    cls::rbd::MirrorImageStatus(status) {
  }

  void encode_meta(bufferlist &bl, uint64_t features) const {
    ENCODE_START(1, 1, bl);
    ::encode(origin, bl, features);
    ENCODE_FINISH(bl);
  }

  void encode(bufferlist &bl, uint64_t features) const {
    encode_meta(bl, features);
    cls::rbd::MirrorImageStatus::encode(bl);
  }

  void decode_meta(bufferlist::iterator &it) {
    DECODE_START(1, it);
    ::decode(origin, it);
    DECODE_FINISH(it);
  }

  void decode(bufferlist::iterator &it) {
    decode_meta(it);
    cls::rbd::MirrorImageStatus::decode(it);
  }
};
WRITE_CLASS_ENCODER_FEATURES(MirrorImageStatusOnDisk)

int image_status_set(cls_method_context_t hctx, const string &global_image_id,
		     const cls::rbd::MirrorImageStatus &status) {
  MirrorImageStatusOnDisk ondisk_status(status);
  ondisk_status.up = false;
  ondisk_status.last_update = ceph_clock_now();

  int r = cls_get_request_origin(hctx, &ondisk_status.origin);
  assert(r == 0);

  bufferlist bl;
  encode(ondisk_status, bl, cls_get_features(hctx));

  r = cls_cxx_map_set_val(hctx, status_global_key(global_image_id), &bl);
  if (r < 0) {
    CLS_ERR("error setting status for mirrored image, global id '%s': %s",
	    global_image_id.c_str(), cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

int image_status_remove(cls_method_context_t hctx,
			const string &global_image_id) {

  int r = cls_cxx_map_remove_key(hctx, status_global_key(global_image_id));
  if (r < 0) {
    CLS_ERR("error removing status for mirrored image, global id '%s': %s",
	    global_image_id.c_str(), cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

int image_status_get(cls_method_context_t hctx, const string &global_image_id,
                     const std::set<entity_inst_t> &watchers,
		     cls::rbd::MirrorImageStatus *status) {

  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, status_global_key(global_image_id), &bl);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading status for mirrored image, global id '%s': '%s'",
	      global_image_id.c_str(), cpp_strerror(r).c_str());
    }
    return r;
  }

  MirrorImageStatusOnDisk ondisk_status;
  try {
    bufferlist::iterator it = bl.begin();
    decode(ondisk_status, it);
  } catch (const buffer::error &err) {
    CLS_ERR("could not decode status for mirrored image, global id '%s'",
	    global_image_id.c_str());
    return -EIO;
  }


  *status = static_cast<cls::rbd::MirrorImageStatus>(ondisk_status);
  status->up = (watchers.find(ondisk_status.origin) != watchers.end());
  return 0;
}

int image_status_list(cls_method_context_t hctx,
	const std::string &start_after, uint64_t max_return,
	map<std::string, cls::rbd::MirrorImage> *mirror_images,
        map<std::string, cls::rbd::MirrorImageStatus> *mirror_statuses) {
  std::string last_read = image_key(start_after);
  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;

  std::set<entity_inst_t> watchers;
  int r = list_watchers(hctx, &watchers);
  if (r < 0) {
    return r;
  }

  while (more && mirror_images->size() < max_return) {
    std::map<std::string, bufferlist> vals;
    CLS_LOG(20, "last_read = '%s'", last_read.c_str());
    r = cls_cxx_map_get_vals(hctx, last_read, IMAGE_KEY_PREFIX, max_read, &vals,
                             &more);
    if (r < 0) {
      CLS_ERR("error reading mirror image directory by name: %s",
              cpp_strerror(r).c_str());
      return r;
    }

    for (auto it = vals.begin(); it != vals.end() &&
	   mirror_images->size() < max_return; ++it) {
      const std::string &image_id = it->first.substr(IMAGE_KEY_PREFIX.size());
      cls::rbd::MirrorImage mirror_image;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(mirror_image, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode mirror image payload of image '%s'",
                image_id.c_str());
	return -EIO;
      }

      (*mirror_images)[image_id] = mirror_image;

      cls::rbd::MirrorImageStatus status;
      int r1 = image_status_get(hctx, mirror_image.global_image_id, watchers,
                                &status);
      if (r1 < 0) {
	continue;
      }

      (*mirror_statuses)[image_id] = status;
    }
    if (!vals.empty()) {
      last_read = image_key(mirror_images->rbegin()->first);
    }
  }

  return 0;
}

int image_status_get_summary(cls_method_context_t hctx,
	std::map<cls::rbd::MirrorImageStatusState, int> *states) {
  std::set<entity_inst_t> watchers;
  int r = list_watchers(hctx, &watchers);
  if (r < 0) {
    return r;
  }

  states->clear();

  string last_read = IMAGE_KEY_PREFIX;
  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  while (more) {
    map<string, bufferlist> vals;
    r = cls_cxx_map_get_vals(hctx, last_read, IMAGE_KEY_PREFIX,
			     max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading mirrored images: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (auto &list_it : vals) {
      const string &key = list_it.first;

      if (0 != key.compare(0, IMAGE_KEY_PREFIX.size(), IMAGE_KEY_PREFIX)) {
	break;
      }

      cls::rbd::MirrorImage mirror_image;
      bufferlist::iterator iter = list_it.second.begin();
      try {
	::decode(mirror_image, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode mirror image payload for key '%s'",
                key.c_str());
	return -EIO;
      }

      cls::rbd::MirrorImageStatus status;
      image_status_get(hctx, mirror_image.global_image_id, watchers, &status);

      cls::rbd::MirrorImageStatusState state = status.up ? status.state :
	cls::rbd::MIRROR_IMAGE_STATUS_STATE_UNKNOWN;
      (*states)[state]++;
    }

    if (!vals.empty()) {
      last_read = vals.rbegin()->first;
    }
  }

  return 0;
}

int image_status_remove_down(cls_method_context_t hctx) {
  std::set<entity_inst_t> watchers;
  int r = list_watchers(hctx, &watchers);
  if (r < 0) {
    return r;
  }

  string last_read = STATUS_GLOBAL_KEY_PREFIX;
  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  while (more) {
    map<string, bufferlist> vals;
    r = cls_cxx_map_get_vals(hctx, last_read, STATUS_GLOBAL_KEY_PREFIX,
			     max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading mirrored images: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (auto &list_it : vals) {
      const string &key = list_it.first;

      if (0 != key.compare(0, STATUS_GLOBAL_KEY_PREFIX.size(),
			   STATUS_GLOBAL_KEY_PREFIX)) {
	break;
      }

      MirrorImageStatusOnDisk status;
      try {
	bufferlist::iterator it = list_it.second.begin();
	status.decode_meta(it);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode status metadata for mirrored image '%s'",
		key.c_str());
	return -EIO;
      }

      if (watchers.find(status.origin) == watchers.end()) {
	CLS_LOG(20, "removing stale status object for key %s",
		key.c_str());
	int r1 = cls_cxx_map_remove_key(hctx, key);
	if (r1 < 0) {
	  CLS_ERR("error removing stale status for key '%s': %s",
		  key.c_str(), cpp_strerror(r1).c_str());
	  return r1;
	}
      }
    }

    if (!vals.empty()) {
      last_read = vals.rbegin()->first;
    }
  }

  return 0;
}

int instances_list(cls_method_context_t hctx,
                   std::vector<std::string> *instance_ids) {
  std::string last_read = INSTANCE_KEY_PREFIX;
  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  while (more) {
    std::map<std::string, bufferlist> vals;
    int r = cls_cxx_map_get_vals(hctx, last_read, INSTANCE_KEY_PREFIX.c_str(),
                                 max_read, &vals, &more);
    if (r < 0) {
      if (r != -ENOENT) {
	CLS_ERR("error reading mirror instances: %s", cpp_strerror(r).c_str());
      }
      return r;
    }

    for (auto &it : vals) {
      instance_ids->push_back(it.first.substr(INSTANCE_KEY_PREFIX.size()));
    }

    if (!vals.empty()) {
      last_read = vals.rbegin()->first;
    }
  }
  return 0;
}

int instances_add(cls_method_context_t hctx, const string &instance_id) {
  bufferlist bl;

  int r = cls_cxx_map_set_val(hctx, instance_key(instance_id), &bl);
  if (r < 0) {
    CLS_ERR("error setting mirror instance %s: %s", instance_id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

int instances_remove(cls_method_context_t hctx, const string &instance_id) {

  int r = cls_cxx_map_remove_key(hctx, instance_key(instance_id));
  if (r < 0) {
    CLS_ERR("error removing mirror instance %s: %s", instance_id.c_str(),
            cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

} // namespace mirror

/**
 * Input:
 * none
 *
 * Output:
 * @param uuid (std::string)
 * @returns 0 on success, negative error code on failure
 */
int mirror_uuid_get(cls_method_context_t hctx, bufferlist *in,
                    bufferlist *out) {
  std::string mirror_uuid;
  int r = mirror::uuid_get(hctx, &mirror_uuid);
  if (r < 0) {
    return r;
  }

  ::encode(mirror_uuid, *out);
  return 0;
}

/**
 * Input:
 * @param mirror_uuid (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_uuid_set(cls_method_context_t hctx, bufferlist *in,
                    bufferlist *out) {
  std::string mirror_uuid;
  try {
    bufferlist::iterator bl_it = in->begin();
    ::decode(mirror_uuid, bl_it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (mirror_uuid.empty()) {
    CLS_ERR("cannot set empty mirror uuid");
    return -EINVAL;
  }

  uint32_t mirror_mode;
  int r = read_key(hctx, mirror::MODE, &mirror_mode);
  if (r < 0 && r != -ENOENT) {
    return r;
  } else if (r == 0 && mirror_mode != cls::rbd::MIRROR_MODE_DISABLED) {
    CLS_ERR("cannot set mirror uuid while mirroring enabled");
    return -EINVAL;
  }

  bufferlist mirror_uuid_bl;
  mirror_uuid_bl.append(mirror_uuid);
  r = cls_cxx_map_set_val(hctx, mirror::UUID, &mirror_uuid_bl);
  if (r < 0) {
    CLS_ERR("failed to set mirror uuid");
    return r;
  }
  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @param cls::rbd::MirrorMode (uint32_t)
 * @returns 0 on success, negative error code on failure
 */
int mirror_mode_get(cls_method_context_t hctx, bufferlist *in,
                    bufferlist *out) {
  uint32_t mirror_mode_decode;
  int r = read_key(hctx, mirror::MODE, &mirror_mode_decode);
  if (r < 0) {
    return r;
  }

  ::encode(mirror_mode_decode, *out);
  return 0;
}

/**
 * Input:
 * @param mirror_mode (cls::rbd::MirrorMode) (uint32_t)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_mode_set(cls_method_context_t hctx, bufferlist *in,
                    bufferlist *out) {
  uint32_t mirror_mode_decode;
  try {
    bufferlist::iterator bl_it = in->begin();
    ::decode(mirror_mode_decode, bl_it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  bool enabled;
  switch (static_cast<cls::rbd::MirrorMode>(mirror_mode_decode)) {
  case cls::rbd::MIRROR_MODE_DISABLED:
    enabled = false;
    break;
  case cls::rbd::MIRROR_MODE_IMAGE:
  case cls::rbd::MIRROR_MODE_POOL:
    enabled = true;
    break;
  default:
    CLS_ERR("invalid mirror mode: %d", mirror_mode_decode);
    return -EINVAL;
  }

  int r;
  if (enabled) {
    std::string mirror_uuid;
    r = mirror::uuid_get(hctx, &mirror_uuid);
    if (r == -ENOENT) {
      return -EINVAL;
    } else if (r < 0) {
      return r;
    }

    bufferlist bl;
    ::encode(mirror_mode_decode, bl);

    r = cls_cxx_map_set_val(hctx, mirror::MODE, &bl);
    if (r < 0) {
      CLS_ERR("error enabling mirroring: %s", cpp_strerror(r).c_str());
      return r;
    }
  } else {
    std::vector<cls::rbd::MirrorPeer> peers;
    r = mirror::read_peers(hctx, &peers);
    if (r < 0 && r != -ENOENT) {
      return r;
    }

    if (!peers.empty()) {
      CLS_ERR("mirroring peers still registered");
      return -EBUSY;
    }

    r = remove_key(hctx, mirror::MODE);
    if (r < 0) {
      return r;
    }

    r = remove_key(hctx, mirror::UUID);
    if (r < 0) {
      return r;
    }
  }
  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @param std::vector<cls::rbd::MirrorPeer>: collection of peers
 * @returns 0 on success, negative error code on failure
 */
int mirror_peer_list(cls_method_context_t hctx, bufferlist *in,
                     bufferlist *out) {
  std::vector<cls::rbd::MirrorPeer> peers;
  int r = mirror::read_peers(hctx, &peers);
  if (r < 0 && r != -ENOENT) {
    return r;
  }

  ::encode(peers, *out);
  return 0;
}

/**
 * Input:
 * @param mirror_peer (cls::rbd::MirrorPeer)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_peer_add(cls_method_context_t hctx, bufferlist *in,
                    bufferlist *out) {
  cls::rbd::MirrorPeer mirror_peer;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(mirror_peer, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  uint32_t mirror_mode_decode;
  int r = read_key(hctx, mirror::MODE, &mirror_mode_decode);
  if (r < 0 && r != -ENOENT) {
    return r;
  } else if (r == -ENOENT ||
             mirror_mode_decode == cls::rbd::MIRROR_MODE_DISABLED) {
    CLS_ERR("mirroring must be enabled on the pool");
    return -EINVAL;
  } else if (!mirror_peer.is_valid()) {
    CLS_ERR("mirror peer is not valid");
    return -EINVAL;
  }

  std::string mirror_uuid;
  r = mirror::uuid_get(hctx, &mirror_uuid);
  if (r < 0) {
    CLS_ERR("error retrieving mirroring uuid: %s", cpp_strerror(r).c_str());
    return r;
  } else if (mirror_peer.uuid == mirror_uuid) {
    CLS_ERR("peer uuid '%s' matches pool mirroring uuid",
            mirror_uuid.c_str());
    return -EINVAL;
  }

  std::vector<cls::rbd::MirrorPeer> peers;
  r = mirror::read_peers(hctx, &peers);
  if (r < 0 && r != -ENOENT) {
    return r;
  }

  for (auto const &peer : peers) {
    if (peer.uuid == mirror_peer.uuid) {
      CLS_ERR("peer uuid '%s' already exists",
              peer.uuid.c_str());
      return -ESTALE;
    } else if (peer.cluster_name == mirror_peer.cluster_name &&
               (peer.pool_id == -1 || mirror_peer.pool_id == -1 ||
                peer.pool_id == mirror_peer.pool_id)) {
      CLS_ERR("peer cluster name '%s' already exists",
              peer.cluster_name.c_str());
      return -EEXIST;
    }
  }

  bufferlist bl;
  ::encode(mirror_peer, bl);
  r = cls_cxx_map_set_val(hctx, mirror::peer_key(mirror_peer.uuid),
                          &bl);
  if (r < 0) {
    CLS_ERR("error adding peer: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param uuid (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_peer_remove(cls_method_context_t hctx, bufferlist *in,
                       bufferlist *out) {
  std::string uuid;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(uuid, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = cls_cxx_map_remove_key(hctx, mirror::peer_key(uuid));
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("error removing peer: %s", cpp_strerror(r).c_str());
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param uuid (std::string)
 * @param client_name (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_peer_set_client(cls_method_context_t hctx, bufferlist *in,
                           bufferlist *out) {
  std::string uuid;
  std::string client_name;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(uuid, it);
    ::decode(client_name, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  cls::rbd::MirrorPeer peer;
  int r = mirror::read_peer(hctx, uuid, &peer);
  if (r < 0) {
    return r;
  }

  peer.client_name = client_name;
  r = mirror::write_peer(hctx, uuid, peer);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param uuid (std::string)
 * @param cluster_name (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_peer_set_cluster(cls_method_context_t hctx, bufferlist *in,
                            bufferlist *out) {
  std::string uuid;
  std::string cluster_name;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(uuid, it);
    ::decode(cluster_name, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  cls::rbd::MirrorPeer peer;
  int r = mirror::read_peer(hctx, uuid, &peer);
  if (r < 0) {
    return r;
  }

  peer.cluster_name = cluster_name;
  r = mirror::write_peer(hctx, uuid, peer);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param std::map<std::string, std::string>: local id to global id map
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_list(cls_method_context_t hctx, bufferlist *in,
		     bufferlist *out) {
  std::string start_after;
  uint64_t max_return;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  std::map<std::string, std::string> mirror_images;
  std::string last_read = mirror::image_key(start_after);

  while (more && mirror_images.size() < max_return) {
    std::map<std::string, bufferlist> vals;
    CLS_LOG(20, "last_read = '%s'", last_read.c_str());
    int r = cls_cxx_map_get_vals(hctx, last_read, mirror::IMAGE_KEY_PREFIX,
                                 max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading mirror image directory by name: %s",
              cpp_strerror(r).c_str());
      return r;
    }

    for (auto it = vals.begin(); it != vals.end(); ++it) {
      const std::string &image_id =
        it->first.substr(mirror::IMAGE_KEY_PREFIX.size());
      cls::rbd::MirrorImage mirror_image;
      bufferlist::iterator iter = it->second.begin();
      try {
	::decode(mirror_image, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode mirror image payload of image '%s'",
                image_id.c_str());
	return -EIO;
      }

      mirror_images[image_id] = mirror_image.global_image_id;
      if (mirror_images.size() >= max_return) {
	break;
      }
    }
    if (!vals.empty()) {
      last_read = mirror::image_key(mirror_images.rbegin()->first);
    }
  }

  ::encode(mirror_images, *out);
  return 0;
}

/**
 * Input:
 * @param global_id (std::string)
 *
 * Output:
 * @param std::string - image id
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_get_image_id(cls_method_context_t hctx, bufferlist *in,
                              bufferlist *out) {
  std::string global_id;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(global_id, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  std::string image_id;
  int r = read_key(hctx, mirror::global_key(global_id), &image_id);
  if (r < 0) {
    CLS_ERR("error retrieving image id for global id '%s': %s",
            global_id.c_str(), cpp_strerror(r).c_str());
    return r;
  }

  ::encode(image_id, *out);
  return 0;
}

/**
 * Input:
 * @param image_id (std::string)
 *
 * Output:
 * @param cls::rbd::MirrorImage - metadata associated with the image_id
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_get(cls_method_context_t hctx, bufferlist *in,
		     bufferlist *out) {
  string image_id;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(image_id, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  cls::rbd::MirrorImage mirror_image;
  int r = mirror::image_get(hctx, image_id, &mirror_image);
  if (r < 0) {
    return r;
  }

  ::encode(mirror_image, *out);
  return 0;
}

/**
 * Input:
 * @param image_id (std::string)
 * @param mirror_image (cls::rbd::MirrorImage)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 * @returns -EEXIST if there's an existing image_id with a different global_image_id
 */
int mirror_image_set(cls_method_context_t hctx, bufferlist *in,
		     bufferlist *out) {
  string image_id;
  cls::rbd::MirrorImage mirror_image;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(image_id, it);
    ::decode(mirror_image, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::image_set(hctx, image_id, mirror_image);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param image_id (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_remove(cls_method_context_t hctx, bufferlist *in,
			bufferlist *out) {
  string image_id;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(image_id, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::image_remove(hctx, image_id);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param global_image_id (std::string)
 * @param status (cls::rbd::MirrorImageStatus)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_set(cls_method_context_t hctx, bufferlist *in,
			    bufferlist *out) {
  string global_image_id;
  cls::rbd::MirrorImageStatus status;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(global_image_id, it);
    ::decode(status, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::image_status_set(hctx, global_image_id, status);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param global_image_id (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_remove(cls_method_context_t hctx, bufferlist *in,
			       bufferlist *out) {
  string global_image_id;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(global_image_id, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::image_status_remove(hctx, global_image_id);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param global_image_id (std::string)
 *
 * Output:
 * @param cls::rbd::MirrorImageStatus - metadata associated with the global_image_id
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_get(cls_method_context_t hctx, bufferlist *in,
			    bufferlist *out) {
  string global_image_id;
  try {
    bufferlist::iterator it = in->begin();
    ::decode(global_image_id, it);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  std::set<entity_inst_t> watchers;
  int r = mirror::list_watchers(hctx, &watchers);
  if (r < 0) {
    return r;
  }

  cls::rbd::MirrorImageStatus status;
  r = mirror::image_status_get(hctx, global_image_id, watchers, &status);
  if (r < 0) {
    return r;
  }

  ::encode(status, *out);
  return 0;
}

/**
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param std::map<std::string, cls::rbd::MirrorImage>: image id to image map
 * @param std::map<std::string, cls::rbd::MirrorImageStatus>: image it to status map
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_list(cls_method_context_t hctx, bufferlist *in,
			     bufferlist *out) {
  std::string start_after;
  uint64_t max_return;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  map<std::string, cls::rbd::MirrorImage> images;
  map<std::string, cls::rbd::MirrorImageStatus> statuses;
  int r = mirror::image_status_list(hctx, start_after, max_return, &images,
				    &statuses);
  if (r < 0) {
    return r;
  }

  ::encode(images, *out);
  ::encode(statuses, *out);
  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @param std::map<cls::rbd::MirrorImageStatusState, int>: states counts
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_get_summary(cls_method_context_t hctx, bufferlist *in,
				    bufferlist *out) {
  std::map<cls::rbd::MirrorImageStatusState, int> states;

  int r = mirror::image_status_get_summary(hctx, &states);
  if (r < 0) {
    return r;
  }

  ::encode(states, *out);
  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_image_status_remove_down(cls_method_context_t hctx, bufferlist *in,
				    bufferlist *out) {
  int r = mirror::image_status_remove_down(hctx);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * none
 *
 * Output:
 * @param std::vector<std::string>: instance ids
 * @returns 0 on success, negative error code on failure
 */
int mirror_instances_list(cls_method_context_t hctx, bufferlist *in,
                          bufferlist *out) {
  std::vector<std::string> instance_ids;

  int r = mirror::instances_list(hctx, &instance_ids);
  if (r < 0) {
    return r;
  }

  ::encode(instance_ids, *out);
  return 0;
}

/**
 * Input:
 * @param instance_id (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_instances_add(cls_method_context_t hctx, bufferlist *in,
                         bufferlist *out) {
  std::string instance_id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(instance_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::instances_add(hctx, instance_id);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Input:
 * @param instance_id (std::string)
 *
 * Output:
 * @returns 0 on success, negative error code on failure
 */
int mirror_instances_remove(cls_method_context_t hctx, bufferlist *in,
                            bufferlist *out) {
  std::string instance_id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(instance_id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int r = mirror::instances_remove(hctx, instance_id);
  if (r < 0) {
    return r;
  }
  return 0;
}

/**
 * Initialize the header with basic metadata.
 * Everything is stored as key/value pairs as omaps in the header object.
 *
 * Input:
 * none
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int group_create(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist snap_seqbl;
  uint64_t snap_seq = 0;
  ::encode(snap_seq, snap_seqbl);
  int r = cls_cxx_map_set_val(hctx, GROUP_SNAP_SEQ, &snap_seqbl);
  if (r < 0)
    return r;

  return 0;
}

/**
 * List consistency groups from the directory.
 *
 * Input:
 * @param start_after (std::string)
 * @param max_return (int64_t)
 *
 * Output:
 * @param map of consistency groups (name, id)
 * @return 0 on success, negative error code on failure
 */
int group_dir_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string start_after;
  uint64_t max_return;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int max_read = RBD_MAX_KEYS_READ;
  bool more = true;
  map<string, string> groups;
  string last_read = dir_key_for_name(start_after);

  while (more && groups.size() < max_return) {
    map<string, bufferlist> vals;
    CLS_LOG(20, "last_read = '%s'", last_read.c_str());
    int r = cls_cxx_map_get_vals(hctx, last_read, RBD_DIR_NAME_KEY_PREFIX,
                                 max_read, &vals, &more);
    if (r < 0) {
      CLS_ERR("error reading directory by name: %s", cpp_strerror(r).c_str());
      return r;
    }

    for (pair<string, bufferlist> val: vals) {
      string id;
      bufferlist::iterator iter = val.second.begin();
      try {
	::decode(id, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("could not decode id of consistency group '%s'", val.first.c_str());
	return -EIO;
      }
      CLS_LOG(20, "adding '%s' -> '%s'", dir_name_from_key(val.first).c_str(), id.c_str());
      groups[dir_name_from_key(val.first)] = id;
      if (groups.size() >= max_return)
	break;
    }
    if (!vals.empty()) {
      last_read = dir_key_for_name(groups.rbegin()->first);
    }
  }

  ::encode(groups, *out);

  return 0;
}

/**
 * Add a consistency group to the directory.
 *
 * Input:
 * @param name (std::string)
 * @param id (std::string)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int group_dir_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = cls_cxx_create(hctx, false);

  if (r < 0) {
    CLS_ERR("could not create consistency group directory: %s",
	    cpp_strerror(r).c_str());
    return r;
  }

  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (!name.size() || !is_valid_id(id)) {
    CLS_ERR("invalid consistency group name '%s' or id '%s'",
	    name.c_str(), id.c_str());
    return -EINVAL;
  }

  CLS_LOG(20, "group_dir_add name=%s id=%s", name.c_str(), id.c_str());

  string tmp;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);
  r = read_key(hctx, name_key, &tmp);
  if (r != -ENOENT) {
    CLS_LOG(10, "name already exists");
    return -EEXIST;
  }
  r = read_key(hctx, id_key, &tmp);
  if (r != -ENOENT) {
    CLS_LOG(10, "id already exists");
    return -EBADF;
  }
  bufferlist id_bl, name_bl;
  ::encode(id, id_bl);
  ::encode(name, name_bl);
  map<string, bufferlist> omap_vals;
  omap_vals[name_key] = id_bl;
  omap_vals[id_key] = name_bl;
  return cls_cxx_map_set_vals(hctx, &omap_vals);
}

/**
 * Remove a consistency group from the directory.
 *
 * Input:
 * @param name (std::string)
 * @param id (std::string)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int group_dir_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string name, id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(name, iter);
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "group_dir_remove name=%s id=%s", name.c_str(), id.c_str());

  string stored_name, stored_id;
  string name_key = dir_key_for_name(name);
  string id_key = dir_key_for_id(id);

  int r = read_key(hctx, name_key, &stored_id);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading name to id mapping: %s", cpp_strerror(r).c_str());
    return r;
  }
  r = read_key(hctx, id_key, &stored_name);
  if (r < 0) {
    if (r != -ENOENT)
      CLS_ERR("error reading id to name mapping: %s", cpp_strerror(r).c_str());
    return r;
  }

  // check if this op raced with a rename
  if (stored_name != name || stored_id != id) {
    CLS_ERR("stored name '%s' and id '%s' do not match args '%s' and '%s'",
	    stored_name.c_str(), stored_id.c_str(), name.c_str(), id.c_str());
    return -ESTALE;
  }

  r = cls_cxx_map_remove_key(hctx, name_key);
  if (r < 0) {
    CLS_ERR("error removing name: %s", cpp_strerror(r).c_str());
    return r;
  }

  r = cls_cxx_map_remove_key(hctx, id_key);
  if (r < 0) {
    CLS_ERR("error removing id: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Set state of an image in the consistency group.
 *
 * Input:
 * @param image_status (cls::rbd::GroupImageStatus)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int group_image_set(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "group_image_set");

  cls::rbd::GroupImageStatus st;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(st, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  string image_key = st.spec.image_key();

  bufferlist image_val_bl;
  ::encode(st.state, image_val_bl);
  int r = cls_cxx_map_set_val(hctx, image_key, &image_val_bl);
  if (r < 0) {
    return r;
  }

  return 0;
}

/**
 * Remove reference to an image from the consistency group.
 *
 * Input:
 * @param spec (cls::rbd::GroupImageSpec)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int group_image_remove(cls_method_context_t hctx,
                       bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "group_image_remove");
  cls::rbd::GroupImageSpec spec;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(spec, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  string image_key = spec.image_key();

  int r = cls_cxx_map_remove_key(hctx, image_key);
  if (r < 0) {
    CLS_ERR("error removing image from group: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/*
 * List images in the consistency group.
 *
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param tuples of descriptions of the images: image_id, pool_id, image reference state.
 * @return 0 on success, negative error code on failure
 */
int group_image_list(cls_method_context_t hctx,
                     bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "group_image_list");
  cls::rbd::GroupImageSpec start_after;
  uint64_t max_return;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  int max_read = RBD_MAX_KEYS_READ;
  std::map<string, bufferlist> vals;
  string last_read = start_after.image_key();
  std::vector<cls::rbd::GroupImageStatus> res;
  bool more;
  do {
    int r = cls_cxx_map_get_vals(hctx, last_read,cls::rbd::RBD_GROUP_IMAGE_KEY_PREFIX,
                                 max_read, &vals, &more);
    if (r < 0)
      return r;

    for (map<string, bufferlist>::iterator it = vals.begin();
	 it != vals.end() && res.size() < max_return; ++it) {

      bufferlist::iterator iter = it->second.begin();
      cls::rbd::GroupImageLinkState state;
      try {
	::decode(state, iter);
      } catch (const buffer::error &err) {
	CLS_ERR("error decoding state for image: %s", it->first.c_str());
	return -EIO;
      }
      cls::rbd::GroupImageSpec spec;
      int r = cls::rbd::GroupImageSpec::from_key(it->first, &spec);
      if (r < 0)
	return r;

      CLS_LOG(20, "Discovered image %s %" PRId64 " %d", spec.image_id.c_str(),
	                                         spec.pool_id,
					         (int)state);
      res.push_back(cls::rbd::GroupImageStatus(spec, state));
    }
    if (res.size() > 0) {
      last_read = res.rbegin()->spec.image_key();
    }

  } while (more && (res.size() < max_return));
  ::encode(res, *out);

  return 0;
}

/**
 * Reference the consistency group this image belongs to.
 *
 * Input:
 * @param group_id (std::string)
 * @param pool_id (int64_t)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int image_add_group(cls_method_context_t hctx,
		    bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "image_add_group");
  cls::rbd::GroupSpec new_group;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(new_group, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  bufferlist existing_refbl;

  int r = cls_cxx_map_get_val(hctx, RBD_GROUP_REF, &existing_refbl);
  if (r == 0) {
    // If we are trying to link this image to the same group then return success.
    // If this image already belongs to another group then abort.
    cls::rbd::GroupSpec old_group;
    try {
      bufferlist::iterator iter = existing_refbl.begin();
      ::decode(old_group, iter);
    } catch (const buffer::error &err) {
      return -EINVAL;
    }

    if ((old_group.group_id != new_group.group_id)
	|| (old_group.pool_id != new_group.pool_id)) {
      return -EEXIST;
    } else {
      return 0; // In this case the values are already correct
    }
  } else if (r < 0 && r != -ENOENT) { // No entry means this image is not a member of any consistency group. So, we can use it.
    return r;
  }

  bufferlist refbl;
  ::encode(new_group, refbl);
  r = cls_cxx_map_set_val(hctx, RBD_GROUP_REF, &refbl);

  if (r < 0) {
    return r;
  }

  return 0;
}

/**
 * Remove image's pointer to the consistency group.
 *
 * Input:
 * @param cg_id (std::string)
 * @param pool_id (int64_t)
 *
 * Output:
 * @return 0 on success, negative error code on failure
 */
int image_remove_group(cls_method_context_t hctx,
		       bufferlist *in,
		       bufferlist *out)
{
  CLS_LOG(20, "image_remove_group");
  cls::rbd::GroupSpec spec;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(spec, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  bufferlist refbl;
  int r = cls_cxx_map_get_val(hctx, RBD_GROUP_REF, &refbl);
  if (r < 0) {
    return r;
  }

  cls::rbd::GroupSpec ref_spec;
  bufferlist::iterator iter = refbl.begin();
  try {
    ::decode(ref_spec, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (ref_spec.pool_id != spec.pool_id || ref_spec.group_id != spec.group_id) {
    return -EBADF;
  }

  r = cls_cxx_map_remove_key(hctx, RBD_GROUP_REF);
  if (r < 0) {
    return r;
  }

  return 0;
}

/**
 * Retrieve the id and pool of the consistency group this image belongs to.
 *
 * Input:
 * none
 *
 * Output:
 * @param GroupSpec
 * @return 0 on success, negative error code on failure
 */
int image_get_group(cls_method_context_t hctx,
		    bufferlist *in, bufferlist *out)
{
  CLS_LOG(20, "image_get_group");
  bufferlist refbl;
  int r = cls_cxx_map_get_val(hctx, RBD_GROUP_REF, &refbl);
  if (r < 0 && r != -ENOENT) {
    return r;
  }

  cls::rbd::GroupSpec spec;

  if (r != -ENOENT) {
    bufferlist::iterator iter = refbl.begin();
    try {
      ::decode(spec, iter);
    } catch (const buffer::error &err) {
      return -EINVAL;
    }
  }

  ::encode(spec, *out);
  return 0;
}

namespace trash {

static const std::string IMAGE_KEY_PREFIX("id_");

std::string image_key(const std::string &image_id) {
  return IMAGE_KEY_PREFIX + image_id;
}

std::string image_id_from_key(const std::string &key) {
  return key.substr(IMAGE_KEY_PREFIX.size());
}

} // namespace trash

/**
 * Add an image entry to the rbd trash. Creates the trash object if
 * needed, and stores the trash spec information of the deleted image.
 *
 * Input:
 * @param id the id of the image
 * @param trash_spec the spec info of the deleted image
 *
 * Output:
 * @returns -EEXIST if the image id is already in the trash
 * @returns 0 on success, negative error code on failure
 */
int trash_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  int r = cls_cxx_create(hctx, false);
  if (r < 0) {
    CLS_ERR("could not create trash: %s", cpp_strerror(r).c_str());
    return r;
  }

  string id;
  cls::rbd::TrashImageSpec trash_spec;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
    ::decode(trash_spec, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  if (!is_valid_id(id)) {
    CLS_ERR("trash_add: invalid id '%s'", id.c_str());
    return -EINVAL;
  }

  CLS_LOG(20, "trash_add id=%s", id.c_str());

  string key = trash::image_key(id);
  cls::rbd::TrashImageSpec tmp;
  r = read_key(hctx, key, &tmp);
  if (r < 0 && r != -ENOENT) {
    CLS_ERR("could not read key %s entry from trash: %s", key.c_str(),
            cpp_strerror(r).c_str());
    return r;
  } else if (r == 0) {
    CLS_LOG(10, "id already exists");
    return -EEXIST;
  }

  map<string, bufferlist> omap_vals;
  ::encode(trash_spec, omap_vals[key]);
  return cls_cxx_map_set_vals(hctx, &omap_vals);
}

/**
 * Removes an image entry from the rbd trash object.
 * image.
 *
 * Input:
 * @param id the id of the image
 *
 * Output:
 * @returns -ENOENT if the image id does not exist in the trash
 * @returns 0 on success, negative error code on failure
 */
int trash_remove(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "trash_remove id=%s", id.c_str());

  string key = trash::image_key(id);
  bufferlist tmp;
  int r = cls_cxx_map_get_val(hctx, key, &tmp);
  if (r < 0) {
    if (r != -ENOENT) {
      CLS_ERR("error reading entry key %s: %s", key.c_str(), cpp_strerror(r).c_str());
    }
    return r;
  }

  r = cls_cxx_map_remove_key(hctx, key);
  if (r < 0) {
    CLS_ERR("error removing entry: %s", cpp_strerror(r).c_str());
    return r;
  }

  return 0;
}

/**
 * Returns the list of trash spec entries registered in the rbd_trash
 * object.
 *
 * Input:
 * @param start_after which name to begin listing after
 *        (use the empty string to start at the beginning)
 * @param max_return the maximum number of names to list
 *
 * Output:
 * @param data the map between image id and trash spec info
 *
 * @returns 0 on success, negative error code on failure
 */
int trash_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string start_after;
  uint64_t max_return;

  try {
    bufferlist::iterator iter = in->begin();
    ::decode(start_after, iter);
    ::decode(max_return, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  map<string, cls::rbd::TrashImageSpec> data;
  string last_read = trash::image_key(start_after);
  bool more = true;

  CLS_LOG(20, "trash_get_images");
  while (data.size() < max_return) {
    map<string, bufferlist> raw_data;
    int max_read = std::min<int32_t>(RBD_MAX_KEYS_READ,
                                     max_return - data.size());
    int r = cls_cxx_map_get_vals(hctx, last_read, trash::IMAGE_KEY_PREFIX,
                                 max_read, &raw_data, &more);
    if (r < 0) {
      CLS_ERR("failed to read the vals off of disk: %s",
              cpp_strerror(r).c_str());
      return r;
    }
    if (raw_data.empty()) {
      break;
    }

    map<string, bufferlist>::iterator it = raw_data.begin();
    for (; it != raw_data.end(); ++it) {
      ::decode(data[trash::image_id_from_key(it->first)], it->second);
    }

    if (!more) {
      break;
    }

    last_read = raw_data.rbegin()->first;
  }

  ::encode(data, *out);
  return 0;
}

/**
 * Returns the trash spec entry of an image registered in the rbd_trash
 * object.
 *
 * Input:
 * @param id the id of the image
 *
 * Output:
 * @param out the trash spec entry
 *
 * @returns 0 on success, negative error code on failure
 */
int trash_get(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  string id;
  try {
    bufferlist::iterator iter = in->begin();
    ::decode(id, iter);
  } catch (const buffer::error &err) {
    return -EINVAL;
  }

  CLS_LOG(20, "trash_get_image id=%s", id.c_str());


  string key = trash::image_key(id);
  bufferlist bl;
  int r = cls_cxx_map_get_val(hctx, key, out);
  if (r != -ENOENT) {
    CLS_ERR("error reading image from trash '%s': '%s'", id.c_str(),
            cpp_strerror(r).c_str());
  }
  return r;
}

CLS_INIT(rbd)
{
  CLS_LOG(20, "Loaded rbd class!");

  cls_handle_t h_class;
  cls_method_handle_t h_create;
  cls_method_handle_t h_get_features;
  cls_method_handle_t h_set_features;
  cls_method_handle_t h_get_size;
  cls_method_handle_t h_set_size;
  cls_method_handle_t h_get_parent;
  cls_method_handle_t h_set_parent;
  cls_method_handle_t h_get_protection_status;
  cls_method_handle_t h_set_protection_status;
  cls_method_handle_t h_get_stripe_unit_count;
  cls_method_handle_t h_set_stripe_unit_count;
  cls_method_handle_t h_get_create_timestamp;
  cls_method_handle_t h_get_flags;
  cls_method_handle_t h_set_flags;
  cls_method_handle_t h_remove_parent;
  cls_method_handle_t h_add_child;
  cls_method_handle_t h_remove_child;
  cls_method_handle_t h_get_children;
  cls_method_handle_t h_get_snapcontext;
  cls_method_handle_t h_get_object_prefix;
  cls_method_handle_t h_get_data_pool;
  cls_method_handle_t h_get_snapshot_name;
  cls_method_handle_t h_get_snapshot_namespace;
  cls_method_handle_t h_get_snapshot_timestamp;
  cls_method_handle_t h_snapshot_add;
  cls_method_handle_t h_snapshot_remove;
  cls_method_handle_t h_snapshot_rename;
  cls_method_handle_t h_get_all_features;
  cls_method_handle_t h_copyup;
  cls_method_handle_t h_get_id;
  cls_method_handle_t h_set_id;
  cls_method_handle_t h_dir_get_id;
  cls_method_handle_t h_dir_get_name;
  cls_method_handle_t h_dir_list;
  cls_method_handle_t h_dir_add_image;
  cls_method_handle_t h_dir_remove_image;
  cls_method_handle_t h_dir_rename_image;
  cls_method_handle_t h_object_map_load;
  cls_method_handle_t h_object_map_save;
  cls_method_handle_t h_object_map_resize;
  cls_method_handle_t h_object_map_update;
  cls_method_handle_t h_object_map_snap_add;
  cls_method_handle_t h_object_map_snap_remove;
  cls_method_handle_t h_metadata_set;
  cls_method_handle_t h_metadata_remove;
  cls_method_handle_t h_metadata_list;
  cls_method_handle_t h_metadata_get;
  cls_method_handle_t h_snapshot_get_limit;
  cls_method_handle_t h_snapshot_set_limit;
  cls_method_handle_t h_old_snapshots_list;
  cls_method_handle_t h_old_snapshot_add;
  cls_method_handle_t h_old_snapshot_remove;
  cls_method_handle_t h_old_snapshot_rename;
  cls_method_handle_t h_mirror_uuid_get;
  cls_method_handle_t h_mirror_uuid_set;
  cls_method_handle_t h_mirror_mode_get;
  cls_method_handle_t h_mirror_mode_set;
  cls_method_handle_t h_mirror_peer_list;
  cls_method_handle_t h_mirror_peer_add;
  cls_method_handle_t h_mirror_peer_remove;
  cls_method_handle_t h_mirror_peer_set_client;
  cls_method_handle_t h_mirror_peer_set_cluster;
  cls_method_handle_t h_mirror_image_list;
  cls_method_handle_t h_mirror_image_get_image_id;
  cls_method_handle_t h_mirror_image_get;
  cls_method_handle_t h_mirror_image_set;
  cls_method_handle_t h_mirror_image_remove;
  cls_method_handle_t h_mirror_image_status_set;
  cls_method_handle_t h_mirror_image_status_remove;
  cls_method_handle_t h_mirror_image_status_get;
  cls_method_handle_t h_mirror_image_status_list;
  cls_method_handle_t h_mirror_image_status_get_summary;
  cls_method_handle_t h_mirror_image_status_remove_down;
  cls_method_handle_t h_mirror_instances_list;
  cls_method_handle_t h_mirror_instances_add;
  cls_method_handle_t h_mirror_instances_remove;
  cls_method_handle_t h_group_create;
  cls_method_handle_t h_group_dir_list;
  cls_method_handle_t h_group_dir_add;
  cls_method_handle_t h_group_dir_remove;
  cls_method_handle_t h_group_image_remove;
  cls_method_handle_t h_group_image_list;
  cls_method_handle_t h_group_image_set;
  cls_method_handle_t h_image_add_group;
  cls_method_handle_t h_image_remove_group;
  cls_method_handle_t h_image_get_group;
  cls_method_handle_t h_trash_add;
  cls_method_handle_t h_trash_remove;
  cls_method_handle_t h_trash_list;
  cls_method_handle_t h_trash_get;

  cls_register("rbd", &h_class);
  cls_register_cxx_method(h_class, "create",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  create, &h_create);
  cls_register_cxx_method(h_class, "get_features",
			  CLS_METHOD_RD,
			  get_features, &h_get_features);
  cls_register_cxx_method(h_class, "set_features",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_features, &h_set_features);
  cls_register_cxx_method(h_class, "get_size",
			  CLS_METHOD_RD,
			  get_size, &h_get_size);
  cls_register_cxx_method(h_class, "set_size",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_size, &h_set_size);
  cls_register_cxx_method(h_class, "get_snapcontext",
			  CLS_METHOD_RD,
			  get_snapcontext, &h_get_snapcontext);
  cls_register_cxx_method(h_class, "get_object_prefix",
			  CLS_METHOD_RD,
			  get_object_prefix, &h_get_object_prefix);
  cls_register_cxx_method(h_class, "get_data_pool", CLS_METHOD_RD,
                          get_data_pool, &h_get_data_pool);
  cls_register_cxx_method(h_class, "get_snapshot_name",
			  CLS_METHOD_RD,
			  get_snapshot_name, &h_get_snapshot_name);
  cls_register_cxx_method(h_class, "get_snapshot_namespace",
			  CLS_METHOD_RD,
			  get_snapshot_namespace, &h_get_snapshot_namespace);
  cls_register_cxx_method(h_class, "get_snapshot_timestamp",
			  CLS_METHOD_RD,
			  get_snapshot_timestamp, &h_get_snapshot_timestamp);
  cls_register_cxx_method(h_class, "snapshot_add",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  snapshot_add, &h_snapshot_add);
  cls_register_cxx_method(h_class, "snapshot_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  snapshot_remove, &h_snapshot_remove);
  cls_register_cxx_method(h_class, "snapshot_rename",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  snapshot_rename, &h_snapshot_rename);
  cls_register_cxx_method(h_class, "get_all_features",
			  CLS_METHOD_RD,
			  get_all_features, &h_get_all_features);
  cls_register_cxx_method(h_class, "copyup",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  copyup, &h_copyup);
  cls_register_cxx_method(h_class, "get_parent",
			  CLS_METHOD_RD,
			  get_parent, &h_get_parent);
  cls_register_cxx_method(h_class, "set_parent",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_parent, &h_set_parent);
  cls_register_cxx_method(h_class, "remove_parent",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  remove_parent, &h_remove_parent);
  cls_register_cxx_method(h_class, "set_protection_status",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_protection_status, &h_set_protection_status);
  cls_register_cxx_method(h_class, "get_protection_status",
			  CLS_METHOD_RD,
			  get_protection_status, &h_get_protection_status);
  cls_register_cxx_method(h_class, "get_stripe_unit_count",
			  CLS_METHOD_RD,
			  get_stripe_unit_count, &h_get_stripe_unit_count);
  cls_register_cxx_method(h_class, "set_stripe_unit_count",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_stripe_unit_count, &h_set_stripe_unit_count);
  cls_register_cxx_method(h_class, "get_create_timestamp",
                          CLS_METHOD_RD,
                          get_create_timestamp, &h_get_create_timestamp);
  cls_register_cxx_method(h_class, "get_flags",
                          CLS_METHOD_RD,
                          get_flags, &h_get_flags);
  cls_register_cxx_method(h_class, "set_flags",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          set_flags, &h_set_flags);
  cls_register_cxx_method(h_class, "metadata_list",
                          CLS_METHOD_RD,
			  metadata_list, &h_metadata_list);
  cls_register_cxx_method(h_class, "metadata_set",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  metadata_set, &h_metadata_set);
  cls_register_cxx_method(h_class, "metadata_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  metadata_remove, &h_metadata_remove);
  cls_register_cxx_method(h_class, "metadata_get",
                          CLS_METHOD_RD,
			  metadata_get, &h_metadata_get);
  cls_register_cxx_method(h_class, "snapshot_get_limit",
			  CLS_METHOD_RD,
			  snapshot_get_limit, &h_snapshot_get_limit);
  cls_register_cxx_method(h_class, "snapshot_set_limit",
			  CLS_METHOD_WR,
			  snapshot_set_limit, &h_snapshot_set_limit);

  /* methods for the rbd_children object */
  cls_register_cxx_method(h_class, "add_child",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  add_child, &h_add_child);
  cls_register_cxx_method(h_class, "remove_child",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  remove_child, &h_remove_child);
  cls_register_cxx_method(h_class, "get_children",
			  CLS_METHOD_RD,
			  get_children, &h_get_children);

  /* methods for the rbd_id.$image_name objects */
  cls_register_cxx_method(h_class, "get_id",
			  CLS_METHOD_RD,
			  get_id, &h_get_id);
  cls_register_cxx_method(h_class, "set_id",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  set_id, &h_set_id);

  /* methods for the rbd_directory object */
  cls_register_cxx_method(h_class, "dir_get_id",
			  CLS_METHOD_RD,
			  dir_get_id, &h_dir_get_id);
  cls_register_cxx_method(h_class, "dir_get_name",
			  CLS_METHOD_RD,
			  dir_get_name, &h_dir_get_name);
  cls_register_cxx_method(h_class, "dir_list",
			  CLS_METHOD_RD,
			  dir_list, &h_dir_list);
  cls_register_cxx_method(h_class, "dir_add_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_add_image, &h_dir_add_image);
  cls_register_cxx_method(h_class, "dir_remove_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_remove_image, &h_dir_remove_image);
  cls_register_cxx_method(h_class, "dir_rename_image",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  dir_rename_image, &h_dir_rename_image);

  /* methods for the rbd_object_map.$image_id object */
  cls_register_cxx_method(h_class, "object_map_load",
                          CLS_METHOD_RD,
			  object_map_load, &h_object_map_load);
  cls_register_cxx_method(h_class, "object_map_save",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_save, &h_object_map_save);
  cls_register_cxx_method(h_class, "object_map_resize",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_resize, &h_object_map_resize);
  cls_register_cxx_method(h_class, "object_map_update",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_update, &h_object_map_update);
  cls_register_cxx_method(h_class, "object_map_snap_add",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_snap_add, &h_object_map_snap_add);
  cls_register_cxx_method(h_class, "object_map_snap_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
			  object_map_snap_remove, &h_object_map_snap_remove);

 /* methods for the old format */
  cls_register_cxx_method(h_class, "snap_list",
			  CLS_METHOD_RD,
			  old_snapshots_list, &h_old_snapshots_list);
  cls_register_cxx_method(h_class, "snap_add",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  old_snapshot_add, &h_old_snapshot_add);
  cls_register_cxx_method(h_class, "snap_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  old_snapshot_remove, &h_old_snapshot_remove);
  cls_register_cxx_method(h_class, "snap_rename",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  old_snapshot_rename, &h_old_snapshot_rename);

  /* methods for the rbd_mirroring object */
  cls_register_cxx_method(h_class, "mirror_uuid_get", CLS_METHOD_RD,
                          mirror_uuid_get, &h_mirror_uuid_get);
  cls_register_cxx_method(h_class, "mirror_uuid_set",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_uuid_set, &h_mirror_uuid_set);
  cls_register_cxx_method(h_class, "mirror_mode_get", CLS_METHOD_RD,
                          mirror_mode_get, &h_mirror_mode_get);
  cls_register_cxx_method(h_class, "mirror_mode_set",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_mode_set, &h_mirror_mode_set);
  cls_register_cxx_method(h_class, "mirror_peer_list", CLS_METHOD_RD,
                          mirror_peer_list, &h_mirror_peer_list);
  cls_register_cxx_method(h_class, "mirror_peer_add",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_peer_add, &h_mirror_peer_add);
  cls_register_cxx_method(h_class, "mirror_peer_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_peer_remove, &h_mirror_peer_remove);
  cls_register_cxx_method(h_class, "mirror_peer_set_client",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_peer_set_client, &h_mirror_peer_set_client);
  cls_register_cxx_method(h_class, "mirror_peer_set_cluster",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_peer_set_cluster, &h_mirror_peer_set_cluster);
  cls_register_cxx_method(h_class, "mirror_image_list", CLS_METHOD_RD,
                          mirror_image_list, &h_mirror_image_list);
  cls_register_cxx_method(h_class, "mirror_image_get_image_id", CLS_METHOD_RD,
                          mirror_image_get_image_id,
                          &h_mirror_image_get_image_id);
  cls_register_cxx_method(h_class, "mirror_image_get", CLS_METHOD_RD,
                          mirror_image_get, &h_mirror_image_get);
  cls_register_cxx_method(h_class, "mirror_image_set",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_image_set, &h_mirror_image_set);
  cls_register_cxx_method(h_class, "mirror_image_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_image_remove, &h_mirror_image_remove);
  cls_register_cxx_method(h_class, "mirror_image_status_set",
                          CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PROMOTE,
                          mirror_image_status_set, &h_mirror_image_status_set);
  cls_register_cxx_method(h_class, "mirror_image_status_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_image_status_remove,
			  &h_mirror_image_status_remove);
  cls_register_cxx_method(h_class, "mirror_image_status_get", CLS_METHOD_RD,
                          mirror_image_status_get, &h_mirror_image_status_get);
  cls_register_cxx_method(h_class, "mirror_image_status_list", CLS_METHOD_RD,
                          mirror_image_status_list,
			  &h_mirror_image_status_list);
  cls_register_cxx_method(h_class, "mirror_image_status_get_summary",
			  CLS_METHOD_RD, mirror_image_status_get_summary,
			  &h_mirror_image_status_get_summary);
  cls_register_cxx_method(h_class, "mirror_image_status_remove_down",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_image_status_remove_down,
			  &h_mirror_image_status_remove_down);
  cls_register_cxx_method(h_class, "mirror_instances_list", CLS_METHOD_RD,
                          mirror_instances_list, &h_mirror_instances_list);
  cls_register_cxx_method(h_class, "mirror_instances_add",
                          CLS_METHOD_RD | CLS_METHOD_WR | CLS_METHOD_PROMOTE,
                          mirror_instances_add, &h_mirror_instances_add);
  cls_register_cxx_method(h_class, "mirror_instances_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          mirror_instances_remove,
                          &h_mirror_instances_remove);
  /* methods for the consistency groups feature */
  cls_register_cxx_method(h_class, "group_create",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  group_create, &h_group_create);
  cls_register_cxx_method(h_class, "group_dir_list",
			  CLS_METHOD_RD,
			  group_dir_list, &h_group_dir_list);
  cls_register_cxx_method(h_class, "group_dir_add",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  group_dir_add, &h_group_dir_add);
  cls_register_cxx_method(h_class, "group_dir_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  group_dir_remove, &h_group_dir_remove);
  cls_register_cxx_method(h_class, "group_image_remove",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  group_image_remove, &h_group_image_remove);
  cls_register_cxx_method(h_class, "group_image_list",
			  CLS_METHOD_RD,
			  group_image_list, &h_group_image_list);
  cls_register_cxx_method(h_class, "group_image_set",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  group_image_set, &h_group_image_set);
  cls_register_cxx_method(h_class, "image_add_group",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  image_add_group, &h_image_add_group);
  cls_register_cxx_method(h_class, "image_remove_group",
			  CLS_METHOD_RD | CLS_METHOD_WR,
			  image_remove_group, &h_image_remove_group);
  cls_register_cxx_method(h_class, "image_get_group",
			  CLS_METHOD_RD,
			  image_get_group, &h_image_get_group);

  /* rbd_trash object methods */
  cls_register_cxx_method(h_class, "trash_add",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          trash_add, &h_trash_add);
  cls_register_cxx_method(h_class, "trash_remove",
                          CLS_METHOD_RD | CLS_METHOD_WR,
                          trash_remove, &h_trash_remove);
  cls_register_cxx_method(h_class, "trash_list",
                          CLS_METHOD_RD,
                          trash_list, &h_trash_list);
  cls_register_cxx_method(h_class, "trash_get",
                          CLS_METHOD_RD,
                          trash_get, &h_trash_get);

  return;
}
