// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_CRUSH_WRAPPER_H
#define CEPH_CRUSH_WRAPPER_H

#include <stdlib.h>
#include <map>
#include <set>
#include <string>

#include <iosfwd>

#include "include/types.h"

extern "C" {
#include "crush.h"
#include "hash.h"
#include "mapper.h"
#include "builder.h"
}

#include "include/assert.h"
#include "include/err.h"
#include "include/encoding.h"
#include "include/mempool.h"

#include "common/Mutex.h"

#define BUG_ON(x) assert(!(x))

namespace ceph {
  class Formatter;
}

namespace CrushTreeDumper {
  typedef mempool::osdmap::map<int64_t,string> name_map_t;
}

WRITE_RAW_ENCODER(crush_rule_mask)   // it's all u8's

inline static void encode(const crush_rule_step &s, bufferlist &bl)
{
  ::encode(s.op, bl);
  ::encode(s.arg1, bl);
  ::encode(s.arg2, bl);
}
inline static void decode(crush_rule_step &s, bufferlist::iterator &p)
{
  ::decode(s.op, p);
  ::decode(s.arg1, p);
  ::decode(s.arg2, p);
}

using namespace std;
class CrushWrapper {
public:
  // magic value used by OSDMap for a "default" fallback choose_args, used if
  // the choose_arg_map passed to do_rule does not exist.  if this also
  // doesn't exist, fall back to canonical weights.
  enum {
    DEFAULT_CHOOSE_ARGS = -1
  };

  std::map<int32_t, string> type_map; /* bucket/device type names */
  std::map<int32_t, string> name_map; /* bucket/device names */
  std::map<int32_t, string> rule_name_map;

  std::map<int32_t, int32_t> class_map; /* item id -> class id */
  std::map<int32_t, string> class_name; /* class id -> class name */
  std::map<string, int32_t> class_rname; /* class name -> class id */
  std::map<int32_t, map<int32_t, int32_t> > class_bucket; /* bucket[id][class] == id */
  std::map<int64_t, crush_choose_arg_map> choose_args;

private:
  struct crush_map *crush;

  bool have_uniform_rules = false;

  /* reverse maps */
  mutable bool have_rmaps;
  mutable std::map<string, int> type_rmap, name_rmap, rule_name_rmap;
  void build_rmaps() const {
    if (have_rmaps) return;
    build_rmap(type_map, type_rmap);
    build_rmap(name_map, name_rmap);
    build_rmap(rule_name_map, rule_name_rmap);
    have_rmaps = true;
  }
  void build_rmap(const map<int, string> &f, std::map<string, int> &r) const {
    r.clear();
    for (std::map<int, string>::const_iterator p = f.begin(); p != f.end(); ++p)
      r[p->second] = p->first;
  }

public:
  CrushWrapper(const CrushWrapper& other);
  const CrushWrapper& operator=(const CrushWrapper& other);

  CrushWrapper() : crush(0), have_rmaps(false) {
    create();
  }
  ~CrushWrapper() {
    if (crush)
      crush_destroy(crush);
    choose_args_clear();
  }

  crush_map *get_crush_map() { return crush; }

  /* building */
  void create() {
    if (crush)
      crush_destroy(crush);
    crush = crush_create();
    choose_args_clear();
    assert(crush);
    have_rmaps = false;

    set_tunables_default();
  }

  /**
   * true if any rule has a rule id != its position in the array
   *
   * These indicate "ruleset" IDs that were created by older versions
   * of Ceph.  They are cleaned up in renumber_rules so that eventually
   * we can remove the code for handling them.
   */
  bool has_legacy_rule_ids() const;

  /**
   * fix rules whose ruleid != ruleset
   *
   * These rules were created in older versions of Ceph.  The concept
   * of a ruleset no longer exists.
   *
   * Return a map of old ID -> new ID.  Caller must update OSDMap
   * to use new IDs.
   */
  std::map<int, int> renumber_rules();

  /// true if any buckets that aren't straw2
  bool has_non_straw2_buckets() const;

