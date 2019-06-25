#include "include/types.h"

#include <map>
#include <list>
#include <set>
using std::map;
using std::list;
using std::set;


class MDSRank;
class CDir;
class CInode;
class CDentry;

class MExportDirDiscover;
class MExportDirDiscoverAck;
class MExportDirCancel;
class MExportDirPrep;
class MExportDirPrepAck;
class MExportDir;
class MExportDirAck;
class MExportDirNotify;
class MExportDirNotifyAck;
class MExportDirFinish;

class MExportCaps;
class MExportCapsAck;
class MGatherCaps;

class EImportStart;


#include "MDSRank.h"
#include "MDCache.h"
#include "CInode.h"
#include "CDir.h"
#include "CDentry.h"
#include "Migrator.h"
#include "Locker.h"
#include "Server.h"

#include "MDBalancer.h"
#include "MDLog.h"
#include "MDSMap.h"
#include "Mutation.h"

#include "include/filepath.h"

#include "events/EExport.h"
#include "events/EImportStart.h"
#include "events/EImportFinish.h"
#include "events/ESessions.h"

#include "msg/Messenger.h"

#include "messages/MClientCaps.h"

#include "messages/MExportDirDiscover.h"
#include "messages/MExportDirDiscoverAck.h"
#include "messages/MExportDirCancel.h"
#include "messages/MExportDirPrep.h"
#include "messages/MExportDirPrepAck.h"
#include "messages/MExportDir.h"
#include "messages/MExportDirAck.h"
#include "messages/MExportDirNotify.h"
#include "messages/MExportDirNotifyAck.h"
#include "messages/MExportDirFinish.h"

#include "messages/MExportCaps.h"
#include "messages/MExportCapsAck.h"
#include "messages/MGatherCaps.h"


#include "MigratorIPC.h"

#include "common/config.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix *_dout << "mds." << mig->mds->get_nodeid() << ".migrator IPC "

void *ipc_migrator(void *arg){
	Migrator *mig = reinterpret_cast<Migrator*>(arg);
	dout(0) << __func__ << " This is a IPC Thread - from Youxu" << dendl;
	test(mig);
	return NULL;
}

void test(Migrator *mig){
	dout(0) << __func__ << " I am a test procedure of ipc_migrator, now print export_queue " << mig->get_export_statename(2) << dendl;
	return;
}