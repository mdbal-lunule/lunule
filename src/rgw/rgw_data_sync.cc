#include <boost/utility/string_ref.hpp>

#include "common/ceph_json.h"
#include "common/RWLock.h"
#include "common/RefCountedObj.h"
#include "common/WorkQueue.h"
#include "common/Throttle.h"
#include "common/errno.h"

#include "rgw_common.h"
#include "rgw_rados.h"
#include "rgw_sync.h"
#include "rgw_data_sync.h"
#include "rgw_rest_conn.h"
#include "rgw_cr_rados.h"
#include "rgw_cr_rest.h"
#include "rgw_http_client.h"
#include "rgw_bucket.h"
#include "rgw_metadata.h"
#include "rgw_sync_module.h"
#include "rgw_sync_log_trim.h"

#include "cls/lock/cls_lock_client.h"

#include "auth/Crypto.h"

#include <boost/asio/yield.hpp>

#define dout_subsys ceph_subsys_rgw

#undef dout_prefix
#define dout_prefix (*_dout << "data sync: ")

static string datalog_sync_status_oid_prefix = "datalog.sync-status";
static string datalog_sync_status_shard_prefix = "datalog.sync-status.shard";
static string datalog_sync_full_sync_index_prefix = "data.full-sync.index";
static string bucket_status_oid_prefix = "bucket.sync-status";

class RGWSyncDebugLogger {
  CephContext *cct;
  string prefix;

  bool ended;

public:
  RGWSyncDebugLogger(CephContext *_cct, const string& source_zone,
                     const string& sync_type, const string& sync_stage,
                     const string& resource, bool log_start = true) {
    init(_cct, source_zone, sync_type, sync_stage, resource, log_start);
  }
  RGWSyncDebugLogger() : cct(NULL), ended(false) {}
  ~RGWSyncDebugLogger();

  void init(CephContext *_cct, const string& source_zone,
            const string& sync_type, const string& sync_stage,
            const string& resource, bool log_start = true);
  void log(const string& state);
  void finish(int status);
};

void RGWSyncDebugLogger::init(CephContext *_cct, const string& source_zone,
                              const string& sync_type, const string& sync_section,
                              const string& resource, bool log_start)
{
  cct = _cct;
  ended = false;
  string zone_str = source_zone.substr(0, 8);
  prefix = "Sync:" + zone_str + ":" + sync_type + ":" + sync_section + ":" + resource;
  if (log_start) {
    log("start");
  }
}

RGWSyncDebugLogger::~RGWSyncDebugLogger()
{
  if (!ended) {
    log("finish");
  }
}

void RGWSyncDebugLogger::log(const string& state)
{
  ldout(cct, 5) << prefix << ":" << state << dendl;
}

void RGWSyncDebugLogger::finish(int status)
{
  ended = true;
  ldout(cct, 5) << prefix << ":" << "finish r=" << status << dendl;
}

class RGWDataSyncDebugLogger : public RGWSyncDebugLogger {
public:
  RGWDataSyncDebugLogger() {}
  RGWDataSyncDebugLogger(RGWDataSyncEnv *sync_env, const string& sync_section,
                         const string& resource, bool log_start = true) {
    init(sync_env, sync_section, resource, log_start);
  }
  void init(RGWDataSyncEnv *sync_env, const string& sync_section,
            const string& resource, bool log_start = true) {
    RGWSyncDebugLogger::init(sync_env->cct, sync_env->source_zone, "data", sync_section, resource, log_start);
  }

};

void rgw_datalog_info::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("num_objects", num_shards, obj);
}

void rgw_datalog_entry::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("key", key, obj);
  utime_t ut;
  JSONDecoder::decode_json("timestamp", ut, obj);
  timestamp = ut.to_real_time();
}

void rgw_datalog_shard_data::decode_json(JSONObj *obj) {
  JSONDecoder::decode_json("marker", marker, obj);
  JSONDecoder::decode_json("truncated", truncated, obj);
  JSONDecoder::decode_json("entries", entries, obj);
};

class RGWReadDataSyncStatusMarkersCR : public RGWShardCollectCR {
  static constexpr int MAX_CONCURRENT_SHARDS = 16;

  RGWDataSyncEnv *env;
  const int num_shards;
  int shard_id{0};;

  map<uint32_t, rgw_data_sync_marker>& markers;

 public:
  RGWReadDataSyncStatusMarkersCR(RGWDataSyncEnv *env, int num_shards,
                                 map<uint32_t, rgw_data_sync_marker>& markers)
    : RGWShardCollectCR(env->cct, MAX_CONCURRENT_SHARDS),
      env(env), num_shards(num_shards), markers(markers)
  {}
  bool spawn_next() override;
};

bool RGWReadDataSyncStatusMarkersCR::spawn_next()
{
  if (shard_id >= num_shards) {
    return false;
  }
  using CR = RGWSimpleRadosReadCR<rgw_data_sync_marker>;
  spawn(new CR(env->async_rados, env->store,
               rgw_raw_obj(env->store->get_zone_params().log_pool, RGWDataSyncStatusManager::shard_obj_name(env->source_zone, shard_id)),
               &markers[shard_id]),
        false);
  shard_id++;
  return true;
}

class RGWReadDataSyncStatusCoroutine : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  rgw_data_sync_status *sync_status;

public:
  RGWReadDataSyncStatusCoroutine(RGWDataSyncEnv *_sync_env,
                                 rgw_data_sync_status *_status)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), sync_status(_status)
  {}
  int operate() override;
};

int RGWReadDataSyncStatusCoroutine::operate()
{
  reenter(this) {
    // read sync info
    using ReadInfoCR = RGWSimpleRadosReadCR<rgw_data_sync_info>;
    yield {
      bool empty_on_enoent = false; // fail on ENOENT
      call(new ReadInfoCR(sync_env->async_rados, sync_env->store,
                          rgw_raw_obj(sync_env->store->get_zone_params().log_pool, RGWDataSyncStatusManager::sync_status_oid(sync_env->source_zone)),
                          &sync_status->sync_info, empty_on_enoent));
    }
    if (retcode < 0) {
      ldout(sync_env->cct, 4) << "failed to read sync status info with "
          << cpp_strerror(retcode) << dendl;
      return set_cr_error(retcode);
    }
    // read shard markers
    using ReadMarkersCR = RGWReadDataSyncStatusMarkersCR;
    yield call(new ReadMarkersCR(sync_env, sync_status->sync_info.num_shards,
                                 sync_status->sync_markers));
    if (retcode < 0) {
      ldout(sync_env->cct, 4) << "failed to read sync status markers with "
          << cpp_strerror(retcode) << dendl;
      return set_cr_error(retcode);
    }
    return set_cr_done();
  }
  return 0;
}

class RGWReadRemoteDataLogShardInfoCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  RGWRESTReadResource *http_op;

  int shard_id;
  RGWDataChangesLogInfo *shard_info;

public:
  RGWReadRemoteDataLogShardInfoCR(RGWDataSyncEnv *_sync_env,
                                                      int _shard_id, RGWDataChangesLogInfo *_shard_info) : RGWCoroutine(_sync_env->cct),
                                                      sync_env(_sync_env),
                                                      http_op(NULL),
                                                      shard_id(_shard_id),
                                                      shard_info(_shard_info) {
  }

  ~RGWReadRemoteDataLogShardInfoCR() override {
    if (http_op) {
      http_op->put();
    }
  }

  int operate() override {
    reenter(this) {
      yield {
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", shard_id);
        rgw_http_param_pair pairs[] = { { "type" , "data" },
	                                { "id", buf },
					{ "info" , NULL },
	                                { NULL, NULL } };

        string p = "/admin/log/";

        http_op = new RGWRESTReadResource(sync_env->conn, p, pairs, NULL, sync_env->http_manager);

        http_op->set_user_info((void *)stack);

        int ret = http_op->aio_read();
        if (ret < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to read from " << p << dendl;
          log_error() << "failed to send http operation: " << http_op->to_str() << " ret=" << ret << std::endl;
          return set_cr_error(ret);
        }

        return io_block(0);
      }
      yield {
        int ret = http_op->wait(shard_info);
        if (ret < 0) {
          return set_cr_error(ret);
        }
        return set_cr_done();
      }
    }
    return 0;
  }
};

struct read_remote_data_log_response {
  string marker;
  bool truncated;
  list<rgw_data_change_log_entry> entries;

  read_remote_data_log_response() : truncated(false) {}

  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("marker", marker, obj);
    JSONDecoder::decode_json("truncated", truncated, obj);
    JSONDecoder::decode_json("entries", entries, obj);
  };
};

class RGWReadRemoteDataLogShardCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  RGWRESTReadResource *http_op;

  int shard_id;
  string *pmarker;
  list<rgw_data_change_log_entry> *entries;
  bool *truncated;

  read_remote_data_log_response response;

public:
  RGWReadRemoteDataLogShardCR(RGWDataSyncEnv *_sync_env,
                              int _shard_id, string *_pmarker, list<rgw_data_change_log_entry> *_entries, bool *_truncated) : RGWCoroutine(_sync_env->cct),
                                                      sync_env(_sync_env),
                                                      http_op(NULL),
                                                      shard_id(_shard_id),
                                                      pmarker(_pmarker),
                                                      entries(_entries),
                                                      truncated(_truncated) {
  }
  ~RGWReadRemoteDataLogShardCR() override {
    if (http_op) {
      http_op->put();
    }
  }

  int operate() override {
    reenter(this) {
      yield {
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", shard_id);
        rgw_http_param_pair pairs[] = { { "type" , "data" },
	                                { "id", buf },
	                                { "marker", pmarker->c_str() },
	                                { "extra-info", "true" },
	                                { NULL, NULL } };

        string p = "/admin/log/";

        http_op = new RGWRESTReadResource(sync_env->conn, p, pairs, NULL, sync_env->http_manager);

        http_op->set_user_info((void *)stack);

        int ret = http_op->aio_read();
        if (ret < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to read from " << p << dendl;
          log_error() << "failed to send http operation: " << http_op->to_str() << " ret=" << ret << std::endl;
          return set_cr_error(ret);
        }

        return io_block(0);
      }
      yield {
        int ret = http_op->wait(&response);
        if (ret < 0) {
          return set_cr_error(ret);
        }
        entries->clear();
        entries->swap(response.entries);
        *pmarker = response.marker;
        *truncated = response.truncated;
        return set_cr_done();
      }
    }
    return 0;
  }
};

class RGWReadRemoteDataLogInfoCR : public RGWShardCollectCR {
  RGWDataSyncEnv *sync_env;

  int num_shards;
  map<int, RGWDataChangesLogInfo> *datalog_info;

  int shard_id;
#define READ_DATALOG_MAX_CONCURRENT 10

public:
  RGWReadRemoteDataLogInfoCR(RGWDataSyncEnv *_sync_env,
                     int _num_shards,
                     map<int, RGWDataChangesLogInfo> *_datalog_info) : RGWShardCollectCR(_sync_env->cct, READ_DATALOG_MAX_CONCURRENT),
                                                                 sync_env(_sync_env), num_shards(_num_shards),
                                                                 datalog_info(_datalog_info), shard_id(0) {}
  bool spawn_next() override;
};

bool RGWReadRemoteDataLogInfoCR::spawn_next() {
  if (shard_id >= num_shards) {
    return false;
  }
  spawn(new RGWReadRemoteDataLogShardInfoCR(sync_env, shard_id, &(*datalog_info)[shard_id]), false);
  shard_id++;
  return true;
}

class RGWListRemoteDataLogShardCR : public RGWSimpleCoroutine {
  RGWDataSyncEnv *sync_env;
  RGWRESTReadResource *http_op;

  int shard_id;
  string marker;
  uint32_t max_entries;
  rgw_datalog_shard_data *result;

public:
  RGWListRemoteDataLogShardCR(RGWDataSyncEnv *env, int _shard_id,
                              const string& _marker, uint32_t _max_entries,
                              rgw_datalog_shard_data *_result)
    : RGWSimpleCoroutine(env->store->ctx()), sync_env(env), http_op(NULL),
      shard_id(_shard_id), marker(_marker), max_entries(_max_entries), result(_result) {}

  int send_request() override {
    RGWRESTConn *conn = sync_env->conn;
    RGWRados *store = sync_env->store;

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", shard_id);

    char max_entries_buf[32];
    snprintf(max_entries_buf, sizeof(max_entries_buf), "%d", (int)max_entries);

    const char *marker_key = (marker.empty() ? "" : "marker");

    rgw_http_param_pair pairs[] = { { "type", "data" },
      { "id", buf },
      { "max-entries", max_entries_buf },
      { marker_key, marker.c_str() },
      { NULL, NULL } };

    string p = "/admin/log/";

    http_op = new RGWRESTReadResource(conn, p, pairs, NULL, sync_env->http_manager);
    http_op->set_user_info((void *)stack);

    int ret = http_op->aio_read();
    if (ret < 0) {
      ldout(store->ctx(), 0) << "ERROR: failed to read from " << p << dendl;
      log_error() << "failed to send http operation: " << http_op->to_str() << " ret=" << ret << std::endl;
      http_op->put();
      return ret;
    }

    return 0;
  }

  int request_complete() override {
    int ret = http_op->wait(result);
    http_op->put();
    if (ret < 0 && ret != -ENOENT) {
      ldout(sync_env->store->ctx(), 0) << "ERROR: failed to list remote datalog shard, ret=" << ret << dendl;
      return ret;
    }
    return 0;
  }
};

class RGWListRemoteDataLogCR : public RGWShardCollectCR {
  RGWDataSyncEnv *sync_env;

  map<int, string> shards;
  int max_entries_per_shard;
  map<int, rgw_datalog_shard_data> *result;

  map<int, string>::iterator iter;
#define READ_DATALOG_MAX_CONCURRENT 10

public:
  RGWListRemoteDataLogCR(RGWDataSyncEnv *_sync_env,
                     map<int, string>& _shards,
                     int _max_entries_per_shard,
                     map<int, rgw_datalog_shard_data> *_result) : RGWShardCollectCR(_sync_env->cct, READ_DATALOG_MAX_CONCURRENT),
                                                                 sync_env(_sync_env), max_entries_per_shard(_max_entries_per_shard),
                                                                 result(_result) {
    shards.swap(_shards);
    iter = shards.begin();
  }
  bool spawn_next() override;
};

bool RGWListRemoteDataLogCR::spawn_next() {
  if (iter == shards.end()) {
    return false;
  }

  spawn(new RGWListRemoteDataLogShardCR(sync_env, iter->first, iter->second, max_entries_per_shard, &(*result)[iter->first]), false);
  ++iter;
  return true;
}

