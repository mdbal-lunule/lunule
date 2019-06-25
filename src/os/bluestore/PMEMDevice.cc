// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Intel <jianpeng.ma@intel.com>
 *
 * Author: Jianpeng Ma <jianpeng.ma@intel.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libpmem.h>

#include "PMEMDevice.h"
#include "include/types.h"
#include "include/compat.h"
#include "include/stringify.h"
#include "common/errno.h"
#include "common/debug.h"
#include "common/blkdev.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev-PMEM("  << path << ") "

PMEMDevice::PMEMDevice(CephContext *cct, aio_callback_t cb, void *cbpriv)
  : BlockDevice(cct),
    fd(-1), addr(0),
    size(0), block_size(0),
    debug_lock("PMEMDevice::debug_lock"),
    injecting_crash(0)
{
}

int PMEMDevice::_lock()
{
  struct flock l;
  memset(&l, 0, sizeof(l));
  l.l_type = F_WRLCK;
  l.l_whence = SEEK_SET;
  l.l_start = 0;
  l.l_len = 0;
  int r = ::fcntl(fd, F_SETLK, &l);
  if (r < 0)
    return -errno;
  return 0;
}

int PMEMDevice::open(const string& p)
{
  path = p;
  int r = 0;
  dout(1) << __func__ << " path " << path << dendl;

  fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    r = -errno;
    derr << __func__ << " open got: " << cpp_strerror(r) << dendl;
    return r;
  }

  r = _lock();
  if (r < 0) {
    derr << __func__ << " failed to lock " << path << ": " << cpp_strerror(r)
	 << dendl;
    goto out_fail;
  }

  struct stat st;
  r = ::fstat(fd, &st);
  if (r < 0) {
    r = -errno;
    derr << __func__ << " fstat got " << cpp_strerror(r) << dendl;
    goto out_fail;
  }
  if (S_ISBLK(st.st_mode)) {
    int64_t s;
    r = get_block_device_size(fd, &s);
    if (r < 0) {
      goto out_fail;
    }
    size = s;
  } else {
    size = st.st_size;
  }

  size_t map_len;
  addr = (char *)pmem_map_file(path.c_str(), size, PMEM_FILE_EXCL, O_RDWR, &map_len, NULL);
  if (addr == NULL) {
    derr << __func__ << " pmem_map_file error" << dendl;
    goto out_fail;
  }
  size = map_len;

  // Operate as though the block size is 4 KB.  The backing file
  // blksize doesn't strictly matter except that some file systems may
  // require a read/modify/write if we write something smaller than
  // it.
  block_size = g_conf->bdev_block_size;
  if (block_size != (unsigned)st.st_blksize) {
    dout(1) << __func__ << " backing device/file reports st_blksize "
      << st.st_blksize << ", using bdev_block_size "
      << block_size << " anyway" << dendl;
  }

  dout(1) << __func__
    << " size " << size
    << " (" << pretty_si_t(size) << "B)"
    << " block_size " << block_size
    << " (" << pretty_si_t(block_size) << "B)"
    << dendl;
  return 0;

 out_fail:
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  fd = -1;
  return r;
}

void PMEMDevice::close()
{
  dout(1) << __func__ << dendl;

  assert(addr != NULL);
  pmem_unmap(addr, size);
  assert(fd >= 0);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  fd = -1;

  path.clear();
}

static string get_dev_property(const char *dev, const char *property)
{
  char val[1024] = {0};
  get_block_device_string_property(dev, property, val, sizeof(val));
  return val;
}

