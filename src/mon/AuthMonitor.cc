// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sstream>

#include "mon/AuthMonitor.h"
#include "mon/Monitor.h"
#include "mon/MonitorDBStore.h"
#include "mon/ConfigKeyService.h"
#include "mon/OSDMonitor.h"
#include "mon/MDSMonitor.h"

#include "messages/MMonCommand.h"
#include "messages/MAuth.h"
#include "messages/MAuthReply.h"
#include "messages/MMonGlobalID.h"
#include "msg/Messenger.h"

#include "auth/AuthServiceHandler.h"
#include "auth/KeyRing.h"
#include "include/stringify.h"
#include "include/assert.h"

#define dout_subsys ceph_subsys_mon
#undef dout_prefix
#define dout_prefix _prefix(_dout, mon, get_last_committed())
static ostream& _prefix(std::ostream *_dout, Monitor *mon, version_t v) {
  return *_dout << "mon." << mon->name << "@" << mon->rank
		<< "(" << mon->get_state_name()
		<< ").auth v" << v << " ";
}

ostream& operator<<(ostream &out, const AuthMonitor &pm)
{
  return out << "auth";
}

bool AuthMonitor::check_rotate()
{
  KeyServerData::Incremental rot_inc;
  rot_inc.op = KeyServerData::AUTH_INC_SET_ROTATING;
  if (!mon->key_server.updated_rotating(rot_inc.rotating_bl, last_rotating_ver))
    return false;
  dout(10) << __func__ << " updated rotating" << dendl;
  push_cephx_inc(rot_inc);
  return true;
}

/*
 Tick function to update the map based on performance every N seconds
*/

void AuthMonitor::tick()
{
  if (!is_active()) return;

  dout(10) << *this << dendl;

  if (!mon->is_leader()) return;

  if (check_rotate())
    propose_pending();
}

void AuthMonitor::on_active()
{
  dout(10) << "AuthMonitor::on_active()" << dendl;

  if (!mon->is_leader())
    return;
  mon->key_server.start_server();
}

void AuthMonitor::create_initial()
{
  dout(10) << "create_initial -- creating initial map" << dendl;

  // initialize rotating keys
  last_rotating_ver = 0;
  check_rotate();
  assert(pending_auth.size() == 1);

  if (mon->is_keyring_required()) {
    KeyRing keyring;
    bufferlist bl;
    int ret = mon->store->get("mkfs", "keyring", bl);
    // fail hard only if there's an error we're not expecting to see
    assert((ret == 0) || (ret == -ENOENT));
    
    // try importing only if there's a key
    if (ret == 0) {
      KeyRing keyring;
      bufferlist::iterator p = bl.begin();

      ::decode(keyring, p);
      import_keyring(keyring);
    }
  }

  max_global_id = MIN_GLOBAL_ID;

  Incremental inc;
  inc.inc_type = GLOBAL_ID;
  inc.max_global_id = max_global_id;
  pending_auth.push_back(inc);

  format_version = 2;
}

void AuthMonitor::update_from_paxos(bool *need_bootstrap)
{
  dout(10) << __func__ << dendl;
  version_t version = get_last_committed();
  version_t keys_ver = mon->key_server.get_ver();
  if (version == keys_ver)
    return;
  assert(version > keys_ver);

  version_t latest_full = get_version_latest_full();

  dout(10) << __func__ << " version " << version << " keys ver " << keys_ver
           << " latest " << latest_full << dendl;

  if ((latest_full > 0) && (latest_full > keys_ver)) {
    bufferlist latest_bl;
    int err = get_version_full(latest_full, latest_bl);
    assert(err == 0);
    assert(latest_bl.length() != 0);
    dout(7) << __func__ << " loading summary e " << latest_full << dendl;
    dout(7) << __func__ << " latest length " << latest_bl.length() << dendl;
    bufferlist::iterator p = latest_bl.begin();
    __u8 struct_v;
    ::decode(struct_v, p);
    ::decode(max_global_id, p);
    ::decode(mon->key_server, p);
    mon->key_server.set_ver(latest_full);
    keys_ver = latest_full;
  }

  dout(10) << __func__ << " key server version " << mon->key_server.get_ver() << dendl;

  // walk through incrementals
  while (version > keys_ver) {
    bufferlist bl;
    int ret = get_version(keys_ver+1, bl);
    assert(ret == 0);
    assert(bl.length());

    // reset if we are moving to initial state.  we will normally have
    // keys in here temporarily for bootstrapping that we need to
    // clear out.
    if (keys_ver == 0)
      mon->key_server.clear_secrets();

    dout(20) << __func__ << " walking through version " << (keys_ver+1)
             << " len " << bl.length() << dendl;

    bufferlist::iterator p = bl.begin();
    __u8 v;
    ::decode(v, p);
    while (!p.end()) {
      Incremental inc;
      ::decode(inc, p);
      switch (inc.inc_type) {
      case GLOBAL_ID:
	max_global_id = inc.max_global_id;
	break;

      case AUTH_DATA:
        {
          KeyServerData::Incremental auth_inc;
          bufferlist::iterator iter = inc.auth_data.begin();
          ::decode(auth_inc, iter);
          mon->key_server.apply_data_incremental(auth_inc);
          break;
        }
      }
    }

    keys_ver++;
    mon->key_server.set_ver(keys_ver);

    if (keys_ver == 1 && mon->is_keyring_required()) {
      auto t(std::make_shared<MonitorDBStore::Transaction>());
      t->erase("mkfs", "keyring");
      mon->store->apply_transaction(t);
    }
  }

  if (last_allocated_id == 0)
    last_allocated_id = max_global_id;

  dout(10) << "update_from_paxos() last_allocated_id=" << last_allocated_id
	   << " max_global_id=" << max_global_id
	   << " format_version " << format_version
	   << dendl;
}

void AuthMonitor::increase_max_global_id()
{
  assert(mon->is_leader());

  max_global_id += g_conf->mon_globalid_prealloc;
  dout(10) << "increasing max_global_id to " << max_global_id << dendl;
  Incremental inc;
  inc.inc_type = GLOBAL_ID;
  inc.max_global_id = max_global_id;
  pending_auth.push_back(inc);
}

