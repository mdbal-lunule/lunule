// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "osd/osd_types.h"
#include "common/debug.h"
#include "common/Formatter.h"
#include "common/errno.h"
#include "common/TextTable.h"
#include "include/stringify.h"

#include "CrushWrapper.h"
#include "CrushTreeDumper.h"

#define dout_subsys ceph_subsys_crush

bool CrushWrapper::has_legacy_rule_ids() const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    crush_rule *r = crush->rules[i];
    if (r &&
	r->mask.ruleset != i) {
      return true;
    }
  }
  return false;
}

std::map<int, int> CrushWrapper::renumber_rules()
{
  std::map<int, int> result;
  for (unsigned i=0; i<crush->max_rules; i++) {
    crush_rule *r = crush->rules[i];
    if (r && r->mask.ruleset != i) {
      result[r->mask.ruleset] = i;
      r->mask.ruleset = i;
    }
  }
  return result;
}

bool CrushWrapper::has_non_straw2_buckets() const
{
  for (int i=0; i<crush->max_buckets; ++i) {
    crush_bucket *b = crush->buckets[i];
    if (!b)
      continue;
    if (b->alg != CRUSH_BUCKET_STRAW2)
      return true;
  }
  return false;
}

bool CrushWrapper::has_v2_rules() const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    if (is_v2_rule(i)) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::is_v2_rule(unsigned ruleid) const
{
  // check rule for use of indep or new SET_* rule steps
  if (ruleid >= crush->max_rules)
    return false;
  crush_rule *r = crush->rules[ruleid];
  if (!r)
    return false;
  for (unsigned j=0; j<r->len; j++) {
    if (r->steps[j].op == CRUSH_RULE_CHOOSE_INDEP ||
	r->steps[j].op == CRUSH_RULE_CHOOSELEAF_INDEP ||
	r->steps[j].op == CRUSH_RULE_SET_CHOOSE_TRIES ||
	r->steps[j].op == CRUSH_RULE_SET_CHOOSELEAF_TRIES) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::has_v3_rules() const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    if (is_v3_rule(i)) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::is_v3_rule(unsigned ruleid) const
{
  // check rule for use of SET_CHOOSELEAF_VARY_R step
  if (ruleid >= crush->max_rules)
    return false;
  crush_rule *r = crush->rules[ruleid];
  if (!r)
    return false;
  for (unsigned j=0; j<r->len; j++) {
    if (r->steps[j].op == CRUSH_RULE_SET_CHOOSELEAF_VARY_R) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::has_v4_buckets() const
{
  for (int i=0; i<crush->max_buckets; ++i) {
    crush_bucket *b = crush->buckets[i];
    if (!b)
      continue;
    if (b->alg == CRUSH_BUCKET_STRAW2)
      return true;
  }
  return false;
}

bool CrushWrapper::has_v5_rules() const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    if (is_v5_rule(i)) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::is_v5_rule(unsigned ruleid) const
{
  // check rule for use of SET_CHOOSELEAF_STABLE step
  if (ruleid >= crush->max_rules)
    return false;
  crush_rule *r = crush->rules[ruleid];
  if (!r)
    return false;
  for (unsigned j=0; j<r->len; j++) {
    if (r->steps[j].op == CRUSH_RULE_SET_CHOOSELEAF_STABLE) {
      return true;
    }
  }
  return false;
}

bool CrushWrapper::has_choose_args() const
{
  return !choose_args.empty();
}

bool CrushWrapper::has_incompat_choose_args() const
{
  if (choose_args.empty())
    return false;
  if (choose_args.size() > 1)
    return true;
  if (choose_args.begin()->first != DEFAULT_CHOOSE_ARGS)
    return true;
  crush_choose_arg_map arg_map = choose_args.begin()->second;
  for (__u32 i = 0; i < arg_map.size; i++) {
    crush_choose_arg *arg = &arg_map.args[i];
    if (arg->weight_set_size == 0 &&
	arg->ids_size == 0)
	continue;
    if (arg->weight_set_size != 1)
      return true;
    if (arg->ids_size != 0)
      return true;
  }
  return false;
}

int CrushWrapper::split_id_class(int i, int *idout, int *classout) const
{
  if (!item_exists(i))
    return -EINVAL;
  string name = get_item_name(i);
  size_t pos = name.find("~");
  if (pos == string::npos) {
    *idout = i;
    *classout = -1;
    return 0;
  }
  string name_no_class = name.substr(0, pos);
  if (!name_exists(name_no_class))
    return -ENOENT;
  string class_name = name.substr(pos + 1);
  if (!class_exists(class_name))
    return -ENOENT;
  *idout = get_item_id(name_no_class);
  *classout = get_class_id(class_name);
  return 0;
}

int CrushWrapper::can_rename_item(const string& srcname,
                                  const string& dstname,
                                  ostream *ss) const
{
  if (name_exists(srcname)) {
    if (name_exists(dstname)) {
      *ss << "dstname = '" << dstname << "' already exists";
      return -EEXIST;
    }
    if (is_valid_crush_name(dstname)) {
      return 0;
    } else {
      *ss << "dstname = '" << dstname << "' does not match [-_.0-9a-zA-Z]+";
      return -EINVAL;
    }
  } else {
    if (name_exists(dstname)) {
      *ss << "srcname = '" << srcname << "' does not exist "
          << "and dstname = '" << dstname << "' already exists";
      return -EALREADY;
    } else {
      *ss << "srcname = '" << srcname << "' does not exist";
      return -ENOENT;
    }
  }
}

int CrushWrapper::rename_item(const string& srcname,
                              const string& dstname,
                              ostream *ss)
{
  int ret = can_rename_item(srcname, dstname, ss);
  if (ret < 0)
    return ret;
  int oldid = get_item_id(srcname);
  return set_item_name(oldid, dstname);
}

int CrushWrapper::can_rename_bucket(const string& srcname,
                                    const string& dstname,
                                    ostream *ss) const
{
  int ret = can_rename_item(srcname, dstname, ss);
  if (ret)
    return ret;
  int srcid = get_item_id(srcname);
  if (srcid >= 0) {
    *ss << "srcname = '" << srcname << "' is not a bucket "
        << "because its id = " << srcid << " is >= 0";
    return -ENOTDIR;
  }
  return 0;
}

int CrushWrapper::rename_bucket(const string& srcname,
                                const string& dstname,
                                ostream *ss)
{
  int ret = can_rename_bucket(srcname, dstname, ss);
  if (ret < 0)
    return ret;
  int oldid = get_item_id(srcname);
  return set_item_name(oldid, dstname);
}

int CrushWrapper::rename_rule(const string& srcname,
                              const string& dstname,
                              ostream *ss)
{
  if (!rule_exists(srcname)) {
    if (ss) {
      *ss << "source rule name '" << srcname << "' does not exist";
    }
    return -ENOENT;
  }
  if (rule_exists(dstname)) {
    if (ss) {
      *ss << "destination rule name '" << dstname << "' already exists";
    }
    return -EEXIST;
  }
  int rule_id = get_rule_id(srcname);
  auto it = rule_name_map.find(rule_id);
  assert(it != rule_name_map.end());
  it->second = dstname;
  if (have_rmaps) {
    rule_name_rmap.erase(srcname);
    rule_name_rmap[dstname] = rule_id;
  }
  return 0;
}

void CrushWrapper::find_takes(set<int> *roots) const
{
  for (unsigned i=0; i<crush->max_rules; i++) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j=0; j<r->len; j++) {
      if (r->steps[j].op == CRUSH_RULE_TAKE)
	roots->insert(r->steps[j].arg1);
    }
  }
}

void CrushWrapper::find_roots(set<int> *roots) const
{
  for (int i = 0; i < crush->max_buckets; i++) {
    if (!crush->buckets[i])
      continue;
    crush_bucket *b = crush->buckets[i];
    if (!_search_item_exists(b->id))
      roots->insert(b->id);
  }
}

bool CrushWrapper::subtree_contains(int root, int item) const
{
  if (root == item)
    return true;

  if (root >= 0)
    return false;  // root is a leaf

  const crush_bucket *b = get_bucket(root);
  if (IS_ERR(b))
    return false;

  for (unsigned j=0; j<b->size; j++) {
    if (subtree_contains(b->items[j], item))
      return true;
  }
  return false;
}

bool CrushWrapper::_maybe_remove_last_instance(CephContext *cct, int item, bool unlink_only)
{
  // last instance?
  if (_search_item_exists(item)) {
    return false;
  }
  if (item < 0 && _bucket_is_in_use(item)) {
    return false;
  }

  if (item < 0 && !unlink_only) {
    crush_bucket *t = get_bucket(item);
    ldout(cct, 5) << "_maybe_remove_last_instance removing bucket " << item << dendl;
    crush_remove_bucket(crush, t);
    if (class_bucket.count(item) != 0)
      class_bucket.erase(item);
    class_remove_item(item);
  }
  if ((item >= 0 || !unlink_only) && name_map.count(item)) {
    ldout(cct, 5) << "_maybe_remove_last_instance removing name for item " << item << dendl;
    name_map.erase(item);
    have_rmaps = false;
    if (item >= 0 && !unlink_only) {
      class_remove_item(item);
    }
  }
  rebuild_roots_with_classes();
  return true;
}

int CrushWrapper::remove_root(int item)
{
  crush_bucket *b = get_bucket(item);
  if (IS_ERR(b)) {
    // should be idempotent
    // e.g.: we use 'crush link' to link same host into
    // different roots, which as a result can cause different
    // shadow trees reference same hosts too. This means
    // we may need to destory the same buckets(hosts, racks, etc.)
    // multiple times during rebuilding all shadow trees.
    return 0;
  }

  for (unsigned n = 0; n < b->size; n++) {
    if (b->items[n] >= 0)
      continue;
    int r = remove_root(b->items[n]);
    if (r < 0)
      return r;
  }

  crush_remove_bucket(crush, b);
  if (name_map.count(item) != 0) {
    name_map.erase(item);
    have_rmaps = false;
  }
  if (class_bucket.count(item) != 0)
    class_bucket.erase(item);
  class_remove_item(item);
  return 0;
}

int CrushWrapper::remove_item(CephContext *cct, int item, bool unlink_only)
{
  ldout(cct, 5) << "remove_item " << item
		<< (unlink_only ? " unlink_only":"") << dendl;

  int ret = -ENOENT;

  if (item < 0 && !unlink_only) {
    crush_bucket *t = get_bucket(item);
    if (IS_ERR(t)) {
      ldout(cct, 1) << "remove_item bucket " << item << " does not exist"
		    << dendl;
      return -ENOENT;
    }

    if (t->size) {
      ldout(cct, 1) << "remove_item bucket " << item << " has " << t->size
		    << " items, not empty" << dendl;
      return -ENOTEMPTY;
    }
    if (_bucket_is_in_use(item)) {
      return -EBUSY;
    }
  }

  for (int i = 0; i < crush->max_buckets; i++) {
    if (!crush->buckets[i])
      continue;
    crush_bucket *b = crush->buckets[i];

    for (unsigned i=0; i<b->size; ++i) {
      int id = b->items[i];
      if (id == item) {
	ldout(cct, 5) << "remove_item removing item " << item
		      << " from bucket " << b->id << dendl;
	for (auto& p : choose_args) {
	  // weight down each weight-set to 0 before we remove the item
	  vector<int> weightv(get_choose_args_positions(p.second), 0);
	  choose_args_adjust_item_weight(cct, p.second, item, weightv, nullptr);
	}
	bucket_remove_item(b, item);
	adjust_item_weight(cct, b->id, b->weight);
	ret = 0;
      }
    }
  }

  if (_maybe_remove_last_instance(cct, item, unlink_only))
    ret = 0;
  
  return ret;
}

bool CrushWrapper::_search_item_exists(int item) const
{
  for (int i = 0; i < crush->max_buckets; i++) {
    if (!crush->buckets[i])
      continue;
    crush_bucket *b = crush->buckets[i];
    for (unsigned j=0; j<b->size; ++j) {
      if (b->items[j] == item)
	return true;
    }
  }
  return false;
}

bool CrushWrapper::_bucket_is_in_use(int item)
{
  for (auto &i : class_bucket)
    for (auto &j : i.second)
      if (j.second == item)
	return true;
  for (unsigned i = 0; i < crush->max_rules; ++i) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j = 0; j < r->len; ++j) {
      if (r->steps[j].op == CRUSH_RULE_TAKE) {
	int step_item = r->steps[j].arg1;
	int original_item;
	int c;
	int res = split_id_class(step_item, &original_item, &c);
	if (res < 0)
	  return false;
	if (step_item == item || original_item == item)
	  return true;
      }
    }
  }
  return false;
}

