// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2015 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "common/perf_counters.h"

#include "mds/MDSRank.h"
#include "mds/MDCache.h"
#include "mds/MDLog.h"
#include "mds/CDir.h"
#include "mds/CDentry.h"
#include "events/EUpdate.h"
#include "messages/MClientRequest.h"

#include "StrayManager.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mds
#undef dout_prefix
#define dout_prefix _prefix(_dout, mds)
static ostream& _prefix(std::ostream *_dout, MDSRank *mds) {
  return *_dout << "mds." << mds->get_nodeid() << ".cache.strays ";
}

class StrayManagerIOContext : public virtual MDSIOContextBase {
protected:
  StrayManager *sm;
  MDSRank *get_mds() override
  {
    return sm->mds;
  }
public:
  explicit StrayManagerIOContext(StrayManager *sm_) : sm(sm_) {}
};

class StrayManagerLogContext : public virtual MDSLogContextBase {
protected:
  StrayManager *sm;
  MDSRank *get_mds() override
  {
    return sm->mds;
  }
public:
  explicit StrayManagerLogContext(StrayManager *sm_) : sm(sm_) {}
};

class StrayManagerContext : public virtual MDSInternalContextBase {
protected:
  StrayManager *sm;
  MDSRank *get_mds() override
  {
    return sm->mds;
  }
public:
  explicit StrayManagerContext(StrayManager *sm_) : sm(sm_) {}
};


/**
 * Context wrapper for _purge_stray_purged completion
 */
class C_IO_PurgeStrayPurged : public StrayManagerIOContext {
  CDentry *dn;
  bool only_head;
public:
  C_IO_PurgeStrayPurged(StrayManager *sm_, CDentry *d, bool oh) : 
    StrayManagerIOContext(sm_), dn(d), only_head(oh) { }
  void finish(int r) override {
    assert(r == 0 || r == -ENOENT);
    sm->_purge_stray_purged(dn, only_head);
  }
};


void StrayManager::purge(CDentry *dn)
{
  CDentry::linkage_t *dnl = dn->get_projected_linkage();
  CInode *in = dnl->get_inode();
  dout(10) << __func__ << " " << *dn << " " << *in << dendl;
  assert(!dn->is_replicated());

  // CHEAT.  there's no real need to journal our intent to purge, since
  // that is implicit in the dentry's presence and non-use in the stray
  // dir.  on recovery, we'll need to re-eval all strays anyway.
  
  SnapContext nullsnapc;

  PurgeItem item;
  item.ino = in->inode.ino;
  if (in->is_dir()) {
    item.action = PurgeItem::PURGE_DIR;
    item.fragtree = in->dirfragtree;
  } else {
    item.action = PurgeItem::PURGE_FILE;

    const SnapContext *snapc;
    SnapRealm *realm = in->find_snaprealm();
    if (realm) {
      dout(10) << " realm " << *realm << dendl;
      snapc = &realm->get_snap_context();
    } else {
      dout(10) << " NO realm, using null context" << dendl;
      snapc = &nullsnapc;
      assert(in->last == CEPH_NOSNAP);
    }

    uint64_t to = 0;
    if (in->is_file()) {
      to = in->inode.get_max_size();
      to = MAX(in->inode.size, to);
      // when truncating a file, the filer does not delete stripe objects that are
      // truncated to zero. so we need to purge stripe objects up to the max size
      // the file has ever been.
      to = MAX(in->inode.max_size_ever, to);
    }

    auto pi = in->get_projected_inode();

    item.size = to;
    item.layout = pi->layout;
    item.old_pools.clear();
    for (const auto &p : pi->old_pools)
      item.old_pools.insert(p);
    item.snapc = *snapc;
  }

  purge_queue.push(item, new C_IO_PurgeStrayPurged(
        this, dn, false));
}

class C_PurgeStrayLogged : public StrayManagerLogContext {
  CDentry *dn;
  version_t pdv;
  LogSegment *ls;
public:
  C_PurgeStrayLogged(StrayManager *sm_, CDentry *d, version_t v, LogSegment *s) : 
    StrayManagerLogContext(sm_), dn(d), pdv(v), ls(s) { }
  void finish(int r) override {
    sm->_purge_stray_logged(dn, pdv, ls);
  }
};