class RGWInitDataSyncStatusCoroutine : public RGWCoroutine {
  static constexpr uint32_t lock_duration = 30;
  RGWDataSyncEnv *sync_env;
  RGWRados *store;
  const rgw_pool& pool;
  const uint32_t num_shards;

  string sync_status_oid;

  string lock_name;
  string cookie;
  rgw_data_sync_status *status;
  map<int, RGWDataChangesLogInfo> shards_info;
public:
  RGWInitDataSyncStatusCoroutine(RGWDataSyncEnv *_sync_env, uint32_t num_shards,
                                 uint64_t instance_id,
                                 rgw_data_sync_status *status)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), store(sync_env->store),
      pool(store->get_zone_params().log_pool),
      num_shards(num_shards), status(status) {
    lock_name = "sync_lock";

    status->sync_info.instance_id = instance_id;

#define COOKIE_LEN 16
    char buf[COOKIE_LEN + 1];

    gen_rand_alphanumeric(cct, buf, sizeof(buf) - 1);
    cookie = buf;

    sync_status_oid = RGWDataSyncStatusManager::sync_status_oid(sync_env->source_zone);
  }

  int operate() override {
    int ret;
    reenter(this) {
      using LockCR = RGWSimpleRadosLockCR;
      yield call(new LockCR(sync_env->async_rados, store,
                            rgw_raw_obj{pool, sync_status_oid},
                            lock_name, cookie, lock_duration));
      if (retcode < 0) {
        ldout(cct, 0) << "ERROR: failed to take a lock on " << sync_status_oid << dendl;
        return set_cr_error(retcode);
      }
      using WriteInfoCR = RGWSimpleRadosWriteCR<rgw_data_sync_info>;
      yield call(new WriteInfoCR(sync_env->async_rados, store,
                                 rgw_raw_obj{pool, sync_status_oid},
                                 status->sync_info));
      if (retcode < 0) {
        ldout(cct, 0) << "ERROR: failed to write sync status info with " << retcode << dendl;
        return set_cr_error(retcode);
      }

      /* take lock again, we just recreated the object */
      yield call(new LockCR(sync_env->async_rados, store,
                            rgw_raw_obj{pool, sync_status_oid},
                            lock_name, cookie, lock_duration));
      if (retcode < 0) {
        ldout(cct, 0) << "ERROR: failed to take a lock on " << sync_status_oid << dendl;
        return set_cr_error(retcode);
      }

      /* fetch current position in logs */
      yield {
        RGWRESTConn *conn = store->get_zone_conn_by_id(sync_env->source_zone);
        if (!conn) {
          ldout(cct, 0) << "ERROR: connection to zone " << sync_env->source_zone << " does not exist!" << dendl;
          return set_cr_error(-EIO);
        }
        for (uint32_t i = 0; i < num_shards; i++) {
          spawn(new RGWReadRemoteDataLogShardInfoCR(sync_env, i, &shards_info[i]), true);
        }
      }
      while (collect(&ret, NULL)) {
        if (ret < 0) {
          ldout(cct, 0) << "ERROR: failed to read remote data log shards" << dendl;
          return set_state(RGWCoroutine_Error);
        }
        yield;
      }
      yield {
        for (uint32_t i = 0; i < num_shards; i++) {
          RGWDataChangesLogInfo& info = shards_info[i];
          auto& marker = status->sync_markers[i];
          marker.next_step_marker = info.marker;
          marker.timestamp = info.last_update;
          const auto& oid = RGWDataSyncStatusManager::shard_obj_name(sync_env->source_zone, i);
          using WriteMarkerCR = RGWSimpleRadosWriteCR<rgw_data_sync_marker>;
          spawn(new WriteMarkerCR(sync_env->async_rados, store,
                                  rgw_raw_obj{pool, oid}, marker), true);
        }
      }
      while (collect(&ret, NULL)) {
        if (ret < 0) {
          ldout(cct, 0) << "ERROR: failed to write data sync status markers" << dendl;
          return set_state(RGWCoroutine_Error);
        }
        yield;
      }

      status->sync_info.state = rgw_data_sync_info::StateBuildingFullSyncMaps;
      yield call(new WriteInfoCR(sync_env->async_rados, store,
                                 rgw_raw_obj{pool, sync_status_oid},
                                 status->sync_info));
      if (retcode < 0) {
        ldout(cct, 0) << "ERROR: failed to write sync status info with " << retcode << dendl;
        return set_cr_error(retcode);
      }
      yield call(new RGWSimpleRadosUnlockCR(sync_env->async_rados, store,
                                            rgw_raw_obj{pool, sync_status_oid},
                                            lock_name, cookie));
      return set_cr_done();
    }
    return 0;
  }
};

int RGWRemoteDataLog::read_log_info(rgw_datalog_info *log_info)
{
  rgw_http_param_pair pairs[] = { { "type", "data" },
                                  { NULL, NULL } };

  int ret = sync_env.conn->get_json_resource("/admin/log", pairs, *log_info);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch datalog info" << dendl;
    return ret;
  }

  ldout(store->ctx(), 20) << "remote datalog, num_shards=" << log_info->num_shards << dendl;

  return 0;
}

int RGWRemoteDataLog::read_source_log_shards_info(map<int, RGWDataChangesLogInfo> *shards_info)
{
  rgw_datalog_info log_info;
  int ret = read_log_info(&log_info);
  if (ret < 0) {
    return ret;
  }

  return run(new RGWReadRemoteDataLogInfoCR(&sync_env, log_info.num_shards, shards_info));
}

int RGWRemoteDataLog::read_source_log_shards_next(map<int, string> shard_markers, map<int, rgw_datalog_shard_data> *result)
{
  if (store->is_meta_master()) {
    return 0;
  }

  return run(new RGWListRemoteDataLogCR(&sync_env, shard_markers, 1, result));
}

int RGWRemoteDataLog::init(const string& _source_zone, RGWRESTConn *_conn, RGWSyncErrorLogger *_error_logger, RGWSyncModuleInstanceRef& _sync_module)
{
  sync_env.init(store->ctx(), store, _conn, async_rados, &http_manager, _error_logger,
                _source_zone, _sync_module, observer);

  if (initialized) {
    return 0;
  }

  int ret = http_manager.set_threaded();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "failed in http_manager.set_threaded() ret=" << ret << dendl;
    return ret;
  }

  initialized = true;

  return 0;
}

void RGWRemoteDataLog::finish()
{
  stop();
}

int RGWRemoteDataLog::read_sync_status(rgw_data_sync_status *sync_status)
{
  // cannot run concurrently with run_sync(), so run in a separate manager
  RGWCoroutinesManager crs(store->ctx(), store->get_cr_registry());
  RGWHTTPManager http_manager(store->ctx(), crs.get_completion_mgr());
  int ret = http_manager.set_threaded();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "failed in http_manager.set_threaded() ret=" << ret << dendl;
    return ret;
  }
  RGWDataSyncEnv sync_env_local = sync_env;
  sync_env_local.http_manager = &http_manager;
  ret = crs.run(new RGWReadDataSyncStatusCoroutine(&sync_env_local, sync_status));
  http_manager.stop();
  return ret;
}

int RGWRemoteDataLog::init_sync_status(int num_shards)
{
  rgw_data_sync_status sync_status;
  sync_status.sync_info.num_shards = num_shards;

  RGWCoroutinesManager crs(store->ctx(), store->get_cr_registry());
  RGWHTTPManager http_manager(store->ctx(), crs.get_completion_mgr());
  int ret = http_manager.set_threaded();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "failed in http_manager.set_threaded() ret=" << ret << dendl;
    return ret;
  }
  RGWDataSyncEnv sync_env_local = sync_env;
  sync_env_local.http_manager = &http_manager;
  uint64_t instance_id;
  get_random_bytes((char *)&instance_id, sizeof(instance_id));
  ret = crs.run(new RGWInitDataSyncStatusCoroutine(&sync_env_local, num_shards, instance_id, &sync_status));
  http_manager.stop();
  return ret;
}

static string full_data_sync_index_shard_oid(const string& source_zone, int shard_id)
{
  char buf[datalog_sync_full_sync_index_prefix.size() + 1 + source_zone.size() + 1 + 16];
  snprintf(buf, sizeof(buf), "%s.%s.%d", datalog_sync_full_sync_index_prefix.c_str(), source_zone.c_str(), shard_id);
  return string(buf);
}

struct bucket_instance_meta_info {
  string key;
  obj_version ver;
  utime_t mtime;
  RGWBucketInstanceMetadataObject data;

  bucket_instance_meta_info() {}

  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("key", key, obj);
    JSONDecoder::decode_json("ver", ver, obj);
    JSONDecoder::decode_json("mtime", mtime, obj);
    JSONDecoder::decode_json("data", data, obj);
  }
};

class RGWListBucketIndexesCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  RGWRados *store;

  rgw_data_sync_status *sync_status;
  int num_shards;

  int req_ret;
  int ret;

  list<string> result;
  list<string>::iterator iter;

  RGWShardedOmapCRManager *entries_index;

  string oid_prefix;

  string path;
  bucket_instance_meta_info meta_info;
  string key;
  string s;
  int i;

  bool failed;

public:
  RGWListBucketIndexesCR(RGWDataSyncEnv *_sync_env,
                         rgw_data_sync_status *_sync_status) : RGWCoroutine(_sync_env->cct), sync_env(_sync_env),
                                                      store(sync_env->store), sync_status(_sync_status),
						      req_ret(0), ret(0), entries_index(NULL), i(0), failed(false) {
    oid_prefix = datalog_sync_full_sync_index_prefix + "." + sync_env->source_zone; 
    path = "/admin/metadata/bucket.instance";
    num_shards = sync_status->sync_info.num_shards;
  }
  ~RGWListBucketIndexesCR() override {
    delete entries_index;
  }

  int operate() override {
    reenter(this) {
      yield {
        string entrypoint = string("/admin/metadata/bucket.instance");
        /* FIXME: need a better scaling solution here, requires streaming output */
        call(new RGWReadRESTResourceCR<list<string> >(store->ctx(), sync_env->conn, sync_env->http_manager,
                                                      entrypoint, NULL, &result));
      }
      if (retcode < 0) {
        ldout(sync_env->cct, 0) << "ERROR: failed to fetch metadata for section bucket.index" << dendl;
        return set_cr_error(retcode);
      }
      entries_index = new RGWShardedOmapCRManager(sync_env->async_rados, store, this, num_shards,
						  store->get_zone_params().log_pool,
                                                  oid_prefix);
      yield; // yield so OmapAppendCRs can start
      for (iter = result.begin(); iter != result.end(); ++iter) {
        ldout(sync_env->cct, 20) << "list metadata: section=bucket.index key=" << *iter << dendl;

        key = *iter;

        yield {
          rgw_http_param_pair pairs[] = { { "key", key.c_str() },
                                          { NULL, NULL } };

          call(new RGWReadRESTResourceCR<bucket_instance_meta_info>(store->ctx(), sync_env->conn, sync_env->http_manager, path, pairs, &meta_info));
        }

        num_shards = meta_info.data.get_bucket_info().num_shards;
        if (num_shards > 0) {
          for (i = 0; i < num_shards; i++) {
            char buf[16];
            snprintf(buf, sizeof(buf), ":%d", i);
            s = key + buf;
            yield entries_index->append(s, store->data_log->get_log_shard_id(meta_info.data.get_bucket_info().bucket, i));
          }
        } else {
          yield entries_index->append(key, store->data_log->get_log_shard_id(meta_info.data.get_bucket_info().bucket, -1));
        }
      }
      yield {
        if (!entries_index->finish()) {
          failed = true;
        }
      }
      if (!failed) {
        for (map<uint32_t, rgw_data_sync_marker>::iterator iter = sync_status->sync_markers.begin(); iter != sync_status->sync_markers.end(); ++iter) {
          int shard_id = (int)iter->first;
          rgw_data_sync_marker& marker = iter->second;
          marker.total_entries = entries_index->get_total_entries(shard_id);
          spawn(new RGWSimpleRadosWriteCR<rgw_data_sync_marker>(sync_env->async_rados, store,
                                                                rgw_raw_obj(store->get_zone_params().log_pool, RGWDataSyncStatusManager::shard_obj_name(sync_env->source_zone, shard_id)),
                                                                marker), true);
        }
      } else {
          yield call(sync_env->error_logger->log_error_cr(sync_env->conn->get_remote_id(), "data.init", "",
                                                          EIO, string("failed to build bucket instances map")));
      }
      while (collect(&ret, NULL)) {
	if (ret < 0) {
          yield call(sync_env->error_logger->log_error_cr(sync_env->conn->get_remote_id(), "data.init", "",
                                                          -ret, string("failed to store sync status: ") + cpp_strerror(-ret)));
	  req_ret = ret;
	}
        yield;
      }
      drain_all();
      if (req_ret < 0) {
        yield return set_cr_error(req_ret);
      }
      yield return set_cr_done();
    }
    return 0;
  }
};

#define DATA_SYNC_UPDATE_MARKER_WINDOW 1

class RGWDataSyncShardMarkerTrack : public RGWSyncShardMarkerTrack<string, string> {
  RGWDataSyncEnv *sync_env;

  string marker_oid;
  rgw_data_sync_marker sync_marker;

  map<string, string> key_to_marker;
  map<string, string> marker_to_key;

  void handle_finish(const string& marker) override {
    map<string, string>::iterator iter = marker_to_key.find(marker);
    if (iter == marker_to_key.end()) {
      return;
    }
    key_to_marker.erase(iter->second);
    reset_need_retry(iter->second);
    marker_to_key.erase(iter);
  }

public:
  RGWDataSyncShardMarkerTrack(RGWDataSyncEnv *_sync_env,
                         const string& _marker_oid,
                         const rgw_data_sync_marker& _marker) : RGWSyncShardMarkerTrack(DATA_SYNC_UPDATE_MARKER_WINDOW),
                                                                sync_env(_sync_env),
                                                                marker_oid(_marker_oid),
                                                                sync_marker(_marker) {}

  RGWCoroutine *store_marker(const string& new_marker, uint64_t index_pos, const real_time& timestamp) override {
    sync_marker.marker = new_marker;
    sync_marker.pos = index_pos;

    ldout(sync_env->cct, 20) << __func__ << "(): updating marker marker_oid=" << marker_oid << " marker=" << new_marker << dendl;
    RGWRados *store = sync_env->store;

    return new RGWSimpleRadosWriteCR<rgw_data_sync_marker>(sync_env->async_rados, store,
                                                           rgw_raw_obj(store->get_zone_params().log_pool, marker_oid),
                                                           sync_marker);
  }