bool AuthMonitor::should_propose(double& delay)
{
  return (!pending_auth.empty());
}

void AuthMonitor::create_pending()
{
  pending_auth.clear();
  dout(10) << "create_pending v " << (get_last_committed() + 1) << dendl;
}

void AuthMonitor::encode_pending(MonitorDBStore::TransactionRef t)
{
  dout(10) << __func__ << " v " << (get_last_committed() + 1) << dendl;

  bufferlist bl;

  __u8 v = 1;
  ::encode(v, bl);
  vector<Incremental>::iterator p;
  for (p = pending_auth.begin(); p != pending_auth.end(); ++p)
    p->encode(bl, mon->get_quorum_con_features());

  version_t version = get_last_committed() + 1;
  put_version(t, version, bl);
  put_last_committed(t, version);
}

void AuthMonitor::encode_full(MonitorDBStore::TransactionRef t)
{
  version_t version = mon->key_server.get_ver();
  // do not stash full version 0 as it will never be removed nor read
  if (version == 0)
    return;

  dout(10) << __func__ << " auth v " << version << dendl;
  assert(get_last_committed() == version);

  bufferlist full_bl;
  Mutex::Locker l(mon->key_server.get_lock());
  dout(20) << __func__ << " key server has "
           << (mon->key_server.has_secrets() ? "" : "no ")
           << "secrets!" << dendl;
  __u8 v = 1;
  ::encode(v, full_bl);
  ::encode(max_global_id, full_bl);
  ::encode(mon->key_server, full_bl);

  put_version_full(t, version, full_bl);
  put_version_latest_full(t, version);
}

version_t AuthMonitor::get_trim_to()
{
  unsigned max = g_conf->paxos_max_join_drift * 2;
  version_t version = get_last_committed();
  if (mon->is_leader() && (version > max))
    return version - max;
  return 0;
}

bool AuthMonitor::preprocess_query(MonOpRequestRef op)
{
  PaxosServiceMessage *m = static_cast<PaxosServiceMessage*>(op->get_req());
  dout(10) << "preprocess_query " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return preprocess_command(op);

  case CEPH_MSG_AUTH:
    return prep_auth(op, false);

  case MSG_MON_GLOBAL_ID:
    return false;

  default:
    ceph_abort();
    return true;
  }
}

bool AuthMonitor::prepare_update(MonOpRequestRef op)
{
  PaxosServiceMessage *m = static_cast<PaxosServiceMessage*>(op->get_req());
  dout(10) << "prepare_update " << *m << " from " << m->get_orig_source_inst() << dendl;
  switch (m->get_type()) {
  case MSG_MON_COMMAND:
    return prepare_command(op);
  case MSG_MON_GLOBAL_ID:
    return prepare_global_id(op);
  case CEPH_MSG_AUTH:
    return prep_auth(op, true);
  default:
    ceph_abort();
    return false;
  }
}

uint64_t AuthMonitor::assign_global_id(MonOpRequestRef op, bool should_increase_max)
{
  MAuth *m = static_cast<MAuth*>(op->get_req());
  int total_mon = mon->monmap->size();
  dout(10) << "AuthMonitor::assign_global_id m=" << *m << " mon=" << mon->rank << "/" << total_mon
	   << " last_allocated=" << last_allocated_id << " max_global_id=" <<  max_global_id << dendl;

  uint64_t next_global_id = last_allocated_id + 1;
  int remainder = next_global_id % total_mon;
  if (remainder)
    remainder = total_mon - remainder;
  next_global_id += remainder + mon->rank;
  dout(10) << "next_global_id should be " << next_global_id << dendl;

  // if we can't bump the max, bail out now on an out-of-bounds gid
  if (next_global_id > max_global_id &&
      (!mon->is_leader() || !should_increase_max)) {
    return 0;
  }

  // can we return a gid?
  bool return_next = (next_global_id <= max_global_id);

  // bump the max?
  while (mon->is_leader() &&
	 (max_global_id < g_conf->mon_globalid_prealloc ||
	  next_global_id >= max_global_id - g_conf->mon_globalid_prealloc / 2)) {
    increase_max_global_id();
  }

  if (return_next) {
    last_allocated_id = next_global_id;
    return next_global_id;
  } else {
    return 0;
  }
}


