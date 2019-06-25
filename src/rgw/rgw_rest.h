// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_REST_H
#define CEPH_RGW_REST_H

#define TIME_BUF_SIZE 128

#include <boost/utility/string_ref.hpp>
#include <boost/container/flat_set.hpp>
#include "common/sstring.hh"
#include "common/ceph_json.h"
#include "include/assert.h" /* needed because of common/ceph_json.h */
#include "rgw_op.h"
#include "rgw_formats.h"
#include "rgw_client_io.h"

extern std::map<std::string, std::string> rgw_to_http_attrs;

extern string camelcase_dash_http_attr(const string& orig);
extern string lowercase_dash_http_attr(const string& orig);

extern void rgw_rest_init(CephContext *cct, RGWRados *store, RGWZoneGroup& zone_group);

extern void rgw_flush_formatter_and_reset(struct req_state *s,
					 ceph::Formatter *formatter);

extern void rgw_flush_formatter(struct req_state *s,
				ceph::Formatter *formatter);

extern int rgw_rest_read_all_input(struct req_state *s, char **data, int *plen,
				   uint64_t max_len, bool allow_chunked=true);

template <class T>
int rgw_rest_get_json_input(CephContext *cct, req_state *s, T& out,
			    uint64_t max_len, bool *empty)
{
  int rv, data_len;
  char *data;

  if (empty)
    *empty = false;

  if ((rv = rgw_rest_read_all_input(s, &data, &data_len, max_len)) < 0) {
    return rv;
  }

  if (!data_len) {
    if (empty) {
      *empty = true;
    }

    return -EINVAL;
  }

  JSONParser parser;

  if (!parser.parse(data, data_len)) {
    free(data);
    return -EINVAL;
  }

  free(data);

  try {
      decode_json_obj(out, &parser);
  } catch (JSONDecoder::err& e) {
      return -EINVAL;
  }

  return 0;
}

template <class T>
int rgw_rest_get_json_input_keep_data(CephContext *cct, req_state *s, T& out, uint64_t max_len, char **pdata, int *len)
{
  int rv, data_len;
  char *data;

  if ((rv = rgw_rest_read_all_input(s, &data, &data_len, max_len)) < 0) {
    return rv;
  }

  if (!data_len) {
    return -EINVAL;
  }

  *len = data_len;

  JSONParser parser;

  if (!parser.parse(data, data_len)) {
    free(data);
    return -EINVAL;
  }

  try {
    decode_json_obj(out, &parser);
  } catch (JSONDecoder::err& e) {
    free(data);
    return -EINVAL;
  }

  *pdata = data;
  return 0;
}

class RESTArgs {
public:
  static int get_string(struct req_state *s, const string& name,
			const string& def_val, string *val,
			bool *existed = NULL);
  static int get_uint64(struct req_state *s, const string& name,
			uint64_t def_val, uint64_t *val, bool *existed = NULL);
  static int get_int64(struct req_state *s, const string& name,
		       int64_t def_val, int64_t *val, bool *existed = NULL);
  static int get_uint32(struct req_state *s, const string& name,
			uint32_t def_val, uint32_t *val, bool *existed = NULL);
  static int get_int32(struct req_state *s, const string& name,
		       int32_t def_val, int32_t *val, bool *existed = NULL);
  static int get_time(struct req_state *s, const string& name,
		      const utime_t& def_val, utime_t *val,
		      bool *existed = NULL);
  static int get_epoch(struct req_state *s, const string& name,
		       uint64_t def_val, uint64_t *epoch,
		       bool *existed = NULL);
  static int get_bool(struct req_state *s, const string& name, bool def_val,
		      bool *val, bool *existed = NULL);
};

class RGWRESTFlusher : public RGWFormatterFlusher {
  struct req_state *s;
  RGWOp *op;
protected:
  void do_flush() override;
  void do_start(int ret) override;
public:
  RGWRESTFlusher(struct req_state *_s, RGWOp *_op) :
    RGWFormatterFlusher(_s->formatter), s(_s), op(_op) {}
  RGWRESTFlusher() : RGWFormatterFlusher(NULL), s(NULL), op(NULL) {}

