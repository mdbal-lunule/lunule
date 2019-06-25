// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RBD_MIRROR_CLUSTER_WATCHER_H
#define CEPH_RBD_MIRROR_CLUSTER_WATCHER_H

#include <map>
#include <memory>
#include <set>

#include "common/ceph_context.h"
#include "common/Mutex.h"
#include "common/Timer.h"
#include "include/rados/librados.hpp"
#include "types.h"
#include "tools/rbd_mirror/service_daemon/Types.h"
#include <unordered_map>

namespace librbd { struct ImageCtx; }

namespace rbd {
namespace mirror {

template <typename> class ServiceDaemon;

/**
 * Tracks mirroring configuration for pools in a single
 * cluster.
 */
class ClusterWatcher {
public:
  typedef std::set<peer_t> Peers;
  typedef std::map<int64_t, Peers>  PoolPeers;
  typedef std::set<std::string> PoolNames;

  ClusterWatcher(RadosRef cluster, Mutex &lock,
                 ServiceDaemon<librbd::ImageCtx>* service_daemon);
  ~ClusterWatcher() = default;
  ClusterWatcher(const ClusterWatcher&) = delete;
  ClusterWatcher& operator=(const ClusterWatcher&) = delete;

  // Caller controls frequency of calls
  void refresh_pools();
  const PoolPeers& get_pool_peers() const;

private:
  typedef std::unordered_map<int64_t, service_daemon::CalloutId> ServicePools;

  RadosRef m_cluster;
  Mutex &m_lock;
  ServiceDaemon<librbd::ImageCtx>* m_service_daemon;

  ServicePools m_service_pools;
  PoolPeers m_pool_peers;

  void read_pool_peers(PoolPeers *pool_peers, PoolNames *pool_names);
};

} // namespace mirror
} // namespace rbd

#endif // CEPH_RBD_MIRROR_CLUSTER_WATCHER_H
