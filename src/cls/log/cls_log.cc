// -*- mode:C; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/types.h"
#include "include/utime.h"
#include "objclass/objclass.h"

#include "cls_log_types.h"
#include "cls_log_ops.h"

#include "global/global_context.h"
#include "include/compat.h"

CLS_VER(1,0)
CLS_NAME(log)

static string log_index_prefix = "1_";


static int write_log_entry(cls_method_context_t hctx, string& index, cls_log_entry& entry)
{
  bufferlist bl;
  ::encode(entry, bl);

  int ret = cls_cxx_map_set_val(hctx, index, &bl);
  if (ret < 0)
    return ret;

  return 0;
}

static void get_index_time_prefix(utime_t& ts, string& index)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%010ld.%06ld_", (long)ts.sec(), (long)ts.usec());

  index = log_index_prefix + buf;
}

static int read_header(cls_method_context_t hctx, cls_log_header& header)
{
  bufferlist header_bl;

  int ret = cls_cxx_map_read_header(hctx, &header_bl);
  if (ret < 0)
    return ret;

  if (header_bl.length() == 0) {
    header = cls_log_header();
    return 0;
  }

  bufferlist::iterator iter = header_bl.begin();
  try {
    ::decode(header, iter);
  } catch (buffer::error& err) {
    CLS_LOG(0, "ERROR: read_header(): failed to decode header");
  }

  return 0;
}

static int write_header(cls_method_context_t hctx, cls_log_header& header)
{
  bufferlist header_bl;
  ::encode(header, header_bl);

  int ret = cls_cxx_map_write_header(hctx, &header_bl);
  if (ret < 0)
    return ret;

  return 0;
}

static void get_index(cls_method_context_t hctx, utime_t& ts, string& index)
{
  get_index_time_prefix(ts, index);

  string unique_id;

  cls_cxx_subop_version(hctx, &unique_id);

  index.append(unique_id);
}

static int cls_log_add(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_add_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(1, "ERROR: cls_log_add_op(): failed to decode op");
    return -EINVAL;
  }

  cls_log_header header;

  int ret = read_header(hctx, header);
  if (ret < 0)
    return ret;

  for (list<cls_log_entry>::iterator iter = op.entries.begin();
       iter != op.entries.end(); ++iter) {
    cls_log_entry& entry = *iter;

    string index;

    utime_t timestamp = entry.timestamp;
    if (op.monotonic_inc && timestamp < header.max_time)
      timestamp = header.max_time;
    else if (timestamp > header.max_time)
      header.max_time = timestamp;

    if (entry.id.empty()) {
      get_index(hctx, timestamp, index);
      entry.id = index;
    } else {
      index = entry.id;
    }

    CLS_LOG(20, "storing entry at %s", index.c_str());


    if (index > header.max_marker)
      header.max_marker = index;

    ret = write_log_entry(hctx, index, entry);
    if (ret < 0)
      return ret;
  }

  ret = write_header(hctx, header);
  if (ret < 0)
    return ret;

  return 0;
}

static int cls_log_list(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_list_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(1, "ERROR: cls_log_list_op(): failed to decode op");
    return -EINVAL;
  }

  map<string, bufferlist> keys;

  string from_index;
  string to_index;

  if (op.marker.empty()) {
    get_index_time_prefix(op.from_time, from_index);
  } else {
    from_index = op.marker;
  }
  bool use_time_boundary = (!op.from_time.is_zero() && (op.to_time >= op.from_time));

  if (use_time_boundary)
    get_index_time_prefix(op.to_time, to_index);