  /*
   * create index from key -> marker, and from marker -> key
   * this is useful so that we can insure that we only have one
   * entry for any key that is used. This is needed when doing
   * incremenatl sync of data, and we don't want to run multiple
   * concurrent sync operations for the same bucket shard 
   */
  bool index_key_to_marker(const string& key, const string& marker) {
    if (key_to_marker.find(key) != key_to_marker.end()) {
      set_need_retry(key);
      return false;
    }
    key_to_marker[key] = marker;
    marker_to_key[marker] = key;
    return true;
  }
};

// ostream wrappers to print buckets without copying strings
struct bucket_str {
  const rgw_bucket& b;
  bucket_str(const rgw_bucket& b) : b(b) {}
};
std::ostream& operator<<(std::ostream& out, const bucket_str& rhs) {
  auto& b = rhs.b;
  if (!b.tenant.empty()) {
    out << b.tenant << '/';
  }
  out << b.name;
  if (!b.bucket_id.empty()) {
    out << ':' << b.bucket_id;
  }
  return out;
}

struct bucket_shard_str {
  const rgw_bucket_shard& bs;
  bucket_shard_str(const rgw_bucket_shard& bs) : bs(bs) {}
};
std::ostream& operator<<(std::ostream& out, const bucket_shard_str& rhs) {
  auto& bs = rhs.bs;
  out << bucket_str{bs.bucket};
  if (bs.shard_id >= 0) {
    out << ':' << bs.shard_id;
  }
  return out;
}

class RGWRunBucketSyncCoroutine : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  rgw_bucket_shard bs;
  RGWBucketInfo bucket_info;
  rgw_bucket_shard_sync_info sync_status;
  RGWMetaSyncEnv meta_sync_env;

  RGWDataSyncDebugLogger logger;
  const std::string status_oid;

  boost::intrusive_ptr<RGWContinuousLeaseCR> lease_cr;
  boost::intrusive_ptr<RGWCoroutinesStack> lease_stack;

public:
  RGWRunBucketSyncCoroutine(RGWDataSyncEnv *_sync_env, const rgw_bucket_shard& bs)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), bs(bs),
      status_oid(RGWBucketSyncStatusManager::status_oid(sync_env->source_zone, bs)) {
    logger.init(sync_env, "Bucket", bs.get_key());
  }
  ~RGWRunBucketSyncCoroutine() override {
    if (lease_cr) {
      lease_cr->abort();
    }
  }

  int operate() override;
};

class RGWDataSyncSingleEntryCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  string raw_key;
  string entry_marker;

  rgw_bucket_shard bs;

  int sync_status;

  bufferlist md_bl;

  RGWDataSyncShardMarkerTrack *marker_tracker;

  boost::intrusive_ptr<RGWOmapAppend> error_repo;
  bool remove_from_repo;

  set<string> keys;

public:
  RGWDataSyncSingleEntryCR(RGWDataSyncEnv *_sync_env,
		           const string& _raw_key, const string& _entry_marker, RGWDataSyncShardMarkerTrack *_marker_tracker,
                           RGWOmapAppend *_error_repo, bool _remove_from_repo) : RGWCoroutine(_sync_env->cct),
                                                      sync_env(_sync_env),
						      raw_key(_raw_key), entry_marker(_entry_marker),
                                                      sync_status(0),
                                                      marker_tracker(_marker_tracker),
                                                      error_repo(_error_repo), remove_from_repo(_remove_from_repo) {
    set_description() << "data sync single entry (source_zone=" << sync_env->source_zone << ") key=" <<_raw_key << " entry=" << entry_marker;
  }

  int operate() override {
    reenter(this) {
      do {
        yield {
          int ret = rgw_bucket_parse_bucket_key(sync_env->cct, raw_key,
                                                &bs.bucket, &bs.shard_id);
          if (ret < 0) {
            return set_cr_error(-EIO);
          }
          if (marker_tracker) {
            marker_tracker->reset_need_retry(raw_key);
          }
          call(new RGWRunBucketSyncCoroutine(sync_env, bs));
        }
      } while (marker_tracker && marker_tracker->need_retry(raw_key));

      sync_status = retcode;

      if (sync_status == -ENOENT) {
        // this was added when 'tenant/' was added to datalog entries, because
        // preexisting tenant buckets could never sync and would stay in the
        // error_repo forever
        ldout(sync_env->store->ctx(), 0) << "WARNING: skipping data log entry "
            "for missing bucket " << raw_key << dendl;
        sync_status = 0;
      }

      if (sync_status < 0) {
        // write actual sync failures for 'radosgw-admin sync error list'
        if (sync_status != -EBUSY && sync_status != -EAGAIN) {
          yield call(sync_env->error_logger->log_error_cr(sync_env->conn->get_remote_id(), "data", raw_key,
                                                          -sync_status, string("failed to sync bucket instance: ") + cpp_strerror(-sync_status)));
          if (retcode < 0) {
            ldout(sync_env->store->ctx(), 0) << "ERROR: failed to log sync failure: retcode=" << retcode << dendl;
          }
        }
        if (error_repo && !error_repo->append(raw_key)) {
          ldout(sync_env->store->ctx(), 0) << "ERROR: failed to log sync failure in error repo: retcode=" << retcode << dendl;
        }
      } else if (error_repo && remove_from_repo) {
        keys = {raw_key};
        yield call(new RGWRadosRemoveOmapKeysCR(sync_env->store, error_repo->get_obj(), keys));
        if (retcode < 0) {
          ldout(sync_env->store->ctx(), 0) << "ERROR: failed to remove omap key from error repo ("
             << error_repo->get_obj() << " retcode=" << retcode << dendl;
        }
      }
      if (sync_env->observer) {
        sync_env->observer->on_bucket_changed(bs.bucket.get_key());
      }
      /* FIXME: what do do in case of error */
      if (marker_tracker && !entry_marker.empty()) {
        /* update marker */
        yield call(marker_tracker->finish(entry_marker));
      }
      if (sync_status == 0) {
        sync_status = retcode;
      }
      if (sync_status < 0) {
        return set_cr_error(sync_status);
      }
      return set_cr_done();
    }
    return 0;
  }
};

#define BUCKET_SHARD_SYNC_SPAWN_WINDOW 20
#define DATA_SYNC_MAX_ERR_ENTRIES 10

enum RemoteDatalogStatus {
  RemoteNotTrimmed = 0,
  RemoteTrimmed = 1,
  RemoteMightTrimmed = 2
};

class RGWDataSyncShardCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  rgw_pool pool;

  uint32_t shard_id;
  rgw_data_sync_marker sync_marker;

  map<string, bufferlist> entries;
  map<string, bufferlist>::iterator iter;

  string oid;

  RGWDataSyncShardMarkerTrack *marker_tracker;

  list<rgw_data_change_log_entry> log_entries;
  list<rgw_data_change_log_entry>::iterator log_iter;
  bool truncated;

  RGWDataChangesLogInfo shard_info;
  string datalog_marker;

  RemoteDatalogStatus remote_trimmed;
  Mutex inc_lock;
  Cond inc_cond;

  boost::asio::coroutine incremental_cr;
  boost::asio::coroutine full_cr;


  set<string> modified_shards;
  set<string> current_modified;

  set<string>::iterator modified_iter;

  int total_entries;

  int spawn_window;

  bool *reset_backoff;

  set<string> spawned_keys;

  boost::intrusive_ptr<RGWContinuousLeaseCR> lease_cr;
  boost::intrusive_ptr<RGWCoroutinesStack> lease_stack;
  string status_oid;


  string error_oid;
  RGWOmapAppend *error_repo;
  map<string, bufferlist> error_entries;
  string error_marker;
  int max_error_entries;

  ceph::real_time error_retry_time;

#define RETRY_BACKOFF_SECS_MIN 60
#define RETRY_BACKOFF_SECS_DEFAULT 60
#define RETRY_BACKOFF_SECS_MAX 600
  uint32_t retry_backoff_secs;

  RGWDataSyncDebugLogger logger;
public:
  RGWDataSyncShardCR(RGWDataSyncEnv *_sync_env,
                     rgw_pool& _pool,
		     uint32_t _shard_id, rgw_data_sync_marker& _marker, bool *_reset_backoff) : RGWCoroutine(_sync_env->cct),
                                                      sync_env(_sync_env),
						      pool(_pool),
						      shard_id(_shard_id),
						      sync_marker(_marker),
                                                      marker_tracker(NULL), truncated(false), remote_trimmed(RemoteNotTrimmed), inc_lock("RGWDataSyncShardCR::inc_lock"),
                                                      total_entries(0), spawn_window(BUCKET_SHARD_SYNC_SPAWN_WINDOW), reset_backoff(NULL),
                                                      lease_cr(nullptr), lease_stack(nullptr), error_repo(nullptr), max_error_entries(DATA_SYNC_MAX_ERR_ENTRIES),
                                                      retry_backoff_secs(RETRY_BACKOFF_SECS_DEFAULT) {
    set_description() << "data sync shard source_zone=" << sync_env->source_zone << " shard_id=" << shard_id;
    status_oid = RGWDataSyncStatusManager::shard_obj_name(sync_env->source_zone, shard_id);
    error_oid = status_oid + ".retry";

    logger.init(sync_env, "DataShard", status_oid);
  }

  ~RGWDataSyncShardCR() override {
    delete marker_tracker;
    if (lease_cr) {
      lease_cr->abort();
    }
    if (error_repo) {
      error_repo->put();
    }
  }

  void append_modified_shards(set<string>& keys) {
    Mutex::Locker l(inc_lock);
    modified_shards.insert(keys.begin(), keys.end());
  }

  void set_marker_tracker(RGWDataSyncShardMarkerTrack *mt) {
    delete marker_tracker;
    marker_tracker = mt;
  }

  int operate() override {
    int r;
    while (true) {
      switch (sync_marker.state) {
      case rgw_data_sync_marker::FullSync:
        r = full_sync();
        if (r < 0) {
          ldout(cct, 10) << "sync: full_sync: shard_id=" << shard_id << " r=" << r << dendl;
          return set_cr_error(r);
        }
        return 0;
      case rgw_data_sync_marker::IncrementalSync:
        r  = incremental_sync();
        if (r < 0) {
          ldout(cct, 10) << "sync: incremental_sync: shard_id=" << shard_id << " r=" << r << dendl;
          return set_cr_error(r);
        }
        return 0;
      default:
        return set_cr_error(-EIO);
      }
    }
    return 0;
  }

  void init_lease_cr() {
    set_status("acquiring sync lock");
    uint32_t lock_duration = cct->_conf->rgw_sync_lease_period;
    string lock_name = "sync_lock";
    if (lease_cr) {
      lease_cr->abort();
    }
    RGWRados *store = sync_env->store;
    lease_cr.reset(new RGWContinuousLeaseCR(sync_env->async_rados, store,
                                            rgw_raw_obj(store->get_zone_params().log_pool, status_oid),
                                            lock_name, lock_duration, this));
    lease_stack.reset(spawn(lease_cr.get(), false));
  }

  int full_sync() {
#define OMAP_GET_MAX_ENTRIES 100
    int max_entries = OMAP_GET_MAX_ENTRIES;
    reenter(&full_cr) {
      yield init_lease_cr();
      while (!lease_cr->is_locked()) {
        if (lease_cr->is_done()) {
          ldout(cct, 5) << "lease cr failed, done early " << dendl;
          set_status("lease lock failed, early abort");
          return set_cr_error(lease_cr->get_ret_status());
        }
        set_sleeping(true);
        yield;
      }
      logger.log("full sync");
      oid = full_data_sync_index_shard_oid(sync_env->source_zone, shard_id);
      set_marker_tracker(new RGWDataSyncShardMarkerTrack(sync_env, status_oid, sync_marker));
      total_entries = sync_marker.pos;
      do {
        yield call(new RGWRadosGetOmapKeysCR(sync_env->store, rgw_raw_obj(pool, oid), sync_marker.marker, &entries, max_entries));
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: " << __func__ << "(): RGWRadosGetOmapKeysCR() returned ret=" << retcode << dendl;
          lease_cr->go_down();
          drain_all();
          return set_cr_error(retcode);
        }
        iter = entries.begin();
        for (; iter != entries.end(); ++iter) {
          ldout(sync_env->cct, 20) << __func__ << ": full sync: " << iter->first << dendl;
          total_entries++;
          if (!marker_tracker->start(iter->first, total_entries, real_time())) {
            ldout(sync_env->cct, 0) << "ERROR: cannot start syncing " << iter->first << ". Duplicate entry?" << dendl;
          } else {
            // fetch remote and write locally
            yield spawn(new RGWDataSyncSingleEntryCR(sync_env, iter->first, iter->first, marker_tracker, error_repo, false), false);
            if (retcode < 0) {
              lease_cr->go_down();
              drain_all();
              return set_cr_error(retcode);
            }
          }
          sync_marker.marker = iter->first;
        }
      } while ((int)entries.size() == max_entries);

      lease_cr->go_down();
      drain_all();

      yield {
        /* update marker to reflect we're done with full sync */
        sync_marker.state = rgw_data_sync_marker::IncrementalSync;
        sync_marker.marker = sync_marker.next_step_marker;
        sync_marker.next_step_marker.clear();
        RGWRados *store = sync_env->store;
        call(new RGWSimpleRadosWriteCR<rgw_data_sync_marker>(sync_env->async_rados, store,
                                                             rgw_raw_obj(store->get_zone_params().log_pool, status_oid),
                                                             sync_marker));
      }
      if (retcode < 0) {
        ldout(sync_env->cct, 0) << "ERROR: failed to set sync marker: retcode=" << retcode << dendl;
        lease_cr->go_down();
        return set_cr_error(retcode);
      }
    }
    return 0;
  }

  int incremental_sync() {
    reenter(&incremental_cr) {
      yield init_lease_cr();
      while (!lease_cr->is_locked()) {
        if (lease_cr->is_done()) {
          ldout(cct, 5) << "lease cr failed, done early " << dendl;
          set_status("lease lock failed, early abort");
          return set_cr_error(lease_cr->get_ret_status());
        }
        set_sleeping(true);
        yield;
      }
      set_status("lease acquired");
      error_repo = new RGWOmapAppend(sync_env->async_rados, sync_env->store,
                                     rgw_raw_obj(pool, error_oid),
                                     1 /* no buffer */);
      error_repo->get();
      spawn(error_repo, false);
      logger.log("inc sync");
      set_marker_tracker(new RGWDataSyncShardMarkerTrack(sync_env, status_oid, sync_marker));
      do {
        current_modified.clear();
        inc_lock.Lock();
        current_modified.swap(modified_shards);
        inc_lock.Unlock();

        /* process out of band updates */
        for (modified_iter = current_modified.begin(); modified_iter != current_modified.end(); ++modified_iter) {
          yield {
            ldout(sync_env->cct, 20) << __func__ << "(): async update notification: " << *modified_iter << dendl;
            spawn(new RGWDataSyncSingleEntryCR(sync_env, *modified_iter, string(), marker_tracker, error_repo, false), false);
          }
        }

        /* process bucket shards that previously failed */
        yield call(new RGWRadosGetOmapKeysCR(sync_env->store, rgw_raw_obj(pool, error_oid),
                                             error_marker, &error_entries,
                                             max_error_entries));
        ldout(sync_env->cct, 20) << __func__ << "(): read error repo, got " << error_entries.size() << " entries" << dendl;
        iter = error_entries.begin();
        for (; iter != error_entries.end(); ++iter) {
          ldout(sync_env->cct, 20) << __func__ << "(): handle error entry: " << iter->first << dendl;
          spawn(new RGWDataSyncSingleEntryCR(sync_env, iter->first, iter->first, nullptr /* no marker tracker */, error_repo, true), false);
          error_marker = iter->first;
        }
        if ((int)error_entries.size() != max_error_entries) {
          if (error_marker.empty() && error_entries.empty()) {
            /* the retry repo is empty, we back off a bit before calling it again */
            retry_backoff_secs *= 2;
            if (retry_backoff_secs > RETRY_BACKOFF_SECS_MAX) {
              retry_backoff_secs = RETRY_BACKOFF_SECS_MAX;
            }
          } else {
            retry_backoff_secs = RETRY_BACKOFF_SECS_DEFAULT;
          }
          error_retry_time = ceph::real_clock::now() + make_timespan(retry_backoff_secs);
          error_marker.clear();
        }


        yield call(new RGWReadRemoteDataLogShardInfoCR(sync_env, shard_id, &shard_info));
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to fetch remote data log info: ret=" << retcode << dendl;
          stop_spawned_services();
          drain_all();
          return set_cr_error(retcode);
        }
        datalog_marker = shard_info.marker;
        remote_trimmed = RemoteNotTrimmed;
#define INCREMENTAL_MAX_ENTRIES 100
	ldout(sync_env->cct, 20) << __func__ << ":" << __LINE__ << ": shard_id=" << shard_id << " datalog_marker=" << datalog_marker << " sync_marker.marker=" << sync_marker.marker << dendl;
	if (datalog_marker > sync_marker.marker) {
          spawned_keys.clear();
          if (sync_marker.marker.empty())
            remote_trimmed = RemoteMightTrimmed; //remote data log shard might be trimmed;
          yield call(new RGWReadRemoteDataLogShardCR(sync_env, shard_id, &sync_marker.marker, &log_entries, &truncated));
          if (retcode < 0) {
            ldout(sync_env->cct, 0) << "ERROR: failed to read remote data log info: ret=" << retcode << dendl;
            stop_spawned_services();
            drain_all();
            return set_cr_error(retcode);
          }
          if ((remote_trimmed == RemoteMightTrimmed) && sync_marker.marker.empty() && log_entries.empty())
            remote_trimmed = RemoteTrimmed;
          else
            remote_trimmed = RemoteNotTrimmed;
          for (log_iter = log_entries.begin(); log_iter != log_entries.end(); ++log_iter) {
            ldout(sync_env->cct, 20) << __func__ << ":" << __LINE__ << ": shard_id=" << shard_id << " log_entry: " << log_iter->log_id << ":" << log_iter->log_timestamp << ":" << log_iter->entry.key << dendl;
            if (!marker_tracker->index_key_to_marker(log_iter->entry.key, log_iter->log_id)) {
              ldout(sync_env->cct, 20) << __func__ << ": skipping sync of entry: " << log_iter->log_id << ":" << log_iter->entry.key << " sync already in progress for bucket shard" << dendl;
              marker_tracker->try_update_high_marker(log_iter->log_id, 0, log_iter->log_timestamp);
              continue;
            }
            if (!marker_tracker->start(log_iter->log_id, 0, log_iter->log_timestamp)) {
              ldout(sync_env->cct, 0) << "ERROR: cannot start syncing " << log_iter->log_id << ". Duplicate entry?" << dendl;
            } else {
              /*
               * don't spawn the same key more than once. We can do that as long as we don't yield
               */
              if (spawned_keys.find(log_iter->entry.key) == spawned_keys.end()) {
                spawned_keys.insert(log_iter->entry.key);
                spawn(new RGWDataSyncSingleEntryCR(sync_env, log_iter->entry.key, log_iter->log_id, marker_tracker, error_repo, false), false);
                if (retcode < 0) {
                  stop_spawned_services();
                  drain_all();
                  return set_cr_error(retcode);
                }
              }
            }
	  }
          while ((int)num_spawned() > spawn_window) {
            set_status() << "num_spawned() > spawn_window";
            yield wait_for_child();
            int ret;
            while (collect(&ret, lease_stack.get())) {
              if (ret < 0) {
                ldout(sync_env->cct, 0) << "ERROR: a sync operation returned error" << dendl;
                /* we have reported this error */
              }
              /* not waiting for child here */
            }
          }
	}
	ldout(sync_env->cct, 20) << __func__ << ":" << __LINE__ << ": shard_id=" << shard_id << " datalog_marker=" << datalog_marker << " sync_marker.marker=" << sync_marker.marker << dendl;
	if (datalog_marker == sync_marker.marker || remote_trimmed == RemoteTrimmed) {
#define INCREMENTAL_INTERVAL 20
	  yield wait(utime_t(INCREMENTAL_INTERVAL, 0));
	}
      } while (true);
    }
    return 0;
  }
  void stop_spawned_services() {
    lease_cr->go_down();
    if (error_repo) {
      error_repo->finish();
      error_repo->put();
      error_repo = NULL;
    }
  }
};