bool AuthMonitor::prep_auth(MonOpRequestRef op, bool paxos_writable)
{
  MAuth *m = static_cast<MAuth*>(op->get_req());
  dout(10) << "prep_auth() blob_size=" << m->get_auth_payload().length() << dendl;

  MonSession *s = op->get_session();
  if (!s) {
    dout(10) << "no session, dropping" << dendl;
    return true;
  }

  int ret = 0;
  AuthCapsInfo caps_info;
  MAuthReply *reply;
  bufferlist response_bl;
  bufferlist::iterator indata = m->auth_payload.begin();
  __u32 proto = m->protocol;
  bool start = false;
  EntityName entity_name;

  // set up handler?
  if (m->protocol == 0 && !s->auth_handler) {
    set<__u32> supported;

    try {
      __u8 struct_v = 1;
      ::decode(struct_v, indata);
      ::decode(supported, indata);
      ::decode(entity_name, indata);
      ::decode(s->global_id, indata);
    } catch (const buffer::error &e) {
      dout(10) << "failed to decode initial auth message" << dendl;
      ret = -EINVAL;
      goto reply;
    }

    // do we require cephx signatures?

    if (!m->get_connection()->has_feature(CEPH_FEATURE_MSG_AUTH)) {
      if (entity_name.get_type() == CEPH_ENTITY_TYPE_MON ||
	  entity_name.get_type() == CEPH_ENTITY_TYPE_OSD ||
	  entity_name.get_type() == CEPH_ENTITY_TYPE_MDS ||
	  entity_name.get_type() == CEPH_ENTITY_TYPE_MGR) {
	if (g_conf->cephx_cluster_require_signatures ||
	    g_conf->cephx_require_signatures) {
	  dout(1) << m->get_source_inst()
                  << " supports cephx but not signatures and"
                  << " 'cephx [cluster] require signatures = true';"
                  << " disallowing cephx" << dendl;
	  supported.erase(CEPH_AUTH_CEPHX);
	}
      } else {
	if (g_conf->cephx_service_require_signatures ||
	    g_conf->cephx_require_signatures) {
	  dout(1) << m->get_source_inst()
                  << " supports cephx but not signatures and"
                  << " 'cephx [service] require signatures = true';"
                  << " disallowing cephx" << dendl;
	  supported.erase(CEPH_AUTH_CEPHX);
	}
      }
    }

    int type;
    if (entity_name.get_type() == CEPH_ENTITY_TYPE_MON ||
	entity_name.get_type() == CEPH_ENTITY_TYPE_OSD ||
	entity_name.get_type() == CEPH_ENTITY_TYPE_MDS ||
	entity_name.get_type() == CEPH_ENTITY_TYPE_MGR)
      type = mon->auth_cluster_required.pick(supported);
    else
      type = mon->auth_service_required.pick(supported);

    s->auth_handler = get_auth_service_handler(type, g_ceph_context, &mon->key_server);
    if (!s->auth_handler) {
      dout(1) << "client did not provide supported auth type" << dendl;
      ret = -ENOTSUP;
      goto reply;
    }
    start = true;
  } else if (!s->auth_handler) {
      dout(10) << "protocol specified but no s->auth_handler" << dendl;
      ret = -EINVAL;
      goto reply;
  }

  /* assign a new global_id? we assume this should only happen on the first
     request. If a client tries to send it later, it'll screw up its auth
     session */
  if (!s->global_id) {
    s->global_id = assign_global_id(op, paxos_writable);
    if (!s->global_id) {

      delete s->auth_handler;
      s->auth_handler = NULL;

      if (mon->is_leader() && paxos_writable) {
        dout(10) << "increasing global id, waitlisting message" << dendl;
        wait_for_active(op, new C_RetryMessage(this, op));
        goto done;
      }

      if (!mon->is_leader()) {
	dout(10) << "not the leader, requesting more ids from leader" << dendl;
	int leader = mon->get_leader();
	MMonGlobalID *req = new MMonGlobalID();
	req->old_max_id = max_global_id;
	mon->messenger->send_message(req, mon->monmap->get_inst(leader));
	wait_for_finished_proposal(op, new C_RetryMessage(this, op));
	return true;
      }

      assert(!paxos_writable);
      return false;
    }
  }

  try {
    uint64_t auid = 0;
    if (start) {
      // new session

      // always send the latest monmap.
      if (m->monmap_epoch < mon->monmap->get_epoch())
	mon->send_latest_monmap(m->get_connection().get());

      proto = s->auth_handler->start_session(entity_name, indata, response_bl, caps_info);
      ret = 0;
      if (caps_info.allow_all)
	s->caps.set_allow_all();
    } else {
      // request
      ret = s->auth_handler->handle_request(indata, response_bl, s->global_id, caps_info, &auid);
    }
    if (ret == -EIO) {
      wait_for_active(op, new C_RetryMessage(this,op));
      goto done;
    }
    if (caps_info.caps.length()) {
      bufferlist::iterator p = caps_info.caps.begin();
      string str;
      try {
	::decode(str, p);
      } catch (const buffer::error &err) {
	derr << "corrupt cap data for " << entity_name << " in auth db" << dendl;
	str.clear();
      }
      s->caps.parse(str, NULL);
      s->auid = auid;
    }
  } catch (const buffer::error &err) {
    ret = -EINVAL;
    dout(0) << "caught error when trying to handle auth request, probably malformed request" << dendl;
  }

reply:
  reply = new MAuthReply(proto, &response_bl, ret, s->global_id);
  mon->send_reply(op, reply);
done:
  return true;
}

bool AuthMonitor::preprocess_command(MonOpRequestRef op)
{
  MMonCommand *m = static_cast<MMonCommand*>(op->get_req());
  int r = -1;
  bufferlist rdata;
  stringstream ss, ds;

  map<string, cmd_vartype> cmdmap;
  if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    // ss has reason for failure
    string rs = ss.str();
    mon->reply_command(op, -EINVAL, rs, rdata, get_last_committed());
    return true;
  }

  string prefix;
  cmd_getval(g_ceph_context, cmdmap, "prefix", prefix);
  if (prefix == "auth add" ||
      prefix == "auth del" ||
      prefix == "auth rm" ||
      prefix == "auth get-or-create" ||
      prefix == "auth get-or-create-key" ||
      prefix == "fs authorize" ||
      prefix == "auth import" ||
      prefix == "auth caps") {
    return false;
  }

  MonSession *session = m->get_session();
  if (!session) {
    mon->reply_command(op, -EACCES, "access denied", rdata, get_last_committed());
    return true;
  }

  // entity might not be supplied, but if it is, it should be valid
  string entity_name;
  cmd_getval(g_ceph_context, cmdmap, "entity", entity_name);
  EntityName entity;
  if (!entity_name.empty() && !entity.from_str(entity_name)) {
    ss << "invalid entity_auth " << entity_name;
    mon->reply_command(op, -EINVAL, ss.str(), get_last_committed());
    return true;
  }

  string format;
  cmd_getval(g_ceph_context, cmdmap, "format", format, string("plain"));
  boost::scoped_ptr<Formatter> f(Formatter::create(format));

  if (prefix == "auth export") {
    KeyRing keyring;
    export_keyring(keyring);
    if (!entity_name.empty()) {
      EntityAuth eauth;
      if (keyring.get_auth(entity, eauth)) {
	KeyRing kr;
	kr.add(entity, eauth);
	if (f)
	  kr.encode_formatted("auth", f.get(), rdata);
	else
	  kr.encode_plaintext(rdata);
	ss << "export " << eauth;
	r = 0;
      } else {
	ss << "no key for " << eauth;
	r = -ENOENT;
      }
    } else {
      if (f)
	keyring.encode_formatted("auth", f.get(), rdata);
      else
	keyring.encode_plaintext(rdata);

      ss << "exported master keyring";
      r = 0;
    }
  } else if (prefix == "auth get" && !entity_name.empty()) {
    KeyRing keyring;
    EntityAuth entity_auth;
    if(!mon->key_server.get_auth(entity, entity_auth)) {
      ss << "failed to find " << entity_name << " in keyring";
      r = -ENOENT;
    } else {
      keyring.add(entity, entity_auth);
      if (f)
	keyring.encode_formatted("auth", f.get(), rdata);
      else
	keyring.encode_plaintext(rdata);
      ss << "exported keyring for " << entity_name;
      r = 0;
    }
  } else if (prefix == "auth print-key" ||
	     prefix == "auth print_key" ||
	     prefix == "auth get-key") {
    EntityAuth auth;
    if (!mon->key_server.get_auth(entity, auth)) {
      ss << "don't have " << entity;
      r = -ENOENT;
      goto done;
    }
    if (f) {
      auth.key.encode_formatted("auth", f.get(), rdata);
    } else {
      auth.key.encode_plaintext(rdata);
    }
    r = 0;
  } else if (prefix == "auth list" ||
	     prefix == "auth ls") {
    if (f) {
      mon->key_server.encode_formatted("auth", f.get(), rdata);
    } else {
      mon->key_server.encode_plaintext(rdata);
      if (rdata.length() > 0)
        ss << "installed auth entries:" << std::endl;
      else
        ss << "no installed auth entries!" << std::endl;
    }
    r = 0;
    goto done;
  } else {
    ss << "invalid command";
    r = -EINVAL;
  }

 done:
  rdata.append(ds);
  string rs;
  getline(ss, rs, '\0');
  mon->reply_command(op, r, rs, rdata, get_last_committed());
  return true;
}