class C_TruncateStrayLogged : public StrayManagerLogContext {
  CDentry *dn;
  LogSegment *ls;
public:
  C_TruncateStrayLogged(StrayManager *sm, CDentry *d, LogSegment *s) :
    StrayManagerLogContext(sm), dn(d), ls(s) { }
  void finish(int r) override {
    sm->_truncate_stray_logged(dn, ls);
  }
};

void StrayManager::_purge_stray_purged(
    CDentry *dn, bool only_head)
{
  CInode *in = dn->get_projected_linkage()->get_inode();
  dout(10) << "_purge_stray_purged " << *dn << " " << *in << dendl;

  logger->inc(l_mdc_strays_enqueued);
  num_strays_enqueuing--;
  logger->set(l_mdc_num_strays_enqueuing, num_strays_enqueuing);

  if (only_head) {
    /* This was a ::truncate */
    EUpdate *le = new EUpdate(mds->mdlog, "purge_stray truncate");
    mds->mdlog->start_entry(le);
    
    auto &pi = in->project_inode();
    pi.inode.size = 0;
    pi.inode.max_size_ever = 0;
    pi.inode.client_ranges.clear();
    pi.inode.truncate_size = 0;
    pi.inode.truncate_from = 0;
    pi.inode.version = in->pre_dirty();

    le->metablob.add_dir_context(dn->dir);
    le->metablob.add_primary_dentry(dn, in, true);

    mds->mdlog->submit_entry(le,
        new C_TruncateStrayLogged(
          this, dn, mds->mdlog->get_current_segment()));
  } else {
    if (in->get_num_ref() != (int)in->is_dirty() ||
        dn->get_num_ref() != (int)dn->is_dirty() + !!in->get_num_ref() + 1/*PIN_PURGING*/) {
      // Nobody should be taking new references to an inode when it
      // is being purged (aside from it were 

      derr << "Rogue reference after purge to " << *dn << dendl;
      assert(0 == "rogue reference to purging inode");
    }

    // kill dentry.
    version_t pdv = dn->pre_dirty();
    dn->push_projected_linkage(); // NULL

    EUpdate *le = new EUpdate(mds->mdlog, "purge_stray");
    mds->mdlog->start_entry(le);

    // update dirfrag fragstat, rstat
    CDir *dir = dn->get_dir();
    fnode_t *pf = dir->project_fnode();
    pf->version = dir->pre_dirty();
    if (in->is_dir())
      pf->fragstat.nsubdirs--;
    else
      pf->fragstat.nfiles--;
    pf->rstat.sub(in->inode.accounted_rstat);

    le->metablob.add_dir_context(dn->dir);
    EMetaBlob::dirlump& dl = le->metablob.add_dir(dn->dir, true);
    le->metablob.add_null_dentry(dl, dn, true);
    le->metablob.add_destroyed_inode(in->ino());

    mds->mdlog->submit_entry(le, new C_PurgeStrayLogged(this, dn, pdv,
          mds->mdlog->get_current_segment()));

    logger->set(l_mdc_num_strays, num_strays);
  }
}

void StrayManager::_purge_stray_logged(CDentry *dn, version_t pdv, LogSegment *ls)
{
  CInode *in = dn->get_linkage()->get_inode();
  dout(10) << "_purge_stray_logged " << *dn << " " << *in << dendl;

  assert(!in->state_test(CInode::STATE_RECOVERING));

  bool new_dn = dn->is_new();

  // unlink
  assert(dn->get_projected_linkage()->is_null());
  dn->dir->unlink_inode(dn, !new_dn);
  dn->pop_projected_linkage();
  dn->mark_dirty(pdv, ls);

  dn->dir->pop_and_dirty_projected_fnode(ls);

  in->state_clear(CInode::STATE_ORPHAN);
  dn->state_clear(CDentry::STATE_PURGING | CDentry::STATE_PURGINGPINNED);
  dn->put(CDentry::PIN_PURGING);

  // drop dentry?
  if (new_dn) {
    dout(20) << " dn is new, removing" << dendl;
    dn->mark_clean();
    dn->dir->remove_dentry(dn);
  }

  // drop inode
  if (in->is_dirty())
    in->mark_clean();
  in->mdcache->remove_inode(in);
}

