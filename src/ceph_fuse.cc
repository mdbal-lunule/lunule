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

#include <sys/stat.h>
#include <sys/utsname.h>
#include <iostream>
#include <string>
using namespace std;

#include "common/config.h"
#include "common/errno.h"

#include "client/Client.h"
#include "client/fuse_ll.h"

#include "msg/Messenger.h"

#include "mon/MonClient.h"

#include "common/Timer.h"
#include "common/ceph_argparse.h"
#if defined(__linux__)
#include "common/linux_version.h"
#endif
#include "global/global_init.h"
#include "global/signal_handler.h"
#include "common/Preforker.h"
#include "common/safe_io.h"

#include <sys/types.h>
#include <fcntl.h>

#include <fuse.h>

#define dout_context g_ceph_context

static void fuse_usage()
{
  const char* argv[] = {
    "ceph-fuse",
    "-h",
  };
  struct fuse_args args = FUSE_ARGS_INIT(2, (char**)argv);
  if (fuse_parse_cmdline(&args, NULL, NULL, NULL) == -1) {
    derr << "fuse_parse_cmdline failed." << dendl;
  }
  assert(args.allocated);
  fuse_opt_free_args(&args);
}

void usage()
{
  cout <<
"usage: ceph-fuse [-n client.username] [-m mon-ip-addr:mon-port] <mount point> [OPTIONS]\n"
"  --client_mountpoint/-r <sub_directory>\n"
"                    use sub_directory as the mounted root, rather than the full Ceph tree.\n"
"\n";
  fuse_usage();
  generic_client_usage();
}

int main(int argc, const char **argv, const char *envp[]) {
  int filer_flags = 0;
  //cerr << "ceph-fuse starting " << myrank << "/" << world << std::endl;
  std::vector<const char*> args;
  argv_to_vec(argc, argv, args);
  if (args.empty()) {
    usage();
  }
  env_to_vec(args);

  std::vector<const char*> def_args{"--pid-file="};

  auto cct = global_init(&def_args, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_DAEMON,
			 CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS);
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_flag(args, i, "--localize-reads", (char*)NULL)) {
      cerr << "setting CEPH_OSD_FLAG_LOCALIZE_READS" << std::endl;
      filer_flags |= CEPH_OSD_FLAG_LOCALIZE_READS;
    } else if (ceph_argparse_flag(args, i, "-h", "--help", (char*)NULL)) {
      usage();
    } else {
      ++i;
    }
  }

  // args for fuse
  const char **newargv;
  int newargc;
  vec_to_argv(argv[0], args, &newargc, &newargv);

  // FUSE will chdir("/"); be ready.
  g_ceph_context->_conf->set_val("chdir", "/");
  g_ceph_context->_conf->apply_changes(NULL);

  // check for 32-bit arch
#ifndef __LP64__
    cerr << std::endl;
    cerr << "WARNING: Ceph inode numbers are 64 bits wide, and FUSE on 32-bit kernels does" << std::endl;
    cerr << "         not cope well with that situation.  Expect to crash shortly." << std::endl;
    cerr << std::endl;