void AuthMonitor::export_keyring(KeyRing& keyring)
{
  mon->key_server.export_keyring(keyring);
}

int AuthMonitor::import_keyring(KeyRing& keyring)
{
  for (map<EntityName, EntityAuth>::iterator p = keyring.get_keys().begin();
       p != keyring.get_keys().end();
       ++p) {
    if (p->second.caps.empty()) {
      dout(0) << "import: no caps supplied" << dendl;
      return -EINVAL;
    }
    int err = add_entity(p->first, p->second);
    assert(err == 0);
  }
  return 0;
}

int AuthMonitor::remove_entity(const EntityName &entity)
{
  dout(10) << __func__ << " " << entity << dendl;
  if (!mon->key_server.contains(entity))
    return -ENOENT;

  KeyServerData::Incremental auth_inc;
  auth_inc.name = entity;
  auth_inc.op = KeyServerData::AUTH_INC_DEL;
  push_cephx_inc(auth_inc);

  return 0;
}

bool AuthMonitor::entity_is_pending(EntityName& entity)
{
  // are we about to have it?
  for (auto& p : pending_auth) {
    if (p.inc_type == AUTH_DATA) {
      KeyServerData::Incremental inc;
      bufferlist::iterator q = p.auth_data.begin();
      ::decode(inc, q);
      if (inc.op == KeyServerData::AUTH_INC_ADD &&
          inc.name == entity) {
        return true;
      }
    }
  }
  return false;
}

int AuthMonitor::exists_and_matches_entity(
    const auth_entity_t& entity,
    bool has_secret,
    stringstream& ss)
{
  return exists_and_matches_entity(entity.name, entity.auth,
                                   entity.auth.caps, has_secret, ss);
}

int AuthMonitor::exists_and_matches_entity(
    const EntityName& name,
    const EntityAuth& auth,
    const map<string,bufferlist>& caps,
    bool has_secret,
    stringstream& ss)
{

  dout(20) << __func__ << " entity " << name << " auth " << auth
           << " caps " << caps << " has_secret " << has_secret << dendl;

  EntityAuth existing_auth;
  // does entry already exist?
  if (mon->key_server.get_auth(name, existing_auth)) {
    // key match?
    if (has_secret) {
      if (existing_auth.key.get_secret().cmp(auth.key.get_secret())) {
        ss << "entity " << name << " exists but key does not match";
        return -EEXIST;
      }
    }

    // caps match?
    if (caps.size() != existing_auth.caps.size()) {
      ss << "entity " << name << " exists but caps do not match";
      return -EINVAL;
    }
    for (auto& it : caps) {
      if (existing_auth.caps.count(it.first) == 0 ||
          !existing_auth.caps[it.first].contents_equal(it.second)) {
        ss << "entity " << name << " exists but cap "
          << it.first << " does not match";
        return -EINVAL;
      }
    }

    // they match, no-op
    return 0;
  }
  return -ENOENT;
}

int AuthMonitor::add_entity(
    const EntityName& name,
    const EntityAuth& auth)
{

  // okay, add it.
  KeyServerData::Incremental auth_inc;
  auth_inc.op = KeyServerData::AUTH_INC_ADD;
  auth_inc.name = name;
  auth_inc.auth = auth;

  dout(10) << " importing " << auth_inc.name << dendl;
  dout(30) << "    " << auth_inc.auth << dendl;
  push_cephx_inc(auth_inc);
  return 0;
}

int AuthMonitor::validate_osd_destroy(
    int32_t id,
    const uuid_d& uuid,
    EntityName& cephx_entity,
    EntityName& lockbox_entity,
    stringstream& ss)
{
  assert(paxos->is_plugged());

  dout(10) << __func__ << " id " << id << " uuid " << uuid << dendl;

  string cephx_str = "osd." + stringify(id);
  string lockbox_str = "client.osd-lockbox." + stringify(uuid);

  if (!cephx_entity.from_str(cephx_str)) {
    dout(10) << __func__ << " invalid cephx entity '"
             << cephx_str << "'" << dendl;
    ss << "invalid cephx key entity '" << cephx_str << "'";
    return -EINVAL;
  }

  if (!lockbox_entity.from_str(lockbox_str)) {
    dout(10) << __func__ << " invalid lockbox entity '"
             << lockbox_str << "'" << dendl;
    ss << "invalid lockbox key entity '" << lockbox_str << "'";
    return -EINVAL;
  }

  if (!mon->key_server.contains(cephx_entity) &&
      !mon->key_server.contains(lockbox_entity)) {
    return -ENOENT;
  }

  return 0;
}