void StrayManager::enqueue(CDentry *dn, bool trunc)
{
  CDentry::linkage_t *dnl = dn->get_projected_linkage();
  assert(dnl);
  CInode *in = dnl->get_inode();
  assert(in);

  /* We consider a stray to be purging as soon as it is enqueued, to avoid
   * enqueing it twice */
  dn->state_set(CDentry::STATE_PURGING);
  in->state_set(CInode::STATE_PURGING);

  /* We must clear this as soon as enqueuing it, to prevent the journal
   * expiry code from seeing a dirty parent and trying to write a backtrace */
  if (!trunc) {
    if (in->is_dirty_parent()) {
      in->clear_dirty_parent();
    }
  }

  dout(20) << __func__ << ": purging dn: " << *dn << dendl;

  if (!dn->state_test(CDentry::STATE_PURGINGPINNED)) {
    dn->get(CDentry::PIN_PURGING);
    dn->state_set(CDentry::STATE_PURGINGPINNED);
  }

  ++num_strays_enqueuing;
  logger->set(l_mdc_num_strays_enqueuing, num_strays_enqueuing);

  // Resources are available, acquire them and execute the purge
  _enqueue(dn, trunc);

  dout(10) << __func__ << ": purging this dentry immediately: "
    << *dn << dendl;
}

class C_OpenSnapParents : public StrayManagerContext {
  CDentry *dn;
  bool trunc;
  public:
    C_OpenSnapParents(StrayManager *sm_, CDentry *dn_, bool t) :
      StrayManagerContext(sm_), dn(dn_), trunc(t) { }
    void finish(int r) override {
      sm->_enqueue(dn, trunc);
    }
};

void StrayManager::_enqueue(CDentry *dn, bool trunc)
{
  assert(started);

  CInode *in = dn->get_linkage()->get_inode();
  if (in->snaprealm &&
      !in->snaprealm->have_past_parents_open() &&
      !in->snaprealm->open_parents(new C_OpenSnapParents(this, dn, trunc))) {
    // this can happen if the dentry had been trimmed from cache.
    return;
  }

  if (trunc) {
    truncate(dn);
  } else {
    purge(dn);
  }
}


void StrayManager::advance_delayed()
{
  if (!started)
    return;

  for (elist<CDentry*>::iterator p = delayed_eval_stray.begin(); !p.end(); ) {
    CDentry *dn = *p;
    ++p;
    dn->item_stray.remove_myself();
    num_strays_delayed--;

    if (dn->get_projected_linkage()->is_null()) {
      /* A special case: a stray dentry can go null if its inode is being
       * re-linked into another MDS's stray dir during a shutdown migration. */
      dout(4) << __func__ << ": delayed dentry is now null: " << *dn << dendl;
      continue;
    }

    const bool purging = eval_stray(dn);
    if (!purging) {
      derr << "Dentry " << *dn << " was purgeable but no longer is!" << dendl;
      /*
       * This can happen if a stray is purgeable, but has gained an extra
       * reference by virtue of having its backtrace updated.
       * FIXME perhaps we could simplify this further by
       * avoiding writing the backtrace of purge-ready strays, so
       * that this code could be more rigid?
       */
    }
  }
  logger->set(l_mdc_num_strays_delayed, num_strays_delayed);
}

void StrayManager::set_num_strays(uint64_t num)
{
  assert(!started);
  num_strays = num;
  logger->set(l_mdc_num_strays, num_strays);
}

void StrayManager::notify_stray_created()
{
  num_strays++;
  logger->set(l_mdc_num_strays, num_strays);
  logger->inc(l_mdc_strays_created);
}

void StrayManager::notify_stray_removed()
{
  num_strays--;
  logger->set(l_mdc_num_strays, num_strays);
}

struct C_EvalStray : public StrayManagerContext {
  CDentry *dn;
  C_EvalStray(StrayManager *sm_, CDentry *d) : StrayManagerContext(sm_), dn(d) {}
  void finish(int r) override {
    sm->eval_stray(dn);
  }
};

