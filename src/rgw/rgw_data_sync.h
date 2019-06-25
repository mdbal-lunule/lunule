#ifndef CEPH_RGW_DATA_SYNC_H
#define CEPH_RGW_DATA_SYNC_H

#include "rgw_coroutine.h"
#include "rgw_http_client.h"
#include "rgw_bucket.h"

#include "rgw_sync_module.h"

#include "common/RWLock.h"
#include "common/ceph_json.h"

namespace rgw {
class BucketChangeObserver;
}

struct rgw_datalog_info {
  uint32_t num_shards;

  rgw_datalog_info() : num_shards(0) {}

  void decode_json(JSONObj *obj);
};

struct rgw_data_sync_info {
  enum SyncState {
    StateInit = 0,
    StateBuildingFullSyncMaps = 1,
    StateSync = 2,
  };

  uint16_t state;
  uint32_t num_shards;

  uint64_t instance_id{0};

  void encode(bufferlist& bl) const {
    ENCODE_START(2, 1, bl);
    ::encode(state, bl);
    ::encode(num_shards, bl);
    ::encode(instance_id, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(2, bl);
     ::decode(state, bl);
     ::decode(num_shards, bl);
     if (struct_v >= 2) {
       ::decode(instance_id, bl);
     }
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const {
    string s;
    switch ((SyncState)state) {
      case StateInit:
	s = "init";
	break;
      case StateBuildingFullSyncMaps:
	s = "building-full-sync-maps";
	break;
      case StateSync:
	s = "sync";
	break;
      default:
	s = "unknown";
	break;
    }
    encode_json("status", s, f);
    encode_json("num_shards", num_shards, f);
    encode_json("instance_id", instance_id, f);
  }
  void decode_json(JSONObj *obj) {
    std::string s;
    JSONDecoder::decode_json("status", s, obj);
    if (s == "building-full-sync-maps") {
      state = StateBuildingFullSyncMaps;
    } else if (s == "sync") {
      state = StateSync;
    } else {
      state = StateInit;
    }
    JSONDecoder::decode_json("num_shards", num_shards, obj);
    JSONDecoder::decode_json("instance_id", num_shards, obj);
  }
  static void generate_test_instances(std::list<rgw_data_sync_info*>& o);

  rgw_data_sync_info() : state((int)StateInit), num_shards(0) {}
};
WRITE_CLASS_ENCODER(rgw_data_sync_info)

struct rgw_data_sync_marker {
  enum SyncState {
    FullSync = 0,
    IncrementalSync = 1,
  };
  uint16_t state;
  string marker;
  string next_step_marker;
  uint64_t total_entries;
  uint64_t pos;
  real_time timestamp;

  rgw_data_sync_marker() : state(FullSync), total_entries(0), pos(0) {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(state, bl);
    ::encode(marker, bl);
    ::encode(next_step_marker, bl);
    ::encode(total_entries, bl);
    ::encode(pos, bl);
    ::encode(timestamp, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
    ::decode(state, bl);
    ::decode(marker, bl);
    ::decode(next_step_marker, bl);
    ::decode(total_entries, bl);
    ::decode(pos, bl);
    ::decode(timestamp, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const {
    encode_json("state", (int)state, f);
    encode_json("marker", marker, f);
    encode_json("next_step_marker", next_step_marker, f);
    encode_json("total_entries", total_entries, f);
    encode_json("pos", pos, f);
    encode_json("timestamp", utime_t(timestamp), f);
  }
  void decode_json(JSONObj *obj) {
    int s;
    JSONDecoder::decode_json("state", s, obj);
    state = s;
    JSONDecoder::decode_json("marker", marker, obj);
    JSONDecoder::decode_json("next_step_marker", next_step_marker, obj);
    JSONDecoder::decode_json("total_entries", total_entries, obj);
    JSONDecoder::decode_json("pos", pos, obj);
    utime_t t;
    JSONDecoder::decode_json("timestamp", t, obj);
    timestamp = t.to_real_time();
  }
  static void generate_test_instances(std::list<rgw_data_sync_marker*>& o);
};
WRITE_CLASS_ENCODER(rgw_data_sync_marker)

struct rgw_data_sync_status {
  rgw_data_sync_info sync_info;
  map<uint32_t, rgw_data_sync_marker> sync_markers;

  rgw_data_sync_status() {}

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(sync_info, bl);
    /* sync markers are encoded separately */
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
    ::decode(sync_info, bl);
    /* sync markers are decoded separately */
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const {
    encode_json("info", sync_info, f);
    encode_json("markers", sync_markers, f);
  }
  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("info", sync_info, obj);
    JSONDecoder::decode_json("markers", sync_markers, obj);
  }
  static void generate_test_instances(std::list<rgw_data_sync_status*>& o);
};
WRITE_CLASS_ENCODER(rgw_data_sync_status)

struct rgw_datalog_entry {
  string key;
  ceph::real_time timestamp;

  void decode_json(JSONObj *obj);
};

struct rgw_datalog_shard_data {
  string marker;
  bool truncated;
  vector<rgw_datalog_entry> entries;

  void decode_json(JSONObj *obj);
};

class RGWAsyncRadosProcessor;
class RGWDataSyncControlCR;

struct rgw_bucket_entry_owner {
  string id;
  string display_name;

  rgw_bucket_entry_owner() {}
  rgw_bucket_entry_owner(const string& _id, const string& _display_name) : id(_id), display_name(_display_name) {}

  void decode_json(JSONObj *obj);
};

class RGWSyncErrorLogger;

struct RGWDataSyncEnv {
  CephContext *cct;
  RGWRados *store;
  RGWRESTConn *conn;
  RGWAsyncRadosProcessor *async_rados;
  RGWHTTPManager *http_manager;
  RGWSyncErrorLogger *error_logger;
  string source_zone;
  RGWSyncModuleInstanceRef sync_module;
  rgw::BucketChangeObserver *observer{nullptr};

  RGWDataSyncEnv() : cct(NULL), store(NULL), conn(NULL), async_rados(NULL), http_manager(NULL), error_logger(NULL), sync_module(NULL) {}

  void init(CephContext *_cct, RGWRados *_store, RGWRESTConn *_conn,
            RGWAsyncRadosProcessor *_async_rados, RGWHTTPManager *_http_manager,
            RGWSyncErrorLogger *_error_logger, const string& _source_zone,
            RGWSyncModuleInstanceRef& _sync_module,
            rgw::BucketChangeObserver *_observer) {
    cct = _cct;
    store = _store;
    conn = _conn;
    async_rados = _async_rados;
    http_manager = _http_manager;
    error_logger = _error_logger;
    source_zone = _source_zone;
    sync_module = _sync_module;
    observer = _observer;
  }

  string shard_obj_name(int shard_id);
  string status_oid();
};

class RGWRemoteDataLog : public RGWCoroutinesManager {
  RGWRados *store;
  RGWAsyncRadosProcessor *async_rados;
  rgw::BucketChangeObserver *observer;
  RGWHTTPManager http_manager;

  RGWDataSyncEnv sync_env;

  RWLock lock;
  RGWDataSyncControlCR *data_sync_cr;

  bool initialized;

public:
  RGWRemoteDataLog(RGWRados *_store, RGWAsyncRadosProcessor *async_rados,
                   rgw::BucketChangeObserver *observer)
    : RGWCoroutinesManager(_store->ctx(), _store->get_cr_registry()),
      store(_store), async_rados(async_rados), observer(observer),
      http_manager(store->ctx(), completion_mgr),
      lock("RGWRemoteDataLog::lock"), data_sync_cr(NULL),
      initialized(false) {}
  int init(const string& _source_zone, RGWRESTConn *_conn, RGWSyncErrorLogger *_error_logger, RGWSyncModuleInstanceRef& module);
  void finish();

  int read_log_info(rgw_datalog_info *log_info);
  int read_source_log_shards_info(map<int, RGWDataChangesLogInfo> *shards_info);
  int read_source_log_shards_next(map<int, string> shard_markers, map<int, rgw_datalog_shard_data> *result);
  int read_sync_status(rgw_data_sync_status *sync_status);
  int init_sync_status(int num_shards);
  int run_sync(int num_shards);

  void wakeup(int shard_id, set<string>& keys);
};

class RGWDataSyncStatusManager {
  RGWRados *store;
  rgw_rados_ref ref;

  string source_zone;
  RGWRESTConn *conn;
  RGWSyncErrorLogger *error_logger;
  RGWSyncModuleInstanceRef sync_module;

  RGWRemoteDataLog source_log;

  string source_status_oid;
  string source_shard_status_oid_prefix;

  map<int, rgw_raw_obj> shard_objs;

  int num_shards;

public:
  RGWDataSyncStatusManager(RGWRados *_store, RGWAsyncRadosProcessor *async_rados,
                           const string& _source_zone,
                           rgw::BucketChangeObserver *observer = nullptr)
    : store(_store), source_zone(_source_zone), conn(NULL), error_logger(NULL),
      sync_module(nullptr),
      source_log(store, async_rados, observer), num_shards(0) {}
  RGWDataSyncStatusManager(RGWRados *_store, RGWAsyncRadosProcessor *async_rados,
                           const string& _source_zone, const RGWSyncModuleInstanceRef& _sync_module,
                           rgw::BucketChangeObserver *observer = nullptr)
    : store(_store), source_zone(_source_zone), conn(NULL), error_logger(NULL),
      sync_module(_sync_module),
      source_log(store, async_rados, observer), num_shards(0) {}
  ~RGWDataSyncStatusManager() {
    finalize();
  }
  int init();
  void finalize();

  static string shard_obj_name(const string& source_zone, int shard_id);
  static string sync_status_oid(const string& source_zone);

  int read_sync_status(rgw_data_sync_status *sync_status) {
    return source_log.read_sync_status(sync_status);
  }
  int init_sync_status() { return source_log.init_sync_status(num_shards); }

  int read_log_info(rgw_datalog_info *log_info) {
    return source_log.read_log_info(log_info);
  }
  int read_source_log_shards_info(map<int, RGWDataChangesLogInfo> *shards_info) {
    return source_log.read_source_log_shards_info(shards_info);
  }
  int read_source_log_shards_next(map<int, string> shard_markers, map<int, rgw_datalog_shard_data> *result) {
    return source_log.read_source_log_shards_next(shard_markers, result);
  }

  int run() { return source_log.run_sync(num_shards); }

  void wakeup(int shard_id, set<string>& keys) { return source_log.wakeup(shard_id, keys); }
  void stop() {
    source_log.finish();
  }
};

class RGWBucketSyncStatusManager;
class RGWBucketSyncCR;

struct rgw_bucket_shard_full_sync_marker {
  rgw_obj_key position;
  uint64_t count;

  rgw_bucket_shard_full_sync_marker() : count(0) {}

  void encode_attr(map<string, bufferlist>& attrs);

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(position, bl);
    ::encode(count, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
    ::decode(position, bl);
    ::decode(count, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);
};
WRITE_CLASS_ENCODER(rgw_bucket_shard_full_sync_marker)

struct rgw_bucket_shard_inc_sync_marker {
  string position;

  rgw_bucket_shard_inc_sync_marker() {}

  void encode_attr(map<string, bufferlist>& attrs);

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(position, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
    ::decode(position, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);

  bool operator<(const rgw_bucket_shard_inc_sync_marker& m) const {
    return (position < m.position);
  }
};
WRITE_CLASS_ENCODER(rgw_bucket_shard_inc_sync_marker)

struct rgw_bucket_shard_sync_info {
  enum SyncState {
    StateInit = 0,
    StateFullSync = 1,
    StateIncrementalSync = 2,
  };

  uint16_t state;
  rgw_bucket_shard_full_sync_marker full_marker;
  rgw_bucket_shard_inc_sync_marker inc_marker;

  void decode_from_attrs(CephContext *cct, map<string, bufferlist>& attrs);
  void encode_all_attrs(map<string, bufferlist>& attrs);
  void encode_state_attr(map<string, bufferlist>& attrs);

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    ::encode(state, bl);
    ::encode(full_marker, bl);
    ::encode(inc_marker, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::iterator& bl) {
     DECODE_START(1, bl);
     ::decode(state, bl);
     ::decode(full_marker, bl);
     ::decode(inc_marker, bl);
     DECODE_FINISH(bl);
  }

  void dump(Formatter *f) const;
  void decode_json(JSONObj *obj);

  rgw_bucket_shard_sync_info() : state((int)StateInit) {}

};
WRITE_CLASS_ENCODER(rgw_bucket_shard_sync_info)


class RGWRemoteBucketLog : public RGWCoroutinesManager {
  RGWRados *store;
  RGWRESTConn *conn{nullptr};
  string source_zone;
  rgw_bucket_shard bs;

  RGWBucketSyncStatusManager *status_manager;
  RGWAsyncRadosProcessor *async_rados;
  RGWHTTPManager *http_manager;

  RGWDataSyncEnv sync_env;
  rgw_bucket_shard_sync_info init_status;

  RGWBucketSyncCR *sync_cr{nullptr};

public:
  RGWRemoteBucketLog(RGWRados *_store, RGWBucketSyncStatusManager *_sm,
                     RGWAsyncRadosProcessor *_async_rados, RGWHTTPManager *_http_manager) : RGWCoroutinesManager(_store->ctx(), _store->get_cr_registry()), store(_store),
                                       status_manager(_sm), async_rados(_async_rados), http_manager(_http_manager) {}

  int init(const string& _source_zone, RGWRESTConn *_conn,
           const rgw_bucket& bucket, int shard_id,
           RGWSyncErrorLogger *_error_logger,
           RGWSyncModuleInstanceRef& _sync_module);
  void finish();

  RGWCoroutine *read_sync_status_cr(rgw_bucket_shard_sync_info *sync_status);
  RGWCoroutine *init_sync_status_cr();
  RGWCoroutine *run_sync_cr();

  void wakeup();
};

class RGWBucketSyncStatusManager {
  RGWRados *store;

  RGWCoroutinesManager cr_mgr;

  RGWHTTPManager http_manager;

  string source_zone;
  RGWRESTConn *conn;
  RGWSyncErrorLogger *error_logger;
  RGWSyncModuleInstanceRef sync_module;

  rgw_bucket bucket;

  map<int, RGWRemoteBucketLog *> source_logs;

  string source_status_oid;
  string source_shard_status_oid_prefix;

  map<int, rgw_bucket_shard_sync_info> sync_status;
  rgw_raw_obj status_obj;

  int num_shards;

public:
  RGWBucketSyncStatusManager(RGWRados *_store, const string& _source_zone,
                             const rgw_bucket& bucket) : store(_store),
                                                                                     cr_mgr(_store->ctx(), _store->get_cr_registry()),
                                                                                     http_manager(store->ctx(), cr_mgr.get_completion_mgr()),
                                                                                     source_zone(_source_zone),
                                                                                     conn(NULL), error_logger(NULL),
                                                                                     bucket(bucket),
                                                                                     num_shards(0) {}
  ~RGWBucketSyncStatusManager();

  int init();

  map<int, rgw_bucket_shard_sync_info>& get_sync_status() { return sync_status; }
  int init_sync_status();

  static string status_oid(const string& source_zone, const rgw_bucket_shard& bs);

  int read_sync_status();
  int run();
};

/// read the sync status of all bucket shards from the given source zone
int rgw_bucket_sync_status(RGWRados *store, const std::string& source_zone,
                           const rgw_bucket& bucket,
                           std::vector<rgw_bucket_shard_sync_info> *status);

class RGWDefaultSyncModule : public RGWSyncModule {
public:
  RGWDefaultSyncModule() {}
  bool supports_data_export() override { return true; }
  int create_instance(CephContext *cct, map<string, string, ltstr_nocase>& config, RGWSyncModuleInstanceRef *instance) override;
};

// DataLogTrimCR factory function
extern RGWCoroutine* create_data_log_trim_cr(RGWRados *store,
                                             RGWHTTPManager *http,
                                             int num_shards, utime_t interval);

#endif
