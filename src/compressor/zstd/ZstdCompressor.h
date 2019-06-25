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

#ifndef CEPH_ZSTDCOMPRESSOR_H
#define CEPH_ZSTDCOMPRESSOR_H

#include "zstd/lib/zstd.h"
#include "include/buffer.h"
#include "include/encoding.h"
#include "compressor/Compressor.h"

#define COMPRESSION_LEVEL 5

class ZstdCompressor : public Compressor {
 public:
  ZstdCompressor() : Compressor(COMP_ALG_ZSTD, "zstd") {}

  int compress(const bufferlist &src, bufferlist &dst) override {
    bufferptr outptr = buffer::create_page_aligned(
      ZSTD_compressBound(src.length()));
    ZSTD_outBuffer_s outbuf;
    outbuf.dst = outptr.c_str();
    outbuf.size = outptr.length();
    outbuf.pos = 0;

    ZSTD_CStream *s = ZSTD_createCStream();
    ZSTD_initCStream(s, COMPRESSION_LEVEL);
    auto p = src.begin();
    size_t left = src.length();
    while (left) {
      assert(!p.end());
      struct ZSTD_inBuffer_s inbuf;
      inbuf.pos = 0;
      inbuf.size = p.get_ptr_and_advance(left, (const char**)&inbuf.src);
      ZSTD_compressStream(s, &outbuf, &inbuf);
      left -= inbuf.size;
    }
    assert(p.end());
    ZSTD_flushStream(s, &outbuf);
    ZSTD_endStream(s, &outbuf);
    ZSTD_freeCStream(s);

    // prefix with decompressed length
    ::encode((uint32_t)src.length(), dst);
    dst.append(outptr, 0, outbuf.pos);
    return 0;
  }

  int decompress(const bufferlist &src, bufferlist &dst) override {
    bufferlist::iterator i = const_cast<bufferlist&>(src).begin();
    return decompress(i, src.length(), dst);
  }

  int decompress(bufferlist::iterator &p,
		 size_t compressed_len,
		 bufferlist &dst) override {
    if (compressed_len < 4) {
      return -1;
    }
    compressed_len -= 4;
    uint32_t dst_len;
    ::decode(dst_len, p);

    bufferptr dstptr(dst_len);
    ZSTD_outBuffer_s outbuf;
    outbuf.dst = dstptr.c_str();
    outbuf.size = dstptr.length();
    outbuf.pos = 0;
    ZSTD_DStream *s = ZSTD_createDStream();
    ZSTD_initDStream(s);
    while (compressed_len > 0) {
      if (p.end()) {
	return -1;
      }
      ZSTD_inBuffer_s inbuf;
      inbuf.pos = 0;
      inbuf.size = p.get_ptr_and_advance(compressed_len, (const char**)&inbuf.src);
      ZSTD_decompressStream(s, &outbuf, &inbuf);
      compressed_len -= inbuf.size;
    }
    ZSTD_freeDStream(s);

    dst.append(dstptr, 0, outbuf.pos);
    return 0;
  }
};

#endif
