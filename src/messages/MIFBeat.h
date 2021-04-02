// Create time: Duo in 20/08/13
// The message which would send from MDSs to MDS0, 

#ifndef CEPH_MIFBEAT_H
#define CEPH_MIFBEAT_H

#include "include/types.h"
#include "msg/Message.h"
#include "mds/mdstypes.h"

class MIFBeat : public Message 
{
   
  
    /* data */
    mds_load_t load;
    __s32        beat;
    double IFvaule;
    vector<migration_decision_t> my_decision;
    //vector<mds_rank_t> target_import_mds_list;
    //vector<__u32> target_export_load_list;
    map<mds_rank_t, double> decision_map;

public:
    mds_load_t& get_load() { return load; }
    int get_beat() { return beat; }
    double get_IFvaule() {return IFvaule;}
    vector<migration_decision_t>& get_decision() {return my_decision;}

    MIFBeat()
    : Message(MSG_MDS_IFBEAT), load(utime_t()), IFvaule(0) { }
    MIFBeat(mds_load_t& load, int beat)
    : Message(MSG_MDS_IFBEAT),
      load(load){
    this->beat = beat;
  }

  MIFBeat(mds_load_t& load, int beat, double IFvaule)
    : Message(MSG_MDS_IFBEAT),
      load(load),
      IFvaule(IFvaule)
      {
    this->beat = beat;
  }
    MIFBeat(mds_load_t& load, int beat, double IFvaule, vector<migration_decision_t>& my_decision)
    : Message(MSG_MDS_IFBEAT),
      load(load),
      IFvaule(IFvaule),
      my_decision(my_decision)  
      {
    //this->IFvaule = IFvaule;

    this->beat = beat;
  }

private:
    ~MIFBeat()override {}
public:
  const char *get_type_name() const override { return "IFB"; }

  void encode_payload(uint64_t features) override {
    for (vector<migration_decision_t>::iterator it = my_decision.begin();it!=my_decision.end();it++){
      decision_map[(*it).target_import_mds]=(*it).target_export_percent;
      decision_map[(*it).target_import_mds]=(*it).target_export_percent;
    }

    ::encode(load, payload);
    ::encode(beat, payload);
    ::encode(IFvaule, payload);
    ::encode(decision_map, payload);
  }
  
  void decode_payload() override {
    bufferlist::iterator p = payload.begin();
    utime_t now(ceph_clock_now());
    ::decode(load, now, p);
    ::decode(beat, p);
    ::decode(IFvaule, p);
    ::decode(decision_map, p);
    for(map<mds_rank_t, double>::iterator it = decision_map.begin(); it != decision_map.end();++it)
    {
      migration_decision_t temp_decision = {it->first,it->second,it->second};
      my_decision.push_back(temp_decision);
    }

    //::decode(target_import_mds_list, payload);
    //::decode(target_export_load_list, payload);
    /*my_decision.clear();
    for (unsigned int i = 0;i<target_import_mds_list.size();i++){
      migration_decision_t temp_decision = {target_import_mds_list[i],target_export_load_list[i]};
      my_decision.push_back(temp_decision);
    }*/
  }
};

#endif
