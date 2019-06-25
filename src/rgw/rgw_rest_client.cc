// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_common.h"
#include "rgw_rest_client.h"
#include "rgw_auth_s3.h"
#include "rgw_http_errors.h"
#include "rgw_rados.h"

#include "common/ceph_crypto_cms.h"
#include "common/armor.h"
#include "common/strtol.h"
#include "include/str_list.h"
#include "rgw_crypt_sanitize.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

int RGWRESTSimpleRequest::get_status()
{
  int retcode = get_req_retcode();
  if (retcode < 0) {
    return retcode;
  }
  return status;
}

int RGWRESTSimpleRequest::handle_header(const string& name, const string& val) 
{
  if (name == "CONTENT_LENGTH") {
    string err;
    long len = strict_strtol(val.c_str(), 10, &err);
    if (!err.empty()) {
      ldout(cct, 0) << "ERROR: failed converting content length (" << val << ") to int " << dendl;
      return -EINVAL;
    }

    max_response = len;
  }

  return 0;
}

int RGWRESTSimpleRequest::receive_header(void *ptr, size_t len)
{
  char line[len + 1];

  char *s = (char *)ptr, *end = (char *)ptr + len;
  char *p = line;
  ldout(cct, 10) << "receive_http_header" << dendl;

  while (s != end) {
    if (*s == '\r') {
      s++;
      continue;
    }
    if (*s == '\n') {
      *p = '\0';
      ldout(cct, 10) << "received header:" << line << dendl;
      // TODO: fill whatever data required here
      char *l = line;
      char *tok = strsep(&l, " \t:");
      if (tok && l) {
        while (*l == ' ')
          l++;
 
        if (strcmp(tok, "HTTP") == 0 || strncmp(tok, "HTTP/", 5) == 0) {
          http_status = atoi(l);
          if (http_status == 100) /* 100-continue response */
            continue;
          status = rgw_http_error_to_errno(http_status);
        } else {
          /* convert header field name to upper case  */
          char *src = tok;
          char buf[len + 1];
          size_t i;
          for (i = 0; i < len && *src; ++i, ++src) {
            switch (*src) {
              case '-':
                buf[i] = '_';
                break;
              default:
                buf[i] = toupper(*src);
            }
          }
          buf[i] = '\0';
          out_headers[buf] = l;
          int r = handle_header(buf, l);
          if (r < 0)
            return r;
        }
      }
    }
    if (s != end)
      *p++ = *s++;
  }
  return 0;
}

static void get_new_date_str(string& date_str)
{
  date_str = rgw_to_asctime(ceph_clock_now());
}

int RGWRESTSimpleRequest::execute(RGWAccessKey& key, const char *method, const char *resource)
{
  string new_url = url;
  string new_resource = resource;

  if (new_url[new_url.size() - 1] == '/' && resource[0] == '/') {
    new_url = new_url.substr(0, new_url.size() - 1);
  } else if (resource[0] != '/') {
    new_resource = "/";
    new_resource.append(resource);
  }
  new_url.append(new_resource);

  string date_str;
  get_new_date_str(date_str);
  headers.push_back(pair<string, string>("HTTP_DATE", date_str));

  string canonical_header;
  map<string, string> meta_map;
  map<string, string> sub_resources;
  rgw_create_s3_canonical_header(method, NULL, NULL, date_str.c_str(),
                            meta_map, new_url.c_str(), sub_resources,
                            canonical_header);

  string digest;
  try {
    digest = rgw::auth::s3::get_v2_signature(cct, key.key, canonical_header);
  } catch (int ret) {
    return ret;
  }

  string auth_hdr = "AWS " + key.id + ":" + digest;

  ldout(cct, 15) << "generated auth header: " << auth_hdr << dendl;

  headers.push_back(pair<string, string>("AUTHORIZATION", auth_hdr));
  int r = process(method, new_url.c_str());
  if (r < 0)
    return r;

  return status;
}