int AuthMonitor::do_osd_destroy(
    const EntityName& cephx_entity,
    const EntityName& lockbox_entity)
{
  assert(paxos->is_plugged());

  dout(10) << __func__ << " cephx " << cephx_entity
                       << " lockbox " << lockbox_entity << dendl;

  bool removed = false;

  int err = remove_entity(cephx_entity);
  if (err == -ENOENT) {
    dout(10) << __func__ << " " << cephx_entity << " does not exist" << dendl;
  } else {
    removed = true;
  }

  err = remove_entity(lockbox_entity);
  if (err == -ENOENT) {
    dout(10) << __func__ << " " << lockbox_entity << " does not exist" << dendl;
  } else {
    removed = true;
  }

  if (!removed) {
    dout(10) << __func__ << " entities do not exist -- no-op." << dendl;
    return 0;
  }

  // given we have paxos plugged, this will not result in a proposal
  // being triggered, but it will still be needed so that we get our
  // pending state encoded into the paxos' pending transaction.
  propose_pending();
  return 0;
}

bufferlist _encode_cap(const string& cap)
{
  bufferlist bl;
  ::encode(cap, bl);
  return bl;
}

int _create_auth(
    EntityAuth& auth,
    const string& key,
    const map<string,bufferlist>& caps)
{
  if (key.empty())
    return -EINVAL;
  try {
    auth.key.decode_base64(key);
  } catch (buffer::error& e) {
    return -EINVAL;
  }
  auth.caps = caps;
  return 0;
}

int AuthMonitor::validate_osd_new(
    int32_t id,
    const uuid_d& uuid,
    const string& cephx_secret,
    const string& lockbox_secret,
    auth_entity_t& cephx_entity,
    auth_entity_t& lockbox_entity,
    stringstream& ss)
{

  dout(10) << __func__ << " osd." << id << " uuid " << uuid << dendl;

  map<string,bufferlist> cephx_caps = {
    { "osd", _encode_cap("allow *") },
    { "mon", _encode_cap("allow profile osd") },
    { "mgr", _encode_cap("allow profile osd") }
  };
  map<string,bufferlist> lockbox_caps = {
    { "mon", _encode_cap("allow command \"config-key get\" "
        "with key=\"dm-crypt/osd/" +
        stringify(uuid) +
        "/luks\"") }
  };

  bool has_lockbox = !lockbox_secret.empty();

  string cephx_name = "osd." + stringify(id);
  string lockbox_name = "client.osd-lockbox." + stringify(uuid);

  if (!cephx_entity.name.from_str(cephx_name)) {
    dout(10) << __func__ << " invalid cephx entity '"
             << cephx_name << "'" << dendl;
    ss << "invalid cephx key entity '" << cephx_name << "'";
    return -EINVAL;
  }

  if (has_lockbox) {
    if (!lockbox_entity.name.from_str(lockbox_name)) {
      dout(10) << __func__ << " invalid cephx lockbox entity '"
               << lockbox_name << "'" << dendl;
      ss << "invalid cephx lockbox entity '" << lockbox_name << "'";
      return -EINVAL;
    }
  }

  if (entity_is_pending(cephx_entity.name) ||
      (has_lockbox && entity_is_pending(lockbox_entity.name))) {
    // If we have pending entities for either the cephx secret or the
    // lockbox secret, then our safest bet is to retry the command at
    // a later time. These entities may be pending because an `osd new`
    // command has been run (which is unlikely, due to the nature of
    // the operation, which will force a paxos proposal), or (more likely)
    // because a competing client created those entities before we handled
    // the `osd new` command. Regardless, let's wait and see.
    return -EAGAIN;
  }

  if (!is_valid_cephx_key(cephx_secret)) {
    ss << "invalid cephx secret.";
    return -EINVAL;
  }

  if (has_lockbox && !is_valid_cephx_key(lockbox_secret)) {
    ss << "invalid cephx lockbox secret.";
    return -EINVAL;
  }

  int err = _create_auth(cephx_entity.auth, cephx_secret, cephx_caps);
  assert(0 == err);

  bool cephx_is_idempotent = false, lockbox_is_idempotent = false;
  err = exists_and_matches_entity(cephx_entity, true, ss);

  if (err != -ENOENT) {
    if (err < 0) {
      return err;
    }
    assert(0 == err);
    cephx_is_idempotent = true;
  }

  if (has_lockbox) {
    err = _create_auth(lockbox_entity.auth, lockbox_secret, lockbox_caps);
    assert(err == 0);
    err = exists_and_matches_entity(lockbox_entity, true, ss);
    if (err != -ENOENT) {
      if (err < 0) {
        return err;
      }
      assert(0 == err);
      lockbox_is_idempotent = true;
    }
  }

  if (cephx_is_idempotent && (!has_lockbox || lockbox_is_idempotent)) {
    return EEXIST;
  }

  return 0;
}

int AuthMonitor::do_osd_new(
    const auth_entity_t& cephx_entity,
    const auth_entity_t& lockbox_entity,
    bool has_lockbox)
{
  assert(paxos->is_plugged());

  dout(10) << __func__ << " cephx " << cephx_entity.name
           << " lockbox ";
  if (has_lockbox) {
    *_dout << lockbox_entity.name;
  } else {
    *_dout << "n/a";
  }
  *_dout << dendl;

  // we must have validated before reaching this point.
  // if keys exist, then this means they also match; otherwise we would
  // have failed before calling this function.
  bool cephx_exists = mon->key_server.contains(cephx_entity.name);

  if (!cephx_exists) {
    int err = add_entity(cephx_entity.name, cephx_entity.auth);
    assert(0 == err);
  }

  if (has_lockbox &&
      !mon->key_server.contains(lockbox_entity.name)) {
    int err = add_entity(lockbox_entity.name, lockbox_entity.auth);
    assert(0 == err);
  }

  // given we have paxos plugged, this will not result in a proposal
  // being triggered, but it will still be needed so that we get our
  // pending state encoded into the paxos' pending transaction.
  propose_pending();
  return 0;
}