struct C_MDC_EvalStray : public StrayManagerContext {
  CDentry *dn;
  C_MDC_EvalStray(StrayManager *sm_, CDentry *d) : StrayManagerContext(sm_), dn(d) {}
  void finish(int r) override {
    sm->eval_stray(dn);
  }
};

bool StrayManager::_eval_stray(CDentry *dn, bool delay)
{
  dout(10) << "eval_stray " << *dn << dendl;
  CDentry::linkage_t *dnl = dn->get_projected_linkage();
  assert(dnl->is_primary());
  dout(10) << " inode is " << *dnl->get_inode() << dendl;
  CInode *in = dnl->get_inode();
  assert(in);
  assert(!in->state_test(CInode::STATE_REJOINUNDEF));

  // The only dentries elegible for purging are those
  // in the stray directories
  assert(dn->get_dir()->get_inode()->is_stray());

  // Inode may not pass through this function if it
  // was already identified for purging (i.e. cannot
  // call eval_stray() after purge()
  assert(!dn->state_test(CDentry::STATE_PURGING));

  if (!dn->is_auth()) {
    return false;
  }

  if (!started)
    delay = true;

  if (dn->item_stray.is_on_list()) {
    if (delay)
      return false;

    dn->item_stray.remove_myself();
    num_strays_delayed--;
    logger->set(l_mdc_num_strays_delayed, num_strays_delayed);
  }

  // purge?
  if (in->inode.nlink == 0) {
    // past snaprealm parents imply snapped dentry remote links.
    // only important for directories.  normal file data snaps are handled
    // by the object store.
    if (in->snaprealm) {
      if (!in->snaprealm->have_past_parents_open() &&
          !in->snaprealm->open_parents(new C_MDC_EvalStray(this, dn))) {
        return false;
      }
      in->snaprealm->prune_past_parents();
      in->purge_stale_snap_data(in->snaprealm->get_snaps());
    }
    if (in->is_dir()) {
      if (in->snaprealm && in->snaprealm->has_past_parents()) {
	dout(20) << "  directory has past parents "
		 << in->snaprealm->srnode.past_parents << dendl;
	if (in->state_test(CInode::STATE_MISSINGOBJS)) {
	  mds->clog->error() << "previous attempt at committing dirfrag of ino "
			     << in->ino() << " has failed, missing object";
	  mds->handle_write_error(-ENOENT);
	}
	return false;  // not until some snaps are deleted.
      }

      in->mdcache->clear_dirty_bits_for_stray(in);

      if (!in->remote_parents.empty()) {
	// unlink any stale remote snap dentry.
	for (auto it = in->remote_parents.begin(); it != in->remote_parents.end(); ) {
	  CDentry *remote_dn = *it;
	  ++it;
	  assert(remote_dn->last != CEPH_NOSNAP);
	  remote_dn->unlink_remote(remote_dn->get_linkage());
	}
      }
    }
    if (dn->is_replicated()) {
      dout(20) << " replicated" << dendl;
      return false;
    }
    if (dn->is_any_leases() || in->is_any_caps()) {
      dout(20) << " caps | leases" << dendl;
      return false;  // wait
    }
    if (in->state_test(CInode::STATE_NEEDSRECOVER) ||
	in->state_test(CInode::STATE_RECOVERING)) {
      dout(20) << " pending recovery" << dendl;
      return false;  // don't mess with file size probing
    }
    if (in->get_num_ref() > (int)in->is_dirty() + (int)in->is_dirty_parent()) {
      dout(20) << " too many inode refs" << dendl;
      return false;
    }
    if (dn->get_num_ref() > (int)dn->is_dirty() + !!in->get_num_ref()) {
      dout(20) << " too many dn refs" << dendl;
      return false;
    }
    if (delay) {
      if (!dn->item_stray.is_on_list()) {
	delayed_eval_stray.push_back(&dn->item_stray);
	num_strays_delayed++;
	logger->set(l_mdc_num_strays_delayed, num_strays_delayed);
      }
    // don't purge multiversion inode with snap data
    } else if (in->snaprealm && in->snaprealm->has_past_parents() &&
              !in->old_inodes.empty()) {
      // A file with snapshots: we will truncate the HEAD revision
      // but leave the metadata intact.
      assert(!in->is_dir());
      dout(20) << " file has past parents "
        << in->snaprealm->srnode.past_parents << dendl;
      if (in->is_file() && in->get_projected_inode()->size > 0) {
	enqueue(dn, true); // truncate head objects    
      }
    } else {
      // A straightforward file, ready to be purged.  Enqueue it.
      if (in->is_dir()) {
	in->close_dirfrags();
      }

      enqueue(dn, false);
    }

    return true;
  } else {
    /*
     * Where a stray has some links, they should be remotes, check
     * if we can do anything with them if we happen to have them in
     * cache.
     */
    _eval_stray_remote(dn, NULL);
    return false;
  }
}