int RGWRESTSimpleRequest::send_data(void *ptr, size_t len)
{
  if (!send_iter)
    return 0;

  if (len > send_iter->get_remaining())
    len = send_iter->get_remaining();

  send_iter->copy(len, (char *)ptr);

  return len;
}

int RGWRESTSimpleRequest::receive_data(void *ptr, size_t len)
{
  size_t cp_len, left_len;

  left_len = max_response > response.length() ? (max_response - response.length()) : 0;
  if (left_len == 0)
    return 0; /* don't read extra data */

  cp_len = (len > left_len) ? left_len : len;
  bufferptr p((char *)ptr, cp_len);

  response.append(p);

  return 0;

}

void RGWRESTSimpleRequest::append_param(string& dest, const string& name, const string& val)
{
  if (dest.empty()) {
    dest.append("?");
  } else {
    dest.append("&");
  }
  string url_name;
  url_encode(name, url_name);
  dest.append(url_name);

  if (!val.empty()) {
    string url_val;
    url_encode(val, url_val);
    dest.append("=");
    dest.append(url_val);
  }
}

void RGWRESTSimpleRequest::get_params_str(map<string, string>& extra_args, string& dest)
{
  map<string, string>::iterator miter;
  for (miter = extra_args.begin(); miter != extra_args.end(); ++miter) {
    append_param(dest, miter->first, miter->second);
  }
  param_vec_t::iterator iter;
  for (iter = params.begin(); iter != params.end(); ++iter) {
    append_param(dest, iter->first, iter->second);
  }
}

int RGWRESTSimpleRequest::sign_request(RGWAccessKey& key, RGWEnv& env, req_info& info)
{
  /* don't sign if no key is provided */
  if (key.key.empty()) {
    return 0;
  }

  if (cct->_conf->subsys.should_gather(ceph_subsys_rgw, 20)) {
    for (const auto& i: env.get_map()) {
      ldout(cct, 20) << "> " << i.first << " -> " << rgw::crypt_sanitize::x_meta_map{i.first, i.second} << dendl;
    }
  }

  string canonical_header;
  if (!rgw_create_s3_canonical_header(info, NULL, canonical_header, false)) {
    ldout(cct, 0) << "failed to create canonical s3 header" << dendl;
    return -EINVAL;
  }

  ldout(cct, 10) << "generated canonical header: " << canonical_header << dendl;

  string digest;
  try {
    digest = rgw::auth::s3::get_v2_signature(cct, key.key, canonical_header);
  } catch (int ret) {
    return ret;
  }

  string auth_hdr = "AWS " + key.id + ":" + digest;
  ldout(cct, 15) << "generated auth header: " << auth_hdr << dendl;
  
  env.set("AUTHORIZATION", auth_hdr);

  return 0;
}

int RGWRESTSimpleRequest::forward_request(RGWAccessKey& key, req_info& info, size_t max_response, bufferlist *inbl, bufferlist *outbl)
{

  string date_str;
  get_new_date_str(date_str);

  RGWEnv new_env;
  req_info new_info(cct, &new_env);
  new_info.rebuild_from(info);

  new_env.set("HTTP_DATE", date_str.c_str());

  int ret = sign_request(key, new_env, new_info);
  if (ret < 0) {
    ldout(cct, 0) << "ERROR: failed to sign request" << dendl;
    return ret;
  }

  for (const auto& kv: new_env.get_map()) {
    headers.emplace_back(kv);
  }

  map<string, string>& meta_map = new_info.x_meta_map;
  for (const auto& kv: meta_map) {
    headers.emplace_back(kv);
  }

  string params_str;
  get_params_str(info.args.get_params(), params_str);

  string new_url = url;
  string& resource = new_info.request_uri;
  string new_resource = resource;
  if (new_url[new_url.size() - 1] == '/' && resource[0] == '/') {
    new_url = new_url.substr(0, new_url.size() - 1);
  } else if (resource[0] != '/') {
    new_resource = "/";
    new_resource.append(resource);
  }
  new_url.append(new_resource + params_str);

  bufferlist::iterator bliter;

  if (inbl) {
    bliter = inbl->begin();
    send_iter = &bliter;

    set_send_length(inbl->length());
  }

  int r = process(new_info.method, new_url.c_str());
  if (r < 0){
    if (r == -EINVAL){
      // curl_easy has errored, generally means the service is not available
      r = -ERR_SERVICE_UNAVAILABLE;
    }
    return r;
  }

  response.append((char)0); /* NULL terminate response */

  if (outbl) {
    outbl->claim(response);
  }

  return status;
}