int PMEMDevice::collect_metadata(string prefix, map<string,string> *pm) const
{
  (*pm)[prefix + "rotational"] = stringify((int)(bool)rotational);
  (*pm)[prefix + "size"] = stringify(get_size());
  (*pm)[prefix + "block_size"] = stringify(get_block_size());
  (*pm)[prefix + "driver"] = "PMEMDevice";
  (*pm)[prefix + "type"] = "ssd";

  struct stat st;
  int r = ::fstat(fd, &st);
  if (r < 0)
    return -errno;
  if (S_ISBLK(st.st_mode)) {
    (*pm)[prefix + "access_mode"] = "blk";
    char partition_path[PATH_MAX];
    char dev_node[PATH_MAX];
    int rc = get_device_by_fd(fd, partition_path, dev_node, PATH_MAX);
    switch (rc) {
    case -EOPNOTSUPP:
    case -EINVAL:
      (*pm)[prefix + "partition_path"] = "unknown";
      (*pm)[prefix + "dev_node"] = "unknown";
      break;
    case -ENODEV:
      (*pm)[prefix + "partition_path"] = string(partition_path);
      (*pm)[prefix + "dev_node"] = "unknown";
      break;
    default:
      {
	(*pm)[prefix + "partition_path"] = string(partition_path);
	(*pm)[prefix + "dev_node"] = string(dev_node);
	(*pm)[prefix + "model"] = get_dev_property(dev_node, "device/model");
	(*pm)[prefix + "dev"] = get_dev_property(dev_node, "dev");

	// nvme exposes a serial number
	string serial = get_dev_property(dev_node, "device/serial");
	if (serial.length()) {
	  (*pm)[prefix + "serial"] = serial;
	}

	// nvme has a device/device/* structure; infer from that.  there
	// is probably a better way?
	string nvme_vendor = get_dev_property(dev_node, "device/device/vendor");
	if (nvme_vendor.length()) {
	  (*pm)[prefix + "type"] = "nvme";
	}
      }
    }
  } else {
    (*pm)[prefix + "access_mode"] = "file";
    (*pm)[prefix + "path"] = path;
  }
  return 0;
}

int PMEMDevice::flush()
{
  //Because all write is persist. So no need
  return 0;
}


void PMEMDevice::aio_submit(IOContext *ioc)
{
  return;
}

int PMEMDevice::write(uint64_t off, bufferlist& bl, bool buffered)
{
  uint64_t len = bl.length();
  dout(20) << __func__ << " " << off << "~" << len  << dendl;
  assert(len > 0);
  assert(off < size);
  assert(off + len <= size);

  dout(40) << "data: ";
  bl.hexdump(*_dout);
  *_dout << dendl;

  if (g_conf->bdev_inject_crash &&
      rand() % g_conf->bdev_inject_crash == 0) {
    derr << __func__ << " bdev_inject_crash: dropping io " << off << "~" << len
      << dendl;
    ++injecting_crash;
    return 0;
  }

  bufferlist::iterator p = bl.begin();
  uint32_t off1 = off;
  while (len) {
    const char *data;
    uint32_t l = p.get_ptr_and_advance(len, &data);
    pmem_memcpy_persist(addr + off1, data, l);
    len -= l;
    off1 += l;
  }

  return 0;
}

int PMEMDevice::aio_write(
  uint64_t off,
  bufferlist &bl,
  IOContext *ioc,
  bool buffered)
{
  return write(off, bl, buffered);
}


int PMEMDevice::read(uint64_t off, uint64_t len, bufferlist *pbl,
		      IOContext *ioc,
		      bool buffered)
{
  dout(5) << __func__ << " " << off << "~" << len  << dendl;
  assert(len > 0);
  assert(off < size);
  assert(off + len <= size);

  bufferptr p = buffer::create_page_aligned(len);
  memcpy(p.c_str(), addr + off, len);

  pbl->clear();
  pbl->push_back(std::move(p));

  dout(40) << "data: ";
  pbl->hexdump(*_dout);
  *_dout << dendl;

  return 0;
}

int PMEMDevice::aio_read(uint64_t off, uint64_t len, bufferlist *pbl,
		      IOContext *ioc)
{
  return read(off, len, pbl, ioc, false);
}

int PMEMDevice::read_random(uint64_t off, uint64_t len, char *buf, bool buffered)
{
  assert(len > 0);
  assert(off < size);
  assert(off + len <= size);

  memcpy(buf, addr + off, len);
  return 0;
}


int PMEMDevice::invalidate_cache(uint64_t off, uint64_t len)
{
  dout(5) << __func__ << " " << off << "~" << len << dendl;
  return 0;
}


