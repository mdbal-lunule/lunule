// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "ReqTracer.h"

void ReqTracer::ReqCollector::hit(string path)
{
  while (path.back() == '/') {
    path.pop_back();
  }
  if (coll.count(path)) {
    coll[path] += 1;
  } else {
    coll.insert(std::make_pair<string, int>(std::move(path), 1));
  }
}

bool ReqTracer::ReqCollector::has(string path, bool nested) const
{
  while (path.back() == '/') {
    path.pop_back();
  }

  if (!nested)
    return coll.count(path);

  string dirpath = path + '/';
  for (auto it = coll.begin(); it != coll.end(); it++) {
    string fullpath = it->first;
    if (fullpath == path || fullpath.find_first_of(dirpath) == 0) {
      return true;
    }
  }
  return false;
}

int ReqTracer::ReqCollector::get(string path, bool nested) const
{
  while (path.back() == '/') {
    path.pop_back();
  }

  if (!nested)
    return coll.count(path) ? coll.at(path) : 0;

  int count = 0;
  string dirpath = path + '/';
  for (auto it = coll.begin(); it != coll.end(); it++) {
    string fullpath = it->first;
    if (fullpath == path || fullpath.find_first_of(dirpath) == 0) {
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
  : _data(queue_len), alpha_beta_mut("lunule-alpha-beta")
{}
  
bool ReqTracer::visited(string path, bool nested) const
{
  for (list<ReqCollector>::const_iterator it = _data.begin(); it != _data.end(); it++) {
    if (it->has(path, nested)) return true;
  }
  return false;
}

int ReqTracer::visited_count(string path, bool nested) const
{
  int count = 0;
  for (list<ReqCollector>::const_iterator it = _data.begin(); it != _data.end(); it++) {
    count += it->get(path, nested);
  }
  return count;
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

void ReqTracer::hit(string path)
{
  Mutex::Locker l(alpha_beta_mut);
  _last.hit(path);
}

pair<double, double> ReqTracer::alpha_beta(string path, int subtree_size)
{
  Mutex::Locker l(alpha_beta_mut);

  int oldcnt = 0, newcnt = 0;
  for (auto it = _last.begin(); it != _last.end(); it++) {
    if (visited(it->first)) {
      oldcnt += it->second;
    } else {
      newcnt += it->second;
    }
  }
  double alpha = (double) oldcnt / (oldcnt + newcnt);
  double beta = (double) (subtree_size - newcnt) / subtree_size;
  return std::make_pair<double, double>(std::move(alpha), std::move(beta));
}

void * ReqTracer::entry()
{
  return 0;
}