class RGWRESTStreamOutCB : public RGWGetDataCB {
  RGWRESTStreamWriteRequest *req;
public:
  explicit RGWRESTStreamOutCB(RGWRESTStreamWriteRequest *_req) : req(_req) {}
  int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) override; /* callback for object iteration when sending data */
};

int RGWRESTStreamOutCB::handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len)
{
  dout(20) << "RGWRESTStreamOutCB::handle_data bl.length()=" << bl.length() << " bl_ofs=" << bl_ofs << " bl_len=" << bl_len << dendl;
  if (!bl_ofs && bl_len == bl.length()) {
    return req->add_output_data(bl);
  }

  bufferptr bp(bl.c_str() + bl_ofs, bl_len);
  bufferlist new_bl;
  new_bl.push_back(bp);

  return req->add_output_data(new_bl);
}

RGWRESTStreamWriteRequest::~RGWRESTStreamWriteRequest()
{
  delete cb;
}

int RGWRESTStreamWriteRequest::add_output_data(bufferlist& bl)
{
  lock.Lock();
  if (status < 0) {
    int ret = status;
    lock.Unlock();
    return ret;
  }
  pending_send.push_back(bl);
  lock.Unlock();

  bool done;
  return http_manager.process_requests(false, &done);
}

static void grants_by_type_add_one_grant(map<int, string>& grants_by_type, int perm, ACLGrant& grant)
{
  string& s = grants_by_type[perm];

  if (!s.empty())
    s.append(", ");

  string id_type_str;
  ACLGranteeType& type = grant.get_type();
  switch (type.get_type()) {
    case ACL_TYPE_GROUP:
      id_type_str = "uri";
      break;
    case ACL_TYPE_EMAIL_USER:
      id_type_str = "emailAddress";
      break;
    default:
      id_type_str = "id";
  }
  rgw_user id;
  grant.get_id(id);
  s.append(id_type_str + "=\"" + id.to_str() + "\"");
}

struct grant_type_to_header {
  int type;
  const char *header;
};

struct grant_type_to_header grants_headers_def[] = {
  { RGW_PERM_FULL_CONTROL, "x-amz-grant-full-control"},
  { RGW_PERM_READ,         "x-amz-grant-read"},
  { RGW_PERM_WRITE,        "x-amz-grant-write"},
  { RGW_PERM_READ_ACP,     "x-amz-grant-read-acp"},
  { RGW_PERM_WRITE_ACP,    "x-amz-grant-write-acp"},
  { 0, NULL}
};

static bool grants_by_type_check_perm(map<int, string>& grants_by_type, int perm, ACLGrant& grant, int check_perm)
{
  if ((perm & check_perm) == check_perm) {
    grants_by_type_add_one_grant(grants_by_type, check_perm, grant);
    return true;
  }
  return false;
}

static void grants_by_type_add_perm(map<int, string>& grants_by_type, int perm, ACLGrant& grant)
{
  struct grant_type_to_header *t;

  for (t = grants_headers_def; t->header; t++) {
    if (grants_by_type_check_perm(grants_by_type, perm, grant, t->type))
      return;
  }
}

static void add_grants_headers(map<int, string>& grants, RGWEnv& env, map<string, string>& meta_map)
{
  struct grant_type_to_header *t;

  for (t = grants_headers_def; t->header; t++) {
    map<int, string>::iterator iter = grants.find(t->type);
    if (iter != grants.end()) {
      env.set(t->header,iter->second);
      meta_map[t->header] = iter->second;
    }
  }
}