int CrushWrapper::_remove_item_under(
  CephContext *cct, int item, int ancestor, bool unlink_only)
{
  ldout(cct, 5) << "_remove_item_under " << item << " under " << ancestor
		<< (unlink_only ? " unlink_only":"") << dendl;

  if (ancestor >= 0) {
    return -EINVAL;
  }

  if (!bucket_exists(ancestor))
    return -EINVAL;

  int ret = -ENOENT;

  crush_bucket *b = get_bucket(ancestor);
  for (unsigned i=0; i<b->size; ++i) {
    int id = b->items[i];
    if (id == item) {
      ldout(cct, 5) << "_remove_item_under removing item " << item
		    << " from bucket " << b->id << dendl;
      for (auto& p : choose_args) {
	// weight down each weight-set to 0 before we remove the item
	vector<int> weightv(get_choose_args_positions(p.second), 0);
	_choose_args_adjust_item_weight_in_bucket(
	  cct, p.second, b->id, item, weightv, nullptr);
      }
      bucket_remove_item(b, item);
      adjust_item_weight(cct, b->id, b->weight);
      ret = 0;
    } else if (id < 0) {
      int r = remove_item_under(cct, item, id, unlink_only);
      if (r == 0)
	ret = 0;
    }
  }
  return ret;
}

int CrushWrapper::remove_item_under(
  CephContext *cct, int item, int ancestor, bool unlink_only)
{
  ldout(cct, 5) << "remove_item_under " << item << " under " << ancestor
		<< (unlink_only ? " unlink_only":"") << dendl;

  if (!unlink_only && _bucket_is_in_use(item)) {
    return -EBUSY;
  }

  int ret = _remove_item_under(cct, item, ancestor, unlink_only);
  if (ret < 0)
    return ret;

  if (item < 0 && !unlink_only) {
    crush_bucket *t = get_bucket(item);
    if (IS_ERR(t)) {
      ldout(cct, 1) << "remove_item_under bucket " << item
                    << " does not exist" << dendl;
      return -ENOENT;
    }

    if (t->size) {
      ldout(cct, 1) << "remove_item_under bucket " << item << " has " << t->size
		    << " items, not empty" << dendl;
      return -ENOTEMPTY;
    }
  }

  if (_maybe_remove_last_instance(cct, item, unlink_only))
    ret = 0;

  return ret;
}

int CrushWrapper::get_common_ancestor_distance(CephContext *cct, int id,
			       const std::multimap<string,string>& loc)
{
  ldout(cct, 5) << __func__ << " " << id << " " << loc << dendl;
  if (!item_exists(id))
    return -ENOENT;
  map<string,string> id_loc = get_full_location(id);
  ldout(cct, 20) << " id is at " << id_loc << dendl;

  for (map<int,string>::const_iterator p = type_map.begin();
       p != type_map.end();
       ++p) {
    map<string,string>::iterator ip = id_loc.find(p->second);
    if (ip == id_loc.end())
      continue;
    for (std::multimap<string,string>::const_iterator q = loc.find(p->second);
	 q != loc.end();
	 ++q) {
      if (q->first != p->second)
	break;
      if (q->second == ip->second)
	return p->first;
    }
  }
  return -ERANGE;
}

int CrushWrapper::parse_loc_map(const std::vector<string>& args,
				std::map<string,string> *ploc)
{
  ploc->clear();
  for (unsigned i = 0; i < args.size(); ++i) {
    const char *s = args[i].c_str();
    const char *pos = strchr(s, '=');
    if (!pos)
      return -EINVAL;
    string key(s, 0, pos-s);
    string value(pos+1);
    if (value.length())
      (*ploc)[key] = value;
    else
      return -EINVAL;
  }
  return 0;
}

int CrushWrapper::parse_loc_multimap(const std::vector<string>& args,
					    std::multimap<string,string> *ploc)
{
  ploc->clear();
  for (unsigned i = 0; i < args.size(); ++i) {
    const char *s = args[i].c_str();
    const char *pos = strchr(s, '=');
    if (!pos)
      return -EINVAL;
    string key(s, 0, pos-s);
    string value(pos+1);
    if (value.length())
      ploc->insert(make_pair(key, value));
    else
      return -EINVAL;
  }
  return 0;
}

bool CrushWrapper::check_item_loc(CephContext *cct, int item, const map<string,string>& loc,
				  int *weight)
{
  ldout(cct, 5) << "check_item_loc item " << item << " loc " << loc << dendl;

  for (map<int,string>::const_iterator p = type_map.begin(); p != type_map.end(); ++p) {
    // ignore device
    if (p->first == 0)
      continue;

    // ignore types that aren't specified in loc
    map<string,string>::const_iterator q = loc.find(p->second);
    if (q == loc.end()) {
      ldout(cct, 2) << "warning: did not specify location for '" << p->second << "' level (levels are "
		    << type_map << ")" << dendl;
      continue;
    }

    if (!name_exists(q->second)) {
      ldout(cct, 5) << "check_item_loc bucket " << q->second << " dne" << dendl;
      return false;
    }

    int id = get_item_id(q->second);
    if (id >= 0) {
      ldout(cct, 5) << "check_item_loc requested " << q->second << " for type " << p->second
		    << " is a device, not bucket" << dendl;
      return false;
    }

    assert(bucket_exists(id));
    crush_bucket *b = get_bucket(id);

    // see if item exists in this bucket
    for (unsigned j=0; j<b->size; j++) {
      if (b->items[j] == item) {
	ldout(cct, 2) << "check_item_loc " << item << " exists in bucket " << b->id << dendl;
	if (weight)
	  *weight = crush_get_bucket_item_weight(b, j);
	return true;
      }
    }
    return false;
  }
  
  ldout(cct, 2) << __func__ << " item " << item << " loc " << loc << dendl;
  return false;
}

map<string, string> CrushWrapper::get_full_location(int id)
{
  vector<pair<string, string> > full_location_ordered;
  map<string,string> full_location;

  get_full_location_ordered(id, full_location_ordered);

  std::copy(full_location_ordered.begin(),
      full_location_ordered.end(),
      std::inserter(full_location, full_location.begin()));

  return full_location;
}

int CrushWrapper::get_full_location_ordered(int id, vector<pair<string, string> >& path)
{
  if (!item_exists(id))
    return -ENOENT;
  int cur = id;
  int ret;
  while (true) {
    pair<string, string> parent_coord = get_immediate_parent(cur, &ret);
    if (ret != 0)
      break;
    path.push_back(parent_coord);
    cur = get_item_id(parent_coord.second);
  }
  return 0;
}

string CrushWrapper::get_full_location_ordered_string(int id)
{
  vector<pair<string, string> > full_location_ordered;
  string full_location;
  get_full_location_ordered(id, full_location_ordered);
  reverse(begin(full_location_ordered), end(full_location_ordered));
  for(auto i = full_location_ordered.begin(); i != full_location_ordered.end(); i++) {
    full_location = full_location + i->first + "=" + i->second;
    if (i != full_location_ordered.end() - 1) {
      full_location = full_location + ",";
    }
  }
  return full_location;
}

map<int, string> CrushWrapper::get_parent_hierarchy(int id)
{
  map<int,string> parent_hierarchy;
  pair<string, string> parent_coord = get_immediate_parent(id);
  int parent_id;

  // get the integer type for id and create a counter from there
  int type_counter = get_bucket_type(id);

  // if we get a negative type then we can assume that we have an OSD
  // change behavior in get_item_type FIXME
  if (type_counter < 0)
    type_counter = 0;

  // read the type map and get the name of the type with the largest ID
  int high_type = 0;
  for (map<int, string>::iterator it = type_map.begin(); it != type_map.end(); ++it){
    if ( (*it).first > high_type )
      high_type = (*it).first;
  }

  parent_id = get_item_id(parent_coord.second);

  while (type_counter < high_type) {
    type_counter++;
    parent_hierarchy[ type_counter ] = parent_coord.first;

    if (type_counter < high_type){
      // get the coordinate information for the next parent
      parent_coord = get_immediate_parent(parent_id);
      parent_id = get_item_id(parent_coord.second);
    }
  }

  return parent_hierarchy;
}

int CrushWrapper::get_children(int id, list<int> *children)
{
  // leaf?
  if (id >= 0) {
    return 0;
  }

  crush_bucket *b = get_bucket(id);
  if (IS_ERR(b)) {
    return -ENOENT;
  }

  for (unsigned n=0; n<b->size; n++) {
    children->push_back(b->items[n]);
  }
  return b->size;
}

int CrushWrapper::get_rule_failure_domain(int rule_id)
{
  crush_rule *rule = get_rule(rule_id);
  if (IS_ERR(rule)) {
    return -ENOENT;
  }
  int type = 0; // default to osd-level
  for (unsigned s = 0; s < rule->len; ++s) {
    if ((rule->steps[s].op == CRUSH_RULE_CHOOSE_FIRSTN ||
         rule->steps[s].op == CRUSH_RULE_CHOOSE_INDEP ||
         rule->steps[s].op == CRUSH_RULE_CHOOSELEAF_FIRSTN ||
         rule->steps[s].op == CRUSH_RULE_CHOOSELEAF_INDEP) &&
         rule->steps[s].arg2 > type) {
      type = rule->steps[s].arg2;
    }
  }
  return type;
}

int CrushWrapper::_get_leaves(int id, list<int> *leaves)
{
  assert(leaves);

  // Already leaf?
  if (id >= 0) {
    leaves->push_back(id);
    return 0;
  }

  crush_bucket *b = get_bucket(id);
  if (IS_ERR(b)) {
    return -ENOENT;
  }

  for (unsigned n = 0; n < b->size; n++) {
    if (b->items[n] >= 0) {
      leaves->push_back(b->items[n]);
    } else {
      // is a bucket, do recursive call
      int r = _get_leaves(b->items[n], leaves);
      if (r < 0) {
        return r;
      }
    }
  }

  return 0; // all is well
}

int CrushWrapper::get_leaves(const string &name, set<int> *leaves)
{
  assert(leaves);
  leaves->clear();

  if (!name_exists(name)) {
    return -ENOENT;
  }

  int id = get_item_id(name);
  if (id >= 0) {
    // already leaf
    leaves->insert(id);
    return 0;
  }

  list<int> unordered;
  int r = _get_leaves(id, &unordered);
  if (r < 0) {
    return r;
  }

  for (auto &p : unordered) {
    leaves->insert(p);
  }

  return 0;
}

int CrushWrapper::insert_item(
  CephContext *cct, int item, float weight, string name,
  const map<string,string>& loc)  // typename -> bucketname
{
  ldout(cct, 5) << "insert_item item " << item << " weight " << weight
		<< " name " << name << " loc " << loc << dendl;

  if (!is_valid_crush_name(name))
    return -EINVAL;

  if (!is_valid_crush_loc(cct, loc))
    return -EINVAL;

  int r = validate_weightf(weight);
  if (r < 0) {
    return r;
  }

  if (name_exists(name)) {
    if (get_item_id(name) != item) {
      ldout(cct, 10) << "device name '" << name << "' already exists as id "
		     << get_item_id(name) << dendl;
      return -EEXIST;
    }
  } else {
    set_item_name(item, name);
  }

  int cur = item;

  // create locations if locations don't exist and add child in
  // location with 0 weight the more detail in the insert_item method
  // declaration in CrushWrapper.h
  for (auto p = type_map.begin(); p != type_map.end(); ++p) {
    // ignore device type
    if (p->first == 0)
      continue;

    // skip types that are unspecified
    map<string,string>::const_iterator q = loc.find(p->second);
    if (q == loc.end()) {
      ldout(cct, 2) << "warning: did not specify location for '"
		    << p->second << "' level (levels are "
		    << type_map << ")" << dendl;
      continue;
    }

    if (!name_exists(q->second)) {
      ldout(cct, 5) << "insert_item creating bucket " << q->second << dendl;
      int empty = 0, newid;
      int r = add_bucket(0, 0,
			 CRUSH_HASH_DEFAULT, p->first, 1, &cur, &empty, &newid);
      if (r < 0) {
        ldout(cct, 1) << "add_bucket failure error: " << cpp_strerror(r)
		      << dendl;
        return r;
      }
      set_item_name(newid, q->second);
      
      cur = newid;
      continue;
    }

    // add to an existing bucket
    int id = get_item_id(q->second);
    if (!bucket_exists(id)) {
      ldout(cct, 1) << "insert_item doesn't have bucket " << id << dendl;
      return -EINVAL;
    }

    // check that we aren't creating a cycle.
    if (subtree_contains(id, cur)) {
      ldout(cct, 1) << "insert_item item " << cur << " already exists beneath "
		    << id << dendl;
      return -EINVAL;
    }

    // we have done sanity check above
    crush_bucket *b = get_bucket(id);

    if (p->first != b->type) {
      ldout(cct, 1) << "insert_item existing bucket has type "
	<< "'" << type_map[b->type] << "' != "
	<< "'" << type_map[p->first] << "'" << dendl;
      return -EINVAL;
    }

    // are we forming a loop?
    if (subtree_contains(cur, b->id)) {
      ldout(cct, 1) << "insert_item " << cur << " already contains " << b->id
		    << "; cannot form loop" << dendl;
      return -ELOOP;
    }

    ldout(cct, 5) << "insert_item adding " << cur << " weight " << weight
		  << " to bucket " << id << dendl;
    int r = bucket_add_item(b, cur, 0);
    assert (!r);
    break;
  }

  // adjust the item's weight in location
  if (adjust_item_weightf_in_loc(cct, item, weight, loc) > 0) {
    if (item >= crush->max_devices) {
      crush->max_devices = item + 1;
      ldout(cct, 5) << "insert_item max_devices now " << crush->max_devices
		    << dendl;
    }
    r = rebuild_roots_with_classes();
    if (r < 0) {
      ldout(cct, 0) << __func__ << " unable to rebuild roots with classes: "
                    << cpp_strerror(r) << dendl;
      return r;
    }
    return 0;
  }

  ldout(cct, 1) << "error: didn't find anywhere to add item " << item
		<< " in " << loc << dendl;
  return -EINVAL;
}