void StrayManager::activate()
{
  dout(10) << __func__ << dendl;
  started = true;
  purge_queue.activate();
}

bool StrayManager::eval_stray(CDentry *dn, bool delay)
{
  // avoid nested eval_stray
  if (dn->state_test(CDentry::STATE_EVALUATINGSTRAY))
      return false;

  dn->state_set(CDentry::STATE_EVALUATINGSTRAY);
  bool ret = _eval_stray(dn, delay);
  dn->state_clear(CDentry::STATE_EVALUATINGSTRAY);
  return ret;
}

void StrayManager::eval_remote(CDentry *remote_dn)
{
  dout(10) << __func__ << " " << *remote_dn << dendl;

  CDentry::linkage_t *dnl = remote_dn->get_projected_linkage();
  assert(dnl->is_remote());
  CInode *in = dnl->get_inode();

  if (!in) {
    dout(20) << __func__ << ": no inode, cannot evaluate" << dendl;
    return;
  }

  if (remote_dn->last != CEPH_NOSNAP) {
    dout(20) << __func__ << ": snap dentry, cannot evaluate" << dendl;
    return;
  }

  // refers to stray?
  CDentry *primary_dn = in->get_projected_parent_dn();
  assert(primary_dn != NULL);
  if (primary_dn->get_dir()->get_inode()->is_stray()) {
    _eval_stray_remote(primary_dn, remote_dn);
  } else {
    dout(20) << __func__ << ": inode's primary dn not stray" << dendl;
  }
}

class C_RetryEvalRemote : public StrayManagerContext {
  CDentry *dn;
  public:
    C_RetryEvalRemote(StrayManager *sm_, CDentry *dn_) :
      StrayManagerContext(sm_), dn(dn_) {
      dn->get(CDentry::PIN_PTRWAITER);
    }
    void finish(int r) override {
      if (dn->get_projected_linkage()->is_remote())
	sm->eval_remote(dn);
      dn->put(CDentry::PIN_PTRWAITER);
    }
};

void StrayManager::_eval_stray_remote(CDentry *stray_dn, CDentry *remote_dn)
{
  dout(20) << __func__ << " " << *stray_dn << dendl;
  assert(stray_dn != NULL);
  assert(stray_dn->get_dir()->get_inode()->is_stray());
  CDentry::linkage_t *stray_dnl = stray_dn->get_projected_linkage();
  assert(stray_dnl->is_primary());
  CInode *stray_in = stray_dnl->get_inode();
  assert(stray_in->inode.nlink >= 1);
  assert(stray_in->last == CEPH_NOSNAP);

  /* If no remote_dn hinted, pick one arbitrarily */
  if (remote_dn == NULL) {
    if (!stray_in->remote_parents.empty()) {
      for (const auto &dn : stray_in->remote_parents) {
	if (dn->last == CEPH_NOSNAP && !dn->is_projected()) {
	  if (dn->is_auth()) {
	    remote_dn = dn;
	    if (remote_dn->dir->can_auth_pin())
	      break;
	  } else if (!remote_dn) {
	    remote_dn = dn;
	  }
	}
      }
    }
    if (!remote_dn) {
      dout(20) << __func__ << ": not reintegrating (no remote parents in cache)" << dendl;
      return;
    }
  }
  assert(remote_dn->last == CEPH_NOSNAP);
  // NOTE: we repeat this check in _rename(), since our submission path is racey.
  if (!remote_dn->is_projected()) {
    if (remote_dn->is_auth()) {
      if (remote_dn->dir->can_auth_pin()) {
	reintegrate_stray(stray_dn, remote_dn);
      } else {
	remote_dn->dir->add_waiter(CDir::WAIT_UNFREEZE, new C_RetryEvalRemote(this, remote_dn));
	dout(20) << __func__ << ": not reintegrating (can't authpin remote parent)" << dendl;
      }

    } else if (!remote_dn->is_auth() && stray_dn->is_auth()) {
      migrate_stray(stray_dn, remote_dn->authority().first);
    } else {
      dout(20) << __func__ << ": not reintegrating" << dendl;
    }
  } else {
    // don't do anything if the remote parent is projected, or we may
    // break user-visible semantics!
    dout(20) << __func__ << ": not reintegrating (projected)" << dendl;
  }
}

