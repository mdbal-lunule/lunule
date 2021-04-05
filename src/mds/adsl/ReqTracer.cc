// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "ReqTracer.h"

void ReqTracer::ReqCollector::hit(string & path)
{
  polish(path);

  if (coll.count(path)) {
    coll[path] += 1;
  } else {
    coll.insert(std::make_pair<string, int>(std::move(path), 1));
  }
}

bool ReqTracer::ReqCollector::has(string & path, bool nested) const
{
  polish(path);

  if (!nested)
    return coll.count(path);

  for (auto it = coll.begin(); it != coll.end(); it++) {
    if (check_path_under(path, it->first)) {
      return true;
    }
  }
  return false;
}

int ReqTracer::ReqCollector::get(string & path, bool nested) const
{
  polish(path);

  if (!nested)
    return coll.count(path) ? coll.at(path) : 0;

  int count = 0;
  for (auto it = coll.begin(); it != coll.end(); it++) {
    if (check_path_under(path, it->first)) {
      // starts with path
      count += it->second;
    }
  }
  return count;
}

int ReqTracer::ReqCollector::size() const
{
  return coll.size();
}

int ReqTracer::ReqCollector::total() const
{
  int count = 0;
  for (auto it = coll.begin(); it != coll.end(); it++) {
    count += it->second;
  }
  return count;
}

ReqTracer::ReqTracer(int queue_len)
  : _data(queue_len), alpha_beta_mut("lunule-alpha-beta"), m_runFlag(true)
{
  //create("MDS-ReqTracer");
}
  
bool ReqTracer::visited(string & path, bool nested) const
{
  list<ReqCollector>::const_iterator last = _data.end();
  last--;
  for (list<ReqCollector>::const_iterator it = _data.begin(); it != last; it++) {
    if (it->has(path, nested)) return true;
  }
  return false;
}

int ReqTracer::visited_count(string & path, bool nested) const
{
  int count = 0;
  for (list<ReqCollector>::const_iterator it = _data.begin(); it != _data.end(); it++) {
    count += it->get(path, nested);
  }
  return count;
}

void ReqTracer::polish(string & path)
{
  while (path.back() == '/') {
    path.pop_back();
  }
  // root?
  if (!path.length()) {
    path.append(1, '/');
  }
}

bool ReqTracer::check_path_under(const string & parent, const string & child, bool direct)
{
  //string _parent = parent, _child = child;
  //polish(_parent);
  //polish(_child);

  if (parent == child)
    return true;

  size_t pos = child.find(parent != "/" ? (parent + '/') : parent);
  if (pos != 0)	return false;
  return direct ? (child.find('/', parent.length() + 1) == string::npos) : true;
}
  
void ReqTracer::switch_epoch()
{
  Mutex::Locker l(alpha_beta_mut);

  assert(_data.size() > 0);
  ReqCollector now_last;
  now_last.swap(_last);
  _data.pop_front();
  _data.push_back(now_last);
}

void ReqTracer::hit(string & path)
{
  Mutex::Locker l(alpha_beta_mut);
  _last.hit(path);
}

pair<double, double> ReqTracer::alpha_beta(string path, int subtree_size, vector<string> & betastrs)
{
  Mutex::Locker l(alpha_beta_mut);

  int oldcnt = 0, newcnt = 0, betacnt = 0;
  ReqCollector & coll = _data.back();
  for (auto it = coll.begin(); it != coll.end(); it++) {
    string fullpath = it->first;
    // first check if childdir
    if (!check_path_under(path, fullpath)) {
      continue;
    }

    if (visited(fullpath, true)) {
      oldcnt += it->second;
      betacnt++;
      betastrs.push_back(fullpath);
    } else {
      newcnt += it->second;
    }
  }
  int total = oldcnt + newcnt;
  double alpha = total ? ((double) oldcnt / (oldcnt + newcnt)) : 0.0;
  double beta = subtree_size ? ((double) (subtree_size - betacnt) / subtree_size) : 0.0;
  return std::make_pair<double, double>(std::move(alpha), std::move(beta));
}

void * ReqTracer::entry()
{
  while (m_runFlag) {
  }
  return 0;
}