int RGWRESTStreamWriteRequest::put_obj_init(RGWAccessKey& key, rgw_obj& obj, uint64_t obj_size, map<string, bufferlist>& attrs)
{
  string resource = obj.bucket.name + "/" + obj.get_oid();
  string new_url = url;
  if (new_url[new_url.size() - 1] != '/')
    new_url.append("/");

  string date_str;
  get_new_date_str(date_str);

  RGWEnv new_env;
  req_info new_info(cct, &new_env);
  
  string params_str;
  map<string, string>& args = new_info.args.get_params();
  get_params_str(args, params_str);

  new_url.append(resource + params_str);

  new_env.set("HTTP_DATE", date_str.c_str());

  new_info.method = "PUT";

  new_info.script_uri = "/";
  new_info.script_uri.append(resource);
  new_info.request_uri = new_info.script_uri;

  /* merge send headers */
  for (auto& attr: attrs) {
    bufferlist& bl = attr.second;
    const string& name = attr.first;
    string val = bl.c_str();
    if (name.compare(0, sizeof(RGW_ATTR_META_PREFIX) - 1, RGW_ATTR_META_PREFIX) == 0) {
      string header_name = RGW_AMZ_META_PREFIX;
      header_name.append(name.substr(sizeof(RGW_ATTR_META_PREFIX) - 1));
      new_env.set(header_name, val);
      new_info.x_meta_map[header_name] = val;
    }
  }
  RGWAccessControlPolicy policy;
  int ret = rgw_policy_from_attrset(cct, attrs, &policy);
  if (ret < 0) {
    ldout(cct, 0) << "ERROR: couldn't get policy ret=" << ret << dendl;
    return ret;
  }

  /* update acl headers */
  RGWAccessControlList& acl = policy.get_acl();
  multimap<string, ACLGrant>& grant_map = acl.get_grant_map();
  multimap<string, ACLGrant>::iterator giter;
  map<int, string> grants_by_type;
  for (giter = grant_map.begin(); giter != grant_map.end(); ++giter) {
    ACLGrant& grant = giter->second;
    ACLPermission& perm = grant.get_permission();
    grants_by_type_add_perm(grants_by_type, perm.get_permissions(), grant);
  }
  add_grants_headers(grants_by_type, new_env, new_info.x_meta_map);
  ret = sign_request(key, new_env, new_info);
  if (ret < 0) {
    ldout(cct, 0) << "ERROR: failed to sign request" << dendl;
    return ret;
  }

  for (const auto& kv: new_env.get_map()) {
    headers.emplace_back(kv);
  }

  cb = new RGWRESTStreamOutCB(this);

  set_send_length(obj_size);

  int r = http_manager.add_request(this, new_info.method, new_url.c_str());
  if (r < 0)
    return r;

  return 0;
}

int RGWRESTStreamWriteRequest::send_data(void *ptr, size_t len)
{
  uint64_t sent = 0;

  dout(20) << "RGWRESTStreamWriteRequest::send_data()" << dendl;
  lock.Lock();
  if (pending_send.empty() || status < 0) {
    lock.Unlock();
    return status;
  }

  list<bufferlist>::iterator iter = pending_send.begin();
  while (iter != pending_send.end() && len > 0) {
    bufferlist& bl = *iter;
    
    list<bufferlist>::iterator next_iter = iter;
    ++next_iter;
    lock.Unlock();

    uint64_t send_len = min(len, (size_t)bl.length());

    memcpy(ptr, bl.c_str(), send_len);

    ptr = (char *)ptr + send_len;
    len -= send_len;
    sent += send_len;

    lock.Lock();

    bufferlist new_bl;
    if (bl.length() > send_len) {
      bufferptr bp(bl.c_str() + send_len, bl.length() - send_len);
      new_bl.append(bp);
    }
    pending_send.pop_front(); /* need to do this after we copy data from bl */
    if (new_bl.length()) {
      pending_send.push_front(new_bl);
    }
    iter = next_iter;
  }
  lock.Unlock();

  return sent;
}