#define MAX_ENTRIES 1000
  size_t max_entries = op.max_entries;
  if (!max_entries || max_entries > MAX_ENTRIES)
    max_entries = MAX_ENTRIES;

  cls_log_list_ret ret;

  int rc = cls_cxx_map_get_vals(hctx, from_index, log_index_prefix, max_entries, &keys, &ret.truncated);
  if (rc < 0)
    return rc;

  list<cls_log_entry>& entries = ret.entries;
  map<string, bufferlist>::iterator iter = keys.begin();

  string marker;

  for (; iter != keys.end(); ++iter) {
    const string& index = iter->first;
    marker = index;
    if (use_time_boundary && index.compare(0, to_index.size(), to_index) >= 0) {
      ret.truncated = false;
      break;
    }

    bufferlist& bl = iter->second;
    bufferlist::iterator biter = bl.begin();
    try {
      cls_log_entry e;
      ::decode(e, biter);
      entries.push_back(e);
    } catch (buffer::error& err) {
      CLS_LOG(0, "ERROR: cls_log_list: could not decode entry, index=%s", index.c_str());
    }
  }

  ret.marker = marker;

  ::encode(ret, *out);

  return 0;
}


static int cls_log_trim(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_trim_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(0, "ERROR: cls_log_list_op(): failed to decode entry");
    return -EINVAL;
  }

  map<string, bufferlist> keys;

  string from_index;
  string to_index;

  if (op.from_marker.empty()) {
    get_index_time_prefix(op.from_time, from_index);
  } else {
    from_index = op.from_marker;
  }
  if (op.to_marker.empty()) {
    get_index_time_prefix(op.to_time, to_index);
  } else {
    to_index = op.to_marker;
  }

#define MAX_TRIM_ENTRIES 1000
  size_t max_entries = MAX_TRIM_ENTRIES;
  bool more;

  int rc = cls_cxx_map_get_vals(hctx, from_index, log_index_prefix, max_entries, &keys, &more);
  if (rc < 0)
    return rc;

  map<string, bufferlist>::iterator iter = keys.begin();

  bool removed = false;
  for (; iter != keys.end(); ++iter) {
    const string& index = iter->first;

    CLS_LOG(20, "index=%s to_index=%s", index.c_str(), to_index.c_str());

    if (index.compare(0, to_index.size(), to_index) > 0)
      break;

    CLS_LOG(20, "removing key: index=%s", index.c_str());

    int rc = cls_cxx_map_remove_key(hctx, index);
    if (rc < 0) {
      CLS_LOG(1, "ERROR: cls_cxx_map_remove_key failed rc=%d", rc);
      return -EINVAL;
    }
    removed = true;
  }

  if (!removed)
    return -ENODATA;

  return 0;
}

static int cls_log_info(cls_method_context_t hctx, bufferlist *in, bufferlist *out)
{
  bufferlist::iterator in_iter = in->begin();

  cls_log_info_op op;
  try {
    ::decode(op, in_iter);
  } catch (buffer::error& err) {
    CLS_LOG(1, "ERROR: cls_log_add_op(): failed to decode op");
    return -EINVAL;
  }

  cls_log_info_ret ret;

  int rc = read_header(hctx, ret.header);
  if (rc < 0)
    return rc;

  ::encode(ret, *out);

  return 0;
}

CLS_INIT(log)
{
  CLS_LOG(1, "Loaded log class!");

  cls_handle_t h_class;
  cls_method_handle_t h_log_add;
  cls_method_handle_t h_log_list;
  cls_method_handle_t h_log_trim;
  cls_method_handle_t h_log_info;

  cls_register("log", &h_class);

  /* log */
  cls_register_cxx_method(h_class, "add", CLS_METHOD_RD | CLS_METHOD_WR, cls_log_add, &h_log_add);
  cls_register_cxx_method(h_class, "list", CLS_METHOD_RD, cls_log_list, &h_log_list);
  cls_register_cxx_method(h_class, "trim", CLS_METHOD_RD | CLS_METHOD_WR, cls_log_trim, &h_log_trim);
  cls_register_cxx_method(h_class, "info", CLS_METHOD_RD, cls_log_info, &h_log_info);

  return;
}

