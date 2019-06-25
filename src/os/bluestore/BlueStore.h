// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OSD_BLUESTORE_H
#define CEPH_OSD_BLUESTORE_H

#include "acconfig.h"

#include <unistd.h>

#include <atomic>
#include <mutex>
#include <condition_variable>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/functional/hash.hpp>
#include <boost/dynamic_bitset.hpp>

#include "include/assert.h"
#include "include/unordered_map.h"
#include "include/memory.h"
#include "include/mempool.h"
#include "common/Finisher.h"
#include "common/perf_counters.h"
#include "compressor/Compressor.h"
#include "os/ObjectStore.h"

#include "bluestore_types.h"
#include "BlockDevice.h"
#include "common/EventTrace.h"

class Allocator;
class FreelistManager;
class BlueFS;

//#define DEBUG_CACHE
//#define DEBUG_DEFERRED



// constants for Buffer::optimize()
#define MAX_BUFFER_SLOP_RATIO_DEN  8  // so actually 1/N


enum {
  l_bluestore_first = 732430,
  l_bluestore_kv_flush_lat,
  l_bluestore_kv_commit_lat,
  l_bluestore_kv_lat,
  l_bluestore_state_prepare_lat,
  l_bluestore_state_aio_wait_lat,
  l_bluestore_state_io_done_lat,
  l_bluestore_state_kv_queued_lat,
  l_bluestore_state_kv_committing_lat,
  l_bluestore_state_kv_done_lat,
  l_bluestore_state_deferred_queued_lat,
  l_bluestore_state_deferred_aio_wait_lat,
  l_bluestore_state_deferred_cleanup_lat,
  l_bluestore_state_finishing_lat,
  l_bluestore_state_done_lat,
  l_bluestore_throttle_lat,
  l_bluestore_submit_lat,
  l_bluestore_commit_lat,
  l_bluestore_read_lat,
  l_bluestore_read_onode_meta_lat,
  l_bluestore_read_wait_aio_lat,
  l_bluestore_compress_lat,
  l_bluestore_decompress_lat,
  l_bluestore_csum_lat,
  l_bluestore_compress_success_count,
  l_bluestore_compress_rejected_count,
  l_bluestore_write_pad_bytes,
  l_bluestore_deferred_write_ops,
  l_bluestore_deferred_write_bytes,
  l_bluestore_write_penalty_read_ops,
  l_bluestore_allocated,
  l_bluestore_stored,
  l_bluestore_compressed,
  l_bluestore_compressed_allocated,
  l_bluestore_compressed_original,
  l_bluestore_onodes,
  l_bluestore_onode_hits,
  l_bluestore_onode_misses,
  l_bluestore_onode_shard_hits,
  l_bluestore_onode_shard_misses,
  l_bluestore_extents,
  l_bluestore_blobs,
  l_bluestore_buffers,
  l_bluestore_buffer_bytes,
  l_bluestore_buffer_hit_bytes,
  l_bluestore_buffer_miss_bytes,
  l_bluestore_write_big,
  l_bluestore_write_big_bytes,
  l_bluestore_write_big_blobs,
  l_bluestore_write_small,
  l_bluestore_write_small_bytes,
  l_bluestore_write_small_unused,
  l_bluestore_write_small_deferred,
  l_bluestore_write_small_pre_read,
  l_bluestore_write_small_new,
  l_bluestore_txc,
  l_bluestore_onode_reshard,
  l_bluestore_blob_split,
  l_bluestore_extent_compress,
  l_bluestore_gc_merged,
  l_bluestore_read_eio,
  l_bluestore_last
};