int CrushWrapper::move_bucket(
  CephContext *cct, int id, const map<string,string>& loc)
{
  // sorry this only works for buckets
  if (id >= 0)
    return -EINVAL;

  if (!item_exists(id))
    return -ENOENT;

  // get the name of the bucket we are trying to move for later
  string id_name = get_item_name(id);

  // detach the bucket
  int bucket_weight = detach_bucket(cct, id);

  // insert the bucket back into the hierarchy
  return insert_item(cct, id, bucket_weight / (float)0x10000, id_name, loc);
}

int CrushWrapper::detach_bucket(CephContext *cct, int item)
{
  if (!crush)
    return (-EINVAL);

  if (item >= 0)
    return (-EINVAL);

  // check that the bucket that we want to detach exists
  assert(bucket_exists(item));

  // get the bucket's weight
  crush_bucket *b = get_bucket(item);
  unsigned bucket_weight = b->weight;

  // get where the bucket is located
  pair<string, string> bucket_location = get_immediate_parent(item);

  // get the id of the parent bucket
  int parent_id = get_item_id(bucket_location.second);

  // get the parent bucket
  crush_bucket *parent_bucket = get_bucket(parent_id);

  if (!IS_ERR(parent_bucket)) {
    // zero out the bucket weight
    bucket_adjust_item_weight(cct, parent_bucket, item, 0);
    adjust_item_weight(cct, parent_bucket->id, parent_bucket->weight);
    for (auto& p : choose_args) {
      // weight down each weight-set to 0 before we remove the item
      vector<int> weightv(get_choose_args_positions(p.second), 0);
      choose_args_adjust_item_weight(cct, p.second, item, weightv, nullptr);
    }

    // remove the bucket from the parent
    bucket_remove_item(parent_bucket, item);
  } else if (PTR_ERR(parent_bucket) != -ENOENT) {
    return PTR_ERR(parent_bucket);
  }

  // check that we're happy
  int test_weight = 0;
  map<string,string> test_location;
  test_location[ bucket_location.first ] = (bucket_location.second);

  bool successful_detach = !(check_item_loc(cct, item, test_location,
					    &test_weight));
  assert(successful_detach);
  assert(test_weight == 0);

  return bucket_weight;
}

int CrushWrapper::swap_bucket(CephContext *cct, int src, int dst)
{
  if (src >= 0 || dst >= 0)
    return -EINVAL;
  if (!item_exists(src) || !item_exists(dst))
    return -EINVAL;
  crush_bucket *a = get_bucket(src);
  crush_bucket *b = get_bucket(dst);
  unsigned aw = a->weight;
  unsigned bw = b->weight;

  // swap weights
  adjust_item_weight(cct, a->id, bw);
  adjust_item_weight(cct, b->id, aw);

  // swap items
  map<int,unsigned> tmp;
  unsigned as = a->size;
  unsigned bs = b->size;
  for (unsigned i = 0; i < as; ++i) {
    int item = a->items[0];
    int itemw = crush_get_bucket_item_weight(a, 0);
    tmp[item] = itemw;
    bucket_remove_item(a, item);
  }
  assert(a->size == 0);
  assert(b->size == bs);
  for (unsigned i = 0; i < bs; ++i) {
    int item = b->items[0];
    int itemw = crush_get_bucket_item_weight(b, 0);
    bucket_remove_item(b, item);
    bucket_add_item(a, item, itemw);
  }
  assert(a->size == bs);
  assert(b->size == 0);
  for (auto t : tmp) {
    bucket_add_item(b, t.first, t.second);
  }
  assert(a->size == bs);
  assert(b->size == as);

  // swap names
  swap_names(src, dst);
  return rebuild_roots_with_classes();
}

int CrushWrapper::link_bucket(
  CephContext *cct, int id, const map<string,string>& loc)
{
  // sorry this only works for buckets
  if (id >= 0)
    return -EINVAL;

  if (!item_exists(id))
    return -ENOENT;

  // get the name of the bucket we are trying to move for later
  string id_name = get_item_name(id);

  crush_bucket *b = get_bucket(id);
  unsigned bucket_weight = b->weight;

  return insert_item(cct, id, bucket_weight / (float)0x10000, id_name, loc);
}

int CrushWrapper::create_or_move_item(
  CephContext *cct, int item, float weight, string name,
  const map<string,string>& loc)  // typename -> bucketname
{
  int ret = 0;
  int old_iweight;

  if (!is_valid_crush_name(name))
    return -EINVAL;

  if (check_item_loc(cct, item, loc, &old_iweight)) {
    ldout(cct, 5) << "create_or_move_item " << item << " already at " << loc
		  << dendl;
  } else {
    if (_search_item_exists(item)) {
      weight = get_item_weightf(item);
      ldout(cct, 10) << "create_or_move_item " << item
		     << " exists with weight " << weight << dendl;
      remove_item(cct, item, true);
    }
    ldout(cct, 5) << "create_or_move_item adding " << item
		  << " weight " << weight
		  << " at " << loc << dendl;
    ret = insert_item(cct, item, weight, name, loc);
    if (ret == 0)
      ret = 1;  // changed
  }
  return ret;
}

int CrushWrapper::update_item(
  CephContext *cct, int item, float weight, string name,
  const map<string,string>& loc)  // typename -> bucketname
{
  ldout(cct, 5) << "update_item item " << item << " weight " << weight
		<< " name " << name << " loc " << loc << dendl;
  int ret = 0;

  if (!is_valid_crush_name(name))
    return -EINVAL;

  if (!is_valid_crush_loc(cct, loc))
    return -EINVAL;

  ret = validate_weightf(weight);
  if (ret < 0) {
    return ret;
  }

  // compare quantized (fixed-point integer) weights!  
  int iweight = (int)(weight * (float)0x10000);
  int old_iweight;
  if (check_item_loc(cct, item, loc, &old_iweight)) {
    ldout(cct, 5) << "update_item " << item << " already at " << loc << dendl;
    if (old_iweight != iweight) {
      ldout(cct, 5) << "update_item " << item << " adjusting weight "
		    << ((float)old_iweight/(float)0x10000) << " -> " << weight
		    << dendl;
      adjust_item_weight_in_loc(cct, item, iweight, loc);
      ret = 1;
    }
    if (get_item_name(item) != name) {
      ldout(cct, 5) << "update_item setting " << item << " name to " << name
		    << dendl;
      set_item_name(item, name);
      ret = 1;
    }
  } else {
    if (item_exists(item)) {
      remove_item(cct, item, true);
    }
    ldout(cct, 5) << "update_item adding " << item << " weight " << weight
		  << " at " << loc << dendl;
    ret = insert_item(cct, item, weight, name, loc);
    if (ret == 0)
      ret = 1;  // changed
  }
  return ret;
}

int CrushWrapper::get_item_weight(int id) const
{
  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == NULL)
      continue;
    if (b->id == id)
      return b->weight;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id)
	return crush_get_bucket_item_weight(b, i);
  }
  return -ENOENT;
}

int CrushWrapper::get_item_weight_in_loc(int id, const map<string,string> &loc)
{
  for (map<string,string>::const_iterator l = loc.begin(); l != loc.end(); ++l) {

    int bid = get_item_id(l->second);
    if (!bucket_exists(bid))
      continue;
    crush_bucket *b = get_bucket(bid);
    for (unsigned int i = 0; i < b->size; i++) {
      if (b->items[i] == id) {
	return crush_get_bucket_item_weight(b, i);
      }
    }
  }
  return -ENOENT;
}

int CrushWrapper::adjust_item_weight(CephContext *cct, int id, int weight)
{
  ldout(cct, 5) << "adjust_item_weight " << id << " weight " << weight << dendl;
  int changed = 0;
  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    for (unsigned i = 0; i < b->size; i++) {
      if (b->items[i] == id) {
	int diff = bucket_adjust_item_weight(cct, b, id, weight);
	ldout(cct, 5) << "adjust_item_weight " << id << " diff " << diff
		      << " in bucket " << bidx << dendl;
	adjust_item_weight(cct, -1 - bidx, b->weight);
	changed++;
      }
    }
  }
  if (!changed)
    return -ENOENT;
  return changed;
}

int CrushWrapper::adjust_item_weight_in_loc(CephContext *cct, int id, int weight, const map<string,string>& loc)
{
  ldout(cct, 5) << "adjust_item_weight_in_loc " << id << " weight " << weight
		<< " in " << loc << dendl;
  int changed = 0;

  for (auto l = loc.begin(); l != loc.end(); ++l) {
    int bid = get_item_id(l->second);
    if (!bucket_exists(bid))
      continue;
    crush_bucket *b = get_bucket(bid);
    for (unsigned int i = 0; i < b->size; i++) {
      if (b->items[i] == id) {
	int diff = bucket_adjust_item_weight(cct, b, id, weight);
	ldout(cct, 5) << "adjust_item_weight_in_loc " << id << " diff " << diff
		      << " in bucket " << bid << dendl;
	adjust_item_weight(cct, bid, b->weight);
	changed++;
      }
    }
  }
  if (!changed)
    return -ENOENT;
  return changed;
}

int CrushWrapper::adjust_subtree_weight(CephContext *cct, int id, int weight)
{
  ldout(cct, 5) << __func__ << " " << id << " weight " << weight << dendl;
  crush_bucket *b = get_bucket(id);
  if (IS_ERR(b))
    return PTR_ERR(b);
  int changed = 0;
  list<crush_bucket*> q;
  q.push_back(b);
  while (!q.empty()) {
    b = q.front();
    q.pop_front();
    int local_changed = 0;
    for (unsigned i=0; i<b->size; ++i) {
      int n = b->items[i];
      if (n >= 0) {
	bucket_adjust_item_weight(cct, b, n, weight);
	++changed;
	++local_changed;
      } else {
	crush_bucket *sub = get_bucket(n);
	if (IS_ERR(sub))
	  continue;
	q.push_back(sub);
      }
    }
    if (local_changed) {
      adjust_item_weight(cct, b->id, b->weight);
    }
  }
  return changed;
}

bool CrushWrapper::check_item_present(int id) const
{
  bool found = false;

  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id)
	found = true;
  }
  return found;
}


pair<string,string> CrushWrapper::get_immediate_parent(int id, int *_ret)
{

  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
   if (is_shadow_item(b->id))
      continue;
    for (unsigned i = 0; i < b->size; i++)
      if (b->items[i] == id) {
        string parent_id = name_map[b->id];
        string parent_bucket_type = type_map[b->type];
        if (_ret)
          *_ret = 0;
        return make_pair(parent_bucket_type, parent_id);
      }
  }

  if (_ret)
    *_ret = -ENOENT;

  return pair<string, string>();
}

int CrushWrapper::get_immediate_parent_id(int id, int *parent) const
{
  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == 0)
      continue;
    if (is_shadow_item(b->id))
      continue;
    for (unsigned i = 0; i < b->size; i++) {
      if (b->items[i] == id) {
	*parent = b->id;
	return 0;
      }
    }
  }
  return -ENOENT;
}

int CrushWrapper::get_parent_of_type(int item, int type) const
{
  do {
    int r = get_immediate_parent_id(item, &item);
    if (r < 0) {
      return 0;
    }
  } while (get_bucket_type(item) != type);
  return item;
}

int CrushWrapper::rename_class(const string& srcname, const string& dstname)
{
  auto i = class_rname.find(srcname);
  if (i == class_rname.end())
    return -ENOENT;
  auto j = class_rname.find(dstname);
  if (j != class_rname.end())
    return -EEXIST;

  int class_id = i->second;
  assert(class_name.count(class_id));
  // rename any shadow buckets of old class name
  for (auto &it: class_map) {
    if (it.first < 0 && it.second == class_id) {
        string old_name = get_item_name(it.first);
        size_t pos = old_name.find("~");
        assert(pos != string::npos);
        string name_no_class = old_name.substr(0, pos);
        string old_class_name = old_name.substr(pos + 1);
        assert(old_class_name == srcname);
        string new_name = name_no_class + "~" + dstname;
        // we do not use set_item_name
        // because the name is intentionally invalid
        name_map[it.first] = new_name;
        have_rmaps = false;
    }
  }

  // rename class
  class_rname.erase(srcname);
  class_name.erase(class_id);
  class_rname[dstname] = class_id;
  class_name[class_id] = dstname;
  return 0;
}

int CrushWrapper::populate_classes(
  const std::map<int32_t, map<int32_t, int32_t>>& old_class_bucket)
{
  // build set of previous used shadow ids
  set<int32_t> used_ids;
  for (auto& p : old_class_bucket) {
    for (auto& q : p.second) {
      used_ids.insert(q.second);
    }
  }
  // accumulate weight values for each carg and bucket as we go. because it is
  // depth first, we will have the nested bucket weights we need when we
  // finish constructing the containing buckets.
  map<int,map<int,vector<int>>> cmap_item_weight; // cargs -> bno -> weights
  set<int> roots;
  find_nonshadow_roots(&roots);
  for (auto &r : roots) {
    if (r >= 0)
      continue;
    for (auto &c : class_name) {
      int clone;
      int res = device_class_clone(r, c.first, old_class_bucket, used_ids,
				   &clone, &cmap_item_weight);
      if (res < 0)
	return res;
    }
  }
  return 0;
}