class RGWDataSyncShardControlCR : public RGWBackoffControlCR {
  RGWDataSyncEnv *sync_env;

  rgw_pool pool;

  uint32_t shard_id;
  rgw_data_sync_marker sync_marker;

public:
  RGWDataSyncShardControlCR(RGWDataSyncEnv *_sync_env, rgw_pool& _pool,
		     uint32_t _shard_id, rgw_data_sync_marker& _marker) : RGWBackoffControlCR(_sync_env->cct, false),
                                                      sync_env(_sync_env),
						      pool(_pool),
						      shard_id(_shard_id),
						      sync_marker(_marker) {
  }

  RGWCoroutine *alloc_cr() override {
    return new RGWDataSyncShardCR(sync_env, pool, shard_id, sync_marker, backoff_ptr());
  }

  RGWCoroutine *alloc_finisher_cr() override {
    RGWRados *store = sync_env->store;
    return new RGWSimpleRadosReadCR<rgw_data_sync_marker>(sync_env->async_rados, store,
                                                          rgw_raw_obj(store->get_zone_params().log_pool, RGWDataSyncStatusManager::shard_obj_name(sync_env->source_zone, shard_id)),
                                                          &sync_marker);
  }

  void append_modified_shards(set<string>& keys) {
    Mutex::Locker l(cr_lock());

    RGWDataSyncShardCR *cr = static_cast<RGWDataSyncShardCR *>(get_cr());
    if (!cr) {
      return;
    }

    cr->append_modified_shards(keys);
  }
};

class RGWDataSyncCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  uint32_t num_shards;

  rgw_data_sync_status sync_status;

  RGWDataSyncShardMarkerTrack *marker_tracker;

  Mutex shard_crs_lock;
  map<int, RGWDataSyncShardControlCR *> shard_crs;

  bool *reset_backoff;

  RGWDataSyncDebugLogger logger;

  RGWDataSyncModule *data_sync_module{nullptr};
public:
  RGWDataSyncCR(RGWDataSyncEnv *_sync_env, uint32_t _num_shards, bool *_reset_backoff) : RGWCoroutine(_sync_env->cct),
                                                      sync_env(_sync_env),
                                                      num_shards(_num_shards),
                                                      marker_tracker(NULL),
                                                      shard_crs_lock("RGWDataSyncCR::shard_crs_lock"),
                                                      reset_backoff(_reset_backoff), logger(sync_env, "Data", "all") {

  }

  ~RGWDataSyncCR() override {
    for (auto iter : shard_crs) {
      iter.second->put();
    }
  }

  int operate() override {
    reenter(this) {

      /* read sync status */
      yield call(new RGWReadDataSyncStatusCoroutine(sync_env, &sync_status));

      data_sync_module = sync_env->sync_module->get_data_handler();

      if (retcode < 0 && retcode != -ENOENT) {
        ldout(sync_env->cct, 0) << "ERROR: failed to fetch sync status, retcode=" << retcode << dendl;
        return set_cr_error(retcode);
      }

      /* state: init status */
      if ((rgw_data_sync_info::SyncState)sync_status.sync_info.state == rgw_data_sync_info::StateInit) {
        ldout(sync_env->cct, 20) << __func__ << "(): init" << dendl;
        sync_status.sync_info.num_shards = num_shards;
        uint64_t instance_id;
        get_random_bytes((char *)&instance_id, sizeof(instance_id));
        yield call(new RGWInitDataSyncStatusCoroutine(sync_env, num_shards, instance_id, &sync_status));
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to init sync, retcode=" << retcode << dendl;
          return set_cr_error(retcode);
        }
        // sets state = StateBuildingFullSyncMaps

        *reset_backoff = true;
      }

      data_sync_module->init(sync_env, sync_status.sync_info.instance_id);

      if  ((rgw_data_sync_info::SyncState)sync_status.sync_info.state == rgw_data_sync_info::StateBuildingFullSyncMaps) {
        /* call sync module init here */
        sync_status.sync_info.num_shards = num_shards;
        yield call(data_sync_module->init_sync(sync_env));
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: sync module init_sync() failed, retcode=" << retcode << dendl;
          return set_cr_error(retcode);
        }
        /* state: building full sync maps */
        ldout(sync_env->cct, 20) << __func__ << "(): building full sync maps" << dendl;
        yield call(new RGWListBucketIndexesCR(sync_env, &sync_status));
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to build full sync maps, retcode=" << retcode << dendl;
          return set_cr_error(retcode);
        }
        sync_status.sync_info.state = rgw_data_sync_info::StateSync;

        /* update new state */
        yield call(set_sync_info_cr());
        if (retcode < 0) {
          ldout(sync_env->cct, 0) << "ERROR: failed to write sync status, retcode=" << retcode << dendl;
          return set_cr_error(retcode);
        }

        *reset_backoff = true;
      }

      yield {
        if  ((rgw_data_sync_info::SyncState)sync_status.sync_info.state == rgw_data_sync_info::StateSync) {
          for (map<uint32_t, rgw_data_sync_marker>::iterator iter = sync_status.sync_markers.begin();
               iter != sync_status.sync_markers.end(); ++iter) {
            RGWDataSyncShardControlCR *cr = new RGWDataSyncShardControlCR(sync_env, sync_env->store->get_zone_params().log_pool,
                                                                          iter->first, iter->second);
            cr->get();
            shard_crs_lock.Lock();
            shard_crs[iter->first] = cr;
            shard_crs_lock.Unlock();
            spawn(cr, true);
          }
        }
      }

      return set_cr_done();
    }
    return 0;
  }

  RGWCoroutine *set_sync_info_cr() {
    RGWRados *store = sync_env->store;
    return new RGWSimpleRadosWriteCR<rgw_data_sync_info>(sync_env->async_rados, store,
                                                         rgw_raw_obj(store->get_zone_params().log_pool, RGWDataSyncStatusManager::sync_status_oid(sync_env->source_zone)),
                                                         sync_status.sync_info);
  }

  void wakeup(int shard_id, set<string>& keys) {
    Mutex::Locker l(shard_crs_lock);
    map<int, RGWDataSyncShardControlCR *>::iterator iter = shard_crs.find(shard_id);
    if (iter == shard_crs.end()) {
      return;
    }
    iter->second->append_modified_shards(keys);
    iter->second->wakeup();
  }
};

class RGWDefaultDataSyncModule : public RGWDataSyncModule {
public:
  RGWDefaultDataSyncModule() {}

  RGWCoroutine *sync_object(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key, uint64_t versioned_epoch, rgw_zone_set *zones_trace) override;
  RGWCoroutine *remove_object(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key, real_time& mtime, bool versioned, uint64_t versioned_epoch, rgw_zone_set *zones_trace) override;
  RGWCoroutine *create_delete_marker(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key, real_time& mtime,
                                     rgw_bucket_entry_owner& owner, bool versioned, uint64_t versioned_epoch, rgw_zone_set *zones_trace) override;
};

class RGWDefaultSyncModuleInstance : public RGWSyncModuleInstance {
  RGWDefaultDataSyncModule data_handler;
public:
  RGWDefaultSyncModuleInstance() {}
  RGWDataSyncModule *get_data_handler() override {
    return &data_handler;
  }
};

int RGWDefaultSyncModule::create_instance(CephContext *cct, map<string, string, ltstr_nocase>& config, RGWSyncModuleInstanceRef *instance)
{
  instance->reset(new RGWDefaultSyncModuleInstance());
  return 0;
}

RGWCoroutine *RGWDefaultDataSyncModule::sync_object(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key, uint64_t versioned_epoch, rgw_zone_set *zones_trace)
{
  return new RGWFetchRemoteObjCR(sync_env->async_rados, sync_env->store, sync_env->source_zone, bucket_info,
                                 key, versioned_epoch,
                                 true, zones_trace);
}

RGWCoroutine *RGWDefaultDataSyncModule::remove_object(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key,
                                                      real_time& mtime, bool versioned, uint64_t versioned_epoch, rgw_zone_set *zones_trace)
{
  return new RGWRemoveObjCR(sync_env->async_rados, sync_env->store, sync_env->source_zone,
                            bucket_info, key, versioned, versioned_epoch,
                            NULL, NULL, false, &mtime, zones_trace);
}

RGWCoroutine *RGWDefaultDataSyncModule::create_delete_marker(RGWDataSyncEnv *sync_env, RGWBucketInfo& bucket_info, rgw_obj_key& key, real_time& mtime,
                                                             rgw_bucket_entry_owner& owner, bool versioned, uint64_t versioned_epoch, rgw_zone_set *zones_trace)
{
  return new RGWRemoveObjCR(sync_env->async_rados, sync_env->store, sync_env->source_zone,
                            bucket_info, key, versioned, versioned_epoch,
                            &owner.id, &owner.display_name, true, &mtime, zones_trace);
}

class RGWDataSyncControlCR : public RGWBackoffControlCR
{
  RGWDataSyncEnv *sync_env;
  uint32_t num_shards;

  static constexpr bool exit_on_error = false; // retry on all errors
public:
  RGWDataSyncControlCR(RGWDataSyncEnv *_sync_env, uint32_t _num_shards) : RGWBackoffControlCR(_sync_env->cct, exit_on_error),
                                                      sync_env(_sync_env), num_shards(_num_shards) {
  }

  RGWCoroutine *alloc_cr() override {
    return new RGWDataSyncCR(sync_env, num_shards, backoff_ptr());
  }

