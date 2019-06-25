// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_RGW_COMPRESSION_H
#define CEPH_RGW_COMPRESSION_H

#include <vector>

#include "compressor/Compressor.h"
#include "rgw_op.h"

class RGWGetObj_Decompress : public RGWGetObj_Filter
{
  CephContext* cct;
  CompressorRef compressor;
  RGWCompressionInfo* cs_info;
  bool partial_content;
  vector<compression_block>::iterator first_block, last_block;
  off_t q_ofs, q_len;
  uint64_t cur_ofs;
  bufferlist waiting;
public:
  RGWGetObj_Decompress(CephContext* cct_, 
                       RGWCompressionInfo* cs_info_, 
                       bool partial_content_,
                       RGWGetDataCB* next);
  ~RGWGetObj_Decompress() override {}

  int handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len) override;
  int fixup_range(off_t& ofs, off_t& end) override;

};

class RGWPutObj_Compress : public RGWPutObj_Filter
{
  CephContext* cct;
  bool compressed{false};
  CompressorRef compressor;
  std::vector<compression_block> blocks;
public:
  RGWPutObj_Compress(CephContext* cct_, CompressorRef compressor,
                     RGWPutObjDataProcessor* next)
    : RGWPutObj_Filter(next), cct(cct_), compressor(compressor) {}
  ~RGWPutObj_Compress() override{}
  int handle_data(bufferlist& bl, off_t ofs, void **phandle, rgw_raw_obj *pobj, bool *again) override;

  bool is_compressed() { return compressed; }
  vector<compression_block>& get_compression_blocks() { return blocks; }

}; /* RGWPutObj_Compress */

#endif /* CEPH_RGW_COMPRESSION_H */
