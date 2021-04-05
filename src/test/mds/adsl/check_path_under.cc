#include "mds/adsl/ReqTracer.h"

int main(int argc, char * argv[])
{
	string parent = "/aaaaa/bbbbb";
	string child = "/aaaaa/bbbbb/ccccc";
	for (int i = 0; i < 100000; i++) {
		ReqTracer::check_path_under(parent, child);
	}
	return 0;
}