  void init(struct req_state *_s, RGWOp *_op) {
    s = _s;
    op = _op;
    set_formatter(s->formatter);
  }
};

class RGWGetObj_ObjStore : public RGWGetObj
{
protected:
  bool sent_header;
public:
  RGWGetObj_ObjStore() : sent_header(false) {}

  void init(RGWRados *store, struct req_state *s, RGWHandler *h) override {
    RGWGetObj::init(store, s, h);
    sent_header = false;
  }

  int get_params() override;
};

class RGWGetObjTags_ObjStore : public RGWGetObjTags {
public:
  RGWGetObjTags_ObjStore() {};
  ~RGWGetObjTags_ObjStore() {};
};

class RGWPutObjTags_ObjStore: public RGWPutObjTags {
public:
  RGWPutObjTags_ObjStore() {};
  ~RGWPutObjTags_ObjStore() {};
};

class RGWListBuckets_ObjStore : public RGWListBuckets {
public:
  RGWListBuckets_ObjStore() {}
  ~RGWListBuckets_ObjStore() override {}
};

class RGWGetUsage_ObjStore : public RGWGetUsage {
public:
  RGWGetUsage_ObjStore() {}
  ~RGWGetUsage_ObjStore() override {}
};

class RGWListBucket_ObjStore : public RGWListBucket {
public:
  RGWListBucket_ObjStore() {}
  ~RGWListBucket_ObjStore() override {}
};

class RGWStatAccount_ObjStore : public RGWStatAccount {
public:
  RGWStatAccount_ObjStore() {}
  ~RGWStatAccount_ObjStore() override {}
};

class RGWStatBucket_ObjStore : public RGWStatBucket {
public:
  RGWStatBucket_ObjStore() {}
  ~RGWStatBucket_ObjStore() override {}
};

class RGWCreateBucket_ObjStore : public RGWCreateBucket {
public:
  RGWCreateBucket_ObjStore() {}
  ~RGWCreateBucket_ObjStore() override {}
};

class RGWDeleteBucket_ObjStore : public RGWDeleteBucket {
public:
  RGWDeleteBucket_ObjStore() {}
  ~RGWDeleteBucket_ObjStore() override {}
};

class RGWPutObj_ObjStore : public RGWPutObj
{
public:
  RGWPutObj_ObjStore() {}
  ~RGWPutObj_ObjStore() override {}

  int verify_params() override;
  int get_params() override;
  int get_data(bufferlist& bl) override;
};

class RGWPostObj_ObjStore : public RGWPostObj
{
  std::string boundary;

public:
  struct post_part_field {
    std::string val;
    std::map<std::string, std::string> params;
  };

  struct post_form_part {
    std::string name;
    std::map<std::string, post_part_field, ltstr_nocase> fields;
    ceph::bufferlist data;
  };

protected:
  using parts_collection_t = \
    std::map<std::string, post_form_part, const ltstr_nocase>;

  std::string err_msg;
  ceph::bufferlist in_data;

  int read_with_boundary(ceph::bufferlist& bl,
                         uint64_t max,
                         bool check_eol,
                         bool& reached_boundary,
                         bool& done);

  int read_line(ceph::bufferlist& bl,
                uint64_t max,
                bool& reached_boundary,
                bool& done);

  int read_data(ceph::bufferlist& bl,
                uint64_t max,
                bool& reached_boundary,
                bool& done);

  int read_form_part_header(struct post_form_part *part, bool& done);

  int get_params() override;

  static int parse_part_field(const std::string& line,
                              std::string& field_name, /* out */
                              post_part_field& field); /* out */

  static void parse_boundary_params(const std::string& params_str,
                                    std::string& first,
                                    std::map<std::string, std::string>& params);

  static bool part_str(parts_collection_t& parts,
                       const std::string& name,
                       std::string *val);

  static std::string get_part_str(parts_collection_t& parts,
                                  const std::string& name,
                                  const std::string& def_val = std::string());