int CrushWrapper::trim_roots_with_class()
{
  set<int> roots;
  find_shadow_roots(&roots);
  for (auto &r : roots) {
    if (r >= 0)
      continue;
    int res = remove_root(r);
    if (res)
      return res;
  }
  // there is no need to reweight because we only remove from the
  // root and down
  return 0;
}

int32_t CrushWrapper::_alloc_class_id() const {
  if (class_name.empty()) {
    return 0;
  }
  int32_t class_id = class_name.rbegin()->first + 1;
  if (class_id >= 0) {
    return class_id;
  }
  // wrapped, pick a random start and do exhaustive search
  uint32_t upperlimit = numeric_limits<int32_t>::max();
  upperlimit++;
  class_id = rand() % upperlimit;
  const auto start = class_id;
  do {
    if (!class_name.count(class_id)) {
      return class_id;
    } else {
      class_id++;
      if (class_id < 0) {
        class_id = 0;
      }
    }
  } while (class_id != start);
  assert(0 == "no available class id");
}

void CrushWrapper::reweight(CephContext *cct)
{
  set<int> roots;
  find_roots(&roots);
  for (set<int>::iterator p = roots.begin(); p != roots.end(); ++p) {
    if (*p >= 0)
      continue;
    crush_bucket *b = get_bucket(*p);
    ldout(cct, 5) << "reweight bucket " << *p << dendl;
    int r = crush_reweight_bucket(crush, b);
    assert(r == 0);
  }
}

int CrushWrapper::add_simple_rule_at(
  string name, string root_name,
  string failure_domain_name,
  string device_class,
  string mode, int rule_type,
  int rno,
  ostream *err)
{
  if (rule_exists(name)) {
    if (err)
      *err << "rule " << name << " exists";
    return -EEXIST;
  }
  if (rno >= 0) {
    if (rule_exists(rno)) {
      if (err)
        *err << "rule with ruleno " << rno << " exists";
      return -EEXIST;
    }
    if (ruleset_exists(rno)) {
      if (err)
        *err << "ruleset " << rno << " exists";
      return -EEXIST;
    }
  } else {
    for (rno = 0; rno < get_max_rules(); rno++) {
      if (!rule_exists(rno) && !ruleset_exists(rno))
        break;
    }
  }
  if (!name_exists(root_name)) {
    if (err)
      *err << "root item " << root_name << " does not exist";
    return -ENOENT;
  }
  int root = get_item_id(root_name);
  int type = 0;
  if (failure_domain_name.length()) {
    type = get_type_id(failure_domain_name);
    if (type < 0) {
      if (err)
	*err << "unknown type " << failure_domain_name;
      return -EINVAL;
    }
  }
  if (device_class.size()) {
    if (!class_exists(device_class)) {
      if (err)
	*err << "device class " << device_class << " does not exist";
      return -EINVAL;
    }
    int c = get_class_id(device_class);
    if (class_bucket.count(root) == 0 ||
	class_bucket[root].count(c) == 0) {
      if (err)
	*err << "root " << root_name << " has no devices with class "
	     << device_class;
      return -EINVAL;
    }
    root = class_bucket[root][c];
  }
  if (mode != "firstn" && mode != "indep") {
    if (err)
      *err << "unknown mode " << mode;
    return -EINVAL;
  }

  int steps = 3;
  if (mode == "indep")
    steps = 5;
  int min_rep = mode == "firstn" ? 1 : 3;
  int max_rep = mode == "firstn" ? 10 : 20;
  //set the ruleset the same as rule_id(rno)
  crush_rule *rule = crush_make_rule(steps, rno, rule_type, min_rep, max_rep);
  assert(rule);
  int step = 0;
  if (mode == "indep") {
    crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSELEAF_TRIES, 5, 0);
    crush_rule_set_step(rule, step++, CRUSH_RULE_SET_CHOOSE_TRIES, 100, 0);
  }
  crush_rule_set_step(rule, step++, CRUSH_RULE_TAKE, root, 0);
  if (type)
    crush_rule_set_step(rule, step++,
			mode == "firstn" ? CRUSH_RULE_CHOOSELEAF_FIRSTN :
			CRUSH_RULE_CHOOSELEAF_INDEP,
			CRUSH_CHOOSE_N,
			type);
  else
    crush_rule_set_step(rule, step++,
			mode == "firstn" ? CRUSH_RULE_CHOOSE_FIRSTN :
			CRUSH_RULE_CHOOSE_INDEP,
			CRUSH_CHOOSE_N,
			0);
  crush_rule_set_step(rule, step++, CRUSH_RULE_EMIT, 0, 0);

  int ret = crush_add_rule(crush, rule, rno);
  if(ret < 0) {
    *err << "failed to add rule " << rno << " because " << cpp_strerror(ret);
    return ret;
  }
  set_rule_name(rno, name);
  have_rmaps = false;
  return rno;
}

int CrushWrapper::add_simple_rule(
  string name, string root_name,
  string failure_domain_name,
  string device_class,
  string mode, int rule_type,
  ostream *err)
{
  return add_simple_rule_at(name, root_name, failure_domain_name, device_class,
			    mode,
			    rule_type, -1, err);
}

float CrushWrapper::_get_take_weight_osd_map(int root,
					     map<int,float> *pmap) const
{
  float sum = 0.0;
  list<int> q;
  q.push_back(root);
  //breadth first iterate the OSD tree
  while (!q.empty()) {
    int bno = q.front();
    q.pop_front();
    crush_bucket *b = crush->buckets[-1-bno];
    assert(b);
    for (unsigned j=0; j<b->size; ++j) {
      int item_id = b->items[j];
      if (item_id >= 0) { //it's an OSD
	float w = crush_get_bucket_item_weight(b, j);
	(*pmap)[item_id] = w;
	sum += w;
      } else { //not an OSD, expand the child later
	q.push_back(item_id);
      }
    }
  }
  return sum;
}

void CrushWrapper::_normalize_weight_map(float sum,
					 const map<int,float>& m,
					 map<int,float> *pmap) const
{
  for (auto& p : m) {
    map<int,float>::iterator q = pmap->find(p.first);
    if (q == pmap->end()) {
      (*pmap)[p.first] = p.second / sum;
    } else {
      q->second += p.second / sum;
    }
  }
}

int CrushWrapper::get_take_weight_osd_map(int root, map<int,float> *pmap) const
{
  map<int,float> m;
  float sum = _get_take_weight_osd_map(root, &m);
  _normalize_weight_map(sum, m, pmap);
  return 0;
}

int CrushWrapper::get_rule_weight_osd_map(unsigned ruleno,
					  map<int,float> *pmap) const
{
  if (ruleno >= crush->max_rules)
    return -ENOENT;
  if (crush->rules[ruleno] == NULL)
    return -ENOENT;
  crush_rule *rule = crush->rules[ruleno];

  // build a weight map for each TAKE in the rule, and then merge them

  // FIXME: if there are multiple takes that place a different number of
  // objects we do not take that into account.  (Also, note that doing this
  // right is also a function of the pool, since the crush rule
  // might choose 2 + choose 2 but pool size may only be 3.)
  for (unsigned i=0; i<rule->len; ++i) {
    map<int,float> m;
    float sum = 0;
    if (rule->steps[i].op == CRUSH_RULE_TAKE) {
      int n = rule->steps[i].arg1;
      if (n >= 0) {
	m[n] = 1.0;
	sum = 1.0;
      } else {
	sum += _get_take_weight_osd_map(n, &m);
      }
    }
    _normalize_weight_map(sum, m, pmap);
  }

  return 0;
}

int CrushWrapper::remove_rule(int ruleno)
{
  if (ruleno >= (int)crush->max_rules)
    return -ENOENT;
  if (crush->rules[ruleno] == NULL)
    return -ENOENT;
  crush_destroy_rule(crush->rules[ruleno]);
  crush->rules[ruleno] = NULL;
  rule_name_map.erase(ruleno);
  have_rmaps = false;
  return rebuild_roots_with_classes();
}

int CrushWrapper::bucket_adjust_item_weight(CephContext *cct, crush_bucket *bucket, int item, int weight)
{
  if (cct->_conf->osd_crush_update_weight_set) {
    unsigned position;
    for (position = 0; position < bucket->size; position++)
      if (bucket->items[position] == item)
	break;
    assert(position != bucket->size);
    for (auto &w : choose_args) {
      crush_choose_arg_map &arg_map = w.second;
      crush_choose_arg *arg = &arg_map.args[-1-bucket->id];
      for (__u32 j = 0; j < arg->weight_set_size; j++) {
	crush_weight_set *weight_set = &arg->weight_set[j];
	weight_set->weights[position] = weight;
      }
    }
  }
  return crush_bucket_adjust_item_weight(crush, bucket, item, weight);
}

int CrushWrapper::add_bucket(
  int bucketno, int alg, int hash, int type, int size,
  int *items, int *weights, int *idout)
{
  if (alg == 0) {
    alg = get_default_bucket_alg();
    if (alg == 0)
      return -EINVAL;
  }
  crush_bucket *b = crush_make_bucket(crush, alg, hash, type, size, items,
				      weights);
  assert(b);
  assert(idout);
  int r = crush_add_bucket(crush, bucketno, b, idout);
  int pos = -1 - *idout;
  for (auto& p : choose_args) {
    crush_choose_arg_map& cmap = p.second;
    if (cmap.args) {
      if ((int)cmap.size <= pos) {
	cmap.args = (crush_choose_arg*)realloc(
	  cmap.args,
	  sizeof(crush_choose_arg) * (pos + 1));
        assert(cmap.args);
	memset(&cmap.args[cmap.size], 0,
	       sizeof(crush_choose_arg) * (pos + 1 - cmap.size));
	cmap.size = pos + 1;
      }
    } else {
      cmap.args = (crush_choose_arg*)calloc(sizeof(crush_choose_arg),
					    pos + 1);
      assert(cmap.args);
      cmap.size = pos + 1;
    }
    if (size > 0) {
      int positions = get_choose_args_positions(cmap);
      crush_choose_arg& carg = cmap.args[pos];
      carg.weight_set = (crush_weight_set*)calloc(sizeof(crush_weight_set),
						  size);
      carg.weight_set_size = positions;
      for (int ppos = 0; ppos < positions; ++ppos) {
	carg.weight_set[ppos].weights = (__u32*)calloc(sizeof(__u32), size);
	carg.weight_set[ppos].size = size;
	for (int bpos = 0; bpos < size; ++bpos) {
	  carg.weight_set[ppos].weights[bpos] = weights[bpos];
	}
      }
    }
  }
  return r;
}

int CrushWrapper::bucket_add_item(crush_bucket *bucket, int item, int weight)
{
  __u32 new_size = bucket->size + 1;
  int r = crush_bucket_add_item(crush, bucket, item, weight);
  if (r < 0) {
    return r;
  }
  for (auto &w : choose_args) {
    crush_choose_arg_map &arg_map = w.second;
    crush_choose_arg *arg = &arg_map.args[-1-bucket->id];
    for (__u32 j = 0; j < arg->weight_set_size; j++) {
      crush_weight_set *weight_set = &arg->weight_set[j];
      weight_set->weights = (__u32*)realloc(weight_set->weights,
					    new_size * sizeof(__u32));
      assert(weight_set->size + 1 == new_size);
      weight_set->weights[weight_set->size] = weight;
      weight_set->size = new_size;
    }
    if (arg->ids_size) {
      arg->ids = (__s32 *)realloc(arg->ids, new_size * sizeof(__s32));
      assert(arg->ids_size + 1 == new_size);
      arg->ids[arg->ids_size] = item;
      arg->ids_size = new_size;
    }
  }
  return 0;
}

int CrushWrapper::bucket_remove_item(crush_bucket *bucket, int item)
{
  __u32 new_size = bucket->size - 1;
  unsigned position;
  for (position = 0; position < bucket->size; position++)
    if (bucket->items[position] == item)
      break;
  assert(position != bucket->size);
  int r = crush_bucket_remove_item(crush, bucket, item);
  if (r < 0) {
    return r;
  }
  for (auto &w : choose_args) {
    crush_choose_arg_map &arg_map = w.second;
    crush_choose_arg *arg = &arg_map.args[-1-bucket->id];
    for (__u32 j = 0; j < arg->weight_set_size; j++) {
      crush_weight_set *weight_set = &arg->weight_set[j];
      assert(weight_set->size - 1 == new_size);
      for (__u32 k = position; k < new_size; k++)
	weight_set->weights[k] = weight_set->weights[k+1];
      if (new_size) {
	weight_set->weights = (__u32*)realloc(weight_set->weights,
					      new_size * sizeof(__u32));
      } else {
	weight_set->weights = NULL;
      }
      weight_set->size = new_size;
    }
    if (arg->ids_size) {
      assert(arg->ids_size - 1 == new_size);
      for (__u32 k = position; k < new_size; k++)
	arg->ids[k] = arg->ids[k+1];
      if (new_size) {
	arg->ids = (__s32 *)realloc(arg->ids, new_size * sizeof(__s32));
      } else {
	arg->ids = NULL;
      }
      arg->ids_size = new_size;
    }
  }
  return 0;
}