class BlueStore : public ObjectStore,
		  public md_config_obs_t {
  // -----------------------------------------------------
  // types
public:
  // config observer
  const char** get_tracked_conf_keys() const override;
  void handle_conf_change(const struct md_config_t *conf,
                                  const std::set<std::string> &changed) override;

  void _set_csum();
  void _set_compression();
  void _set_throttle_params();
  int _set_cache_sizes();

  class TransContext;

  typedef map<uint64_t, bufferlist> ready_regions_t;

  struct BufferSpace;
  struct Collection;
  typedef boost::intrusive_ptr<Collection> CollectionRef;

  struct AioContext {
    virtual void aio_finish(BlueStore *store) = 0;
    virtual ~AioContext() {}
  };

  /// cached buffer
  struct Buffer {
    MEMPOOL_CLASS_HELPERS();

    enum {
      STATE_EMPTY,     ///< empty buffer -- used for cache history
      STATE_CLEAN,     ///< clean data that is up to date
      STATE_WRITING,   ///< data that is being written (io not yet complete)
    };
    static const char *get_state_name(int s) {
      switch (s) {
      case STATE_EMPTY: return "empty";
      case STATE_CLEAN: return "clean";
      case STATE_WRITING: return "writing";
      default: return "???";
      }
    }
    enum {
      FLAG_NOCACHE = 1,  ///< trim when done WRITING (do not become CLEAN)
      // NOTE: fix operator<< when you define a second flag
    };
    static const char *get_flag_name(int s) {
      switch (s) {
      case FLAG_NOCACHE: return "nocache";
      default: return "???";
      }
    }

    BufferSpace *space;
    uint16_t state;             ///< STATE_*
    uint16_t cache_private = 0; ///< opaque (to us) value used by Cache impl
    uint32_t flags;             ///< FLAG_*
    uint64_t seq;
    uint32_t offset, length;
    bufferlist data;

    boost::intrusive::list_member_hook<> lru_item;
    boost::intrusive::list_member_hook<> state_item;

    Buffer(BufferSpace *space, unsigned s, uint64_t q, uint32_t o, uint32_t l,
	   unsigned f = 0)
      : space(space), state(s), flags(f), seq(q), offset(o), length(l) {}
    Buffer(BufferSpace *space, unsigned s, uint64_t q, uint32_t o, bufferlist& b,
	   unsigned f = 0)
      : space(space), state(s), flags(f), seq(q), offset(o),
	length(b.length()), data(b) {}

    bool is_empty() const {
      return state == STATE_EMPTY;
    }
    bool is_clean() const {
      return state == STATE_CLEAN;
    }
    bool is_writing() const {
      return state == STATE_WRITING;
    }

    uint32_t end() const {
      return offset + length;
    }

    void truncate(uint32_t newlen) {
      assert(newlen < length);
      if (data.length()) {
	bufferlist t;
	t.substr_of(data, 0, newlen);
	data.claim(t);
      }
      length = newlen;
    }
    void maybe_rebuild() {
      if (data.length() &&
	  (data.get_num_buffers() > 1 ||
	   data.front().wasted() > data.length() / MAX_BUFFER_SLOP_RATIO_DEN)) {
	data.rebuild();
      }
    }

    void dump(Formatter *f) const {
      f->dump_string("state", get_state_name(state));
      f->dump_unsigned("seq", seq);
      f->dump_unsigned("offset", offset);
      f->dump_unsigned("length", length);
      f->dump_unsigned("data_length", data.length());
    }
  };

  struct Cache;

  /// map logical extent range (object) onto buffers
  struct BufferSpace {
    typedef boost::intrusive::list<
      Buffer,
      boost::intrusive::member_hook<
        Buffer,
	boost::intrusive::list_member_hook<>,
	&Buffer::state_item> > state_list_t;

    mempool::bluestore_cache_other::map<uint32_t, std::unique_ptr<Buffer>>
      buffer_map;

    // we use a bare intrusive list here instead of std::map because
    // it uses less memory and we expect this to be very small (very
    // few IOs in flight to the same Blob at the same time).
    state_list_t writing;   ///< writing buffers, sorted by seq, ascending

    ~BufferSpace() {
      assert(buffer_map.empty());
      assert(writing.empty());
    }

    void _add_buffer(Cache* cache, Buffer *b, int level, Buffer *near) {
      cache->_audit("_add_buffer start");
      buffer_map[b->offset].reset(b);
      if (b->is_writing()) {
	b->data.reassign_to_mempool(mempool::mempool_bluestore_writing);
        if (writing.empty() || writing.rbegin()->seq <= b->seq) {
          writing.push_back(*b);
        } else {
          auto it = writing.begin();
          while (it->seq < b->seq) {
            ++it;
          }

          assert(it->seq >= b->seq);
          // note that this will insert b before it
          // hence the order is maintained
          writing.insert(it, *b);
        }
      } else {
	b->data.reassign_to_mempool(mempool::mempool_bluestore_cache_data);
	cache->_add_buffer(b, level, near);
      }
      cache->_audit("_add_buffer end");
    }
    void _rm_buffer(Cache* cache, Buffer *b) {
      _rm_buffer(cache, buffer_map.find(b->offset));
    }
    void _rm_buffer(Cache* cache,
		    map<uint32_t, std::unique_ptr<Buffer>>::iterator p) {
      assert(p != buffer_map.end());
      cache->_audit("_rm_buffer start");
      if (p->second->is_writing()) {
        writing.erase(writing.iterator_to(*p->second));
      } else {
	cache->_rm_buffer(p->second.get());
      }
      buffer_map.erase(p);
      cache->_audit("_rm_buffer end");
    }

    map<uint32_t,std::unique_ptr<Buffer>>::iterator _data_lower_bound(
      uint32_t offset) {
      auto i = buffer_map.lower_bound(offset);
      if (i != buffer_map.begin()) {
	--i;
	if (i->first + i->second->length <= offset)
	  ++i;
      }
      return i;
    }

    // must be called under protection of the Cache lock
    void _clear(Cache* cache);

    // return value is the highest cache_private of a trimmed buffer, or 0.
    int discard(Cache* cache, uint32_t offset, uint32_t length) {
      std::lock_guard<std::recursive_mutex> l(cache->lock);
      return _discard(cache, offset, length);
    }
    int _discard(Cache* cache, uint32_t offset, uint32_t length);

    void write(Cache* cache, uint64_t seq, uint32_t offset, bufferlist& bl,
	       unsigned flags) {
      std::lock_guard<std::recursive_mutex> l(cache->lock);
      Buffer *b = new Buffer(this, Buffer::STATE_WRITING, seq, offset, bl,
			     flags);
      b->cache_private = _discard(cache, offset, bl.length());
      _add_buffer(cache, b, (flags & Buffer::FLAG_NOCACHE) ? 0 : 1, nullptr);
    }
    void finish_write(Cache* cache, uint64_t seq);
    void did_read(Cache* cache, uint32_t offset, bufferlist& bl) {
      std::lock_guard<std::recursive_mutex> l(cache->lock);
      Buffer *b = new Buffer(this, Buffer::STATE_CLEAN, 0, offset, bl);
      b->cache_private = _discard(cache, offset, bl.length());
      _add_buffer(cache, b, 1, nullptr);
    }

    void read(Cache* cache, uint32_t offset, uint32_t length,
	      BlueStore::ready_regions_t& res,
	      interval_set<uint32_t>& res_intervals);

    void truncate(Cache* cache, uint32_t offset) {
      discard(cache, offset, (uint32_t)-1 - offset);
    }

    void split(Cache* cache, size_t pos, BufferSpace &r);

    void dump(Cache* cache, Formatter *f) const {
      std::lock_guard<std::recursive_mutex> l(cache->lock);
      f->open_array_section("buffers");
      for (auto& i : buffer_map) {
	f->open_object_section("buffer");
	assert(i.first == i.second->offset);
	i.second->dump(f);
	f->close_section();
      }
      f->close_section();
    }
  };

  struct SharedBlobSet;

  /// in-memory shared blob state (incl cached buffers)
  struct SharedBlob {
    MEMPOOL_CLASS_HELPERS();

    std::atomic_int nref = {0}; ///< reference count
    bool loaded = false;

    CollectionRef coll;
    union {
      uint64_t sbid_unloaded;              ///< sbid if persistent isn't loaded
      bluestore_shared_blob_t *persistent; ///< persistent part of the shared blob if any
    };
    BufferSpace bc;             ///< buffer cache

    SharedBlob(Collection *_coll) : coll(_coll), sbid_unloaded(0) {
      if (get_cache()) {
	get_cache()->add_blob();
      }
    }
    SharedBlob(uint64_t i, Collection *_coll);
    ~SharedBlob();

    uint64_t get_sbid() const {
      return loaded ? persistent->sbid : sbid_unloaded;
    }

    friend void intrusive_ptr_add_ref(SharedBlob *b) { b->get(); }
    friend void intrusive_ptr_release(SharedBlob *b) { b->put(); }

    friend ostream& operator<<(ostream& out, const SharedBlob& sb);

    void get() {
      ++nref;
    }
    void put();

    /// get logical references
    void get_ref(uint64_t offset, uint32_t length);

    /// put logical references, and get back any released extents
    void put_ref(uint64_t offset, uint32_t length,
		 PExtentVector *r, set<SharedBlob*> *maybe_unshared_blobs);

    friend bool operator==(const SharedBlob &l, const SharedBlob &r) {
      return l.get_sbid() == r.get_sbid();
    }
    inline Cache* get_cache() {
      return coll ? coll->cache : nullptr;
    }
    inline SharedBlobSet* get_parent() {
      return coll ? &(coll->shared_blob_set) : nullptr;
    }
    inline bool is_loaded() const {
      return loaded;
    }

  };
  typedef boost::intrusive_ptr<SharedBlob> SharedBlobRef;

  /// a lookup table of SharedBlobs
  struct SharedBlobSet {
    std::mutex lock;   ///< protect lookup, insertion, removal

    // we use a bare pointer because we don't want to affect the ref
    // count
    mempool::bluestore_cache_other::unordered_map<uint64_t,SharedBlob*> sb_map;

    SharedBlobRef lookup(uint64_t sbid) {
      std::lock_guard<std::mutex> l(lock);
      auto p = sb_map.find(sbid);
      if (p == sb_map.end()) {
        return nullptr;
      }
      return p->second;
    }

    void add(Collection* coll, SharedBlob *sb) {
      std::lock_guard<std::mutex> l(lock);
      sb_map[sb->get_sbid()] = sb;
      sb->coll = coll;
    }

    bool try_remove(SharedBlob *sb) {
      std::lock_guard<std::mutex> l(lock);
      if (sb->nref == 0) {
	assert(sb->get_parent() == this);
	sb_map.erase(sb->get_sbid());
	return true;
      }
      return false;
    }

    void remove(SharedBlob *sb) {
      std::lock_guard<std::mutex> l(lock);
      assert(sb->get_parent() == this);
      sb_map.erase(sb->get_sbid());
    }

    bool empty() {
      std::lock_guard<std::mutex> l(lock);
      return sb_map.empty();
    }

    void dump(CephContext *cct, int lvl);
  };

//#define CACHE_BLOB_BL  // not sure if this is a win yet or not... :/

  /// in-memory blob metadata and associated cached buffers (if any)
  struct Blob {
    MEMPOOL_CLASS_HELPERS();

    std::atomic_int nref = {0};     ///< reference count
    int16_t id = -1;                ///< id, for spanning blobs only, >= 0
    int16_t last_encoded_id = -1;   ///< (ephemeral) used during encoding only
    SharedBlobRef shared_blob;      ///< shared blob state (if any)

  private:
    mutable bluestore_blob_t blob;  ///< decoded blob metadata
#ifdef CACHE_BLOB_BL
    mutable bufferlist blob_bl;     ///< cached encoded blob, blob is dirty if empty
#endif
    /// refs from this shard.  ephemeral if id<0, persisted if spanning.
    bluestore_blob_use_tracker_t used_in_blob;

  public:

    friend void intrusive_ptr_add_ref(Blob *b) { b->get(); }
    friend void intrusive_ptr_release(Blob *b) { b->put(); }

    friend ostream& operator<<(ostream& out, const Blob &b);

    const bluestore_blob_use_tracker_t& get_blob_use_tracker() const {
      return used_in_blob;
    }
    bool is_referenced() const {
      return used_in_blob.is_not_empty();
    }
    uint32_t get_referenced_bytes() const {
      return used_in_blob.get_referenced_bytes();
    }

    bool is_spanning() const {
      return id >= 0;
    }

    bool can_split() const {
      std::lock_guard<std::recursive_mutex> l(shared_blob->get_cache()->lock);
      // splitting a BufferSpace writing list is too hard; don't try.
      return shared_blob->bc.writing.empty() &&
             used_in_blob.can_split() &&
             get_blob().can_split();
    }

    bool can_split_at(uint32_t blob_offset) const {
      return used_in_blob.can_split_at(blob_offset) &&
             get_blob().can_split_at(blob_offset);
    }

    bool can_reuse_blob(uint32_t min_alloc_size,
			uint32_t target_blob_size,
			uint32_t b_offset,
			uint32_t *length0);

    void dup(Blob& o) {
      o.shared_blob = shared_blob;
      o.blob = blob;
#ifdef CACHE_BLOB_BL
      o.blob_bl = blob_bl;
#endif
    }

    inline const bluestore_blob_t& get_blob() const {
      return blob;
    }
    inline bluestore_blob_t& dirty_blob() {
#ifdef CACHE_BLOB_BL
      blob_bl.clear();
#endif
      return blob;
    }

    /// discard buffers for unallocated regions
    void discard_unallocated(Collection *coll);

    /// get logical references
    void get_ref(Collection *coll, uint32_t offset, uint32_t length);
    /// put logical references, and get back any released extents
    bool put_ref(Collection *coll, uint32_t offset, uint32_t length,
		 PExtentVector *r);

    /// split the blob
    void split(Collection *coll, uint32_t blob_offset, Blob *o);

    void get() {
      ++nref;
    }
    void put() {
      if (--nref == 0)
	delete this;
    }


#ifdef CACHE_BLOB_BL
    void _encode() const {
      if (blob_bl.length() == 0 ) {
	::encode(blob, blob_bl);
      } else {
	assert(blob_bl.length());
      }
    }
    void bound_encode(
      size_t& p,
      bool include_ref_map) const {
      _encode();
      p += blob_bl.length();
      if (include_ref_map) {
	used_in_blob.bound_encode(p);
      }
    }
    void encode(
      bufferlist::contiguous_appender& p,
      bool include_ref_map) const {
      _encode();
      p.append(blob_bl);
      if (include_ref_map) {
	used_in_blob.encode(p);
      }
    }
    void decode(
      Collection */*coll*/,
      bufferptr::iterator& p,
      bool include_ref_map) {
      const char *start = p.get_pos();
      denc(blob, p);
      const char *end = p.get_pos();
      blob_bl.clear();
      blob_bl.append(start, end - start);
      if (include_ref_map) {
	used_in_blob.decode(p);
      }
    }
#else
    void bound_encode(
      size_t& p,
      uint64_t struct_v,
      uint64_t sbid,
      bool include_ref_map) const {
      denc(blob, p, struct_v);
      if (blob.is_shared()) {
        denc(sbid, p);
      }
      if (include_ref_map) {
	used_in_blob.bound_encode(p);
      }
    }
    void encode(
      bufferlist::contiguous_appender& p,
      uint64_t struct_v,
      uint64_t sbid,
      bool include_ref_map) const {
      denc(blob, p, struct_v);
      if (blob.is_shared()) {
        denc(sbid, p);
      }
      if (include_ref_map) {
	used_in_blob.encode(p);
      }
    }
    void decode(
      Collection *coll,
      bufferptr::iterator& p,
      uint64_t struct_v,
      uint64_t* sbid,
      bool include_ref_map);
#endif
  };
  typedef boost::intrusive_ptr<Blob> BlobRef;
  typedef mempool::bluestore_cache_other::map<int,BlobRef> blob_map_t;

  /// a logical extent, pointing to (some portion of) a blob
  typedef boost::intrusive::set_base_hook<boost::intrusive::optimize_size<true> > ExtentBase; //making an alias to avoid build warnings
  struct Extent : public ExtentBase {
    MEMPOOL_CLASS_HELPERS();

    uint32_t logical_offset = 0;      ///< logical offset
    uint32_t blob_offset = 0;         ///< blob offset
    uint32_t length = 0;              ///< length
    BlobRef  blob;                    ///< the blob with our data

    /// ctor for lookup only
    explicit Extent(uint32_t lo) : ExtentBase(), logical_offset(lo) { }
    /// ctor for delayed initialization (see decode_some())
    explicit Extent() : ExtentBase() {
    }
    /// ctor for general usage
    Extent(uint32_t lo, uint32_t o, uint32_t l, BlobRef& b)
      : ExtentBase(),
        logical_offset(lo), blob_offset(o), length(l) {
      assign_blob(b);
    }
    ~Extent() {
      if (blob) {
	blob->shared_blob->get_cache()->rm_extent();
      }
    }

    void assign_blob(const BlobRef& b) {
      assert(!blob);
      blob = b;
      blob->shared_blob->get_cache()->add_extent();
    }

    // comparators for intrusive_set
    friend bool operator<(const Extent &a, const Extent &b) {
      return a.logical_offset < b.logical_offset;
    }
    friend bool operator>(const Extent &a, const Extent &b) {
      return a.logical_offset > b.logical_offset;
    }
    friend bool operator==(const Extent &a, const Extent &b) {
      return a.logical_offset == b.logical_offset;
    }

    uint32_t blob_start() const {
      return logical_offset - blob_offset;
    }

    uint32_t blob_end() const {
      return blob_start() + blob->get_blob().get_logical_length();
    }

    uint32_t logical_end() const {
      return logical_offset + length;
    }

    // return true if any piece of the blob is out of
    // the given range [o, o + l].
    bool blob_escapes_range(uint32_t o, uint32_t l) const {
      return blob_start() < o || blob_end() > o + l;
    }
  };
  typedef boost::intrusive::set<Extent> extent_map_t;


  friend ostream& operator<<(ostream& out, const Extent& e);

  struct OldExtent {
    boost::intrusive::list_member_hook<> old_extent_item;
    Extent e;
    PExtentVector r;
    bool blob_empty; // flag to track the last removed extent that makes blob
                     // empty - required to update compression stat properly
    OldExtent(uint32_t lo, uint32_t o, uint32_t l, BlobRef& b)
      : e(lo, o, l, b), blob_empty(false) {
    }
    static OldExtent* create(CollectionRef c,
                             uint32_t lo,
			     uint32_t o,
			     uint32_t l,
			     BlobRef& b);
  };
  typedef boost::intrusive::list<
      OldExtent,
      boost::intrusive::member_hook<
        OldExtent,
    boost::intrusive::list_member_hook<>,
    &OldExtent::old_extent_item> > old_extent_map_t;

  struct Onode;

  /// a sharded extent map, mapping offsets to lextents to blobs
  struct ExtentMap {
    Onode *onode;
    extent_map_t extent_map;        ///< map of Extents to Blobs
    blob_map_t spanning_blob_map;   ///< blobs that span shards

    struct Shard {
      bluestore_onode_t::shard_info *shard_info = nullptr;
      unsigned extents = 0;  ///< count extents in this shard
      bool loaded = false;   ///< true if shard is loaded
      bool dirty = false;    ///< true if shard is dirty and needs reencoding
    };
    mempool::bluestore_cache_other::vector<Shard> shards;    ///< shards

    bufferlist inline_bl;    ///< cached encoded map, if unsharded; empty=>dirty

    uint32_t needs_reshard_begin = 0;
    uint32_t needs_reshard_end = 0;

    bool needs_reshard() const {
      return needs_reshard_end > needs_reshard_begin;
    }
    void clear_needs_reshard() {
      needs_reshard_begin = needs_reshard_end = 0;
    }
    void request_reshard(uint32_t begin, uint32_t end) {
      if (begin < needs_reshard_begin) {
	needs_reshard_begin = begin;
      }
      if (end > needs_reshard_end) {
	needs_reshard_end = end;
      }
    }

    struct DeleteDisposer {
      void operator()(Extent *e) { delete e; }
    };

    ExtentMap(Onode *o);
    ~ExtentMap() {
      extent_map.clear_and_dispose(DeleteDisposer());
    }

    void clear() {
      extent_map.clear_and_dispose(DeleteDisposer());
      shards.clear();
      inline_bl.clear();
      clear_needs_reshard();
    }

    bool encode_some(uint32_t offset, uint32_t length, bufferlist& bl,
		     unsigned *pn);
    unsigned decode_some(bufferlist& bl);

    void bound_encode_spanning_blobs(size_t& p);
    void encode_spanning_blobs(bufferlist::contiguous_appender& p);
    void decode_spanning_blobs(bufferptr::iterator& p);

    BlobRef get_spanning_blob(int id) {
      auto p = spanning_blob_map.find(id);
      assert(p != spanning_blob_map.end());
      return p->second;
    }

    void update(KeyValueDB::Transaction t, bool force);
    decltype(BlueStore::Blob::id) allocate_spanning_blob_id();
    void reshard(
      KeyValueDB *db,
      KeyValueDB::Transaction t);

    /// initialize Shards from the onode
    void init_shards(bool loaded, bool dirty);

    /// return index of shard containing offset
    /// or -1 if not found
    int seek_shard(uint32_t offset) {
      size_t end = shards.size();
      size_t mid, left = 0;
      size_t right = end; // one passed the right end

      while (left < right) {
        mid = left + (right - left) / 2;
        if (offset >= shards[mid].shard_info->offset) {
          size_t next = mid + 1;
          if (next >= end || offset < shards[next].shard_info->offset)
            return mid;
          //continue to search forwards
          left = next;
        } else {
          //continue to search backwards
          right = mid;
        }
      }

      return -1; // not found
    }

    /// check if a range spans a shard
    bool spans_shard(uint32_t offset, uint32_t length) {
      if (shards.empty()) {
	return false;
      }
      int s = seek_shard(offset);
      assert(s >= 0);
      if (s == (int)shards.size() - 1) {
	return false; // last shard
      }
      if (offset + length <= shards[s+1].shard_info->offset) {
	return false;
      }
      return true;
    }

    /// ensure that a range of the map is loaded
    void fault_range(KeyValueDB *db,
		     uint32_t offset, uint32_t length);

    /// ensure a range of the map is marked dirty
    void dirty_range(uint32_t offset, uint32_t length);

    /// for seek_lextent test
    extent_map_t::iterator find(uint64_t offset);

    /// seek to the first lextent including or after offset
    extent_map_t::iterator seek_lextent(uint64_t offset);
    extent_map_t::const_iterator seek_lextent(uint64_t offset) const;

    /// add a new Extent
    void add(uint32_t lo, uint32_t o, uint32_t l, BlobRef& b) {
      extent_map.insert(*new Extent(lo, o, l, b));
    }

    /// remove (and delete) an Extent
    void rm(extent_map_t::iterator p) {
      extent_map.erase_and_dispose(p, DeleteDisposer());
    }

    bool has_any_lextents(uint64_t offset, uint64_t length);

    /// consolidate adjacent lextents in extent_map
    int compress_extent_map(uint64_t offset, uint64_t length);

    /// punch a logical hole.  add lextents to deref to target list.
    void punch_hole(CollectionRef &c,
		    uint64_t offset, uint64_t length,
		    old_extent_map_t *old_extents);

    /// put new lextent into lextent_map overwriting existing ones if
    /// any and update references accordingly
    Extent *set_lextent(CollectionRef &c,
			uint64_t logical_offset,
			uint64_t offset, uint64_t length,
                        BlobRef b,
			old_extent_map_t *old_extents);

    /// split a blob (and referring extents)
    BlobRef split_blob(BlobRef lb, uint32_t blob_offset, uint32_t pos);
  };

  /// Compressed Blob Garbage collector
  /*
  The primary idea of the collector is to estimate a difference between
  allocation units(AU) currently present for compressed blobs and new AUs
  required to store that data uncompressed. 
  Estimation is performed for protrusive extents within a logical range
  determined by a concatenation of old_extents collection and specific(current)
  write request.
  The root cause for old_extents use is the need to handle blob ref counts
  properly. Old extents still hold blob refs and hence we need to traverse
  the collection to determine if blob to be released.
  Protrusive extents are extents that fit into the blob set in action
  (ones that are below the logical range from above) but not removed totally
  due to the current write. 
  E.g. for
  extent1 <loffs = 100, boffs = 100, len  = 100> -> 
    blob1<compressed, len_on_disk=4096, logical_len=8192>
  extent2 <loffs = 200, boffs = 200, len  = 100> ->
    blob2<raw, len_on_disk=4096, llen=4096>
  extent3 <loffs = 300, boffs = 300, len  = 100> ->
    blob1<compressed, len_on_disk=4096, llen=8192>
  extent4 <loffs = 4096, boffs = 0, len  = 100>  ->
    blob3<raw, len_on_disk=4096, llen=4096>
  write(300~100)
  protrusive extents are within the following ranges <0~300, 400~8192-400>
  In this case existing AUs that might be removed due to GC (i.e. blob1) 
  use 2x4K bytes.
  And new AUs expected after GC = 0 since extent1 to be merged into blob2.
  Hence we should do a collect.
  */
  class GarbageCollector
  {
  public:
    /// return amount of allocation units that might be saved due to GC
    int64_t estimate(
      uint64_t offset,
      uint64_t length,
      const ExtentMap& extent_map,
      const old_extent_map_t& old_extents,
      uint64_t min_alloc_size);

    /// return a collection of extents to perform GC on
    const vector<AllocExtent>& get_extents_to_collect() const {
      return extents_to_collect;
    }
    GarbageCollector(CephContext* _cct) : cct(_cct) {}

  private:
    struct BlobInfo {
      uint64_t referenced_bytes = 0;    ///< amount of bytes referenced in blob
      int64_t expected_allocations = 0; ///< new alloc units required 
                                        ///< in case of gc fulfilled
      bool collect_candidate = false;   ///< indicate if blob has any extents 
                                        ///< eligible for GC.
      extent_map_t::const_iterator first_lextent; ///< points to the first 
                                                  ///< lextent referring to 
                                                  ///< the blob if any.
                                                  ///< collect_candidate flag 
                                                  ///< determines the validity
      extent_map_t::const_iterator last_lextent;  ///< points to the last 
                                                  ///< lextent referring to 
                                                  ///< the blob if any.

      BlobInfo(uint64_t ref_bytes) :
        referenced_bytes(ref_bytes) {
      }
    };
    CephContext* cct;
    map<Blob*, BlobInfo> affected_blobs; ///< compressed blobs and their ref_map
                                         ///< copies that are affected by the
                                         ///< specific write

    vector<AllocExtent> extents_to_collect; ///< protrusive extents that should
                                            ///< be collected if GC takes place

    boost::optional<uint64_t > used_alloc_unit; ///< last processed allocation
                                                ///<  unit when traversing 
                                                ///< protrusive extents. 
                                                ///< Other extents mapped to
                                                ///< this AU to be ignored 
                                                ///< (except the case where
                                                ///< uncompressed extent follows
                                                ///< compressed one - see below).
    BlobInfo* blob_info_counted = nullptr; ///< set if previous allocation unit
                                           ///< caused expected_allocations
					   ///< counter increment at this blob.
                                           ///< if uncompressed extent follows 
                                           ///< a decrement for the 
                                	   ///< expected_allocations counter 
                                           ///< is needed
    int64_t expected_allocations = 0;      ///< new alloc units required in case
                                           ///< of gc fulfilled
    int64_t expected_for_release = 0;      ///< alloc units currently used by
                                           ///< compressed blobs that might
                                           ///< gone after GC
    uint64_t gc_start_offset;              ///starting offset for GC
    uint64_t gc_end_offset;                ///ending offset for GC

  protected:
    void process_protrusive_extents(const BlueStore::ExtentMap& extent_map, 
				    uint64_t start_offset,
				    uint64_t end_offset,
				    uint64_t start_touch_offset,
				    uint64_t end_touch_offset,
				    uint64_t min_alloc_size);
  };

  struct OnodeSpace;

  /// an in-memory object
  struct Onode {
    MEMPOOL_CLASS_HELPERS();

    std::atomic_int nref;  ///< reference count
    Collection *c;

    ghobject_t oid;

    /// key under PREFIX_OBJ where we are stored
    mempool::bluestore_cache_other::string key;

    boost::intrusive::list_member_hook<> lru_item;

    bluestore_onode_t onode;  ///< metadata stored as value in kv store
    bool exists;              ///< true if object logically exists

    ExtentMap extent_map;

    // track txc's that have not been committed to kv store (and whose
    // effects cannot be read via the kvdb read methods)
    std::atomic<int> flushing_count = {0};
    std::mutex flush_lock;  ///< protect flush_txns
    std::condition_variable flush_cond;   ///< wait here for uncommitted txns

    Onode(Collection *c, const ghobject_t& o,
	  const mempool::bluestore_cache_other::string& k)
      : nref(0),
	c(c),
	oid(o),
	key(k),
	exists(false),
	extent_map(this) {
    }

    void flush();
    void get() {
      ++nref;
    }
    void put() {
      if (--nref == 0)
	delete this;
    }
  };
  typedef boost::intrusive_ptr<Onode> OnodeRef;


  /// a cache (shard) of onodes and buffers
  struct Cache {
    CephContext* cct;
    PerfCounters *logger;
    std::recursive_mutex lock;          ///< protect lru and other structures

    std::atomic<uint64_t> num_extents = {0};
    std::atomic<uint64_t> num_blobs = {0};

    static Cache *create(CephContext* cct, string type, PerfCounters *logger);

    Cache(CephContext* cct) : cct(cct), logger(nullptr) {}
    virtual ~Cache() {}

    virtual void _add_onode(OnodeRef& o, int level) = 0;
    virtual void _rm_onode(OnodeRef& o) = 0;
    virtual void _touch_onode(OnodeRef& o) = 0;

    virtual void _add_buffer(Buffer *b, int level, Buffer *near) = 0;
    virtual void _rm_buffer(Buffer *b) = 0;
    virtual void _move_buffer(Cache *src, Buffer *b) = 0;
    virtual void _adjust_buffer_size(Buffer *b, int64_t delta) = 0;
    virtual void _touch_buffer(Buffer *b) = 0;

    virtual uint64_t _get_num_onodes() = 0;
    virtual uint64_t _get_buffer_bytes() = 0;

    void add_extent() {
      ++num_extents;
    }
    void rm_extent() {
      --num_extents;
    }

    void add_blob() {
      ++num_blobs;
    }
    void rm_blob() {
      --num_blobs;
    }

    void trim(uint64_t target_bytes,
	      float target_meta_ratio,
	      float target_data_ratio,
	      float bytes_per_onode);

    void trim_all();

    virtual void _trim(uint64_t onode_max, uint64_t buffer_max) = 0;

    virtual void add_stats(uint64_t *onodes, uint64_t *extents,
			   uint64_t *blobs,
			   uint64_t *buffers,
			   uint64_t *bytes) = 0;

    bool empty() {
      std::lock_guard<std::recursive_mutex> l(lock);
      return _get_num_onodes() == 0 && _get_buffer_bytes() == 0;
    }

#ifdef DEBUG_CACHE
    virtual void _audit(const char *s) = 0;
#else
    void _audit(const char *s) { /* no-op */ }
#endif
  };

  /// simple LRU cache for onodes and buffers
  struct LRUCache : public Cache {
  private:
    typedef boost::intrusive::list<
      Onode,
      boost::intrusive::member_hook<
        Onode,
	boost::intrusive::list_member_hook<>,
	&Onode::lru_item> > onode_lru_list_t;
    typedef boost::intrusive::list<
      Buffer,
      boost::intrusive::member_hook<
	Buffer,
	boost::intrusive::list_member_hook<>,
	&Buffer::lru_item> > buffer_lru_list_t;

    onode_lru_list_t onode_lru;

    buffer_lru_list_t buffer_lru;
    uint64_t buffer_size = 0;

  public:
    LRUCache(CephContext* cct) : Cache(cct) {}
    uint64_t _get_num_onodes() override {
      return onode_lru.size();
    }
    void _add_onode(OnodeRef& o, int level) override {
      if (level > 0)
	onode_lru.push_front(*o);
      else
	onode_lru.push_back(*o);
    }
    void _rm_onode(OnodeRef& o) override {
      auto q = onode_lru.iterator_to(*o);
      onode_lru.erase(q);
    }
    void _touch_onode(OnodeRef& o) override;

    uint64_t _get_buffer_bytes() override {
      return buffer_size;
    }
    void _add_buffer(Buffer *b, int level, Buffer *near) override {
      if (near) {
	auto q = buffer_lru.iterator_to(*near);
	buffer_lru.insert(q, *b);
      } else if (level > 0) {
	buffer_lru.push_front(*b);
      } else {
	buffer_lru.push_back(*b);
      }
      buffer_size += b->length;
    }
    void _rm_buffer(Buffer *b) override {
      assert(buffer_size >= b->length);
      buffer_size -= b->length;
      auto q = buffer_lru.iterator_to(*b);
      buffer_lru.erase(q);
    }
    void _move_buffer(Cache *src, Buffer *b) override {
      src->_rm_buffer(b);
      _add_buffer(b, 0, nullptr);
    }
    void _adjust_buffer_size(Buffer *b, int64_t delta) override {
      assert((int64_t)buffer_size + delta >= 0);
      buffer_size += delta;
    }
    void _touch_buffer(Buffer *b) override {
      auto p = buffer_lru.iterator_to(*b);
      buffer_lru.erase(p);
      buffer_lru.push_front(*b);
      _audit("_touch_buffer end");
    }

    void _trim(uint64_t onode_max, uint64_t buffer_max) override;

    void add_stats(uint64_t *onodes, uint64_t *extents,
		   uint64_t *blobs,
		   uint64_t *buffers,
		   uint64_t *bytes) override {
      std::lock_guard<std::recursive_mutex> l(lock);
      *onodes += onode_lru.size();
      *extents += num_extents;
      *blobs += num_blobs;
      *buffers += buffer_lru.size();
      *bytes += buffer_size;
    }

#ifdef DEBUG_CACHE
    void _audit(const char *s) override;
#endif
  };

  // 2Q cache for buffers, LRU for onodes
  struct TwoQCache : public Cache {
  private:
    // stick with LRU for onodes for now (fixme?)
    typedef boost::intrusive::list<
      Onode,
      boost::intrusive::member_hook<
        Onode,
	boost::intrusive::list_member_hook<>,
	&Onode::lru_item> > onode_lru_list_t;
    typedef boost::intrusive::list<
      Buffer,
      boost::intrusive::member_hook<
	Buffer,
	boost::intrusive::list_member_hook<>,
	&Buffer::lru_item> > buffer_list_t;

    onode_lru_list_t onode_lru;

    buffer_list_t buffer_hot;      ///< "Am" hot buffers
    buffer_list_t buffer_warm_in;  ///< "A1in" newly warm buffers
    buffer_list_t buffer_warm_out; ///< "A1out" empty buffers we've evicted
    uint64_t buffer_bytes = 0;     ///< bytes

    enum {
      BUFFER_NEW = 0,
      BUFFER_WARM_IN,   ///< in buffer_warm_in
      BUFFER_WARM_OUT,  ///< in buffer_warm_out
      BUFFER_HOT,       ///< in buffer_hot
      BUFFER_TYPE_MAX
    };

    uint64_t buffer_list_bytes[BUFFER_TYPE_MAX] = {0}; ///< bytes per type

  public:
    TwoQCache(CephContext* cct) : Cache(cct) {}
    uint64_t _get_num_onodes() override {
      return onode_lru.size();
    }
    void _add_onode(OnodeRef& o, int level) override {
      if (level > 0)
	onode_lru.push_front(*o);
      else
	onode_lru.push_back(*o);
    }
    void _rm_onode(OnodeRef& o) override {
      auto q = onode_lru.iterator_to(*o);
      onode_lru.erase(q);
    }
    void _touch_onode(OnodeRef& o) override;

    uint64_t _get_buffer_bytes() override {
      return buffer_bytes;
    }
    void _add_buffer(Buffer *b, int level, Buffer *near) override;
    void _rm_buffer(Buffer *b) override;
    void _move_buffer(Cache *src, Buffer *b) override;
    void _adjust_buffer_size(Buffer *b, int64_t delta) override;
    void _touch_buffer(Buffer *b) override {
      switch (b->cache_private) {
      case BUFFER_WARM_IN:
	// do nothing (somewhat counter-intuitively!)
	break;
      case BUFFER_WARM_OUT:
	// move from warm_out to hot LRU
	assert(0 == "this happens via discard hint");
	break;
      case BUFFER_HOT:
	// move to front of hot LRU
	buffer_hot.erase(buffer_hot.iterator_to(*b));
	buffer_hot.push_front(*b);
	break;
      }
      _audit("_touch_buffer end");
    }

    void _trim(uint64_t onode_max, uint64_t buffer_max) override;

    void add_stats(uint64_t *onodes, uint64_t *extents,
		   uint64_t *blobs,
		   uint64_t *buffers,
		   uint64_t *bytes) override {
      std::lock_guard<std::recursive_mutex> l(lock);
      *onodes += onode_lru.size();
      *extents += num_extents;
      *blobs += num_blobs;
      *buffers += buffer_hot.size() + buffer_warm_in.size();
      *bytes += buffer_bytes;
    }

#ifdef DEBUG_CACHE
    void _audit(const char *s) override;
#endif
  };

  struct OnodeSpace {
  private:
    Cache *cache;

    /// forward lookups
    mempool::bluestore_cache_other::unordered_map<ghobject_t,OnodeRef> onode_map;

    friend class Collection; // for split_cache()

  public:
    OnodeSpace(Cache *c) : cache(c) {}
    ~OnodeSpace() {
      clear();
    }

    OnodeRef add(const ghobject_t& oid, OnodeRef o);
    OnodeRef lookup(const ghobject_t& o);
    void remove(const ghobject_t& oid) {
      onode_map.erase(oid);
    }
    void rename(OnodeRef& o, const ghobject_t& old_oid,
		const ghobject_t& new_oid,
		const mempool::bluestore_cache_other::string& new_okey);
    void clear();
    bool empty();

    void dump(CephContext *cct, int lvl);

    /// return true if f true for any item
    bool map_any(std::function<bool(OnodeRef)> f);
  };

  struct Collection : public CollectionImpl {
    BlueStore *store;
    Cache *cache;       ///< our cache shard
    coll_t cid;
    bluestore_cnode_t cnode;
    RWLock lock;

    bool exists;

    SharedBlobSet shared_blob_set;      ///< open SharedBlobs

    // cache onodes on a per-collection basis to avoid lock
    // contention.
    OnodeSpace onode_map;

    //pool options
    pool_opts_t pool_opts;

    OnodeRef get_onode(const ghobject_t& oid, bool create);

    // the terminology is confusing here, sorry!
    //
    //  blob_t     shared_blob_t
    //  !shared    unused                -> open
    //  shared     !loaded               -> open + shared
    //  shared     loaded                -> open + shared + loaded
    //
    // i.e.,
    //  open = SharedBlob is instantiated
    //  shared = blob_t shared flag is set; SharedBlob is hashed.
    //  loaded = SharedBlob::shared_blob_t is loaded from kv store
    void open_shared_blob(uint64_t sbid, BlobRef b);
    void load_shared_blob(SharedBlobRef sb);
    void make_blob_shared(uint64_t sbid, BlobRef b);
    uint64_t make_blob_unshared(SharedBlob *sb);

    BlobRef new_blob() {
      BlobRef b = new Blob();
      b->shared_blob = new SharedBlob(this);
      return b;
    }

    const coll_t &get_cid() override {
      return cid;
    }

    bool contains(const ghobject_t& oid) {
      if (cid.is_meta())
	return oid.hobj.pool == -1;
      spg_t spgid;
      if (cid.is_pg(&spgid))
	return
	  spgid.pgid.contains(cnode.bits, oid) &&
	  oid.shard_id == spgid.shard;
      return false;
    }

    void split_cache(Collection *dest);

    Collection(BlueStore *ns, Cache *ca, coll_t c);
  };

  class OmapIteratorImpl : public ObjectMap::ObjectMapIteratorImpl {
    CollectionRef c;
    OnodeRef o;
    KeyValueDB::Iterator it;
    string head, tail;
  public:
    OmapIteratorImpl(CollectionRef c, OnodeRef o, KeyValueDB::Iterator it);
    int seek_to_first() override;
    int upper_bound(const string &after) override;
    int lower_bound(const string &to) override;
    bool valid() override;
    int next(bool validate=true) override;
    string key() override;
    bufferlist value() override;
    int status() override {
      return 0;
    }
  };

  class OpSequencer;
  typedef boost::intrusive_ptr<OpSequencer> OpSequencerRef;

  struct volatile_statfs{
    enum {
      STATFS_ALLOCATED = 0,
      STATFS_STORED,
      STATFS_COMPRESSED_ORIGINAL,
      STATFS_COMPRESSED,
      STATFS_COMPRESSED_ALLOCATED,
      STATFS_LAST
    };
    int64_t values[STATFS_LAST];
    volatile_statfs() {
      memset(this, 0, sizeof(volatile_statfs));
    }
    void reset() {
      *this = volatile_statfs();
    }
    volatile_statfs& operator+=(const volatile_statfs& other) {
      for (size_t i = 0; i < STATFS_LAST; ++i) {
	values[i] += other.values[i];
      }
      return *this;
    }
    int64_t& allocated() {
      return values[STATFS_ALLOCATED];
    }
    int64_t& stored() {
      return values[STATFS_STORED];
    }
    int64_t& compressed_original() {
      return values[STATFS_COMPRESSED_ORIGINAL];
    }
    int64_t& compressed() {
      return values[STATFS_COMPRESSED];
    }
    int64_t& compressed_allocated() {
      return values[STATFS_COMPRESSED_ALLOCATED];
    }
    bool is_empty() {
      return values[STATFS_ALLOCATED] == 0 &&
	values[STATFS_STORED] == 0 &&
	values[STATFS_COMPRESSED] == 0 &&
	values[STATFS_COMPRESSED_ORIGINAL] == 0 &&
	values[STATFS_COMPRESSED_ALLOCATED] == 0;
    }
    void decode(bufferlist::iterator& it) {
      for (size_t i = 0; i < STATFS_LAST; i++) {
	::decode(values[i], it);
      }
    }

    void encode(bufferlist& bl) {
      for (size_t i = 0; i < STATFS_LAST; i++) {
	::encode(values[i], bl);
      }
    }
  };

  struct TransContext : public AioContext {
    MEMPOOL_CLASS_HELPERS();

    typedef enum {
      STATE_PREPARE,
      STATE_AIO_WAIT,
      STATE_IO_DONE,
      STATE_KV_QUEUED,     // queued for kv_sync_thread submission
      STATE_KV_SUBMITTED,  // submitted to kv; not yet synced
      STATE_KV_DONE,
      STATE_DEFERRED_QUEUED,    // in deferred_queue (pending or running)
      STATE_DEFERRED_CLEANUP,   // remove deferred kv record
      STATE_DEFERRED_DONE,
      STATE_FINISHING,
      STATE_DONE,
    } state_t;

    state_t state = STATE_PREPARE;

    const char *get_state_name() {
      switch (state) {
      case STATE_PREPARE: return "prepare";
      case STATE_AIO_WAIT: return "aio_wait";
      case STATE_IO_DONE: return "io_done";
      case STATE_KV_QUEUED: return "kv_queued";
      case STATE_KV_SUBMITTED: return "kv_submitted";
      case STATE_KV_DONE: return "kv_done";
      case STATE_DEFERRED_QUEUED: return "deferred_queued";
      case STATE_DEFERRED_CLEANUP: return "deferred_cleanup";
      case STATE_DEFERRED_DONE: return "deferred_done";
      case STATE_FINISHING: return "finishing";
      case STATE_DONE: return "done";
      }
      return "???";
    }

#if defined(WITH_LTTNG) && defined(WITH_EVENTTRACE)
    const char *get_state_latency_name(int state) {
      switch (state) {
      case l_bluestore_state_prepare_lat: return "prepare";
      case l_bluestore_state_aio_wait_lat: return "aio_wait";
      case l_bluestore_state_io_done_lat: return "io_done";
      case l_bluestore_state_kv_queued_lat: return "kv_queued";
      case l_bluestore_state_kv_committing_lat: return "kv_committing";
      case l_bluestore_state_kv_done_lat: return "kv_done";
      case l_bluestore_state_deferred_queued_lat: return "deferred_queued";
      case l_bluestore_state_deferred_cleanup_lat: return "deferred_cleanup";
      case l_bluestore_state_finishing_lat: return "finishing";
      case l_bluestore_state_done_lat: return "done";
      }
      return "???";
    }
#endif

    void log_state_latency(PerfCounters *logger, int state) {
      utime_t lat, now = ceph_clock_now();
      lat = now - last_stamp;
      logger->tinc(state, lat);
#if defined(WITH_LTTNG) && defined(WITH_EVENTTRACE)
      if (state >= l_bluestore_state_prepare_lat && state <= l_bluestore_state_done_lat) {
        double usecs = (now.to_nsec()-last_stamp.to_nsec())/1000;
        OID_ELAPSED("", usecs, get_state_latency_name(state));
      }
#endif
      last_stamp = now;
    }

    OpSequencerRef osr;
    boost::intrusive::list_member_hook<> sequencer_item;

    uint64_t bytes = 0, cost = 0;

    set<OnodeRef> onodes;     ///< these need to be updated/written
    set<OnodeRef> modified_objects;  ///< objects we modified (and need a ref)
    set<SharedBlobRef> shared_blobs;  ///< these need to be updated/written
    set<SharedBlobRef> shared_blobs_written; ///< update these on io completion

    KeyValueDB::Transaction t; ///< then we will commit this
    Context *oncommit = nullptr;         ///< signal on commit
    Context *onreadable = nullptr;       ///< signal on readable
    Context *onreadable_sync = nullptr;  ///< signal on readable
    list<Context*> oncommits;  ///< more commit completions
    list<CollectionRef> removed_collections; ///< colls we removed

    boost::intrusive::list_member_hook<> deferred_queue_item;
    bluestore_deferred_transaction_t *deferred_txn = nullptr; ///< if any

    interval_set<uint64_t> allocated, released;
    volatile_statfs statfs_delta;

    IOContext ioc;
    bool had_ios = false;  ///< true if we submitted IOs before our kv txn

    uint64_t seq = 0;
    utime_t start;
    utime_t last_stamp;

    uint64_t last_nid = 0;     ///< if non-zero, highest new nid we allocated
    uint64_t last_blobid = 0;  ///< if non-zero, highest new blobid we allocated

    explicit TransContext(CephContext* cct, OpSequencer *o)
      : osr(o),
	ioc(cct, this),
	start(ceph_clock_now()) {
      last_stamp = start;
    }
    ~TransContext() {
      delete deferred_txn;
    }

    void write_onode(OnodeRef &o) {
      onodes.insert(o);
    }
    void write_shared_blob(SharedBlobRef &sb) {
      shared_blobs.insert(sb);
    }
    void unshare_blob(SharedBlob *sb) {
      shared_blobs.erase(sb);
    }

    /// note we logically modified object (when onode itself is unmodified)
    void note_modified_object(OnodeRef &o) {
      // onode itself isn't written, though
      modified_objects.insert(o);
    }
    void removed(OnodeRef& o) {
      onodes.erase(o);
      modified_objects.erase(o);
    }

    void aio_finish(BlueStore *store) override {
      store->txc_aio_finish(this);
    }
  };

  typedef boost::intrusive::list<
    TransContext,
    boost::intrusive::member_hook<
      TransContext,
      boost::intrusive::list_member_hook<>,
      &TransContext::deferred_queue_item> > deferred_queue_t;

  struct DeferredBatch : public AioContext {
    OpSequencer *osr;
    struct deferred_io {
      bufferlist bl;    ///< data
      uint64_t seq;     ///< deferred transaction seq
    };
    map<uint64_t,deferred_io> iomap; ///< map of ios in this batch
    deferred_queue_t txcs;           ///< txcs in this batch
    IOContext ioc;                   ///< our aios
    /// bytes of pending io for each deferred seq (may be 0)
    map<uint64_t,int> seq_bytes;

    void _discard(CephContext *cct, uint64_t offset, uint64_t length);
    void _audit(CephContext *cct);

    DeferredBatch(CephContext *cct, OpSequencer *osr)
      : osr(osr), ioc(cct, this) {}

    /// prepare a write
    void prepare_write(CephContext *cct,
		       uint64_t seq, uint64_t offset, uint64_t length,
		       bufferlist::const_iterator& p);

    void aio_finish(BlueStore *store) override {
      store->_deferred_aio_finish(osr);
    }
  };

  class OpSequencer : public Sequencer_impl {
  public:
    std::mutex qlock;
    std::condition_variable qcond;
    typedef boost::intrusive::list<
      TransContext,
      boost::intrusive::member_hook<
        TransContext,
	boost::intrusive::list_member_hook<>,
	&TransContext::sequencer_item> > q_list_t;
    q_list_t q;  ///< transactions

    boost::intrusive::list_member_hook<> deferred_osr_queue_item;

    DeferredBatch *deferred_running = nullptr;
    DeferredBatch *deferred_pending = nullptr;

    Sequencer *parent;
    BlueStore *store;

    uint64_t last_seq = 0;

    std::atomic_int txc_with_unstable_io = {0};  ///< num txcs with unstable io

    std::atomic_int kv_committing_serially = {0};

    std::atomic_int kv_submitted_waiters = {0};

    std::atomic_bool registered = {true}; ///< registered in BlueStore's osr_set
    std::atomic_bool zombie = {false};    ///< owning Sequencer has gone away

    OpSequencer(CephContext* cct, BlueStore *store)
      : Sequencer_impl(cct),
	parent(NULL), store(store) {
      store->register_osr(this);
    }
    ~OpSequencer() override {
      assert(q.empty());
      _unregister();
    }

    void discard() override {
      // Note that we may have txc's in flight when the parent Sequencer
      // goes away.  Reflect this with zombie==registered==true and let
      // _osr_drain_all clean up later.
      assert(!zombie);
      zombie = true;
      parent = nullptr;
      bool empty;
      {
	std::lock_guard<std::mutex> l(qlock);
	empty = q.empty();
      }
      if (empty) {
	_unregister();
      }
    }

    void _unregister() {
      if (registered) {
	store->unregister_osr(this);
	registered = false;
      }
    }

    void queue_new(TransContext *txc) {
      std::lock_guard<std::mutex> l(qlock);
      txc->seq = ++last_seq;
      q.push_back(*txc);
    }

    void drain() {
      std::unique_lock<std::mutex> l(qlock);
      while (!q.empty())
	qcond.wait(l);
    }

    void drain_preceding(TransContext *txc) {
      std::unique_lock<std::mutex> l(qlock);
      while (!q.empty() && &q.front() != txc)
	qcond.wait(l);
    }

    bool _is_all_kv_submitted() {
      // caller must hold qlock
      if (q.empty()) {
	return true;
      }
      TransContext *txc = &q.back();
      if (txc->state >= TransContext::STATE_KV_SUBMITTED) {
	return true;
      }
      return false;
    }

    void flush() override {
      std::unique_lock<std::mutex> l(qlock);
      while (true) {
	// set flag before the check because the condition
	// may become true outside qlock, and we need to make
	// sure those threads see waiters and signal qcond.
	++kv_submitted_waiters;
	if (_is_all_kv_submitted()) {
	  return;
	}
	qcond.wait(l);
	--kv_submitted_waiters;
      }
    }

    bool flush_commit(Context *c) override {
      std::lock_guard<std::mutex> l(qlock);
      if (q.empty()) {
	return true;
      }
      TransContext *txc = &q.back();
      if (txc->state >= TransContext::STATE_KV_DONE) {
	return true;
      }
      txc->oncommits.push_back(c);
      return false;
    }
  };

  typedef boost::intrusive::list<
    OpSequencer,
    boost::intrusive::member_hook<
      OpSequencer,
      boost::intrusive::list_member_hook<>,
      &OpSequencer::deferred_osr_queue_item> > deferred_osr_queue_t;

  struct KVSyncThread : public Thread {
    BlueStore *store;
    explicit KVSyncThread(BlueStore *s) : store(s) {}
    void *entry() override {
      store->_kv_sync_thread();
      return NULL;
    }
  };
  struct KVFinalizeThread : public Thread {
    BlueStore *store;
    explicit KVFinalizeThread(BlueStore *s) : store(s) {}
    void *entry() {
      store->_kv_finalize_thread();
      return NULL;
    }
  };

  struct DBHistogram {
    struct value_dist {
      uint64_t count;
      uint32_t max_len;
    };

    struct key_dist {
      uint64_t count;
      uint32_t max_len;
      map<int, struct value_dist> val_map; ///< slab id to count, max length of value and key
    };

    map<string, map<int, struct key_dist> > key_hist;
    map<int, uint64_t> value_hist;
    int get_key_slab(size_t sz);
    string get_key_slab_to_range(int slab);
    int get_value_slab(size_t sz);
    string get_value_slab_to_range(int slab);
    void update_hist_entry(map<string, map<int, struct key_dist> > &key_hist,
			  const string &prefix, size_t key_size, size_t value_size);
    void dump(Formatter *f);
  };

  // --------------------------------------------------------
  // members
private:
  BlueFS *bluefs = nullptr;
  unsigned bluefs_shared_bdev = 0;  ///< which bluefs bdev we are sharing
  bool bluefs_single_shared_device = true;
  utime_t bluefs_last_balance;

  KeyValueDB *db = nullptr;
  BlockDevice *bdev = nullptr;
  std::string freelist_type;
  FreelistManager *fm = nullptr;
  Allocator *alloc = nullptr;
  uuid_d fsid;
  int path_fd = -1;  ///< open handle to $path
  int fsid_fd = -1;  ///< open handle (locked) to $path/fsid
  bool mounted = false;

  RWLock coll_lock = {"BlueStore::coll_lock"};  ///< rwlock to protect coll_map
  mempool::bluestore_cache_other::unordered_map<coll_t, CollectionRef> coll_map;

  vector<Cache*> cache_shards;

  std::mutex osr_lock;              ///< protect osd_set
  std::set<OpSequencerRef> osr_set; ///< set of all OpSequencers

  std::atomic<uint64_t> nid_last = {0};
  std::atomic<uint64_t> nid_max = {0};
  std::atomic<uint64_t> blobid_last = {0};
  std::atomic<uint64_t> blobid_max = {0};

  Throttle throttle_bytes;          ///< submit to commit
  Throttle throttle_deferred_bytes;  ///< submit to deferred complete

  interval_set<uint64_t> bluefs_extents;  ///< block extents owned by bluefs
  interval_set<uint64_t> bluefs_extents_reclaiming; ///< currently reclaiming

  std::mutex deferred_lock;
  std::atomic<uint64_t> deferred_seq = {0};
  deferred_osr_queue_t deferred_queue; ///< osr's with deferred io pending
  int deferred_queue_size = 0;         ///< num txc's queued across all osrs
  atomic_int deferred_aggressive = {0}; ///< aggressive wakeup of kv thread
  Finisher deferred_finisher;

  int m_finisher_num = 1;
  vector<Finisher*> finishers;

  KVSyncThread kv_sync_thread;
  std::mutex kv_lock;
  std::condition_variable kv_cond;
  bool _kv_only = false;
  bool kv_sync_started = false;
  bool kv_stop = false;
  bool kv_finalize_started = false;
  bool kv_finalize_stop = false;
  deque<TransContext*> kv_queue;             ///< ready, already submitted
  deque<TransContext*> kv_queue_unsubmitted; ///< ready, need submit by kv thread
  deque<TransContext*> kv_committing;        ///< currently syncing
  deque<DeferredBatch*> deferred_done_queue;   ///< deferred ios done
  deque<DeferredBatch*> deferred_stable_queue; ///< deferred ios done + stable

  KVFinalizeThread kv_finalize_thread;
  std::mutex kv_finalize_lock;
  std::condition_variable kv_finalize_cond;
  deque<TransContext*> kv_committing_to_finalize;   ///< pending finalization
  deque<DeferredBatch*> deferred_stable_to_finalize; ///< pending finalization

  PerfCounters *logger = nullptr;

  list<CollectionRef> removed_collections;

  RWLock debug_read_error_lock = {"BlueStore::debug_read_error_lock"};
  set<ghobject_t> debug_data_error_objects;
  set<ghobject_t> debug_mdata_error_objects;

  std::atomic<int> csum_type = {Checksummer::CSUM_CRC32C};

  uint64_t block_size = 0;     ///< block size of block device (power of 2)
  uint64_t block_mask = 0;     ///< mask to get just the block offset
  size_t block_size_order = 0; ///< bits to shift to get block size

  uint64_t min_alloc_size = 0; ///< minimum allocation unit (power of 2)
  ///< bits for min_alloc_size
  uint8_t min_alloc_size_order = 0;
  static_assert(std::numeric_limits<uint8_t>::max() >
		std::numeric_limits<decltype(min_alloc_size)>::digits,
		"not enough bits for min_alloc_size");

  ///< maximum allocation unit (power of 2)
  std::atomic<uint64_t> max_alloc_size = {0};

  ///< number threshold for forced deferred writes
  std::atomic<int> deferred_batch_ops = {0};

  ///< size threshold for forced deferred writes
  std::atomic<uint64_t> prefer_deferred_size = {0};

  ///< approx cost per io, in bytes
  std::atomic<uint64_t> throttle_cost_per_io = {0};

  std::atomic<Compressor::CompressionMode> comp_mode =
    {Compressor::COMP_NONE}; ///< compression mode
  CompressorRef compressor;
  std::atomic<uint64_t> comp_min_blob_size = {0};
  std::atomic<uint64_t> comp_max_blob_size = {0};

  std::atomic<uint64_t> max_blob_size = {0};  ///< maximum blob size

  uint64_t kv_ios = 0;
  uint64_t kv_throttle_costs = 0;

  // cache trim control
  uint64_t cache_size = 0;      ///< total cache size
  float cache_meta_ratio = 0;   ///< cache ratio dedicated to metadata
  float cache_kv_ratio = 0;     ///< cache ratio dedicated to kv (e.g., rocksdb)
  float cache_data_ratio = 0;   ///< cache ratio dedicated to object data

  std::mutex vstatfs_lock;
  volatile_statfs vstatfs;

  struct MempoolThread : public Thread {
    BlueStore *store;
    Cond cond;
    Mutex lock;
    bool stop = false;
  public:
    explicit MempoolThread(BlueStore *s)
      : store(s),
	lock("BlueStore::MempoolThread::lock") {}
    void *entry() override;
    void init() {
      assert(stop == false);
      create("bstore_mempool");
    }
    void shutdown() {
      lock.Lock();
      stop = true;
      cond.Signal();
      lock.Unlock();
      join();
    }
  } mempool_thread;

  // --------------------------------------------------------
  // private methods

  void _init_logger();
  void _shutdown_logger();
  int _reload_logger();

  int _open_path();
  void _close_path();
  int _open_fsid(bool create);
  int _lock_fsid();
  int _read_fsid(uuid_d *f);
  int _write_fsid();
  void _close_fsid();
  void _set_alloc_sizes();
  void _set_blob_size();

  int _open_bdev(bool create);
  void _close_bdev();
  int _open_db(bool create);
  void _close_db();
  int _open_fm(bool create);
  void _close_fm();
  int _open_alloc();
  void _close_alloc();
  int _open_collections(int *errors=0);
  void _close_collections();

  int _setup_block_symlink_or_file(string name, string path, uint64_t size,
				   bool create);

public:
  static int _write_bdev_label(CephContext* cct,
			       string path, bluestore_bdev_label_t label);
  static int _read_bdev_label(CephContext* cct, string path,
			      bluestore_bdev_label_t *label);
private:
  int _check_or_set_bdev_label(string path, uint64_t size, string desc,
			       bool create);

  int _open_super_meta();

  void _open_statfs();

  int _reconcile_bluefs_freespace();
  int _balance_bluefs_freespace(PExtentVector *extents);
  void _commit_bluefs_freespace(const PExtentVector& extents);

  CollectionRef _get_collection(const coll_t& cid);
  void _queue_reap_collection(CollectionRef& c);
  void _reap_collections();
  void _update_cache_logger();

  void _assign_nid(TransContext *txc, OnodeRef o);
  uint64_t _assign_blobid(TransContext *txc);

  void _dump_onode(const OnodeRef& o, int log_level=30);
  void _dump_extent_map(ExtentMap& em, int log_level=30);
  void _dump_transaction(Transaction *t, int log_level = 30);

  TransContext *_txc_create(OpSequencer *osr);
  void _txc_update_store_statfs(TransContext *txc);
  void _txc_add_transaction(TransContext *txc, Transaction *t);
  void _txc_calc_cost(TransContext *txc);
  void _txc_write_nodes(TransContext *txc, KeyValueDB::Transaction t);
  void _txc_state_proc(TransContext *txc);
  void _txc_aio_submit(TransContext *txc);
public:
  void txc_aio_finish(void *p) {
    _txc_state_proc(static_cast<TransContext*>(p));
  }
private:
  void _txc_finish_io(TransContext *txc);
  void _txc_finalize_kv(TransContext *txc, KeyValueDB::Transaction t);
  void _txc_applied_kv(TransContext *txc);
  void _txc_committed_kv(TransContext *txc);
  void _txc_finish(TransContext *txc);
  void _txc_release_alloc(TransContext *txc);

  void _osr_drain_preceding(TransContext *txc);
  void _osr_drain_all();
  void _osr_unregister_all();

  void _kv_start();
  void _kv_stop();
  void _kv_sync_thread();
  void _kv_finalize_thread();

  bluestore_deferred_op_t *_get_deferred_op(TransContext *txc, OnodeRef o);
  void _deferred_queue(TransContext *txc);
public:
  void deferred_try_submit();
private:
  void _deferred_submit_unlock(OpSequencer *osr);
  void _deferred_aio_finish(OpSequencer *osr);
  int _deferred_replay();

public:
  using mempool_dynamic_bitset =
    boost::dynamic_bitset<uint64_t,
			  mempool::bluestore_fsck::pool_allocator<uint64_t>>;

private:
  int _fsck_check_extents(
    const ghobject_t& oid,
    const PExtentVector& extents,
    bool compressed,
    mempool_dynamic_bitset &used_blocks,
    uint64_t granularity,
    store_statfs_t& expected_statfs);

  void _buffer_cache_write(
    TransContext *txc,
    BlobRef b,
    uint64_t offset,
    bufferlist& bl,
    unsigned flags) {
    b->shared_blob->bc.write(b->shared_blob->get_cache(), txc->seq, offset, bl,
			     flags);
    txc->shared_blobs_written.insert(b->shared_blob);
  }

  int _collection_list(
    Collection *c, const ghobject_t& start, const ghobject_t& end,
    int max, vector<ghobject_t> *ls, ghobject_t *next);

  template <typename T, typename F>
  T select_option(const std::string& opt_name, T val1, F f) {
    //NB: opt_name reserved for future use
    boost::optional<T> val2 = f();
    if (val2) {
      return *val2;
    }
    return val1;
  }

  void _apply_padding(uint64_t head_pad,
		      uint64_t tail_pad,
		      bufferlist& padded);

  // -- ondisk version ---
public:
  const int32_t latest_ondisk_format = 2;        ///< our version
  const int32_t min_readable_ondisk_format = 1;  ///< what we can read
  const int32_t min_compat_ondisk_format = 2;    ///< who can read us

private:
  int32_t ondisk_format = 0;  ///< value detected on mount

  int _upgrade_super();  ///< upgrade (called during open_super)
  void _prepare_ondisk_format_super(KeyValueDB::Transaction& t);

  // --- public interface ---
public:
  BlueStore(CephContext *cct, const string& path);
  BlueStore(CephContext *cct, const string& path, uint64_t min_alloc_size); // Ctor for UT only
  ~BlueStore() override;

  string get_type() override {
    return "bluestore";
  }

  bool needs_journal() override { return false; };
  bool wants_journal() override { return false; };
  bool allows_journal() override { return false; };

  bool is_rotational() override;
  bool is_journal_rotational() override;

  string get_default_device_class() override {
    string device_class;
    map<string, string> metadata;
    collect_metadata(&metadata);
    auto it = metadata.find("bluestore_bdev_type");
    if (it != metadata.end()) {
      device_class = it->second;
    }
    return device_class;
  }

  static int get_block_device_fsid(CephContext* cct, const string& path,
				   uuid_d *fsid);

  bool test_mount_in_use() override;

private:
  int _mount(bool kv_only);
public:
  int mount() override {
    return _mount(false);
  }
  int umount() override;

  int start_kv_only(KeyValueDB **pdb) {
    int r = _mount(true);
    if (r < 0)
      return r;
    *pdb = db;
    return 0;
  }

  int write_meta(const std::string& key, const std::string& value) override;
  int read_meta(const std::string& key, std::string *value) override;


  int fsck(bool deep) override {
    return _fsck(deep, false);
  }
  int repair(bool deep) override {
    return _fsck(deep, true);
  }
  int _fsck(bool deep, bool repair);

  void set_cache_shards(unsigned num) override;

  int validate_hobject_key(const hobject_t &obj) const override {
    return 0;
  }
  unsigned get_max_attr_name_length() override {
    return 256;  // arbitrary; there is no real limit internally
  }

  int mkfs() override;
  int mkjournal() override {
    return 0;
  }

  void get_db_statistics(Formatter *f) override;
  void generate_db_histogram(Formatter *f) override;
  void _flush_cache();
  void flush_cache() override;
  void dump_perf_counters(Formatter *f) override {
    f->open_object_section("perf_counters");
    logger->dump_formatted(f, false);
    f->close_section();
  }

  void register_osr(OpSequencer *osr) {
    std::lock_guard<std::mutex> l(osr_lock);
    osr_set.insert(osr);
  }
  void unregister_osr(OpSequencer *osr) {
    std::lock_guard<std::mutex> l(osr_lock);
    osr_set.erase(osr);
  }

public:
  int statfs(struct store_statfs_t *buf) override;

  void collect_metadata(map<string,string> *pm) override;

  bool exists(const coll_t& cid, const ghobject_t& oid) override;
  bool exists(CollectionHandle &c, const ghobject_t& oid) override;
  int set_collection_opts(
    const coll_t& cid,
    const pool_opts_t& opts) override;
  int stat(
    const coll_t& cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false) override;
  int stat(
    CollectionHandle &c,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false) override;
  int read(
    const coll_t& cid,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0) override;
  int read(
    CollectionHandle &c,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0) override;
  int _do_read(
    Collection *c,
    OnodeRef o,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0);

private:
  int _fiemap(CollectionHandle &c_, const ghobject_t& oid,
 	     uint64_t offset, size_t len, interval_set<uint64_t>& destset);
public:
  int fiemap(const coll_t& cid, const ghobject_t& oid,
	     uint64_t offset, size_t len, bufferlist& bl) override;
  int fiemap(CollectionHandle &c, const ghobject_t& oid,
	     uint64_t offset, size_t len, bufferlist& bl) override;
  int fiemap(const coll_t& cid, const ghobject_t& oid,
	     uint64_t offset, size_t len, map<uint64_t, uint64_t>& destmap) override;
  int fiemap(CollectionHandle &c, const ghobject_t& oid,
	     uint64_t offset, size_t len, map<uint64_t, uint64_t>& destmap) override;


  int getattr(const coll_t& cid, const ghobject_t& oid, const char *name,
	      bufferptr& value) override;
  int getattr(CollectionHandle &c, const ghobject_t& oid, const char *name,
	      bufferptr& value) override;

  int getattrs(const coll_t& cid, const ghobject_t& oid,
	       map<string,bufferptr>& aset) override;
  int getattrs(CollectionHandle &c, const ghobject_t& oid,
	       map<string,bufferptr>& aset) override;

  int list_collections(vector<coll_t>& ls) override;

  CollectionHandle open_collection(const coll_t &c) override;

  bool collection_exists(const coll_t& c) override;
  int collection_empty(const coll_t& c, bool *empty) override;
  int collection_bits(const coll_t& c) override;

  int collection_list(const coll_t& cid,
		      const ghobject_t& start,
		      const ghobject_t& end,
		      int max,
		      vector<ghobject_t> *ls, ghobject_t *next) override;
  int collection_list(CollectionHandle &c,
		      const ghobject_t& start,
		      const ghobject_t& end,
		      int max,
		      vector<ghobject_t> *ls, ghobject_t *next) override;

  int omap_get(
    const coll_t& cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    ) override;
  int omap_get(
    CollectionHandle &c,     ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    ) override;

  /// Get omap header
  int omap_get_header(
    const coll_t& cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    ) override;
  int omap_get_header(
    CollectionHandle &c,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    ) override;

  /// Get keys defined on oid
  int omap_get_keys(
    const coll_t& cid,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    ) override;
  int omap_get_keys(
    CollectionHandle &c,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    ) override;

  /// Get key values
  int omap_get_values(
    const coll_t& cid,                    ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    ) override;
  int omap_get_values(
    CollectionHandle &c,         ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    ) override;

  /// Filters keys into out which are defined on oid
  int omap_check_keys(
    const coll_t& cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    ) override;
  int omap_check_keys(
    CollectionHandle &c,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    ) override;

  ObjectMap::ObjectMapIterator get_omap_iterator(
    const coll_t& cid,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    ) override;
  ObjectMap::ObjectMapIterator get_omap_iterator(
    CollectionHandle &c,   ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    ) override;

  void set_fsid(uuid_d u) override {
    fsid = u;
  }
  uuid_d get_fsid() override {
    return fsid;
  }

  uint64_t estimate_objects_overhead(uint64_t num_objects) override {
    return num_objects * 300; //assuming per-object overhead is 300 bytes
  }

  struct BSPerfTracker {
    PerfCounters::avg_tracker<uint64_t> os_commit_latency;
    PerfCounters::avg_tracker<uint64_t> os_apply_latency;

    objectstore_perf_stat_t get_cur_stats() const {
      objectstore_perf_stat_t ret;
      ret.os_commit_latency = os_commit_latency.current_avg();
      ret.os_apply_latency = os_apply_latency.current_avg();
      return ret;
    }

    void update_from_perfcounters(PerfCounters &logger);
  } perf_tracker;

  objectstore_perf_stat_t get_cur_stats() override {
    perf_tracker.update_from_perfcounters(*logger);
    return perf_tracker.get_cur_stats();
  }
  const PerfCounters* get_perf_counters() const override {
    return logger;
  }

  int queue_transactions(
    Sequencer *osr,
    vector<Transaction>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL) override;

  // error injection
  void inject_data_error(const ghobject_t& o) override {
    RWLock::WLocker l(debug_read_error_lock);
    debug_data_error_objects.insert(o);
  }
  void inject_mdata_error(const ghobject_t& o) override {
    RWLock::WLocker l(debug_read_error_lock);
    debug_mdata_error_objects.insert(o);
  }
  void compact() override {
    assert(db);
    db->compact();
  }
  
private:
  bool _debug_data_eio(const ghobject_t& o) {
    if (!cct->_conf->bluestore_debug_inject_read_err) {
      return false;
    }
    RWLock::RLocker l(debug_read_error_lock);
    return debug_data_error_objects.count(o);
  }
  bool _debug_mdata_eio(const ghobject_t& o) {
    if (!cct->_conf->bluestore_debug_inject_read_err) {
      return false;
    }
    RWLock::RLocker l(debug_read_error_lock);
    return debug_mdata_error_objects.count(o);
  }
  void _debug_obj_on_delete(const ghobject_t& o) {
    if (cct->_conf->bluestore_debug_inject_read_err) {
      RWLock::WLocker l(debug_read_error_lock);
      debug_data_error_objects.erase(o);
      debug_mdata_error_objects.erase(o);
    }
  }

private:

  // --------------------------------------------------------
  // read processing internal methods
  int _verify_csum(
    OnodeRef& o,
    const bluestore_blob_t* blob,
    uint64_t blob_xoffset,
    const bufferlist& bl,
    uint64_t logical_offset) const;
  int _decompress(bufferlist& source, bufferlist* result);


  // --------------------------------------------------------
  // write ops

  struct WriteContext {
    bool buffered = false;          ///< buffered write
    bool compress = false;          ///< compressed write
    uint64_t target_blob_size = 0;  ///< target (max) blob size
    unsigned csum_order = 0;        ///< target checksum chunk order

    old_extent_map_t old_extents;   ///< must deref these blobs

    struct write_item {
      uint64_t logical_offset;      ///< write logical offset
      BlobRef b;
      uint64_t blob_length;
      uint64_t b_off;
      bufferlist bl;
      uint64_t b_off0; ///< original offset in a blob prior to padding
      uint64_t length0; ///< original data length prior to padding

      bool mark_unused;
      bool new_blob; ///< whether new blob was created

      bool compressed = false;
      bufferlist compressed_bl;
      size_t compressed_len = 0;

      write_item(
	uint64_t logical_offs,
        BlobRef b,
        uint64_t blob_len,
        uint64_t o,
        bufferlist& bl,
        uint64_t o0,
        uint64_t l0,
        bool _mark_unused,
	bool _new_blob)
       :
         logical_offset(logical_offs),
         b(b),
         blob_length(blob_len),
         b_off(o),
         bl(bl),
         b_off0(o0),
         length0(l0),
         mark_unused(_mark_unused),
	 new_blob(_new_blob) {}
    };
    vector<write_item> writes;                 ///< blobs we're writing

    /// partial clone of the context
    void fork(const WriteContext& other) {
      buffered = other.buffered;
      compress = other.compress;
      target_blob_size = other.target_blob_size;
      csum_order = other.csum_order;
    }
    void write(
      uint64_t loffs,
      BlobRef b,
      uint64_t blob_len,
      uint64_t o,
      bufferlist& bl,
      uint64_t o0,
      uint64_t len0,
      bool _mark_unused,
      bool _new_blob) {
      writes.emplace_back(loffs,
                          b,
                          blob_len,
                          o,
                          bl,
                          o0,
                          len0,
                          _mark_unused,
                          _new_blob);
    }
    /// Checks for writes to the same pextent within a blob
    bool has_conflict(
      BlobRef b,
      uint64_t loffs,
      uint64_t loffs_end,
      uint64_t min_alloc_size);
  };

  void _do_write_small(
    TransContext *txc,
    CollectionRef &c,
    OnodeRef o,
    uint64_t offset, uint64_t length,
    bufferlist::iterator& blp,
    WriteContext *wctx);
  void _do_write_big(
    TransContext *txc,
    CollectionRef &c,
    OnodeRef o,
    uint64_t offset, uint64_t length,
    bufferlist::iterator& blp,
    WriteContext *wctx);
  int _do_alloc_write(
    TransContext *txc,
    CollectionRef c,
    OnodeRef o,
    WriteContext *wctx);
  void _wctx_finish(
    TransContext *txc,
    CollectionRef& c,
    OnodeRef o,
    WriteContext *wctx,
    set<SharedBlob*> *maybe_unshared_blobs=0);

  int _do_transaction(Transaction *t,
		      TransContext *txc,
		      ThreadPool::TPHandle *handle);

  int _write(TransContext *txc,
	     CollectionRef& c,
	     OnodeRef& o,
	     uint64_t offset, size_t len,
	     bufferlist& bl,
	     uint32_t fadvise_flags);
  void _pad_zeros(bufferlist *bl, uint64_t *offset,
		  uint64_t chunk_size);

  void _choose_write_options(CollectionRef& c,
                             OnodeRef o,
                             uint32_t fadvise_flags,
                             WriteContext *wctx);

  int _do_gc(TransContext *txc,
             CollectionRef& c,
             OnodeRef o,
             const GarbageCollector& gc,
             const WriteContext& wctx,
             uint64_t *dirty_start,
             uint64_t *dirty_end);

  int _do_write(TransContext *txc,
		CollectionRef &c,
		OnodeRef o,
		uint64_t offset, uint64_t length,
		bufferlist& bl,
		uint32_t fadvise_flags);
  void _do_write_data(TransContext *txc,
                      CollectionRef& c,
                      OnodeRef o,
                      uint64_t offset,
                      uint64_t length,
                      bufferlist& bl,
                      WriteContext *wctx);

  int _touch(TransContext *txc,
	     CollectionRef& c,
	     OnodeRef& o);
  int _do_zero(TransContext *txc,
	       CollectionRef& c,
	       OnodeRef& o,
	       uint64_t offset, size_t len);
  int _zero(TransContext *txc,
	    CollectionRef& c,
	    OnodeRef& o,
	    uint64_t offset, size_t len);
  void _do_truncate(TransContext *txc,
		   CollectionRef& c,
		   OnodeRef o,
		   uint64_t offset,
		   set<SharedBlob*> *maybe_unshared_blobs=0);
  int _truncate(TransContext *txc,
		CollectionRef& c,
		OnodeRef& o,
		uint64_t offset);
  int _remove(TransContext *txc,
	      CollectionRef& c,
	      OnodeRef& o);
  int _do_remove(TransContext *txc,
		 CollectionRef& c,
		 OnodeRef o);
  int _setattr(TransContext *txc,
	       CollectionRef& c,
	       OnodeRef& o,
	       const string& name,
	       bufferptr& val);
  int _setattrs(TransContext *txc,
		CollectionRef& c,
		OnodeRef& o,
		const map<string,bufferptr>& aset);
  int _rmattr(TransContext *txc,
	      CollectionRef& c,
	      OnodeRef& o,
	      const string& name);
  int _rmattrs(TransContext *txc,
	       CollectionRef& c,
	       OnodeRef& o);
  void _do_omap_clear(TransContext *txc, uint64_t id);
  int _omap_clear(TransContext *txc,
		  CollectionRef& c,
		  OnodeRef& o);
  int _omap_setkeys(TransContext *txc,
		    CollectionRef& c,
		    OnodeRef& o,
		    bufferlist& bl);
  int _omap_setheader(TransContext *txc,
		      CollectionRef& c,
		      OnodeRef& o,
		      bufferlist& header);
  int _omap_rmkeys(TransContext *txc,
		   CollectionRef& c,
		   OnodeRef& o,
		   bufferlist& bl);
  int _omap_rmkey_range(TransContext *txc,
			CollectionRef& c,
			OnodeRef& o,
			const string& first, const string& last);
  int _set_alloc_hint(
    TransContext *txc,
    CollectionRef& c,
    OnodeRef& o,
    uint64_t expected_object_size,
    uint64_t expected_write_size,
    uint32_t flags);
  int _do_clone_range(TransContext *txc,
		      CollectionRef& c,
		      OnodeRef& oldo,
		      OnodeRef& newo,
		      uint64_t srcoff, uint64_t length, uint64_t dstoff);
  int _clone(TransContext *txc,
	     CollectionRef& c,
	     OnodeRef& oldo,
	     OnodeRef& newo);
  int _clone_range(TransContext *txc,
		   CollectionRef& c,
		   OnodeRef& oldo,
		   OnodeRef& newo,
		   uint64_t srcoff, uint64_t length, uint64_t dstoff);
  int _rename(TransContext *txc,
	      CollectionRef& c,
	      OnodeRef& oldo,
	      OnodeRef& newo,
	      const ghobject_t& new_oid);
  int _create_collection(TransContext *txc, const coll_t &cid,
			 unsigned bits, CollectionRef *c);
  int _remove_collection(TransContext *txc, const coll_t &cid,
                         CollectionRef *c);
  int _split_collection(TransContext *txc,
			CollectionRef& c,
			CollectionRef& d,
			unsigned bits, int rem);
};

inline ostream& operator<<(ostream& out, const BlueStore::OpSequencer& s) {
  return out << *s.parent;
}

static inline void intrusive_ptr_add_ref(BlueStore::Onode *o) {
  o->get();
}
static inline void intrusive_ptr_release(BlueStore::Onode *o) {
  o->put();
}

static inline void intrusive_ptr_add_ref(BlueStore::OpSequencer *o) {
  o->get();
}
static inline void intrusive_ptr_release(BlueStore::OpSequencer *o) {
  o->put();
}

#endif