#endif

  Preforker forker;
  if (g_conf->daemonize) {
    global_init_prefork(g_ceph_context);
    int r;
    string err;
    r = forker.prefork(err);
    if (r < 0 || forker.is_parent()) {
      // Start log if current process is about to exit. Otherwise, we hit an assert
      // in the Ceph context destructor.
      g_ceph_context->_log->start();
    }
    if (r < 0) {
      cerr << "ceph-fuse " << err << std::endl;
      return r;
    }
    if (forker.is_parent()) {
      r = forker.parent_wait(err);
      if (r < 0) {
	cerr << "ceph-fuse " << err << std::endl;
      }
      return r;
    }
    global_init_postfork_start(cct.get());
  }

  {
    common_init_finish(g_ceph_context);

    //cout << "child, mounting" << std::endl;
    class RemountTest : public Thread {
    public:
      CephFuse *cfuse;
      Client *client;
      RemountTest() : cfuse(NULL), client(NULL) {}
      void init(CephFuse *cf, Client *cl) {
	cfuse = cf;
	client = cl;
      }
      ~RemountTest() override {}
      void *entry() override {
#if defined(__linux__)
	int ver = get_linux_version();
	assert(ver != 0);
        bool client_try_dentry_invalidate = g_conf->get_val<bool>(
          "client_try_dentry_invalidate");
	bool can_invalidate_dentries =
          client_try_dentry_invalidate && ver < KERNEL_VERSION(3, 18, 0);
	int tr = client->test_dentry_handling(can_invalidate_dentries);
        bool client_die_on_failed_dentry_invalidate = g_conf->get_val<bool>(
          "client_die_on_failed_dentry_invalidate");
	if (tr != 0 && client_die_on_failed_dentry_invalidate) {
	  cerr << "ceph-fuse[" << getpid()
	       << "]: fuse failed dentry invalidate/remount test with error "
	       << cpp_strerror(tr) << ", stopping" << std::endl;

	  char buf[5050];
	  string mountpoint = cfuse->get_mount_point();
	  snprintf(buf, sizeof(buf), "fusermount -u -z %s", mountpoint.c_str());
	  int umount_r = system(buf);
	  if (umount_r) {
	    if (umount_r != -1) {
	      if (WIFEXITED(umount_r)) {
		umount_r = WEXITSTATUS(umount_r);
		cerr << "got error " << umount_r
		     << " when unmounting Ceph on failed remount test!" << std::endl;
	      } else {
		cerr << "attempt to umount on failed remount test failed (on a signal?)" << std::endl;
	      }
	    } else {
	      cerr << "system() invocation failed during remount test" << std::endl;
	    }
	  }
	}
	return reinterpret_cast<void*>(tr);
#else
	return reinterpret_cast<void*>(0);
#endif
      }
    } tester;


    // get monmap
    Messenger *messenger = NULL;
    StandaloneClient *client;
    CephFuse *cfuse;
    UserPerm perms;
    int tester_r = 0;
    void *tester_rp = NULL;

    MonClient *mc = new MonClient(g_ceph_context);
    int r = mc->build_initial_monmap();
    if (r == -EINVAL)
      usage();
    if (r < 0)
      goto out_mc_start_failed;

    // start up network
    messenger = Messenger::create_client_messenger(g_ceph_context, "client");
    messenger->set_default_policy(Messenger::Policy::lossy_client(0));
    messenger->set_policy(entity_name_t::TYPE_MDS,
			  Messenger::Policy::lossless_client(0));

    client = new StandaloneClient(messenger, mc);
    if (filer_flags) {
      client->set_filer_flags(filer_flags);
    }

    cfuse = new CephFuse(client, forker.get_signal_fd());

    r = cfuse->init(newargc, newargv);
    if (r != 0) {
      cerr << "ceph-fuse[" << getpid() << "]: fuse failed to initialize" << std::endl;
      goto out_messenger_start_failed;
    }

    cerr << "ceph-fuse[" << getpid() << "]: starting ceph client" << std::endl;
    r = messenger->start();
    if (r < 0) {
      cerr << "ceph-fuse[" << getpid() << "]: ceph messenger failed with " << cpp_strerror(-r) << std::endl;
      goto out_messenger_start_failed;
    }

    init_async_signal_handler();
    register_async_signal_handler(SIGHUP, sighup_handler);

    // start client
    r = client->init();
    if (r < 0) {
      cerr << "ceph-fuse[" << getpid() << "]: ceph client failed with " << cpp_strerror(-r) << std::endl;
      goto out_init_failed;
    }
    
    client->update_metadata("mount_point", cfuse->get_mount_point());
    perms = client->pick_my_perms();
    // start up fuse
    // use my argc, argv (make sure you pass a mount point!)
    r = client->mount(g_conf->client_mountpoint.c_str(), perms,
		      g_ceph_context->_conf->fuse_require_active_mds);
    if (r < 0) {
      if (r == CEPH_FUSE_NO_MDS_UP)
        cerr << "ceph-fuse[" << getpid() << "]: probably no MDS server is up?" << std::endl;
      cerr << "ceph-fuse[" << getpid() << "]: ceph mount failed with " << cpp_strerror(-r) << std::endl;
      goto out_shutdown;
    }

    r = cfuse->start();
    if (r != 0) {
      cerr << "ceph-fuse[" << getpid() << "]: fuse failed to start" << std::endl;
      goto out_client_unmount;
    }

    cerr << "ceph-fuse[" << getpid() << "]: starting fuse" << std::endl;
    tester.init(cfuse, client);
    tester.create("tester");
    r = cfuse->loop();
    tester.join(&tester_rp);
    tester_r = static_cast<int>(reinterpret_cast<uint64_t>(tester_rp));
    cerr << "ceph-fuse[" << getpid() << "]: fuse finished with error " << r
	 << " and tester_r " << tester_r <<std::endl;
    
    
  out_client_unmount:
    client->unmount();
    cfuse->finalize();
  out_shutdown:
    client->shutdown();
  out_init_failed:
    unregister_async_signal_handler(SIGHUP, sighup_handler);
    shutdown_async_signal_handler();

    // wait for messenger to finish
    messenger->shutdown();
    messenger->wait();
  out_messenger_start_failed:
    delete cfuse;
    delete client;
    delete messenger;
  out_mc_start_failed:
    free(newargv);
    delete mc;
    //cout << "child done" << std::endl;
    return forker.signal_exit(r);
  }
}

