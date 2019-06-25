// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_OS_BLUESTORE_BLUEFS_H
#define CEPH_OS_BLUESTORE_BLUEFS_H

#include <atomic>
#include <mutex>

#include "bluefs_types.h"
#include "common/RefCountedObj.h"
#include "BlockDevice.h"

#include "boost/intrusive/list.hpp"
#include <boost/intrusive_ptr.hpp>

class PerfCounters;

class Allocator;

enum {
  l_bluefs_first = 732600,
  l_bluefs_gift_bytes,
  l_bluefs_reclaim_bytes,
  l_bluefs_db_total_bytes,
  l_bluefs_db_used_bytes,
  l_bluefs_wal_total_bytes,
  l_bluefs_wal_used_bytes,
  l_bluefs_slow_total_bytes,
  l_bluefs_slow_used_bytes,
  l_bluefs_num_files,
  l_bluefs_log_bytes,
  l_bluefs_log_compactions,
  l_bluefs_logged_bytes,
  l_bluefs_files_written_wal,
  l_bluefs_files_written_sst,
  l_bluefs_bytes_written_wal,
  l_bluefs_bytes_written_sst,
  l_bluefs_last,
};

class BlueFS {
public:
  CephContext* cct;
  static constexpr unsigned MAX_BDEV = 3;
  static constexpr unsigned BDEV_WAL = 0;
  static constexpr unsigned BDEV_DB = 1;
  static constexpr unsigned BDEV_SLOW = 2;

  enum {
    WRITER_UNKNOWN,
    WRITER_WAL,
    WRITER_SST,
  };

  struct File : public RefCountedObject {
    MEMPOOL_CLASS_HELPERS();

    bluefs_fnode_t fnode;
    int refs;
    uint64_t dirty_seq;
    bool locked;
    bool deleted;
    boost::intrusive::list_member_hook<> dirty_item;

    std::atomic_int num_readers, num_writers;
    std::atomic_int num_reading;

    File()
      : RefCountedObject(NULL, 0),
	refs(0),
	dirty_seq(0),
	locked(false),
	deleted(false),
	num_readers(0),
	num_writers(0),
	num_reading(0)
      {}
    ~File() override {
      assert(num_readers.load() == 0);
      assert(num_writers.load() == 0);
      assert(num_reading.load() == 0);
      assert(!locked);
    }

    friend void intrusive_ptr_add_ref(File *f) {
      f->get();
    }
    friend void intrusive_ptr_release(File *f) {
      f->put();
    }
  };
  typedef boost::intrusive_ptr<File> FileRef;

  typedef boost::intrusive::list<
      File,
      boost::intrusive::member_hook<
        File,
	boost::intrusive::list_member_hook<>,
	&File::dirty_item> > dirty_file_list_t;

  struct Dir : public RefCountedObject {
    MEMPOOL_CLASS_HELPERS();

    mempool::bluefs::map<string,FileRef> file_map;

    Dir() : RefCountedObject(NULL, 0) {}

    friend void intrusive_ptr_add_ref(Dir *d) {
      d->get();
    }
    friend void intrusive_ptr_release(Dir *d) {
      d->put();
    }
  };
  typedef boost::intrusive_ptr<Dir> DirRef;

  struct FileWriter {
    MEMPOOL_CLASS_HELPERS();

    FileRef file;
    uint64_t pos;           ///< start offset for buffer
    bufferlist buffer;      ///< new data to write (at end of file)
    bufferlist tail_block;  ///< existing partial block at end of file, if any
    bufferlist::page_aligned_appender buffer_appender;  //< for const char* only
    int writer_type = 0;    ///< WRITER_*

    std::mutex lock;
    std::array<IOContext*,MAX_BDEV> iocv; ///< for each bdev

    FileWriter(FileRef f)
      : file(f),
	pos(0),
	buffer_appender(buffer.get_page_aligned_appender(
			  g_conf->bluefs_alloc_size / CEPH_PAGE_SIZE)) {
      ++file->num_writers;
      iocv.fill(nullptr);
    }
    // NOTE: caller must call BlueFS::close_writer()
    ~FileWriter() {
      --file->num_writers;
    }

    // note: BlueRocksEnv uses this append exclusively, so it's safe
    // to use buffer_appender exclusively here (e.g., it's notion of
    // offset will remain accurate).
    void append(const char *buf, size_t len) {
      buffer_appender.append(buf, len);
    }

    // note: used internally only, for ino 1 or 0.
    void append(bufferlist& bl) {
      buffer.claim_append(bl);
    }

    uint64_t get_effective_write_pos() {
      buffer_appender.flush();
      return pos + buffer.length();
    }
  };

  struct FileReaderBuffer {
    MEMPOOL_CLASS_HELPERS();