  // tunables
  void set_tunables_argonaut() {
    crush->choose_local_tries = 2;
    crush->choose_local_fallback_tries = 5;
    crush->choose_total_tries = 19;
    crush->chooseleaf_descend_once = 0;
    crush->chooseleaf_vary_r = 0;
    crush->chooseleaf_stable = 0;
    crush->allowed_bucket_algs = CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  void set_tunables_bobtail() {
    crush->choose_local_tries = 0;
    crush->choose_local_fallback_tries = 0;
    crush->choose_total_tries = 50;
    crush->chooseleaf_descend_once = 1;
    crush->chooseleaf_vary_r = 0;
    crush->chooseleaf_stable = 0;
    crush->allowed_bucket_algs = CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  void set_tunables_firefly() {
    crush->choose_local_tries = 0;
    crush->choose_local_fallback_tries = 0;
    crush->choose_total_tries = 50;
    crush->chooseleaf_descend_once = 1;
    crush->chooseleaf_vary_r = 1;
    crush->chooseleaf_stable = 0;
    crush->allowed_bucket_algs = CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  void set_tunables_hammer() {
    crush->choose_local_tries = 0;
    crush->choose_local_fallback_tries = 0;
    crush->choose_total_tries = 50;
    crush->chooseleaf_descend_once = 1;
    crush->chooseleaf_vary_r = 1;
    crush->chooseleaf_stable = 0;
    crush->allowed_bucket_algs =
      (1 << CRUSH_BUCKET_UNIFORM) |
      (1 << CRUSH_BUCKET_LIST) |
      (1 << CRUSH_BUCKET_STRAW) |
      (1 << CRUSH_BUCKET_STRAW2);
  }
  void set_tunables_jewel() {
    crush->choose_local_tries = 0;
    crush->choose_local_fallback_tries = 0;
    crush->choose_total_tries = 50;
    crush->chooseleaf_descend_once = 1;
    crush->chooseleaf_vary_r = 1;
    crush->chooseleaf_stable = 1;
    crush->allowed_bucket_algs =
      (1 << CRUSH_BUCKET_UNIFORM) |
      (1 << CRUSH_BUCKET_LIST) |
      (1 << CRUSH_BUCKET_STRAW) |
      (1 << CRUSH_BUCKET_STRAW2);
  }

  void set_tunables_legacy() {
    set_tunables_argonaut();
    crush->straw_calc_version = 0;
  }
  void set_tunables_optimal() {
    set_tunables_jewel();
    crush->straw_calc_version = 1;
  }
  void set_tunables_default() {
    set_tunables_jewel();
    crush->straw_calc_version = 1;
  }

  int get_choose_local_tries() const {
    return crush->choose_local_tries;
  }
  void set_choose_local_tries(int n) {
    crush->choose_local_tries = n;
  }

  int get_choose_local_fallback_tries() const {
    return crush->choose_local_fallback_tries;
  }
  void set_choose_local_fallback_tries(int n) {
    crush->choose_local_fallback_tries = n;
  }

  int get_choose_total_tries() const {
    return crush->choose_total_tries;
  }
  void set_choose_total_tries(int n) {
    crush->choose_total_tries = n;
  }

  int get_chooseleaf_descend_once() const {
    return crush->chooseleaf_descend_once;
  }
  void set_chooseleaf_descend_once(int n) {
    crush->chooseleaf_descend_once = !!n;
  }

  int get_chooseleaf_vary_r() const {
    return crush->chooseleaf_vary_r;
  }
  void set_chooseleaf_vary_r(int n) {
    crush->chooseleaf_vary_r = n;
  }

  int get_chooseleaf_stable() const {
    return crush->chooseleaf_stable;
  }
  void set_chooseleaf_stable(int n) {
    crush->chooseleaf_stable = n;
  }

  int get_straw_calc_version() const {
    return crush->straw_calc_version;
  }
  void set_straw_calc_version(int n) {
    crush->straw_calc_version = n;
  }

  unsigned get_allowed_bucket_algs() const {
    return crush->allowed_bucket_algs;
  }
  void set_allowed_bucket_algs(unsigned n) {
    crush->allowed_bucket_algs = n;
  }

  bool has_argonaut_tunables() const {
    return
      crush->choose_local_tries == 2 &&
      crush->choose_local_fallback_tries == 5 &&
      crush->choose_total_tries == 19 &&
      crush->chooseleaf_descend_once == 0 &&
      crush->chooseleaf_vary_r == 0 &&
      crush->chooseleaf_stable == 0 &&
      crush->allowed_bucket_algs == CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  bool has_bobtail_tunables() const {
    return
      crush->choose_local_tries == 0 &&
      crush->choose_local_fallback_tries == 0 &&
      crush->choose_total_tries == 50 &&
      crush->chooseleaf_descend_once == 1 &&
      crush->chooseleaf_vary_r == 0 &&
      crush->chooseleaf_stable == 0 &&
      crush->allowed_bucket_algs == CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  bool has_firefly_tunables() const {
    return
      crush->choose_local_tries == 0 &&
      crush->choose_local_fallback_tries == 0 &&
      crush->choose_total_tries == 50 &&
      crush->chooseleaf_descend_once == 1 &&
      crush->chooseleaf_vary_r == 1 &&
      crush->chooseleaf_stable == 0 &&
      crush->allowed_bucket_algs == CRUSH_LEGACY_ALLOWED_BUCKET_ALGS;
  }
  bool has_hammer_tunables() const {
    return
      crush->choose_local_tries == 0 &&
      crush->choose_local_fallback_tries == 0 &&
      crush->choose_total_tries == 50 &&
      crush->chooseleaf_descend_once == 1 &&
      crush->chooseleaf_vary_r == 1 &&
      crush->chooseleaf_stable == 0 &&
      crush->allowed_bucket_algs == ((1 << CRUSH_BUCKET_UNIFORM) |
				      (1 << CRUSH_BUCKET_LIST) |
				      (1 << CRUSH_BUCKET_STRAW) |
				      (1 << CRUSH_BUCKET_STRAW2));
  }
  bool has_jewel_tunables() const {
    return
      crush->choose_local_tries == 0 &&
      crush->choose_local_fallback_tries == 0 &&
      crush->choose_total_tries == 50 &&
      crush->chooseleaf_descend_once == 1 &&
      crush->chooseleaf_vary_r == 1 &&
      crush->chooseleaf_stable == 1 &&
      crush->allowed_bucket_algs == ((1 << CRUSH_BUCKET_UNIFORM) |
				      (1 << CRUSH_BUCKET_LIST) |
				      (1 << CRUSH_BUCKET_STRAW) |
				      (1 << CRUSH_BUCKET_STRAW2));
  }

  bool has_optimal_tunables() const {
    return has_jewel_tunables();
  }
  bool has_legacy_tunables() const {
    return has_argonaut_tunables();
  }

  bool has_nondefault_tunables() const {
    return
      (crush->choose_local_tries != 2 ||
       crush->choose_local_fallback_tries != 5 ||
       crush->choose_total_tries != 19);
  }
  bool has_nondefault_tunables2() const {
    return
      crush->chooseleaf_descend_once != 0;
  }
  bool has_nondefault_tunables3() const {
    return
      crush->chooseleaf_vary_r != 0;
  }
  bool has_nondefault_tunables5() const {
    return
        crush->chooseleaf_stable != 0;
  }

  bool has_v2_rules() const;
  bool has_v3_rules() const;
  bool has_v4_buckets() const;
  bool has_v5_rules() const;
  bool has_choose_args() const;          // any choose_args
  bool has_incompat_choose_args() const; // choose_args that can't be made compat

  bool is_v2_rule(unsigned ruleid) const;
  bool is_v3_rule(unsigned ruleid) const;
  bool is_v5_rule(unsigned ruleid) const;

  string get_min_required_version() const {
    if (has_v5_rules() || has_nondefault_tunables5())
      return "jewel";
    else if (has_v4_buckets())
      return "hammer";
    else if (has_nondefault_tunables3())
      return "firefly";
    else if (has_nondefault_tunables2() || has_nondefault_tunables())
      return "bobtail";
    else
      return "argonaut";
  }

  // default bucket types
  unsigned get_default_bucket_alg() const {
    // in order of preference
    if (crush->allowed_bucket_algs & (1 << CRUSH_BUCKET_STRAW2))
      return CRUSH_BUCKET_STRAW2;
    if (crush->allowed_bucket_algs & (1 << CRUSH_BUCKET_STRAW))
      return CRUSH_BUCKET_STRAW;
    if (crush->allowed_bucket_algs & (1 << CRUSH_BUCKET_TREE))
      return CRUSH_BUCKET_TREE;
    if (crush->allowed_bucket_algs & (1 << CRUSH_BUCKET_LIST))
      return CRUSH_BUCKET_LIST;
    if (crush->allowed_bucket_algs & (1 << CRUSH_BUCKET_UNIFORM))
      return CRUSH_BUCKET_UNIFORM;
    return 0;
  }

  // bucket types
  int get_num_type_names() const {
    return type_map.size();
  }
  int get_max_type_id() const {
    if (type_map.empty())
      return 0;
    return type_map.rbegin()->first;
  }
  int get_type_id(const string& name) const {
    build_rmaps();
    if (type_rmap.count(name))
      return type_rmap[name];
    return -1;
  }
  const char *get_type_name(int t) const {
    std::map<int,string>::const_iterator p = type_map.find(t);
    if (p != type_map.end())
      return p->second.c_str();
    return 0;
  }
  void set_type_name(int i, const string& name) {
    type_map[i] = name;
    if (have_rmaps)
      type_rmap[name] = i;
  }

  // item/bucket names
  bool name_exists(const string& name) const {
    build_rmaps();
    return name_rmap.count(name);
  }
  bool item_exists(int i) const {
    return name_map.count(i);
  }
  int get_item_id(const string& name) const {
    build_rmaps();
    if (name_rmap.count(name))
      return name_rmap[name];
    return 0;  /* hrm */
  }
  const char *get_item_name(int t) const {
    std::map<int,string>::const_iterator p = name_map.find(t);
    if (p != name_map.end())
      return p->second.c_str();
    return 0;
  }
  int set_item_name(int i, const string& name) {
    if (!is_valid_crush_name(name))
      return -EINVAL;
    name_map[i] = name;
    if (have_rmaps)
      name_rmap[name] = i;
    return 0;
  }
  void swap_names(int a, int b) {
    string an = name_map[a];
    string bn = name_map[b];
    name_map[a] = bn;
    name_map[b] = an;
    if (have_rmaps) {
      name_rmap[an] = b;
      name_rmap[bn] = a;
    }
  }
  int split_id_class(int i, int *idout, int *classout) const;

  bool class_exists(const string& name) const {
    return class_rname.count(name);
  }
  const char *get_class_name(int i) const {
    auto p = class_name.find(i);
    if (p != class_name.end())
      return p->second.c_str();
    return 0;
  }
  int get_class_id(const string& name) const {
    auto p = class_rname.find(name);
    if (p != class_rname.end())
      return p->second;
    else
      return -EINVAL;
  }
  int remove_class_name(const string& name) {
    auto p = class_rname.find(name);
    if (p == class_rname.end())
      return -ENOENT;
    int class_id = p->second;
    auto q = class_name.find(class_id);
    if (q == class_name.end())
      return -ENOENT;
    class_rname.erase(name);
    class_name.erase(class_id);
    return 0;
  }

  int32_t _alloc_class_id() const;

  int get_or_create_class_id(const string& name) {
    int c = get_class_id(name);
    if (c < 0) {
      int i = _alloc_class_id();
      class_name[i] = name;
      class_rname[name] = i;
      return i;
    } else {
      return c;
    }
  }

  const char *get_item_class(int t) const {
    std::map<int,int>::const_iterator p = class_map.find(t);
    if (p == class_map.end())
      return 0;
    return get_class_name(p->second);
  }
  int set_item_class(int i, const string& name) {
    if (!is_valid_crush_name(name))
      return -EINVAL;
    class_map[i] = get_or_create_class_id(name);
    return 0;
  }
  int set_item_class(int i, int c) {
    class_map[i] = c;
    return c;
  }
  void get_devices_by_class(const string &name, set<int> *devices) const {
    assert(devices);
    devices->clear();
    if (!class_exists(name)) {
      return;
    }
    auto cid = get_class_id(name);
    for (auto& p : class_map) {
      if (p.first >= 0 && p.second == cid) {
        devices->insert(p.first);
      }
    }
  }
  void class_remove_item(int i) {
    auto it = class_map.find(i);
    if (it == class_map.end()) {
      return;
    }
    class_map.erase(it);
  }
  int can_rename_item(const string& srcname,
		      const string& dstname,
		      ostream *ss) const;
  int rename_item(const string& srcname,
		  const string& dstname,
		  ostream *ss);
  int can_rename_bucket(const string& srcname,
			const string& dstname,
			ostream *ss) const;
  int rename_bucket(const string& srcname,
		    const string& dstname,
		    ostream *ss);

  // rule names
  int rename_rule(const string& srcname,
                  const string& dstname,
                  ostream *ss);
  bool rule_exists(string name) const {
    build_rmaps();
    return rule_name_rmap.count(name);
  }
  int get_rule_id(string name) const {
    build_rmaps();
    if (rule_name_rmap.count(name))
      return rule_name_rmap[name];
    return -ENOENT;
  }
  const char *get_rule_name(int t) const {
    std::map<int,string>::const_iterator p = rule_name_map.find(t);
    if (p != rule_name_map.end())
      return p->second.c_str();
    return 0;
  }
  void set_rule_name(int i, const string& name) {
    rule_name_map[i] = name;
    if (have_rmaps)
      rule_name_rmap[name] = i;
  }
  bool is_shadow_item(int id) const {
    const char *name = get_item_name(id);
    return name && !is_valid_crush_name(name);
  }


  /**
   * find tree nodes referenced by rules by a 'take' command
   *
   * Note that these may not be parentless roots.
   */
  void find_takes(set<int> *roots) const;

  /**
   * find tree roots
   *
   * These are parentless nodes in the map.
   */
  void find_roots(set<int> *roots) const;


  /**
   * find tree roots that contain shadow (device class) items only
   */
  void find_shadow_roots(set<int> *roots) const {
    set<int> all;
    find_roots(&all);
    for (auto& p: all) {
      if (is_shadow_item(p)) {
        roots->insert(p);
      }
    }
  }

  /**
   * find tree roots that are not shadow (device class) items
   *
   * These are parentless nodes in the map that are not shadow
   * items for device classes.
   */
  void find_nonshadow_roots(set<int> *roots) const {
    set<int> all;
    find_roots(&all);
    for (auto& p: all) {
      if (!is_shadow_item(p)) {
        roots->insert(p);
      }
    }
  }

  /**
   * see if an item is contained within a subtree
   *
   * @param root haystack
   * @param item needle
   * @return true if the item is located beneath the given node
   */
  bool subtree_contains(int root, int item) const;

private:
  /**
   * search for an item in any bucket
   *
   * @param i item
   * @return true if present
   */
  bool _search_item_exists(int i) const;
public:

  /**
   * see if item is located where we think it is
   *
   * This verifies that the given item is located at a particular
   * location in the hierarchy.  However, that check is imprecise; we
   * are actually verifying that the most specific location key/value
   * is correct.  For example, if loc specifies that rack=foo and
   * host=bar, it will verify that host=bar is correct; any placement
   * above that level in the hierarchy is ignored.  This matches the
   * semantics for insert_item().
   *
   * @param cct cct
   * @param item item id
   * @param loc location to check (map of type to bucket names)
   * @param weight optional pointer to weight of item at that location
   * @return true if item is at specified location
   */
  bool check_item_loc(CephContext *cct, int item, const map<string,string>& loc, int *iweight);
  bool check_item_loc(CephContext *cct, int item, const map<string,string>& loc, float *weight) {
    int iweight;
    bool ret = check_item_loc(cct, item, loc, &iweight);
    if (weight)
      *weight = (float)iweight / (float)0x10000;
    return ret;
  }


  /**
   * returns the (type, name) of the parent bucket of id
   *
   * FIXME: ambiguous for items that occur multiple times in the map
   */
  pair<string,string> get_immediate_parent(int id, int *ret = NULL);

  int get_immediate_parent_id(int id, int *parent) const;

  /**
   * return ancestor of the given type, or 0 if none
   * (parent is always a bucket and thus <0)
   */
  int get_parent_of_type(int id, int type) const;

  /**
   * get the fully qualified location of a device by successively finding
   * parents beginning at ID and ending at highest type number specified in
   * the CRUSH map which assumes that if device foo is under device bar, the
   * type_id of foo < bar where type_id is the integer specified in the CRUSH map
   *
   * returns the location in the form of (type=foo) where type is a type of bucket
   * specified in the CRUSH map and foo is a name specified in the CRUSH map
   */
  map<string, string> get_full_location(int id);

  /*
   * identical to get_full_location(int id) although it returns the type/name
   * pairs in the order they occur in the hierarchy.
   *
   * returns -ENOENT if id is not found.
   */
  int get_full_location_ordered(int id, vector<pair<string, string> >& path);

  /*
   * identical to get_full_location_ordered(int id, vector<pair<string, string> >& path),
   * although it returns a concatenated string with the type/name pairs in descending
   * hierarchical order with format key1=val1,key2=val2.
   *
   * returns the location in descending hierarchy as a string.
   */
  string get_full_location_ordered_string(int id);

  /**
   * returns (type_id, type) of all parent buckets between id and
   * default, can be used to check for anomolous CRUSH maps
   */
  map<int, string> get_parent_hierarchy(int id);

  /**
   * enumerate immediate children of given node
   *
   * @param id parent bucket or device id
   * @return number of items, or error
   */
  int get_children(int id, list<int> *children);

  /**
    * get failure-domain type of a specific crush rule
    * @param rule_id crush rule id
    * @return type of failure-domain or a negative errno on error.
    */
  int get_rule_failure_domain(int rule_id);

  /**
    * enumerate leaves(devices) of given node
    *
    * @param name parent bucket name
    * @return 0 on success or a negative errno on error.
    */
  int get_leaves(const string &name, set<int> *leaves);
  int _get_leaves(int id, list<int> *leaves); // worker

  /**
   * insert an item into the map at a specific position
   *
   * Add an item as a specific location of the hierarchy.
   * Specifically, we look for the most specific location constraint
   * for which a bucket already exists, and then create intervening
   * buckets beneath that in order to place the item.
   *
   * Note that any location specifiers *above* the most specific match
   * are ignored.  For example, if we specify that osd.12 goes in
   * host=foo, rack=bar, and row=baz, and rack=bar is the most
   * specific match, we will create host=foo beneath that point and
   * put osd.12 inside it.  However, we will not verify that rack=bar
   * is beneath row=baz or move it.
   *
   * In short, we will build out a hierarchy, and move leaves around,
   * but not adjust the hierarchy's internal structure.  Yet.
   *
   * If the item is already present in the map, we will return EEXIST.
   * If the location key/value pairs are nonsensical
   * (rack=nameofdevice), or location specifies that do not attach us
   * to any existing part of the hierarchy, we will return EINVAL.
   *
   * @param cct cct
   * @param id item id
   * @param weight item weight
   * @param name item name
   * @param loc location (map of type to bucket names)
   * @return 0 for success, negative on error
   */
  int insert_item(CephContext *cct, int id, float weight, string name, const map<string,string>& loc);

  /**
   * move a bucket in the hierarchy to the given location
   *
   * This has the same location and ancestor creation behavior as
   * insert_item(), but will relocate the specified existing bucket.
   *
   * @param cct cct
   * @param id bucket id
   * @param loc location (map of type to bucket names)
   * @return 0 for success, negative on error
   */
  int move_bucket(CephContext *cct, int id, const map<string,string>& loc);

  /**
   * swap bucket contents of two buckets without touching bucket ids
   *
   * @param cct cct
   * @param src bucket a
   * @param dst bucket b
   * @return 0 for success, negative on error
   */
  int swap_bucket(CephContext *cct, int src, int dst);

  /**
   * add a link to an existing bucket in the hierarchy to the new location
   *
   * This has the same location and ancestor creation behavior as
   * insert_item(), but will add a new link to the specified existing
   * bucket.
   *
   * @param cct cct
   * @param id bucket id
   * @param loc location (map of type to bucket names)
   * @return 0 for success, negative on error
   */
  int link_bucket(CephContext *cct, int id, const map<string,string>& loc);

  /**
   * add or update an item's position in the map
   *
   * This is analogous to insert_item, except we will move an item if
   * it is already present.
   *
   * @param cct cct
   * @param id item id
   * @param weight item weight
   * @param name item name
   * @param loc location (map of type to bucket names)
   * @return 0 for no change, 1 for successful change, negative on error
   */
  int update_item(CephContext *cct, int id, float weight, string name, const map<string,string>& loc);

  /**
   * create or move an item, but do not adjust its weight if it already exists
   *
   * @param cct cct
   * @param item item id
   * @param weight initial item weight (if we need to create it)
   * @param name item name
   * @param loc location (map of type to bucket names)
   * @return 0 for no change, 1 for successful change, negative on error
   */
  int create_or_move_item(CephContext *cct, int item, float weight, string name,
			  const map<string,string>& loc);

  /**
   * remove all instances of an item from the map
   *
   * @param cct cct
   * @param id item id to remove
   * @param unlink_only unlink but do not remove bucket (useful if multiple links or not empty)
   * @return 0 on success, negative on error
   */
  int remove_item(CephContext *cct, int id, bool unlink_only);

  /**
   * recursively remove buckets starting at item and stop removing
   * when a bucket is in use.
   *
   * @param item id to remove
   * @return 0 on success, negative on error
   */
  int remove_root(int item);

  /**
   * remove all instances of an item nested beneath a certain point from the map
   *
   * @param cct cct
   * @param id item id to remove
   * @param ancestor ancestor item id under which to search for id
   * @param unlink_only unlink but do not remove bucket (useful if bucket has multiple links or is not empty)
   * @return 0 on success, negative on error
   */
private:
  bool _maybe_remove_last_instance(CephContext *cct, int id, bool unlink_only);
  int _remove_item_under(CephContext *cct, int id, int ancestor, bool unlink_only);
  bool _bucket_is_in_use(int id);
public:
  int remove_item_under(CephContext *cct, int id, int ancestor, bool unlink_only);

  /**
   * calculate the locality/distance from a given id to a crush location map
   *
   * Specifically, we look for the lowest-valued type for which the
   * location of id matches that described in loc.
   *
   * @param cct cct
   * @param id the existing id in the map
   * @param loc a set of key=value pairs describing a location in the hierarchy
   */
  int get_common_ancestor_distance(CephContext *cct, int id,
				   const std::multimap<string,string>& loc);

  /**
   * parse a set of key/value pairs out of a string vector
   *
   * These are used to describe a location in the CRUSH hierarchy.
   *
   * @param args list of strings (each key= or key=value)
   * @param ploc pointer to a resulting location map or multimap
   */
  static int parse_loc_map(const std::vector<string>& args,
			   std::map<string,string> *ploc);
  static int parse_loc_multimap(const std::vector<string>& args,
				std::multimap<string,string> *ploc);

  /**
   * get an item's weight
   *
   * Will return the weight for the first instance it finds.
   *
   * @param id item id to check
   * @return weight of item
   */
  int get_item_weight(int id) const;
  float get_item_weightf(int id) const {
    return (float)get_item_weight(id) / (float)0x10000;
  }
  int get_item_weight_in_loc(int id, const map<string,string> &loc);
  float get_item_weightf_in_loc(int id, const map<string,string> &loc) {
    return (float)get_item_weight_in_loc(id, loc) / (float)0x10000;
  }

  int validate_weightf(float weight) {
    uint64_t iweight = weight * 0x10000;
    if (iweight > std::numeric_limits<int>::max()) {
      return -EOVERFLOW;
    }
    return 0;
  }
  int adjust_item_weight(CephContext *cct, int id, int weight);
  int adjust_item_weightf(CephContext *cct, int id, float weight) {
    int r = validate_weightf(weight);
    if (r < 0) {
      return r;
    }
    return adjust_item_weight(cct, id, (int)(weight * (float)0x10000));
  }
  int adjust_item_weight_in_loc(CephContext *cct, int id, int weight, const map<string,string>& loc);
  int adjust_item_weightf_in_loc(CephContext *cct, int id, float weight, const map<string,string>& loc) {
    int r = validate_weightf(weight);
    if (r < 0) {
      return r;
    }
    return adjust_item_weight_in_loc(cct, id, (int)(weight * (float)0x10000), loc);
  }
  void reweight(CephContext *cct);

  int adjust_subtree_weight(CephContext *cct, int id, int weight);
  int adjust_subtree_weightf(CephContext *cct, int id, float weight) {
    int r = validate_weightf(weight);
    if (r < 0) {
      return r;
    }
    return adjust_subtree_weight(cct, id, (int)(weight * (float)0x10000));
  }

  /// check if item id is present in the map hierarchy
  bool check_item_present(int id) const;


  /*** devices ***/
  int get_max_devices() const {
    if (!crush) return 0;
    return crush->max_devices;
  }


  /*** rules ***/
private:
  crush_rule *get_rule(unsigned ruleno) const {
    if (!crush) return (crush_rule *)(-ENOENT);
    if (ruleno >= crush->max_rules)
      return 0;
    return crush->rules[ruleno];
  }
  crush_rule_step *get_rule_step(unsigned ruleno, unsigned step) const {
    crush_rule *n = get_rule(ruleno);
    if (IS_ERR(n)) return (crush_rule_step *)(-EINVAL);
    if (step >= n->len) return (crush_rule_step *)(-EINVAL);
    return &n->steps[step];
  }

public:
  /* accessors */
  int get_max_rules() const {
    if (!crush) return 0;
    return crush->max_rules;
  }
  bool rule_exists(unsigned ruleno) const {
    if (!crush) return false;
    if (ruleno < crush->max_rules &&
	crush->rules[ruleno] != NULL)
      return true;
    return false;
  }
  bool rule_has_take(unsigned ruleno, int take) const {
    if (!crush) return false;
    crush_rule *rule = get_rule(ruleno);
    for (unsigned i = 0; i < rule->len; ++i) {
      if (rule->steps[i].op == CRUSH_RULE_TAKE &&
	  rule->steps[i].arg1 == take) {
	return true;
      }
    }
    return false;
  }
  int get_rule_len(unsigned ruleno) const {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return PTR_ERR(r);
    return r->len;
  }
  int get_rule_mask_ruleset(unsigned ruleno) const {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return -1;
    return r->mask.ruleset;
  }
  int get_rule_mask_type(unsigned ruleno) const {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return -1;
    return r->mask.type;
  }
  int get_rule_mask_min_size(unsigned ruleno) const {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return -1;
    return r->mask.min_size;
  }
  int get_rule_mask_max_size(unsigned ruleno) const {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return -1;
    return r->mask.max_size;
  }
  int get_rule_op(unsigned ruleno, unsigned step) const {
    crush_rule_step *s = get_rule_step(ruleno, step);
    if (IS_ERR(s)) return PTR_ERR(s);
    return s->op;
  }
  int get_rule_arg1(unsigned ruleno, unsigned step) const {
    crush_rule_step *s = get_rule_step(ruleno, step);
    if (IS_ERR(s)) return PTR_ERR(s);
    return s->arg1;
  }
  int get_rule_arg2(unsigned ruleno, unsigned step) const {
    crush_rule_step *s = get_rule_step(ruleno, step);
    if (IS_ERR(s)) return PTR_ERR(s);
    return s->arg2;
  }

private:
  float _get_take_weight_osd_map(int root, map<int,float> *pmap) const;
  void _normalize_weight_map(float sum, const map<int,float>& m,
			     map<int,float> *pmap) const;

public:
  /**
   * calculate a map of osds to weights for a given rule
   *
   * Generate a map of which OSDs get how much relative weight for a
   * given rule.
   *
   * @param ruleno [in] rule id
   * @param pmap [out] map of osd to weight
   * @return 0 for success, or negative error code
   */
  int get_rule_weight_osd_map(unsigned ruleno, map<int,float> *pmap) const;

  /**
   * calculate a map of osds to weights for a given starting root
   *
   * Generate a map of which OSDs get how much relative weight for a
   * given starting root
   *
   * @param root node
   * @param pmap [out] map of osd to weight
   * @return 0 for success, or negative error code
   */
  int get_take_weight_osd_map(int root, map<int,float> *pmap) const;

  /* modifiers */

  int add_rule(int ruleno, int len, int type, int minsize, int maxsize) {
    if (!crush) return -ENOENT;
    crush_rule *n = crush_make_rule(len, ruleno, type, minsize, maxsize);
    assert(n);
    ruleno = crush_add_rule(crush, n, ruleno);
    return ruleno;
  }
  int set_rule_mask_max_size(unsigned ruleno, int max_size) {
    crush_rule *r = get_rule(ruleno);
    if (IS_ERR(r)) return -1;
    return r->mask.max_size = max_size;
  }
  int set_rule_step(unsigned ruleno, unsigned step, int op, int arg1, int arg2) {
    if (!crush) return -ENOENT;
    crush_rule *n = get_rule(ruleno);
    if (!n) return -1;
    crush_rule_set_step(n, step, op, arg1, arg2);
    return 0;
  }
  int set_rule_step_take(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_TAKE, val, 0);
  }
  int set_rule_step_set_choose_tries(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSE_TRIES, val, 0);
  }
  int set_rule_step_set_choose_local_tries(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSE_LOCAL_TRIES, val, 0);
  }
  int set_rule_step_set_choose_local_fallback_tries(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSE_LOCAL_FALLBACK_TRIES, val, 0);
  }
  int set_rule_step_set_chooseleaf_tries(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSELEAF_TRIES, val, 0);
  }
  int set_rule_step_set_chooseleaf_vary_r(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSELEAF_VARY_R, val, 0);
  }
  int set_rule_step_set_chooseleaf_stable(unsigned ruleno, unsigned step, int val) {
    return set_rule_step(ruleno, step, CRUSH_RULE_SET_CHOOSELEAF_STABLE, val, 0);
  }
  int set_rule_step_choose_firstn(unsigned ruleno, unsigned step, int val, int type) {
    return set_rule_step(ruleno, step, CRUSH_RULE_CHOOSE_FIRSTN, val, type);
  }
  int set_rule_step_choose_indep(unsigned ruleno, unsigned step, int val, int type) {
    return set_rule_step(ruleno, step, CRUSH_RULE_CHOOSE_INDEP, val, type);
  }
  int set_rule_step_choose_leaf_firstn(unsigned ruleno, unsigned step, int val, int type) {
    return set_rule_step(ruleno, step, CRUSH_RULE_CHOOSELEAF_FIRSTN, val, type);
  }
  int set_rule_step_choose_leaf_indep(unsigned ruleno, unsigned step, int val, int type) {
    return set_rule_step(ruleno, step, CRUSH_RULE_CHOOSELEAF_INDEP, val, type);
  }
  int set_rule_step_emit(unsigned ruleno, unsigned step) {
    return set_rule_step(ruleno, step, CRUSH_RULE_EMIT, 0, 0);
  }

  int add_simple_rule(
    string name, string root_name, string failure_domain_type,
    string device_class,
    string mode, int rule_type, ostream *err = 0);

  /**
   * @param rno rule[set] id to use, -1 to pick the lowest available
   */
  int add_simple_rule_at(
    string name, string root_name,
    string failure_domain_type, string device_class, string mode,
    int rule_type, int rno, ostream *err = 0);

  int remove_rule(int ruleno);


  /** buckets **/
  const crush_bucket *get_bucket(int id) const {
    if (!crush)
      return (crush_bucket *)(-EINVAL);
    unsigned int pos = (unsigned int)(-1 - id);
    unsigned int max_buckets = crush->max_buckets;
    if (pos >= max_buckets)
      return (crush_bucket *)(-ENOENT);
    crush_bucket *ret = crush->buckets[pos];
    if (ret == NULL)
      return (crush_bucket *)(-ENOENT);
    return ret;
  }