int CrushWrapper::bucket_set_alg(int bid, int alg)
{
  crush_bucket *b = get_bucket(bid);
  if (!b) {
    return -ENOENT;
  }
  b->alg = alg;
  return 0;
}

int CrushWrapper::update_device_class(int id,
                                      const string& class_name,
                                      const string& name,
                                      ostream *ss)
{
  assert(item_exists(id));
  auto old_class_name = get_item_class(id);
  if (old_class_name && old_class_name != class_name) {
    *ss << "osd." << id << " has already bound to class '" << old_class_name
        << "', can not reset class to '" << class_name  << "'; "
        << "use 'ceph osd crush rm-device-class <osd>' to "
        << "remove old class first";
    return -EBUSY;
  }

  int class_id = get_or_create_class_id(class_name);
  if (id < 0) {
    *ss << name << " id " << id << " is negative";
    return -EINVAL;
  }

  if (class_map.count(id) != 0 && class_map[id] == class_id) {
    *ss << name << " already set to class " << class_name;
    return 0;
  }

  set_item_class(id, class_id);

  int r = rebuild_roots_with_classes();
  if (r < 0)
    return r;
  return 1;
}

int CrushWrapper::remove_device_class(CephContext *cct, int id, ostream *ss)
{
  assert(ss);
  const char *name = get_item_name(id);
  if (!name) {
    *ss << "osd." << id << " does not have a name";
    return -ENOENT;
  }

  const char *class_name = get_item_class(id);
  if (!class_name) {
    *ss << "osd." << id << " has not been bound to a specific class yet";
    return 0;
  }
  class_remove_item(id);

  int r = rebuild_roots_with_classes();
  if (r < 0) {
    *ss << "unable to rebuild roots with class '" << class_name << "' "
        << "of osd." << id << ": " << cpp_strerror(r);
    return r;
  }
  return 0;
}

int CrushWrapper::device_class_clone(
  int original_id, int device_class,
  const std::map<int32_t, map<int32_t, int32_t>>& old_class_bucket,
  const std::set<int32_t>& used_ids,
  int *clone,
  map<int,map<int,vector<int>>> *cmap_item_weight)
{
  const char *item_name = get_item_name(original_id);
  if (item_name == NULL)
    return -ECHILD;
  const char *class_name = get_class_name(device_class);
  if (class_name == NULL)
    return -EBADF;
  string copy_name = item_name + string("~") + class_name;
  if (name_exists(copy_name)) {
    *clone = get_item_id(copy_name);
    return 0;
  }

  crush_bucket *original = get_bucket(original_id);
  assert(!IS_ERR(original));
  crush_bucket *copy = crush_make_bucket(crush,
					 original->alg,
					 original->hash,
					 original->type,
					 0, NULL, NULL);
  assert(copy);

  vector<unsigned> item_orig_pos;  // new item pos -> orig item pos
  for (unsigned i = 0; i < original->size; i++) {
    int item = original->items[i];
    int weight = crush_get_bucket_item_weight(original, i);
    if (item >= 0) {
      if (class_map.count(item) != 0 && class_map[item] == device_class) {
	int res = crush_bucket_add_item(crush, copy, item, weight);
	if (res)
	  return res;
      } else {
	continue;
      }
    } else {
      int child_copy_id;
      int res = device_class_clone(item, device_class, old_class_bucket,
				   used_ids, &child_copy_id,
				   cmap_item_weight);
      if (res < 0)
	return res;
      crush_bucket *child_copy = get_bucket(child_copy_id);
      assert(!IS_ERR(child_copy));
      res = crush_bucket_add_item(crush, copy, child_copy_id,
				  child_copy->weight);
      if (res)
	return res;
    }
    item_orig_pos.push_back(i);
  }
  assert(item_orig_pos.size() == copy->size);

  int bno = 0;
  if (old_class_bucket.count(original_id) &&
      old_class_bucket.at(original_id).count(device_class)) {
    bno = old_class_bucket.at(original_id).at(device_class);
  } else {
    // pick a new shadow bucket id that is not used by the current map
    // *or* any previous shadow buckets.
    bno = -1;
    while (((-1-bno) < crush->max_buckets && crush->buckets[-1-bno]) ||
	   used_ids.count(bno)) {
      --bno;
    }
  }
  int res = crush_add_bucket(crush, bno, copy, clone);
  if (res)
    return res;
  assert(!bno || bno == *clone);

  res = set_item_class(*clone, device_class);
  if (res < 0)
    return res;

  // we do not use set_item_name because the name is intentionally invalid
  name_map[*clone] = copy_name;
  if (have_rmaps)
    name_rmap[copy_name] = *clone;
  class_bucket[original_id][device_class] = *clone;

  // set up choose_args for the new bucket.
  for (auto& w : choose_args) {
    crush_choose_arg_map& cmap = w.second;
    if (-1-bno >= (int)cmap.size) {
      unsigned new_size = -1-bno + 1;
      cmap.args = (crush_choose_arg*)realloc(cmap.args,
					     new_size * sizeof(cmap.args[0]));
      assert(cmap.args);
      memset(cmap.args + cmap.size, 0,
	     (new_size - cmap.size) * sizeof(cmap.args[0]));
      cmap.size = new_size;
    }
    auto& o = cmap.args[-1-original_id];
    auto& n = cmap.args[-1-bno];
    n.ids_size = 0; // FIXME: implement me someday
    n.weight_set_size = o.weight_set_size;
    n.weight_set = (crush_weight_set*)calloc(
      n.weight_set_size, sizeof(crush_weight_set));
    for (size_t s = 0; s < n.weight_set_size; ++s) {
      n.weight_set[s].size = copy->size;
      n.weight_set[s].weights = (__u32*)calloc(copy->size, sizeof(__u32));
    }
    for (size_t s = 0; s < n.weight_set_size; ++s) {
      vector<int> bucket_weights(n.weight_set_size);
      for (size_t i = 0; i < copy->size; ++i) {
	int item = copy->items[i];
	if (item >= 0) {
	  n.weight_set[s].weights[i] = o.weight_set[s].weights[item_orig_pos[i]];
	} else {
	  n.weight_set[s].weights[i] = (*cmap_item_weight)[w.first][item][s];
	}
	bucket_weights[s] += n.weight_set[s].weights[i];
      }
      (*cmap_item_weight)[w.first][bno] = bucket_weights;
    }
  }
  return 0;
}

int CrushWrapper::get_rules_by_class(const string &class_name, set<int> *rules)
{
  assert(rules);
  rules->clear();
  if (!class_exists(class_name)) {
    return -ENOENT;
  }
  int class_id = get_class_id(class_name);
  for (unsigned i = 0; i < crush->max_rules; ++i) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j = 0; j < r->len; ++j) {
      if (r->steps[j].op == CRUSH_RULE_TAKE) {
        int step_item = r->steps[j].arg1;
        int original_item;
        int c;
        int res = split_id_class(step_item, &original_item, &c);
        if (res < 0) {
          return res;
        }
        if (c != -1 && c == class_id) {
          rules->insert(i);
          break;
        }
      }
    }
  }
  return 0;
}

// return rules that might reference the given osd
int CrushWrapper::get_rules_by_osd(int osd, set<int> *rules)
{
  assert(rules);
  rules->clear();
  if (osd < 0) {
    return -EINVAL;
  }
  for (unsigned i = 0; i < crush->max_rules; ++i) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j = 0; j < r->len; ++j) {
      if (r->steps[j].op == CRUSH_RULE_TAKE) {
        int step_item = r->steps[j].arg1;
        list<int> unordered;
        int rc = _get_leaves(step_item, &unordered);
        if (rc < 0) {
          return rc; // propagate fatal errors!
        }
        bool match = false;
        for (auto &o: unordered) {
          assert(o >= 0);
          if (o == osd) {
            match = true;
            break;
          }
        }
        if (match) {
          rules->insert(i);
          break;
        }
      }
    }
  }
  return 0;
}

bool CrushWrapper::_class_is_dead(int class_id)
{
  for (auto &p: class_map) {
    if (p.first >= 0 && p.second == class_id) {
      return false;
    }
  }
  for (unsigned i = 0; i < crush->max_rules; ++i) {
    crush_rule *r = crush->rules[i];
    if (!r)
      continue;
    for (unsigned j = 0; j < r->len; ++j) {
      if (r->steps[j].op == CRUSH_RULE_TAKE) {
        int root = r->steps[j].arg1;
        for (auto &p : class_bucket) {
          auto& q = p.second;
          if (q.count(class_id) && q[class_id] == root) {
            return false;
          }
        }
      }
    }
  }
  // no more referenced by any devices or crush rules
  return true;
}

void CrushWrapper::cleanup_dead_classes()
{
  auto p = class_name.begin();
  while (p != class_name.end()) {
    if (_class_is_dead(p->first)) {
      string n = p->second;
      ++p;
      remove_class_name(n);
    } else {
      ++p;
    }
  }
}

int CrushWrapper::rebuild_roots_with_classes()
{
  std::map<int32_t, map<int32_t, int32_t> > old_class_bucket = class_bucket;
  cleanup_dead_classes();
  int r = trim_roots_with_class();
  if (r < 0)
    return r;
  class_bucket.clear();
  return populate_classes(old_class_bucket);
}

void CrushWrapper::encode(bufferlist& bl, uint64_t features) const
{
  assert(crush);

  __u32 magic = CRUSH_MAGIC;
  ::encode(magic, bl);

  ::encode(crush->max_buckets, bl);
  ::encode(crush->max_rules, bl);
  ::encode(crush->max_devices, bl);

  bool encode_compat_choose_args = false;
  crush_choose_arg_map arg_map;
  memset(&arg_map, '\0', sizeof(arg_map));
  if (has_choose_args() &&
      !HAVE_FEATURE(features, CRUSH_CHOOSE_ARGS)) {
    assert(!has_incompat_choose_args());
    encode_compat_choose_args = true;
    arg_map = choose_args.begin()->second;
  }

  // buckets
  for (int i=0; i<crush->max_buckets; i++) {
    __u32 alg = 0;
    if (crush->buckets[i]) alg = crush->buckets[i]->alg;
    ::encode(alg, bl);
    if (!alg)
      continue;

    ::encode(crush->buckets[i]->id, bl);
    ::encode(crush->buckets[i]->type, bl);
    ::encode(crush->buckets[i]->alg, bl);
    ::encode(crush->buckets[i]->hash, bl);
    ::encode(crush->buckets[i]->weight, bl);
    ::encode(crush->buckets[i]->size, bl);
    for (unsigned j=0; j<crush->buckets[i]->size; j++)
      ::encode(crush->buckets[i]->items[j], bl);

    switch (crush->buckets[i]->alg) {
    case CRUSH_BUCKET_UNIFORM:
      ::encode((reinterpret_cast<crush_bucket_uniform*>(crush->buckets[i]))->item_weight, bl);
      break;

    case CRUSH_BUCKET_LIST:
      for (unsigned j=0; j<crush->buckets[i]->size; j++) {
	::encode((reinterpret_cast<crush_bucket_list*>(crush->buckets[i]))->item_weights[j], bl);
	::encode((reinterpret_cast<crush_bucket_list*>(crush->buckets[i]))->sum_weights[j], bl);
      }
      break;

    case CRUSH_BUCKET_TREE:
      ::encode((reinterpret_cast<crush_bucket_tree*>(crush->buckets[i]))->num_nodes, bl);
      for (unsigned j=0; j<(reinterpret_cast<crush_bucket_tree*>(crush->buckets[i]))->num_nodes; j++)
	::encode((reinterpret_cast<crush_bucket_tree*>(crush->buckets[i]))->node_weights[j], bl);
      break;

    case CRUSH_BUCKET_STRAW:
      for (unsigned j=0; j<crush->buckets[i]->size; j++) {
	::encode((reinterpret_cast<crush_bucket_straw*>(crush->buckets[i]))->item_weights[j], bl);
	::encode((reinterpret_cast<crush_bucket_straw*>(crush->buckets[i]))->straws[j], bl);
      }
      break;

    case CRUSH_BUCKET_STRAW2:
      {
	__u32 *weights;
	if (encode_compat_choose_args &&
	    arg_map.args[i].weight_set_size > 0) {
	  weights = arg_map.args[i].weight_set[0].weights;
	} else {
	  weights = (reinterpret_cast<crush_bucket_straw2*>(crush->buckets[i]))->item_weights;
	}
	for (unsigned j=0; j<crush->buckets[i]->size; j++) {
	  ::encode(weights[j], bl);
	}
      }
      break;

    default:
      ceph_abort();
      break;
    }
  }

  // rules
  for (unsigned i=0; i<crush->max_rules; i++) {
    __u32 yes = crush->rules[i] ? 1:0;
    ::encode(yes, bl);
    if (!yes)
      continue;

    ::encode(crush->rules[i]->len, bl);
    ::encode(crush->rules[i]->mask, bl);
    for (unsigned j=0; j<crush->rules[i]->len; j++)
      ::encode(crush->rules[i]->steps[j], bl);
  }

  // name info
  ::encode(type_map, bl);
  ::encode(name_map, bl);
  ::encode(rule_name_map, bl);

  // tunables
  ::encode(crush->choose_local_tries, bl);
  ::encode(crush->choose_local_fallback_tries, bl);
  ::encode(crush->choose_total_tries, bl);
  ::encode(crush->chooseleaf_descend_once, bl);
  ::encode(crush->chooseleaf_vary_r, bl);
  ::encode(crush->straw_calc_version, bl);
  ::encode(crush->allowed_bucket_algs, bl);
  if (features & CEPH_FEATURE_CRUSH_TUNABLES5) {
    ::encode(crush->chooseleaf_stable, bl);
  }

  if (HAVE_FEATURE(features, SERVER_LUMINOUS)) {
    // device classes
    ::encode(class_map, bl);
    ::encode(class_name, bl);
    ::encode(class_bucket, bl);

    // choose args
    __u32 size = (__u32)choose_args.size();
    ::encode(size, bl);
    for (auto c : choose_args) {
      ::encode(c.first, bl);
      crush_choose_arg_map arg_map = c.second;
      size = 0;
      for (__u32 i = 0; i < arg_map.size; i++) {
	crush_choose_arg *arg = &arg_map.args[i];
	if (arg->weight_set_size == 0 &&
	    arg->ids_size == 0)
	  continue;
	size++;
      }
      ::encode(size, bl);
      for (__u32 i = 0; i < arg_map.size; i++) {
	crush_choose_arg *arg = &arg_map.args[i];
	if (arg->weight_set_size == 0 &&
	    arg->ids_size == 0)
	  continue;
	::encode(i, bl);
	::encode(arg->weight_set_size, bl);
	for (__u32 j = 0; j < arg->weight_set_size; j++) {
	  crush_weight_set *weight_set = &arg->weight_set[j];
	  ::encode(weight_set->size, bl);
	  for (__u32 k = 0; k < weight_set->size; k++)
	    ::encode(weight_set->weights[k], bl);
	}
	::encode(arg->ids_size, bl);
	for (__u32 j = 0; j < arg->ids_size; j++)
	  ::encode(arg->ids[j], bl);
      }
    }
  }
}

