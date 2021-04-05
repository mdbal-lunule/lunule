// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef __MDS_ADSL_REQTRACER_H__
#define __MDS_ADSL_REQTRACER_H__

#include <map>
using std::map;
#include <string>
using std::string;
#include <list>
using std::list;
#include <vector>
using std::vector;
using std::pair;

#include "include/assert.h"
#include "common/Thread.h"
#include "common/Mutex.h"

#define REQTRACER_QUEUE_LEN_DEFAULT 5

class ReqTracer : public Thread {
    struct ReqCollector {
      map<string, int> coll;
      ReqCollector() {}
      void hit(string & path);
      bool has(string & path, bool nested = false) const;
      int get(string & path, bool nested = false) const;
      int size() const;
      map<string, int>::iterator begin() { return coll.begin(); }
      map<string, int>::iterator end() { return coll.end(); }
      int total() const;

      void swap(ReqCollector & another) { std::swap(coll, another.coll); }
    };
    list <ReqCollector> _data;
    ReqCollector _last;
    Mutex alpha_beta_mut;

    bool m_runFlag;

    bool visited(string & path, bool nested = false) const;
    int visited_count(string & path, bool nested = false) const;

    static void polish(string & path);
  public:
    static bool check_path_under(const string & parent, const string & child, bool direct = false);
    ReqTracer(int queue_len = REQTRACER_QUEUE_LEN_DEFAULT);
    void switch_epoch();
    void hit(string & path);
    pair<double, double> alpha_beta(string path, int subtree_size, vector<string> & betastrs);
  protected:
    void *entry() override;
    void mark_stop() { m_runFlag = false; }
    void wait_finish() { Thread::join(); }
};

#endif