  void wakeup(int shard_id, set<string>& keys) {
    Mutex& m = cr_lock();

    m.Lock();
    RGWDataSyncCR *cr = static_cast<RGWDataSyncCR *>(get_cr());
    if (!cr) {
      m.Unlock();
      return;
    }

    cr->get();
    m.Unlock();

    if (cr) {
      cr->wakeup(shard_id, keys);
    }

    cr->put();
  }
};

void RGWRemoteDataLog::wakeup(int shard_id, set<string>& keys) {
  RWLock::RLocker rl(lock);
  if (!data_sync_cr) {
    return;
  }
  data_sync_cr->wakeup(shard_id, keys);
}

int RGWRemoteDataLog::run_sync(int num_shards)
{
  lock.get_write();
  data_sync_cr = new RGWDataSyncControlCR(&sync_env, num_shards);
  data_sync_cr->get(); // run() will drop a ref, so take another
  lock.unlock();

  int r = run(data_sync_cr);

  lock.get_write();
  data_sync_cr->put();
  data_sync_cr = NULL;
  lock.unlock();

  if (r < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to run sync" << dendl;
    return r;
  }
  return 0;
}

int RGWDataSyncStatusManager::init()
{
  auto zone_def_iter = store->zone_by_id.find(source_zone);
  if (zone_def_iter == store->zone_by_id.end()) {
    ldout(store->ctx(), 0) << "ERROR: failed to find zone config info for zone=" << source_zone << dendl;
    return -EIO;
  }

  auto& zone_def = zone_def_iter->second;

  if (!store->get_sync_modules_manager()->supports_data_export(zone_def.tier_type)) {
    return -ENOTSUP;
  }

  RGWZoneParams& zone_params = store->get_zone_params();

  if (sync_module == nullptr) { 
    sync_module = store->get_sync_module();
  }

  conn = store->get_zone_conn_by_id(source_zone);
  if (!conn) {
    ldout(store->ctx(), 0) << "connection object to zone " << source_zone << " does not exist" << dendl;
    return -EINVAL;
  }

  error_logger = new RGWSyncErrorLogger(store, RGW_SYNC_ERROR_LOG_SHARD_PREFIX, ERROR_LOGGER_SHARDS);

  int r = source_log.init(source_zone, conn, error_logger, sync_module);
  if (r < 0) {
    lderr(store->ctx()) << "ERROR: failed to init remote log, r=" << r << dendl;
    finalize();
    return r;
  }

  rgw_datalog_info datalog_info;
  r = source_log.read_log_info(&datalog_info);
  if (r < 0) {
    ldout(store->ctx(), 5) << "ERROR: master.read_log_info() returned r=" << r << dendl;
    finalize();
    return r;
  }

  num_shards = datalog_info.num_shards;

  for (int i = 0; i < num_shards; i++) {
    shard_objs[i] = rgw_raw_obj(zone_params.log_pool, shard_obj_name(source_zone, i));
  }

  return 0;
}

void RGWDataSyncStatusManager::finalize()
{
  delete error_logger;
  error_logger = nullptr;
}

string RGWDataSyncStatusManager::sync_status_oid(const string& source_zone)
{
  char buf[datalog_sync_status_oid_prefix.size() + source_zone.size() + 16];
  snprintf(buf, sizeof(buf), "%s.%s", datalog_sync_status_oid_prefix.c_str(), source_zone.c_str());

  return string(buf);
}

string RGWDataSyncStatusManager::shard_obj_name(const string& source_zone, int shard_id)
{
  char buf[datalog_sync_status_shard_prefix.size() + source_zone.size() + 16];
  snprintf(buf, sizeof(buf), "%s.%s.%d", datalog_sync_status_shard_prefix.c_str(), source_zone.c_str(), shard_id);

  return string(buf);
}

int RGWRemoteBucketLog::init(const string& _source_zone, RGWRESTConn *_conn,
                             const rgw_bucket& bucket, int shard_id,
                             RGWSyncErrorLogger *_error_logger,
                             RGWSyncModuleInstanceRef& _sync_module)
{
  conn = _conn;
  source_zone = _source_zone;
  bs.bucket = bucket;
  bs.shard_id = shard_id;

  sync_env.init(store->ctx(), store, conn, async_rados, http_manager,
                _error_logger, source_zone, _sync_module, nullptr);

  return 0;
}

struct bucket_index_marker_info {
  string bucket_ver;
  string master_ver;
  string max_marker;
  bool syncstopped{false};

  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("bucket_ver", bucket_ver, obj);
    JSONDecoder::decode_json("master_ver", master_ver, obj);
    JSONDecoder::decode_json("max_marker", max_marker, obj);
    JSONDecoder::decode_json("syncstopped", syncstopped, obj);
  }
};

class RGWReadRemoteBucketIndexLogInfoCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  const string instance_key;

  bucket_index_marker_info *info;

public:
  RGWReadRemoteBucketIndexLogInfoCR(RGWDataSyncEnv *_sync_env,
                                  const rgw_bucket_shard& bs,
                                  bucket_index_marker_info *_info)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env),
      instance_key(bs.get_key()), info(_info) {}

  int operate() override {
    reenter(this) {
      yield {
        rgw_http_param_pair pairs[] = { { "type" , "bucket-index" },
	                                { "bucket-instance", instance_key.c_str() },
					{ "info" , NULL },
	                                { NULL, NULL } };

        string p = "/admin/log/";
        call(new RGWReadRESTResourceCR<bucket_index_marker_info>(sync_env->cct, sync_env->conn, sync_env->http_manager, p, pairs, info));
      }
      if (retcode < 0) {
        return set_cr_error(retcode);
      }
      return set_cr_done();
    }
    return 0;
  }
};

class RGWInitBucketShardSyncStatusCoroutine : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  rgw_bucket_shard bs;
  const string sync_status_oid;

  rgw_bucket_shard_sync_info& status;

  bucket_index_marker_info info;
public:
  RGWInitBucketShardSyncStatusCoroutine(RGWDataSyncEnv *_sync_env,
                                        const rgw_bucket_shard& bs,
                                        rgw_bucket_shard_sync_info& _status)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), bs(bs),
      sync_status_oid(RGWBucketSyncStatusManager::status_oid(sync_env->source_zone, bs)),
      status(_status)
  {}

  int operate() override {
    reenter(this) {
      /* fetch current position in logs */
      yield call(new RGWReadRemoteBucketIndexLogInfoCR(sync_env, bs, &info));
      if (retcode < 0 && retcode != -ENOENT) {
        ldout(cct, 0) << "ERROR: failed to fetch bucket index status" << dendl;
        return set_cr_error(retcode);
      }
      yield {
        auto store = sync_env->store;
        rgw_raw_obj obj(store->get_zone_params().log_pool, sync_status_oid);

        if (info.syncstopped) {
          call(new RGWRadosRemoveCR(store, obj));
        } else {
          status.state = rgw_bucket_shard_sync_info::StateFullSync;
          status.inc_marker.position = info.max_marker;
          map<string, bufferlist> attrs;
          status.encode_all_attrs(attrs);
          call(new RGWSimpleRadosWriteAttrsCR(sync_env->async_rados, store, obj, attrs));
        }
      }
      return set_cr_done();
    }
    return 0;
  }
};

RGWCoroutine *RGWRemoteBucketLog::init_sync_status_cr()
{
  return new RGWInitBucketShardSyncStatusCoroutine(&sync_env, bs, init_status);
}

template <class T>
static void decode_attr(CephContext *cct, map<string, bufferlist>& attrs, const string& attr_name, T *val)
{
  map<string, bufferlist>::iterator iter = attrs.find(attr_name);
  if (iter == attrs.end()) {
    *val = T();
    return;
  }

  bufferlist::iterator biter = iter->second.begin();
  try {
    ::decode(*val, biter);
  } catch (buffer::error& err) {
    ldout(cct, 0) << "ERROR: failed to decode attribute: " << attr_name << dendl;
  }
}

void rgw_bucket_shard_sync_info::decode_from_attrs(CephContext *cct, map<string, bufferlist>& attrs)
{
  decode_attr(cct, attrs, "state", &state);
  decode_attr(cct, attrs, "full_marker", &full_marker);
  decode_attr(cct, attrs, "inc_marker", &inc_marker);
}

void rgw_bucket_shard_sync_info::encode_all_attrs(map<string, bufferlist>& attrs)
{
  encode_state_attr(attrs);
  full_marker.encode_attr(attrs);
  inc_marker.encode_attr(attrs);
}

void rgw_bucket_shard_sync_info::encode_state_attr(map<string, bufferlist>& attrs)
{
  ::encode(state, attrs["state"]);
}

void rgw_bucket_shard_full_sync_marker::encode_attr(map<string, bufferlist>& attrs)
{
  ::encode(*this, attrs["full_marker"]);
}

void rgw_bucket_shard_inc_sync_marker::encode_attr(map<string, bufferlist>& attrs)
{
  ::encode(*this, attrs["inc_marker"]);
}

class RGWReadBucketSyncStatusCoroutine : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  string oid;
  rgw_bucket_shard_sync_info *status;

  map<string, bufferlist> attrs;
public:
  RGWReadBucketSyncStatusCoroutine(RGWDataSyncEnv *_sync_env,
                                   const rgw_bucket_shard& bs,
                                   rgw_bucket_shard_sync_info *_status)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env),
      oid(RGWBucketSyncStatusManager::status_oid(sync_env->source_zone, bs)),
      status(_status) {}
  int operate() override;
};

int RGWReadBucketSyncStatusCoroutine::operate()
{
  reenter(this) {
    yield call(new RGWSimpleRadosReadAttrsCR(sync_env->async_rados, sync_env->store,
                                                   rgw_raw_obj(sync_env->store->get_zone_params().log_pool, oid),
                                                   &attrs));
    if (retcode == -ENOENT) {
      *status = rgw_bucket_shard_sync_info();
      return set_cr_done();
    }
    if (retcode < 0) {
      ldout(sync_env->cct, 0) << "ERROR: failed to call fetch bucket shard info oid=" << oid << " ret=" << retcode << dendl;
      return set_cr_error(retcode);
    }
    status->decode_from_attrs(sync_env->cct, attrs);
    return set_cr_done();
  }
  return 0;
}
RGWCoroutine *RGWRemoteBucketLog::read_sync_status_cr(rgw_bucket_shard_sync_info *sync_status)
{
  return new RGWReadBucketSyncStatusCoroutine(&sync_env, bs, sync_status);
}

RGWBucketSyncStatusManager::~RGWBucketSyncStatusManager() {
  for (map<int, RGWRemoteBucketLog *>::iterator iter = source_logs.begin(); iter != source_logs.end(); ++iter) {
    delete iter->second;
  }
  delete error_logger;
}


void rgw_bucket_entry_owner::decode_json(JSONObj *obj)
{
  JSONDecoder::decode_json("ID", id, obj);
  JSONDecoder::decode_json("DisplayName", display_name, obj);
}

struct bucket_list_entry {
  bool delete_marker;
  rgw_obj_key key;
  bool is_latest;
  real_time mtime;
  string etag;
  uint64_t size;
  string storage_class;
  rgw_bucket_entry_owner owner;
  uint64_t versioned_epoch;
  string rgw_tag;

  bucket_list_entry() : delete_marker(false), is_latest(false), size(0), versioned_epoch(0) {}

  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("IsDeleteMarker", delete_marker, obj);
    JSONDecoder::decode_json("Key", key.name, obj);
    JSONDecoder::decode_json("VersionId", key.instance, obj);
    JSONDecoder::decode_json("IsLatest", is_latest, obj);
    string mtime_str;
    JSONDecoder::decode_json("RgwxMtime", mtime_str, obj);

    struct tm t;
    uint32_t nsec;
    if (parse_iso8601(mtime_str.c_str(), &t, &nsec)) {
      ceph_timespec ts;
      ts.tv_sec = (uint64_t)internal_timegm(&t);
      ts.tv_nsec = nsec;
      mtime = real_clock::from_ceph_timespec(ts);
    }
    JSONDecoder::decode_json("ETag", etag, obj);
    JSONDecoder::decode_json("Size", size, obj);
    JSONDecoder::decode_json("StorageClass", storage_class, obj);
    JSONDecoder::decode_json("Owner", owner, obj);
    JSONDecoder::decode_json("VersionedEpoch", versioned_epoch, obj);
    JSONDecoder::decode_json("RgwxTag", rgw_tag, obj);
  }
};

struct bucket_list_result {
  string name;
  string prefix;
  string key_marker;
  string version_id_marker;
  int max_keys;
  bool is_truncated;
  list<bucket_list_entry> entries;

  bucket_list_result() : max_keys(0), is_truncated(false) {}

  void decode_json(JSONObj *obj) {
    JSONDecoder::decode_json("Name", name, obj);
    JSONDecoder::decode_json("Prefix", prefix, obj);
    JSONDecoder::decode_json("KeyMarker", key_marker, obj);
    JSONDecoder::decode_json("VersionIdMarker", version_id_marker, obj);
    JSONDecoder::decode_json("MaxKeys", max_keys, obj);
    JSONDecoder::decode_json("IsTruncated", is_truncated, obj);
    JSONDecoder::decode_json("Entries", entries, obj);
  }
};

class RGWListBucketShardCR: public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  const rgw_bucket_shard& bs;
  const string instance_key;
  rgw_obj_key marker_position;

  bucket_list_result *result;

public:
  RGWListBucketShardCR(RGWDataSyncEnv *_sync_env, const rgw_bucket_shard& bs,
                       rgw_obj_key& _marker_position, bucket_list_result *_result)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), bs(bs),
      instance_key(bs.get_key()), marker_position(_marker_position),
      result(_result) {}

  int operate() override {
    reenter(this) {
      yield {
        rgw_http_param_pair pairs[] = { { "rgwx-bucket-instance", instance_key.c_str() },
					{ "versions" , NULL },
					{ "format" , "json" },
					{ "objs-container" , "true" },
					{ "key-marker" , marker_position.name.c_str() },
					{ "version-id-marker" , marker_position.instance.c_str() },
	                                { NULL, NULL } };
        // don't include tenant in the url, it's already part of instance_key
        string p = string("/") + bs.bucket.name;
        call(new RGWReadRESTResourceCR<bucket_list_result>(sync_env->cct, sync_env->conn, sync_env->http_manager, p, pairs, result));
      }
      if (retcode < 0) {
        return set_cr_error(retcode);
      }
      return set_cr_done();
    }
    return 0;
  }
};

class RGWListBucketIndexLogCR: public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  const string instance_key;
  string marker;

  list<rgw_bi_log_entry> *result;