void StrayManager::reintegrate_stray(CDentry *straydn, CDentry *rdn)
{
  dout(10) << __func__ << " " << *straydn << " into " << *rdn << dendl;

  logger->inc(l_mdc_strays_reintegrated);
  
  // rename it to another mds.
  filepath src;
  straydn->make_path(src);
  filepath dst;
  rdn->make_path(dst);

  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_RENAME);
  req->set_filepath(dst);
  req->set_filepath2(src);
  req->set_tid(mds->issue_tid());

  mds->send_message_mds(req, rdn->authority().first);
}
 
void StrayManager::migrate_stray(CDentry *dn, mds_rank_t to)
{
  CInode *in = dn->get_projected_linkage()->get_inode();
  assert(in);
  CInode *diri = dn->dir->get_inode();
  assert(diri->is_stray());
  dout(10) << "migrate_stray from mds." << MDS_INO_STRAY_OWNER(diri->inode.ino)
	   << " to mds." << to
	   << " " << *dn << " " << *in << dendl;

  logger->inc(l_mdc_strays_migrated);

  // rename it to another mds.
  filepath src;
  dn->make_path(src);
  assert(src.depth() == 2);

  filepath dst(MDS_INO_MDSDIR(to));
  dst.push_dentry(src[0]);
  dst.push_dentry(src[1]);

  MClientRequest *req = new MClientRequest(CEPH_MDS_OP_RENAME);
  req->set_filepath(dst);
  req->set_filepath2(src);
  req->set_tid(mds->issue_tid());

  mds->send_message_mds(req, to);
}

StrayManager::StrayManager(MDSRank *mds, PurgeQueue &purge_queue_)
  : delayed_eval_stray(member_offset(CDentry, item_stray)),
    mds(mds), logger(NULL), started(false), num_strays(0),
    num_strays_delayed(0), num_strays_enqueuing(0),
    purge_queue(purge_queue_)
{
  assert(mds != NULL);
}

void StrayManager::truncate(CDentry *dn)
{
  const CDentry::linkage_t *dnl = dn->get_projected_linkage();
  const CInode *in = dnl->get_inode();
  assert(in);
  dout(10) << __func__ << ": " << *dn << " " << *in << dendl;
  assert(!dn->is_replicated());

  const SnapRealm *realm = in->find_snaprealm();
  assert(realm);
  dout(10) << " realm " << *realm << dendl;
  const SnapContext *snapc = &realm->get_snap_context();

  uint64_t to = in->inode.get_max_size();
  to = MAX(in->inode.size, to);
  // when truncating a file, the filer does not delete stripe objects that are
  // truncated to zero. so we need to purge stripe objects up to the max size
  // the file has ever been.
  to = MAX(in->inode.max_size_ever, to);

  assert(to > 0);

  PurgeItem item;
  item.action = PurgeItem::TRUNCATE_FILE;
  item.ino = in->inode.ino;
  item.layout = in->inode.layout;
  item.snapc = *snapc;
  item.size = to;

  purge_queue.push(item, new C_IO_PurgeStrayPurged(
        this, dn, true));
}

void StrayManager::_truncate_stray_logged(CDentry *dn, LogSegment *ls)
{
  CInode *in = dn->get_projected_linkage()->get_inode();

  dout(10) << __func__ << ": " << *dn << " " << *in << dendl;

  dn->state_clear(CDentry::STATE_PURGING | CDentry::STATE_PURGINGPINNED);
  dn->put(CDentry::PIN_PURGING);

  in->pop_and_dirty_projected_inode(ls);

  eval_stray(dn);
}

