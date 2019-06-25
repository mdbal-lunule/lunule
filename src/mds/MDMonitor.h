#ifndef CEPH_MDS_MONITOR_H
#define CEPH_MDS_MONITOR_H

#include <boost/utility/string_view.hpp>

#include "MDSRank.h"
#include "Mutation.h"

#include <boost/lexical_cast.hpp>
#include "include/assert.h"  // lexical_cast includes system assert.h

#include <boost/config/warning_disable.hpp>
#include <boost/fusion/include/std_pair.hpp>

#include "MDSRank.h"
#include "Server.h"
#include "Locker.h"
#include "MDCache.h"
#include "MDLog.h"
#include "Migrator.h"
#include "MDBalancer.h"
#include "InoTable.h"
#include "SnapClient.h"
#include "Mutation.h"

#include "msg/Messenger.h"

#include "osdc/Objecter.h"

#include "messages/MClientSession.h"
#include "messages/MClientRequest.h"
#include "messages/MClientReply.h"
#include "messages/MClientReconnect.h"
#include "messages/MClientCaps.h"
#include "messages/MClientSnap.h"

#include "messages/MMDSSlaveRequest.h"

#include "messages/MLock.h"

#include "events/EUpdate.h"
#include "events/ESlaveUpdate.h"
#include "events/ESession.h"
#include "events/EOpen.h"
#include "events/ECommitted.h"

#include "include/filepath.h"
#include "common/errno.h"
#include "common/Timer.h"
#include "common/perf_counters.h"
#include "include/compat.h"
#include "osd/OSDMap.h"

#include <errno.h>

#include <list>
#include <iostream>
#include <boost/utility/string_view.hpp>
using namespace std;

#include "common/config.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << mds->get_nodeid() << ".server "

class OSDMap;
class PerfCounters;
class LogEvent;
class EMetaBlob;
class EUpdate;
class MMDSSlaveRequest;
struct SnapInfo;
class MClientRequest;
class MClientReply;
class MDLog;

/* monitor_init - set monitor queue to 0*/
void Server::monitor_init(){
	int i = 0;

	iops_client_request = 0;
	iops_slave_request = 0;
	// 28 is the length of the monitor request type
	for(; i < 28; i++){
		// mon_req[i] = 0;
		mon_op[i] = 0;
	}
}

/* monitor_run - compute the IOPS of MDS(requests handled per second)*/
void *Server::monitor_run(void *args){
	int i = 0;
	int iops;
	// int tmp[mon_mdss_req_num] = {0};
	Server *server = reinterpret_cast<Server*>(args);
	MDSRank *mds = server->mds;
	while(1){
		sleep(1);

		for(; i < mon_mdss_req_num; i++){
			iops += server->mon_op[i];
			server->mon_op[i] = 0;
		}

		dout(0) << __func__ << " IOPS " << iops << " IOPS-CLIENT-REQ " << server->iops_client_request << " IOPS-SLAVE-REQ " << server->iops_slave_request << " Cache-Inodes " << server->mdcache->lru.lru_get_size() << " Cache-Inodes-Pinned " << server->mdcache->lru.lru_get_num_pinned() << dendl;
		
		i = 0;
		iops = 0;
		server->iops_client_request = 0;
		server->iops_slave_request = 0;
		// dout(20) << __func__ << " IOPS " << iops << dendl;
		// dout(20) << __func__ << " iops_client_request " << server->iops_client_request << dendl;
	}

	return NULL;
}
#endif
