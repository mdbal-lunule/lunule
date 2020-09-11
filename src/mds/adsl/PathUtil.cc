#include "PathUtil.h"

adsl::WL_Matcher adsl::g_matcher;

void adsl::WL_Matcher::insert(const char * key, const char * value)
{
	patterns.insert(std::make_pair<string, string>(string(key), string(value)));
}

adsl::WL_Matcher::WL_Matcher()
{
	insert("ai", "/ai");
	insert("tar", "/tar");
	insert("ycsb-zipfian", "/ycsbzipf");
	insert("web", "/web");
	insert("fb-zipfian", "/filebench/zipfian/");
}

string adsl::WL_Matcher::match(string path)
{
	if(path.empty()){
		return "root";
	}

	for (auto it = patterns.begin(); it != patterns.end(); it++) {
		if (path.find(it->second) != string::npos) {
			return it->first;
		}
	}
	return "other";
}

map<string, int> adsl::req2workload(map<string, int> & reqs)
{
	map<string, int> cnt_map;
	for (auto it = reqs.begin(); it != reqs.end(); it ++) {
		string workload = g_matcher.match(it->first);
		auto cit = cnt_map.find(workload);
		if (cit != cnt_map.end()) {
			cit->second += it->second;
		}
		else {
			cnt_map.insert(std::make_pair<string, int>(std::move(workload), std::move(it->second)));
		}
	}
	return cnt_map;
}

WorkloadType adsl::workload2type(string typestr)
{
	if (typestr == "ai" || typestr == "tar") {
		return WLT_SCAN;
	}
	else if (typestr == "web" || typestr == "ycsb-zipfian" || typestr == "fb-zipfian") {
		return WLT_ZIPF;
	}else if (typestr == "root")
	{
		return WLT_ROOT;
	}
	return WLT_MIXED;
}