    uint64_t bl_off;        ///< prefetch buffer logical offset
    bufferlist bl;          ///< prefetch buffer
    uint64_t pos;           ///< current logical offset
    uint64_t max_prefetch;  ///< max allowed prefetch

    explicit FileReaderBuffer(uint64_t mpf)
      : bl_off(0),
	pos(0),
	max_prefetch(mpf) {}

    uint64_t get_buf_end() {
      return bl_off + bl.length();
    }
    uint64_t get_buf_remaining(uint64_t p) {
      if (p >= bl_off && p < bl_off + bl.length())
	return bl_off + bl.length() - p;
      return 0;
    }

    void skip(size_t n) {
      pos += n;
    }
    void seek(uint64_t offset) {
      pos = offset;
    }
  };

  struct FileReader {
    MEMPOOL_CLASS_HELPERS();

    FileRef file;
    FileReaderBuffer buf;
    bool random;
    bool ignore_eof;        ///< used when reading our log file

    FileReader(FileRef f, uint64_t mpf, bool rand, bool ie)
      : file(f),
	buf(mpf),
	random(rand),
	ignore_eof(ie) {
      ++file->num_readers;
    }
    ~FileReader() {
      --file->num_readers;
    }
  };

  struct FileLock {
    MEMPOOL_CLASS_HELPERS();

    FileRef file;
    explicit FileLock(FileRef f) : file(f) {}
  };

private:
  std::mutex lock;

  PerfCounters *logger = nullptr;

  // cache
  mempool::bluefs::map<string, DirRef> dir_map;              ///< dirname -> Dir
  mempool::bluefs::unordered_map<uint64_t,FileRef> file_map; ///< ino -> File

  // map of dirty files, files of same dirty_seq are grouped into list.
  map<uint64_t, dirty_file_list_t> dirty_files;

  bluefs_super_t super;        ///< latest superblock (as last written)
  uint64_t ino_last = 0;       ///< last assigned ino (this one is in use)
  uint64_t log_seq = 0;        ///< last used log seq (by current pending log_t)
  uint64_t log_seq_stable = 0; ///< last stable/synced log seq
  FileWriter *log_writer = 0;  ///< writer for the log
  bluefs_transaction_t log_t;  ///< pending, unwritten log transaction
  bool log_flushing = false;   ///< true while flushing the log
  std::condition_variable log_cond;

  uint64_t new_log_jump_to = 0;
  uint64_t old_log_jump_to = 0;
  FileRef new_log = nullptr;
  FileWriter *new_log_writer = nullptr;

  /*
   * There are up to 3 block devices:
   *
   *  BDEV_DB   db/      - the primary db device
   *  BDEV_WAL  db.wal/  - a small, fast device, specifically for the WAL
   *  BDEV_SLOW db.slow/ - a big, slow device, to spill over to as BDEV_DB fills
   */
  vector<BlockDevice*> bdev;                  ///< block devices we can use
  vector<IOContext*> ioc;                     ///< IOContexts for bdevs
  vector<interval_set<uint64_t> > block_all;  ///< extents in bdev we own
  vector<uint64_t> block_total;               ///< sum of block_all
  vector<Allocator*> alloc;                   ///< allocators for bdevs
  vector<interval_set<uint64_t>> pending_release; ///< extents to release

  void _init_logger();
  void _shutdown_logger();
  void _update_logger_stats();

  void _init_alloc();
  void _stop_alloc();

  void _pad_bl(bufferlist& bl);  ///< pad bufferlist to block size w/ zeros

  FileRef _get_file(uint64_t ino);
  void _drop_link(FileRef f);

  int _allocate(uint8_t bdev, uint64_t len,
		bluefs_fnode_t* node);
  int _flush_range(FileWriter *h, uint64_t offset, uint64_t length);
  int _flush(FileWriter *h, bool force);
  int _fsync(FileWriter *h, std::unique_lock<std::mutex>& l);

  void _claim_completed_aios(FileWriter *h, list<aio_t> *ls);
  void wait_for_aio(FileWriter *h);  // safe to call without a lock

  int _flush_and_sync_log(std::unique_lock<std::mutex>& l,
			  uint64_t want_seq = 0,
			  uint64_t jump_to = 0);
  uint64_t _estimate_log_size();
  bool _should_compact_log();
  void _compact_log_dump_metadata(bluefs_transaction_t *t);
  void _compact_log_sync();
  void _compact_log_async(std::unique_lock<std::mutex>& l);

  //void _aio_finish(void *priv);

  void _flush_bdev_safely(FileWriter *h);
  void flush_bdev();  // this is safe to call without a lock

  int _preallocate(FileRef f, uint64_t off, uint64_t len);
  int _truncate(FileWriter *h, uint64_t off);