void set_str_from_headers(map<string, string>& out_headers, const string& header_name, string& str)
{
  map<string, string>::iterator iter = out_headers.find(header_name);
  if (iter != out_headers.end()) {
    str = iter->second;
  } else {
    str.clear();
  }
}

static int parse_rgwx_mtime(CephContext *cct, const string& s, ceph::real_time *rt)
{
  string err;
  vector<string> vec;

  get_str_vec(s, ".", vec);

  if (vec.empty()) {
    return -EINVAL;
  }

  long secs = strict_strtol(vec[0].c_str(), 10, &err);
  long nsecs = 0;
  if (!err.empty()) {
    ldout(cct, 0) << "ERROR: failed converting mtime (" << s << ") to real_time " << dendl;
    return -EINVAL;
  }

  if (vec.size() > 1) {
    nsecs = strict_strtol(vec[1].c_str(), 10, &err);
    if (!err.empty()) {
      ldout(cct, 0) << "ERROR: failed converting mtime (" << s << ") to real_time " << dendl;
      return -EINVAL;
    }
  }

  *rt = utime_t(secs, nsecs).to_real_time();

  return 0;
}

int RGWRESTStreamWriteRequest::complete(string& etag, real_time *mtime)
{
  int ret = http_manager.complete_requests();
  if (ret < 0)
    return ret;

  set_str_from_headers(out_headers, "ETAG", etag);

  if (mtime) {
    string mtime_str;
    set_str_from_headers(out_headers, "RGWX_MTIME", mtime_str);

    ret = parse_rgwx_mtime(cct, mtime_str, mtime);
    if (ret < 0) {
      return ret;
    }
  }
  return status;
}

int RGWRESTStreamRWRequest::send_request(RGWAccessKey& key, map<string, string>& extra_headers, rgw_obj& obj, RGWHTTPManager *mgr)
{
  string urlsafe_bucket, urlsafe_object;
  url_encode(obj.bucket.get_key(':', 0), urlsafe_bucket);
  url_encode(obj.key.name, urlsafe_object);
  string resource = urlsafe_bucket + "/" + urlsafe_object;

  return send_request(&key, extra_headers, resource, nullptr, mgr);
}

int RGWRESTStreamRWRequest::send_request(RGWAccessKey *key, map<string, string>& extra_headers, const string& resource,
                                         bufferlist *send_data, RGWHTTPManager *mgr)
{
  string new_url = url;
  if (new_url[new_url.size() - 1] != '/')
    new_url.append("/");

  string date_str;
  get_new_date_str(date_str);

  RGWEnv new_env;
  req_info new_info(cct, &new_env);
  
  string params_str;
  map<string, string>& args = new_info.args.get_params();
  get_params_str(args, params_str);

  /* merge params with extra args so that we can sign correctly */
  for (param_vec_t::iterator iter = params.begin(); iter != params.end(); ++iter) {
    new_info.args.append(iter->first, iter->second);
  }

  string new_resource;
  if (resource[0] == '/') {
    new_resource = resource.substr(1);
  } else {
    new_resource = resource;
  }

  new_url.append(new_resource + params_str);

  new_env.set("HTTP_DATE", date_str.c_str());

  for (map<string, string>::iterator iter = extra_headers.begin();
       iter != extra_headers.end(); ++iter) {
    new_env.set(iter->first.c_str(), iter->second.c_str());
  }

  new_info.method = method;

  new_info.script_uri = "/";
  new_info.script_uri.append(new_resource);
  new_info.request_uri = new_info.script_uri;

  new_info.init_meta_info(NULL);

  if (key) {
    int ret = sign_request(*key, new_env, new_info);
    if (ret < 0) {
      ldout(cct, 0) << "ERROR: failed to sign request" << dendl;
      return ret;
    }
  }

  for (const auto& kv: new_env.get_map()) {
    headers.emplace_back(kv);
  }

  bool send_data_hint = false;
  if (send_data) {
    outbl.claim(*send_data);
    send_data_hint = true;
  }

  RGWHTTPManager *pmanager = &http_manager;
  if (mgr) {
    pmanager = mgr;
  }

  int r = pmanager->add_request(this, new_info.method, new_url.c_str(), send_data_hint);
  if (r < 0)
    return r;

  if (!mgr) {
    r = pmanager->complete_requests();
    if (r < 0)
      return r;
  }

  return 0;
}