static void decode_32_or_64_string_map(map<int32_t,string>& m, bufferlist::iterator& blp)
{
  m.clear();
  __u32 n;
  ::decode(n, blp);
  while (n--) {
    __s32 key;
    ::decode(key, blp);

    __u32 strlen;
    ::decode(strlen, blp);
    if (strlen == 0) {
      // der, key was actually 64-bits!
      ::decode(strlen, blp);
    }
    ::decode_nohead(strlen, m[key], blp);
  }
}

void CrushWrapper::decode(bufferlist::iterator& blp)
{
  create();

  __u32 magic;
  ::decode(magic, blp);
  if (magic != CRUSH_MAGIC)
    throw buffer::malformed_input("bad magic number");

  ::decode(crush->max_buckets, blp);
  ::decode(crush->max_rules, blp);
  ::decode(crush->max_devices, blp);

  // legacy tunables, unless we decode something newer
  set_tunables_legacy();

  try {
    // buckets
    crush->buckets = (crush_bucket**)calloc(1, crush->max_buckets * sizeof(crush_bucket*));
    for (int i=0; i<crush->max_buckets; i++) {
      decode_crush_bucket(&crush->buckets[i], blp);
    }

    // rules
    crush->rules = (crush_rule**)calloc(1, crush->max_rules * sizeof(crush_rule*));
    for (unsigned i = 0; i < crush->max_rules; ++i) {
      __u32 yes;
      ::decode(yes, blp);
      if (!yes) {
	crush->rules[i] = NULL;
	continue;
      }

      __u32 len;
      ::decode(len, blp);
      crush->rules[i] = reinterpret_cast<crush_rule*>(calloc(1, crush_rule_size(len)));
      crush->rules[i]->len = len;
      ::decode(crush->rules[i]->mask, blp);
      for (unsigned j=0; j<crush->rules[i]->len; j++)
	::decode(crush->rules[i]->steps[j], blp);
    }

    // name info
    // NOTE: we had a bug where we were incoding int instead of int32, which means the
    // 'key' field for these maps may be either 32 or 64 bits, depending.  tolerate
    // both by assuming the string is always non-empty.
    decode_32_or_64_string_map(type_map, blp);
    decode_32_or_64_string_map(name_map, blp);
    decode_32_or_64_string_map(rule_name_map, blp);

    // tunables
    if (!blp.end()) {
      ::decode(crush->choose_local_tries, blp);
      ::decode(crush->choose_local_fallback_tries, blp);
      ::decode(crush->choose_total_tries, blp);
    }
    if (!blp.end()) {
      ::decode(crush->chooseleaf_descend_once, blp);
    }
    if (!blp.end()) {
      ::decode(crush->chooseleaf_vary_r, blp);
    }
    if (!blp.end()) {
      ::decode(crush->straw_calc_version, blp);
    }
    if (!blp.end()) {
      ::decode(crush->allowed_bucket_algs, blp);
    }
    if (!blp.end()) {
      ::decode(crush->chooseleaf_stable, blp);
    }
    if (!blp.end()) {
      ::decode(class_map, blp);
      ::decode(class_name, blp);
      for (auto &c : class_name)
	class_rname[c.second] = c.first;
      ::decode(class_bucket, blp);
    }
    if (!blp.end()) {
      __u32 choose_args_size;
      ::decode(choose_args_size, blp);
      for (__u32 i = 0; i < choose_args_size; i++) {
        typename decltype(choose_args)::key_type choose_args_index;
	::decode(choose_args_index, blp);
	crush_choose_arg_map arg_map;
	arg_map.size = crush->max_buckets;
	arg_map.args = (crush_choose_arg*)calloc(
	  arg_map.size, sizeof(crush_choose_arg));
	__u32 size;
	::decode(size, blp);
	for (__u32 j = 0; j < size; j++) {
	  __u32 bucket_index;
	  ::decode(bucket_index, blp);
	  assert(bucket_index < arg_map.size);
	  crush_choose_arg *arg = &arg_map.args[bucket_index];
	  ::decode(arg->weight_set_size, blp);
	  if (arg->weight_set_size) {
	    arg->weight_set = (crush_weight_set*)calloc(
	      arg->weight_set_size, sizeof(crush_weight_set));
	    for (__u32 k = 0; k < arg->weight_set_size; k++) {
	      crush_weight_set *weight_set = &arg->weight_set[k];
	      ::decode(weight_set->size, blp);
	      weight_set->weights = (__u32*)calloc(
		weight_set->size, sizeof(__u32));
	      for (__u32 l = 0; l < weight_set->size; l++)
		::decode(weight_set->weights[l], blp);
	    }
	  }
	  ::decode(arg->ids_size, blp);
	  if (arg->ids_size) {
	    assert(arg->ids_size == crush->buckets[bucket_index]->size);
	    arg->ids = (__s32 *)calloc(arg->ids_size, sizeof(__s32));
	    for (__u32 k = 0; k < arg->ids_size; k++)
	      ::decode(arg->ids[k], blp);
	  }
	}
	choose_args[choose_args_index] = arg_map;
      }
    }
    finalize();
  }
  catch (...) {
    crush_destroy(crush);
    throw;
  }
}

void CrushWrapper::decode_crush_bucket(crush_bucket** bptr, bufferlist::iterator &blp)
{
  __u32 alg;
  ::decode(alg, blp);
  if (!alg) {
    *bptr = NULL;
    return;
  }

  int size = 0;
  switch (alg) {
  case CRUSH_BUCKET_UNIFORM:
    size = sizeof(crush_bucket_uniform);
    break;
  case CRUSH_BUCKET_LIST:
    size = sizeof(crush_bucket_list);
    break;
  case CRUSH_BUCKET_TREE:
    size = sizeof(crush_bucket_tree);
    break;
  case CRUSH_BUCKET_STRAW:
    size = sizeof(crush_bucket_straw);
    break;
  case CRUSH_BUCKET_STRAW2:
    size = sizeof(crush_bucket_straw2);
    break;
  default:
    {
      char str[128];
      snprintf(str, sizeof(str), "unsupported bucket algorithm: %d", alg);
      throw buffer::malformed_input(str);
    }
  }
  crush_bucket *bucket = reinterpret_cast<crush_bucket*>(calloc(1, size));
  *bptr = bucket;
    
  ::decode(bucket->id, blp);
  ::decode(bucket->type, blp);
  ::decode(bucket->alg, blp);
  ::decode(bucket->hash, blp);
  ::decode(bucket->weight, blp);
  ::decode(bucket->size, blp);

  bucket->items = (__s32*)calloc(1, bucket->size * sizeof(__s32));
  for (unsigned j = 0; j < bucket->size; ++j) {
    ::decode(bucket->items[j], blp);
  }

  switch (bucket->alg) {
  case CRUSH_BUCKET_UNIFORM:
    ::decode((reinterpret_cast<crush_bucket_uniform*>(bucket))->item_weight, blp);
    break;

  case CRUSH_BUCKET_LIST: {
    crush_bucket_list* cbl = reinterpret_cast<crush_bucket_list*>(bucket);
    cbl->item_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    cbl->sum_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));

    for (unsigned j = 0; j < bucket->size; ++j) {
      ::decode(cbl->item_weights[j], blp);
      ::decode(cbl->sum_weights[j], blp);
    }
    break;
  }

  case CRUSH_BUCKET_TREE: {
    crush_bucket_tree* cbt = reinterpret_cast<crush_bucket_tree*>(bucket);
    ::decode(cbt->num_nodes, blp);
    cbt->node_weights = (__u32*)calloc(1, cbt->num_nodes * sizeof(__u32));
    for (unsigned j=0; j<cbt->num_nodes; j++) {
      ::decode(cbt->node_weights[j], blp);
    }
    break;
  }

  case CRUSH_BUCKET_STRAW: {
    crush_bucket_straw* cbs = reinterpret_cast<crush_bucket_straw*>(bucket);
    cbs->straws = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    cbs->item_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    for (unsigned j = 0; j < bucket->size; ++j) {
      ::decode(cbs->item_weights[j], blp);
      ::decode(cbs->straws[j], blp);
    }
    break;
  }

  case CRUSH_BUCKET_STRAW2: {
    crush_bucket_straw2* cbs = reinterpret_cast<crush_bucket_straw2*>(bucket);
    cbs->item_weights = (__u32*)calloc(1, bucket->size * sizeof(__u32));
    for (unsigned j = 0; j < bucket->size; ++j) {
      ::decode(cbs->item_weights[j], blp);
    }
    break;
  }

  default:
    // We should have handled this case in the first switch statement
    ceph_abort();
    break;
  }
}

  
void CrushWrapper::dump(Formatter *f) const
{
  f->open_array_section("devices");
  for (int i=0; i<get_max_devices(); i++) {
    f->open_object_section("device");
    f->dump_int("id", i);
    const char *n = get_item_name(i);
    if (n) {
      f->dump_string("name", n);
    } else {
      char name[20];
      sprintf(name, "device%d", i);
      f->dump_string("name", name);
    }
    const char *device_class = get_item_class(i);
    if (device_class != NULL)
      f->dump_string("class", device_class);
    f->close_section();
  }
  f->close_section();

  f->open_array_section("types");
  int n = get_num_type_names();
  for (int i=0; n; i++) {
    const char *name = get_type_name(i);
    if (!name) {
      if (i == 0) {
	f->open_object_section("type");
	f->dump_int("type_id", 0);
	f->dump_string("name", "device");
	f->close_section();
      }
      continue;
    }
    n--;
    f->open_object_section("type");
    f->dump_int("type_id", i);
    f->dump_string("name", name);
    f->close_section();
  }
  f->close_section();

  f->open_array_section("buckets");
  for (int bucket = -1; bucket > -1-get_max_buckets(); --bucket) {
    if (!bucket_exists(bucket))
      continue;
    f->open_object_section("bucket");
    f->dump_int("id", bucket);
    if (get_item_name(bucket))
      f->dump_string("name", get_item_name(bucket));
    f->dump_int("type_id", get_bucket_type(bucket));
    if (get_type_name(get_bucket_type(bucket)))
      f->dump_string("type_name", get_type_name(get_bucket_type(bucket)));
    f->dump_int("weight", get_bucket_weight(bucket));
    f->dump_string("alg", crush_bucket_alg_name(get_bucket_alg(bucket)));
    f->dump_string("hash", crush_hash_name(get_bucket_hash(bucket)));
    f->open_array_section("items");
    for (int j=0; j<get_bucket_size(bucket); j++) {
      f->open_object_section("item");
      f->dump_int("id", get_bucket_item(bucket, j));
      f->dump_int("weight", get_bucket_item_weight(bucket, j));
      f->dump_int("pos", j);
      f->close_section();
    }
    f->close_section();
    f->close_section();
  }
  f->close_section();

  f->open_array_section("rules");
  dump_rules(f);
  f->close_section();

  f->open_object_section("tunables");
  dump_tunables(f);
  f->close_section();

  dump_choose_args(f);
}