public:
  RGWListBucketIndexLogCR(RGWDataSyncEnv *_sync_env, const rgw_bucket_shard& bs,
                          string& _marker, list<rgw_bi_log_entry> *_result)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env),
      instance_key(bs.get_key()), marker(_marker), result(_result) {}

  int operate() override {
    reenter(this) {
      yield {
        rgw_http_param_pair pairs[] = { { "bucket-instance", instance_key.c_str() },
					{ "format" , "json" },
					{ "marker" , marker.c_str() },
					{ "type", "bucket-index" },
	                                { NULL, NULL } };

        call(new RGWReadRESTResourceCR<list<rgw_bi_log_entry> >(sync_env->cct, sync_env->conn, sync_env->http_manager, "/admin/log", pairs, result));
      }
      if (retcode < 0) {
        return set_cr_error(retcode);
      }
      return set_cr_done();
    }
    return 0;
  }
};

#define BUCKET_SYNC_UPDATE_MARKER_WINDOW 10

class RGWBucketFullSyncShardMarkerTrack : public RGWSyncShardMarkerTrack<rgw_obj_key, rgw_obj_key> {
  RGWDataSyncEnv *sync_env;

  string marker_oid;
  rgw_bucket_shard_full_sync_marker sync_marker;

public:
  RGWBucketFullSyncShardMarkerTrack(RGWDataSyncEnv *_sync_env,
                         const string& _marker_oid,
                         const rgw_bucket_shard_full_sync_marker& _marker) : RGWSyncShardMarkerTrack(BUCKET_SYNC_UPDATE_MARKER_WINDOW),
                                                                sync_env(_sync_env),
                                                                marker_oid(_marker_oid),
                                                                sync_marker(_marker) {}

  RGWCoroutine *store_marker(const rgw_obj_key& new_marker, uint64_t index_pos, const real_time& timestamp) override {
    sync_marker.position = new_marker;
    sync_marker.count = index_pos;

    map<string, bufferlist> attrs;
    sync_marker.encode_attr(attrs);

    RGWRados *store = sync_env->store;

    ldout(sync_env->cct, 20) << __func__ << "(): updating marker marker_oid=" << marker_oid << " marker=" << new_marker << dendl;
    return new RGWSimpleRadosWriteAttrsCR(sync_env->async_rados, store,
                                          rgw_raw_obj(store->get_zone_params().log_pool, marker_oid),
                                          attrs);
  }
};

class RGWBucketIncSyncShardMarkerTrack : public RGWSyncShardMarkerTrack<string, rgw_obj_key> {
  RGWDataSyncEnv *sync_env;

  string marker_oid;
  rgw_bucket_shard_inc_sync_marker sync_marker;

  map<rgw_obj_key, string> key_to_marker;
  map<string, rgw_obj_key> marker_to_key;

  void handle_finish(const string& marker) override {
    map<string, rgw_obj_key>::iterator iter = marker_to_key.find(marker);
    if (iter == marker_to_key.end()) {
      return;
    }
    key_to_marker.erase(iter->second);
    reset_need_retry(iter->second);
    marker_to_key.erase(iter);
  }

public:
  RGWBucketIncSyncShardMarkerTrack(RGWDataSyncEnv *_sync_env,
                         const string& _marker_oid,
                         const rgw_bucket_shard_inc_sync_marker& _marker) : RGWSyncShardMarkerTrack(BUCKET_SYNC_UPDATE_MARKER_WINDOW),
                                                                sync_env(_sync_env),
                                                                marker_oid(_marker_oid),
                                                                sync_marker(_marker) {}

  RGWCoroutine *store_marker(const string& new_marker, uint64_t index_pos, const real_time& timestamp) override {
    sync_marker.position = new_marker;

    map<string, bufferlist> attrs;
    sync_marker.encode_attr(attrs);

    RGWRados *store = sync_env->store;

    ldout(sync_env->cct, 20) << __func__ << "(): updating marker marker_oid=" << marker_oid << " marker=" << new_marker << dendl;
    return new RGWSimpleRadosWriteAttrsCR(sync_env->async_rados,
                                          store,
                                          rgw_raw_obj(store->get_zone_params().log_pool, marker_oid),
                                          attrs);
  }

  /*
   * create index from key -> <op, marker>, and from marker -> key
   * this is useful so that we can insure that we only have one
   * entry for any key that is used. This is needed when doing
   * incremenatl sync of data, and we don't want to run multiple
   * concurrent sync operations for the same bucket shard 
   * Also, we should make sure that we don't run concurrent operations on the same key with
   * different ops.
   */
  bool index_key_to_marker(const rgw_obj_key& key, const string& marker) {
    if (key_to_marker.find(key) != key_to_marker.end()) {
      set_need_retry(key);
      return false;
    }
    key_to_marker[key] = marker;
    marker_to_key[marker] = key;
    return true;
  }

  bool can_do_op(const rgw_obj_key& key) {
    return (key_to_marker.find(key) == key_to_marker.end());
  }
};

template <class T, class K>
class RGWBucketSyncSingleEntryCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;

  RGWBucketInfo *bucket_info;
  const rgw_bucket_shard& bs;

  rgw_obj_key key;
  bool versioned;
  uint64_t versioned_epoch;
  rgw_bucket_entry_owner owner;
  real_time timestamp;
  RGWModifyOp op;
  RGWPendingState op_state;

  T entry_marker;
  RGWSyncShardMarkerTrack<T, K> *marker_tracker;

  int sync_status;

  stringstream error_ss;

  RGWDataSyncDebugLogger logger;

  bool error_injection;

  RGWDataSyncModule *data_sync_module;
  
  rgw_zone_set zones_trace;

public:
  RGWBucketSyncSingleEntryCR(RGWDataSyncEnv *_sync_env,
                             RGWBucketInfo *_bucket_info,
                             const rgw_bucket_shard& bs,
                             const rgw_obj_key& _key, bool _versioned, uint64_t _versioned_epoch,
                             real_time& _timestamp,
                             const rgw_bucket_entry_owner& _owner,
                             RGWModifyOp _op, RGWPendingState _op_state,
		             const T& _entry_marker, RGWSyncShardMarkerTrack<T, K> *_marker_tracker, rgw_zone_set& _zones_trace) : RGWCoroutine(_sync_env->cct),
						      sync_env(_sync_env),
                                                      bucket_info(_bucket_info), bs(bs),
                                                      key(_key), versioned(_versioned), versioned_epoch(_versioned_epoch),
                                                      owner(_owner),
                                                      timestamp(_timestamp), op(_op),
                                                      op_state(_op_state),
                                                      entry_marker(_entry_marker),
                                                      marker_tracker(_marker_tracker),
                                                      sync_status(0){
    stringstream ss;
    ss << bucket_shard_str{bs} << "/" << key << "[" << versioned_epoch << "]";
    set_description() << "bucket sync single entry (source_zone=" << sync_env->source_zone << ") b=" << ss.str() << " log_entry=" << entry_marker << " op=" << (int)op << " op_state=" << (int)op_state;
    ldout(sync_env->cct, 20) << "bucket sync single entry (source_zone=" << sync_env->source_zone << ") b=" << ss.str() << " log_entry=" << entry_marker << " op=" << (int)op << " op_state=" << (int)op_state << dendl;
    set_status("init");

    logger.init(sync_env, "Object", ss.str());

    error_injection = (sync_env->cct->_conf->rgw_sync_data_inject_err_probability > 0);

    data_sync_module = sync_env->sync_module->get_data_handler();
    
    zones_trace = _zones_trace;
    zones_trace.insert(sync_env->store->get_zone().id);
  }

  int operate() override {
    reenter(this) {
      /* skip entries that are not complete */
      if (op_state != CLS_RGW_STATE_COMPLETE) {
        goto done;
      }
      do {
        yield {
          marker_tracker->reset_need_retry(key);
          if (key.name.empty()) {
            /* shouldn't happen */
            set_status("skipping empty entry");
            ldout(sync_env->cct, 0) << "ERROR: " << __func__ << "(): entry with empty obj name, skipping" << dendl;
            goto done;
          }
          if (error_injection &&
              rand() % 10000 < cct->_conf->rgw_sync_data_inject_err_probability * 10000.0) {
            ldout(sync_env->cct, 0) << __func__ << ": injecting data sync error on key=" << key.name << dendl;
            retcode = -EIO;
          } else if (op == CLS_RGW_OP_ADD ||
                     op == CLS_RGW_OP_LINK_OLH) {
            if (op == CLS_RGW_OP_ADD && !key.instance.empty() && key.instance != "null") {
              set_status("skipping entry");
              ldout(sync_env->cct, 10) << "bucket skipping sync obj: " << sync_env->source_zone << "/" << bucket_info->bucket << "/" << key << "[" << versioned_epoch << "]: versioned object will be synced on link_olh" << dendl;
              goto done;

            }
            set_status("syncing obj");
            ldout(sync_env->cct, 5) << "bucket sync: sync obj: " << sync_env->source_zone << "/" << bucket_info->bucket << "/" << key << "[" << versioned_epoch << "]" << dendl;
            logger.log("fetch");
            call(data_sync_module->sync_object(sync_env, *bucket_info, key, versioned_epoch, &zones_trace));
          } else if (op == CLS_RGW_OP_DEL || op == CLS_RGW_OP_UNLINK_INSTANCE) {
            set_status("removing obj");
            if (op == CLS_RGW_OP_UNLINK_INSTANCE) {
              versioned = true;
            }
            logger.log("remove");
            call(data_sync_module->remove_object(sync_env, *bucket_info, key, timestamp, versioned, versioned_epoch, &zones_trace));
          } else if (op == CLS_RGW_OP_LINK_OLH_DM) {
            logger.log("creating delete marker");
            set_status("creating delete marker");
            ldout(sync_env->cct, 10) << "creating delete marker: obj: " << sync_env->source_zone << "/" << bucket_info->bucket << "/" << key << "[" << versioned_epoch << "]" << dendl;
            call(data_sync_module->create_delete_marker(sync_env, *bucket_info, key, timestamp, owner, versioned, versioned_epoch, &zones_trace));
          }
        }
      } while (marker_tracker->need_retry(key));
      {
        stringstream ss;
        if (retcode >= 0) {
          ss << "done";
        } else {
          ss << "done, retcode=" << retcode;
        }
        logger.log(ss.str());
      }

      if (retcode < 0 && retcode != -ENOENT) {
        set_status() << "failed to sync obj; retcode=" << retcode;
        ldout(sync_env->cct, 0) << "ERROR: failed to sync object: "
            << bucket_shard_str{bs} << "/" << key.name << dendl;
        error_ss << bucket_shard_str{bs} << "/" << key.name;
        sync_status = retcode;
      }
      if (!error_ss.str().empty()) {
        yield call(sync_env->error_logger->log_error_cr(sync_env->conn->get_remote_id(), "data", error_ss.str(), -retcode, "failed to sync object"));
      }
done:
      if (sync_status == 0) {
        /* update marker */
        set_status() << "calling marker_tracker->finish(" << entry_marker << ")";
        yield call(marker_tracker->finish(entry_marker));
        sync_status = retcode;
      }
      if (sync_status < 0) {
        return set_cr_error(sync_status);
      }
      return set_cr_done();
    }
    return 0;
  }
};

#define BUCKET_SYNC_SPAWN_WINDOW 20

class RGWBucketShardFullSyncCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  const rgw_bucket_shard& bs;
  RGWBucketInfo *bucket_info;
  boost::intrusive_ptr<RGWContinuousLeaseCR> lease_cr;
  bucket_list_result list_result;
  list<bucket_list_entry>::iterator entries_iter;
  rgw_bucket_shard_full_sync_marker& full_marker;
  RGWBucketFullSyncShardMarkerTrack marker_tracker;
  rgw_obj_key list_marker;
  bucket_list_entry *entry{nullptr};
  RGWModifyOp op{CLS_RGW_OP_ADD};

  int total_entries{0};

  int sync_status{0};

  const string& status_oid;

  RGWDataSyncDebugLogger logger;
  rgw_zone_set zones_trace;
public:
  RGWBucketShardFullSyncCR(RGWDataSyncEnv *_sync_env, const rgw_bucket_shard& bs,
                           RGWBucketInfo *_bucket_info,
                           const std::string& status_oid,
                           RGWContinuousLeaseCR *lease_cr,
                           rgw_bucket_shard_full_sync_marker& _full_marker)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), bs(bs),
      bucket_info(_bucket_info), lease_cr(lease_cr), full_marker(_full_marker),
      marker_tracker(sync_env, status_oid, full_marker),
      status_oid(status_oid) {
    logger.init(sync_env, "BucketFull", bs.get_key());
    zones_trace.insert(sync_env->source_zone);
  }

  int operate() override;
};

int RGWBucketShardFullSyncCR::operate()
{
  int ret;
  reenter(this) {
    list_marker = full_marker.position;

    total_entries = full_marker.count;
    do {
      if (!lease_cr->is_locked()) {
        drain_all();
        return set_cr_error(-ECANCELED);
      }
      set_status("listing remote bucket");
      ldout(sync_env->cct, 20) << __func__ << "(): listing bucket for full sync" << dendl;
      yield call(new RGWListBucketShardCR(sync_env, bs, list_marker,
                                          &list_result));
      if (retcode < 0 && retcode != -ENOENT) {
        set_status("failed bucket listing, going down");
        drain_all();
        return set_cr_error(retcode);
      }
      entries_iter = list_result.entries.begin();
      for (; entries_iter != list_result.entries.end(); ++entries_iter) {
        if (!lease_cr->is_locked()) {
          drain_all();
          return set_cr_error(-ECANCELED);
        }
        ldout(sync_env->cct, 20) << "[full sync] syncing object: "
            << bucket_shard_str{bs} << "/" << entries_iter->key << dendl;
        entry = &(*entries_iter);
        total_entries++;
        list_marker = entries_iter->key;
        if (!marker_tracker.start(entry->key, total_entries, real_time())) {
          ldout(sync_env->cct, 0) << "ERROR: cannot start syncing " << entry->key << ". Duplicate entry?" << dendl;
        } else {
          op = (entry->key.instance.empty() || entry->key.instance == "null" ? CLS_RGW_OP_ADD : CLS_RGW_OP_LINK_OLH);
          using SyncCR = RGWBucketSyncSingleEntryCR<rgw_obj_key, rgw_obj_key>;
          yield spawn(new SyncCR(sync_env, bucket_info, bs, entry->key,
                                 false, /* versioned, only matters for object removal */
                                 entry->versioned_epoch, entry->mtime,
                                 entry->owner, op, CLS_RGW_STATE_COMPLETE,
                                 entry->key, &marker_tracker, zones_trace),
                      false);
        }
        while (num_spawned() > BUCKET_SYNC_SPAWN_WINDOW) {
          yield wait_for_child();
          bool again = true;
          while (again) {
            again = collect(&ret, nullptr);
            if (ret < 0) {
              ldout(sync_env->cct, 0) << "ERROR: a sync operation returned error" << dendl;
              sync_status = ret;
              /* we have reported this error */
            }
          }
        }
      }
    } while (list_result.is_truncated && sync_status == 0);
    set_status("done iterating over all objects");
    /* wait for all operations to complete */
    while (num_spawned()) {
      yield wait_for_child();
      bool again = true;
      while (again) {
        again = collect(&ret, nullptr);
        if (ret < 0) {
          ldout(sync_env->cct, 0) << "ERROR: a sync operation returned error" << dendl;
          sync_status = ret;
          /* we have reported this error */
        }
      }
    }
    if (!lease_cr->is_locked()) {
      return set_cr_error(-ECANCELED);
    }
    /* update sync state to incremental */
    if (sync_status == 0) {
      yield {
        rgw_bucket_shard_sync_info sync_status;
        sync_status.state = rgw_bucket_shard_sync_info::StateIncrementalSync;
        map<string, bufferlist> attrs;
        sync_status.encode_state_attr(attrs);
        RGWRados *store = sync_env->store;
        call(new RGWSimpleRadosWriteAttrsCR(sync_env->async_rados, store,
                                            rgw_raw_obj(store->get_zone_params().log_pool, status_oid),
                                            attrs));
      }
    } else {
      ldout(sync_env->cct, 0) << "ERROR: failure in sync, backing out (sync_status=" << sync_status<< ")" << dendl;
    }
    if (retcode < 0 && sync_status == 0) { /* actually tried to set incremental state and failed */
      ldout(sync_env->cct, 0) << "ERROR: failed to set sync state on bucket "
          << bucket_shard_str{bs} << " retcode=" << retcode << dendl;
      return set_cr_error(retcode);
    }
    if (sync_status < 0) {
      return set_cr_error(sync_status);
    }
    return set_cr_done();
  }
  return 0;
}