int RGWRESTStreamRWRequest::complete_request(string& etag, real_time *mtime, uint64_t *psize, map<string, string>& attrs)
{
  set_str_from_headers(out_headers, "ETAG", etag);
  if (status >= 0) {
    if (mtime) {
      string mtime_str;
      set_str_from_headers(out_headers, "RGWX_MTIME", mtime_str);
      if (!mtime_str.empty()) {
        int ret = parse_rgwx_mtime(cct, mtime_str, mtime);
        if (ret < 0) {
          return ret;
        }
      } else {
        *mtime = real_time();
      }
    }
    if (psize) {
      string size_str;
      set_str_from_headers(out_headers, "RGWX_OBJECT_SIZE", size_str);
      string err;
      *psize = strict_strtoll(size_str.c_str(), 10, &err);
      if (!err.empty()) {
        ldout(cct, 0) << "ERROR: failed parsing embedded metadata object size (" << size_str << ") to int " << dendl;
        return -EIO;
      }
    }
  }

  map<string, string>::iterator iter;
  for (iter = out_headers.begin(); iter != out_headers.end(); ++iter) {
    const string& attr_name = iter->first;
    if (attr_name.compare(0, sizeof(RGW_HTTP_RGWX_ATTR_PREFIX) - 1, RGW_HTTP_RGWX_ATTR_PREFIX) == 0) {
      string name = attr_name.substr(sizeof(RGW_HTTP_RGWX_ATTR_PREFIX) - 1);
      const char *src = name.c_str();
      char buf[name.size() + 1];
      char *dest = buf;
      for (; *src; ++src, ++dest) {
        switch(*src) {
          case '_':
            *dest = '-';
            break;
          default:
            *dest = tolower(*src);
        }
      }
      *dest = '\0';
      attrs[buf] = iter->second;
    }
  }
  return status;
}

int RGWRESTStreamRWRequest::handle_header(const string& name, const string& val)
{
  if (name == "RGWX_EMBEDDED_METADATA_LEN") {
    string err;
    long len = strict_strtol(val.c_str(), 10, &err);
    if (!err.empty()) {
      ldout(cct, 0) << "ERROR: failed converting embedded metadata len (" << val << ") to int " << dendl;
      return -EINVAL;
    }

    cb->set_extra_data_len(len);
  }
  return 0;
}

int RGWRESTStreamRWRequest::receive_data(void *ptr, size_t len)
{
  bufferptr bp((const char *)ptr, len);
  bufferlist bl;
  bl.append(bp);
  int ret = cb->handle_data(bl, ofs, len);
  if (ret < 0)
    return ret;
  ofs += len;
  return len;
}

int RGWRESTStreamRWRequest::send_data(void *ptr, size_t len)
{
  if (outbl.length() == 0) {
    return 0;
  }

  uint64_t send_size = min(len, (size_t)(outbl.length() - write_ofs));
  if (send_size > 0) {
    memcpy(ptr, outbl.c_str() + write_ofs, send_size);
    write_ofs += send_size;
  }
  return send_size;
}

class StreamIntoBufferlist : public RGWGetDataCB {
  bufferlist& bl;
public:
  StreamIntoBufferlist(bufferlist& _bl) : bl(_bl) {}
  int handle_data(bufferlist& inbl, off_t bl_ofs, off_t bl_len) override {
    bl.claim_append(inbl);
    return bl_len;
  }
};

