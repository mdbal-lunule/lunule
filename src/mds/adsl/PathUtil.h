#ifndef MDS_ADSL_PATHUTIL_H_
#define MDS_ADSL_PATHUTIL_H_

#include <string>
using std::string;
#include <vector>
using std::vector;
#include <map>
using std::map;

#define WorkloadType WorkloadType1

/**
 * Solution 1:
 * 	
 */
typedef enum _WorkloadType1 {
  WLT_SCAN,
  WLT_ZIPF,
  WLT_MIXED,
  WLT_ROOT,
} WorkloadType1;

struct WorkloadType2 {
  float scan;
  float zipf;
  float b;
  WorkloadType2() {}
};

namespace adsl {
class WL_Matcher {
	map<string, string> patterns;
	void insert(const char * key, const char * value);
public:
	WL_Matcher();
	string match(string path);
};
extern WL_Matcher g_matcher;

map<string, int> req2workload(map<string, int> & reqs);
WorkloadType workload2type(string typestr);
}; // namespace adsl

#endif /* mds/adsl/PathUtil.h */