class RGWBucketShardIncrementalSyncCR : public RGWCoroutine {
  RGWDataSyncEnv *sync_env;
  const rgw_bucket_shard& bs;
  RGWBucketInfo *bucket_info;
  boost::intrusive_ptr<RGWContinuousLeaseCR> lease_cr;
  list<rgw_bi_log_entry> list_result;
  list<rgw_bi_log_entry>::iterator entries_iter;
  map<pair<string, string>, pair<real_time, RGWModifyOp> > squash_map;
  rgw_bucket_shard_inc_sync_marker& inc_marker;
  rgw_obj_key key;
  rgw_bi_log_entry *entry{nullptr};
  RGWBucketIncSyncShardMarkerTrack marker_tracker;
  bool updated_status{false};
  const string& status_oid;
  const string& zone_id;
  ceph::real_time sync_modify_time;

  string cur_id;

  RGWDataSyncDebugLogger logger;

  int sync_status{0};
  bool syncstopped{false};

public:
  RGWBucketShardIncrementalSyncCR(RGWDataSyncEnv *_sync_env,
                                  const rgw_bucket_shard& bs,
                                  RGWBucketInfo *_bucket_info,
                                  const std::string& status_oid,
                                  RGWContinuousLeaseCR *lease_cr,
                                  rgw_bucket_shard_inc_sync_marker& _inc_marker)
    : RGWCoroutine(_sync_env->cct), sync_env(_sync_env), bs(bs),
      bucket_info(_bucket_info), lease_cr(lease_cr), inc_marker(_inc_marker),
      marker_tracker(sync_env, status_oid, inc_marker), status_oid(status_oid) , zone_id(_sync_env->store->get_zone().id){
    set_description() << "bucket shard incremental sync bucket="
        << bucket_shard_str{bs};
    set_status("init");
    logger.init(sync_env, "BucketInc", bs.get_key());
  }

  int operate() override;
};

int RGWBucketShardIncrementalSyncCR::operate()
{
  int ret;
  reenter(this) {
    do {
      if (!lease_cr->is_locked()) {
        drain_all();
        return set_cr_error(-ECANCELED);
      }
      ldout(sync_env->cct, 20) << __func__ << "(): listing bilog for incremental sync" << inc_marker.position << dendl;
      set_status() << "listing bilog; position=" << inc_marker.position;
      yield call(new RGWListBucketIndexLogCR(sync_env, bs, inc_marker.position,
                                             &list_result));
      if (retcode < 0 && retcode != -ENOENT ) {
        drain_all();
        if (!syncstopped) {
          /* wait for all operations to complete */
          return set_cr_error(retcode);
        } else {
          /* no need to retry */
          break;	
	}
      }
      squash_map.clear();
      for (auto& e : list_result) {
        if (e.op == RGWModifyOp::CLS_RGW_OP_SYNCSTOP && (sync_modify_time < e.timestamp)) {
          ldout(sync_env->cct, 20) << " syncstop on " << e.timestamp << dendl;
          sync_modify_time = e.timestamp;
          syncstopped = true;
          continue;
        }
        if (e.op == RGWModifyOp::CLS_RGW_OP_RESYNC && (sync_modify_time < e.timestamp)) {
          ldout(sync_env->cct, 20) << " resync on " << e.timestamp << dendl;
          sync_modify_time = e.timestamp;
          syncstopped = false;
          continue;
        }
        if (e.op == CLS_RGW_OP_CANCEL) {
          continue;
        }
        if (e.state != CLS_RGW_STATE_COMPLETE) {
          continue;
        }
        if (e.zones_trace.find(zone_id) != e.zones_trace.end()) {
          continue;
        }
        auto& squash_entry = squash_map[make_pair(e.object, e.instance)];
        if (squash_entry.first <= e.timestamp) {
          squash_entry = make_pair<>(e.timestamp, e.op);
        }
      }

      entries_iter = list_result.begin();
      for (; entries_iter != list_result.end(); ++entries_iter) {
        if (!lease_cr->is_locked()) {
          drain_all();
          return set_cr_error(-ECANCELED);
        }
        entry = &(*entries_iter);
        {
          ssize_t p = entry->id.find('#'); /* entries might have explicit shard info in them, e.g., 6#00000000004.94.3 */
          if (p < 0) {
            cur_id = entry->id;
          } else {
            cur_id = entry->id.substr(p + 1);
          }
        }
        inc_marker.position = cur_id;

        if (entry->op == RGWModifyOp::CLS_RGW_OP_SYNCSTOP || entry->op == RGWModifyOp::CLS_RGW_OP_RESYNC) {
          ldout(sync_env->cct, 20) << "detected syncstop or resync  on " << entries_iter->timestamp << " , skipping entry" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }

        if (!key.set(rgw_obj_index_key{entry->object, entry->instance})) {
          set_status() << "parse_raw_oid() on " << entry->object << " returned false, skipping entry";
          ldout(sync_env->cct, 20) << "parse_raw_oid() on " << entry->object << " returned false, skipping entry" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }

        ldout(sync_env->cct, 20) << "parsed entry: id=" << cur_id << " iter->object=" << entry->object << " iter->instance=" << entry->instance << " name=" << key.name << " instance=" << key.instance << " ns=" << key.ns << dendl;

        if (!key.ns.empty()) {
          set_status() << "skipping entry in namespace: " << entry->object;
          ldout(sync_env->cct, 20) << "skipping entry in namespace: " << entry->object << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }

        set_status() << "got entry.id=" << cur_id << " key=" << key << " op=" << (int)entry->op;
        if (entry->op == CLS_RGW_OP_CANCEL) {
          set_status() << "canceled operation, skipping";
          ldout(sync_env->cct, 20) << "[inc sync] skipping object: "
              << bucket_shard_str{bs} << "/" << key << ": canceled operation" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }
        if (entry->state != CLS_RGW_STATE_COMPLETE) {
          set_status() << "non-complete operation, skipping";
          ldout(sync_env->cct, 20) << "[inc sync] skipping object: "
              << bucket_shard_str{bs} << "/" << key << ": non-complete operation" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }
        if (entry->zones_trace.find(zone_id) != entry->zones_trace.end()) {
          set_status() << "redundant operation, skipping";
          ldout(sync_env->cct, 20) << "[inc sync] skipping object: "
              <<bucket_shard_str{bs} <<"/"<<key<<": redundant operation" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }
        if (make_pair<>(entry->timestamp, entry->op) != squash_map[make_pair(entry->object, entry->instance)]) {
          set_status() << "squashed operation, skipping";
          ldout(sync_env->cct, 20) << "[inc sync] skipping object: "
              << bucket_shard_str{bs} << "/" << key << ": squashed operation" << dendl;
          /* not updating high marker though */
          continue;
        }
        ldout(sync_env->cct, 20) << "[inc sync] syncing object: "
            << bucket_shard_str{bs} << "/" << key << dendl;
        updated_status = false;
        while (!marker_tracker.can_do_op(key)) {
          if (!updated_status) {
            set_status() << "can't do op, conflicting inflight operation";
            updated_status = true;
          }
          ldout(sync_env->cct, 5) << *this << ": [inc sync] can't do op on key=" << key << " need to wait for conflicting operation to complete" << dendl;
          yield wait_for_child();
          bool again = true;
          while (again) {
            again = collect(&ret, nullptr);
            if (ret < 0) {
              ldout(sync_env->cct, 0) << "ERROR: a child operation returned error (ret=" << ret << ")" << dendl;
              sync_status = ret;
              /* we have reported this error */
            }
          }
        }
        if (!marker_tracker.index_key_to_marker(key, cur_id)) {
          set_status() << "can't do op, sync already in progress for object";
          ldout(sync_env->cct, 20) << __func__ << ": skipping sync of entry: " << cur_id << ":" << key << " sync already in progress for object" << dendl;
          marker_tracker.try_update_high_marker(cur_id, 0, entry->timestamp);
          continue;
        }
        // yield {
          set_status() << "start object sync";
          if (!marker_tracker.start(cur_id, 0, entry->timestamp)) {
            ldout(sync_env->cct, 0) << "ERROR: cannot start syncing " << cur_id << ". Duplicate entry?" << dendl;
          } else {
            uint64_t versioned_epoch = 0;
            rgw_bucket_entry_owner owner(entry->owner, entry->owner_display_name);
            if (entry->ver.pool < 0) {
              versioned_epoch = entry->ver.epoch;
            }
            ldout(sync_env->cct, 20) << __func__ << "(): entry->timestamp=" << entry->timestamp << dendl;
            using SyncCR = RGWBucketSyncSingleEntryCR<string, rgw_obj_key>;
            spawn(new SyncCR(sync_env, bucket_info, bs, key,
                             entry->is_versioned(), versioned_epoch,
                             entry->timestamp, owner, entry->op, entry->state,
                             cur_id, &marker_tracker, entry->zones_trace),
                  false);
          }
        // }
        while (num_spawned() > BUCKET_SYNC_SPAWN_WINDOW) {
          set_status() << "num_spawned() > spawn_window";
          yield wait_for_child();
          bool again = true;
          while (again) {
            again = collect(&ret, nullptr);
            if (ret < 0) {
              ldout(sync_env->cct, 0) << "ERROR: a sync operation returned error" << dendl;
              sync_status = ret;
              /* we have reported this error */
            }
            /* not waiting for child here */
          }
        }
      }
    } while (!list_result.empty() && sync_status == 0);

    if (syncstopped) {
      drain_all();

      yield {
        const string& oid = RGWBucketSyncStatusManager::status_oid(sync_env->source_zone, bs);
        RGWRados *store = sync_env->store;
        call(new RGWRadosRemoveCR(store, rgw_raw_obj{store->get_zone_params().log_pool, oid}));
      }
      lease_cr->abort();
      return set_cr_done();
    }

    while (num_spawned()) {
      yield wait_for_child();
      bool again = true;
      while (again) {
        again = collect(&ret, nullptr);
        if (ret < 0) {
          ldout(sync_env->cct, 0) << "ERROR: a sync operation returned error" << dendl;
          sync_status = ret;
          /* we have reported this error */
        }
        /* not waiting for child here */
      }
    }

    yield call(marker_tracker.flush());
    if (retcode < 0) {
      ldout(sync_env->cct, 0) << "ERROR: marker_tracker.flush() returned retcode=" << retcode << dendl;
      return set_cr_error(retcode);
    }
    if (sync_status < 0) {
      ldout(sync_env->cct, 0) << "ERROR: failure in sync, backing out (sync_status=" << sync_status<< ")" << dendl;
    }

    /* wait for all operations to complete */
    drain_all();

    if (sync_status < 0) {
      return set_cr_error(sync_status);
    }

    return set_cr_done();
  }
  return 0;
}