bool AuthMonitor::prepare_command(MonOpRequestRef op)
{
  MMonCommand *m = static_cast<MMonCommand*>(op->get_req());
  stringstream ss, ds;
  bufferlist rdata;
  string rs;
  int err = -EINVAL;

  map<string, cmd_vartype> cmdmap;
  if (!cmdmap_from_json(m->cmd, &cmdmap, ss)) {
    // ss has reason for failure
    string rs = ss.str();
    mon->reply_command(op, -EINVAL, rs, rdata, get_last_committed());
    return true;
  }

  string prefix;
  vector<string>caps_vec;
  string entity_name;
  EntityName entity;

  cmd_getval(g_ceph_context, cmdmap, "prefix", prefix);

  string format;
  cmd_getval(g_ceph_context, cmdmap, "format", format, string("plain"));
  boost::scoped_ptr<Formatter> f(Formatter::create(format));

  MonSession *session = m->get_session();
  if (!session) {
    mon->reply_command(op, -EACCES, "access denied", rdata, get_last_committed());
    return true;
  }

  cmd_getval(g_ceph_context, cmdmap, "caps", caps_vec);
  if ((caps_vec.size() % 2) != 0) {
    ss << "bad capabilities request; odd number of arguments";
    err = -EINVAL;
    goto done;
  }

  cmd_getval(g_ceph_context, cmdmap, "entity", entity_name);
  if (!entity_name.empty() && !entity.from_str(entity_name)) {
    ss << "bad entity name";
    err = -EINVAL;
    goto done;
  }

  if (prefix == "auth import") {
    bufferlist bl = m->get_data();
    if (bl.length() == 0) {
      ss << "auth import: no data supplied";
      getline(ss, rs);
      mon->reply_command(op, -EINVAL, rs, get_last_committed());
      return true;
    }
    bufferlist::iterator iter = bl.begin();
    KeyRing keyring;
    try {
      ::decode(keyring, iter);
    } catch (const buffer::error &ex) {
      ss << "error decoding keyring" << " " << ex.what();
      err = -EINVAL;
      goto done;
    }
    err = import_keyring(keyring);
    if (err < 0) {
      ss << "auth import: no caps supplied";
      getline(ss, rs);
      mon->reply_command(op, -EINVAL, rs, get_last_committed());
      return true;
    }
    ss << "imported keyring";
    getline(ss, rs);
    err = 0;
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
					      get_last_committed() + 1));
    return true;
  } else if (prefix == "auth add" && !entity_name.empty()) {
    /* expected behavior:
     *  - if command reproduces current state, return 0.
     *  - if command adds brand new entity, handle it.
     *  - if command adds new state to existing entity, return error.
     */
    KeyServerData::Incremental auth_inc;
    auth_inc.name = entity;
    bufferlist bl = m->get_data();
    bool has_keyring = (bl.length() > 0);
    map<string,bufferlist> new_caps;

    KeyRing new_keyring;
    if (has_keyring) {
      bufferlist::iterator iter = bl.begin();
      try {
        ::decode(new_keyring, iter);
      } catch (const buffer::error &ex) {
        ss << "error decoding keyring";
        err = -EINVAL;
        goto done;
      }
    }

    // are we about to have it?
    if (entity_is_pending(entity)) {
      wait_for_finished_proposal(op,
          new Monitor::C_Command(mon, op, 0, rs, get_last_committed() + 1));
      return true;
    }

    // build new caps from provided arguments (if available)
    for (vector<string>::iterator it = caps_vec.begin();
	 it != caps_vec.end() && (it + 1) != caps_vec.end();
	 it += 2) {
      string sys = *it;
      bufferlist cap;
      ::encode(*(it+1), cap);
      new_caps[sys] = cap;
    }

    // pull info out of provided keyring
    EntityAuth new_inc;
    if (has_keyring) {
      if (!new_keyring.get_auth(auth_inc.name, new_inc)) {
	ss << "key for " << auth_inc.name
	   << " not found in provided keyring";
	err = -EINVAL;
	goto done;
      }
      if (!new_caps.empty() && !new_inc.caps.empty()) {
	ss << "caps cannot be specified both in keyring and in command";
	err = -EINVAL;
	goto done;
      }
      if (new_caps.empty()) {
	new_caps = new_inc.caps;
      }
    }

    err = exists_and_matches_entity(auth_inc.name, new_inc,
                                    new_caps, has_keyring, ss);
    // if entity/key/caps do not exist in the keyring, just fall through
    // and add the entity; otherwise, make sure everything matches (in
    // which case it's a no-op), because if not we must fail.
    if (err != -ENOENT) {
      if (err < 0) {
        goto done;
      }
      // no-op.
      assert(err == 0);
      goto done;
    }
    err = 0;

    // okay, add it.
    if (!has_keyring) {
      dout(10) << "AuthMonitor::prepare_command generating random key for "
        << auth_inc.name << dendl;
      new_inc.key.create(g_ceph_context, CEPH_CRYPTO_AES);
    }
    new_inc.caps = new_caps;

    err = add_entity(auth_inc.name, new_inc);
    assert(err == 0);

    ss << "added key for " << auth_inc.name;
    getline(ss, rs);
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
						   get_last_committed() + 1));
    return true;
  } else if ((prefix == "auth get-or-create-key" ||
	     prefix == "auth get-or-create") &&
	     !entity_name.empty()) {
    // auth get-or-create <name> [mon osdcapa osd osdcapb ...]

    if (!valid_caps(caps_vec, &ss)) {
      err = -EINVAL;
      goto done;
    }

    // Parse the list of caps into a map
    std::map<std::string, bufferlist> wanted_caps;
    for (vector<string>::const_iterator it = caps_vec.begin();
	 it != caps_vec.end() && (it + 1) != caps_vec.end();
	 it += 2) {
      const std::string &sys = *it;
      bufferlist cap;
      ::encode(*(it+1), cap);
      wanted_caps[sys] = cap;
    }

    // do we have it?
    EntityAuth entity_auth;
    if (mon->key_server.get_auth(entity, entity_auth)) {
      for (const auto &sys_cap : wanted_caps) {
	if (entity_auth.caps.count(sys_cap.first) == 0 ||
	    !entity_auth.caps[sys_cap.first].contents_equal(sys_cap.second)) {
	  ss << "key for " << entity << " exists but cap " << sys_cap.first
            << " does not match";
	  err = -EINVAL;
	  goto done;
	}
      }

      if (prefix == "auth get-or-create-key") {
        if (f) {
          entity_auth.key.encode_formatted("auth", f.get(), rdata);
        } else {
          ds << entity_auth.key;
        }
      } else {
	KeyRing kr;
	kr.add(entity, entity_auth.key);
        if (f) {
          kr.set_caps(entity, entity_auth.caps);
          kr.encode_formatted("auth", f.get(), rdata);
        } else {
          kr.encode_plaintext(rdata);
        }
      }
      err = 0;
      goto done;
    }

    // ...or are we about to?
    for (vector<Incremental>::iterator p = pending_auth.begin();
	 p != pending_auth.end();
	 ++p) {
      if (p->inc_type == AUTH_DATA) {
	KeyServerData::Incremental auth_inc;
	bufferlist::iterator q = p->auth_data.begin();
	::decode(auth_inc, q);
	if (auth_inc.op == KeyServerData::AUTH_INC_ADD &&
	    auth_inc.name == entity) {
	  wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
						get_last_committed() + 1));
	  return true;
	}
      }
    }

    // create it
    KeyServerData::Incremental auth_inc;
    auth_inc.op = KeyServerData::AUTH_INC_ADD;
    auth_inc.name = entity;
    auth_inc.auth.key.create(g_ceph_context, CEPH_CRYPTO_AES);
    auth_inc.auth.caps = wanted_caps;

    push_cephx_inc(auth_inc);

    if (prefix == "auth get-or-create-key") {
      if (f) {
        auth_inc.auth.key.encode_formatted("auth", f.get(), rdata);
      } else {
        ds << auth_inc.auth.key;
      }
    } else {
      KeyRing kr;
      kr.add(entity, auth_inc.auth.key);
      if (f) {
        kr.set_caps(entity, wanted_caps);
        kr.encode_formatted("auth", f.get(), rdata);
      } else {
        kr.encode_plaintext(rdata);
      }
    }

    rdata.append(ds);
    getline(ss, rs);
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs, rdata,
					      get_last_committed() + 1));
    return true;
  } else if (prefix == "fs authorize") {
    string filesystem;
    cmd_getval(g_ceph_context, cmdmap, "filesystem", filesystem);
    string mds_cap_string, osd_cap_string;
    string osd_cap_wanted = "r";

    for (auto it = caps_vec.begin();
	 it != caps_vec.end() && (it + 1) != caps_vec.end();
	 it += 2) {
      const string &path = *it;
      const string &cap = *(it+1);
      if (cap != "r" && cap != "rw" && cap != "rwp") {
	ss << "Only 'r', 'rw', and 'rwp' permissions are allowed for filesystems.";
	err = -EINVAL;
	goto done;
      }
      if (cap.find('w') != string::npos) {
	osd_cap_wanted = "rw";
      }

      mds_cap_string += mds_cap_string.empty() ? "" : ", ";
      mds_cap_string += "allow " + cap;
      if (path != "/") {
	mds_cap_string += " path=" + path;
      }
    }

    auto fs = mon->mdsmon()->get_fsmap().get_filesystem(filesystem);
    if (!fs) {
      ss << "filesystem " << filesystem << " does not exist.";
      err = -EINVAL;
      goto done;
    }

    auto data_pools = fs->mds_map.get_data_pools();
    for (auto p : data_pools) {
      const string &pool_name = mon->osdmon()->osdmap.get_pool_name(p);
      osd_cap_string += osd_cap_string.empty() ? "" : ", ";
      osd_cap_string += "allow " + osd_cap_wanted + " pool=" + pool_name;
    }

    std::map<string, bufferlist> wanted_caps = {
      { "mon", _encode_cap("allow r") },
      { "osd", _encode_cap(osd_cap_string) },
      { "mds", _encode_cap(mds_cap_string) }
    };

    EntityAuth entity_auth;
    if (mon->key_server.get_auth(entity, entity_auth)) {
      for (const auto &sys_cap : wanted_caps) {
	if (entity_auth.caps.count(sys_cap.first) == 0 ||
	    !entity_auth.caps[sys_cap.first].contents_equal(sys_cap.second)) {
	  ss << "key for " << entity << " exists but cap " << sys_cap.first
	     << " does not match";
	  err = -EINVAL;
	  goto done;
	}
      }

      KeyRing kr;
      kr.add(entity, entity_auth.key);
      if (f) {
	kr.set_caps(entity, entity_auth.caps);
	kr.encode_formatted("auth", f.get(), rdata);
      } else {
	kr.encode_plaintext(rdata);
      }
      err = 0;
      goto done;
    }

    KeyServerData::Incremental auth_inc;
    auth_inc.op = KeyServerData::AUTH_INC_ADD;
    auth_inc.name = entity;
    auth_inc.auth.key.create(g_ceph_context, CEPH_CRYPTO_AES);
    auth_inc.auth.caps = wanted_caps;

    push_cephx_inc(auth_inc);
    KeyRing kr;
    kr.add(entity, auth_inc.auth.key);
    if (f) {
      kr.set_caps(entity, wanted_caps);
      kr.encode_formatted("auth", f.get(), rdata);
    } else {
      kr.encode_plaintext(rdata);
    }

    rdata.append(ds);
    getline(ss, rs);
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs, rdata,
						  get_last_committed() + 1));
    return true;
  } else if (prefix == "auth caps" && !entity_name.empty()) {
    KeyServerData::Incremental auth_inc;
    auth_inc.name = entity;
    if (!mon->key_server.get_auth(auth_inc.name, auth_inc.auth)) {
      ss << "couldn't find entry " << auth_inc.name;
      err = -ENOENT;
      goto done;
    }

    if (!valid_caps(caps_vec, &ss)) {
      err = -EINVAL;
      goto done;
    }

    map<string,bufferlist> newcaps;
    for (vector<string>::iterator it = caps_vec.begin();
	 it != caps_vec.end(); it += 2)
      ::encode(*(it+1), newcaps[*it]);

    auth_inc.op = KeyServerData::AUTH_INC_ADD;
    auth_inc.auth.caps = newcaps;
    push_cephx_inc(auth_inc);

    ss << "updated caps for " << auth_inc.name;
    getline(ss, rs);
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
					      get_last_committed() + 1));
    return true;
  } else if ((prefix == "auth del" || prefix == "auth rm") &&
             !entity_name.empty()) {
    KeyServerData::Incremental auth_inc;
    auth_inc.name = entity;
    if (!mon->key_server.contains(auth_inc.name)) {
      ss << "entity " << entity << " does not exist";
      err = 0;
      goto done;
    }
    auth_inc.op = KeyServerData::AUTH_INC_DEL;
    push_cephx_inc(auth_inc);

    ss << "updated";
    getline(ss, rs);
    wait_for_finished_proposal(op, new Monitor::C_Command(mon, op, 0, rs,
					      get_last_committed() + 1));
    return true;
  }