namespace {
  // depth first walker
  class TreeDumper {
    typedef CrushTreeDumper::Item Item;
    const CrushWrapper *crush;
    const CrushTreeDumper::name_map_t& weight_set_names;
  public:
    explicit TreeDumper(const CrushWrapper *crush,
			const CrushTreeDumper::name_map_t& wsnames)
      : crush(crush), weight_set_names(wsnames) {}

    void dump(Formatter *f) {
      set<int> roots;
      crush->find_roots(&roots);
      for (set<int>::iterator root = roots.begin(); root != roots.end(); ++root) {
	dump_item(Item(*root, 0, 0, crush->get_bucket_weightf(*root)), f);
      }
    }

  private:
    void dump_item(const Item& qi, Formatter* f) {
      if (qi.is_bucket()) {
	f->open_object_section("bucket");
	CrushTreeDumper::dump_item_fields(crush, weight_set_names, qi, f);
	dump_bucket_children(qi, f);
	f->close_section();
      } else {
	f->open_object_section("device");
	CrushTreeDumper::dump_item_fields(crush, weight_set_names, qi, f);
	f->close_section();
      }
    }

    void dump_bucket_children(const Item& parent, Formatter* f) {
      f->open_array_section("items");
      const int max_pos = crush->get_bucket_size(parent.id);
      for (int pos = 0; pos < max_pos; pos++) {
	int id = crush->get_bucket_item(parent.id, pos);
	float weight = crush->get_bucket_item_weightf(parent.id, pos);
	dump_item(Item(id, parent.id, parent.depth + 1, weight), f);
      }
      f->close_section();
    }
  };
}

void CrushWrapper::dump_tree(
  Formatter *f,
  const CrushTreeDumper::name_map_t& weight_set_names) const
{
  assert(f);
  TreeDumper(this, weight_set_names).dump(f);
}

void CrushWrapper::dump_tunables(Formatter *f) const
{
  f->dump_int("choose_local_tries", get_choose_local_tries());
  f->dump_int("choose_local_fallback_tries", get_choose_local_fallback_tries());
  f->dump_int("choose_total_tries", get_choose_total_tries());
  f->dump_int("chooseleaf_descend_once", get_chooseleaf_descend_once());
  f->dump_int("chooseleaf_vary_r", get_chooseleaf_vary_r());
  f->dump_int("chooseleaf_stable", get_chooseleaf_stable());
  f->dump_int("straw_calc_version", get_straw_calc_version());
  f->dump_int("allowed_bucket_algs", get_allowed_bucket_algs());

  // be helpful about it
  if (has_jewel_tunables())
    f->dump_string("profile", "jewel");
  else if (has_hammer_tunables())
    f->dump_string("profile", "hammer");
  else if (has_firefly_tunables())
    f->dump_string("profile", "firefly");
  else if (has_bobtail_tunables())
    f->dump_string("profile", "bobtail");
  else if (has_argonaut_tunables())
    f->dump_string("profile", "argonaut");
  else
    f->dump_string("profile", "unknown");
  f->dump_int("optimal_tunables", (int)has_optimal_tunables());
  f->dump_int("legacy_tunables", (int)has_legacy_tunables());

  // be helpful about minimum version required
  f->dump_string("minimum_required_version", get_min_required_version());

  f->dump_int("require_feature_tunables", (int)has_nondefault_tunables());
  f->dump_int("require_feature_tunables2", (int)has_nondefault_tunables2());
  f->dump_int("has_v2_rules", (int)has_v2_rules());
  f->dump_int("require_feature_tunables3", (int)has_nondefault_tunables3());
  f->dump_int("has_v3_rules", (int)has_v3_rules());
  f->dump_int("has_v4_buckets", (int)has_v4_buckets());
  f->dump_int("require_feature_tunables5", (int)has_nondefault_tunables5());
  f->dump_int("has_v5_rules", (int)has_v5_rules());
}

void CrushWrapper::dump_choose_args(Formatter *f) const
{
  f->open_object_section("choose_args");
  for (auto c : choose_args) {
    crush_choose_arg_map arg_map = c.second;
    f->open_array_section(stringify(c.first).c_str());
    for (__u32 i = 0; i < arg_map.size; i++) {
      crush_choose_arg *arg = &arg_map.args[i];
      if (arg->weight_set_size == 0 &&
	  arg->ids_size == 0)
	continue;
      f->open_object_section("choose_args");
      int bucket_index = i;
      f->dump_int("bucket_id", -1-bucket_index);
      if (arg->weight_set_size > 0) {
	f->open_array_section("weight_set");
	for (__u32 j = 0; j < arg->weight_set_size; j++) {
	  f->open_array_section("weights");
	  __u32 *weights = arg->weight_set[j].weights;
	  __u32 size = arg->weight_set[j].size;
	  for (__u32 k = 0; k < size; k++) {
	    f->dump_float("weight", (float)weights[k]/(float)0x10000);
	  }
	  f->close_section();
	}
	f->close_section();
      }
      if (arg->ids_size > 0) {
	f->open_array_section("ids");
	for (__u32 j = 0; j < arg->ids_size; j++)
	  f->dump_int("id", arg->ids[j]);
	f->close_section();
      }
      f->close_section();
    }
    f->close_section();
  }
  f->close_section();
}

void CrushWrapper::dump_rules(Formatter *f) const
{
  for (int i=0; i<get_max_rules(); i++) {
    if (!rule_exists(i))
      continue;
    dump_rule(i, f);
  }
}

void CrushWrapper::dump_rule(int ruleset, Formatter *f) const
{
  f->open_object_section("rule");
  f->dump_int("rule_id", ruleset);
  if (get_rule_name(ruleset))
    f->dump_string("rule_name", get_rule_name(ruleset));
  f->dump_int("ruleset", get_rule_mask_ruleset(ruleset));
  f->dump_int("type", get_rule_mask_type(ruleset));
  f->dump_int("min_size", get_rule_mask_min_size(ruleset));
  f->dump_int("max_size", get_rule_mask_max_size(ruleset));
  f->open_array_section("steps");
  for (int j=0; j<get_rule_len(ruleset); j++) {
    f->open_object_section("step");
    switch (get_rule_op(ruleset, j)) {
    case CRUSH_RULE_NOOP:
      f->dump_string("op", "noop");
      break;
    case CRUSH_RULE_TAKE:
      f->dump_string("op", "take");
      {
        int item = get_rule_arg1(ruleset, j);
        f->dump_int("item", item);

        const char *name = get_item_name(item);
        f->dump_string("item_name", name ? name : "");
      }
      break;
    case CRUSH_RULE_EMIT:
      f->dump_string("op", "emit");
      break;
    case CRUSH_RULE_CHOOSE_FIRSTN:
      f->dump_string("op", "choose_firstn");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      f->dump_string("type", get_type_name(get_rule_arg2(ruleset, j)));
      break;
    case CRUSH_RULE_CHOOSE_INDEP:
      f->dump_string("op", "choose_indep");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      f->dump_string("type", get_type_name(get_rule_arg2(ruleset, j)));
      break;
    case CRUSH_RULE_CHOOSELEAF_FIRSTN:
      f->dump_string("op", "chooseleaf_firstn");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      f->dump_string("type", get_type_name(get_rule_arg2(ruleset, j)));
      break;
    case CRUSH_RULE_CHOOSELEAF_INDEP:
      f->dump_string("op", "chooseleaf_indep");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      f->dump_string("type", get_type_name(get_rule_arg2(ruleset, j)));
      break;
    case CRUSH_RULE_SET_CHOOSE_TRIES:
      f->dump_string("op", "set_choose_tries");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      break;
    case CRUSH_RULE_SET_CHOOSELEAF_TRIES:
      f->dump_string("op", "set_chooseleaf_tries");
      f->dump_int("num", get_rule_arg1(ruleset, j));
      break;
    default:
      f->dump_int("opcode", get_rule_op(ruleset, j));
      f->dump_int("arg1", get_rule_arg1(ruleset, j));
      f->dump_int("arg2", get_rule_arg2(ruleset, j));
    }
    f->close_section();
  }
  f->close_section();
  f->close_section();
}

void CrushWrapper::list_rules(Formatter *f) const
{
  for (int rule = 0; rule < get_max_rules(); rule++) {
    if (!rule_exists(rule))
      continue;
    f->dump_string("name", get_rule_name(rule));
  }
}

void CrushWrapper::list_rules(ostream *ss) const
{
  for (int rule = 0; rule < get_max_rules(); rule++) {
    if (!rule_exists(rule))
      continue;
    *ss << get_rule_name(rule) << "\n";
  }
}

class CrushTreePlainDumper : public CrushTreeDumper::Dumper<TextTable> {
public:
  typedef CrushTreeDumper::Dumper<TextTable> Parent;

  explicit CrushTreePlainDumper(const CrushWrapper *crush,
				const CrushTreeDumper::name_map_t& wsnames)
    : Parent(crush, wsnames) {}
  explicit CrushTreePlainDumper(const CrushWrapper *crush,
                                const CrushTreeDumper::name_map_t& wsnames,
                                bool show_shadow)
    : Parent(crush, wsnames, show_shadow) {}


  void dump(TextTable *tbl) {
    tbl->define_column("ID", TextTable::LEFT, TextTable::RIGHT);
    tbl->define_column("CLASS", TextTable::LEFT, TextTable::RIGHT);
    tbl->define_column("WEIGHT", TextTable::LEFT, TextTable::RIGHT);
    for (auto& p : crush->choose_args) {
      if (p.first == CrushWrapper::DEFAULT_CHOOSE_ARGS) {
	tbl->define_column("(compat)", TextTable::LEFT, TextTable::RIGHT);
      } else {
	string name;
	auto q = weight_set_names.find(p.first);
	name = q != weight_set_names.end() ? q->second :
	  stringify(p.first);
	tbl->define_column(name.c_str(), TextTable::LEFT, TextTable::RIGHT);
      }
    }
    tbl->define_column("TYPE NAME", TextTable::LEFT, TextTable::LEFT);
    Parent::dump(tbl);
  }

protected:
  void dump_item(const CrushTreeDumper::Item &qi, TextTable *tbl) override {
    const char *c = crush->get_item_class(qi.id);
    if (!c)
      c = "";
    *tbl << qi.id
	 << c
	 << weightf_t(qi.weight);
    for (auto& p : crush->choose_args) {
      if (qi.parent < 0) {
	const crush_choose_arg_map cmap = crush->choose_args_get(p.first);
	int bidx = -1 - qi.parent;
	const crush_bucket *b = crush->get_bucket(qi.parent);
	if (b &&
	    bidx < (int)cmap.size &&
	    cmap.args[bidx].weight_set &&
	    cmap.args[bidx].weight_set_size >= 1) {
	  int pos;
	  for (pos = 0;
	       pos < (int)cmap.args[bidx].weight_set[0].size &&
		 b->items[pos] != qi.id;
	       ++pos) ;
	  *tbl << weightf_t((float)cmap.args[bidx].weight_set[0].weights[pos] /
			    (float)0x10000);
	  continue;
	}
      }
      *tbl << "";
    }
    ostringstream ss;
    for (int k=0; k < qi.depth; k++) {
      ss << "    ";
    }
    if (qi.is_bucket()) {
      ss << crush->get_type_name(crush->get_bucket_type(qi.id)) << " "
	 << crush->get_item_name(qi.id);
    } else {
      ss << "osd." << qi.id;
    }
    *tbl << ss.str();
    *tbl << TextTable::endrow;
  }
};


class CrushTreeFormattingDumper : public CrushTreeDumper::FormattingDumper {
public:
  typedef CrushTreeDumper::FormattingDumper Parent;

  explicit CrushTreeFormattingDumper(
    const CrushWrapper *crush,
    const CrushTreeDumper::name_map_t& wsnames)
    : Parent(crush, wsnames) {}

  explicit CrushTreeFormattingDumper(
    const CrushWrapper *crush,
    const CrushTreeDumper::name_map_t& wsnames,
    bool show_shadow)
    : Parent(crush, wsnames, show_shadow) {}

  void dump(Formatter *f) {
    f->open_array_section("nodes");
    Parent::dump(f);
    f->close_section();
    f->open_array_section("stray");
    f->close_section();
  }
};


void CrushWrapper::dump_tree(
  ostream *out,
  Formatter *f,
  const CrushTreeDumper::name_map_t& weight_set_names,
  bool show_shadow) const
{
  if (out) {
    TextTable tbl;
    CrushTreePlainDumper(this, weight_set_names, show_shadow).dump(&tbl);
    *out << tbl;
  }
  if (f) {
    CrushTreeFormattingDumper(this, weight_set_names, show_shadow).dump(f);
  }
}

void CrushWrapper::generate_test_instances(list<CrushWrapper*>& o)
{
  o.push_back(new CrushWrapper);
  // fixme
}

/**
 * Determine the default CRUSH ruleset ID to be used with
 * newly created replicated pools.
 *
 * @returns a ruleset ID (>=0) or -1 if no suitable ruleset found
 */