  int _read(
    FileReader *h,   ///< [in] read from here
    FileReaderBuffer *buf, ///< [in] reader state
    uint64_t offset, ///< [in] offset
    size_t len,      ///< [in] this many bytes
    bufferlist *outbl,   ///< [out] optional: reference the result here
    char *out);      ///< [out] optional: or copy it here
  int _read_random(
    FileReader *h,   ///< [in] read from here
    uint64_t offset, ///< [in] offset
    size_t len,      ///< [in] this many bytes
    char *out);      ///< [out] optional: or copy it here

  void _invalidate_cache(FileRef f, uint64_t offset, uint64_t length);

  int _open_super();
  int _write_super();
  int _replay(bool noop); ///< replay journal

  FileWriter *_create_writer(FileRef f);
  void _close_writer(FileWriter *h);

  // always put the super in the second 4k block.  FIXME should this be
  // block size independent?
  unsigned get_super_offset() {
    return 4096;
  }
  unsigned get_super_length() {
    return 4096;
  }

public:
  BlueFS(CephContext* cct);
  ~BlueFS();

  // the super is always stored on bdev 0
  int mkfs(uuid_d osd_uuid);
  int mount();
  void umount();

  void collect_metadata(map<string,string> *pm);
  int fsck();

  uint64_t get_fs_usage();
  uint64_t get_total(unsigned id);
  uint64_t get_free(unsigned id);
  void get_usage(vector<pair<uint64_t,uint64_t>> *usage); // [<free,total> ...]
  void dump_perf_counters(Formatter *f);

  void dump_block_extents(ostream& out);

  /// get current extents that we own for given block device
  int get_block_extents(unsigned id, interval_set<uint64_t> *extents);

  int open_for_write(
    const string& dir,
    const string& file,
    FileWriter **h,
    bool overwrite);

  int open_for_read(
    const string& dir,
    const string& file,
    FileReader **h,
    bool random = false);

  void close_writer(FileWriter *h) {
    std::lock_guard<std::mutex> l(lock);
    _close_writer(h);
  }

  int rename(const string& old_dir, const string& old_file,
	     const string& new_dir, const string& new_file);

  int readdir(const string& dirname, vector<string> *ls);

  int unlink(const string& dirname, const string& filename);
  int mkdir(const string& dirname);
  int rmdir(const string& dirname);
  bool wal_is_rotational();

  bool dir_exists(const string& dirname);
  int stat(const string& dirname, const string& filename,
	   uint64_t *size, utime_t *mtime);

  int lock_file(const string& dirname, const string& filename, FileLock **p);
  int unlock_file(FileLock *l);

  void flush_log();
  void compact_log();

  /// sync any uncommitted state to disk
  void sync_metadata();

  int add_block_device(unsigned bdev, const string& path);
  bool bdev_support_label(unsigned id);
  uint64_t get_block_device_size(unsigned bdev);

  /// gift more block space
  void add_block_extent(unsigned bdev, uint64_t offset, uint64_t len);

  /// reclaim block space
  int reclaim_blocks(unsigned bdev, uint64_t want,
		     AllocExtentVector *extents);

  void flush(FileWriter *h) {
    std::lock_guard<std::mutex> l(lock);
    _flush(h, false);
  }
  void flush_range(FileWriter *h, uint64_t offset, uint64_t length) {
    std::lock_guard<std::mutex> l(lock);
    _flush_range(h, offset, length);
  }
  int fsync(FileWriter *h) {
    std::unique_lock<std::mutex> l(lock);
    return _fsync(h, l);
  }
  int read(FileReader *h, FileReaderBuffer *buf, uint64_t offset, size_t len,
	   bufferlist *outbl, char *out) {
    // no need to hold the global lock here; we only touch h and
    // h->file, and read vs write or delete is already protected (via
    // atomics and asserts).
    return _read(h, buf, offset, len, outbl, out);
  }
  int read_random(FileReader *h, uint64_t offset, size_t len,
		  char *out) {
    // no need to hold the global lock here; we only touch h and
    // h->file, and read vs write or delete is already protected (via
    // atomics and asserts).
    return _read_random(h, offset, len, out);
  }
  void invalidate_cache(FileRef f, uint64_t offset, uint64_t len) {
    std::lock_guard<std::mutex> l(lock);
    _invalidate_cache(f, offset, len);
  }
  int preallocate(FileRef f, uint64_t offset, uint64_t len) {
    std::lock_guard<std::mutex> l(lock);
    return _preallocate(f, offset, len);
  }
  int truncate(FileWriter *h, uint64_t offset) {
    std::lock_guard<std::mutex> l(lock);
    return _truncate(h, offset);
  }

};

#endif
