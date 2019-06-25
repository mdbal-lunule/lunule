// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef SCRUBSTACK_H_
#define SCRUBSTACK_H_

#include "CDir.h"
#include "CDentry.h"
#include "CInode.h"
#include "MDSContext.h"
#include "ScrubHeader.h"

#include "include/elist.h"

class MDCache;
class Finisher;

class ScrubStack {
protected:
  /// A finisher needed so that we don't re-enter kick_off_scrubs
  Finisher *finisher;

  /// The stack of dentries we want to scrub
  elist<CInode*> inode_stack;
  /// current number of dentries we're actually scrubbing
  int scrubs_in_progress;
  ScrubStack *scrubstack; // hack for dout
  int stack_size;

  class C_KickOffScrubs : public MDSInternalContext {
    ScrubStack *stack;
  public:
    C_KickOffScrubs(MDCache *mdcache, ScrubStack *s);
    void finish(int r) override { }
    void complete(int r) override {
      stack->scrubs_in_progress--;
      stack->kick_off_scrubs();
      // don't delete self
    }
  };
  C_KickOffScrubs scrub_kick;

public:
  MDCache *mdcache;
  ScrubStack(MDCache *mdc, Finisher *finisher_) :
    finisher(finisher_),
    inode_stack(member_offset(CInode, item_scrub)),
    scrubs_in_progress(0),
    scrubstack(this),
    stack_size(0),
    scrub_kick(mdc, this),
    mdcache(mdc) {}
  ~ScrubStack() {
    assert(inode_stack.empty());
    assert(!scrubs_in_progress);
  }
  /**
   * Put a inode on the top of the scrub stack, so it is the highest priority.
   * If there are other scrubs in progress, they will not continue scrubbing new
   * entries until this one is completed.
   * @param in The inodey to scrub
   * @param header The ScrubHeader propagated from whereever this scrub
   *               was initiated
   */
  void enqueue_inode_top(CInode *in, ScrubHeaderRef& header,
			 MDSInternalContextBase *on_finish) {
    enqueue_inode(in, header, on_finish, true);
  }
  /** Like enqueue_inode_top, but we wait for all pending scrubs before
   * starting this one.
   */
  void enqueue_inode_bottom(CInode *in, ScrubHeaderRef& header,
			    MDSInternalContextBase *on_finish) {
    enqueue_inode(in, header, on_finish, false);
  }

private:
  /**
   * Put the inode at either the top or bottom of the stack, with
   * the given scrub params, and then try and kick off more scrubbing.
   */
  void enqueue_inode(CInode *in, ScrubHeaderRef& header,
                      MDSInternalContextBase *on_finish, bool top);
  void _enqueue_inode(CInode *in, CDentry *parent, ScrubHeaderRef& header,
                      MDSInternalContextBase *on_finish, bool top);
  /**
   * Kick off as many scrubs as are appropriate, based on the current
   * state of the stack.
   */
  void kick_off_scrubs();
  /**
   * Push a indoe on top of the stack.
   */
  inline void push_inode(CInode *in);
  /**
   * Push a inode to the bottom of the stack.
   */
  inline void push_inode_bottom(CInode *in);
  /**
   * Pop the given inode off the stack.
   */
  inline void pop_inode(CInode *in);

  /**
   * Scrub a file inode.
   * @param in The indoe to scrub
   */
  void scrub_file_inode(CInode *in);

  /**
   * Callback from completion of CInode::validate_disk_state
   * @param in The inode we were validating
   * @param r The return status from validate_disk_state
   * @param result Populated results from validate_disk_state
   */
  void _validate_inode_done(CInode *in, int r,
			    const CInode::validated_data &result);
  friend class C_InodeValidated;

  /**
   * Make progress on scrubbing a directory-representing dirfrag and
   * its children..
   *
   * 1) Select the next dirfrag which hasn't been scrubbed, and make progress
   * on it if possible.
   *
   * 2) If not, move on to the next dirfrag and start it up, if any.
   *
   * 3) If waiting for results from dirfrag scrubs, do nothing.
   *
   * 4) If all dirfrags have been scrubbed, scrub my inode.
   *
   * @param in The CInode to scrub as a directory
   * @param added_dentries set to true if we pushed some of our children
   * onto the ScrubStack
   * @param is_terminal set to true if there are no descendant dentries
   * remaining to start scrubbing.
   * @param done set to true if we and all our children have finished scrubbing
   */
  void scrub_dir_inode(CInode *in, bool *added_children, bool *is_terminal,
                       bool *done);
  /**
   * Make progress on scrubbing a dirfrag. It may return after each of the
   * following steps, but will report making progress on each one.
   *
   * 1) enqueues the next unscrubbed child directory dentry at the
   * top of the stack.
   *
   * 2) Initiates a scrub on the next unscrubbed file dentry
   *
   * If there are scrubs currently in progress on child dentries, no more child
   * dentries to scrub, and this function is invoked, it will report no
   * progress. Try again later.
   *
   */
  void scrub_dirfrag(CDir *dir, ScrubHeaderRef& header,
		     bool *added_children, bool *is_terminal, bool *done);
  /**
   * Scrub a directory-representing dentry.
   *
   * @param in The directory inode we're doing final scrub on.
   */
  void scrub_dir_inode_final(CInode *in);

  /**
   * Get a CDir into memory, and return it if it's already complete.
   * Otherwise, fetch it and kick off scrubbing when done.
   *
   * @param in The Inode to get the next directory from
   * @param new_dir The CDir we're returning to you. NULL if
   * not ready yet or there aren't any.
   * @returns false if you have to wait, true if there's no work
   * left to do (we returned it, or there are none left in this inode).
   */
  bool get_next_cdir(CInode *in, CDir **new_dir);

};

#endif /* SCRUBSTACK_H_ */