  static bool part_bl(parts_collection_t& parts,
                      const std::string& name,
                      ceph::bufferlist *pbl);

public:
  RGWPostObj_ObjStore() {}
  ~RGWPostObj_ObjStore() override {}

  int verify_params() override;
};


class RGWPutMetadataAccount_ObjStore : public RGWPutMetadataAccount
{
public:
  RGWPutMetadataAccount_ObjStore() {}
  ~RGWPutMetadataAccount_ObjStore() override {}
};

class RGWPutMetadataBucket_ObjStore : public RGWPutMetadataBucket
{
public:
  RGWPutMetadataBucket_ObjStore() {}
  ~RGWPutMetadataBucket_ObjStore() override {}
};

class RGWPutMetadataObject_ObjStore : public RGWPutMetadataObject
{
public:
  RGWPutMetadataObject_ObjStore() {}
  ~RGWPutMetadataObject_ObjStore() override {}
};

class RGWDeleteObj_ObjStore : public RGWDeleteObj {
public:
  RGWDeleteObj_ObjStore() {}
  ~RGWDeleteObj_ObjStore() override {}
};

class  RGWGetCrossDomainPolicy_ObjStore : public RGWGetCrossDomainPolicy {
public:
  RGWGetCrossDomainPolicy_ObjStore() = default;
  ~RGWGetCrossDomainPolicy_ObjStore() override = default;
};

class  RGWGetHealthCheck_ObjStore : public RGWGetHealthCheck {
public:
  RGWGetHealthCheck_ObjStore() = default;
  ~RGWGetHealthCheck_ObjStore() override = default;
};

class RGWCopyObj_ObjStore : public RGWCopyObj {
public:
  RGWCopyObj_ObjStore() {}
  ~RGWCopyObj_ObjStore() override {}
};

class RGWGetACLs_ObjStore : public RGWGetACLs {
public:
  RGWGetACLs_ObjStore() {}
  ~RGWGetACLs_ObjStore() override {}
};

class RGWPutACLs_ObjStore : public RGWPutACLs {
public:
  RGWPutACLs_ObjStore() {}
  ~RGWPutACLs_ObjStore() override {}

  int get_params() override;
};

class RGWGetLC_ObjStore : public RGWGetLC {
public:
  RGWGetLC_ObjStore() {}
  ~RGWGetLC_ObjStore() override {}
};

class RGWPutLC_ObjStore : public RGWPutLC {
public:
  RGWPutLC_ObjStore() {}
  ~RGWPutLC_ObjStore() override {}

  int get_params() override;
};

class RGWDeleteLC_ObjStore : public RGWDeleteLC {
public:
  RGWDeleteLC_ObjStore() {}
  ~RGWDeleteLC_ObjStore() override {}

};

class RGWGetCORS_ObjStore : public RGWGetCORS {
public:
  RGWGetCORS_ObjStore() {}
  ~RGWGetCORS_ObjStore() override {}
};

class RGWPutCORS_ObjStore : public RGWPutCORS {
public:
  RGWPutCORS_ObjStore() {}
  ~RGWPutCORS_ObjStore() override {}
};

class RGWDeleteCORS_ObjStore : public RGWDeleteCORS {
public:
  RGWDeleteCORS_ObjStore() {}
  ~RGWDeleteCORS_ObjStore() override {}
};

class RGWOptionsCORS_ObjStore : public RGWOptionsCORS {
public:
  RGWOptionsCORS_ObjStore() {}
  ~RGWOptionsCORS_ObjStore() override {}
};

class RGWInitMultipart_ObjStore : public RGWInitMultipart {
public:
  RGWInitMultipart_ObjStore() {}
  ~RGWInitMultipart_ObjStore() override {}
};

class RGWCompleteMultipart_ObjStore : public RGWCompleteMultipart {
public:
  RGWCompleteMultipart_ObjStore() {}
  ~RGWCompleteMultipart_ObjStore() override {}

  int get_params() override;
};

class RGWAbortMultipart_ObjStore : public RGWAbortMultipart {
public:
  RGWAbortMultipart_ObjStore() {}
  ~RGWAbortMultipart_ObjStore() override {}
};