private:
  crush_bucket *get_bucket(int id) {
    if (!crush)
      return (crush_bucket *)(-EINVAL);
    unsigned int pos = (unsigned int)(-1 - id);
    unsigned int max_buckets = crush->max_buckets;
    if (pos >= max_buckets)
      return (crush_bucket *)(-ENOENT);
    crush_bucket *ret = crush->buckets[pos];
    if (ret == NULL)
      return (crush_bucket *)(-ENOENT);
    return ret;
  }
  /**
   * detach a bucket from its parent and adjust the parent weight
   *
   * returns the weight of the detached bucket
   **/
  int detach_bucket(CephContext *cct, int item);

public:
  int get_max_buckets() const {
    if (!crush) return -EINVAL;
    return crush->max_buckets;
  }
  int get_next_bucket_id() const {
    if (!crush) return -EINVAL;
    return crush_get_next_bucket_id(crush);
  }
  bool bucket_exists(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b))
      return false;
    return true;
  }
  int get_bucket_weight(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return b->weight;
  }
  float get_bucket_weightf(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return 0;
    return b->weight / (float)0x10000;
  }
  int get_bucket_type(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return b->type;
  }
  int get_bucket_alg(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return b->alg;
  }
  int get_bucket_hash(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return b->hash;
  }
  int get_bucket_size(int id) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return b->size;
  }
  int get_bucket_item(int id, int pos) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    if ((__u32)pos >= b->size)
      return PTR_ERR(b);
    return b->items[pos];
  }
  int get_bucket_item_weight(int id, int pos) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return PTR_ERR(b);
    return crush_get_bucket_item_weight(b, pos);
  }
  float get_bucket_item_weightf(int id, int pos) const {
    const crush_bucket *b = get_bucket(id);
    if (IS_ERR(b)) return 0;
    return (float)crush_get_bucket_item_weight(b, pos) / (float)0x10000;
  }

  /* modifiers */
  int add_bucket(int bucketno, int alg, int hash, int type, int size,
		 int *items, int *weights, int *idout);
  int bucket_add_item(crush_bucket *bucket, int item, int weight);
  int bucket_remove_item(struct crush_bucket *bucket, int item);
  int bucket_adjust_item_weight(CephContext *cct, struct crush_bucket *bucket, int item, int weight);

  void finalize() {
    assert(crush);
    crush_finalize(crush);
    if (!name_map.empty() &&
	name_map.rbegin()->first >= crush->max_devices) {
      crush->max_devices = name_map.rbegin()->first + 1;
    }
    have_uniform_rules = !has_legacy_rule_ids();
  }
  int bucket_set_alg(int id, int alg);

  int update_device_class(int id, const string& class_name, const string& name, ostream *ss);
  int remove_device_class(CephContext *cct, int id, ostream *ss);
  int device_class_clone(
    int original, int device_class,
    const std::map<int32_t, map<int32_t, int32_t>>& old_class_bucket,
    const std::set<int32_t>& used_ids,
    int *clone,
    map<int,map<int,vector<int>>> *cmap_item_weight);
  int rename_class(const string& srcname, const string& dstname);
  int populate_classes(
    const std::map<int32_t, map<int32_t, int32_t>>& old_class_bucket);
  int get_rules_by_class(const string &class_name, set<int> *rules);
  int get_rules_by_osd(int osd, set<int> *rules);
  bool _class_is_dead(int class_id);
  void cleanup_dead_classes();
  int rebuild_roots_with_classes();
  /* remove unused roots generated for class devices */
  int trim_roots_with_class();

  void start_choose_profile() {
    free(crush->choose_tries);
    /*
     * the original choose_total_tries value was off by one (it
     * counted "retries" and not "tries").  add one to alloc.
     */
    crush->choose_tries = (__u32 *)calloc(sizeof(*crush->choose_tries),
					  (crush->choose_total_tries + 1));
    memset(crush->choose_tries, 0,
	   sizeof(*crush->choose_tries) * (crush->choose_total_tries + 1));
  }
  void stop_choose_profile() {
    free(crush->choose_tries);
    crush->choose_tries = 0;
  }

  int get_choose_profile(__u32 **vec) {
    if (crush->choose_tries) {
      *vec = crush->choose_tries;
      return crush->choose_total_tries;
    }
    return 0;
  }


  void set_max_devices(int m) {
    crush->max_devices = m;
  }

  int find_rule(int ruleset, int type, int size) const {
    if (!crush) return -1;
    if (have_uniform_rules &&
	ruleset < (int)crush->max_rules &&
	crush->rules[ruleset] &&
	crush->rules[ruleset]->mask.type == type &&
	crush->rules[ruleset]->mask.min_size <= size &&
	crush->rules[ruleset]->mask.max_size >= size) {
      return ruleset;
    }
    return crush_find_rule(crush, ruleset, type, size);
  }

  bool ruleset_exists(const int ruleset) const {
    for (size_t i = 0; i < crush->max_rules; ++i) {
      if (rule_exists(i) && crush->rules[i]->mask.ruleset == ruleset) {
	return true;
      }
    }

    return false;
  }

  /**
   * Return the lowest numbered ruleset of type `type`
   *
   * @returns a ruleset ID, or -1 if no matching rules found.
   */
  int find_first_ruleset(int type) const {
    int result = -1;

    for (size_t i = 0; i < crush->max_rules; ++i) {
      if (crush->rules[i]
          && crush->rules[i]->mask.type == type
          && (crush->rules[i]->mask.ruleset < result || result == -1)) {
        result = crush->rules[i]->mask.ruleset;
      }
    }

    return result;
  }

  bool have_choose_args(int64_t choose_args_index) const {
    return choose_args.count(choose_args_index);
  }

  crush_choose_arg_map choose_args_get_with_fallback(
    int64_t choose_args_index) const {
    auto i = choose_args.find(choose_args_index);
    if (i == choose_args.end()) {
      i = choose_args.find(DEFAULT_CHOOSE_ARGS);
    }
    if (i == choose_args.end()) {
      crush_choose_arg_map arg_map;
      arg_map.args = NULL;
      arg_map.size = 0;
      return arg_map;
    } else {
      return i->second;
    }
  }
  crush_choose_arg_map choose_args_get(int64_t choose_args_index) const {
    auto i = choose_args.find(choose_args_index);
    if (i == choose_args.end()) {
      crush_choose_arg_map arg_map;
      arg_map.args = NULL;
      arg_map.size = 0;
      return arg_map;
    } else {
      return i->second;
    }
  }

  void destroy_choose_args(crush_choose_arg_map arg_map) {
    for (__u32 i = 0; i < arg_map.size; i++) {
      crush_choose_arg *arg = &arg_map.args[i];
      for (__u32 j = 0; j < arg->weight_set_size; j++) {
	crush_weight_set *weight_set = &arg->weight_set[j];
	free(weight_set->weights);
      }
      if (arg->weight_set)
	free(arg->weight_set);
      if (arg->ids)
	free(arg->ids);
    }
    free(arg_map.args);
  }

  void create_choose_args(int64_t id, int positions) {
    if (choose_args.count(id))
      return;
    assert(positions);
    auto &cmap = choose_args[id];
    cmap.args = (crush_choose_arg*)calloc(sizeof(crush_choose_arg),
					  crush->max_buckets);
    cmap.size = crush->max_buckets;
    for (int bidx=0; bidx < crush->max_buckets; ++bidx) {
      crush_bucket *b = crush->buckets[bidx];
      auto &carg = cmap.args[bidx];
      carg.ids = NULL;
      carg.ids_size = 0;
      if (b && b->alg == CRUSH_BUCKET_STRAW2) {
	crush_bucket_straw2 *sb = (crush_bucket_straw2*)b;
	carg.weight_set_size = positions;
	carg.weight_set = (crush_weight_set*)calloc(sizeof(crush_weight_set),
						    carg.weight_set_size);
	// initialize with canonical weights
	for (int pos = 0; pos < positions; ++pos) {
	  carg.weight_set[pos].size = b->size;
	  carg.weight_set[pos].weights = (__u32*)calloc(4, b->size);
	  for (unsigned i = 0; i < b->size; ++i) {
	    carg.weight_set[pos].weights[i] = sb->item_weights[i];
	  }
	}
      } else {
	carg.weight_set = NULL;
	carg.weight_set_size = 0;
      }
    }
  }

  void rm_choose_args(int64_t id) {
    auto p = choose_args.find(id);
    if (p != choose_args.end()) {
      destroy_choose_args(p->second);
      choose_args.erase(p);
    }
  }

  void choose_args_clear() {
    for (auto w : choose_args)
      destroy_choose_args(w.second);
    choose_args.clear();
  }

  // adjust choose_args_map weight, preserving the hierarchical summation
  // property.  used by callers optimizing layouts by tweaking weights.
  int _choose_args_adjust_item_weight_in_bucket(
    CephContext *cct,
    crush_choose_arg_map cmap,
    int bucketid,
    int id,
    const vector<int>& weight,
    ostream *ss);
  int choose_args_adjust_item_weight(
    CephContext *cct,
    crush_choose_arg_map cmap,
    int id, const vector<int>& weight,
    ostream *ss);
  int choose_args_adjust_item_weightf(
    CephContext *cct,
    crush_choose_arg_map cmap,
    int id, const vector<double>& weightf,
    ostream *ss) {
    vector<int> weight(weightf.size());
    for (unsigned i = 0; i < weightf.size(); ++i) {
      weight[i] = (int)(weightf[i] * (float)0x10000);
    }
    return choose_args_adjust_item_weight(cct, cmap, id, weight, ss);
  }

  int get_choose_args_positions(crush_choose_arg_map cmap) {
    // infer positions from other buckets
    for (unsigned j = 0; j < cmap.size; ++j) {
      if (cmap.args[j].weight_set_size) {
	return cmap.args[j].weight_set_size;
      }
    }
    return 1;
  }

  template<typename WeightVector>
  void do_rule(int rule, int x, vector<int>& out, int maxout,
	       const WeightVector& weight,
	       uint64_t choose_args_index) const {
    int rawout[maxout];
    char work[crush_work_size(crush, maxout)];
    crush_init_workspace(crush, work);
    crush_choose_arg_map arg_map = choose_args_get_with_fallback(
      choose_args_index);
    int numrep = crush_do_rule(crush, rule, x, rawout, maxout, &weight[0],
			       weight.size(), work, arg_map.args);
    if (numrep < 0)
      numrep = 0;
    out.resize(numrep);
    for (int i=0; i<numrep; i++)
      out[i] = rawout[i];
  }

  int _choose_type_stack(
    CephContext *cct,
    const vector<pair<int,int>>& stack,
    const set<int>& overfull,
    const vector<int>& underfull,
    const vector<int>& orig,
    vector<int>::const_iterator& i,
    set<int>& used,
    vector<int> *pw) const;

  int try_remap_rule(
    CephContext *cct,
    int rule,
    int maxout,
    const set<int>& overfull,
    const vector<int>& underfull,
    const vector<int>& orig,
    vector<int> *out) const;

  bool check_crush_rule(int ruleset, int type, int size,  ostream& ss) {
    assert(crush);

    __u32 i;
    for (i = 0; i < crush->max_rules; i++) {
      if (crush->rules[i] &&
	  crush->rules[i]->mask.ruleset == ruleset &&
	  crush->rules[i]->mask.type == type) {

        if (crush->rules[i]->mask.min_size <= size &&
            crush->rules[i]->mask.max_size >= size) {
          return true;
        } else if (size < crush->rules[i]->mask.min_size) {
          ss << "pool size is smaller than the crush rule min size";
          return false;
        } else {
          ss << "pool size is bigger than the crush rule max size";
          return false;
        }
      }
    }

    return false;
  }

  void encode(bufferlist &bl, uint64_t features) const;
  void decode(bufferlist::iterator &blp);
  void decode_crush_bucket(crush_bucket** bptr, bufferlist::iterator &blp);
  void dump(Formatter *f) const;
  void dump_rules(Formatter *f) const;
  void dump_rule(int ruleset, Formatter *f) const;
  void dump_tunables(Formatter *f) const;
  void dump_choose_args(Formatter *f) const;
  void list_rules(Formatter *f) const;
  void list_rules(ostream *ss) const;
  void dump_tree(ostream *out,
                 Formatter *f,
		 const CrushTreeDumper::name_map_t& ws,
                 bool show_shadow = false) const;
  void dump_tree(ostream *out, Formatter *f) {
    dump_tree(out, f, CrushTreeDumper::name_map_t());
  }
  void dump_tree(Formatter *f,
		 const CrushTreeDumper::name_map_t& ws) const;
  static void generate_test_instances(list<CrushWrapper*>& o);

  int get_osd_pool_default_crush_replicated_ruleset(CephContext *cct);

  static bool is_valid_crush_name(const string& s);
  static bool is_valid_crush_loc(CephContext *cct,
				 const map<string,string>& loc);
};
WRITE_CLASS_ENCODER_FEATURES(CrushWrapper)

#endif