int RGWRunBucketSyncCoroutine::operate()
{
  reenter(this) {
    yield {
      set_status("acquiring sync lock");
      auto store = sync_env->store;
      lease_cr.reset(new RGWContinuousLeaseCR(sync_env->async_rados, store,
                                              rgw_raw_obj(store->get_zone_params().log_pool, status_oid),
                                              "sync_lock",
                                              cct->_conf->rgw_sync_lease_period,
                                              this));
      lease_stack.reset(spawn(lease_cr.get(), false));
    }
    while (!lease_cr->is_locked()) {
      if (lease_cr->is_done()) {
        ldout(cct, 5) << "lease cr failed, done early" << dendl;
        set_status("lease lock failed, early abort");
        return set_cr_error(lease_cr->get_ret_status());
      }
      set_sleeping(true);
      yield;
    }

    yield call(new RGWReadBucketSyncStatusCoroutine(sync_env, bs, &sync_status));
    if (retcode < 0 && retcode != -ENOENT) {
      ldout(sync_env->cct, 0) << "ERROR: failed to read sync status for bucket="
          << bucket_shard_str{bs} << dendl;
      lease_cr->go_down();
      drain_all();
      return set_cr_error(retcode);
    }

    ldout(sync_env->cct, 20) << __func__ << "(): sync status for bucket "
        << bucket_shard_str{bs} << ": " << sync_status.state << dendl;

    yield call(new RGWGetBucketInstanceInfoCR(sync_env->async_rados, sync_env->store, bs.bucket, &bucket_info));
    if (retcode == -ENOENT) {
      /* bucket instance info has not been synced in yet, fetch it now */
      yield {
        ldout(sync_env->cct, 10) << "no local info for bucket "
            << bucket_str{bs.bucket} << ": fetching metadata" << dendl;
        string raw_key = string("bucket.instance:") + bs.bucket.get_key();

        meta_sync_env.init(cct, sync_env->store, sync_env->store->rest_master_conn, sync_env->async_rados, sync_env->http_manager, sync_env->error_logger);

        call(new RGWMetaSyncSingleEntryCR(&meta_sync_env, raw_key,
                                          string() /* no marker */,
                                          MDLOG_STATUS_COMPLETE,
                                          NULL /* no marker tracker */));
      }
      if (retcode < 0) {
        ldout(sync_env->cct, 0) << "ERROR: failed to fetch bucket instance info for " << bucket_str{bs.bucket} << dendl;
        lease_cr->go_down();
        drain_all();
        return set_cr_error(retcode);
      }

      yield call(new RGWGetBucketInstanceInfoCR(sync_env->async_rados, sync_env->store, bs.bucket, &bucket_info));
    }
    if (retcode < 0) {
      ldout(sync_env->cct, 0) << "ERROR: failed to retrieve bucket info for bucket=" << bucket_str{bs.bucket} << dendl;
      lease_cr->go_down();
      drain_all();
      return set_cr_error(retcode);
    }

    if (sync_status.state == rgw_bucket_shard_sync_info::StateInit) {
      yield call(new RGWInitBucketShardSyncStatusCoroutine(sync_env, bs, sync_status));
      if (retcode < 0) {
        ldout(sync_env->cct, 0) << "ERROR: init sync on " << bucket_shard_str{bs}
            << " failed, retcode=" << retcode << dendl;
        lease_cr->go_down();
        drain_all();
        return set_cr_error(retcode);
      }
    }

    if (sync_status.state == rgw_bucket_shard_sync_info::StateFullSync) {
      yield call(new RGWBucketShardFullSyncCR(sync_env, bs, &bucket_info,
                                              status_oid, lease_cr.get(),
                                              sync_status.full_marker));
      if (retcode < 0) {
        ldout(sync_env->cct, 5) << "full sync on " << bucket_shard_str{bs}
            << " failed, retcode=" << retcode << dendl;
        lease_cr->go_down();
        drain_all();
        return set_cr_error(retcode);
      }
      sync_status.state = rgw_bucket_shard_sync_info::StateIncrementalSync;
    }

    if (sync_status.state == rgw_bucket_shard_sync_info::StateIncrementalSync) {
      yield call(new RGWBucketShardIncrementalSyncCR(sync_env, bs, &bucket_info,
                                                     status_oid, lease_cr.get(),
                                                     sync_status.inc_marker));
      if (retcode < 0) {
        ldout(sync_env->cct, 5) << "incremental sync on " << bucket_shard_str{bs}
            << " failed, retcode=" << retcode << dendl;
        lease_cr->go_down();
        drain_all();
        return set_cr_error(retcode);
      }
    }

    lease_cr->go_down();
    drain_all();
    return set_cr_done();
  }

  return 0;
}

RGWCoroutine *RGWRemoteBucketLog::run_sync_cr()
{
  return new RGWRunBucketSyncCoroutine(&sync_env, bs);
}

int RGWBucketSyncStatusManager::init()
{
  conn = store->get_zone_conn_by_id(source_zone);
  if (!conn) {
    ldout(store->ctx(), 0) << "connection object to zone " << source_zone << " does not exist" << dendl;
    return -EINVAL;
  }

  int ret = http_manager.set_threaded();
  if (ret < 0) {
    ldout(store->ctx(), 0) << "failed in http_manager.set_threaded() ret=" << ret << dendl;
    return ret;
  }


  const string key = bucket.get_key();

  rgw_http_param_pair pairs[] = { { "key", key.c_str() },
                                  { NULL, NULL } };

  string path = string("/admin/metadata/bucket.instance");

  bucket_instance_meta_info result;
  ret = cr_mgr.run(new RGWReadRESTResourceCR<bucket_instance_meta_info>(store->ctx(), conn, &http_manager, path, pairs, &result));
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to fetch bucket metadata info from zone=" << source_zone << " path=" << path << " key=" << key << " ret=" << ret << dendl;
    return ret;
  }

  RGWBucketInfo& bi = result.data.get_bucket_info();
  num_shards = bi.num_shards;

  error_logger = new RGWSyncErrorLogger(store, RGW_SYNC_ERROR_LOG_SHARD_PREFIX, ERROR_LOGGER_SHARDS);

  sync_module.reset(new RGWDefaultSyncModuleInstance());

  int effective_num_shards = (num_shards ? num_shards : 1);

  auto async_rados = store->get_async_rados();

  for (int i = 0; i < effective_num_shards; i++) {
    RGWRemoteBucketLog *l = new RGWRemoteBucketLog(store, this, async_rados, &http_manager);
    ret = l->init(source_zone, conn, bucket, (num_shards ? i : -1), error_logger, sync_module);
    if (ret < 0) {
      ldout(store->ctx(), 0) << "ERROR: failed to initialize RGWRemoteBucketLog object" << dendl;
      return ret;
    }
    source_logs[i] = l;
  }

  return 0;
}

int RGWBucketSyncStatusManager::init_sync_status()
{
  list<RGWCoroutinesStack *> stacks;

  for (map<int, RGWRemoteBucketLog *>::iterator iter = source_logs.begin(); iter != source_logs.end(); ++iter) {
    RGWCoroutinesStack *stack = new RGWCoroutinesStack(store->ctx(), &cr_mgr);
    RGWRemoteBucketLog *l = iter->second;
    stack->call(l->init_sync_status_cr());

    stacks.push_back(stack);
  }

  return cr_mgr.run(stacks);
}

int RGWBucketSyncStatusManager::read_sync_status()
{
  list<RGWCoroutinesStack *> stacks;

  for (map<int, RGWRemoteBucketLog *>::iterator iter = source_logs.begin(); iter != source_logs.end(); ++iter) {
    RGWCoroutinesStack *stack = new RGWCoroutinesStack(store->ctx(), &cr_mgr);
    RGWRemoteBucketLog *l = iter->second;
    stack->call(l->read_sync_status_cr(&sync_status[iter->first]));

    stacks.push_back(stack);
  }

  int ret = cr_mgr.run(stacks);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to read sync status for "
        << bucket_str{bucket} << dendl;
    return ret;
  }

  return 0;
}

int RGWBucketSyncStatusManager::run()
{
  list<RGWCoroutinesStack *> stacks;

  for (map<int, RGWRemoteBucketLog *>::iterator iter = source_logs.begin(); iter != source_logs.end(); ++iter) {
    RGWCoroutinesStack *stack = new RGWCoroutinesStack(store->ctx(), &cr_mgr);
    RGWRemoteBucketLog *l = iter->second;
    stack->call(l->run_sync_cr());

    stacks.push_back(stack);
  }

  int ret = cr_mgr.run(stacks);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed to read sync status for "
        << bucket_str{bucket} << dendl;
    return ret;
  }

  return 0;
}

string RGWBucketSyncStatusManager::status_oid(const string& source_zone,
                                              const rgw_bucket_shard& bs)
{
  return bucket_status_oid_prefix + "." + source_zone + ":" + bs.get_key();
}

class RGWCollectBucketSyncStatusCR : public RGWShardCollectCR {
  static constexpr int max_concurrent_shards = 16;
  RGWRados *const store;
  RGWDataSyncEnv *const env;
  const int num_shards;
  rgw_bucket_shard bs;

  using Vector = std::vector<rgw_bucket_shard_sync_info>;
  Vector::iterator i, end;

 public:
  RGWCollectBucketSyncStatusCR(RGWRados *store, RGWDataSyncEnv *env,
                               int num_shards, const rgw_bucket& bucket,
                               Vector *status)
    : RGWShardCollectCR(store->ctx(), max_concurrent_shards),
      store(store), env(env), num_shards(num_shards),
      bs(bucket, num_shards > 0 ? 0 : -1), // start at shard 0 or -1
      i(status->begin()), end(status->end())
  {}

  bool spawn_next() override {
    if (i == end) {
      return false;
    }
    spawn(new RGWReadBucketSyncStatusCoroutine(env, bs, &*i), false);
    ++i;
    ++bs.shard_id;
    return true;
  }
};

int rgw_bucket_sync_status(RGWRados *store, const std::string& source_zone,
                           const rgw_bucket& bucket,
                           std::vector<rgw_bucket_shard_sync_info> *status)
{
  // read the bucket instance info for num_shards
  RGWObjectCtx ctx(store);
  RGWBucketInfo info;
  int ret = store->get_bucket_instance_info(ctx, bucket, info, nullptr, nullptr);
  if (ret < 0) {
    return ret;
  }
  status->clear();
  status->resize(std::max<size_t>(1, info.num_shards));

  RGWDataSyncEnv env;
  RGWSyncModuleInstanceRef module; // null sync module
  env.init(store->ctx(), store, nullptr, store->get_async_rados(),
           nullptr, nullptr, source_zone, module, nullptr);

  RGWCoroutinesManager crs(store->ctx(), store->get_cr_registry());
  return crs.run(new RGWCollectBucketSyncStatusCR(store, &env, info.num_shards,
                                                  bucket, status));
}


// TODO: move into rgw_data_sync_trim.cc
#undef dout_prefix
#define dout_prefix (*_dout << "data trim: ")

namespace {

/// return the marker that it's safe to trim up to
const std::string& get_stable_marker(const rgw_data_sync_marker& m)
{
  return m.state == m.FullSync ? m.next_step_marker : m.marker;
}

/// comparison operator for take_min_markers()
bool operator<(const rgw_data_sync_marker& lhs,
               const rgw_data_sync_marker& rhs)
{
  // sort by stable marker
  return get_stable_marker(lhs) < get_stable_marker(rhs);
}

/// populate the container starting with 'dest' with the minimum stable marker
/// of each shard for all of the peers in [first, last)
template <typename IterIn, typename IterOut>
void take_min_markers(IterIn first, IterIn last, IterOut dest)
{
  if (first == last) {
    return;
  }
  // initialize markers with the first peer's
  auto m = dest;
  for (auto &shard : first->sync_markers) {
    *m = std::move(shard.second);
    ++m;
  }
  // for remaining peers, replace with smaller markers
  for (auto p = first + 1; p != last; ++p) {
    m = dest;
    for (auto &shard : p->sync_markers) {
      if (shard.second < *m) {
        *m = std::move(shard.second);
      }
      ++m;
    }
  }
}

} // anonymous namespace

class DataLogTrimCR : public RGWCoroutine {
  RGWRados *store;
  RGWHTTPManager *http;
  const int num_shards;
  const std::string& zone_id; //< my zone id
  std::vector<rgw_data_sync_status> peer_status; //< sync status for each peer
  std::vector<rgw_data_sync_marker> min_shard_markers; //< min marker per shard
  std::vector<std::string>& last_trim; //< last trimmed marker per shard
  int ret{0};

 public:
  DataLogTrimCR(RGWRados *store, RGWHTTPManager *http,
                   int num_shards, std::vector<std::string>& last_trim)
    : RGWCoroutine(store->ctx()), store(store), http(http),
      num_shards(num_shards),
      zone_id(store->get_zone().id),
      peer_status(store->zone_conn_map.size()),
      min_shard_markers(num_shards),
      last_trim(last_trim)
  {}

  int operate() override;
};

int DataLogTrimCR::operate()
{
  reenter(this) {
    ldout(cct, 10) << "fetching sync status for zone " << zone_id << dendl;
    set_status("fetching sync status");
    yield {
      // query data sync status from each sync peer
      rgw_http_param_pair params[] = {
        { "type", "data" },
        { "status", nullptr },
        { "source-zone", zone_id.c_str() },
        { nullptr, nullptr }
      };

      auto p = peer_status.begin();
      for (auto& c : store->zone_conn_map) {
        ldout(cct, 20) << "query sync status from " << c.first << dendl;
        using StatusCR = RGWReadRESTResourceCR<rgw_data_sync_status>;
        spawn(new StatusCR(cct, c.second, http, "/admin/log/", params, &*p),
              false);
        ++p;
      }
    }

    // must get a successful reply from all peers to consider trimming
    ret = 0;
    while (ret == 0 && num_spawned() > 0) {
      yield wait_for_child();
      collect_next(&ret);
    }
    drain_all();

    if (ret < 0) {
      ldout(cct, 4) << "failed to fetch sync status from all peers" << dendl;
      return set_cr_error(ret);
    }

    ldout(cct, 10) << "trimming log shards" << dendl;
    set_status("trimming log shards");
    yield {
      // determine the minimum marker for each shard
      take_min_markers(peer_status.begin(), peer_status.end(),
                       min_shard_markers.begin());

      for (int i = 0; i < num_shards; i++) {
        const auto& m = min_shard_markers[i];
        auto& stable = get_stable_marker(m);
        if (stable <= last_trim[i]) {
          continue;
        }
        ldout(cct, 10) << "trimming log shard " << i
            << " at marker=" << stable
            << " last_trim=" << last_trim[i] << dendl;
        using TrimCR = RGWSyncLogTrimCR;
        spawn(new TrimCR(store, store->data_log->get_oid(i),
                         stable, &last_trim[i]),
              true);
      }
    }
    return set_cr_done();
  }
  return 0;
}

class DataLogTrimPollCR : public RGWCoroutine {
  RGWRados *store;
  RGWHTTPManager *http;
  const int num_shards;
  const utime_t interval; //< polling interval
  const std::string lock_oid; //< use first data log shard for lock
  const std::string lock_cookie;
  std::vector<std::string> last_trim; //< last trimmed marker per shard

 public:
  DataLogTrimPollCR(RGWRados *store, RGWHTTPManager *http,
                    int num_shards, utime_t interval)
    : RGWCoroutine(store->ctx()), store(store), http(http),
      num_shards(num_shards), interval(interval),
      lock_oid(store->data_log->get_oid(0)),
      lock_cookie(RGWSimpleRadosLockCR::gen_random_cookie(cct)),
      last_trim(num_shards)
  {}

  int operate() override;
};

int DataLogTrimPollCR::operate()
{
  reenter(this) {
    for (;;) {
      set_status("sleeping");
      wait(interval);

      // request a 'data_trim' lock that covers the entire wait interval to
      // prevent other gateways from attempting to trim for the duration
      set_status("acquiring trim lock");
      yield call(new RGWSimpleRadosLockCR(store->get_async_rados(), store,
                                          rgw_raw_obj(store->get_zone_params().log_pool, lock_oid),
                                          "data_trim", lock_cookie,
                                          interval.sec()));
      if (retcode < 0) {
        // if the lock is already held, go back to sleep and try again later
        ldout(cct, 4) << "failed to lock " << lock_oid << ", trying again in "
            << interval.sec() << "s" << dendl;
        continue;
      }

      set_status("trimming");
      yield call(new DataLogTrimCR(store, http, num_shards, last_trim));

      // note that the lock is not released. this is intentional, as it avoids
      // duplicating this work in other gateways
    }
  }
  return 0;
}

RGWCoroutine* create_data_log_trim_cr(RGWRados *store,
                                      RGWHTTPManager *http,
                                      int num_shards, utime_t interval)
{
  return new DataLogTrimPollCR(store, http, num_shards, interval);
}