class RGWListMultipart_ObjStore : public RGWListMultipart {
public:
  RGWListMultipart_ObjStore() {}
  ~RGWListMultipart_ObjStore() override {}

  int get_params() override;
};

class RGWListBucketMultiparts_ObjStore : public RGWListBucketMultiparts {
public:
  RGWListBucketMultiparts_ObjStore() {}
  ~RGWListBucketMultiparts_ObjStore() override {}

  int get_params() override;
};

class RGWBulkDelete_ObjStore : public RGWBulkDelete {
public:
  RGWBulkDelete_ObjStore() {}
  ~RGWBulkDelete_ObjStore() override {}
};

class RGWBulkUploadOp_ObjStore : public RGWBulkUploadOp {
public:
  RGWBulkUploadOp_ObjStore() = default;
  ~RGWBulkUploadOp_ObjStore() = default;
};

class RGWDeleteMultiObj_ObjStore : public RGWDeleteMultiObj {
public:
  RGWDeleteMultiObj_ObjStore() {}
  ~RGWDeleteMultiObj_ObjStore() override {}

  int get_params() override;
};

class RGWInfo_ObjStore : public RGWInfo {
public:
    RGWInfo_ObjStore() = default;
    ~RGWInfo_ObjStore() override = default;
};

class RGWRESTOp : public RGWOp {
protected:
  int http_ret;
  RGWRESTFlusher flusher;
public:
  RGWRESTOp() : http_ret(0) {}
  void init(RGWRados *store, struct req_state *s,
            RGWHandler *dialect_handler) override {
    RGWOp::init(store, s, dialect_handler);
    flusher.init(s, this);
  }
  void send_response() override;
  virtual int check_caps(RGWUserCaps& caps)
    { return -EPERM; } /* should to be implemented! */
  int verify_permission() override;
};

class RGWHandler_REST : public RGWHandler {
protected:

  virtual bool is_obj_update_op() { return false; }
  virtual RGWOp *op_get() { return NULL; }
  virtual RGWOp *op_put() { return NULL; }
  virtual RGWOp *op_delete() { return NULL; }
  virtual RGWOp *op_head() { return NULL; }
  virtual RGWOp *op_post() { return NULL; }
  virtual RGWOp *op_copy() { return NULL; }
  virtual RGWOp *op_options() { return NULL; }

  static int allocate_formatter(struct req_state *s, int default_formatter,
				bool configurable);
public:
  static constexpr int MAX_BUCKET_NAME_LEN = 255;
  static constexpr int MAX_OBJ_NAME_LEN = 1024;

  RGWHandler_REST() {}
  ~RGWHandler_REST() override {}

  static int validate_bucket_name(const string& bucket);
  static int validate_object_name(const string& object);

  int init_permissions(RGWOp* op) override;
  int read_permissions(RGWOp* op) override;

  virtual RGWOp* get_op(RGWRados* store);
  virtual void put_op(RGWOp* op);
};

class RGWHandler_REST_SWIFT;
class RGWHandler_SWIFT_Auth;
class RGWHandler_REST_S3;

namespace rgw {
namespace auth {

class StrategyRegistry;

}
}

class RGWRESTMgr {
  bool should_log;

protected:
  std::map<std::string, RGWRESTMgr*> resource_mgrs;
  std::multimap<size_t, std::string> resources_by_size;
  RGWRESTMgr* default_mgr;

  virtual RGWRESTMgr* get_resource_mgr(struct req_state* s,
                                       const std::string& uri,
                                       std::string* out_uri);

  virtual RGWRESTMgr* get_resource_mgr_as_default(struct req_state* const s,
                                                  const std::string& uri,
                                                  std::string* our_uri) {
    return this;
  }

public:
  RGWRESTMgr()
    : should_log(false),
      default_mgr(nullptr) {
  }
  virtual ~RGWRESTMgr();

  void register_resource(std::string resource, RGWRESTMgr* mgr);
  void register_default_mgr(RGWRESTMgr* mgr);