int CrushWrapper::get_osd_pool_default_crush_replicated_ruleset(CephContext *cct)
{
  int crush_ruleset = cct->_conf->osd_pool_default_crush_rule;
  if (crush_ruleset < 0) {
    crush_ruleset = find_first_ruleset(pg_pool_t::TYPE_REPLICATED);
  } else if (!ruleset_exists(crush_ruleset)) {
    crush_ruleset = -1; // match find_first_ruleset() retval
  }
  return crush_ruleset;
}

bool CrushWrapper::is_valid_crush_name(const string& s)
{
  if (s.empty())
    return false;
  for (string::const_iterator p = s.begin(); p != s.end(); ++p) {
    if (!(*p == '-') &&
	!(*p == '_') &&
	!(*p == '.') &&
	!(*p >= '0' && *p <= '9') &&
	!(*p >= 'A' && *p <= 'Z') &&
	!(*p >= 'a' && *p <= 'z'))
      return false;
  }
  return true;
}

bool CrushWrapper::is_valid_crush_loc(CephContext *cct,
                                      const map<string,string>& loc)
{
  for (map<string,string>::const_iterator l = loc.begin(); l != loc.end(); ++l) {
    if (!is_valid_crush_name(l->first) ||
        !is_valid_crush_name(l->second)) {
      ldout(cct, 1) << "loc["
                    << l->first << "] = '"
                    << l->second << "' not a valid crush name ([A-Za-z0-9_-.]+)"
                    << dendl;
      return false;
    }
  }
  return true;
}

int CrushWrapper::_choose_type_stack(
  CephContext *cct,
  const vector<pair<int,int>>& stack,
  const set<int>& overfull,
  const vector<int>& underfull,
  const vector<int>& orig,
  vector<int>::const_iterator& i,
  set<int>& used,
  vector<int> *pw) const
{
  vector<int> w = *pw;
  vector<int> o;

  ldout(cct, 10) << __func__ << " stack " << stack
		 << " orig " << orig
		 << " at " << *i
		 << " pw " << *pw
		 << dendl;

  vector<int> cumulative_fanout(stack.size());
  int f = 1;
  for (int j = (int)stack.size() - 1; j >= 0; --j) {
    cumulative_fanout[j] = f;
    f *= stack[j].second;
  }
  ldout(cct, 10) << __func__ << " cumulative_fanout " << cumulative_fanout
		 << dendl;

  // identify underful targets for each intermediate level.
  // this serves two purposes:
  //   1. we can tell when we are selecting a bucket that does not have any underfull
  //      devices beneath it.  that means that if the current input includes an overfull
  //      device, we won't be able to find an underfull device with this parent to
  //      swap for it.
  //   2. when we decide we should reject a bucket due to the above, this list gives us
  //      a list of peers to consider that *do* have underfull devices available..  (we
  //      are careful to pick one that has the same parent.)
  vector<set<int>> underfull_buckets; // level -> set of buckets with >0 underfull item(s)
  underfull_buckets.resize(stack.size() - 1);
  for (auto osd : underfull) {
    int item = osd;
    for (int j = (int)stack.size() - 2; j >= 0; --j) {
      int type = stack[j].first;
      item = get_parent_of_type(item, type);
      ldout(cct, 10) << __func__ << " underfull " << osd << " type " << type
		     << " is " << item << dendl;
      underfull_buckets[j].insert(item);
    }
  }
  ldout(cct, 20) << __func__ << " underfull_buckets " << underfull_buckets << dendl;

  for (unsigned j = 0; j < stack.size(); ++j) {
    int type = stack[j].first;
    int fanout = stack[j].second;
    int cum_fanout = cumulative_fanout[j];
    ldout(cct, 10) << " level " << j << ": type " << type << " fanout " << fanout
		   << " cumulative " << cum_fanout
		   << " w " << w << dendl;
    vector<int> o;
    auto tmpi = i;
    for (auto from : w) {
      ldout(cct, 10) << " from " << from << dendl;
      // identify leaves under each choice.  we use this to check whether any of these
      // leaves are overfull.  (if so, we need to make sure there are underfull candidates
      // to swap for them.)
      vector<set<int>> leaves;
      leaves.resize(fanout);
      for (int pos = 0; pos < fanout; ++pos) {
	if (type > 0) {
	  // non-leaf
	  int item = get_parent_of_type(*tmpi, type);
	  o.push_back(item);
	  int n = cum_fanout;
	  while (n-- && tmpi != orig.end()) {
	    leaves[pos].insert(*tmpi++);
	  }
	  ldout(cct, 10) << __func__ << "   from " << *tmpi << " got " << item
			 << " of type " << type << " over leaves " << leaves[pos] << dendl;
	} else {
	  // leaf
	  bool replaced = false;
	  if (overfull.count(*i)) {
	    for (auto item : underfull) {
	      ldout(cct, 10) << __func__ << " pos " << pos
			     << " was " << *i << " considering " << item
			     << dendl;
	      if (used.count(item)) {
		ldout(cct, 20) << __func__ << "   in used " << used << dendl;
		continue;
	      }
	      if (!subtree_contains(from, item)) {
		ldout(cct, 20) << __func__ << "   not in subtree " << from << dendl;
		continue;
	      }
	      if (std::find(orig.begin(), orig.end(), item) != orig.end()) {
		ldout(cct, 20) << __func__ << "   in orig " << orig << dendl;
		continue;
	      }
	      o.push_back(item);
	      used.insert(item);
	      ldout(cct, 10) << __func__ << " pos " << pos << " replace "
			     << *i << " -> " << item << dendl;
	      replaced = true;
	      ++i;
	      break;
	    }
	  }
	  if (!replaced) {
	    ldout(cct, 10) << __func__ << " pos " << pos << " keep " << *i
			   << dendl;
	    o.push_back(*i);
	    ++i;
	  }
	  if (i == orig.end()) {
	    ldout(cct, 10) << __func__ << " end of orig, break 1" << dendl;
	    break;
	  }
	}
      }
      if (j + 1 < stack.size()) {
	// check if any buckets have overfull leaves but no underfull candidates
	for (int pos = 0; pos < fanout; ++pos) {
	  if (underfull_buckets[j].count(o[pos]) == 0) {
	    // are any leaves overfull?
	    bool any_overfull = false;
	    for (auto osd : leaves[pos]) {
	      if (overfull.count(osd)) {
		any_overfull = true;
	      }
	    }
	    if (any_overfull) {
	      ldout(cct, 10) << " bucket " << o[pos] << " has no underfull targets and "
			     << ">0 leaves " << leaves[pos] << " is overfull; alts "
			     << underfull_buckets[j]
			     << dendl;
	      for (auto alt : underfull_buckets[j]) {
		if (std::find(o.begin(), o.end(), alt) == o.end()) {
		  // see if alt has the same parent
		  if (j == 0 ||
		      get_parent_of_type(o[pos], stack[j-1].first) ==
		      get_parent_of_type(alt, stack[j-1].first)) {
		    if (j)
		      ldout(cct, 10) << "  replacing " << o[pos]
				     << " (which has no underfull leaves) with " << alt
				     << " (same parent "
				     << get_parent_of_type(alt, stack[j-1].first) << " type "
				     << type << ")" << dendl;
		    else
		      ldout(cct, 10) << "  replacing " << o[pos]
				     << " (which has no underfull leaves) with " << alt
				     << " (first level)" << dendl;
		    o[pos] = alt;
		    break;
		  } else {
		    ldout(cct, 30) << "  alt " << alt << " for " << o[pos]
				   << " has different parent, skipping" << dendl;
		  }
		}
	      }
	    }
	  }
	}
      }
      if (i == orig.end()) {
	ldout(cct, 10) << __func__ << " end of orig, break 2" << dendl;
	break;
      }
    }
    ldout(cct, 10) << __func__ << "  w <- " << o << " was " << w << dendl;
    w.swap(o);
  }
  *pw = w;
  return 0;
}

int CrushWrapper::try_remap_rule(
  CephContext *cct,
  int ruleno,
  int maxout,
  const set<int>& overfull,
  const vector<int>& underfull,
  const vector<int>& orig,
  vector<int> *out) const
{
  const crush_map *map = crush;
  const crush_rule *rule = get_rule(ruleno);
  assert(rule);

  ldout(cct, 10) << __func__ << " ruleno " << ruleno
		<< " numrep " << maxout << " overfull " << overfull
		<< " underfull " << underfull << " orig " << orig
		<< dendl;
  vector<int> w; // working set
  out->clear();

  auto i = orig.begin();
  set<int> used;

  vector<pair<int,int>> type_stack;  // (type, fan-out)

  for (unsigned step = 0; step < rule->len; ++step) {
    const crush_rule_step *curstep = &rule->steps[step];
    ldout(cct, 10) << __func__ << " step " << step << " w " << w << dendl;
    switch (curstep->op) {
    case CRUSH_RULE_TAKE:
      if ((curstep->arg1 >= 0 && curstep->arg1 < map->max_devices) ||
	  (-1-curstep->arg1 >= 0 && -1-curstep->arg1 < map->max_buckets &&
	   map->buckets[-1-curstep->arg1])) {
	w.clear();
	w.push_back(curstep->arg1);
	ldout(cct, 10) << __func__ << " take " << w << dendl;
      } else {
	ldout(cct, 1) << " bad take value " << curstep->arg1 << dendl;
      }
      break;

    case CRUSH_RULE_CHOOSELEAF_FIRSTN:
    case CRUSH_RULE_CHOOSELEAF_INDEP:
      {
	int numrep = curstep->arg1;
	int type = curstep->arg2;
	if (numrep <= 0)
	  numrep += maxout;
	type_stack.push_back(make_pair(type, numrep));
	type_stack.push_back(make_pair(0, 1));
	int r = _choose_type_stack(cct, type_stack, overfull, underfull, orig,
				   i, used, &w);
	if (r < 0)
	  return r;
	type_stack.clear();
      }
      break;

    case CRUSH_RULE_CHOOSE_FIRSTN:
    case CRUSH_RULE_CHOOSE_INDEP:
      {
	int numrep = curstep->arg1;
	int type = curstep->arg2;
	if (numrep <= 0)
	  numrep += maxout;
	type_stack.push_back(make_pair(type, numrep));
      }
      break;

    case CRUSH_RULE_EMIT:
      ldout(cct, 10) << " emit " << w << dendl;
      if (!type_stack.empty()) {
	int r = _choose_type_stack(cct, type_stack, overfull, underfull, orig,
				   i, used, &w);
	if (r < 0)
	  return r;
	type_stack.clear();
      }
      for (auto item : w) {
	out->push_back(item);
      }
      w.clear();
      break;

    default:
      // ignore
      break;
    }
  }

  return 0;
}


int CrushWrapper::_choose_args_adjust_item_weight_in_bucket(
  CephContext *cct,
  crush_choose_arg_map cmap,
  int bucketid,
  int id,
  const vector<int>& weight,
  ostream *ss)
{
  int changed = 0;
  int bidx = -1 - bucketid;
  crush_bucket *b = crush->buckets[bidx];
  if (bidx >= (int)cmap.size) {
    if (ss)
      *ss << "no weight-set for bucket " << b->id;
    ldout(cct, 10) << __func__ << "  no crush_choose_arg for bucket " << b->id
		   << dendl;
    return 0;
  }
  crush_choose_arg *carg = &cmap.args[bidx];
  if (carg->weight_set == NULL) {
    if (ss)
      *ss << "no weight-set for bucket " << b->id;
    ldout(cct, 10) << __func__ << "  no weight_set for bucket " << b->id
		   << dendl;
    return 0;
  }
  if (carg->weight_set_size != weight.size()) {
    if (ss)
      *ss << "weight_set_size != " << weight.size() << " for bucket " << b->id;
    ldout(cct, 10) << __func__ << "  weight_set_size != " << weight.size()
		   << " for bucket " << b->id << dendl;
    return 0;
  }
  for (unsigned i = 0; i < b->size; i++) {
    if (b->items[i] == id) {
      for (unsigned j = 0; j < weight.size(); ++j) {
	carg->weight_set[j].weights[i] = weight[j];
      }
      ldout(cct, 5) << __func__ << "  set " << id << " to " << weight
		    << " in bucket " << b->id << dendl;
      changed++;
    }
  }
  if (changed) {
    vector<int> bucket_weight(weight.size(), 0);
    for (unsigned i = 0; i < b->size; i++) {
      for (unsigned j = 0; j < weight.size(); ++j) {
	bucket_weight[j] += carg->weight_set[j].weights[i];
      }
    }
    choose_args_adjust_item_weight(cct, cmap, b->id, bucket_weight, nullptr);
  }
  return changed;
}

int CrushWrapper::choose_args_adjust_item_weight(
  CephContext *cct,
  crush_choose_arg_map cmap,
  int id,
  const vector<int>& weight,
  ostream *ss)
{
  ldout(cct, 5) << __func__ << " " << id << " weight " << weight << dendl;
  int changed = 0;
  for (int bidx = 0; bidx < crush->max_buckets; bidx++) {
    crush_bucket *b = crush->buckets[bidx];
    if (b == nullptr) {
      continue;
    }
    changed += _choose_args_adjust_item_weight_in_bucket(
      cct, cmap, b->id, id, weight, ss);
  }
  if (!changed) {
    if (ss)
      *ss << "item " << id << " not found in crush map";
    return -ENOENT;
  }
  return changed;
}