done:
  rdata.append(ds);
  getline(ss, rs, '\0');
  mon->reply_command(op, err, rs, rdata, get_last_committed());
  return false;
}

bool AuthMonitor::prepare_global_id(MonOpRequestRef op)
{
  dout(10) << "AuthMonitor::prepare_global_id" << dendl;
  increase_max_global_id();

  return true;
}

void AuthMonitor::upgrade_format()
{
  unsigned int current = 2;
  if (!mon->get_quorum_mon_features().contains_all(
	ceph::features::mon::FEATURE_LUMINOUS)) {
    current = 1;
  }
  if (format_version >= current) {
    dout(20) << __func__ << " format " << format_version << " is current" << dendl;
    return;
  }

  bool changed = false;
  if (format_version == 0) {
    dout(1) << __func__ << " upgrading from format 0 to 1" << dendl;
    map<EntityName, EntityAuth>::iterator p;
    for (p = mon->key_server.secrets_begin();
	 p != mon->key_server.secrets_end();
	 ++p) {
      // grab mon caps, if any
      string mon_caps;
      if (p->second.caps.count("mon") == 0)
	continue;
      try {
	bufferlist::iterator it = p->second.caps["mon"].begin();
	::decode(mon_caps, it);
      }
      catch (buffer::error) {
	dout(10) << __func__ << " unable to parse mon cap for "
		 << p->first << dendl;
	continue;
      }

      string n = p->first.to_str();
      string new_caps;

      // set daemon profiles
      if ((p->first.is_osd() || p->first.is_mds()) &&
	  mon_caps == "allow rwx") {
	new_caps = string("allow profile ") + string(p->first.get_type_name());
      }

      // update bootstrap keys
      if (n == "client.bootstrap-osd") {
	new_caps = "allow profile bootstrap-osd";
      }
      if (n == "client.bootstrap-mds") {
	new_caps = "allow profile bootstrap-mds";
      }

      if (new_caps.length() > 0) {
	dout(5) << __func__ << " updating " << p->first << " mon cap from "
		<< mon_caps << " to " << new_caps << dendl;

	bufferlist bl;
	::encode(new_caps, bl);

	KeyServerData::Incremental auth_inc;
	auth_inc.name = p->first;
	auth_inc.auth = p->second;
	auth_inc.auth.caps["mon"] = bl;
	auth_inc.op = KeyServerData::AUTH_INC_ADD;
	push_cephx_inc(auth_inc);
	changed = true;
      }
    }
  }

  if (format_version == 1) {
    dout(1) << __func__ << " upgrading from format 1 to 2" << dendl;
    map<EntityName, EntityAuth>::iterator p;
    for (p = mon->key_server.secrets_begin();
	 p != mon->key_server.secrets_end();
	 ++p) {
      string n = p->first.to_str();

      string newcap;
      if (n == "client.admin") {
	// admin gets it all
	newcap = "allow *";
      } else if (n.find("osd.") == 0 ||
		 n.find("mds.") == 0 ||
		 n.find("mon.") == 0) {
	// daemons follow their profile
	string type = n.substr(0, 3);
	newcap = "allow profile " + type;
      } else if (p->second.caps.count("mon")) {
	// if there are any mon caps, give them 'r' mgr caps
	newcap = "allow r";
      }

      if (newcap.length() > 0) {
	dout(5) << " giving " << n << " mgr '" << newcap << "'" << dendl;
	bufferlist bl;
	::encode(newcap, bl);

	KeyServerData::Incremental auth_inc;
	auth_inc.name = p->first;
	auth_inc.auth = p->second;
	auth_inc.auth.caps["mgr"] = bl;
	auth_inc.op = KeyServerData::AUTH_INC_ADD;
	push_cephx_inc(auth_inc);
      }

      if (n.find("mgr.") == 0 &&
	  p->second.caps.count("mon")) {
	// the kraken ceph-mgr@.service set the mon cap to 'allow *'.
	auto blp = p->second.caps["mon"].begin();
	string oldcaps;
	::decode(oldcaps, blp);
	if (oldcaps == "allow *") {
	  dout(5) << " fixing " << n << " mon cap to 'allow profile mgr'"
		  << dendl;
	  bufferlist bl;
	  ::encode("allow profile mgr", bl);
	  KeyServerData::Incremental auth_inc;
	  auth_inc.name = p->first;
	  auth_inc.auth = p->second;
	  auth_inc.auth.caps["mon"] = bl;
	  auth_inc.op = KeyServerData::AUTH_INC_ADD;
	  push_cephx_inc(auth_inc);
	}
      }
    }

    // add bootstrap key if it does not already exist
    // (might have already been get-or-create'd by
    //  ceph-create-keys)
    EntityName bootstrap_mgr_name;
    int r = bootstrap_mgr_name.from_str("client.bootstrap-mgr");
    assert(r);
    if (!mon->key_server.contains(bootstrap_mgr_name)) {
      KeyServerData::Incremental auth_inc;
      auth_inc.name = bootstrap_mgr_name;
      ::encode("allow profile bootstrap-mgr", auth_inc.auth.caps["mon"]);
      auth_inc.op = KeyServerData::AUTH_INC_ADD;
      // generate key
      auth_inc.auth.key.create(g_ceph_context, CEPH_CRYPTO_AES);
      push_cephx_inc(auth_inc);
    }
    changed = true;
  }

  if (changed) {
    // note new format
    dout(10) << __func__ << " proposing update from format " << format_version
	     << " -> " << current << dendl;
    format_version = current;
    propose_pending();
  }
}

void AuthMonitor::dump_info(Formatter *f)
{
  /*** WARNING: do not include any privileged information here! ***/
  f->open_object_section("auth");
  f->dump_unsigned("first_committed", get_first_committed());
  f->dump_unsigned("last_committed", get_last_committed());
  f->dump_unsigned("num_secrets", mon->key_server.get_num_secrets());
  f->close_section();
}
