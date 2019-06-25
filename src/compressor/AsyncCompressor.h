// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_ASYNCCOMPRESSOR_H
#define CEPH_ASYNCCOMPRESSOR_H

#include <deque>
#include <vector>
#include <atomic>

#include "include/str_list.h"

#include "Compressor.h"
#include "common/WorkQueue.h"

class AsyncCompressor {
 private:
  CompressorRef compressor;
  CephContext *cct;
  std::atomic<uint64_t> job_id { 0 };
  vector<int> coreids;
  ThreadPool compress_tp;

  enum class status_t {
    WAIT,
    WORKING,
    DONE,
    ERROR
  };

  struct Job {
    uint64_t id;
    std::atomic<status_t> status { status_t::WAIT };
    bool is_compress;
    bufferlist data;
    Job(uint64_t i, bool compress): id(i), is_compress(compress) {}
    Job(const Job &j): id(j.id), status(j.status.load()), is_compress(j.is_compress), data(j.data) {}
  };
  Mutex job_lock;
  // only when job.status == DONE && with job_lock holding, we can insert/erase element in jobs
  // only when job.status == WAIT && with pool_lock holding, you can change its status and modify element's info later
  unordered_map<uint64_t, Job> jobs;

  struct CompressWQ : public ThreadPool::WorkQueue<Job> {
    typedef AsyncCompressor::Job Job;
    AsyncCompressor *async_compressor;
    deque<Job*> job_queue;

    CompressWQ(AsyncCompressor *ac, time_t timeout, time_t suicide_timeout, ThreadPool *tp)
      : ThreadPool::WorkQueue<Job>("AsyncCompressor::CompressWQ", timeout, suicide_timeout, tp), async_compressor(ac) {}

    bool _enqueue(Job *item) override {
      job_queue.push_back(item);
      return true;
    }
    void _dequeue(Job *item) override {
      ceph_abort();
    }
    bool _empty() override {
      return job_queue.empty();
    }
    Job* _dequeue() override {
      if (job_queue.empty())
        return NULL;
      Job *item = NULL;
      while (!job_queue.empty()) {
        item = job_queue.front();
        job_queue.pop_front();

        auto expected = status_t::WAIT;
        if (item->status.compare_exchange_strong(expected, status_t::WORKING)) {
          break;
        } else {
          Mutex::Locker l(async_compressor->job_lock);
          async_compressor->jobs.erase(item->id);
          item = NULL;
        }
      }
      return item;
    }
    void _process(Job *item, ThreadPool::TPHandle &) override {
      assert(item->status == status_t::WORKING);
      bufferlist out;
      int r;
      if (item->is_compress)
        r = async_compressor->compressor->compress(item->data, out);
      else
        r = async_compressor->compressor->decompress(item->data, out);
      if (!r) {
        item->data.swap(out);
        auto expected = status_t::WORKING;
        assert(item->status.compare_exchange_strong(expected, status_t::DONE));
      } else {
        item->status = status_t::ERROR;
      }
    }
    void _process_finish(Job *item) override {}
    void _clear() override {}
  } compress_wq;
  friend class CompressWQ;
  void _compress(bufferlist &in, bufferlist &out);
  void _decompress(bufferlist &in, bufferlist &out);

 public:
  explicit AsyncCompressor(CephContext *c);
  virtual ~AsyncCompressor() {}

  int get_cpuid(int id) {
    if (coreids.empty())
      return -1;
    return coreids[id % coreids.size()];
  }

  void init();
  void terminate();
  uint64_t async_compress(bufferlist &data);
  uint64_t async_decompress(bufferlist &data);
  int get_compress_data(uint64_t compress_id, bufferlist &data, bool blocking, bool *finished);
  int get_decompress_data(uint64_t decompress_id, bufferlist &data, bool blocking, bool *finished);
};

#endif