  virtual RGWRESTMgr* get_manager(struct req_state* const s,
                                  /* Prefix to be concatenated with @uri
                                   * during the lookup. */
                                  const std::string& frontend_prefix,
                                  const std::string& uri,
                                  std::string* out_uri) final {
    return get_resource_mgr(s, frontend_prefix + uri, out_uri);
  }

  virtual RGWHandler_REST* get_handler(
    struct req_state* const s,
    const rgw::auth::StrategyRegistry& auth_registry,
    const std::string& frontend_prefix
  ) {
    return nullptr;
  }

  virtual void put_handler(RGWHandler_REST* const handler) {
    delete handler;
  }

  void set_logging(bool _should_log) {
    should_log = _should_log;
  }

  bool get_logging() const {
    return should_log;
  }
};

class RGWLibIO;
class RGWRestfulIO;

class RGWREST {
  using x_header = basic_sstring<char, uint16_t, 32>;
  boost::container::flat_set<x_header> x_headers;
  RGWRESTMgr mgr;

  static int preprocess(struct req_state *s, rgw::io::BasicClient* rio);
public:
  RGWREST() {}
  RGWHandler_REST *get_handler(RGWRados *store,
                               struct req_state *s,
                               const rgw::auth::StrategyRegistry& auth_registry,
                               const std::string& frontend_prefix,
                               RGWRestfulIO *rio,
                               RGWRESTMgr **pmgr,
                               int *init_error);
#if 0
  RGWHandler *get_handler(RGWRados *store, struct req_state *s,
			  RGWLibIO *io, RGWRESTMgr **pmgr,
			  int *init_error);
#endif

  void put_handler(RGWHandler_REST *handler) {
    mgr.put_handler(handler);
  }

  void register_resource(string resource, RGWRESTMgr *m,
			 bool register_empty = false) {
    if (!register_empty && resource.empty())
      return;

    mgr.register_resource(resource, m);
  }

  void register_default_mgr(RGWRESTMgr *m) {
    mgr.register_default_mgr(m);
  }

  void register_x_headers(const std::string& headers);

  bool log_x_headers(void) {
    return (x_headers.size() > 0);
  }

  bool log_x_header(const std::string& header) {
    return (x_headers.find(header) != x_headers.end());
  }
};

static constexpr int64_t NO_CONTENT_LENGTH = -1;
static constexpr int64_t CHUNKED_TRANSFER_ENCODING = -2;

extern void dump_errno(int http_ret, string& out);
extern void dump_errno(const struct rgw_err &err, string& out);
extern void dump_errno(struct req_state *s);
extern void dump_errno(struct req_state *s, int http_ret);
extern void end_header(struct req_state *s,
                       RGWOp* op = nullptr,
                       const char *content_type = nullptr,
                       const int64_t proposed_content_length =
		       NO_CONTENT_LENGTH,
		       bool force_content_type = false,
		       bool force_no_error = false);
extern void dump_start(struct req_state *s);
extern void list_all_buckets_start(struct req_state *s);
extern void dump_owner(struct req_state *s, const rgw_user& id, string& name,
		       const char *section = NULL);
extern void dump_header(struct req_state* s,
                        const boost::string_ref& name,
                        const boost::string_ref& val);
extern void dump_header(struct req_state* s,
                        const boost::string_ref& name,
                        ceph::buffer::list& bl);
extern void dump_header(struct req_state* s,
                        const boost::string_ref& name,
                        long long val);
extern void dump_header(struct req_state* s,
                        const boost::string_ref& name,
                        const utime_t& val);

template <class... Args>
static inline void dump_header_prefixed(struct req_state* s,
                                        const boost::string_ref& name_prefix,
                                        const boost::string_ref& name,
                                        Args&&... args) {
  char full_name_buf[name_prefix.size() + name.size() + 1];
  const auto len = snprintf(full_name_buf, sizeof(full_name_buf), "%.*s%.*s",
                            static_cast<int>(name_prefix.length()),
                            name_prefix.data(),
                            static_cast<int>(name.length()),
                            name.data());
  boost::string_ref full_name(full_name_buf, len);
  return dump_header(s, std::move(full_name), std::forward<Args>(args)...);
}

