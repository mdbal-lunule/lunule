// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef _MDS_ADSL_REQCOUNTER_H_
#define _MDS_ADSL_REQCOUNTER_H_

#include <list>
using std::list;
#include "common/Mutex.h"

#define REQCOUNTER_QUEUE_LEN_DEFAULT 5

class ReqCounter {
    list<bool> _data;
    bool _last_hit;
    int _cache_hit_times;
    Mutex mut;
  public:
    ReqCounter(int queue_len = REQCOUNTER_QUEUE_LEN_DEFAULT);
    void switch_epoch(int epoch_num = 1);
    int hit();
};

#endif /* mds/adsl/ReqCounter.h */
