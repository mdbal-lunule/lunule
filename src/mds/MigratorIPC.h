#ifndef CEPH_MDS_IPC
#define CEPH_MDS_IPC

#include "Migrator.h"

class Migrator;

void *ipc_migrator(void *arg);

void test(Migrator *mig);

#endif