template <class... Args>
static inline void dump_header_infixed(struct req_state* s,
                                       const boost::string_ref& prefix,
                                       const boost::string_ref& infix,
                                       const boost::string_ref& sufix,
                                       Args&&... args) {
  char full_name_buf[prefix.size() + infix.size() + sufix.size() + 1];
  const auto len = snprintf(full_name_buf, sizeof(full_name_buf), "%.*s%.*s%.*s",
                            static_cast<int>(prefix.length()),
                            prefix.data(),
                            static_cast<int>(infix.length()),
                            infix.data(),
                            static_cast<int>(sufix.length()),
                            sufix.data());
  boost::string_ref full_name(full_name_buf, len);
  return dump_header(s, std::move(full_name), std::forward<Args>(args)...);
}

template <class... Args>
static inline void dump_header_quoted(struct req_state* s,
                                      const boost::string_ref& name,
                                      const boost::string_ref& val) {
  /* We need two extra bytes for quotes. */
  char qvalbuf[val.size() + 2 + 1];
  const auto len = snprintf(qvalbuf, sizeof(qvalbuf), "\"%.*s\"",
                            static_cast<int>(val.length()), val.data());
  return dump_header(s, name, boost::string_ref(qvalbuf, len));
}

template <class ValueT>
static inline void dump_header_if_nonempty(struct req_state* s,
                                           const boost::string_ref& name,
                                           const ValueT& value) {
  if (name.length() > 0 && value.length() > 0) {
    return dump_header(s, name, value);
  }
}

static inline std::string compute_domain_uri(const struct req_state *s) {
  std::string uri = (!s->info.domain.empty()) ? s->info.domain :
    [&s]() -> std::string {
    auto env = *(s->info.env);
    std::string uri =
    env.get("SERVER_PORT_SECURE") ? "https://" : "http://";
    if (env.exists("SERVER_NAME")) {
      uri.append(env.get("SERVER_NAME", "<SERVER_NAME>"));
    } else {
      uri.append(env.get("HTTP_HOST", "<HTTP_HOST>"));
    }
    return uri;
  }();
  return uri;
}

extern void dump_content_length(struct req_state *s, uint64_t len);
extern int64_t parse_content_length(const char *content_length);
extern void dump_etag(struct req_state *s,
                      const boost::string_ref& etag,
                      bool quoted = false);
extern void dump_etag(struct req_state *s,
                      ceph::buffer::list& bl_etag,
                      bool quoted = false);
extern void dump_epoch_header(struct req_state *s, const char *name, real_time t);
extern void dump_time_header(struct req_state *s, const char *name, real_time t);
extern void dump_last_modified(struct req_state *s, real_time t);
extern void abort_early(struct req_state* s, RGWOp* op, int err,
			RGWHandler* handler);
extern void dump_range(struct req_state* s, uint64_t ofs, uint64_t end,
		       uint64_t total_size);
extern void dump_continue(struct req_state *s);
extern void list_all_buckets_end(struct req_state *s);
extern void dump_time(struct req_state *s, const char *name, real_time *t);
extern std::string dump_time_to_str(const real_time& t);
extern void dump_bucket_from_state(struct req_state *s);
extern void dump_uri_from_state(struct req_state *s);
extern void dump_redirect(struct req_state *s, const string& redirect);
extern bool is_valid_url(const char *url);
extern void dump_access_control(struct req_state *s, const char *origin,
				const char *meth,
				const char *hdr, const char *exp_hdr,
				uint32_t max_age);
extern void dump_access_control(req_state *s, RGWOp *op);

extern int dump_body(struct req_state* s, const char* buf, size_t len);
extern int dump_body(struct req_state* s, /* const */ ceph::buffer::list& bl);
extern int dump_body(struct req_state* s, const std::string& str);
extern int recv_body(struct req_state* s, char* buf, size_t max);

#endif /* CEPH_RGW_REST_H */
